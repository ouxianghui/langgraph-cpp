#include "foundation/blob/blob_store.hpp"

#include "foundation/blob/blob_common.hh"
#include "foundation/blob/blob_metadata.hh"
#include "foundation/filesystem/filesystem.hpp"
#include "foundation/logging/logger.hpp"

#include <algorithm>
#include <filesystem>
#include <set>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace lgc {
using namespace blob_detail;
namespace {

class StoreFileLock final {
public:
    StoreFileLock() = default;
    ~StoreFileLock() { release(); }

    StoreFileLock(const StoreFileLock&) = delete;
    StoreFileLock& operator=(const StoreFileLock&) = delete;

    StoreFileLock(StoreFileLock&& other) noexcept
    {
#if defined(_WIN32)
        handle_ = other.handle_;
        overlapped_ = other.overlapped_;
        other.handle_ = INVALID_HANDLE_VALUE;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
    }

    StoreFileLock& operator=(StoreFileLock&& other) noexcept
    {
        if (this == &other)
            return *this;

        release();
#if defined(_WIN32)
        handle_ = other.handle_;
        overlapped_ = other.overlapped_;
        other.handle_ = INVALID_HANDLE_VALUE;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
        return *this;
    }

    [[nodiscard]] static Result<StoreFileLock> acquire(const fs::path& path)
    {
        StoreFileLock lock;
#if defined(_WIN32)
        lock.handle_ = ::CreateFileW(
            path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (lock.handle_ == INVALID_HANDLE_VALUE)
            return Status::unavailable("failed to open blob store lock file");

        if (!::LockFileEx(lock.handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &lock.overlapped_)) {
            ::CloseHandle(lock.handle_);
            lock.handle_ = INVALID_HANDLE_VALUE;
            return Status::unavailable("failed to acquire blob store lock");
        }
#else
        lock.fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (lock.fd_ < 0)
            return Status::unavailable("failed to open blob store lock file");

        if (::flock(lock.fd_, LOCK_EX) != 0) {
            ::close(lock.fd_);
            lock.fd_ = -1;
            return Status::unavailable("failed to acquire blob store lock");
        }
#endif
        return std::move(lock);
    }

private:
    void release() noexcept
    {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) {
            (void)::UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped_);
            (void)::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) {
            (void)::flock(fd_, LOCK_UN);
            (void)::close(fd_);
            fd_ = -1;
        }
#endif
    }

#if defined(_WIN32)
    HANDLE handle_ { INVALID_HANDLE_VALUE };
    OVERLAPPED overlapped_ {};
#else
    int fd_ { -1 };
#endif
};


} // namespace

FileSystemBlobStore::FileSystemBlobStore(
    std::filesystem::path rootDirectory,
    BlobStoreOptions options,
    std::shared_ptr<ILogger> logger)
    : rootDirectory_(std::move(rootDirectory))
    , options_(std::move(options))
    , logger_(std::move(logger))
{
}

