#pragma once

#include "foundation/logging/logger.hpp"
#include "foundation/storage/i_storage.hpp"
#include "foundation/time/clock.hpp"

#include <memory>
#include <mutex>
#include <string>

struct sqlite3;

namespace lgc {

enum class SQLiteJournalMode : std::uint8_t {
    Wal,
    Delete,
};

enum class SQLiteSynchronousMode : std::uint8_t {
    Normal,
    Full,
    Extra,
};

struct SQLiteStorageOptions {
    std::chrono::milliseconds busyTimeout_ { 5'000 };
    SQLiteJournalMode journalMode_ { SQLiteJournalMode::Wal };
    SQLiteSynchronousMode synchronousMode_ { SQLiteSynchronousMode::Full };
    bool foreignKeys_ { true };
    bool useProcessLock_ { false };

    [[nodiscard]] Status validate() const;
};

class SQLiteStorage final : public IStorage {
public:
    explicit SQLiteStorage(
        std::string databasePath,
        std::shared_ptr<ILogger> logger = Logger::defaultLogger(),
        StorageLimits limits = {},
        const WallClock& clock = SystemWallClock::instance(),
        SQLiteStorageOptions options = {});
    ~SQLiteStorage() override;

    SQLiteStorage(const SQLiteStorage&) = delete;
    SQLiteStorage& operator=(const SQLiteStorage&) = delete;

    [[nodiscard]] Result<void> open();
    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] Result<void> backupTo(const std::string& destinationPath);

    [[nodiscard]] Result<void> put(
        StorageKey key,
        std::string value,
        const StoragePutOptions& options = {}) override;
    [[nodiscard]] Result<std::optional<StorageItem>> get(const StorageKey& key) override;
    [[nodiscard]] Result<StorageListResult> list(const StorageListOptions& options = {}) override;
    [[nodiscard]] Result<void> remove(const StorageKey& key) override;
    [[nodiscard]] Result<void> clearScope(std::string_view scope) override;
    [[nodiscard]] Status flush() override;
    [[nodiscard]] Status close() override;
    [[nodiscard]] bool isClosed() const noexcept override;

private:
    ::sqlite3* db_ { nullptr };

    [[nodiscard]] Result<void> ensureOpenLocked();
    [[nodiscard]] Result<void> acquireProcessLockLocked();
    void releaseProcessLockLocked() noexcept;
    [[nodiscard]] Result<void> configureDatabaseLocked();
    [[nodiscard]] Result<void> migrateSchemaLocked();
    [[nodiscard]] Result<int> readSchemaVersionLocked();
    [[nodiscard]] Status closeLocked() noexcept;
    [[nodiscard]] Result<void> executeSqlLocked(const char* sql);
    [[nodiscard]] Result<std::optional<StorageItem>> getLocked(const StorageKey& key);
    [[nodiscard]] Result<std::uint64_t> countItemsLocked();
    void rollbackLocked();

    std::string databasePath_;
    std::shared_ptr<ILogger> logger_;
    StorageLimits limits_;
    const WallClock* clock_ { nullptr };
    SQLiteStorageOptions options_;
    mutable std::mutex mutex_;
    std::string processLockKey_;
    bool processLockHeld_ { false };
    bool closed_ { false };
};

} // namespace lgc
