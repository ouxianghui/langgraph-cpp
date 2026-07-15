#include "foundation/storage/sqlite_storage.hpp"

#include "foundation/storage/sqlite_common.hh"

#include <sqlite3.h>

namespace lgc {
namespace {
constexpr int kSchemaVersion = 1;

constexpr const char* kSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS lc_storage (
    scope TEXT NOT NULL,
    key TEXT NOT NULL,
    value BLOB NOT NULL,
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL,
    version INTEGER NOT NULL,
    PRIMARY KEY (scope, key)
);
)sql";

using sqlite_detail::sqliteStatus;
}

Result<int> SQLiteStorage::readSchemaVersionLocked()
{
    constexpr const char* kSql = "PRAGMA user_version;";

    sqlite3_stmt* statement = nullptr;
    auto rc = sqlite3_prepare_v2(db_, kSql, -1, &statement, nullptr);
    if (rc != SQLITE_OK)
        return sqliteStatus(db_, rc, "failed to prepare sqlite schema version");

    rc = sqlite3_step(statement);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return sqliteStatus(db_, rc, "failed to read sqlite schema version");
    }

    const auto version = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    if (version < 0)
        return Status::dataLoss("sqlite schema version is negative");
    return version;
}

Result<void> SQLiteStorage::migrateSchemaLocked()
{
    auto current = readSchemaVersionLocked();
    if (!current.isOk())
        return current.status();
    if (*current > kSchemaVersion)
        return Status::failedPrecondition("sqlite storage schema version is newer than this runtime");
    if (*current == kSchemaVersion) {
        return executeSqlLocked(kSchemaSql);
    }

    if (auto result = executeSqlLocked("BEGIN IMMEDIATE;"); !result.isOk())
        return result.status();
    if (auto result = executeSqlLocked(kSchemaSql); !result.isOk()) {
        rollbackLocked();
        return result.status();
    }
    if (auto result = executeSqlLocked("PRAGMA user_version = 1;"); !result.isOk()) {
        rollbackLocked();
        return result.status();
    }
    if (auto result = executeSqlLocked("COMMIT;"); !result.isOk()) {
        rollbackLocked();
        return result.status();
    }
    return okResult();
}

} // namespace lgc
