#pragma once

#include "langgraph/store/base_store.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace lgc {

/// Thread-safe in-memory namespaced key-value store for edge runtime tests and local workflows.
class InMemoryStore final : public BaseStore {
public:
    [[nodiscard]] Result<std::vector<StoreBatchResult>> batch(
        std::vector<StoreOp> ops) override;

private:
    struct NamespaceLess {
        [[nodiscard]] bool operator()(const StoreNamespace& lhs, const StoreNamespace& rhs) const noexcept
        {
            return lhs < rhs;
        }
    };

    [[nodiscard]] static Result<void> validateNamespace(const StoreNamespace& nameSpace);
    [[nodiscard]] static Result<void> validateNamespacePrefix(const StoreNamespace& nameSpace);
    [[nodiscard]] static Result<void> validateNamespaceSuffix(const StoreNamespace& nameSpace);
    [[nodiscard]] static Result<void> validateKey(std::string_view key);
    mutable std::mutex mutex_;
    std::map<StoreNamespace, std::map<std::string, StoreItem>, NamespaceLess> items_;
};

} // namespace lgc
