#include "foundation/cache/cache.hpp"

#include "foundation/cache/cache_common.hh"
#include "foundation/filesystem/filesystem.hpp"

#include <algorithm>
#include <limits>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace lc {
namespace {
namespace fs = std::filesystem;
using namespace cache_detail;
}

DiskCache::DiskCache(
    std::filesystem::path directory,
    CacheOptions options,
    const Clock& clock,
    std::shared_ptr<ILogger> logger)
    : directory_(std::move(directory))
    , options_(options)
    , clock_(&clock)
    , logger_(std::move(logger))
{
}

Result<std::string> DiskCache::get(const CacheKey& key)
{
    if (auto status = validateCacheKey(key); !status.isOk())
        return errorResult<std::string>(std::move(status));

    std::lock_guard lock(mutex_);
    std::error_code existsEc;
    if (!fs::exists(directory_, existsEc)) {
        if (existsEc)
            return errorResult<std::string>(Status::internal("failed to check cache directory: " + existsEc.message()));
        return errorResult<std::string>(Status::notFound("cache entry not found"));
    }
    if (auto status = requireMarker(); !status.isOk())
        return errorResult<std::string>(std::move(status));

    auto fileLock = CacheFileLock::acquire(lockPath());
    if (!fileLock.isOk())
        return errorResult<std::string>(fileLock.status());

    const auto path = entryPath(key);
    auto payload = readCachePayload(path, options_);
    if (!payload.isOk()) {
        if (payload.status().code() != StatusCode::NotFound) {
            logTo(logger_,
                LogLevel::Warn,
                "DiskCache",
                "get failed namespace={} key={} status={}",
                __FILE__,
                __LINE__,
                key.namespace_,
                key.key_,
                payload.status().toString());
        }
        return errorResult<std::string>(payload.status());
    }

    if (auto status = requirePayloadMatchesKey(*payload, key); !status.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "DiskCache",
            "get failed namespace={} key={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.key_,
            status.toString());
        return errorResult<std::string>(std::move(status));
    }

    if (expiredUnixMillis(*clock_, payload->expiresAtUnixMillis_)) {
        std::error_code ec;
        fs::remove(path, ec);
        return errorResult<std::string>(Status::notFound("cache entry expired"));
    }

    if (auto status = touchCacheEntry(path); !status.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "DiskCache",
            "touch failed namespace={} key={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.key_,
            status.toString());
    }

    return okResult(payload->value_);
}

Status DiskCache::put(const CacheKey& key, std::string value, CacheWriteOptions options)
{
    if (auto status = validateCacheKey(key); !status.isOk())
        return status;
    if (options.ttl_ && *options.ttl_ <= Clock::Duration::zero())
        return Status::invalidArgument("cache ttl must be positive");
    if (options_.defaultTtl_ && *options_.defaultTtl_ <= Clock::Duration::zero())
        return Status::invalidArgument("cache default ttl must be positive");
    if (options_.maxEntries_ == 0)
        return Status::invalidArgument("cache max entries must be greater than 0");
    if (auto status = requireValueWithinLimit(value.size(), options_); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    if (auto dir = ensureDir(directory_); !dir.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "DiskCache",
            "put failed namespace={} key={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.key_,
            dir.status().toString());
        return dir.status();
    }

    auto fileLock = CacheFileLock::acquire(lockPath());
    if (!fileLock.isOk())
        return fileLock.status();

    if (auto status = ensureCacheDirectory(); !status.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "DiskCache",
            "put failed namespace={} key={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.key_,
            status.toString());
        return status;
    }

    const auto path = entryPath(key);
    if (auto status = ensureDir(path.parent_path()); !status.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "DiskCache",
            "put failed namespace={} key={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.key_,
            status.status().toString());
        return status.status();
    }

    nlohmann::json payload {
        { "namespace", key.namespace_ },
        { "key", key.key_ },
        { "value", std::move(value) },
    };
    const auto expiresAt = expiresAtUnixMillis(options);
    payload["expires_at_unix_ms"] = expiresAt ? nlohmann::json(*expiresAt) : nlohmann::json(nullptr);

    if (auto result = writeFileAtomic(path, payload.dump()); !result.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "DiskCache",
            "put failed namespace={} key={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.key_,
            result.status().toString());
        return result.status();
    }
    auto status = evictOverflow();
    if (!status.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "DiskCache",
            "put capacity check failed namespace={} key={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.key_,
            status.toString());
    }
    return status;
}

