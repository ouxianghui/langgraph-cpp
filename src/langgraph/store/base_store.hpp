#pragma once

#include "foundation/async/future.hpp"
#include "foundation/status/result.hpp"
#include "langgraph/store/store_types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace lc {

/// LangGraph-style long-term store.
///
/// Store is application memory, not graph execution history. Checkpoints are
/// owned by BaseCheckpointSaver; Store implementations own their own
/// synchronization, persistence, filtering, and optional semantic search
/// behavior.
class BaseStore {
public:
    virtual ~BaseStore() = default;

    [[nodiscard]] virtual Result<std::vector<StoreBatchResult>> batch(
        std::vector<StoreOp> ops) = 0;
    [[nodiscard]] virtual Future<std::vector<StoreBatchResult>> abatch(
        std::vector<StoreOp> ops);

    [[nodiscard]] Result<void> put(
        StoreNamespace nameSpace,
        std::string key,
        nlohmann::json value);
    [[nodiscard]] Result<std::optional<StoreItem>> get(
        const StoreNamespace& nameSpace,
        std::string_view key);
    [[nodiscard]] Result<std::vector<StoreSearchItem>> search(
        const StoreSearchOptions& options = {});
    [[nodiscard]] Result<void> deleteItem(
        const StoreNamespace& nameSpace,
        std::string_view key);
    [[nodiscard]] Result<std::vector<StoreNamespace>> listNamespaces(
        const StoreListNamespacesOptions& options = {});
};

} // namespace lc
