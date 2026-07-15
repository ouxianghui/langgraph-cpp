#include "foundation/storage/sqlite_storage.hpp"

#include "foundation/storage/sqlite_common.hh"

#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include <sqlite3.h>

namespace lgc {
namespace {
using sqlite_detail::bindInt64;
using sqlite_detail::bindItemValues;
using sqlite_detail::bindText;
using sqlite_detail::itemFromStatement;
using sqlite_detail::nowMs;
using sqlite_detail::prefixUpperBound;
using sqlite_detail::sqliteStatus;
}

Result<std::optional<StorageItem>> SQLiteStorage::getLocked(const StorageKey& key)
{
    constexpr const char* kSql = R"sql(
SELECT scope, key, value, created_at_ms, updated_at_ms, version
FROM lc_storage
WHERE scope = ?1 AND key = ?2;
)sql";

    sqlite3_stmt* statement = nullptr;
    auto rc = sqlite3_prepare_v2(db_, kSql, -1, &statement, nullptr);
    if (rc != SQLITE_OK)
        return sqliteStatus(db_, rc, "failed to prepare sqlite storage get");

    auto finalize = [&] { sqlite3_finalize(statement); };

    if (auto result = bindText(statement, 1, key.scope_); !result.isOk()) {
        finalize();
        return result.status();
    }
    if (auto result = bindText(statement, 2, key.key_); !result.isOk()) {
        finalize();
        return result.status();
    }

    rc = sqlite3_step(statement);
    if (rc == SQLITE_ROW) {
        auto item = itemFromStatement(statement);
        finalize();
        return std::optional<StorageItem>(std::move(item));
    }

    finalize();
    if (rc == SQLITE_DONE)
        return std::optional<StorageItem> {};

    return sqliteStatus(db_, rc, "failed to read sqlite storage item");
}

Result<std::uint64_t> SQLiteStorage::countItemsLocked()
{
    constexpr const char* kSql = "SELECT COUNT(*) FROM lc_storage;";

    sqlite3_stmt* statement = nullptr;
    auto rc = sqlite3_prepare_v2(db_, kSql, -1, &statement, nullptr);
    if (rc != SQLITE_OK)
        return sqliteStatus(db_, rc, "failed to prepare sqlite storage count");

    rc = sqlite3_step(statement);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return sqliteStatus(db_, rc, "failed to count sqlite storage items");
    }

    const auto count = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    if (count < 0)
        return Status::dataLoss("sqlite storage count is negative");
    return static_cast<std::uint64_t>(count);
}

