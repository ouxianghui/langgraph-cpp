#include "foundation/versioning/versioning.hpp"

#include <algorithm>
#include <charconv>
#include <exception>
#include <limits>
#include <string>
#include <utility>

namespace lc {
namespace {

[[nodiscard]] StorageKey toStorageKey(const StorageVersionKey& key)
{
    return StorageKey {
        .scope_ = key.scope_,
        .key_ = key.key_,
    };
}

[[nodiscard]] Status validateVersionKey(const StorageVersionKey& key)
{
    return validateStorageKey(toStorageKey(key));
}

[[nodiscard]] std::string migrationLockValue(Version fromVersion, Version toVersion, std::string_view name)
{
    std::string value;
    value.reserve(32 + name.size());
    value.append(std::to_string(fromVersion));
    value.push_back(':');
    value.append(std::to_string(toVersion));
    value.push_back(':');
    value.append(name);
    return value;
}

class StorageMigrationLock final {
public:
    StorageMigrationLock(IStorage& storage, StorageVersionKey key)
        : storage_(storage)
        , key_(std::move(key))
    {
    }

    StorageMigrationLock(const StorageMigrationLock&) = delete;
    StorageMigrationLock& operator=(const StorageMigrationLock&) = delete;

    ~StorageMigrationLock()
    {
        if (held_)
            (void)storage_.remove(toStorageKey(key_));
    }

    [[nodiscard]] Status acquire(std::string value)
    {
        if (auto status = validateVersionKey(key_); !status.isOk())
            return status;

        auto result = storage_.put(
            toStorageKey(key_),
            std::move(value),
            StoragePutOptions {
                .mode_ = StoragePutMode::InsertOnly,
            });
        if (result.isOk()) {
            held_ = true;
            return Status::ok();
        }
        if (result.status().code() == StatusCode::AlreadyExists)
            return Status::unavailable("storage migration lock is already held");
        return result.status();
    }

    [[nodiscard]] Status release()
    {
        if (!held_)
            return Status::ok();
        auto result = storage_.remove(toStorageKey(key_));
        if (result.isOk())
            held_ = false;
        return result.status();
    }

    void keepHeld() noexcept { held_ = false; }

private:
    IStorage& storage_;
    StorageVersionKey key_;
    bool held_ { false };
};

[[nodiscard]] Status storageLooksEmpty(
    IStorage& storage,
    const StorageVersionKey& versionKey,
    const StorageVersionKey& lockKey)
{
    const auto versionStorageKey = toStorageKey(versionKey);
    const auto lockStorageKey = toStorageKey(lockKey);
    std::string cursor;

    for (;;) {
        auto listed = storage.list(StorageListOptions {
            .limit_ = 1000,
            .cursor_ = cursor,
        });
        if (!listed.isOk())
            return listed.status();

        for (const auto& item : listed->items_) {
            if (item.key_ == versionStorageKey || item.key_ == lockStorageKey)
                continue;
            return Status::failedPrecondition(
                "storage schema version is missing for non-empty storage; use legacy migration mode");
        }
        if (listed->nextCursor_.empty())
            return Status::ok();
        cursor = listed->nextCursor_;
    }
}

[[nodiscard]] Status runMigrationCallback(
    const StorageMigrationFn& migrate,
    StorageMigrationContext& context,
    std::string_view name)
{
    try {
        return migrate(context);
    } catch (const std::exception& error) {
        std::string message("storage migration threw name=");
        message.append(name);
        message.append(": ");
        message.append(error.what());
        return Status::internal(std::move(message));
    } catch (...) {
        std::string message("storage migration threw name=");
        message.append(name);
        return Status::internal(std::move(message));
    }
}

[[nodiscard]] const StorageMigration* findMigration(
    const std::vector<StorageMigration>& migrations,
    Version current,
    Version target)
{
    const auto it = std::find_if(migrations.begin(), migrations.end(), [&](const auto& migration) {
        return migration.fromVersion_ == current && migration.toVersion_ <= target;
    });
    if (it == migrations.end())
        return nullptr;
    return &*it;
}

[[nodiscard]] Status validateMigrationPath(
    const std::vector<StorageMigration>& migrations,
    Version current,
    Version target)
{
    while (current < target) {
        const auto* migration = findMigration(migrations, current, target);
        if (!migration)
            return Status::failedPrecondition("missing storage migration");
        current = migration->toVersion_;
    }
    return Status::ok();
}

[[nodiscard]] Result<Version> parseVersion(std::string_view text)
{
    if (text.empty())
        return Status::invalidArgument("storage schema version is empty");

    std::uint64_t value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc {} || result.ptr != end)
        return Status::invalidArgument("storage schema version must be an unsigned integer");
    if (value > std::numeric_limits<Version>::max())
        return Status::resourceExhausted("storage schema version is too large");