Result<void> FileSystemBlobStore::put(BlobKey key, BlobData data, const BlobPutOptions& options)
{
    const auto logFailure = [&](const Status& status) {
        logTo(logger_,
            LogLevel::Warn,
            "FileSystemBlobStore",
            "put failed namespace={} name={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.name_,
            status.toString());
    };

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
    if (auto result = ensureDir(rootDirectory_); !result.isOk()) {
        logFailure(result.status());
        return result.status();
    }
    auto storeLock = StoreFileLock::acquire(lockPath(rootDirectory_));
    if (!storeLock.isOk()) {
        logFailure(storeLock.status());
        return storeLock.status();
    }
    if (auto status = ensureStoreDirectory(); !status.isOk()) {
        logFailure(status);
        return status;
    }

    auto metadata = metadataPath(key);
    if (!metadata.isOk()) {
        logFailure(metadata.status());
        return metadata.status();
    }

    std::error_code ec;
    const bool exists = fs::exists(*metadata, ec);
    if (ec) {
        auto status = Status::internal("failed to check blob metadata: " + ec.message());
        logFailure(status);
        return status;
    }
    if (exists && !options.replace_) {
        auto status = Status::alreadyExists("blob already exists");
        logFailure(status);
        return status;
    }

    std::optional<BlobInfo> previousInfo;
    if (exists) {
        auto existingInfo = readInfo(key);
        if (existingInfo.isOk()) {
            previousInfo = *existingInfo;
        } else if (existingInfo.status().code() != StatusCode::NotFound) {
            logFailure(existingInfo.status());
            return existingInfo.status();
        }

    }

    const auto now = nowSystem();
    BlobInfo info {
        .key_ = key,
        .contentType_ = options.contentType_,
        .size_ = data.size(),
        .checksumSha256_ = std::move(*checksum),
        .metadata_ = options.metadata_,
        .createdAt_ = previousInfo.has_value() ? previousInfo->createdAt_ : now,
        .updatedAt_ = now,
    };

    auto content = contentPath(info);
    if (!content.isOk()) {
        logFailure(content.status());
        return content.status();
    }

    std::error_code contentExistsEc;
    const bool contentExistedBeforeWrite = fs::exists(*content, contentExistsEc);
    if (contentExistsEc) {
        auto status = Status::internal("failed to check blob content: " + contentExistsEc.message());
        logFailure(status);
        return status;
    }

    if (auto result = writeDataFile(*content, std::span<const std::byte>(data.data(), data.size())); !result.isOk()) {
        logFailure(result.status());
        return result.status();
    }

    if (auto result = writeInfoFile(*metadata, info); !result.isOk()) {
        if (!contentExistedBeforeWrite && (!previousInfo || previousInfo->checksumSha256_ != info.checksumSha256_)) {
            if (auto cleanup = removeFileStrict(*content, "staged blob content"); !cleanup.isOk()) {
                auto status = Status::internal(result.status().toString() + "; " + cleanup.toString());
                logFailure(status);
                return status;
            }
        }
        logFailure(result.status());
        return result.status();
    }

    if (previousInfo && previousInfo->checksumSha256_ != info.checksumSha256_) {
        auto previousContent = contentPath(*previousInfo);
        if (previousContent.isOk()) {
            std::error_code removeEc;
            fs::remove(*previousContent, removeEc);
            if (removeEc) {
                logTo(logger_,
                    LogLevel::Warn,
                    "FileSystemBlobStore",
                    "old blob content cleanup failed namespace={} name={} error={}",
                    __FILE__,
                    __LINE__,
                    key.namespace_,
                    key.name_,
                    removeEc.message());
            }
        }
    }
    return okResult();
}

