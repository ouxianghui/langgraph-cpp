#include "langgraph/store/store_common.hh"

#include <algorithm>
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

StorageStore::StorageStore(
    std::shared_ptr<IStorage> storage,
    StorageStoreOptions options)
    : storage_(std::move(storage))
    , options_(std::move(options))
{
}

Result<void> StorageStore::validateOptions() const
{
    if (!storage_)
        return Status::invalidArgument("storage store storage cannot be null");
    if (options_.scope_.empty())
        return Status::invalidArgument("storage store scope cannot be empty");
    if (options_.listPageSize_ == 0U)
        return Status::invalidArgument("storage store list page size must be greater than zero");
    return okResult();
}

Result<std::string> StorageStore::keyPrefixFor(const StoreNamespace& nameSpace) const
{
    if (auto status = validateStoreNamespacePrefix(nameSpace); !status.isOk())
        return status.status();

    std::string key = "items/";
    for (const auto& part : nameSpace) {
        key.append(storeHexEncode(part));
        key.push_back('/');
    }
    return key;
}

Result<StorageKey> StorageStore::storageKeyFor(
    const StoreNamespace& nameSpace,
    std::string_view key) const
{
    if (auto status = validateStoreNamespace(nameSpace); !status.isOk())
        return status.status();
    if (auto status = validateStoreKey(key); !status.isOk())
        return status.status();
    auto prefix = keyPrefixFor(nameSpace);
    if (!prefix.isOk())
        return prefix.status();

    return StorageKey {
        .scope_ = options_.scope_,
        .key_ = *prefix + "@/" + storeHexEncode(key),
    };
}

Result<StoreItem> StorageStore::itemFromStorageValue(const StorageItem& item) const
{
    auto envelope = parseStoreEnvelope(item.value_);
    if (!envelope.isOk())
        return envelope.status();
    return storeItemFromEnvelope(std::move(*envelope), item);
}

Result<std::vector<StoreBatchResult>> StorageStore::batch(std::vector<StoreOp> ops)
{
    if (auto status = validateOptions(); !status.isOk())
        return status.status();

    std::vector<StoreBatchResult> results;
    results.reserve(ops.size());
    std::map<std::pair<StoreNamespace, std::string>, StorePutOp> pendingPuts;

    for (const auto& op : ops) {
        if (const auto* get = std::get_if<StoreGetOp>(&op)) {
            auto storageKey = storageKeyFor(get->namespace_, get->key_);
            if (!storageKey.isOk())
                return storageKey.status();
            auto item = storage_->get(*storageKey);
            if (!item.isOk())
                return item.status();
            if (!item->has_value()) {
                results.emplace_back(std::monostate {});
                continue;
            }
            auto storeItem = itemFromStorageValue(**item);
            if (!storeItem.isOk())
                return storeItem.status();
            results.emplace_back(std::move(*storeItem));
            continue;
        }

        if (const auto* search = std::get_if<StoreSearchOp>(&op)) {
            if (auto status = validateSearchOp(*search); !status.isOk())
                return status.status();
            if (search->limit_ == 0U) {
                results.emplace_back(std::vector<StoreSearchItem> {});
                continue;
            }

            auto prefix = keyPrefixFor(search->namespacePrefix_);
            if (!prefix.isOk())
                return prefix.status();

            std::vector<StoreSearchItem> out;
            std::size_t skipped = 0;
            std::string cursor;
            for (;;) {
                auto page = storage_->list(StorageListOptions {
                    .scope_ = options_.scope_,
                    .keyPrefix_ = *prefix,
                    .limit_ = options_.listPageSize_,
                    .cursor_ = cursor,
                });
                if (!page.isOk())
                    return page.status();
                for (const auto& item : page->items_) {
                    auto storeItem = itemFromStorageValue(item);
                    if (!storeItem.isOk())
                        return storeItem.status();
                    auto matched = storeItemMatchesFilter(*storeItem, search->filter_);
                    if (!matched.isOk())
                        return matched.status();
                    if (!*matched)
                        continue;
                    if (skipped < search->offset_) {
                        ++skipped;
                        continue;
                    }
                    out.push_back(storeSearchItemFromItem(std::move(*storeItem)));
                    if (out.size() >= search->limit_)
                        break;
                }
                if (out.size() >= search->limit_ || page->nextCursor_.empty())
                    break;
                cursor = std::move(page->nextCursor_);
            }
            results.emplace_back(std::move(out));
            continue;
        }

        if (const auto* put = std::get_if<StorePutOp>(&op)) {
            auto storageKey = storageKeyFor(put->namespace_, put->key_);
            if (!storageKey.isOk())
                return storageKey.status();
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

            auto items = listStorageStoreItems(storage_, options_, "items/");
            if (!items.isOk())
                return items.status();

            std::set<StoreNamespace> namespaces;
            for (const auto& item : *items) {
                if (!storeNamespaceMatchesAll(item.item_.namespace_, list->matchConditions_))
                    continue;
                auto emitted = item.item_.namespace_;
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
        auto storageKey = storageKeyFor(put.namespace_, put.key_);
        if (!storageKey.isOk())
            return storageKey.status();

        if (!put.value_.has_value()) {
            if (auto removed = storage_->remove(*storageKey); !removed.isOk())
                return removed.status();
            continue;
        }

        auto envelope = storeEnvelopeFromItem(put.namespace_, put.key_, std::move(*put.value_));
        if (auto stored = storage_->put(
                std::move(*storageKey),
                envelope.dump(),
                StoragePutOptions { .mode_ = StoragePutMode::Upsert });
            !stored.isOk()) {
            return stored.status();
        }
    }

    return okResult(std::move(results));
}

} // namespace lc
