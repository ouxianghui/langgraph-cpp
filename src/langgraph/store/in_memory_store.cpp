#include "langgraph/store/store_common.hh"
#include "langgraph/store/in_memory_store.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <utility>

namespace lc {

using namespace detail;

namespace {

[[nodiscard]] Result<void> validateSearchOp(const StoreSearchOp& op)
{
    return validateStoreSearchOptions(StoreSearchOptions {
        .namespacePrefix_ = op.namespacePrefix_,
        .query_ = op.query_,
        .filter_ = op.filter_,
        .limit_ = op.limit_,
        .offset_ = op.offset_,
    });
}

[[nodiscard]] Result<void> validateListNamespacesOp(const StoreListNamespacesOp& op)
{
    for (const auto& condition : op.matchConditions_) {
        if (auto status = validateStoreNamespaceMatchCondition(condition); !status.isOk())
            return status.status();
    }
    return okResult();
}

} // namespace

Result<void> InMemoryStore::validateNamespace(const StoreNamespace& nameSpace)
{
    return validateStoreNamespace(nameSpace);
}

Result<void> InMemoryStore::validateNamespacePrefix(const StoreNamespace& nameSpace)
{
    return validateStoreNamespacePrefix(nameSpace);
}

Result<void> InMemoryStore::validateNamespaceSuffix(const StoreNamespace& nameSpace)
{
    return validateStoreNamespaceSuffix(nameSpace);
}

Result<void> InMemoryStore::validateKey(std::string_view key)
{
    return validateStoreKey(key);
}

Result<std::vector<StoreBatchResult>> InMemoryStore::batch(std::vector<StoreOp> ops)
{
    std::vector<StoreBatchResult> results;
    results.reserve(ops.size());
    std::map<std::pair<StoreNamespace, std::string>, StorePutOp> pendingPuts;

    std::lock_guard lock(mutex_);
    for (const auto& op : ops) {
        if (const auto* get = std::get_if<StoreGetOp>(&op)) {
            if (auto status = validateNamespace(get->namespace_); !status.isOk())
                return status.status();
            if (auto status = validateKey(get->key_); !status.isOk())
                return status.status();

            const auto bucket = items_.find(get->namespace_);
            if (bucket == items_.end()) {
                results.emplace_back(std::monostate {});
                continue;
            }
            const auto found = bucket->second.find(get->key_);
            if (found == bucket->second.end()) {
                results.emplace_back(std::monostate {});
                continue;
            }
            results.emplace_back(found->second);
            continue;
        }

        if (const auto* search = std::get_if<StoreSearchOp>(&op)) {
            if (auto status = validateSearchOp(*search); !status.isOk())
                return status.status();
            if (search->limit_ == 0U) {
                results.emplace_back(std::vector<StoreSearchItem> {});
                continue;
            }

            std::vector<StoreSearchItem> out;
            std::size_t skipped = 0;
            StoreNamespaceMatchCondition prefix {
                .matchType_ = StoreNamespaceMatchType::Prefix,
                .path_ = search->namespacePrefix_,
            };
            for (const auto& [nameSpace, bucket] : items_) {
                if (!storeNamespaceMatches(nameSpace, prefix))
                    continue;
                for (const auto& [key, item] : bucket) {
                    (void)key;
                    auto matched = storeItemMatchesFilter(item, search->filter_);
                    if (!matched.isOk())
                        return matched.status();
                    if (!*matched)
                        continue;
                    if (skipped < search->offset_) {
                        ++skipped;
                        continue;
                    }
                    out.push_back(storeSearchItemFromItem(item));
                    if (out.size() >= search->limit_)
                        break;
                }
                if (out.size() >= search->limit_)
                    break;
            }
            results.emplace_back(std::move(out));
            continue;
        }

        if (const auto* put = std::get_if<StorePutOp>(&op)) {
            if (auto status = validateNamespace(put->namespace_); !status.isOk())
                return status.status();
            if (auto status = validateKey(put->key_); !status.isOk())
                return status.status();
            pendingPuts[{ put->namespace_, put->key_ }] = *put;
            results.emplace_back(std::monostate {});
            continue;
        }

        if (const auto* list = std::get_if<StoreListNamespacesOp>(&op)) {
            if (auto status = validateListNamespacesOp(*list); !status.isOk())
                return status.status();
            if (list->limit_ == 0U) {
                results.emplace_back(std::vector<StoreNamespace> {});
                continue;
            }

            std::set<StoreNamespace, NamespaceLess> namespaces;
            for (const auto& [nameSpace, bucket] : items_) {
                (void)bucket;
                if (!storeNamespaceMatchesAll(nameSpace, list->matchConditions_))
                    continue;
                auto emitted = nameSpace;
                if (list->maxDepth_.has_value() && emitted.size() > *list->maxDepth_)
                    emitted.resize(*list->maxDepth_);
                namespaces.insert(std::move(emitted));
            }

            std::vector<StoreNamespace> out;
            std::size_t skipped = 0;
            for (const auto& nameSpace : namespaces) {
                if (skipped < list->offset_) {
                    ++skipped;
                    continue;
                }
                out.push_back(nameSpace);
                if (out.size() >= list->limit_)
                    break;
            }
            results.emplace_back(std::move(out));
            continue;
        }
    }

    for (auto& [id, put] : pendingPuts) {
        (void)id;
        if (!put.value_.has_value()) {
            auto bucket = items_.find(put.namespace_);
            if (bucket == items_.end())
                continue;
            bucket->second.erase(put.key_);
            if (bucket->second.empty())
                items_.erase(bucket);
            continue;
        }

        const auto now = std::chrono::system_clock::now();
        auto& bucket = items_[put.namespace_];
        auto found = bucket.find(put.key_);
        if (found == bucket.end()) {
            auto itemNamespace = put.namespace_;
            auto itemKey = put.key_;
            bucket.emplace(put.key_, StoreItem {
                                       .namespace_ = std::move(itemNamespace),
                                       .key_ = std::move(itemKey),
                                       .value_ = std::move(*put.value_),
                                       .createdAt_ = now,
                                       .updatedAt_ = now,
                                   });
            continue;
        }

        found->second.value_ = std::move(*put.value_);
        found->second.updatedAt_ = now;
    }

    return okResult(std::move(results));
}

} // namespace lc