Result<void> FileSystemBlobStore::putFile(
    BlobKey key,
    const std::filesystem::path& sourcePath,
    const BlobPutOptions& options)
{
    const auto logFailure = [&](const Status& status) {
        logTo(logger_,
            LogLevel::Warn,
            "FileSystemBlobStore",
            "putFile failed namespace={} name={} source={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.name_,
            sourcePath.string(),
            status.toString());
    };

    if (auto status = validateBlobKey(key); !status.isOk())
        return status;
    if (auto status = validatePutOptions(options, options_); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    if (auto result = ensureDir(rootDirectory_); !result.isOk()) {
        logFailure(result.status());
        return result.status();
    }
    auto storeLock = StoreFileLock::acquire(lockPath(rootDirectory_));
    if (!storeLock.isOk()) {
        logFailure(storeLock.status());
        return storeLock.status();
    }
    if (auto status = ensureStoreDirectory(); !status.isOk()) {
        logFailure(status);
        return status;
    }

    auto metadata = metadataPath(key);
    if (!metadata.isOk()) {
        logFailure(metadata.status());
        return metadata.status();
    }

    std::error_code ec;
    const bool exists = fs::exists(*metadata, ec);
    if (ec) {
        auto status = Status::internal("failed to check blob metadata: " + ec.message());
        logFailure(status);
        return status;
    }
    if (exists && !options.replace_) {
        auto status = Status::alreadyExists("blob already exists");
        logFailure(status);
        return status;
    }

    std::optional<BlobInfo> previousInfo;
    if (exists) {
        auto existingInfo = readInfo(key);
        if (existingInfo.isOk()) {
            previousInfo = *existingInfo;
        } else if (existingInfo.status().code() != StatusCode::NotFound) {
            logFailure(existingInfo.status());
            return existingInfo.status();
        }
    }

    auto staged = stageContentFromFile(sourcePath, metadata->parent_path(), options_);
    if (!staged.isOk()) {
        logFailure(staged.status());
        return staged.status();
    }

    const auto now = nowSystem();
    BlobInfo info {
        .key_ = key,
        .contentType_ = options.contentType_,
        .size_ = staged->size_,
        .checksumSha256_ = staged->checksumSha256_,
        .metadata_ = options.metadata_,
        .createdAt_ = previousInfo.has_value() ? previousInfo->createdAt_ : now,
        .updatedAt_ = now,
    };

    auto content = contentPath(info);
    if (!content.isOk()) {
        logFailure(content.status());
        return content.status();
    }

    auto installedContent = installStagedContent(staged->temp_, *content, info.size_);
    if (!installedContent.isOk()) {
        logFailure(installedContent.status());
        return installedContent.status();
    }

    if (auto result = writeInfoFile(*metadata, info); !result.isOk()) {
        if (*installedContent && (!previousInfo || previousInfo->checksumSha256_ != info.checksumSha256_)) {
            if (auto cleanup = removeFileStrict(*content, "staged blob content"); !cleanup.isOk()) {
                auto status = Status::internal(result.status().toString() + "; " + cleanup.toString());
                logFailure(status);
                return status;
            }
        }
        logFailure(result.status());
        return result.status();
    }

    if (previousInfo && previousInfo->checksumSha256_ != info.checksumSha256_) {
        auto previousContent = contentPath(*previousInfo);
        if (previousContent.isOk()) {
            std::error_code removeEc;
            fs::remove(*previousContent, removeEc);
            if (removeEc) {
                logTo(logger_,
                    LogLevel::Warn,
                    "FileSystemBlobStore",
                    "old blob content cleanup failed namespace={} name={} error={}",
                    __FILE__,
                    __LINE__,
                    key.namespace_,
                    key.name_,
                    removeEc.message());
            }
        }
    }
    return okResult();
}

Result<std::optional<BlobInfo>> FileSystemBlobStore::stat(const BlobKey& key)
{
    if (auto status = validateBlobKey(key); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    auto initialized = initializedStore(rootDirectory_);
    if (!initialized.isOk())
        return initialized.status();
    if (!*initialized)
        return std::optional<BlobInfo> {};

    auto storeLock = StoreFileLock::acquire(lockPath(rootDirectory_));
    if (!storeLock.isOk())
        return storeLock.status();

    auto info = readInfo(key);
    if (!info.isOk()) {
        if (info.status().code() == StatusCode::NotFound)
            return std::optional<BlobInfo> {};
        return info.status();
    }
    if (auto status = requireBlobWithinLimit(info->size_, options_); !status.isOk())
        return status;

    auto dataFile = contentPath(*info);
    if (!dataFile.isOk())
        return dataFile.status();

    std::error_code sizeEc;
    const auto actualSize = fs::file_size(*dataFile, sizeEc);
    if (sizeEc)
        return Status::dataLoss("blob metadata exists but data file is missing or unreadable");
    if (actualSize != static_cast<std::uintmax_t>(info->size_))
        return Status::dataLoss("blob data size does not match metadata");

    return std::optional<BlobInfo>(*info);
}

Result<std::optional<Blob>> FileSystemBlobStore::get(const BlobKey& key)
{
    if (auto status = validateBlobKey(key); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    auto initialized = initializedStore(rootDirectory_);
    if (!initialized.isOk())
        return initialized.status();
    if (!*initialized)
        return std::optional<Blob> {};

    auto storeLock = StoreFileLock::acquire(lockPath(rootDirectory_));
    if (!storeLock.isOk())
        return storeLock.status();

    auto info = readInfo(key);
    if (!info.isOk()) {
        if (info.status().code() == StatusCode::NotFound)
            return std::optional<Blob> {};
        logTo(logger_,
            LogLevel::Warn,
            "FileSystemBlobStore",
            "get failed namespace={} name={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.name_,
            info.status().toString());
        return info.status();
    }

    if (auto status = requireBlobWithinLimit(info->size_, options_); !status.isOk())
        return status;

    auto path = contentPath(*info);
    if (!path.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "FileSystemBlobStore",
            "get failed namespace={} name={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.name_,
            path.status().toString());
        return path.status();
    }

    std::error_code sizeEc;
    const auto actualSize = fs::file_size(*path, sizeEc);
    if (sizeEc) {
        auto status = Status::dataLoss("blob metadata exists but data file is missing or unreadable");
        logTo(logger_,
            LogLevel::Warn,
            "FileSystemBlobStore",
            "get failed namespace={} name={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.name_,
            status.toString());
        return status;
    }

    if (actualSize != static_cast<std::uintmax_t>(info->size_)) {
        auto status = Status::dataLoss("blob data size does not match metadata");
        logTo(logger_,
            LogLevel::Warn,
            "FileSystemBlobStore",
            "get failed namespace={} name={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.name_,
            status.toString());
        return status;
    }

    if (auto status = requireBlobWithinLimit(static_cast<std::size_t>(actualSize), options_); !status.isOk())
        return status;

    auto bytes = readBytes(*path, options_);
    if (!bytes.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "FileSystemBlobStore",
            "get failed namespace={} name={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.name_,
            bytes.status().toString());
        return bytes.status();
    }

    auto checksum = sha256Hex(std::as_bytes(std::span(bytes->data(), bytes->size())));
    if (!checksum.isOk())
        return checksum.status();
    if (*checksum != info->checksumSha256_) {
        auto status = Status::dataLoss("blob checksum mismatch");
        logTo(logger_,
            LogLevel::Warn,
            "FileSystemBlobStore",
            "get failed namespace={} name={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.name_,
            status.toString());
        return status;
    }

    return std::optional<Blob>(Blob {
        .info_ = *info,
        .data_ = std::move(*bytes),
    });
}

Result<void> FileSystemBlobStore::read(
    const BlobKey& key,
    const BlobReadCallback& callback,
    const BlobReadOptions& options)
{
    if (auto status = validateBlobKey(key); !status.isOk())
        return status;
    if (auto status = validateBlobReadOptions(options); !status.isOk())
        return status;
    if (!callback)
        return Status::invalidArgument("blob read callback cannot be empty");

    std::lock_guard lock(mutex_);
    auto initialized = initializedStore(rootDirectory_);
    if (!initialized.isOk())
        return initialized.status();
    if (!*initialized)
        return Status::notFound("blob not found");

    auto storeLock = StoreFileLock::acquire(lockPath(rootDirectory_));
    if (!storeLock.isOk())
        return storeLock.status();

    auto info = readInfo(key);
    if (!info.isOk()) {
        if (info.status().code() == StatusCode::NotFound)
            return Status::notFound("blob not found");
        return info.status();
    }

    auto path = contentPath(*info);
    if (!path.isOk())
        return path.status();

    std::error_code sizeEc;
    const auto actualSize = fs::file_size(*path, sizeEc);
    if (sizeEc)
        return Status::dataLoss("blob metadata exists but data file is missing or unreadable");
    if (actualSize != static_cast<std::uintmax_t>(info->size_))
        return Status::dataLoss("blob data size does not match metadata");

    return streamBytes(*path, *info, options_, callback, options);
}

Result<BlobListResult> FileSystemBlobStore::list(const BlobListOptions& options)
{
    if (auto status = validateBlobListOptionsForStore(options, options_); !status.isOk())
        return status;
    auto cursorKey = decodeBlobCursor(options.cursor_);
    if (!cursorKey.isOk())
        return cursorKey.status();

    std::lock_guard lock(mutex_);
    BlobListResult result;
    std::vector<BlobInfo> pageCandidates;
    pageCandidates.reserve(options.limit_ + 1);

    auto initialized = initializedStore(rootDirectory_);
    if (!initialized.isOk())
        return initialized.status();
    if (!*initialized)
        return result;

    auto storeLock = StoreFileLock::acquire(lockPath(rootDirectory_));
    if (!storeLock.isOk())
        return storeLock.status();

    initialized = initializedStore(rootDirectory_);
    if (!initialized.isOk())
        return initialized.status();
    if (!*initialized)
        return result;

    fs::path scanRoot = rootDirectory_;
    if (!options.namespace_.empty()) {
        auto namespaceRoot = namespaceDirectory(options.namespace_);
        if (!namespaceRoot.isOk())
            return namespaceRoot.status();
        scanRoot = *namespaceRoot;
    }

    std::error_code ec;
    if (!fs::exists(scanRoot, ec)) {
        if (ec)
            return Status::internal("failed to check blob list root: " + ec.message());
        return result;
    }
    if (!fs::is_directory(scanRoot, ec) || ec)
        return Status::failedPrecondition("blob list root exists but is not a directory");

    const auto cursorOrderedKey = cursorKey->has_value()
        ? std::optional<std::pair<std::string, std::string>>(toOrderedKey(**cursorKey))
        : std::optional<std::pair<std::string, std::string>> {};
    std::size_t scannedMetadataFiles = 0;

    for (fs::recursive_directory_iterator it(scanRoot, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec)
            continue;
        if (!isMetadataFile(it->path()))
            continue;
        ++scannedMetadataFiles;
        if (options_.maxListScanEntries_ != 0U && scannedMetadataFiles > options_.maxListScanEntries_)
            return Status::resourceExhausted("blob list scan exceeded configured maximum");

        auto info = readInfoFile(it->path(), options_);
        if (!info.isOk()) {
            if (info.status().code() == StatusCode::NotFound)
                continue;
            logTo(logger_,
                LogLevel::Warn,
                "FileSystemBlobStore",
                "list failed namespace={} prefix={} status={}",
                __FILE__,
                __LINE__,
                options.namespace_,
                options.namePrefix_,
                info.status().toString());
            return info.status();
        }

        if (auto status = requireMetadataPathMatches(*info, it->path()); !status.isOk()) {
            logTo(logger_,
                LogLevel::Warn,
                "FileSystemBlobStore",
                "list failed namespace={} prefix={} status={}",
                __FILE__,
                __LINE__,
                options.namespace_,
                options.namePrefix_,
                status.toString());
            return status;
        }

        auto dataFile = contentPath(*info);
        if (!dataFile.isOk())
            return dataFile.status();

        std::error_code sizeEc;
        const auto actualSize = fs::file_size(*dataFile, sizeEc);
        if (sizeEc) {
            auto status = Status::dataLoss("blob metadata exists but data file is missing or unreadable");
            logTo(logger_,
                LogLevel::Warn,
                "FileSystemBlobStore",
                "list failed namespace={} prefix={} status={}",
                __FILE__,
                __LINE__,
                options.namespace_,
                options.namePrefix_,
                status.toString());
            return status;
        }
        if (actualSize != static_cast<std::uintmax_t>(info->size_)) {
            auto status = Status::dataLoss("blob data size does not match metadata");
            logTo(logger_,
                LogLevel::Warn,
                "FileSystemBlobStore",
                "list failed namespace={} prefix={} status={}",
                __FILE__,
                __LINE__,
                options.namespace_,
                options.namePrefix_,
                status.toString());
            return status;
        }

        if (!matches(info->key_, options))
            continue;

        const auto orderedKey = toOrderedKey(info->key_);
        if (cursorOrderedKey.has_value() && orderedKey <= *cursorOrderedKey)
            continue;

        auto pos = std::ranges::lower_bound(
            pageCandidates,
            *info,
            [](const BlobInfo& lhs, const BlobInfo& rhs) {
                return toOrderedKey(lhs.key_) < toOrderedKey(rhs.key_);
            });
        pageCandidates.insert(pos, *info);
        if (pageCandidates.size() > options.limit_ + 1)
            pageCandidates.pop_back();
    }
    if (ec) {
        auto status = Status::internal("failed to list blob store: " + ec.message());
        logTo(logger_,
            LogLevel::Warn,
            "FileSystemBlobStore",
            "list failed namespace={} prefix={} status={}",
            __FILE__,
            __LINE__,
            options.namespace_,
            options.namePrefix_,
            status.toString());
        return status;
    }

    for (const auto& info : pageCandidates) {
        if (result.items_.size() >= options.limit_)
            break;
        result.items_.push_back(info);
    }

    if (pageCandidates.size() > options.limit_ && result.items_.size() == options.limit_) {
        const auto lastKey = result.items_.back().key_;
        result.nextCursor_ = encodeBlobCursor(lastKey);
    }
    return result;
}

Result<void> FileSystemBlobStore::remove(const BlobKey& key)
{
    if (auto status = validateBlobKey(key); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    auto initialized = initializedStore(rootDirectory_);
    if (!initialized.isOk())
        return initialized.status();
    if (!*initialized)
        return okResult();

    auto storeLock = StoreFileLock::acquire(lockPath(rootDirectory_));
    if (!storeLock.isOk())
        return storeLock.status();

    auto info = readInfo(key);
    if (!info.isOk()) {
        if (info.status().code() == StatusCode::NotFound)
            return okResult();
        return info.status();
    }

    auto content = contentPath(*info);
    if (!content.isOk())
        return content.status();

    auto metadata = metadataPath(key);
    if (!metadata.isOk())
        return metadata.status();

    std::error_code ec;
    fs::remove(*metadata, ec);
    if (ec) {
        auto status = Status::internal("failed to remove blob metadata: " + ec.message());
        logTo(logger_,
            LogLevel::Warn,
            "FileSystemBlobStore",
            "remove failed namespace={} name={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.name_,
            status.toString());
        return status;
    }
    ec.clear();
    fs::remove(*content, ec);
    if (ec) {
        auto status = Status::internal("failed to remove blob content: " + ec.message());
        logTo(logger_,
            LogLevel::Warn,
            "FileSystemBlobStore",
            "remove failed namespace={} name={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.name_,
            status.toString());
        return status;
    }
    return okResult();
}

Result<void> FileSystemBlobStore::clearNamespace(std::string_view namespaceName)
{
    if (namespaceName.empty())
        return Status::invalidArgument("blob namespace cannot be empty");

    std::lock_guard lock(mutex_);
    if (auto status = requireStoreMarker(); !status.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "FileSystemBlobStore",
            "clearNamespace rejected namespace={} status={}",
            __FILE__,
            __LINE__,
            namespaceName,
            status.toString());
        return status;
    }

    std::error_code existsEc;
    if (!fs::exists(rootDirectory_, existsEc)) {
        if (existsEc)
            return Status::internal("failed to check blob store root: " + existsEc.message());
        return okResult();
    }

    auto storeLock = StoreFileLock::acquire(lockPath(rootDirectory_));
    if (!storeLock.isOk())
        return storeLock.status();
    if (auto status = requireStoreMarker(); !status.isOk())
        return status;

    auto directory = namespaceDirectory(namespaceName);
    if (!directory.isOk())
        return directory.status();

    std::error_code ec;
    fs::remove_all(*directory, ec);
    if (ec) {
        auto status = Status::internal("failed to clear blob namespace: " + ec.message());
        logTo(logger_,
            LogLevel::Warn,
            "FileSystemBlobStore",
            "clearNamespace failed namespace={} status={}",
            __FILE__,
            __LINE__,
            namespaceName,
            status.toString());
        return status;
    }
    return okResult();
}

Result<void> FileSystemBlobStore::compact()
{
    std::lock_guard lock(mutex_);

    std::error_code existsEc;
    if (!fs::exists(rootDirectory_, existsEc)) {
        if (existsEc)
            return Status::internal("failed to check blob store root: " + existsEc.message());
        return okResult();
    }
    if (auto status = requireStoreMarker(); !status.isOk())
        return status;

    auto storeLock = StoreFileLock::acquire(lockPath(rootDirectory_));
    if (!storeLock.isOk())
        return storeLock.status();
    if (auto status = requireStoreMarker(); !status.isOk())
        return status;

    std::set<fs::path> referencedContent;
    std::vector<fs::path> tempFiles;
    std::vector<fs::path> contentFiles;
    std::size_t scannedEntries = 0;

    std::error_code ec;
    for (fs::recursive_directory_iterator it(rootDirectory_, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec)
            continue;

        ++scannedEntries;
        if (options_.maxListScanEntries_ != 0U && scannedEntries > options_.maxListScanEntries_)
            return Status::resourceExhausted("blob compaction scan exceeded configured maximum");

        const auto path = it->path();
        if (isBlobTempFile(path)) {
            tempFiles.push_back(path);
            continue;
        }
        if (isBlobContentFile(path)) {
            auto normalized = normalize(path);
            if (!normalized.isOk())
                return normalized.status();
            contentFiles.push_back(*normalized);
            continue;
        }
        if (!isMetadataFile(path))
            continue;

        auto info = readInfoFile(path, options_);
        if (!info.isOk())
            return info.status();
        if (auto status = requireMetadataPathMatches(*info, path); !status.isOk())
            return status;
        auto content = contentPath(*info);
        if (!content.isOk())
            return content.status();

        std::error_code sizeEc;
        const auto actualSize = fs::file_size(*content, sizeEc);
        if (sizeEc)
            return Status::dataLoss("blob metadata exists but data file is missing or unreadable");
        if (actualSize != static_cast<std::uintmax_t>(info->size_))
            return Status::dataLoss("blob data size does not match metadata");
        referencedContent.insert(*content);
    }
    if (ec)
        return Status::internal("failed to compact blob store: " + ec.message());

    for (const auto& path : tempFiles) {
        if (auto status = removeFileStrict(path, "blob temporary file"); !status.isOk())
            return status;
    }
    for (const auto& path : contentFiles) {
        if (referencedContent.contains(path))
            continue;
        if (auto status = removeFileStrict(path, "orphan blob content"); !status.isOk())
            return status;
    }
    return okResult();
}

const std::filesystem::path& FileSystemBlobStore::rootDirectory() const noexcept
{
    return rootDirectory_;
}

std::filesystem::path FileSystemBlobStore::markerPath() const
{
    return rootDirectory_ / kBlobStoreMarkerName;
}

Result<std::filesystem::path> FileSystemBlobStore::namespaceDirectory(std::string_view namespaceName) const
{
    return resolveChild(rootDirectory_, fs::path(namespaceName));
}

Result<std::filesystem::path> FileSystemBlobStore::contentPath(const BlobInfo& info) const
{
    return resolveChild(
        rootDirectory_,
        fs::path(info.key_.namespace_) / (info.key_.name_ + "." + info.checksumSha256_ + ".data"));
}

Result<std::filesystem::path> FileSystemBlobStore::metadataPath(const BlobKey& key) const
{
    return resolveChild(rootDirectory_, fs::path(key.namespace_) / (key.name_ + ".meta.json"));
}

Status FileSystemBlobStore::ensureStoreDirectory() const
{
    if (auto result = ensureDir(rootDirectory_); !result.isOk())
        return result.status();

    std::error_code ec;
    const auto marker = markerPath();
    if (fs::exists(marker, ec)) {
        if (ec)
            return Status::internal("failed to check blob store marker: " + ec.message());
        if (!fs::is_regular_file(marker, ec) || ec)
            return Status::failedPrecondition("blob store marker path exists but is not a regular file");
        return validateStoreMarkerFile(marker);
    }
    if (ec)
        return Status::internal("failed to check blob store marker: " + ec.message());

    auto written = writeFileAtomic(marker, kBlobStoreMarkerContents);
    if (!written.isOk())
        return written.status();
    return Status::ok();
}

Status FileSystemBlobStore::requireStoreMarker() const
{
    std::error_code ec;
    if (!fs::exists(rootDirectory_, ec)) {
        if (ec)
            return Status::internal("failed to check blob store root: " + ec.message());
        return Status::ok();
    }

    if (!fs::is_directory(rootDirectory_, ec) || ec)
        return Status::failedPrecondition("blob store root exists but is not a directory");

    const auto marker = markerPath();
    if (!fs::exists(marker, ec)) {
        if (ec)
            return Status::internal("failed to check blob store marker: " + ec.message());
        return Status::failedPrecondition("blob store marker is missing");
    }

    if (!fs::is_regular_file(marker, ec) || ec)
        return Status::failedPrecondition("blob store marker path exists but is not a regular file");

    return validateStoreMarkerFile(marker);
}

Status FileSystemBlobStore::requireMetadataPathMatches(
    const BlobInfo& info,
    const std::filesystem::path& metadataFile) const
{
    auto expected = metadataPath(info.key_);
    if (!expected.isOk())
        return expected.status();

    auto actual = normalize(metadataFile);
    if (!actual.isOk())
        return actual.status();

    if (*expected != *actual)
        return Status::dataLoss("blob metadata path does not match its key");
    return Status::ok();
}

Result<BlobInfo> FileSystemBlobStore::readInfo(const BlobKey& key) const
{
    auto path = metadataPath(key);
    if (!path.isOk())
        return path.status();

    auto info = readInfoFile(*path, options_);
    if (!info.isOk())
        return info.status();
    if (info->key_ != key)
        return Status::dataLoss("blob metadata payload does not match requested key");
    return info;
}

} // namespace lgc
