#include "foundation/storage/sqlite_storage.hpp"

#include "foundation/storage/sqlite_common.hh"

#include <chrono>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

#include <sqlite3.h>

namespace lgc {
namespace {
using sqlite_detail::sqliteStatus;
}

std::mutex& processLockMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_set<std::string>& processLocks()
{
    static std::unordered_set<std::string> locks;
    return locks;
}

[[nodiscard]] std::string processLockKeyForPath(const std::string& path)
{
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    if (ec)
        return path;
    return absolute.lexically_normal().string();
}


Status SQLiteStorageOptions::validate() const
{
    if (busyTimeout_ < std::chrono::milliseconds::zero())
        return Status::invalidArgument("sqlite busy timeout cannot be negative");
    if (busyTimeout_ > std::chrono::hours(24))
        return Status::outOfRange("sqlite busy timeout is too large");
    return Status::ok();
}

SQLiteStorage::SQLiteStorage(
    std::string databasePath,
    std::shared_ptr<ILogger> logger,
    StorageLimits limits,
    const WallClock& clock,
    SQLiteStorageOptions options)
    : databasePath_(std::move(databasePath))
    , logger_(std::move(logger))
    , limits_(limits)
    , clock_(&clock)
    , options_(options)
{
}

SQLiteStorage::~SQLiteStorage()
{
    std::lock_guard lock(mutex_);
    (void)closeLocked();
}

Result<void> SQLiteStorage::open()
{
    std::lock_guard lock(mutex_);
    return ensureOpenLocked();
}

bool SQLiteStorage::isOpen() const noexcept
{
    std::lock_guard lock(mutex_);
    return db_ != nullptr;
}

Result<void> SQLiteStorage::backupTo(const std::string& destinationPath)
{
    if (destinationPath.empty())
        return Status::invalidArgument("sqlite backup destination path cannot be empty");
    if (processLockKeyForPath(destinationPath) == processLockKeyForPath(databasePath_))
        return Status::invalidArgument("sqlite backup destination must differ from source path");

    std::lock_guard lock(mutex_);
    if (auto result = ensureOpenLocked(); !result.isOk())
        return result.status();
    if (auto checkpoint = executeSqlLocked("PRAGMA wal_checkpoint(FULL);"); !checkpoint.isOk())
        return checkpoint.status();

    sqlite3* destination = nullptr;
    auto rc = sqlite3_open_v2(
        destinationPath.c_str(),
        &destination,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK) {
        auto status = sqliteStatus(destination, rc, "failed to open sqlite backup destination");
        if (destination != nullptr)
            sqlite3_close(destination);
        return status;
    }

    if (sqlite3_busy_timeout(destination, static_cast<int>(options_.busyTimeout_.count())) != SQLITE_OK) {
        auto status = sqliteStatus(destination, SQLITE_ERROR, "failed to configure sqlite backup destination");
        sqlite3_close(destination);
        return status;
    }

    sqlite3_backup* backup = sqlite3_backup_init(destination, "main", db_, "main");
    if (backup == nullptr) {
        auto status = sqliteStatus(destination, sqlite3_errcode(destination), "failed to initialize sqlite backup");
        sqlite3_close(destination);
        return status;
    }

    const auto started = std::chrono::steady_clock::now();
    Status status = Status::ok();
    while (true) {
        rc = sqlite3_backup_step(backup, -1);
        if (rc == SQLITE_DONE)
            break;
        if (rc == SQLITE_OK)
            continue;
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            if (std::chrono::steady_clock::now() - started >= options_.busyTimeout_) {
                status = Status::deadlineExceeded("sqlite backup timed out waiting for lock");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        status = sqliteStatus(destination, rc, "failed to copy sqlite backup");
        break;
    }

    const auto finishRc = sqlite3_backup_finish(backup);
    if (status.isOk() && finishRc != SQLITE_OK)
        status = sqliteStatus(destination, finishRc, "failed to finish sqlite backup");

    const auto closeRc = sqlite3_close(destination);
    if (status.isOk() && closeRc != SQLITE_OK)
        status = sqliteStatus(destination, closeRc, "failed to close sqlite backup destination");

    if (!status.isOk())
        return status;
    return okResult();
}

Result<void> SQLiteStorage::ensureOpenLocked()
{
    if (closed_)
        return Status::unavailable("sqlite storage is closed");

    if (db_ != nullptr)
        return okResult();

    if (databasePath_.empty())
        return Status::invalidArgument("sqlite database path cannot be empty");
    if (auto status = options_.validate(); !status.isOk())
        return status;
    if (auto result = acquireProcessLockLocked(); !result.isOk())
        return result.status();

    sqlite3* db = nullptr;
    const auto rc = sqlite3_open_v2(
        databasePath_.c_str(),
        &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);

    if (rc != SQLITE_OK) {
        auto status = sqliteStatus(db, rc, "failed to open sqlite storage");
        logTo(logger_,
            LogLevel::Warn,
            "SQLiteStorage",
            "open failed path={} status={}",
            __FILE__,
            __LINE__,
            databasePath_,
            status.toString());
        if (db != nullptr)
            sqlite3_close(db);
        releaseProcessLockLocked();
        return status;
    }

    db_ = db;
    const auto failOpen = [&](Status status) {
        logTo(logger_,
            LogLevel::Warn,
            "SQLiteStorage",
            "open failed path={} status={}",
            __FILE__,
            __LINE__,
            databasePath_,
            status.toString());
        sqlite3_close(db_);
        db_ = nullptr;
        releaseProcessLockLocked();
        return status;
    };

    if (auto result = configureDatabaseLocked(); !result.isOk()) {
        return failOpen(result.status());
    }
    if (auto result = migrateSchemaLocked(); !result.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "SQLiteStorage",
            "schema initialization failed path={} status={}",
            __FILE__,
            __LINE__,
            databasePath_,
            result.status().toString());
        return failOpen(result.status());
    }

    logTo(logger_,
        LogLevel::Info,
        "SQLiteStorage",
        "opened path={}",
        __FILE__,
        __LINE__,
        databasePath_);
    return okResult();
}

Result<void> SQLiteStorage::acquireProcessLockLocked()
{
    if (!options_.useProcessLock_ || processLockHeld_)
        return okResult();

    auto key = processLockKeyForPath(databasePath_);
    std::lock_guard lock(processLockMutex());
    auto& locks = processLocks();
    if (locks.find(key) != locks.end())
        return Status::failedPrecondition("sqlite storage is already open in this process");

    locks.insert(key);
    processLockKey_ = std::move(key);
    processLockHeld_ = true;
    return okResult();
}

void SQLiteStorage::releaseProcessLockLocked() noexcept
{
    if (!processLockHeld_)
        return;

    std::lock_guard lock(processLockMutex());
    processLocks().erase(processLockKey_);
    processLockKey_.clear();
    processLockHeld_ = false;
}

Result<void> SQLiteStorage::configureDatabaseLocked()
{
    const auto busyTimeoutMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        options_.busyTimeout_)
                                  .count();
    if (busyTimeoutMs > std::numeric_limits<int>::max())
        return Status::outOfRange("sqlite busy timeout is too large");
    if (sqlite3_busy_timeout(db_, static_cast<int>(busyTimeoutMs)) != SQLITE_OK)
        return sqliteStatus(db_, SQLITE_ERROR, "failed to configure sqlite busy timeout");

