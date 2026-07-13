#include "foundation/blob/blob_store.hpp"

#include "foundation/filesystem/filesystem.hpp"

#include <charconv>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace lc {
namespace fs = std::filesystem;

Status validateBlobKey(const BlobKey& key)
{
    if (key.namespace_.empty())
        return Status::invalidArgument("blob namespace cannot be empty");
    if (key.name_.empty())
        return Status::invalidArgument("blob name cannot be empty");
    if (auto status = requireSafeRelativePath(fs::path(key.namespace_)); !status.isOk())
        return status;
    if (auto status = requireSafeRelativePath(fs::path(key.name_)); !status.isOk())
        return status;
    return Status::ok();
}

Status validateBlobListOptions(const BlobListOptions& options)
{
    if (options.limit_ == 0)
        return Status::invalidArgument("blob list limit must be greater than 0");
    if (!options.namespace_.empty()) {
        if (auto status = requireSafeRelativePath(fs::path(options.namespace_)); !status.isOk())
            return status;
    }
    if (!options.namePrefix_.empty()) {
        if (fs::path(options.namePrefix_).is_absolute())
            return Status::invalidArgument("blob name prefix must be relative");
        for (const auto& part : fs::path(options.namePrefix_).lexically_normal()) {
            if (part == "..")
                return Status::permissionDenied("blob name prefix cannot contain '..'");
        }
    }
    return Status::ok();
}

Status validateBlobReadOptions(const BlobReadOptions& options)
{
    if (options.chunkBytes_ == 0)
        return Status::invalidArgument("blob read chunk size must be greater than 0");
    if (options.chunkBytes_ > 16 * 1024 * 1024)
        return Status::outOfRange("blob read chunk size is too large");
    return Status::ok();
}

std::string encodeBlobCursor(const BlobKey& key)
{
    std::string cursor;
    cursor.reserve(24 + key.namespace_.size() + key.name_.size());
    cursor.append(std::to_string(key.namespace_.size()));
    cursor.push_back(':');
    cursor.append(key.namespace_);
    cursor.append(key.name_);
    return cursor;
}

Result<std::optional<BlobKey>> decodeBlobCursor(std::string_view cursor)
{
    if (cursor.empty())
        return std::optional<BlobKey> {};

    const auto separator = cursor.find(':');
    if (separator == std::string_view::npos)
        return Status::invalidArgument("invalid blob cursor");

    std::size_t namespaceSize = 0;
    const auto sizeText = cursor.substr(0, separator);
    const auto* begin = sizeText.data();
    const auto* end = begin + sizeText.size();
    const auto result = std::from_chars(begin, end, namespaceSize);
    if (result.ec != std::errc {} || result.ptr != end)
        return Status::invalidArgument("invalid blob cursor");

    const auto payload = cursor.substr(separator + 1);
    if (namespaceSize == 0 || namespaceSize >= payload.size())
        return Status::invalidArgument("invalid blob cursor");

    BlobKey key {
        .namespace_ = std::string(payload.substr(0, namespaceSize)),
        .name_ = std::string(payload.substr(namespaceSize)),
    };
    if (auto status = validateBlobKey(key); !status.isOk())
        return status;

    return std::optional<BlobKey>(std::move(key));
}

} // namespace lc