    return static_cast<Version>(value);
}

[[nodiscard]] Status validateMigration(const StorageMigration& migration)
{
    if (migration.name_.empty())
        return Status::invalidArgument("storage migration name cannot be empty");
    if (!migration.migrate_)
        return Status::invalidArgument("storage migration callback cannot be empty");
    if (migration.toVersion_ <= migration.fromVersion_)
        return Status::invalidArgument("storage migration must increase version");
    if (auto status = requireStorageSchemaVersion(migration.fromVersion_); !status.isOk())
        return status;
    return requireStorageSchemaVersion(migration.toVersion_);
}

} // namespace

Status requireSupportedVersion(Version version, VersionPolicy policy) noexcept
{
    if (version < policy.minSupported_) {
        std::string message(policy.name_);
        message.append(" version is too old");
        return Status::failedPrecondition(std::move(message));
    }
    if (version > policy.current_) {
        std::string message(policy.name_);
        message.append(" version is newer than this runtime supports");
        return Status::unimplemented(std::move(message));
    }
    return Status::ok();
}

Status requireApiContractVersion(Version version) noexcept
{
    return requireSupportedVersion(version, apiContractVersionPolicy());
}

Status requireSchemaContractVersion(Version version) noexcept
{
    return requireSupportedVersion(version, schemaContractVersionPolicy());
}

Status requireCheckpointSchemaVersion(Version version) noexcept
{
    return requireSupportedVersion(version, checkpointSchemaVersionPolicy());
}

Status requireContentEnvelopeVersion(Version version) noexcept
{
    return requireSupportedVersion(version, contentEnvelopeVersionPolicy());
}

Status requireStorageSchemaVersion(Version version) noexcept
{
    return requireSupportedVersion(version, storageSchemaVersionPolicy());
}

Result<std::optional<Version>> readStorageSchemaVersion(
    IStorage& storage,
    const StorageVersionKey& key)
{
    if (auto status = validateVersionKey(key); !status.isOk())
        return status;

    auto item = storage.get(toStorageKey(key));
    if (!item.isOk())
        return item.status();
    if (!item->has_value())
        return std::optional<Version> {};

    auto version = parseVersion((*item)->value_);
    if (!version.isOk())
        return version.status();
    if (auto status = requireStorageSchemaVersion(*version); !status.isOk())
        return status;

    return std::optional<Version>(*version);
}

Result<void> writeStorageSchemaVersion(
    IStorage& storage,
    Version version,
    const StorageVersionKey& key)
{
    if (auto status = validateVersionKey(key); !status.isOk())
        return status;
    if (auto status = requireStorageSchemaVersion(version); !status.isOk())
        return status;

    return storage.put(toStorageKey(key), std::to_string(version));
}

StorageMigrator::StorageMigrator(Version targetVersion)
    : targetVersion_(targetVersion)
{
}

Status StorageMigrator::add(StorageMigration migration)
{
    if (auto status = validateMigration(migration); !status.isOk())
        return status;
    if (migration.toVersion_ > targetVersion_)
        return Status::invalidArgument("storage migration target exceeds migrator target");

    const auto duplicate = std::any_of(migrations_.begin(), migrations_.end(), [&](const auto& existing) {
        return existing.fromVersion_ == migration.fromVersion_;
    });
    if (duplicate)
        return Status::alreadyExists("storage migration already registered for source version");

    migrations_.push_back(std::move(migration));
    std::sort(migrations_.begin(), migrations_.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.fromVersion_ < rhs.fromVersion_;
    });
    return Status::ok();
}