Result<void> SQLiteStorage::put(
    StorageKey key,
    std::string value,
    const StoragePutOptions& options)
{
    const auto logFailure = [&](const Status& status) {
        logTo(logger_,
            LogLevel::Warn,
            "SQLiteStorage",
            "put failed scope={} key={} status={}",
            __FILE__,
            __LINE__,
            key.scope_,
            key.key_,
            status.toString());
    };

    if (auto status = validateStorageKey(key, limits_); !status.isOk())
        return status;
    if (auto status = validateStoragePutOptions(options); !status.isOk())
        return status;
    if (auto status = validateStorageValue(value, limits_); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    if (auto result = ensureOpenLocked(); !result.isOk()) {
        logFailure(result.status());
        return result.status();
    }

    if (auto result = executeSqlLocked("BEGIN IMMEDIATE;"); !result.isOk()) {
        logFailure(result.status());
        return result.status();
    }

    auto existing = getLocked(key);
    if (!existing.isOk()) {
        rollbackLocked();
        logFailure(existing.status());
        return existing.status();
    }

    const auto exists = existing->has_value();
    if (options.mode_ == StoragePutMode::InsertOnly && exists) {
        rollbackLocked();
        auto status = Status::alreadyExists("storage item already exists");
        logFailure(status);
        return status;
    }
    if (options.mode_ == StoragePutMode::ReplaceOnly && !exists) {
        rollbackLocked();
        auto status = Status::notFound("storage item does not exist");
        logFailure(status);
        return status;
    }
    if (options.expectedVersion_.has_value()) {
        if (!exists) {
            rollbackLocked();
            auto status = Status::notFound("storage item does not exist");
            logFailure(status);
            return status;
        }
        if ((*existing)->version_ != *options.expectedVersion_) {
            rollbackLocked();
            auto status = Status::failedPrecondition("storage version mismatch");
            logFailure(status);
            return status;
        }
    }
    if (!exists && limits_.maxItems_ != 0U) {
        auto count = countItemsLocked();
        if (!count.isOk()) {
            rollbackLocked();
            logFailure(count.status());
            return count.status();
        }
        if (*count >= limits_.maxItems_) {
            rollbackLocked();
            auto status = Status::resourceExhausted("sqlite storage item limit exceeded");
            logFailure(status);
            return status;
        }
    }

    const auto now = std::chrono::system_clock::time_point(std::chrono::milliseconds(nowMs(*clock_)));
    const auto version = exists ? (*existing)->version_ + 1 : 1;
    if (exists && version == 0) {
        rollbackLocked();
        auto status = Status::resourceExhausted("storage item version exhausted");
        logFailure(status);
        return status;
    }

    StorageItem item {
        .key_ = std::move(key),
        .value_ = std::move(value),
        .createdAt_ = exists ? (*existing)->createdAt_ : now,
        .updatedAt_ = now,
        .version_ = version,
    };

    const char* sql = exists
        ? "UPDATE lc_storage SET value = ?3, created_at_ms = ?4, updated_at_ms = ?5, version = ?6 WHERE scope = ?1 AND key = ?2;"
        : "INSERT INTO lc_storage(scope, key, value, created_at_ms, updated_at_ms, version) VALUES(?1, ?2, ?3, ?4, ?5, ?6);";

    sqlite3_stmt* statement = nullptr;
    auto rc = sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr);
    if (rc != SQLITE_OK) {
        rollbackLocked();
        auto status = sqliteStatus(db_, rc, "failed to prepare sqlite storage put");
        logFailure(status);
        return status;
    }

    auto finalize = [&] { sqlite3_finalize(statement); };

    if (auto result = bindText(statement, 1, item.key_.scope_); !result.isOk()) {
        finalize();
        rollbackLocked();
        logFailure(result.status());
        return result.status();
    }
    if (auto result = bindText(statement, 2, item.key_.key_); !result.isOk()) {
        finalize();
        rollbackLocked();
        logFailure(result.status());
        return result.status();
    }
    if (auto result = bindItemValues(statement, item, 3, 4, 5, 6); !result.isOk()) {
        finalize();
        rollbackLocked();
        logFailure(result.status());
        return result.status();
    }

    rc = sqlite3_step(statement);
    finalize();
    if (rc != SQLITE_DONE) {
        rollbackLocked();
        auto status = sqliteStatus(db_, rc, "failed to write sqlite storage item");
        logFailure(status);
        return status;
    }

    if (auto result = executeSqlLocked("COMMIT;"); !result.isOk()) {
        rollbackLocked();
        logFailure(result.status());
        return result.status();
    }

    return okResult();
}

Result<std::optional<StorageItem>> SQLiteStorage::get(const StorageKey& key)
{
    if (auto status = validateStorageKey(key, limits_); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    if (auto result = ensureOpenLocked(); !result.isOk())
        return result.status();

    return getLocked(key);
}

Result<StorageListResult> SQLiteStorage::list(const StorageListOptions& options)
{
    auto cursorKey = storageKeyFromCursor(options.cursor_, limits_);
    if (!cursorKey.isOk())
        return cursorKey.status();
    if (auto status = validateStorageListOptions(options, limits_); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    if (auto result = ensureOpenLocked(); !result.isOk())
        return result.status();

    constexpr const char* kSql = R"sql(
SELECT scope, key, value, created_at_ms, updated_at_ms, version
FROM lc_storage
WHERE (?1 = '' OR scope = ?1)
  AND (?2 = '' OR key >= ?2)
  AND (?3 = '' OR key < ?3)
  AND (?4 = '' OR scope > ?4 OR (scope = ?4 AND key > ?5))
ORDER BY scope ASC, key ASC
LIMIT ?6;
)sql";

    sqlite3_stmt* statement = nullptr;
    auto rc = sqlite3_prepare_v2(db_, kSql, -1, &statement, nullptr);
    if (rc != SQLITE_OK)
        return sqliteStatus(db_, rc, "failed to prepare sqlite storage list");

    auto finalize = [&] { sqlite3_finalize(statement); };

    if (auto result = bindText(statement, 1, options.scope_); !result.isOk()) {
        finalize();
        return result.status();
    }

    const auto cursor = cursorKey->has_value()
        ? std::optional<StorageKey>(**cursorKey)
        : std::optional<StorageKey> {};
    const auto prefixEnd = prefixUpperBound(options.keyPrefix_);
    const std::string cursorScope = cursor.has_value() ? cursor->scope_ : std::string();
    const std::string cursorKeyValue = cursor.has_value() ? cursor->key_ : std::string();
    const auto sqlLimit = static_cast<sqlite3_int64>(options.limit_ + 1);

    if (auto result = bindText(statement, 2, options.keyPrefix_); !result.isOk()) {
        finalize();
        return result.status();
    }
    if (auto result = bindText(statement, 3, prefixEnd.value_or(std::string())); !result.isOk()) {
        finalize();
        return result.status();
    }
    if (auto result = bindText(statement, 4, cursorScope); !result.isOk()) {
        finalize();
        return result.status();
    }
    if (auto result = bindText(statement, 5, cursorKeyValue); !result.isOk()) {
        finalize();
        return result.status();
    }
    if (auto result = bindInt64(statement, 6, sqlLimit); !result.isOk()) {
        finalize();
        return result.status();
    }

    StorageListResult result;
    std::optional<StorageKey> lastReturnedKey;

    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        auto item = itemFromStatement(statement);

        if (options.limit_ > 0 && result.items_.size() >= options.limit_) {
            if (lastReturnedKey.has_value())
                result.nextCursor_ = storageCursorFromKey(*lastReturnedKey);
            break;
        }

        lastReturnedKey = item.key_;
        result.items_.push_back(std::move(item));
    }

    finalize();
    if (rc != SQLITE_DONE && !(options.limit_ > 0 && !result.nextCursor_.empty()))
        return sqliteStatus(db_, rc, "failed to list sqlite storage items");

    return result;
}

