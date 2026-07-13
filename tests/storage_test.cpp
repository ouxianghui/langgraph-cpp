#include "foundation/storage/i_storage.hpp"
#include "foundation/storage/memory_storage.hpp"
#include "foundation/time/clock.hpp"
#include "langgraph/store/store.hpp"

#if LANGGRAPH_CPP_WITH_SQLITE
#include "foundation/storage/sqlite_storage.hpp"
#endif

#include <cassert>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

[[nodiscard]] std::string hexEncode(std::string_view value)
{
    constexpr std::array<char, 16> digits {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };

    std::string out;
    out.reserve(value.size() * 2);
    for (const auto ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        out.push_back(digits[byte >> 4U]);
        out.push_back(digits[byte & 0x0FU]);
    }
    return out;
}

[[nodiscard]] nlohmann::json namespaceJson(const lc::StoreNamespace& nameSpace)
{
    auto out = nlohmann::json::array();
    for (const auto& part : nameSpace)
        out.push_back(part);
    return out;
}

[[nodiscard]] lc::StorageKey storageStoreKey(
    const lc::StoreNamespace& nameSpace,
    std::string_view key)
{
    std::string storageKey = "items/";
    for (const auto& part : nameSpace) {
        storageKey.append(hexEncode(part));
        storageKey.push_back('/');
    }
    storageKey.append("@/");
    storageKey.append(hexEncode(key));
    return lc::StorageKey {
        .scope_ = "langgraph/store",
        .key_ = std::move(storageKey),
    };
}

void exerciseStorage(lc::IStorage& storage)
{
    const lc::StorageKey first { .scope_ = "thread-a", .key_ = "checkpoint-1" };
    const lc::StorageKey second { .scope_ = "thread-a", .key_ = "checkpoint-2" };
    const lc::StorageKey third { .scope_ = "thread-a", .key_ = "checkpoint-3" };
    const lc::StorageKey otherScope { .scope_ = "thread-b", .key_ = "checkpoint-1" };

    assert(storage.put(first, R"({"step":1})").isOk());
    assert(storage.put(second, R"({"step":2})").isOk());
    assert(storage.put(third, R"({"step":3})").isOk());
    assert(storage.put(otherScope, R"({"step":1})").isOk());

    auto item = storage.get(first);
    assert(item.isOk());
    assert(item->has_value());
    assert((*item)->key_ == first);
    assert((*item)->value_ == R"({"step":1})");
    assert((*item)->version_ == 1);
    const auto createdAt = (*item)->createdAt_;

    auto duplicate = storage.put(first, "duplicate", lc::StoragePutOptions {
        .mode_ = lc::StoragePutMode::InsertOnly,
    });
    assert(!duplicate.isOk());
    assert(duplicate.status().code() == lc::StatusCode::AlreadyExists);

    auto missingReplace = storage.put({ .scope_ = "thread-a", .key_ = "missing" }, "missing", lc::StoragePutOptions {
        .mode_ = lc::StoragePutMode::ReplaceOnly,
    });
    assert(!missingReplace.isOk());
    assert(missingReplace.status().code() == lc::StatusCode::NotFound);

    auto mismatch = storage.put(first, R"({"step":9})", lc::StoragePutOptions {
        .expectedVersion_ = 99,
    });
    assert(!mismatch.isOk());
    assert(mismatch.status().code() == lc::StatusCode::FailedPrecondition);

    assert(storage.put(first, R"({"step":10})", lc::StoragePutOptions {
        .expectedVersion_ = 1,
    }).isOk());
    item = storage.get(first);
    assert(item.isOk());
    assert(item->has_value());
    assert((*item)->value_ == R"({"step":10})");
    assert((*item)->createdAt_ == createdAt);
    assert((*item)->updatedAt_ >= createdAt);
    assert((*item)->version_ == 2);

    auto list = storage.list(lc::StorageListOptions {
        .scope_ = "thread-a",
        .keyPrefix_ = "checkpoint-",
    });
    assert(list.isOk());
    assert(list->items_.size() == 3);
    assert(list->items_[0].key_.key_ == "checkpoint-1");
    assert(list->items_[1].key_.key_ == "checkpoint-2");
    assert(list->items_[2].key_.key_ == "checkpoint-3");
    assert(list->nextCursor_.empty());

    auto firstPage = storage.list(lc::StorageListOptions {
        .scope_ = "thread-a",
        .keyPrefix_ = "checkpoint-",
        .limit_ = 1,
    });
    assert(firstPage.isOk());
    assert(firstPage->items_.size() == 1);
    assert(!firstPage->nextCursor_.empty());

    auto secondPage = storage.list(lc::StorageListOptions {
        .scope_ = "thread-a",
        .keyPrefix_ = "checkpoint-",
        .limit_ = 2,
        .cursor_ = firstPage->nextCursor_,
    });
    assert(secondPage.isOk());
    assert(secondPage->items_.size() == 2);
    assert(secondPage->items_[0].key_.key_ == "checkpoint-2");
    assert(secondPage->items_[1].key_.key_ == "checkpoint-3");
    assert(secondPage->nextCursor_.empty());

    assert(storage.remove(second).isOk());
    item = storage.get(second);
    assert(item.isOk());
    assert(!item->has_value());

    assert(storage.clearScope("thread-a").isOk());
    list = storage.list(lc::StorageListOptions { .scope_ = "thread-a" });
    assert(list.isOk());
    assert(list->items_.empty());

    list = storage.list(lc::StorageListOptions { .scope_ = "thread-b" });
    assert(list.isOk());
    assert(list->items_.size() == 1);

    auto invalidCursor = storage.list(lc::StorageListOptions { .cursor_ = "not-a-cursor" });
    assert(!invalidCursor.isOk());
    assert(invalidCursor.status().code() == lc::StatusCode::InvalidArgument);
}

