#include "foundation/blob/blob_store.hpp"

#include "foundation/blob/blob_common.hh"
#include "foundation/blob/blob_metadata.hh"

#include <algorithm>
#include <filesystem>
#include <span>
#include <system_error>
#include <utility>

namespace lgc {
using namespace blob_detail;

MemoryBlobStore::MemoryBlobStore(BlobStoreOptions options)
    : options_(std::move(options))
{
}

Result<void> MemoryBlobStore::put(BlobKey key, BlobData data, const BlobPutOptions& options)
{
    if (auto status = validateBlobKey(key); !status.isOk())
        return status;
    if (auto status = validatePutOptions(options, options_); !status.isOk())
        return status;
    if (auto status = requireBlobWithinLimit(data.size(), options_); !status.isOk())
        return status;

    auto checksum = sha256Hex(std::as_bytes(std::span(data.data(), data.size())));
    if (!checksum.isOk())
        return checksum.status();

    std::lock_guard lock(mutex_);
    const auto mapKey = toMapKey(key);
    const auto existing = blobs_.find(mapKey);
    if (existing != blobs_.end() && !options.replace_)
        return Status::alreadyExists("blob already exists");

    const auto now = nowSystem();
    BlobInfo info {
        .key_ = std::move(key),
        .contentType_ = options.contentType_,
        .size_ = data.size(),
        .checksumSha256_ = std::move(*checksum),
        .metadata_ = options.metadata_,
        .createdAt_ = existing == blobs_.end() ? now : existing->second.info_.createdAt_,
        .updatedAt_ = now,
    };
    blobs_[mapKey] = Blob {
        .info_ = std::move(info),
        .data_ = std::move(data),
    };
    return okResult();
}

Result<void> MemoryBlobStore::putFile(
    BlobKey key,
    const std::filesystem::path& sourcePath,
    const BlobPutOptions& options)
{
    std::error_code ec;
    const auto size = fs::file_size(sourcePath, ec);
    if (!ec && options_.maxBlobBytes_ && size > *options_.maxBlobBytes_)
        return Status::resourceExhausted("blob exceeds configured maximum size");

    auto bytes = readBytes(sourcePath, options_);
    if (!bytes.isOk())
        return bytes.status();
    return put(std::move(key), std::move(*bytes), options);
}

Result<std::optional<BlobInfo>> MemoryBlobStore::stat(const BlobKey& key)
{
    if (auto status = validateBlobKey(key); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    const auto it = blobs_.find(toMapKey(key));
    if (it == blobs_.end())
        return std::optional<BlobInfo> {};
    return std::optional<BlobInfo>(it->second.info_);
}

Result<std::optional<Blob>> MemoryBlobStore::get(const BlobKey& key)
{
    if (auto status = validateBlobKey(key); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    const auto it = blobs_.find(toMapKey(key));
    if (it == blobs_.end())
        return std::optional<Blob> {};
    return std::optional<Blob>(it->second);
}

Result<void> MemoryBlobStore::read(
    const BlobKey& key,
    const BlobReadCallback& callback,
    const BlobReadOptions& options)
{
    if (!callback)
        return Status::invalidArgument("blob read callback cannot be empty");
    if (auto status = validateBlobReadOptions(options); !status.isOk())
        return status;

    auto loaded = get(key);
    if (!loaded.isOk())
        return loaded.status();
    if (!loaded->has_value())
        return Status::notFound("blob not found");

    const auto& data = (*loaded)->data_;
    for (std::size_t offset = 0; offset < data.size(); offset += options.chunkBytes_) {
        const auto count = std::min(options.chunkBytes_, data.size() - offset);
        if (auto status = callback(std::span<const std::byte>(data.data() + offset, count)); !status.isOk())
            return status;
    }
    return okResult();
}

Result<BlobListResult> MemoryBlobStore::list(const BlobListOptions& options)
{
    if (auto status = validateBlobListOptionsForStore(options, options_); !status.isOk())
        return status;

    auto cursorKey = decodeBlobCursor(options.cursor_);
    if (!cursorKey.isOk())
        return cursorKey.status();

    std::lock_guard lock(mutex_);
    BlobListResult result;
    bool pastCursor = !*cursorKey;

    for (const auto& [mapKey, blob] : blobs_) {
        if (!pastCursor) {
            if (mapKey <= toMapKey(**cursorKey))
                continue;
            pastCursor = true;
        }
        if (!matches(blob.info_.key_, options))
            continue;

        result.items_.push_back(blob.info_);
        if (result.items_.size() >= options.limit_)
            break;
    }

    if (result.items_.size() == options.limit_) {
        const auto last = result.items_.back().key_;
        auto next = blobs_.upper_bound(toMapKey(last));
        while (next != blobs_.end() && !matches(next->second.info_.key_, options))
            ++next;
        if (next != blobs_.end())
            result.nextCursor_ = encodeBlobCursor(last);
    }
    return result;
}

Result<void> MemoryBlobStore::remove(const BlobKey& key)
{
    if (auto status = validateBlobKey(key); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    blobs_.erase(toMapKey(key));
    return okResult();
}

Result<void> MemoryBlobStore::clearNamespace(std::string_view namespaceName)
{
    if (namespaceName.empty())
        return Status::invalidArgument("blob namespace cannot be empty");

    std::lock_guard lock(mutex_);
    for (auto it = blobs_.begin(); it != blobs_.end();) {
        if (it->first.first == namespaceName)
            it = blobs_.erase(it);
        else
            ++it;
    }
    return okResult();
}

Result<void> MemoryBlobStore::compact()
{
    return okResult();
}

MemoryBlobStore::MapKey MemoryBlobStore::toMapKey(const BlobKey& key)
{
    return { key.namespace_, key.name_ };
}

} // namespace lgc