Status StorageMigrator::add(
    Version fromVersion,
    Version toVersion,
    std::string name,
    StorageMigrationFn migrate)
{
    return add(StorageMigration {
        .fromVersion_ = fromVersion,
        .toVersion_ = toVersion,
        .name_ = std::move(name),
        .migrate_ = std::move(migrate),
    });
}

Result<Version> StorageMigrator::migrate(
    IStorage& storage,
    StorageMigrationOptions options) const
{
    const auto target = options.targetVersion_.value_or(targetVersion_);
    if (target > targetVersion_)
        return Status::invalidArgument("storage migration requested target exceeds migrator target");
    if (auto status = requireStorageSchemaVersion(target); !status.isOk())
        return status;
    if (auto status = validateVersionKey(options.versionKey_); !status.isOk())
        return status;
    if (auto status = validateVersionKey(options.lockKey_); !status.isOk())
        return status;
    if (toStorageKey(options.versionKey_) == toStorageKey(options.lockKey_))
        return Status::invalidArgument("storage migration lock key must differ from version key");

    auto stored = readStorageSchemaVersion(storage, options.versionKey_);
    if (!stored.isOk())
        return stored.status();

    if (!stored->has_value()) {
        StorageMigrationLock lock(storage, options.lockKey_);

        if (options.missingVersionMode_ == StorageMigrationMode::InitializeEmptyStorage) {
            if (auto status = lock.acquire(migrationLockValue(0, target, "initialize")); !status.isOk())
                return status;
            if (auto status = storageLooksEmpty(storage, options.versionKey_, options.lockKey_); !status.isOk())
                return status;
            if (auto written = writeStorageSchemaVersion(storage, target, options.versionKey_); !written.isOk())
                return written.status();
            if (auto status = lock.release(); !status.isOk())
                return status;
            return target;
        }

        if (auto status = validateMigrationPath(migrations_, Version { 0 }, target); !status.isOk())
            return status;
        if (auto status = lock.acquire(migrationLockValue(0, target, "legacy")); !status.isOk())
            return status;

        auto current = Version { 0 };
        while (current < target) {
            const auto* migration = findMigration(migrations_, current, target);
            if (!migration)
                return Status::failedPrecondition("missing storage migration");

            StorageMigrationContext context {
                .storage_ = storage,
                .fromVersion_ = migration->fromVersion_,
                .toVersion_ = migration->toVersion_,
            };
            if (auto status = runMigrationCallback(migration->migrate_, context, migration->name_); !status.isOk()) {
                lock.keepHeld();
                return status;
            }
            if (auto written = writeStorageSchemaVersion(storage, migration->toVersion_, options.versionKey_); !written.isOk()) {
                lock.keepHeld();
                return written.status();
            }
            current = migration->toVersion_;
        }

        if (auto status = lock.release(); !status.isOk())
            return status;
        return target;
    }

    auto current = **stored;
    if (current > target)
        return Status::failedPrecondition("storage schema version is newer than requested target");
    if (current == target)
        return current;

    if (auto status = validateMigrationPath(migrations_, current, target); !status.isOk())
        return status;

    StorageMigrationLock lock(storage, options.lockKey_);
    if (auto status = lock.acquire(migrationLockValue(current, target, "migrate")); !status.isOk())
        return status;

    while (current < target) {
        const auto* migration = findMigration(migrations_, current, target);
        if (!migration)
            return Status::failedPrecondition("missing storage migration");

        StorageMigrationContext context {
            .storage_ = storage,
            .fromVersion_ = migration->fromVersion_,
            .toVersion_ = migration->toVersion_,
        };
        if (auto status = runMigrationCallback(migration->migrate_, context, migration->name_); !status.isOk()) {
            lock.keepHeld();
            return status;
        }
        if (auto written = writeStorageSchemaVersion(storage, migration->toVersion_, options.versionKey_); !written.isOk()) {
            lock.keepHeld();
            return written.status();
        }
        current = migration->toVersion_;
    }

    if (auto status = lock.release(); !status.isOk())
        return status;
    return current;
}

Version StorageMigrator::targetVersion() const noexcept
{
    return targetVersion_;
}

const std::vector<StorageMigration>& StorageMigrator::migrations() const noexcept
{
    return migrations_;
}

} // namespace lc
