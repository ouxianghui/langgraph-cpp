#include "foundation/crypto/encryption.hpp"
#include "foundation/serialization/state_codec.hpp"
#include "foundation/status/status.hpp"
#include "foundation/storage/memory_storage.hpp"
#include "langgraph/checkpoint/checkpointer.hpp"

#include <cassert>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

int main()
{
    using namespace std::chrono_literals;
    using nlohmann::json;

    auto key = lc::AesGcm::generateKey();
    assert(key.isOk());
    assert(key->size() == 32);

    lc::AesGcm encryptor(key->clone(), "local-key");
    assert(encryptor.supports(lc::EncryptionAlgorithm::AesGcm));
    assert(encryptor.defaultKeyId() == "local-key");
    assert(lc::encryptionName(lc::EncryptionAlgorithm::AesGcm) == "aes-gcm");

    const std::string plaintext = R"({"state":"ready","step":1})";
    lc::EncryptionOptions options {
        .keyId_ = "checkpoint-key",
        .associatedData_ = { 1, 2, 3, 4 },
    };
    const lc::DecryptionOptions decryptionOptions {
        .associatedData_ = options.associatedData_,
    };
    auto encrypted = lc::encryptText(encryptor, plaintext, options);
    assert(encrypted.isOk());
    assert(encrypted->keyId_ == "checkpoint-key");
    assert(encrypted->nonce_.size() == 12);
    assert(encrypted->tag_.size() == 16);
    assert(!encrypted->ciphertext_.empty());

    auto decrypted = lc::decryptText(encryptor, *encrypted, decryptionOptions);
    assert(decrypted.isOk());
    assert(*decrypted == plaintext);

    auto serialized = lc::encodeEncrypted(*encrypted);
    assert(serialized.isOk());
    assert(serialized->find("aad_hex") == std::string::npos);
    auto parsed = lc::decodeEncrypted(*serialized);
    assert(parsed.isOk());
    assert(parsed->associatedData_.empty());
    assert(lc::decryptText(encryptor, *parsed, decryptionOptions).value() == plaintext);

    auto missingAad = lc::decryptText(encryptor, *parsed);
    assert(!missingAad.isOk());
    assert(missingAad.status().code() == lc::StatusCode::Unauthenticated);

    auto emptyPlaintext = lc::encryptText(encryptor, "");
    assert(emptyPlaintext.isOk());
    auto emptyDecrypted = lc::decryptText(encryptor, *emptyPlaintext);
    assert(emptyDecrypted.isOk());
    assert(emptyDecrypted->empty());

    auto wrongAad = lc::decryptText(
        encryptor,
        *parsed,
        lc::DecryptionOptions {
            .associatedData_ = { 4, 3, 2, 1 },
        });
    assert(!wrongAad.isOk());
    assert(wrongAad.status().code() == lc::StatusCode::Unauthenticated);

    auto wrongKey = lc::AesGcm::generateKey();
    assert(wrongKey.isOk());
    lc::AesGcm wrongEncryptor(std::move(*wrongKey), "wrong-key");
    auto wrongKeyResult = lc::decryptText(wrongEncryptor, *parsed, decryptionOptions);
    assert(!wrongKeyResult.isOk());
    assert(wrongKeyResult.status().code() == lc::StatusCode::Unauthenticated);

    auto tamperedCiphertext = *parsed;
    tamperedCiphertext.ciphertext_.front() ^= 0x01U;
    auto tamperedCiphertextResult = lc::decryptText(encryptor, tamperedCiphertext, decryptionOptions);
    assert(!tamperedCiphertextResult.isOk());
    assert(tamperedCiphertextResult.status().code() == lc::StatusCode::Unauthenticated);

    auto tamperedTag = *parsed;
    tamperedTag.tag_.front() ^= 0x01U;
    auto tamperedTagResult = lc::decryptText(encryptor, tamperedTag, decryptionOptions);
    assert(!tamperedTagResult.isOk());
    assert(tamperedTagResult.status().code() == lc::StatusCode::Unauthenticated);

    auto tamperedNonce = *parsed;
    tamperedNonce.nonce_.front() ^= 0x01U;
    auto tamperedNonceResult = lc::decryptText(encryptor, tamperedNonce, decryptionOptions);
    assert(!tamperedNonceResult.isOk());
    assert(tamperedNonceResult.status().code() == lc::StatusCode::Unauthenticated);

    auto invalidNonce = *parsed;
    invalidNonce.nonce_.pop_back();
    assert(lc::encodeEncrypted(invalidNonce).status().code() == lc::StatusCode::InvalidArgument);
    assert(lc::decryptText(encryptor, invalidNonce, decryptionOptions).status().code()
        == lc::StatusCode::InvalidArgument);

    auto invalidTag = *parsed;
    invalidTag.tag_.pop_back();
    assert(lc::encodeEncrypted(invalidTag).status().code() == lc::StatusCode::InvalidArgument);
    assert(lc::decryptText(encryptor, invalidTag, decryptionOptions).status().code()
        == lc::StatusCode::InvalidArgument);

    assert(lc::decodeEncrypted("{").status().code() == lc::StatusCode::InvalidArgument);

    auto payloadJson = json::parse(*serialized);
    auto unknownVersion = payloadJson;
    unknownVersion["version"] = 2;
    assert(lc::decodeEncrypted(unknownVersion.dump()).status().code() == lc::StatusCode::Unimplemented);

    auto unknownField = payloadJson;
    unknownField["extra"] = true;
    assert(lc::decodeEncrypted(unknownField.dump()).status().code() == lc::StatusCode::InvalidArgument);

    auto serializedAad = payloadJson;
    serializedAad["aad_hex"] = "0102";
    assert(lc::decodeEncrypted(serializedAad.dump()).status().code() == lc::StatusCode::InvalidArgument);

    auto missingKeyId = payloadJson;
    missingKeyId.erase("key_id");
    assert(lc::decodeEncrypted(missingKeyId.dump()).status().code() == lc::StatusCode::InvalidArgument);

    auto hugeCiphertext = payloadJson;
    hugeCiphertext["ciphertext_hex"] = std::string(32, 'a');
    assert(lc::decodeEncrypted(
               hugeCiphertext.dump(),
               lc::EncryptedPayloadOptions { .maxCiphertextBytes_ = 4 })
               .status()
               .code()
        == lc::StatusCode::ResourceExhausted);

    auto badNonceJson = payloadJson;
    badNonceJson["nonce_hex"] = "00";
    assert(lc::decodeEncrypted(badNonceJson.dump()).status().code() == lc::StatusCode::InvalidArgument);

    auto badTagJson = payloadJson;
    badTagJson["tag_hex"] = "00";
    assert(lc::decodeEncrypted(badTagJson.dump()).status().code() == lc::StatusCode::InvalidArgument);

    assert(lc::decodeEncrypted(
               payloadJson.dump(),
               lc::EncryptedPayloadOptions { .maxPayloadBytes_ = 4 })
               .status()
               .code()
        == lc::StatusCode::ResourceExhausted);

    bool invalidKeyThrown = false;
    try {
        lc::AesGcm invalid(lc::SecureBytes(lc::Bytes { 1, 2, 3 }));
    } catch (const std::invalid_argument&) {
        invalidKeyThrown = true;
    }
    assert(invalidKeyThrown);
    assert(lc::AesGcm::generateKey(15).status().code() == lc::StatusCode::InvalidArgument);

    auto state = lc::State::fromJson(R"({"messages":["hello"],"count":1})");
    assert(state.isOk());
    lc::Checkpoint checkpoint {
        .threadId_ = "thread-1",
        .checkpointId_ = "checkpoint-1",
        .step_ = 1,
        .state_ = *state,
        .nextNodes_ = { "planner" },
        .createdAt_ = std::chrono::system_clock::now(),
    };

    auto codec = lc::SecureCheckpointCodec(
        std::make_shared<lc::JsonCheckpointCodec>(),
        std::make_shared<lc::AesGcm>(key->clone(), "codec-key"),
        "codec-key");
    auto encoded = codec.encode(checkpoint);
    assert(encoded.isOk());
    assert(encoded->contentType_ == "application/vnd.langgraph-cpp.encrypted-checkpoint+json");
    assert(encoded->data_.find("ciphertext_hex") != std::string::npos);
    assert(encoded->data_.find("messages") == std::string::npos);

    auto decoded = codec.decode(*encoded);
    assert(decoded.isOk());
    assert(decoded->threadId_ == checkpoint.threadId_);
    assert(decoded->checkpointId_ == checkpoint.checkpointId_);
    assert(decoded->state_ == checkpoint.state_);

    lc::CheckpointWrite write {
        .nodeId_ = "planner",
        .update_ = *lc::State::fromJson(R"({"secret":"codec-write-secret"})"),
        .order_ = 0,
    };
    auto encodedWrite = codec.encodeWrite(write);
    assert(encodedWrite.isOk());
    assert(encodedWrite->contentType_ == "application/vnd.langgraph-cpp.encrypted-checkpoint+json");
    assert(encodedWrite->data_.find("ciphertext_hex") != std::string::npos);
    assert(encodedWrite->data_.find("codec-write-secret") == std::string::npos);

    auto decodedWrite = codec.decodeWrite(*encodedWrite);
    assert(decodedWrite.isOk());
    assert(*decodedWrite == write);

    auto innerStorage = std::make_shared<lc::MemoryStorage>();
    auto storage = lc::SecureStorage(
        innerStorage,
        std::make_shared<lc::AesGcm>(key->clone(), "storage-key"),
        "storage-key");

    lc::StorageKey storageKey { .scope_ = "checkpoint", .key_ = "thread-1/latest" };
    assert(storage.put(storageKey, plaintext).isOk());
    auto rawStored = innerStorage->get(storageKey);
    assert(rawStored.isOk());
    assert(rawStored->has_value());
    assert(rawStored->value().value_.find("ciphertext_hex") != std::string::npos);
    assert(rawStored->value().value_.find("ready") == std::string::npos);

    auto stored = storage.get(storageKey);
    assert(stored.isOk());
    assert(stored->has_value());
    assert(stored->value().value_ == plaintext);

    auto listed = storage.list(lc::StorageListOptions { .scope_ = "checkpoint" });
    assert(listed.isOk());
    assert(listed->items_.size() == 1);
    assert(listed->items_.front().value_ == plaintext);

    auto wrongStorageKey = lc::AesGcm::generateKey();
    assert(wrongStorageKey.isOk());
    auto wrongStorage = lc::SecureStorage(
        innerStorage,
        std::make_shared<lc::AesGcm>(std::move(*wrongStorageKey), "wrong-storage-key"),
        "wrong-storage-key");
    auto wrongStorageRead = wrongStorage.get(storageKey);
    assert(!wrongStorageRead.isOk());
    assert(wrongStorageRead.status().code() == lc::StatusCode::Unauthenticated);

    auto checkpointStorage = std::make_shared<lc::MemoryStorage>();
    auto checkpointStorageKey = lc::AesGcm::generateKey();
    assert(checkpointStorageKey.isOk());
    lc::StorageSaver encryptedCheckpointer(
        checkpointStorage,
        lc::StorageSaverOptions {
            .codec_ = std::make_shared<lc::SecureCheckpointCodec>(
                std::make_shared<lc::JsonCheckpointCodec>(),
                std::make_shared<lc::AesGcm>(checkpointStorageKey->clone(), "checkpoint-store-key"),
                "checkpoint-store-key"),
        });

    auto secretState = lc::State::fromJson(R"({"secret":"storage-checkpoint-secret","count":7})");
    assert(secretState.isOk());
    lc::Checkpoint secretCheckpoint {
        .threadId_ = "secure-thread",
        .checkpointId_ = "secure-1",
        .step_ = 1,
        .state_ = *secretState,
        .createdAt_ = std::chrono::system_clock::now(),
    };
    assert(encryptedCheckpointer.put(secretCheckpoint).isOk());

    auto rawCheckpoints = checkpointStorage->list(lc::StorageListOptions {
        .scope_ = "langgraph/checkpoints",
    });
    assert(rawCheckpoints.isOk());
    bool sawEncryptedCheckpointPayload = false;
    for (const auto& item : rawCheckpoints->items_) {
        assert(item.value_.find("storage-checkpoint-secret") == std::string::npos);
        if (item.value_.find("encrypted-checkpoint") != std::string::npos) {
            sawEncryptedCheckpointPayload = true;
            assert(item.value_.find("ciphertext_hex") != std::string::npos);
        }
    }
    assert(sawEncryptedCheckpointPayload);

    auto loadedSecret = encryptedCheckpointer.getTuple(lc::CheckpointQuery::latest("secure-thread"));
    assert(loadedSecret.isOk());
    assert(loadedSecret->has_value());
    assert((*loadedSecret)->checkpoint_.state_.view().at("secret") == "storage-checkpoint-secret");

    auto wrongCheckpointStorageKey = lc::AesGcm::generateKey();
    assert(wrongCheckpointStorageKey.isOk());
    lc::StorageSaver wrongKeyCheckpointer(
        checkpointStorage,
        lc::StorageSaverOptions {
            .codec_ = std::make_shared<lc::SecureCheckpointCodec>(
                std::make_shared<lc::JsonCheckpointCodec>(),
                std::make_shared<lc::AesGcm>(
                    std::move(*wrongCheckpointStorageKey),
                    "wrong-checkpoint-store-key"),
                "wrong-checkpoint-store-key"),
        });
    auto wrongCheckpointRead = wrongKeyCheckpointer.getTuple(lc::CheckpointQuery::latest("secure-thread"));
    assert(!wrongCheckpointRead.isOk());
    assert(wrongCheckpointRead.status().code() == lc::StatusCode::Unauthenticated);

    return 0;
}
