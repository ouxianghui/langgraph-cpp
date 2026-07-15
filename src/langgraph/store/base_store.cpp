#include "langgraph/store/base_store.hpp"

#include <memory>
#include <string>
#include <utility>

namespace lgc {

namespace {

[[nodiscard]] Result<StoreBatchResult> singleResult(Result<std::vector<StoreBatchResult>> results)
{
    if (!results.isOk())
        return results.status();
    if (results->size() != 1U)
        return Status::internal("store batch returned an unexpected result count");
    return std::move(results->front());
}

} // namespace

Future<std::vector<StoreBatchResult>> BaseStore::abatch(std::vector<StoreOp> ops)
{
    Promise<std::vector<StoreBatchResult>> promise;
    auto future = promise.future();
    auto result = batch(std::move(ops));
    if (!result.isOk()) {
        (void)promise.reject(result.status());
        return future;
    }
    (void)promise.resolve(std::move(*result));
    return future;
}

Result<void> BaseStore::put(
    StoreNamespace nameSpace,
    std::string key,
    nlohmann::json value)
{
    auto result = singleResult(batch(std::vector<StoreOp> {
        StorePutOp {
            .namespace_ = std::move(nameSpace),
            .key_ = std::move(key),
            .value_ = std::move(value),
        },
    }));
    if (!result.isOk())
        return result.status();
    if (!std::holds_alternative<std::monostate>(*result))
        return Status::internal("store put returned an unexpected result type");
    return okResult();
}

Result<std::optional<StoreItem>> BaseStore::get(
    const StoreNamespace& nameSpace,
    std::string_view key)
{
    auto result = singleResult(batch(std::vector<StoreOp> {
        StoreGetOp {
            .namespace_ = nameSpace,
            .key_ = std::string(key),
        },
    }));
    if (!result.isOk())
        return result.status();
    if (std::holds_alternative<std::monostate>(*result))
        return std::optional<StoreItem> {};
    if (auto* item = std::get_if<StoreItem>(&*result))
        return std::optional<StoreItem>(std::move(*item));
    return Status::internal("store get returned an unexpected result type");
}

Result<std::vector<StoreSearchItem>> BaseStore::search(const StoreSearchOptions& options)
{
    auto result = singleResult(batch(std::vector<StoreOp> {
        StoreSearchOp {
            .namespacePrefix_ = options.namespacePrefix_,
            .query_ = options.query_,
            .filter_ = options.filter_,
            .limit_ = options.limit_,
            .offset_ = options.offset_,
        },
    }));
    if (!result.isOk())
        return result.status();
    if (auto* items = std::get_if<std::vector<StoreSearchItem>>(&*result))
        return std::move(*items);
    return Status::internal("store search returned an unexpected result type");
}

Result<void> BaseStore::deleteItem(
    const StoreNamespace& nameSpace,
    std::string_view key)
{
    auto result = singleResult(batch(std::vector<StoreOp> {
        StorePutOp {
            .namespace_ = nameSpace,
            .key_ = std::string(key),
            .value_ = std::nullopt,
        },
    }));
    if (!result.isOk())
        return result.status();
    if (!std::holds_alternative<std::monostate>(*result))
        return Status::internal("store delete returned an unexpected result type");
    return okResult();
}

Result<std::vector<StoreNamespace>> BaseStore::listNamespaces(
    const StoreListNamespacesOptions& options)
{
    std::vector<StoreNamespaceMatchCondition> conditions;
    if (!options.prefix_.empty()) {
        conditions.push_back(StoreNamespaceMatchCondition {
            .matchType_ = StoreNamespaceMatchType::Prefix,
            .path_ = options.prefix_,
        });
    }
    if (!options.suffix_.empty()) {
        conditions.push_back(StoreNamespaceMatchCondition {
            .matchType_ = StoreNamespaceMatchType::Suffix,
            .path_ = options.suffix_,
        });
    }

    auto result = singleResult(batch(std::vector<StoreOp> {
        StoreListNamespacesOp {
            .matchConditions_ = std::move(conditions),
            .maxDepth_ = options.maxDepth_,
            .limit_ = options.limit_,
            .offset_ = options.offset_,
        },
    }));
    if (!result.isOk())
        return result.status();
    if (auto* namespaces = std::get_if<std::vector<StoreNamespace>>(&*result))
        return std::move(*namespaces);
    return Status::internal("store list_namespaces returned an unexpected result type");
}

} // namespace lgc