void exerciseStore(lc::BaseStore& store)
{
    const std::vector<std::string> ns { "agents", "thread-a" };

    assert(store.put(ns, "profile", {
        { "name", "edge" },
        { "score", 7 },
    }).isOk());
    assert(store.put({ "agents", "thread-b" }, "profile", { { "name", "other" } }).isOk());
    assert(store.put({ "logs", "thread-a" }, "entry", { { "ok", true } }).isOk());

    auto item = store.get(ns, "profile");
    assert(item.isOk());
    assert(item->has_value());
    assert((*item)->namespace_ == ns);
    assert((*item)->key_ == "profile");
    assert((*item)->value_.at("name") == "edge");
    assert((*item)->createdAt_ <= (*item)->updatedAt_);

    auto missing = store.get(ns, "missing");
    assert(missing.isOk());
    assert(!missing->has_value());

    auto agents = store.search(lc::StoreSearchOptions {
        .namespacePrefix_ = { "agents" },
    });
    assert(agents.isOk());
    assert(agents->size() == 2);
    assert(agents->at(0).namespace_ == std::vector<std::string>({ "agents", "thread-a" }));
    assert(agents->at(1).namespace_ == std::vector<std::string>({ "agents", "thread-b" }));

    auto limited = store.search(lc::StoreSearchOptions {
        .namespacePrefix_ = { "agents" },
        .limit_ = 1,
        .offset_ = 1,
    });
    assert(limited.isOk());
    assert(limited->size() == 1);
    assert(limited->front().namespace_ == std::vector<std::string>({ "agents", "thread-b" }));
    assert(!limited->front().score_.has_value());

    auto filtered = store.search(lc::StoreSearchOptions {
        .namespacePrefix_ = { "agents" },
        .filter_ = nlohmann::json {
            { "score", { { "$gte", 7 } } },
        },
    });
    assert(filtered.isOk());
    assert(filtered->size() == 1);
    assert(filtered->front().namespace_ == ns);

    auto missingFilter = store.search(lc::StoreSearchOptions {
        .namespacePrefix_ = { "agents" },
        .filter_ = nlohmann::json {
            { "score", { { "$gt", 99 } } },
        },
    });
    assert(missingFilter.isOk());
    assert(missingFilter->empty());

    auto unsupportedQuery = store.search(lc::StoreSearchOptions {
        .namespacePrefix_ = { "agents" },
        .query_ = std::string("edge profile"),
    });
    assert(!unsupportedQuery.isOk());
    assert(unsupportedQuery.status().code() == lc::StatusCode::Unimplemented);

    auto namespaces = store.listNamespaces(lc::StoreListNamespacesOptions {
        .prefix_ = { "agents" },
    });
    assert(namespaces.isOk());
    assert(namespaces->size() == 2);
    assert(namespaces->at(0) == std::vector<std::string>({ "agents", "thread-a" }));
    assert(namespaces->at(1) == std::vector<std::string>({ "agents", "thread-b" }));

    auto shallowNamespaces = store.listNamespaces(lc::StoreListNamespacesOptions {
        .prefix_ = { "agents" },
        .maxDepth_ = 1,
    });
    assert(shallowNamespaces.isOk());
    assert(shallowNamespaces->size() == 1);
    assert(shallowNamespaces->front() == std::vector<std::string>({ "agents" }));

    auto suffixedNamespaces = store.listNamespaces(lc::StoreListNamespacesOptions {
        .suffix_ = { "thread-a" },
    });
    assert(suffixedNamespaces.isOk());
    assert(suffixedNamespaces->size() == 2);
    assert(suffixedNamespaces->at(0) == std::vector<std::string>({ "agents", "thread-a" }));
    assert(suffixedNamespaces->at(1) == std::vector<std::string>({ "logs", "thread-a" }));

    auto batch = store.batch(std::vector<lc::StoreOp> {
        lc::StoreGetOp {
            .namespace_ = ns,
            .key_ = "profile",
        },
        lc::StoreSearchOp {
            .namespacePrefix_ = { "agents" },
        },
        lc::StorePutOp {
            .namespace_ = { "agents", "thread-c" },
            .key_ = "profile",
            .value_ = nlohmann::json { { "name", "batched" } },
        },
        lc::StoreListNamespacesOp {
            .matchConditions_ = {
                lc::StoreNamespaceMatchCondition {
                    .matchType_ = lc::StoreNamespaceMatchType::Prefix,
                    .path_ = { "agents" },
                },
            },
        },
    });
    assert(batch.isOk());
    assert(batch->size() == 4);
    const auto* batchItem = std::get_if<lc::StoreItem>(&batch->at(0));
    assert(batchItem != nullptr);
    assert(batchItem->key_ == "profile");
    const auto* batchSearch = std::get_if<std::vector<lc::StoreSearchItem>>(&batch->at(1));
    assert(batchSearch != nullptr);
    assert(batchSearch->size() == 2);
    assert(std::holds_alternative<std::monostate>(batch->at(2)));
    const auto* batchNamespaces = std::get_if<std::vector<lc::StoreNamespace>>(&batch->at(3));
    assert(batchNamespaces != nullptr);
    assert(batchNamespaces->size() == 2);

    auto batchInserted = store.get({ "agents", "thread-c" }, "profile");
    assert(batchInserted.isOk());
    assert(batchInserted->has_value());
    assert((*batchInserted)->value_.at("name") == "batched");

    auto wildcardNamespaces = store.listNamespaces(lc::StoreListNamespacesOptions {
        .prefix_ = { "*", "thread-a" },
    });
    assert(wildcardNamespaces.isOk());
    assert(wildcardNamespaces->size() == 2);
    assert(wildcardNamespaces->at(0) == std::vector<std::string>({ "agents", "thread-a" }));
    assert(wildcardNamespaces->at(1) == std::vector<std::string>({ "logs", "thread-a" }));

    auto deleteBatch = store.batch(std::vector<lc::StoreOp> {
        lc::StorePutOp {
            .namespace_ = { "agents", "thread-b" },
            .key_ = "profile",
            .value_ = std::nullopt,
        },
        lc::StoreListNamespacesOp {
            .matchConditions_ = {
                lc::StoreNamespaceMatchCondition {
                    .matchType_ = lc::StoreNamespaceMatchType::Prefix,
                    .path_ = { "agents" },
                },
            },
        },
    });
    assert(deleteBatch.isOk());
    assert(deleteBatch->size() == 2);
    assert(std::holds_alternative<std::monostate>(deleteBatch->at(0)));
    const auto* namespacesBeforeDelete = std::get_if<std::vector<lc::StoreNamespace>>(&deleteBatch->at(1));
    assert(namespacesBeforeDelete != nullptr);
    assert(namespacesBeforeDelete->size() == 3);

    auto batchDeleted = store.get({ "agents", "thread-b" }, "profile");
    assert(batchDeleted.isOk());
    assert(!batchDeleted->has_value());
    auto agentsAfterBatchDelete = store.search(lc::StoreSearchOptions {
        .namespacePrefix_ = { "agents" },
    });
    assert(agentsAfterBatchDelete.isOk());
    assert(agentsAfterBatchDelete->size() == 2);

    auto erased = store.deleteItem(ns, "profile");
    assert(erased.isOk());
    auto deleted = store.get(ns, "profile");
    assert(deleted.isOk());
    assert(!deleted->has_value());
    erased = store.deleteItem(ns, "profile");
    assert(erased.isOk());

    auto invalid = store.put({ "" }, "profile", nlohmann::json::object());
    assert(!invalid.isOk());
    assert(invalid.status().code() == lc::StatusCode::InvalidArgument);

    auto emptyNamespace = store.put({}, "profile", nlohmann::json::object());
    assert(!emptyNamespace.isOk());
    assert(emptyNamespace.status().code() == lc::StatusCode::InvalidArgument);

    auto dottedNamespace = store.put({ "agent.v1" }, "profile", nlohmann::json::object());
    assert(!dottedNamespace.isOk());
    assert(dottedNamespace.status().code() == lc::StatusCode::InvalidArgument);

    auto reservedNamespace = store.put({ "langgraph", "system" }, "profile", nlohmann::json::object());
    assert(!reservedNamespace.isOk());
    assert(reservedNamespace.status().code() == lc::StatusCode::InvalidArgument);

    auto allItems = store.search();
    assert(allItems.isOk());
    assert(allItems->size() == 2);
}

