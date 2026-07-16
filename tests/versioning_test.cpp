#include "foundation/serialization/content_envelope.hpp"
#include "foundation/serialization/state_codec.hpp"
#include "foundation/storage/memory_storage.hpp"
#include "foundation/versioning/versioning.hpp"

#include <cassert>
#include <chrono>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

int main()
{
    using namespace std::chrono_literals;

    assert(lgc::kApiContractVersion == 28);
    assert(lgc::kSchemaContractVersion == 1);
    assert(lgc::kCheckpointSchemaVersion == 3);
    assert(lgc::kContentEnvelopeVersion == 1);
    assert(lgc::kStorageSchemaVersion == 1);

    assert(lgc::apiContractVersionPolicy().current_ == lgc::kApiContractVersion);
    assert(lgc::schemaContractVersionPolicy().current_ == lgc::kSchemaContractVersion);
    assert(lgc::checkpointSchemaVersionPolicy().current_ == lgc::kCheckpointSchemaVersion);
    assert(lgc::contentEnvelopeVersionPolicy().current_ == lgc::kContentEnvelopeVersion);
    assert(lgc::storageSchemaVersionPolicy().current_ == lgc::kStorageSchemaVersion);
    assert(lgc::requireApiContractVersion(lgc::kApiContractVersion).isOk());
    assert(lgc::requireSchemaContractVersion(lgc::kSchemaContractVersion).isOk());
    assert(lgc::requireCheckpointSchemaVersion(lgc::kCheckpointSchemaVersion).isOk());

    auto futureApi = lgc::requireApiContractVersion(lgc::kApiContractVersion + 1);
    assert(!futureApi.isOk());
    assert(futureApi.code() == lgc::StatusCode::Unimplemented);

    auto futureSchemaContract = lgc::requireSchemaContractVersion(lgc::kSchemaContractVersion + 1);
    assert(!futureSchemaContract.isOk());
    assert(futureSchemaContract.code() == lgc::StatusCode::Unimplemented);

    auto futureCheckpoint = lgc::requireCheckpointSchemaVersion(lgc::kCheckpointSchemaVersion + 1);
    assert(!futureCheckpoint.isOk());
    assert(futureCheckpoint.code() == lgc::StatusCode::Unimplemented);

    const auto state = lgc::State::fromJson(R"({"messages":[]})");
    assert(state.isOk());

    lgc::Checkpoint checkpoint {
        .threadId_ = "thread-1",
        .checkpointId_ = "checkpoint-1",
        .step_ = 1,
        .state_ = *state,
        .createdAt_ = std::chrono::system_clock::time_point(123ms),
    };

    lgc::JsonCheckpointCodec checkpointCodec;
    auto encodedCheckpoint = checkpointCodec.encode(checkpoint);
    assert(encodedCheckpoint.isOk());
    auto checkpointJson = nlohmann::json::parse(encodedCheckpoint->data_);
    assert(checkpointJson.at("schema_version") == lgc::kCheckpointSchemaVersion);

    checkpointJson["schema_version"] = lgc::kCheckpointSchemaVersion + 1;
    auto decodedFutureCheckpoint = checkpointCodec.decode(lgc::Payload {
        .contentType_ = encodedCheckpoint->contentType_,
        .data_ = checkpointJson.dump(),
    });
    assert(!decodedFutureCheckpoint.isOk());
    assert(decodedFutureCheckpoint.status().code() == lgc::StatusCode::Unimplemented);

    checkpointJson.erase("schema_version");
    auto decodedLegacyCheckpoint = checkpointCodec.decode(lgc::Payload {
        .contentType_ = encodedCheckpoint->contentType_,
        .data_ = checkpointJson.dump(),
    });
    assert(decodedLegacyCheckpoint.isOk());
    assert(decodedLegacyCheckpoint->checkpointId_ == checkpoint.checkpointId_);

    lgc::EnvelopeCodec pipeline;
    auto envelopePayload = pipeline.encode(lgc::Payload {
        .contentType_ = "application/json",
        .data_ = R"({"ok":true})",
    });
    assert(envelopePayload.isOk());
    auto envelopeJson = nlohmann::json::parse(envelopePayload->data_);
    assert(envelopeJson.at("version") == lgc::kContentEnvelopeVersion);

    envelopeJson["version"] = lgc::kContentEnvelopeVersion + 1;
    auto futureEnvelope = pipeline.decode(lgc::Payload {
        .contentType_ = std::string(lgc::envelopeContentType()),
        .data_ = envelopeJson.dump(),
    });
    assert(!futureEnvelope.isOk());
    assert(futureEnvelope.status().code() == lgc::StatusCode::Unimplemented);

    lgc::MemoryStorage storage;
    auto missingVersion = lgc::readStorageSchemaVersion(storage);
    assert(missingVersion.isOk());
    assert(!missingVersion->has_value());

    lgc::StorageMigrator defaultMigrator;
    auto initialized = defaultMigrator.migrate(storage);
    assert(initialized.isOk());
    assert(*initialized == lgc::kStorageSchemaVersion);

    auto storedVersion = lgc::readStorageSchemaVersion(storage);
    assert(storedVersion.isOk());
    assert(storedVersion->has_value());
    assert(**storedVersion == lgc::kStorageSchemaVersion);

    lgc::MemoryStorage dirtyUnversionedStorage;
    assert(dirtyUnversionedStorage.put(
        lgc::StorageKey {
            .scope_ = "checkpoint",
            .key_ = "legacy-data",
        },
        "present")
               .isOk());
    auto dirtyInitialized = defaultMigrator.migrate(dirtyUnversionedStorage);
    assert(!dirtyInitialized.isOk());
    assert(dirtyInitialized.status().code() == lgc::StatusCode::FailedPrecondition);

    lgc::MemoryStorage migratingStorage;
    assert(lgc::writeStorageSchemaVersion(migratingStorage, 0).isOk());

    lgc::StorageMigrator migrator;
    bool migrationRan = false;
    auto added = migrator.add(
        0,
        1,
        "bootstrap",
        [&](lgc::StorageMigrationContext& context) {
            migrationRan = true;
            assert(context.fromVersion_ == 0);
            assert(context.toVersion_ == 1);
            auto written = context.storage_.put(
                lgc::StorageKey {
                    .scope_ = "checkpoint",
                    .key_ = "migrated",
                },
                "true");
            return written.status();
        });
    assert(added.isOk());

    auto duplicate = migrator.add(
        0,
        1,
        "duplicate",
        [](lgc::StorageMigrationContext&) {
            return lgc::Status::ok();
        });
    assert(!duplicate.isOk());
    assert(duplicate.code() == lgc::StatusCode::AlreadyExists);

    auto migrated = migrator.migrate(migratingStorage, lgc::StorageMigrationOptions {
        .missingVersionMode_ = lgc::StorageMigrationMode::MigrateLegacyStorage,
    });
    assert(migrated.isOk());
    assert(*migrated == lgc::kStorageSchemaVersion);
    assert(migrationRan);

    auto marker = migratingStorage.get(lgc::StorageKey {
        .scope_ = "checkpoint",
        .key_ = "migrated",
    });
    assert(marker.isOk());
    assert(marker->has_value());
    assert((*marker)->value_ == "true");

    lgc::MemoryStorage legacyStorage;
    assert(legacyStorage.put(
        lgc::StorageKey {
            .scope_ = "checkpoint",
            .key_ = "legacy-data",
        },
        "present")
               .isOk());

    bool legacyMigrationRan = false;
    lgc::StorageMigrator legacyMigrator;
    assert(legacyMigrator
               .add(
                   0,
                   1,
                   "legacy-bootstrap",
                   [&](lgc::StorageMigrationContext& context) {
                       legacyMigrationRan = true;
                       return context.storage_.put(
                           lgc::StorageKey {
                               .scope_ = "checkpoint",
                               .key_ = "legacy-migrated",
                           },
                           "true")
                           .status();
                   })
               .isOk());
    auto legacyMigrated = legacyMigrator.migrate(legacyStorage, lgc::StorageMigrationOptions {
        .missingVersionMode_ = lgc::StorageMigrationMode::MigrateLegacyStorage,
    });
    assert(legacyMigrated.isOk());
    assert(*legacyMigrated == lgc::kStorageSchemaVersion);
    assert(legacyMigrationRan);

    lgc::MemoryStorage throwingStorage;
    assert(lgc::writeStorageSchemaVersion(throwingStorage, 0).isOk());
    lgc::StorageMigrator throwingMigrator;
    assert(throwingMigrator
               .add(
                   0,
                   1,
                   "throws",
                   [](lgc::StorageMigrationContext&) -> lgc::Status {
                       throw std::runtime_error("boom");
                   })
               .isOk());
    auto thrown = throwingMigrator.migrate(throwingStorage, lgc::StorageMigrationOptions {
        .missingVersionMode_ = lgc::StorageMigrationMode::MigrateLegacyStorage,
    });
    assert(!thrown.isOk());
    assert(thrown.status().code() == lgc::StatusCode::Internal);

    auto lockedRetry = throwingMigrator.migrate(throwingStorage, lgc::StorageMigrationOptions {
        .missingVersionMode_ = lgc::StorageMigrationMode::MigrateLegacyStorage,
    });
    assert(!lockedRetry.isOk());
    assert(lockedRetry.status().code() == lgc::StatusCode::Unavailable);

    return 0;
}