Status DiskCache::remove(const CacheKey& key)
{
    if (auto status = validateCacheKey(key); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    std::error_code existsEc;
    if (!fs::exists(directory_, existsEc)) {
        if (existsEc)
            return Status::internal("failed to check cache directory: " + existsEc.message());
        return Status::notFound("cache entry not found");
    }
    if (auto status = requireMarker(); !status.isOk())
        return status;

    auto fileLock = CacheFileLock::acquire(lockPath());
    if (!fileLock.isOk())
        return fileLock.status();

    std::error_code ec;
    const bool removed = fs::remove(entryPath(key), ec);
    if (ec) {
        auto status = Status::internal("failed to remove cache entry: " + ec.message());
        logTo(logger_,
            LogLevel::Warn,
            "DiskCache",
            "remove failed namespace={} key={} status={}",
            __FILE__,
            __LINE__,
            key.namespace_,
            key.key_,
            status.toString());
        return status;
    }
    if (!removed)
        return Status::notFound("cache entry not found");
    return Status::ok();
}

Status DiskCache::clear()
{
    std::lock_guard lock(mutex_);
    if (auto status = requireMarker(); !status.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "DiskCache",
            "clear rejected directory={} status={}",
            __FILE__,
            __LINE__,
            directory_.string(),
            status.toString());
        return status;
    }

    std::error_code existsEc;
    if (fs::exists(directory_, existsEc)) {
        if (existsEc)
            return Status::internal("failed to check cache directory: " + existsEc.message());
        auto fileLock = CacheFileLock::acquire(lockPath());
        if (!fileLock.isOk())
            return fileLock.status();
        if (auto status = requireMarker(); !status.isOk())
            return status;

        std::error_code ec;
        fs::remove_all(directory_, ec);
        if (ec) {
            auto status = Status::internal("failed to clear cache: " + ec.message());
            logTo(logger_,
                LogLevel::Warn,
                "DiskCache",
                "clear failed directory={} status={}",
                __FILE__,
                __LINE__,
                directory_.string(),
                status.toString());
            return status;
        }
        return Status::ok();
    }

    if (existsEc)
        return Status::internal("failed to check cache directory: " + existsEc.message());
    return Status::ok();
}

Status DiskCache::clearNamespace(std::string_view namespaceName)
{
    if (containsNull(namespaceName))
        return Status::invalidArgument("cache namespace contains a null byte");

    std::lock_guard lock(mutex_);
    if (auto status = requireMarker(); !status.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "DiskCache",
            "clearNamespace rejected namespace={} status={}",
            __FILE__,
            __LINE__,
            namespaceName,
            status.toString());
        return status;
    }

    std::error_code existsEc;
    if (!fs::exists(directory_, existsEc)) {
        if (existsEc)
            return Status::internal("failed to check cache directory: " + existsEc.message());
        return Status::ok();
    }

    auto fileLock = CacheFileLock::acquire(lockPath());
    if (!fileLock.isOk())
        return fileLock.status();
    if (auto status = requireMarker(); !status.isOk())
        return status;

    std::error_code ec;
    fs::remove_all(namespaceDirectory(namespaceName), ec);
    if (ec) {
        auto status = Status::internal("failed to clear cache namespace: " + ec.message());
        logTo(logger_,
            LogLevel::Warn,
            "DiskCache",
            "clearNamespace failed namespace={} status={}",
            __FILE__,
            __LINE__,
            namespaceName,
            status.toString());
        return status;
    }
    return Status::ok();
}

Status DiskCache::compact()
{
    std::lock_guard lock(mutex_);

    std::error_code existsEc;
    if (!fs::exists(directory_, existsEc)) {
        if (existsEc)
            return Status::internal("failed to check cache directory: " + existsEc.message());
        return Status::ok();
    }
    if (auto status = requireMarker(); !status.isOk())
        return status;

    auto fileLock = CacheFileLock::acquire(lockPath());
    if (!fileLock.isOk())
        return fileLock.status();
    if (auto status = requireMarker(); !status.isOk())
        return status;

    auto temps = cacheTempFiles(directory_, options_.maxEntriesToScan_);
    if (!temps.isOk())
        return temps.status();
    for (const auto& temp : *temps) {
        std::error_code ec;
        fs::remove(temp.path(), ec);
        if (ec) {
            auto status = Status::internal("failed to remove cache temporary file: " + ec.message());
            logTo(logger_,
                LogLevel::Warn,
                "DiskCache",
                "compact failed path={} status={}",
                __FILE__,
                __LINE__,
                temp.path().string(),
                status.toString());
            return status;
        }
    }

    return evictOverflow();
}

std::size_t DiskCache::size() const
{
    std::lock_guard lock(mutex_);
    std::error_code existsEc;
    if (!fs::exists(directory_, existsEc) || existsEc)
        return 0;
    if (auto status = requireMarker(); !status.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "DiskCache",
            "size rejected directory={} status={}",
            __FILE__,
            __LINE__,
            directory_.string(),
            status.toString());
        return 0;
    }

    auto fileLock = CacheFileLock::acquire(lockPath());
    if (!fileLock.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "DiskCache",
            "size failed directory={} status={}",
            __FILE__,
            __LINE__,
            directory_.string(),
            fileLock.status().toString());
        return 0;
    }

    auto files = cacheEntryFiles(directory_, options_.maxEntriesToScan_);
    if (!files.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "DiskCache",
            "size scan failed directory={} status={}",
            __FILE__,
            __LINE__,
            directory_.string(),
            files.status().toString());
        return 0;
    }

    std::size_t count = 0;
    for (const auto& file : *files) {
        auto payload = readCachePayload(file.path(), options_);
        if (!payload.isOk()) {
            ++count;
            continue;
        }
        if (expiredUnixMillis(*clock_, payload->expiresAtUnixMillis_)) {
            std::error_code ec;
            fs::remove(file.path(), ec);
            continue;
        }
        ++count;
    }
    return count;
}