void exerciseStorageStoreSchemaMaintenance()
{
    auto storage = std::make_shared<lc::MemoryStorage>();
    lc::StorageStore store(storage, lc::StorageStoreOptions {
                                       .listPageSize_ = 1,
                                   });

    const lc::StoreNamespace legacyNamespace { "legacy", "thread-a" };
    const auto legacyKey = storageStoreKey(legacyNamespace, "profile");
    const nlohmann::json legacyEnvelope {
        { "namespace", namespaceJson(legacyNamespace) },
        { "key", "profile" },
        { "value", { { "name", "legacy" } } },
    };
    assert(storage->put(legacyKey, legacyEnvelope.dump()).isOk());

    auto loaded = store.get(legacyNamespace, "profile");
    assert(loaded.isOk());
    assert(loaded->has_value());
    assert((*loaded)->value_.at("name") == "legacy");

    assert(store.put(legacyNamespace, "profile", { { "name", "rewritten" } }).isOk());

    auto rawLegacy = storage->get(legacyKey);
    assert(rawLegacy.isOk());
    assert(rawLegacy->has_value());
    auto rewrittenEnvelope = nlohmann::json::parse((*rawLegacy)->value_);
    assert(rewrittenEnvelope.at("schema_version") == 1);
    assert(rewrittenEnvelope.at("value").at("name") == "rewritten");

    const lc::StoreNamespace futureNamespace { "legacy", "future" };
    const nlohmann::json futureEnvelope {
        { "schema_version", 999 },
        { "namespace", namespaceJson(futureNamespace) },
        { "key", "profile" },
        { "value", { { "name", "future" } } },
    };
    assert(storage->put(storageStoreKey(futureNamespace, "profile"), futureEnvelope.dump()).isOk());
    auto futureLoaded = store.get(futureNamespace, "profile");
    assert(!futureLoaded.isOk());
    assert(futureLoaded.status().code() == lc::StatusCode::Unimplemented);
}

