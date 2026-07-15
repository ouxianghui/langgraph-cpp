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

    auto key = lgc::AesGcm::generateKey();
    assert(key.isOk());
    assert(key->size() == 32);

    lgc::AesGcm encryptor(key->clone(), "local-key");
    assert(encryptor.supports(lgc::EncryptionAlgorithm::AesGcm));
    assert(encryptor.defaultKeyId() == "local-key");
    assert(lgc::encryptionName(lgc::EncryptionAlgorithm::AesGcm) == "aes-gcm");

    const std::string plaintext = R"({"state":"ready","step":1})";
    lgc::EncryptionOptions options {
        .keyId_ = "checkpoint-key",
        .associatedData_ = { 1, 2, 3, 4 },
    };
    const lgc::DecryptionOptions decryptionOptions {
        .associatedData_ = options.associatedData_,
    };
    auto encrypted = lgc::encryptText(encryptor, plaintext, options);
    assert(encrypted.isOk());
    assert(encrypted->keyId_ == "checkpoint-key");
    assert(encrypted->nonce_.size() == 12);
    assert(encrypted->tag_.size() == 16);
    assert(!encrypted->ciphertext_.empty());

    auto decrypted = lgc::decryptText(encryptor, *encrypted, decryptionOptions);
    assert(decrypted.isOk());
    assert(*decrypted == plaintext);

    auto serialized = lgc::encodeEncrypted(*encrypted);
    assert(serialized.isOk());
    assert(serialized->find("aad_hex") == std::string::npos);
    auto parsed = lgc::decodeEncrypted(*serialized);
    assert(parsed.isOk());
    assert(parsed->associatedData_.empty());
    assert(lgc::decryptText(encryptor, *parsed, decryptionOptions).value() == plaintext);

    auto missingAad = lgc::decryptText(encryptor, *parsed);
    assert(!missingAad.isOk());
    assert(missingAad.status().code() == lgc::StatusCode::Unauthenticated);

    auto emptyPlaintext = lgc::encryptText(encryptor, "");
    assert(emptyPlaintext.isOk());
    auto emptyDecrypted = lgc::decryptText(encryptor, *emptyPlaintext);
    assert(emptyDecrypted.isOk());
    assert(emptyDecrypted->empty());

    auto wrongAad = lgc::decryptText(
        encryptor,
        *parsed,
        lgc::DecryptionOptions {
            .associatedData_ = { 4, 3, 2, 1 },
        });
    assert(!wrongAad.isOk());
    assert(wrongAad.status().code() == lgc::StatusCode::Unauthenticated);

    auto wrongKey = lgc::AesGcm::generateKey();
    assert(wrongKey.isOk());
    lgc::AesGcm wrongEncryptor(std::move(*wrongKey), "wrong-key");
    auto wrongKeyResult = lgc::decryptText(wrongEncryptor, *parsed, decryptionOptions);
    assert(!wrongKeyResult.isOk());
    assert(wrongKeyResult.status().code() == lgc::StatusCode::Unauthenticated);

    auto tamperedCiphertext = *parsed;
    tamperedCiphertext.ciphertext_.front() ^= 0x01U;
    auto tamperedCiphertextResult = lgc::decryptText(encryptor, tamperedCiphertext, decryptionOptions);
    assert(!tamperedCiphertextResult.isOk());
    assert(tamperedCiphertextResult.status().code() == lgc::StatusCode::Unauthenticated);

    auto tamperedTag = *parsed;
    tamperedTag.tag_.front() ^= 0x01U;
    auto tamperedTagResult = lgc::decryptText(encryptor, tamperedTag, decryptionOptions);
    assert(!tamperedTagResult.isOk());
    assert(tamperedTagResult.status().code() == lgc::StatusCode::Unauthenticated);

    auto tamperedNonce = *parsed;
    tamperedNonce.nonce_.front() ^= 0x01U;
    auto tamperedNonceResult = lgc::decryptText(encryptor, tamperedNonce, decryptionOptions);
    assert(!tamperedNonceResult.isOk());
    assert(tamperedNonceResult.status().code() == lgc::StatusCode::Unauthenticated);

    auto invalidNonce = *parsed;
    invalidNonce.nonce_.pop_back();
    assert(lgc::encodeEncrypted(invalidNonce).status().code() == lgc::StatusCode::InvalidArgument);
    assert(lgc::decryptText(encryptor, invalidNonce, decryptionOptions).status().code()
        == lgc::StatusCode::InvalidArgument);

    auto invalidTag = *parsed;
    invalidTag.tag_.pop_back();
    assert(lgc::encodeEncrypted(invalidTag).status().code() == lgc::StatusCode::InvalidArgument);
    assert(lgc::decryptText(encryptor, invalidTag, decryptionOptions).status().code()
        == lgc::StatusCode::InvalidArgument);

    assert(lgc::decodeEncrypted("{").status().code() == lgc::StatusCode::InvalidArgument);

    auto payloadJson = json::parse(*serialized);
    auto unknownVersion = payloadJson;
    unknownVersion["version"] = 2;
    assert(lgc::decodeEncrypted(unknownVersion.dump()).status().code() == lgc::StatusCode::Unimplemented);

    auto unknownField = payloadJson;
    unknownField["extra"] = true;
    assert(lgc::decodeEncrypted(unknownField.dump()).status().code() == lgc::StatusCode::InvalidArgument);

    auto serializedAad = payloadJson;
    serializedAad["aad_hex"] = "0102";
    assert(lgc::decodeEncrypted(serializedAad.dump()).status().code() == lgc::StatusCode::InvalidArgument);

    auto missingKeyId = payloadJson;
    missingKeyId.erase("key_id");
    assert(lgc::decodeEncrypted(missingKeyId.dump()).status().code() == lgc::StatusCode::InvalidArgument);

    auto hugeCiphertext = payloadJson;
    hugeCiphertext["ciphertext_hex"] = std::string(32, 'a');
    assert(lgc::decodeEncrypted(
               hugeCiphertext.dump(),
               lgc::EncryptedPayloadOptions { .maxCiphertextBytes_ = 4 })
               .status()
               .code()
        == lgc::StatusCode::ResourceExhausted);

    auto badNonceJson = payloadJson;
    badNonceJson["nonce_hex"] = "00";
    assert(lgc::decodeEncrypted(badNonceJson.dump()).status().code() == lgc::StatusCode::InvalidArgument);

    auto badTagJson = payloadJson;
    badTagJson["tag_hex"] = "00";
    assert(lgc::decodeEncrypted(badTagJson.dump()).status().code() == lgc::StatusCode::InvalidArgument);

    assert(lgc::decodeEncrypted(
               payloadJson.dump(),
               lgc::EncryptedPayloadOptions { .maxPayloadBytes_ = 4 })
               .status()
               .code()
        == lgc::StatusCode::ResourceExhausted);

    bool invalidKeyThrown = false;
    try {
        lgc::AesGcm invalid(lgc::SecureBytes(lgc::Bytes { 1, 2, 3 }));
    } catch (const std::invalid_argument&) {
        invalidKeyThrown = true;
    }
    assert(invalidKeyThrown);
    assert(lgc::AesGcm::generateKey(15).status().code() == lgc::StatusCode::InvalidArgument);

    auto state = lgc::State::fromJson(R"({"messages":["hello"],"count":1})");
    assert(state.isOk());
    lgc::Checkpoint checkpoint {
        .threadId_ = "thread-1",
        .checkpointId_ = "checkpoint-1",
        .step_ = 1,
        .state_ = *state,
        .nextNodes_ = { "planner" },
        .createdAt_ = std::chrono::system_clock::now(),
    };

    auto codec = lgc::SecureCheckpointCodec(
        std::make_shared<lgc::JsonCheckpointCodec>(),
        std::make_shared<lgc::AesGcm>(key->clone(), "codec-key"),
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

    lgc::CheckpointWrite write {
        .nodeId_ = "planner",
        .update_ = *lgc::State::fromJson(R"({"secret":"codec-write-secret"})"),
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

    auto innerStorage = std::make_shared<lgc::MemoryStorage>();
    auto storage = lgc::SecureStorage(
        innerStorage,
        std::make_shared<lgc::AesGcm>(key->clone(), "storage-key"),
        "storage-key");

    lgc::StorageKey storageKey { .scope_ = "checkpoint", .key_ = "thread-1/latest" };
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

    auto listed = storage.list(lgc::StorageListOptions { .scope_ = "checkpoint" });
    assert(listed.isOk());
    assert(listed->items_.size() == 1);
    assert(listed->items_.front().value_ == plaintext);

    auto wrongStorageKey = lgc::AesGcm::generateKey();
    assert(wrongStorageKey.isOk());
    auto wrongStorage = lgc::SecureStorage(
        innerStorage,
        std::make_shared<lgc::AesGcm>(std::move(*wrongStorageKey), "wrong-storage-key"),
        "wrong-storage-key");
    auto wrongStorageRead = wrongStorage.get(storageKey);
    assert(!wrongStorageRead.isOk());
    assert(wrongStorageRead.status().code() == lgc::StatusCode::Unauthenticated);

    auto checkpointStorage = std::make_shared<lgc::MemoryStorage>();
    auto checkpointStorageKey = lgc::AesGcm::generateKey();
    assert(checkpointStorageKey.isOk());
    lgc::StorageSaver encryptedCheckpointer(
        checkpointStorage,
        lgc::StorageSaverOptions {
            .codec_ = std::make_shared<lgc::SecureCheckpointCodec>(
                std::make_shared<lgc::JsonCheckpointCodec>(),
                std::make_shared<lgc::AesGcm>(checkpointStorageKey->clone(), "checkpoint-store-key"),
                "checkpoint-store-key"),
        });

    auto secretState = lgc::State::fromJson(R"({"secret":"storage-checkpoint-secret","count":7})");
    assert(secretState.isOk());
    lgc::Checkpoint secretCheckpoint {
        .threadId_ = "secure-thread",
        .checkpointId_ = "secure-1",
        .step_ = 1,
        .state_ = *secretState,
        .createdAt_ = std::chrono::system_clock::now(),
    };
    assert(encryptedCheckpointer.put(secretCheckpoint).isOk());

    auto rawCheckpoints = checkpointStorage->list(lgc::StorageListOptions {
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

    auto loadedSecret = encryptedCheckpointer.getTuple(lgc::CheckpointQuery::latest("secure-thread"));
    assert(loadedSecret.isOk());
    assert(loadedSecret->has_value());
    assert((*loadedSecret)->checkpoint_.state_.view().at("secret") == "storage-checkpoint-secret");

    auto wrongCheckpointStorageKey = lgc::AesGcm::generateKey();
    assert(wrongCheckpointStorageKey.isOk());
    lgc::StorageSaver wrongKeyCheckpointer(
        checkpointStorage,
        lgc::StorageSaverOptions {
            .codec_ = std::make_shared<lgc::SecureCheckpointCodec>(
                std::make_shared<lgc::JsonCheckpointCodec>(),
                std::make_shared<lgc::AesGcm>(
                    std::move(*wrongCheckpointStorageKey),
                    "wrong-checkpoint-store-key"),
                "wrong-checkpoint-store-key"),
        });
    auto wrongCheckpointRead = wrongKeyCheckpointer.getTuple(lgc::CheckpointQuery::latest("secure-thread"));
    assert(!wrongCheckpointRead.isOk());
    assert(wrongCheckpointRead.status().code() == lgc::StatusCode::Unauthenticated);

    return 0;
}
