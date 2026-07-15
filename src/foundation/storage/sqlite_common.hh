#pragma once

#include "foundation/storage/sqlite_storage.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include <sqlite3.h>

namespace lgc::sqlite_detail {

[[nodiscard]] std::optional<std::string> prefixUpperBound(std::string prefix);
[[nodiscard]] Status sqliteStatus(sqlite3* db, int code, std::string context);
[[nodiscard]] std::int64_t nowMs(const WallClock& clock);
[[nodiscard]] std::chrono::system_clock::time_point timePointFromMs(std::int64_t value);
[[nodiscard]] std::int64_t timePointToMs(std::chrono::system_clock::time_point value);
[[nodiscard]] Result<void> bindText(sqlite3_stmt* statement, int index, const std::string& value);
[[nodiscard]] Result<void> bindBlob(sqlite3_stmt* statement, int index, const std::string& value);
[[nodiscard]] Result<void> bindInt64(sqlite3_stmt* statement, int index, sqlite3_int64 value);
[[nodiscard]] Result<void> bindItemValues(
    sqlite3_stmt* statement,
    const StorageItem& item,
    int valueIndex,
    int createdAtIndex,
    int updatedAtIndex,
    int versionIndex);
[[nodiscard]] StorageItem itemFromStatement(sqlite3_stmt* statement);

} // namespace lgc::sqlite_detail