std::filesystem::path temporaryDatabasePath()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("langgraph_cpp_storage_" + std::to_string(now) + ".sqlite");
}

} // namespace

int main()
{
    lc::MemoryStorage memoryStorage;
    exerciseStorage(memoryStorage);

    lc::InMemoryStore memoryStore;
    exerciseStore(memoryStore);

    auto memoryStoreStorage = std::make_shared<lc::MemoryStorage>();
    lc::StorageStore memoryBackedStore(memoryStoreStorage);
    exerciseStore(memoryBackedStore);
    exerciseStorageStoreSchemaMaintenance();

    auto invalid = memoryStorage.put(lc::StorageKey { .scope_ = "", .key_ = "key" }, "value");
    assert(!invalid.isOk());
    assert(invalid.status().code() == lc::StatusCode::InvalidArgument);

    auto invalidClear = memoryStorage.clearScope("");
    assert(!invalidClear.isOk());
    assert(invalidClear.status().code() == lc::StatusCode::InvalidArgument);

    assert(lc::storageKeyFromCursor("999:short").status().code() == lc::StatusCode::InvalidArgument);

    assert(memoryStorage.flush().isOk());
    assert(memoryStorage.close().isOk());
    assert(memoryStorage.isClosed());
    auto afterMemoryClose = memoryStorage.get({ .scope_ = "thread-b", .key_ = "checkpoint-1" });
    assert(!afterMemoryClose.isOk());
    assert(afterMemoryClose.status().code() == lc::StatusCode::Unavailable);

    lc::ManualWallClock wall(lc::WallClock::TimePoint(std::chrono::milliseconds(42)));
    lc::MemoryStorage limitedStorage(
        lc::StorageLimits {
            .maxScopeBytes_ = 16,
            .maxKeyBytes_ = 16,
            .maxValueBytes_ = 4,
            .maxListItems_ = 2,
            .maxItems_ = 1,
        },
        wall);
    assert(limitedStorage.put({ .scope_ = "scope", .key_ = "key" }, "1234").isOk());
    auto limitedItem = limitedStorage.get({ .scope_ = "scope", .key_ = "key" });
    assert(limitedItem.isOk());
    assert(limitedItem->has_value());
    assert((*limitedItem)->createdAt_ == lc::WallClock::TimePoint(std::chrono::milliseconds(42)));
    assert(limitedStorage.put({ .scope_ = "scope", .key_ = "too-many" }, "1").status().code()
        == lc::StatusCode::ResourceExhausted);
    assert(limitedStorage.put({ .scope_ = "scope", .key_ = "key" }, "12345").status().code()
        == lc::StatusCode::ResourceExhausted);
    assert(limitedStorage.list(lc::StorageListOptions { .limit_ = 0 }).status().code()
        == lc::StatusCode::InvalidArgument);
    assert(limitedStorage.list(lc::StorageListOptions { .limit_ = 3 }).status().code()
        == lc::StatusCode::ResourceExhausted);

#if LANGGRAPH_CPP_WITH_SQLITE
    lc::SQLiteStorageOptions invalidOptions;
    invalidOptions.busyTimeout_ = std::chrono::milliseconds(-1);
    assert(invalidOptions.validate().code() == lc::StatusCode::InvalidArgument);

    const auto path = temporaryDatabasePath();
    const auto backupPath = temporaryDatabasePath();
    std::filesystem::remove(path);
    std::filesystem::remove(backupPath);

    {
        lc::SQLiteStorage sqliteStorage(path.string());
        assert(sqliteStorage.open().isOk());
        assert(sqliteStorage.isOpen());
        exerciseStorage(sqliteStorage);
        assert(sqliteStorage.put({ .scope_ = "thread-persisted", .key_ = "latest" }, "state").isOk());
        assert(!sqliteStorage.backupTo(path.string()).isOk());
        assert(sqliteStorage.backupTo(backupPath.string()).isOk());
        assert(sqliteStorage.flush().isOk());
        assert(sqliteStorage.close().isOk());
        assert(sqliteStorage.isClosed());
        auto afterClose = sqliteStorage.get({ .scope_ = "thread-persisted", .key_ = "latest" });
        assert(!afterClose.isOk());
        assert(afterClose.status().code() == lc::StatusCode::Unavailable);
    }

    {
        lc::SQLiteStorage reopened(path.string());
        auto persisted = reopened.get({ .scope_ = "thread-persisted", .key_ = "latest" });
        assert(persisted.isOk());
        assert(persisted->has_value());
        assert((*persisted)->value_ == "state");
        assert((*persisted)->version_ == 1);
    }

    {
        lc::SQLiteStorage backup(backupPath.string());
        auto persisted = backup.get({ .scope_ = "thread-persisted", .key_ = "latest" });
        assert(persisted.isOk());
        assert(persisted->has_value());
        assert((*persisted)->value_ == "state");
    }

    {
        const auto storePath = temporaryDatabasePath();
        std::filesystem::remove(storePath);
        {
            auto sqliteStorage = std::make_shared<lc::SQLiteStorage>(storePath.string());
            lc::StorageStore store(sqliteStorage);
            exerciseStore(store);
            assert(store.put({ "agents", "thread-a" }, "profile", { { "name", "persisted" } }).isOk());
            assert(sqliteStorage->close().isOk());
        }
        {
            auto reopenedStorage = std::make_shared<lc::SQLiteStorage>(storePath.string());
            lc::StorageStore store(reopenedStorage);
            auto item = store.get({ "agents", "thread-a" }, "profile");
            assert(item.isOk());
            assert(item->has_value());
            assert((*item)->value_.at("name") == "persisted");
            assert(reopenedStorage->close().isOk());
        }
        std::filesystem::remove(storePath);
    }

    {
        const auto pressurePath = temporaryDatabasePath();
        std::filesystem::remove(pressurePath);
        lc::SQLiteStorage pressure(pressurePath.string());
        constexpr int count = 256;
        for (int i = 0; i < count; ++i) {
            const auto key = "checkpoint-" + std::to_string(100000 + i);
            assert(pressure.put({ .scope_ = "thread-pressure", .key_ = key }, "state").isOk());
        }
        assert(pressure.put({ .scope_ = "thread-pressure", .key_ = "metadata" }, "ignored").isOk());

        std::string cursor;
        int seen = 0;
        do {
            auto page = pressure.list(lc::StorageListOptions {
                .scope_ = "thread-pressure",
                .keyPrefix_ = "checkpoint-",
                .limit_ = 17,
                .cursor_ = cursor,
            });
            assert(page.isOk());
            for (const auto& item : page->items_) {
                assert(item.key_.key_.starts_with("checkpoint-"));
                ++seen;
            }
            cursor = page->nextCursor_;
        } while (!cursor.empty());

        assert(seen == count);
        assert(pressure.close().isOk());
        std::filesystem::remove(pressurePath);
    }

    {
        const auto lockPath = temporaryDatabasePath();
        std::filesystem::remove(lockPath);
        const lc::SQLiteStorageOptions lockOptions {
            .useProcessLock_ = true,
        };

        lc::SQLiteStorage first(
            lockPath.string(),
            lc::Logger::defaultLogger(),
            {},
            lc::SystemWallClock::instance(),
            lockOptions);
        assert(first.open().isOk());

        lc::SQLiteStorage second(
            lockPath.string(),
            lc::Logger::defaultLogger(),
            {},
            lc::SystemWallClock::instance(),
            lockOptions);
        auto locked = second.open();
        assert(!locked.isOk());
        assert(locked.status().code() == lc::StatusCode::FailedPrecondition);

        assert(first.close().isOk());
        assert(second.open().isOk());
        assert(second.close().isOk());
        std::filesystem::remove(lockPath);
    }

    {
        lc::SQLiteStorage invalid(std::filesystem::temp_directory_path().string());
        assert(!invalid.open().isOk());
    }

    std::filesystem::remove(path);
    std::filesystem::remove(backupPath);
#endif

    return 0;
}
