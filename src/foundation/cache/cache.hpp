#pragma once

#include "foundation/logging/logger.hpp"
#include "foundation/status/result.hpp"
#include "foundation/time/clock.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace lc {

struct CacheKey {
    std::string namespace_;
    std::string key_;
};

struct CacheOptions {
    /// Maximum number of entries. MemoryCache and DiskCache evict least-recently-used entries.
    /// DiskCache tracks recency with the entry file timestamp; LFU is intentionally not implemented.
    std::size_t maxEntries_ { 1024 };
    /// `0` means unbounded.
    std::size_t maxValueBytes_ { 16 * 1024 * 1024 };
    /// DiskCache-only payload file budget. `std::nullopt` means unbounded.
    std::optional<std::uintmax_t> maxDiskSizeBytes_;
    /// DiskCache-only scan safety limit. `0` means unbounded.
    std::size_t maxEntriesToScan_ { 100'000 };
    std::optional<Clock::Duration> defaultTtl_;
};

struct CacheWriteOptions {
    std::optional<Clock::Duration> ttl_;
};

class ICache {
public:
    virtual ~ICache() = default;

    [[nodiscard]] virtual Result<std::string> get(const CacheKey& key) = 0;
    [[nodiscard]] virtual Status put(const CacheKey& key, std::string value, CacheWriteOptions options = {}) = 0;
    [[nodiscard]] virtual Status remove(const CacheKey& key) = 0;
    [[nodiscard]] virtual Status clear() = 0;
    [[nodiscard]] virtual Status clearNamespace(std::string_view namespaceName) = 0;
    [[nodiscard]] virtual Status compact() = 0;
    [[nodiscard]] virtual std::size_t size() const = 0;
};

class MemoryCache final : public ICache {
public:
    explicit MemoryCache(CacheOptions options = {}, const Clock& clock = SteadyClock::instance());

    [[nodiscard]] Result<std::string> get(const CacheKey& key) override;
    [[nodiscard]] Status put(const CacheKey& key, std::string value, CacheWriteOptions options = {}) override;
    [[nodiscard]] Status remove(const CacheKey& key) override;
    [[nodiscard]] Status clear() override;
    [[nodiscard]] Status clearNamespace(std::string_view namespaceName) override;
    [[nodiscard]] Status compact() override;
    [[nodiscard]] std::size_t size() const override;

private:
    struct Entry {
        std::string value_;
        std::optional<Clock::TimePoint> expiresAt_;
        std::list<std::string>::iterator lru_;
    };

    [[nodiscard]] std::string entryId(const CacheKey& key) const;
    [[nodiscard]] std::optional<Clock::TimePoint> expiresAt(const CacheWriteOptions& options) const;
    [[nodiscard]] bool expired(const Entry& entry, Clock::TimePoint now) const noexcept;
    void touchEntry(std::unordered_map<std::string, Entry>::iterator it);
    void evictExpired(Clock::TimePoint now) const;
    void evictOverflow();

    CacheOptions options_;
    const Clock* clock_;
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, Entry> entries_;
    mutable std::list<std::string> lru_;
};

class DiskCache final : public ICache {
public:
    DiskCache(
        std::filesystem::path directory,
        CacheOptions options = {},
        const Clock& clock = SteadyClock::instance(),
        std::shared_ptr<ILogger> logger = Logger::defaultLogger());

    [[nodiscard]] Result<std::string> get(const CacheKey& key) override;
    [[nodiscard]] Status put(const CacheKey& key, std::string value, CacheWriteOptions options = {}) override;
    [[nodiscard]] Status remove(const CacheKey& key) override;
    [[nodiscard]] Status clear() override;
    [[nodiscard]] Status clearNamespace(std::string_view namespaceName) override;
    [[nodiscard]] Status compact() override;
    [[nodiscard]] std::size_t size() const override;

    [[nodiscard]] const std::filesystem::path& directory() const noexcept;

private:
    [[nodiscard]] std::filesystem::path markerPath() const;
    [[nodiscard]] std::filesystem::path namespaceDirectory(std::string_view namespaceName) const;
    [[nodiscard]] std::filesystem::path entryPath(const CacheKey& key) const;
    [[nodiscard]] std::optional<std::int64_t> expiresAtUnixMillis(const CacheWriteOptions& options) const;
    [[nodiscard]] std::filesystem::path lockPath() const;
    [[nodiscard]] Status ensureCacheDirectory() const;
    [[nodiscard]] Status requireMarker() const;
    [[nodiscard]] Status evictOverflow();

    std::filesystem::path directory_;
    CacheOptions options_;
    const Clock* clock_;
    std::shared_ptr<ILogger> logger_;
    mutable std::mutex mutex_;
};

[[nodiscard]] Status validateCacheKey(const CacheKey& key);

} // namespace lc
