#include "foundation/storage/sqlite_common.hh"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace lgc::sqlite_detail {

[[nodiscard]] std::optional<std::string> prefixUpperBound(std::string prefix)
{
    if (prefix.empty())
        return std::nullopt;

    for (auto it = prefix.rbegin(); it != prefix.rend(); ++it) {
        auto ch = static_cast<unsigned char>(*it);
        if (ch == 0xff)
            continue;
        *it = static_cast<char>(ch + 1);
        prefix.erase(it.base(), prefix.end());
        return prefix;
    }
    return std::nullopt;
}

[[nodiscard]] Status sqliteStatus(sqlite3* db, int code, std::string context)
{
    std::string message = std::move(context);
    if (db != nullptr) {
        message.append(": ");
        message.append(sqlite3_errmsg(db));
    }

    switch (code) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return Status::unavailable(std::move(message));
    case SQLITE_CONSTRAINT:
        return Status::failedPrecondition(std::move(message));
    case SQLITE_MISUSE:
        return Status::internal(std::move(message));
    default:
        return Status::unknown(std::move(message));
    }
}

[[nodiscard]] std::int64_t nowMs(const WallClock& clock)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        clock.now().time_since_epoch())
        .count();
}

[[nodiscard]] std::chrono::system_clock::time_point timePointFromMs(std::int64_t value)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(value));
}

[[nodiscard]] std::int64_t timePointToMs(std::chrono::system_clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

[[nodiscard]] Result<void> bindText(sqlite3_stmt* statement, int index, const std::string& value)
{
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return Status::resourceExhausted("storage text value is too large");

    const auto rc = sqlite3_bind_text(
        statement,
        index,
        value.data(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT);
    if (rc != SQLITE_OK)
        return Status::internal("failed to bind sqlite text value");

    return okResult();
}

[[nodiscard]] Result<void> bindBlob(sqlite3_stmt* statement, int index, const std::string& value)
{
    const auto rc = sqlite3_bind_blob64(
        statement,
        index,
        value.data(),
        static_cast<sqlite3_uint64>(value.size()),
        SQLITE_TRANSIENT);
    if (rc != SQLITE_OK)
        return Status::internal("failed to bind sqlite blob value");

    return okResult();
}

[[nodiscard]] Result<void> bindInt64(sqlite3_stmt* statement, int index, sqlite3_int64 value)
{
    const auto rc = sqlite3_bind_int64(statement, index, value);
    if (rc != SQLITE_OK)
        return Status::internal("failed to bind sqlite integer value");

    return okResult();
}

[[nodiscard]] Result<void> bindItemValues(
    sqlite3_stmt* statement,
    const StorageItem& item,
    int valueIndex,
    int createdAtIndex,
    int updatedAtIndex,
    int versionIndex)
{
    if (auto result = bindBlob(statement, valueIndex, item.value_); !result.isOk())
        return result.status();

    auto rc = sqlite3_bind_int64(statement, createdAtIndex, timePointToMs(item.createdAt_));
    if (rc != SQLITE_OK)
        return Status::internal("failed to bind sqlite created timestamp");

    rc = sqlite3_bind_int64(statement, updatedAtIndex, timePointToMs(item.updatedAt_));
    if (rc != SQLITE_OK)
        return Status::internal("failed to bind sqlite updated timestamp");

    if (item.version_ > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
        return Status::resourceExhausted("storage item version exhausted");

    rc = sqlite3_bind_int64(statement, versionIndex, static_cast<sqlite3_int64>(item.version_));
    if (rc != SQLITE_OK)
        return Status::internal("failed to bind sqlite item version");

    return okResult();
}

[[nodiscard]] StorageItem itemFromStatement(sqlite3_stmt* statement)
{
    const auto* scope = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
    const auto* key = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
    const auto* value = static_cast<const char*>(sqlite3_column_blob(statement, 2));
    const auto valueSize = sqlite3_column_bytes(statement, 2);
    const auto createdAtMs = sqlite3_column_int64(statement, 3);
    const auto updatedAtMs = sqlite3_column_int64(statement, 4);
    const auto version = sqlite3_column_int64(statement, 5);

    return StorageItem {
        .key_ = StorageKey {
            .scope_ = scope == nullptr ? std::string() : std::string(scope),
            .key_ = key == nullptr ? std::string() : std::string(key),
        },
        .value_ = value == nullptr ? std::string() : std::string(value, static_cast<std::size_t>(valueSize)),
        .createdAt_ = timePointFromMs(createdAtMs),
        .updatedAt_ = timePointFromMs(updatedAtMs),
        .version_ = static_cast<std::uint64_t>(version),
    };
}

} // namespace lgc::sqlite_detail
