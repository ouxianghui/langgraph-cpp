#pragma once

#include "foundation/cache/cache.hpp"
#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace lgc::cache_detail {

namespace fs = std::filesystem;

inline constexpr std::string_view kCacheMarkerName = ".langgraph-cache";
inline constexpr std::string_view kCacheMarkerContents = "langgraph-cpp disk cache\n";
inline constexpr std::string_view kCacheLockSuffix = ".langgraph-cache.lock";

class CacheFileLock final {
public:
    CacheFileLock();
    ~CacheFileLock();

    CacheFileLock(const CacheFileLock&) = delete;
    CacheFileLock& operator=(const CacheFileLock&) = delete;

    CacheFileLock(CacheFileLock&& other) noexcept;
    CacheFileLock& operator=(CacheFileLock&& other) noexcept;

    [[nodiscard]] static Result<CacheFileLock> acquire(const fs::path& path);

private:
    void release() noexcept;

#if defined(_WIN32)
    HANDLE handle_ { INVALID_HANDLE_VALUE };
    OVERLAPPED overlapped_ {};
#else
    int fd_ { -1 };
#endif
};

struct CachePayload {
    std::string namespace_;
    std::string key_;
    std::string value_;
    std::optional<std::int64_t> expiresAtUnixMillis_;
};

[[nodiscard]] bool containsNull(std::string_view value) noexcept;
[[nodiscard]] std::string hexEncode(std::string_view value);
[[nodiscard]] std::int64_t nowUnixMillis(const Clock& clock);
[[nodiscard]] std::optional<std::int64_t> expirationFromTtl(
    const Clock& clock,
    std::optional<Clock::Duration> ttl);
[[nodiscard]] bool expiredUnixMillis(const Clock& clock, std::optional<std::int64_t> expiresAt) noexcept;
[[nodiscard]] Status requireValueWithinLimit(std::size_t valueBytes, const CacheOptions& options);
[[nodiscard]] Result<CachePayload> readCachePayload(const fs::path& path, const CacheOptions& options);
[[nodiscard]] Status requirePayloadMatchesKey(const CachePayload& payload, const CacheKey& key);
[[nodiscard]] Result<std::vector<fs::directory_entry>> cacheEntryFiles(
    const fs::path& directory,
    std::size_t maxScanEntries);
[[nodiscard]] Status touchCacheEntry(const fs::path& path);
[[nodiscard]] Status validateCacheMarkerFile(const fs::path& marker);
[[nodiscard]] std::uintmax_t fileSizeOrZero(const fs::directory_entry& file) noexcept;
[[nodiscard]] Result<std::vector<fs::directory_entry>> cacheTempFiles(
    const fs::path& directory,
    std::size_t maxScanEntries);

} // namespace lgc::cache_detail
