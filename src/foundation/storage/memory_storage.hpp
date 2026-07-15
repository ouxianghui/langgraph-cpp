#pragma once

#include "foundation/storage/i_storage.hpp"
#include "foundation/time/clock.hpp"

#include <map>
#include <mutex>

namespace lgc {

class MemoryStorage final : public IStorage {
public:
    explicit MemoryStorage(
        StorageLimits limits = {},
        const WallClock& clock = SystemWallClock::instance());
    ~MemoryStorage() override = default;

    MemoryStorage(const MemoryStorage&) = delete;
    MemoryStorage& operator=(const MemoryStorage&) = delete;

    [[nodiscard]] Result<void> put(
        StorageKey key,
        std::string value,
        const StoragePutOptions& options = {}) override;
    [[nodiscard]] Result<std::optional<StorageItem>> get(const StorageKey& key) override;
    [[nodiscard]] Result<StorageListResult> list(const StorageListOptions& options = {}) override;
    [[nodiscard]] Result<void> remove(const StorageKey& key) override;
    [[nodiscard]] Result<void> clearScope(std::string_view scope) override;
    [[nodiscard]] Status flush() override;
    [[nodiscard]] Status close() override;
    [[nodiscard]] bool isClosed() const noexcept override;

private:
    using MapKey = std::pair<std::string, std::string>;

    [[nodiscard]] static MapKey toMapKey(const StorageKey& key);
    [[nodiscard]] Status ensureOpenLocked() const;

    mutable std::mutex mutex_;
    std::map<MapKey, StorageItem> items_;
    StorageLimits limits_;
    const WallClock* clock_ { nullptr };
    bool closed_ { false };
};

} // namespace lgc