const std::filesystem::path& DiskCache::directory() const noexcept
{
    return directory_;
}

std::filesystem::path DiskCache::markerPath() const
{
    return directory_ / kCacheMarkerName;
}

std::filesystem::path DiskCache::namespaceDirectory(std::string_view namespaceName) const
{
    return directory_ / hexEncode(namespaceName);
}

std::filesystem::path DiskCache::entryPath(const CacheKey& key) const
{
    return namespaceDirectory(key.namespace_) / (hexEncode(key.key_) + ".json");
}

std::optional<std::int64_t> DiskCache::expiresAtUnixMillis(const CacheWriteOptions& options) const
{
    const auto ttl = options.ttl_.or_else([this] { return options_.defaultTtl_; });
    return expirationFromTtl(*clock_, ttl);
}

std::filesystem::path DiskCache::lockPath() const
{
    auto path = directory_;
    path += std::string(kCacheLockSuffix);
    return path;
}

Status DiskCache::ensureCacheDirectory() const
{
    if (auto result = ensureDir(directory_); !result.isOk())
        return result.status();

    std::error_code ec;
    const auto marker = markerPath();
    if (fs::exists(marker, ec)) {
        if (ec)
            return Status::internal("failed to check cache marker: " + ec.message());
        if (!fs::is_regular_file(marker, ec) || ec)
            return Status::failedPrecondition("cache marker path exists but is not a regular file");
        return validateCacheMarkerFile(marker);
    }
    if (ec)
        return Status::internal("failed to check cache marker: " + ec.message());

    auto written = writeFileAtomic(marker, kCacheMarkerContents);
    if (!written.isOk())
        return written.status();
    return Status::ok();
}

Status DiskCache::requireMarker() const
{
    std::error_code ec;
    if (!fs::exists(directory_, ec)) {
        if (ec)
            return Status::internal("failed to check cache directory: " + ec.message());
        return Status::ok();
    }

    if (!fs::is_directory(directory_, ec) || ec)
        return Status::failedPrecondition("cache path exists but is not a directory");

    const auto marker = markerPath();
    if (!fs::exists(marker, ec)) {
        if (ec)
            return Status::internal("failed to check cache marker: " + ec.message());
        return Status::failedPrecondition("cache marker is missing");
    }

    if (!fs::is_regular_file(marker, ec) || ec)
        return Status::failedPrecondition("cache marker path exists but is not a regular file");

    return validateCacheMarkerFile(marker);
}

Status DiskCache::evictOverflow()
{
    auto filesResult = cacheEntryFiles(directory_, options_.maxEntriesToScan_);
    if (!filesResult.isOk())
        return filesResult.status();

    auto files = std::move(*filesResult);
    std::uintmax_t totalBytes = 0;
    for (auto it = files.begin(); it != files.end();) {
        auto payload = readCachePayload(it->path(), options_);
        if (payload.isOk() && expiredUnixMillis(*clock_, payload->expiresAtUnixMillis_)) {
            std::error_code ec;
            fs::remove(it->path(), ec);
            if (ec) {
                logTo(logger_,
                    LogLevel::Warn,
                    "DiskCache",
                    "expired entry cleanup failed path={} error={}",
                    __FILE__,
                    __LINE__,
                    it->path().string(),
                    ec.message());
            }
            it = files.erase(it);
        } else {
            const auto size = fileSizeOrZero(*it);
            if (totalBytes > std::numeric_limits<std::uintmax_t>::max() - size) {
                totalBytes = std::numeric_limits<std::uintmax_t>::max();
            } else {
                totalBytes += size;
            }
            ++it;
        }
    }

    if (files.size() <= options_.maxEntries_
        && (!options_.maxDiskSizeBytes_ || totalBytes <= *options_.maxDiskSizeBytes_))
        return Status::ok();

    std::ranges::sort(files, [](const auto& lhs, const auto& rhs) {
        std::error_code leftEc;
        std::error_code rightEc;
        return lhs.last_write_time(leftEc) < rhs.last_write_time(rightEc);
    });

    for (std::size_t i = 0;
         i < files.size()
         && (files.size() - i > options_.maxEntries_
             || (options_.maxDiskSizeBytes_ && totalBytes > *options_.maxDiskSizeBytes_));
         ++i) {
        const auto removedBytes = fileSizeOrZero(files[i]);
        std::error_code ec;
        fs::remove(files[i].path(), ec);
        if (ec) {
            auto status = Status::internal("failed to evict cache entry: " + ec.message());
            logTo(logger_,
                LogLevel::Warn,
                "DiskCache",
                "eviction failed path={} status={}",
                __FILE__,
                __LINE__,
                files[i].path().string(),
                status.toString());
            return status;
        }
        totalBytes = removedBytes > totalBytes ? 0 : totalBytes - removedBytes;
    }
    return Status::ok();
}

} // namespace lc