Result<void> SQLiteStorage::remove(const StorageKey& key)
{
    if (auto status = validateStorageKey(key, limits_); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    if (auto result = ensureOpenLocked(); !result.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "SQLiteStorage",
            "remove failed scope={} key={} status={}",
            __FILE__,
            __LINE__,
            key.scope_,
            key.key_,
            result.status().toString());
        return result.status();
    }

    constexpr const char* kSql = "DELETE FROM lc_storage WHERE scope = ?1 AND key = ?2;";

    sqlite3_stmt* statement = nullptr;
    auto rc = sqlite3_prepare_v2(db_, kSql, -1, &statement, nullptr);
    if (rc != SQLITE_OK) {
        auto status = sqliteStatus(db_, rc, "failed to prepare sqlite storage remove");
        logTo(logger_,
            LogLevel::Warn,
            "SQLiteStorage",
            "remove failed scope={} key={} status={}",
            __FILE__,
            __LINE__,
            key.scope_,
            key.key_,
            status.toString());
        return status;
    }

    auto finalize = [&] { sqlite3_finalize(statement); };

    if (auto result = bindText(statement, 1, key.scope_); !result.isOk()) {
        finalize();
        return result.status();
    }
    if (auto result = bindText(statement, 2, key.key_); !result.isOk()) {
        finalize();
        return result.status();
    }

    rc = sqlite3_step(statement);
    finalize();
    if (rc != SQLITE_DONE) {
        auto status = sqliteStatus(db_, rc, "failed to remove sqlite storage item");
        logTo(logger_,
            LogLevel::Warn,
            "SQLiteStorage",
            "remove failed scope={} key={} status={}",
            __FILE__,
            __LINE__,
            key.scope_,
            key.key_,
            status.toString());
        return status;
    }

    return okResult();
}

Result<void> SQLiteStorage::clearScope(std::string_view scope)
{
    if (auto status = validateStorageName(scope, "storage scope", limits_.maxScopeBytes_); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    if (auto result = ensureOpenLocked(); !result.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "SQLiteStorage",
            "clearScope failed scope={} status={}",
            __FILE__,
            __LINE__,
            scope,
            result.status().toString());
        return result.status();
    }

    constexpr const char* kSql = "DELETE FROM lc_storage WHERE scope = ?1;";

    sqlite3_stmt* statement = nullptr;
    auto rc = sqlite3_prepare_v2(db_, kSql, -1, &statement, nullptr);
    if (rc != SQLITE_OK) {
        auto status = sqliteStatus(db_, rc, "failed to prepare sqlite storage clear");
        logTo(logger_,
            LogLevel::Warn,
            "SQLiteStorage",
            "clearScope failed scope={} status={}",
            __FILE__,
            __LINE__,
            scope,
            status.toString());
        return status;
    }

    auto finalize = [&] { sqlite3_finalize(statement); };

    const std::string scopeValue(scope);
    if (auto result = bindText(statement, 1, scopeValue); !result.isOk()) {
        finalize();
        return result.status();
    }

    rc = sqlite3_step(statement);
    finalize();
    if (rc != SQLITE_DONE) {
        auto status = sqliteStatus(db_, rc, "failed to clear sqlite storage scope");
        logTo(logger_,
            LogLevel::Warn,
            "SQLiteStorage",
            "clearScope failed scope={} status={}",
            __FILE__,
            __LINE__,
            scope,
            status.toString());
        return status;
    }

    return okResult();
}

} // namespace lgc
