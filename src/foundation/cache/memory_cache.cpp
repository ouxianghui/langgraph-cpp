#include "foundation/cache/cache.hpp"

#include "foundation/cache/cache_common.hh"

#include <utility>

namespace lgc {
namespace {
using namespace cache_detail;
}

MemoryCache::MemoryCache(CacheOptions options, const Clock& clock)
    : options_(options)
    , clock_(&clock)
{
}

Result<std::string> MemoryCache::get(const CacheKey& key)
{
    if (auto status = validateCacheKey(key); !status.isOk())
        return errorResult<std::string>(std::move(status));

    std::lock_guard lock(mutex_);
    const auto now = clock_->now();
    evictExpired(now);

    auto it = entries_.find(entryId(key));
    if (it == entries_.end())
        return errorResult<std::string>(Status::notFound("cache entry not found"));
    if (expired(it->second, now)) {
        lru_.erase(it->second.lru_);
        entries_.erase(it);
        return errorResult<std::string>(Status::notFound("cache entry expired"));
    }

    touchEntry(it);
    return okResult(it->second.value_);
}

Status MemoryCache::put(const CacheKey& key, std::string value, CacheWriteOptions options)
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
    evictExpired(clock_->now());

    const auto id = entryId(key);
    auto it = entries_.find(id);
    if (it != entries_.end()) {
        it->second.value_ = std::move(value);
        it->second.expiresAt_ = expiresAt(options);
        touchEntry(it);
    } else {
        lru_.push_front(id);
        entries_.emplace(id, Entry {
            .value_ = std::move(value),
            .expiresAt_ = expiresAt(options),
            .lru_ = lru_.begin(),
        });
    }

    evictOverflow();
    return Status::ok();
}

Status MemoryCache::remove(const CacheKey& key)
{
    if (auto status = validateCacheKey(key); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    auto it = entries_.find(entryId(key));
    if (it == entries_.end())
        return Status::notFound("cache entry not found");

    lru_.erase(it->second.lru_);
    entries_.erase(it);
    return Status::ok();
}

Status MemoryCache::clear()
{
    std::lock_guard lock(mutex_);
    entries_.clear();
    lru_.clear();
    return Status::ok();
}

Status MemoryCache::clearNamespace(std::string_view namespaceName)
{
    if (containsNull(namespaceName))
        return Status::invalidArgument("cache namespace contains a null byte");

    std::lock_guard lock(mutex_);
    const std::string prefix = hexEncode(namespaceName) + ':';
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->first.starts_with(prefix)) {
            lru_.erase(it->second.lru_);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    return Status::ok();
}

Status MemoryCache::compact()
{
    std::lock_guard lock(mutex_);
    evictExpired(clock_->now());
    evictOverflow();
    return Status::ok();
}

std::size_t MemoryCache::size() const
{
    std::lock_guard lock(mutex_);
    evictExpired(clock_->now());
    return entries_.size();
}

std::string MemoryCache::entryId(const CacheKey& key) const
{
    return hexEncode(key.namespace_) + ':' + hexEncode(key.key_);
}

std::optional<Clock::TimePoint> MemoryCache::expiresAt(const CacheWriteOptions& options) const
{
    const auto ttl = options.ttl_.or_else([this] { return options_.defaultTtl_; });
    if (!ttl)
        return std::nullopt;
    return clock_->now() + *ttl;
}

bool MemoryCache::expired(const Entry& entry, Clock::TimePoint now) const noexcept
{
    return entry.expiresAt_ && now >= *entry.expiresAt_;
}

void MemoryCache::touchEntry(std::unordered_map<std::string, Entry>::iterator it)
{
    lru_.erase(it->second.lru_);
    lru_.push_front(it->first);
    it->second.lru_ = lru_.begin();
}

void MemoryCache::evictExpired(Clock::TimePoint now) const
{
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (expired(it->second, now)) {
            lru_.erase(it->second.lru_);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

void MemoryCache::evictOverflow()
{
    while (entries_.size() > options_.maxEntries_ && !lru_.empty()) {
        const auto id = lru_.back();
        lru_.pop_back();
        entries_.erase(id);
    }
}

} // namespace lgc
