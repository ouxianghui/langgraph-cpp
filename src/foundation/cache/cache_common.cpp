#include "foundation/cache/cache_common.hh"

#include "foundation/filesystem/filesystem.hpp"

#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace lc::cache_detail {

CacheFileLock::CacheFileLock() = default;

CacheFileLock::~CacheFileLock()
{
    release();
}

CacheFileLock::CacheFileLock(CacheFileLock&& other) noexcept
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

CacheFileLock& CacheFileLock::operator=(CacheFileLock&& other) noexcept
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

Result<CacheFileLock> CacheFileLock::acquire(const fs::path& path)
{
    CacheFileLock lock;
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
        return Status::unavailable("failed to open cache lock file");

    if (!::LockFileEx(lock.handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &lock.overlapped_)) {
        ::CloseHandle(lock.handle_);
        lock.handle_ = INVALID_HANDLE_VALUE;
        return Status::unavailable("failed to acquire cache lock");
    }
#else
    lock.fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock.fd_ < 0)
        return Status::unavailable("failed to open cache lock file");

    if (::flock(lock.fd_, LOCK_EX) != 0) {
        ::close(lock.fd_);
        lock.fd_ = -1;
        return Status::unavailable("failed to acquire cache lock");
    }
#endif
    return std::move(lock);
}

void CacheFileLock::release() noexcept
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

[[nodiscard]] bool containsNull(std::string_view value) noexcept
{
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] std::string hexEncode(std::string_view value)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto ch : value)
        out << std::setw(2) << static_cast<unsigned int>(static_cast<unsigned char>(ch));
    return out.str();
}

[[nodiscard]] bool isHex(std::string_view value) noexcept
{
    if (value.empty() || value.size() % 2 != 0)
        return false;

    for (const char ch : value) {
        const bool digit = ch >= '0' && ch <= '9';
        const bool lower = ch >= 'a' && ch <= 'f';
        const bool upper = ch >= 'A' && ch <= 'F';
        if (!digit && !lower && !upper)
            return false;
    }
    return true;
}

[[nodiscard]] bool isCacheEntryFile(const fs::directory_entry& entry)
{
    std::error_code ec;
    if (!entry.is_regular_file(ec) || ec)
        return false;
    const auto& path = entry.path();
    return path.extension() == ".json" && isHex(path.stem().string());
}

[[nodiscard]] std::int64_t nowUnixMillis(const Clock& clock)
{
    using namespace std::chrono;
    if (&clock == &SteadyClock::instance())
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    return duration_cast<milliseconds>(clock.now().time_since_epoch()).count();
}

[[nodiscard]] std::optional<std::int64_t> expirationFromTtl(const Clock& clock, std::optional<Clock::Duration> ttl)
{
    if (!ttl)
        return std::nullopt;
    return nowUnixMillis(clock) + std::chrono::duration_cast<std::chrono::milliseconds>(*ttl).count();
}

[[nodiscard]] bool expiredUnixMillis(const Clock& clock, std::optional<std::int64_t> expiresAt) noexcept
{
    return expiresAt && nowUnixMillis(clock) >= *expiresAt;
}

[[nodiscard]] Result<std::string> requiredStringField(const nlohmann::json& payload, std::string_view name)
{
    const auto it = payload.find(std::string(name));
    if (it == payload.end() || !it->is_string())
        return Status::dataLoss("cache entry field '" + std::string(name) + "' must be a string");
    return it->get<std::string>();
}

[[nodiscard]] Result<std::optional<std::int64_t>> requiredExpirationField(const nlohmann::json& payload)
{
    const auto it = payload.find("expires_at_unix_ms");
    if (it == payload.end())
        return Status::dataLoss("cache entry field 'expires_at_unix_ms' is missing");
    if (it->is_null())
        return std::optional<std::int64_t> {};
    if (!it->is_number_integer())
        return Status::dataLoss("cache entry field 'expires_at_unix_ms' must be an integer or null");

    try {
        return std::optional<std::int64_t> { it->get<std::int64_t>() };
    } catch (const std::exception& e) {
        return Status::dataLoss(std::string("cache entry field 'expires_at_unix_ms' is invalid: ") + e.what());
    }
}

[[nodiscard]] Result<nlohmann::json> readJsonFile(const fs::path& path)
{
    auto text = readFile(path);
    if (!text.isOk())
        return text.status();

    try {
        return nlohmann::json::parse(*text);
    } catch (const std::exception& e) {
        return Status::dataLoss(std::string("failed to parse cache entry: ") + e.what());
    }
}

[[nodiscard]] Status requireValueWithinLimit(std::size_t valueBytes, const CacheOptions& options)
{
    if (options.maxValueBytes_ != 0 && valueBytes > options.maxValueBytes_)
        return Status::resourceExhausted("cache value exceeds configured maximum size");
    return Status::ok();
}

