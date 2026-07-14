#pragma once

#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"
#include "foundation/storage/i_storage.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lc {

using Version = std::uint32_t;

inline constexpr Version kApiContractVersion = 24;
inline constexpr Version kMinApiContractVersion = 24;
inline constexpr Version kSchemaContractVersion = 1;
inline constexpr Version kMinSchemaContractVersion = 1;
inline constexpr Version kCheckpointSchemaVersion = 3;
inline constexpr Version kMinCheckpointSchemaVersion = 1;
inline constexpr Version kContentEnvelopeVersion = 1;
inline constexpr Version kMinContentEnvelopeVersion = 1;
inline constexpr Version kStorageSchemaVersion = 1;
inline constexpr Version kMinStorageSchemaVersion = 0;

struct VersionPolicy {
    std::string_view name_;
    Version minSupported_ { 1 };
    Version current_ { 1 };
};

[[nodiscard]] constexpr VersionPolicy apiContractVersionPolicy() noexcept
{
    return VersionPolicy {
        .name_ = "api_contract",
        .minSupported_ = kMinApiContractVersion,
        .current_ = kApiContractVersion,
    };
}

[[nodiscard]] constexpr VersionPolicy schemaContractVersionPolicy() noexcept
{
    return VersionPolicy {
        .name_ = "schema_contract",
        .minSupported_ = kMinSchemaContractVersion,
        .current_ = kSchemaContractVersion,
    };
}

[[nodiscard]] constexpr VersionPolicy checkpointSchemaVersionPolicy() noexcept
{
    return VersionPolicy {
        .name_ = "checkpoint_schema",
        .minSupported_ = kMinCheckpointSchemaVersion,
        .current_ = kCheckpointSchemaVersion,
    };
}

[[nodiscard]] constexpr VersionPolicy contentEnvelopeVersionPolicy() noexcept
{
    return VersionPolicy {
        .name_ = "content_envelope",
        .minSupported_ = kMinContentEnvelopeVersion,
        .current_ = kContentEnvelopeVersion,
    };
}

[[nodiscard]] constexpr VersionPolicy storageSchemaVersionPolicy() noexcept
{
    return VersionPolicy {
        .name_ = "storage_schema",
        .minSupported_ = kMinStorageSchemaVersion,
        .current_ = kStorageSchemaVersion,
    };
}

[[nodiscard]] Status requireSupportedVersion(Version version, VersionPolicy policy) noexcept;
[[nodiscard]] Status requireApiContractVersion(Version version) noexcept;
[[nodiscard]] Status requireSchemaContractVersion(Version version) noexcept;
[[nodiscard]] Status requireCheckpointSchemaVersion(Version version) noexcept;
[[nodiscard]] Status requireContentEnvelopeVersion(Version version) noexcept;
[[nodiscard]] Status requireStorageSchemaVersion(Version version) noexcept;

struct StorageVersionKey {
    std::string scope_ { "__lc_system" };
    std::string key_ { "storage_schema_version" };
};

enum class StorageMigrationMode : std::uint8_t {
    InitializeEmptyStorage,
    MigrateLegacyStorage,
};

[[nodiscard]] Result<std::optional<Version>> readStorageSchemaVersion(
    IStorage& storage,
    const StorageVersionKey& key = {});

[[nodiscard]] Result<void> writeStorageSchemaVersion(
    IStorage& storage,
    Version version,
    const StorageVersionKey& key = {});

struct StorageMigrationContext {
    IStorage& storage_;
    Version fromVersion_ { 0 };
    Version toVersion_ { 0 };
};

using StorageMigrationFn = std::function<Status(StorageMigrationContext&)>;

struct StorageMigration {
    Version fromVersion_ { 0 };
    Version toVersion_ { 0 };
    std::string name_;
    StorageMigrationFn migrate_;
};

struct StorageMigrationOptions {
    StorageVersionKey versionKey_;
    StorageVersionKey lockKey_ {
        .scope_ = "__lc_system",
        .key_ = "storage_schema_migration_lock",
    };
    std::optional<Version> targetVersion_;
    StorageMigrationMode missingVersionMode_ { StorageMigrationMode::InitializeEmptyStorage };
};

class StorageMigrator final {
public:
    explicit StorageMigrator(Version targetVersion = kStorageSchemaVersion);

    [[nodiscard]] Status add(StorageMigration migration);
    [[nodiscard]] Status add(
        Version fromVersion,
        Version toVersion,
        std::string name,
        StorageMigrationFn migrate);

    [[nodiscard]] Result<Version> migrate(
        IStorage& storage,
        StorageMigrationOptions options = {}) const;

    [[nodiscard]] Version targetVersion() const noexcept;
    [[nodiscard]] const std::vector<StorageMigration>& migrations() const noexcept;

private:
    Version targetVersion_ { kStorageSchemaVersion };
    std::vector<StorageMigration> migrations_;
};

} // namespace lc