    if (options_.foreignKeys_) {
        if (auto result = executeSqlLocked("PRAGMA foreign_keys = ON;"); !result.isOk())
            return result.status();
    } else {
        if (auto result = executeSqlLocked("PRAGMA foreign_keys = OFF;"); !result.isOk())
            return result.status();
    }

    switch (options_.journalMode_) {
    case SQLiteJournalMode::Wal:
        if (auto result = executeSqlLocked("PRAGMA journal_mode = WAL;"); !result.isOk())
            return result.status();
        break;
    case SQLiteJournalMode::Delete:
        if (auto result = executeSqlLocked("PRAGMA journal_mode = DELETE;"); !result.isOk())
            return result.status();
        break;
    }

    switch (options_.synchronousMode_) {
    case SQLiteSynchronousMode::Normal:
        return executeSqlLocked("PRAGMA synchronous = NORMAL;");
    case SQLiteSynchronousMode::Full:
        return executeSqlLocked("PRAGMA synchronous = FULL;");
    case SQLiteSynchronousMode::Extra:
        return executeSqlLocked("PRAGMA synchronous = EXTRA;");
    }
    return Status::internal("unknown sqlite synchronous mode");
}

Status SQLiteStorage::closeLocked() noexcept
{
    if (db_ == nullptr) {
        closed_ = true;
        releaseProcessLockLocked();
        return Status::ok();
    }

    const auto rc = sqlite3_close(db_);
    if (rc != SQLITE_OK) {
        auto status = sqliteStatus(db_, rc, "failed to close sqlite storage");
        logTo(logger_,
            LogLevel::Warn,
            "SQLiteStorage",
            "close failed path={} status={}",
            __FILE__,
            __LINE__,
            databasePath_,
            status.toString());
        return status;
    }

    db_ = nullptr;
    closed_ = true;
    releaseProcessLockLocked();
    return Status::ok();
}

Result<void> SQLiteStorage::executeSqlLocked(const char* sql)
{
    char* error = nullptr;
    const auto rc = sqlite3_exec(db_, sql, nullptr, nullptr, &error);
    if (rc == SQLITE_OK)
        return okResult();

    std::string message = "failed to execute sqlite statement";
    if (error != nullptr) {
        message.append(": ");
        message.append(error);
        sqlite3_free(error);
    }

    return sqliteStatus(db_, rc, std::move(message));
}

void SQLiteStorage::rollbackLocked()
{
    auto ignored = executeSqlLocked("ROLLBACK;");
    (void)ignored;
}

Status SQLiteStorage::flush()
{
    std::lock_guard lock(mutex_);
    if (auto result = ensureOpenLocked(); !result.isOk())
        return result.status();

    return executeSqlLocked("PRAGMA wal_checkpoint(FULL);").status();
}

Status SQLiteStorage::close()
{
    std::lock_guard lock(mutex_);
    return closeLocked();
}

bool SQLiteStorage::isClosed() const noexcept
{
    std::lock_guard lock(mutex_);
    return closed_;
}

} // namespace lgc