[[nodiscard]] Result<CachePayload> parseCachePayload(const nlohmann::json& payload, const CacheOptions& options)
{
    if (!payload.is_object())
        return Status::dataLoss("cache entry must be a JSON object");

    auto namespaceName = requiredStringField(payload, "namespace");
    if (!namespaceName.isOk())
        return namespaceName.status();

    auto key = requiredStringField(payload, "key");
    if (!key.isOk())
        return key.status();

    auto value = requiredStringField(payload, "value");
    if (!value.isOk())
        return value.status();
    if (auto status = requireValueWithinLimit(value->size(), options); !status.isOk())
        return status;

    auto expiresAt = requiredExpirationField(payload);
    if (!expiresAt.isOk())
        return expiresAt.status();

    CacheKey cacheKey {
        .namespace_ = *namespaceName,
        .key_ = *key,
    };
    if (auto status = validateCacheKey(cacheKey); !status.isOk())
        return Status::dataLoss(std::string(status.message()));

    return CachePayload {
        .namespace_ = std::move(*namespaceName),
        .key_ = std::move(*key),
        .value_ = std::move(*value),
        .expiresAtUnixMillis_ = *expiresAt,
    };
}

[[nodiscard]] Result<CachePayload> readCachePayload(const fs::path& path, const CacheOptions& options)
{
    auto payload = readJsonFile(path);
    if (!payload.isOk())
        return payload.status();
    return parseCachePayload(*payload, options);
}

[[nodiscard]] Status requirePayloadMatchesKey(const CachePayload& payload, const CacheKey& key)
{
    if (payload.namespace_ != key.namespace_ || payload.key_ != key.key_)
        return Status::dataLoss("cache entry payload does not match requested key");
    return Status::ok();
}

[[nodiscard]] Status addCacheEntryFile(
    std::vector<fs::directory_entry>& files,
    const fs::directory_entry& entry,
    std::size_t maxScanEntries)
{
    if (maxScanEntries != 0 && files.size() >= maxScanEntries)
        return Status::resourceExhausted("cache entry scan exceeded configured maximum");
    files.push_back(entry);
    return Status::ok();
}

[[nodiscard]] Result<std::vector<fs::directory_entry>> cacheEntryFiles(const fs::path& directory, std::size_t maxScanEntries)
{
    std::vector<fs::directory_entry> files;
    std::error_code ec;
    if (!fs::exists(directory, ec) || ec || !fs::is_directory(directory, ec) || ec)
        return files;

    for (fs::directory_iterator root(directory, ec), end; !ec && root != end; root.increment(ec)) {
        if (isCacheEntryFile(*root)) {
            if (auto status = addCacheEntryFile(files, *root, maxScanEntries); !status.isOk())
                return status;
            continue;
        }

        if (!root->is_directory(ec) || ec || !isHex(root->path().filename().string()))
            continue;

        std::error_code childEc;
        for (fs::directory_iterator child(root->path(), childEc), childEnd; !childEc && child != childEnd;
             child.increment(childEc)) {
            if (isCacheEntryFile(*child)) {
                if (auto status = addCacheEntryFile(files, *child, maxScanEntries); !status.isOk())
                    return status;
            }
        }
    }
    if (ec)
        return Status::internal("failed to scan cache entries: " + ec.message());
    return files;
}

[[nodiscard]] Status touchCacheEntry(const fs::path& path)
{
    std::error_code ec;
    fs::last_write_time(path, fs::file_time_type::clock::now(), ec);
    if (ec)
        return Status::internal("failed to update cache entry access time: " + ec.message());
    return Status::ok();
}

[[nodiscard]] Status validateCacheMarkerFile(const fs::path& marker)
{
    auto markerContents = readFile(marker, ReadFileOptions { .maxBytes_ = kCacheMarkerContents.size() + 1 });
    if (!markerContents.isOk())
        return markerContents.status();
    if (*markerContents != kCacheMarkerContents)
        return Status::failedPrecondition("cache marker contents are invalid");
    return Status::ok();
}

[[nodiscard]] std::uintmax_t fileSizeOrZero(const fs::directory_entry& file) noexcept
{
    std::error_code ec;
    const auto size = file.file_size(ec);
    return ec ? 0 : size;
}

[[nodiscard]] bool isCacheTempFile(const fs::directory_entry& entry)
{
    std::error_code ec;
    if (!entry.is_regular_file(ec) || ec)
        return false;
    const auto name = entry.path().filename().string();
    return name.starts_with(".tmp-") && name.ends_with(".tmp")
        && (name.find(".json-") != std::string::npos || name.find(".langgraph-cache-") != std::string::npos);
}

[[nodiscard]] Result<std::vector<fs::directory_entry>> cacheTempFiles(const fs::path& directory, std::size_t maxScanEntries)
{
    std::vector<fs::directory_entry> files;
    std::error_code ec;
    if (!fs::exists(directory, ec) || ec || !fs::is_directory(directory, ec) || ec)
        return files;

    for (fs::directory_iterator root(directory, ec), end; !ec && root != end; root.increment(ec)) {
        if (isCacheTempFile(*root)) {
            if (auto status = addCacheEntryFile(files, *root, maxScanEntries); !status.isOk())
                return status;
            continue;
        }

        if (!root->is_directory(ec) || ec || !isHex(root->path().filename().string()))
            continue;

        std::error_code childEc;
        for (fs::directory_iterator child(root->path(), childEc), childEnd; !childEc && child != childEnd;
             child.increment(childEc)) {
            if (isCacheTempFile(*child)) {
                if (auto status = addCacheEntryFile(files, *child, maxScanEntries); !status.isOk())
                    return status;
            }
        }
    }
    if (ec)
        return Status::internal("failed to scan cache temporary files: " + ec.message());
    return files;
}

} // namespace lc::cache_detail
