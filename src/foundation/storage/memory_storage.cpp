#include "foundation/storage/memory_storage.hpp"

#include <chrono>
#include <limits>
#include <optional>
#include <utility>

namespace lc {

MemoryStorage::MemoryStorage(StorageLimits limits, const WallClock& clock)
    : limits_(limits)
    , clock_(&clock)
{
}

MemoryStorage::MapKey MemoryStorage::toMapKey(const StorageKey& key)
{
    return { key.scope_, key.key_ };
}

Status MemoryStorage::ensureOpenLocked() const
{
    if (closed_)
        return Status::unavailable("memory storage is closed");
    return Status::ok();
}

Result<void> MemoryStorage::put(
    StorageKey key,
    std::string value,
    const StoragePutOptions& options)
{
    if (auto status = validateStorageKey(key, limits_); !status.isOk())
        return status;
    if (auto status = validateStoragePutOptions(options); !status.isOk())
        return status;
    if (auto status = validateStorageValue(value, limits_); !status.isOk())
        return status;

    const auto mapKey = toMapKey(key);
    const auto now = clock_->now();

    std::lock_guard lock(mutex_);
    if (auto status = ensureOpenLocked(); !status.isOk())
        return status;

    const auto it = items_.find(mapKey);
    const auto exists = it != items_.end();

    if (options.mode_ == StoragePutMode::InsertOnly && exists)
        return Status::alreadyExists("storage item already exists");
    if (options.mode_ == StoragePutMode::ReplaceOnly && !exists)
        return Status::notFound("storage item does not exist");
    if (options.expectedVersion_.has_value()) {
        if (!exists)
            return Status::notFound("storage item does not exist");
        if (it->second.version_ != *options.expectedVersion_)
            return Status::failedPrecondition("storage version mismatch");
    }
    if (!exists && limits_.maxItems_ != 0U && items_.size() >= limits_.maxItems_)
        return Status::resourceExhausted("memory storage item limit exceeded");

    const auto version = exists ? it->second.version_ + 1 : 1;
    if (exists && version == 0)
        return Status::resourceExhausted("storage item version exhausted");

    StorageItem item {
        .key_ = std::move(key),
        .value_ = std::move(value),
        .createdAt_ = exists ? it->second.createdAt_ : now,
        .updatedAt_ = now,
        .version_ = version,
    };

    items_[mapKey] = std::move(item);
    return okResult();
}

Result<std::optional<StorageItem>> MemoryStorage::get(const StorageKey& key)
{
    if (auto status = validateStorageKey(key, limits_); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    if (auto status = ensureOpenLocked(); !status.isOk())
        return status;

    const auto it = items_.find(toMapKey(key));
    if (it == items_.end())
        return std::optional<StorageItem> {};

    return std::optional<StorageItem>(it->second);
}

Result<StorageListResult> MemoryStorage::list(const StorageListOptions& options)
{
    auto cursorKey = storageKeyFromCursor(options.cursor_, limits_);
    if (!cursorKey.isOk())
        return cursorKey.status();
    if (auto status = validateStorageListOptions(options, limits_); !status.isOk())
        return status;

    const auto cursorMapKey = cursorKey->has_value()
        ? std::optional<MapKey>(toMapKey(**cursorKey))
        : std::optional<MapKey> {};

    StorageListResult result;
    std::optional<StorageKey> lastReturnedKey;

    std::lock_guard lock(mutex_);
    if (auto status = ensureOpenLocked(); !status.isOk())
        return status;

    for (const auto& [mapKey, item] : items_) {
        if (cursorMapKey.has_value() && mapKey <= *cursorMapKey)
            continue;
        if (!storageKeyMatchesListOptions(item.key_, options))
            continue;

        if (options.limit_ > 0 && result.items_.size() >= options.limit_) {
            if (lastReturnedKey.has_value())
                result.nextCursor_ = storageCursorFromKey(*lastReturnedKey);
            break;
        }

        result.items_.push_back(item);
        lastReturnedKey = item.key_;
    }

    return result;
}

Result<void> MemoryStorage::remove(const StorageKey& key)
{
    if (auto status = validateStorageKey(key, limits_); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    if (auto status = ensureOpenLocked(); !status.isOk())
        return status;

    items_.erase(toMapKey(key));
    return okResult();
}

Result<void> MemoryStorage::clearScope(std::string_view scope)
{
    if (auto status = validateStorageName(scope, "storage scope", limits_.maxScopeBytes_); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    if (auto status = ensureOpenLocked(); !status.isOk())
        return status;

    for (auto it = items_.begin(); it != items_.end();) {
        if (it->first.first == scope)
            it = items_.erase(it);
        else
            ++it;
    }

    return okResult();
}

Status MemoryStorage::flush()
{
    std::lock_guard lock(mutex_);
    return ensureOpenLocked();
}

Status MemoryStorage::close()
{
    std::lock_guard lock(mutex_);
    closed_ = true;
    return Status::ok();
}

bool MemoryStorage::isClosed() const noexcept
{
    std::lock_guard lock(mutex_);
    return closed_;
}

} // namespace lc
