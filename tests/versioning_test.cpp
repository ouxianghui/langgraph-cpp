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

    assert(lc::kApiContractVersion == 23);
    assert(lc::kSchemaContractVersion == 1);
    assert(lc::kCheckpointSchemaVersion == 3);
    assert(lc::kContentEnvelopeVersion == 1);
    assert(lc::kStorageSchemaVersion == 1);

    assert(lc::apiContractVersionPolicy().current_ == lc::kApiContractVersion);
    assert(lc::schemaContractVersionPolicy().current_ == lc::kSchemaContractVersion);
    assert(lc::checkpointSchemaVersionPolicy().current_ == lc::kCheckpointSchemaVersion);
    assert(lc::contentEnvelopeVersionPolicy().current_ == lc::kContentEnvelopeVersion);
    assert(lc::storageSchemaVersionPolicy().current_ == lc::kStorageSchemaVersion);
    assert(lc::requireApiContractVersion(lc::kApiContractVersion).isOk());
    assert(lc::requireSchemaContractVersion(lc::kSchemaContractVersion).isOk());
    assert(lc::requireCheckpointSchemaVersion(lc::kCheckpointSchemaVersion).isOk());

    auto futureApi = lc::requireApiContractVersion(lc::kApiContractVersion + 1);
    assert(!futureApi.isOk());
    assert(futureApi.code() == lc::StatusCode::Unimplemented);

    auto futureSchemaContract = lc::requireSchemaContractVersion(lc::kSchemaContractVersion + 1);
    assert(!futureSchemaContract.isOk());
    assert(futureSchemaContract.code() == lc::StatusCode::Unimplemented);

    auto futureCheckpoint = lc::requireCheckpointSchemaVersion(lc::kCheckpointSchemaVersion + 1);
    assert(!futureCheckpoint.isOk());
    assert(futureCheckpoint.code() == lc::StatusCode::Unimplemented);

    const auto state = lc::State::fromJson(R"({"messages":[]})");
    assert(state.isOk());

    lc::Checkpoint checkpoint {
        .threadId_ = "thread-1",
        .checkpointId_ = "checkpoint-1",
        .step_ = 1,
        .state_ = *state,
        .createdAt_ = std::chrono::system_clock::time_point(123ms),
    };

    lc::JsonCheckpointCodec checkpointCodec;
    auto encodedCheckpoint = checkpointCodec.encode(checkpoint);
    assert(encodedCheckpoint.isOk());
    auto checkpointJson = nlohmann::json::parse(encodedCheckpoint->data_);
    assert(checkpointJson.at("schema_version") == lc::kCheckpointSchemaVersion);

    checkpointJson["schema_version"] = lc::kCheckpointSchemaVersion + 1;
    auto decodedFutureCheckpoint = checkpointCodec.decode(lc::Payload {
        .contentType_ = encodedCheckpoint->contentType_,
        .data_ = checkpointJson.dump(),
    });
    assert(!decodedFutureCheckpoint.isOk());
    assert(decodedFutureCheckpoint.status().code() == lc::StatusCode::Unimplemented);

    checkpointJson.erase("schema_version");
    auto decodedLegacyCheckpoint = checkpointCodec.decode(lc::Payload {
        .contentType_ = encodedCheckpoint->contentType_,
        .data_ = checkpointJson.dump(),
    });
    assert(decodedLegacyCheckpoint.isOk());
    assert(decodedLegacyCheckpoint->checkpointId_ == checkpoint.checkpointId_);

    lc::EnvelopeCodec pipeline;
    auto envelopePayload = pipeline.encode(lc::Payload {
        .contentType_ = "application/json",
        .data_ = R"({"ok":true})",
    });
    assert(envelopePayload.isOk());
    auto envelopeJson = nlohmann::json::parse(envelopePayload->data_);
    assert(envelopeJson.at("version") == lc::kContentEnvelopeVersion);

    envelopeJson["version"] = lc::kContentEnvelopeVersion + 1;
    auto futureEnvelope = pipeline.decode(lc::Payload {
        .contentType_ = std::string(lc::envelopeContentType()),
        .data_ = envelopeJson.dump(),
    });
    assert(!futureEnvelope.isOk());
    assert(futureEnvelope.status().code() == lc::StatusCode::Unimplemented);

    lc::MemoryStorage storage;
    auto missingVersion = lc::readStorageSchemaVersion(storage);
    assert(missingVersion.isOk());
    assert(!missingVersion->has_value());

    lc::StorageMigrator defaultMigrator;
    auto initialized = defaultMigrator.migrate(storage);
    assert(initialized.isOk());
    assert(*initialized == lc::kStorageSchemaVersion);

    auto storedVersion = lc::readStorageSchemaVersion(storage);
    assert(storedVersion.isOk());
    assert(storedVersion->has_value());
    assert(**storedVersion == lc::kStorageSchemaVersion);

    lc::MemoryStorage dirtyUnversionedStorage;
    assert(dirtyUnversionedStorage.put(
        lc::StorageKey {
            .scope_ = "checkpoint",
            .key_ = "legacy-data",
        },
        "present")
               .isOk());
    auto dirtyInitialized = defaultMigrator.migrate(dirtyUnversionedStorage);
    assert(!dirtyInitialized.isOk());
    assert(dirtyInitialized.status().code() == lc::StatusCode::FailedPrecondition);

    lc::MemoryStorage migratingStorage;
    assert(lc::writeStorageSchemaVersion(migratingStorage, 0).isOk());

    lc::StorageMigrator migrator;
    bool migrationRan = false;
    auto added = migrator.add(
        0,
        1,
        "bootstrap",
        [&](lc::StorageMigrationContext& context) {
            migrationRan = true;
            assert(context.fromVersion_ == 0);
            assert(context.toVersion_ == 1);
            auto written = context.storage_.put(
                lc::StorageKey {
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
        [](lc::StorageMigrationContext&) {
            return lc::Status::ok();
        });
    assert(!duplicate.isOk());
    assert(duplicate.code() == lc::StatusCode::AlreadyExists);

    auto migrated = migrator.migrate(migratingStorage, lc::StorageMigrationOptions {
        .missingVersionMode_ = lc::StorageMigrationMode::MigrateLegacyStorage,
    });
    assert(migrated.isOk());
    assert(*migrated == lc::kStorageSchemaVersion);
    assert(migrationRan);

    auto marker = migratingStorage.get(lc::StorageKey {
        .scope_ = "checkpoint",
        .key_ = "migrated",
    });
    assert(marker.isOk());
    assert(marker->has_value());
    assert((*marker)->value_ == "true");

    lc::MemoryStorage legacyStorage;
    assert(legacyStorage.put(
        lc::StorageKey {
            .scope_ = "checkpoint",
            .key_ = "legacy-data",
        },
        "present")
               .isOk());

    bool legacyMigrationRan = false;
    lc::StorageMigrator legacyMigrator;
    assert(legacyMigrator
               .add(
                   0,
                   1,
                   "legacy-bootstrap",
                   [&](lc::StorageMigrationContext& context) {
                       legacyMigrationRan = true;
                       return context.storage_.put(
                           lc::StorageKey {
                               .scope_ = "checkpoint",
                               .key_ = "legacy-migrated",
                           },
                           "true")
                           .status();
                   })
               .isOk());
    auto legacyMigrated = legacyMigrator.migrate(legacyStorage, lc::StorageMigrationOptions {
        .missingVersionMode_ = lc::StorageMigrationMode::MigrateLegacyStorage,
    });
    assert(legacyMigrated.isOk());
    assert(*legacyMigrated == lc::kStorageSchemaVersion);
    assert(legacyMigrationRan);

    lc::MemoryStorage throwingStorage;
    assert(lc::writeStorageSchemaVersion(throwingStorage, 0).isOk());
    lc::StorageMigrator throwingMigrator;
    assert(throwingMigrator
               .add(
                   0,
                   1,
                   "throws",
                   [](lc::StorageMigrationContext&) -> lc::Status {
                       throw std::runtime_error("boom");
                   })
               .isOk());
    auto thrown = throwingMigrator.migrate(throwingStorage, lc::StorageMigrationOptions {
        .missingVersionMode_ = lc::StorageMigrationMode::MigrateLegacyStorage,
    });
    assert(!thrown.isOk());
    assert(thrown.status().code() == lc::StatusCode::Internal);

    auto lockedRetry = throwingMigrator.migrate(throwingStorage, lc::StorageMigrationOptions {
        .missingVersionMode_ = lc::StorageMigrationMode::MigrateLegacyStorage,
    });
    assert(!lockedRetry.isOk());
    assert(lockedRetry.status().code() == lc::StatusCode::Unavailable);

    return 0;
}
