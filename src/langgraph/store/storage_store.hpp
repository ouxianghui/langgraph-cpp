#pragma once

#include "foundation/storage/i_storage.hpp"
#include "langgraph/store/base_store.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace lgc {

struct StorageStoreOptions {
    /// Storage scope prefix used for long-lived store items.
    std::string scope_ { "langgraph/store" };
    std::size_t listPageSize_ { 100 };
};

/// Persistent BaseStore adapter backed by an IStorage implementation, including SQLiteStorage.
class StorageStore final : public BaseStore {
public:
    explicit StorageStore(
        std::shared_ptr<IStorage> storage,
        StorageStoreOptions options = {});

    [[nodiscard]] Result<std::vector<StoreBatchResult>> batch(
        std::vector<StoreOp> ops) override;

private:
    [[nodiscard]] Result<StorageKey> storageKeyFor(
        const StoreNamespace& nameSpace,
        std::string_view key) const;
    [[nodiscard]] Result<std::string> keyPrefixFor(const StoreNamespace& nameSpace) const;
    [[nodiscard]] Result<StoreItem> itemFromStorageValue(
        const StorageItem& item) const;
    [[nodiscard]] Result<void> validateOptions() const;

    std::shared_ptr<IStorage> storage_;
    StorageStoreOptions options_;
};

} // namespace lgc
