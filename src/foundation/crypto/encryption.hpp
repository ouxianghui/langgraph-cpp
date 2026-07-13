#pragma once

#include "foundation/crypto/crypto.hpp"
#include "foundation/serialization/state_codec.hpp"
#include "foundation/status/result.hpp"
#include "foundation/storage/i_storage.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace lc {

enum class EncryptionAlgorithm : std::uint8_t {
    AesGcm,
};

struct EncryptionOptions {
    EncryptionAlgorithm algorithm_ { EncryptionAlgorithm::AesGcm };
    std::string keyId_;
    Bytes associatedData_;
    std::optional<Bytes> nonce_;
};

struct DecryptionOptions {
    Bytes associatedData_;
};

struct EncryptedPayloadOptions {
    std::size_t maxPayloadBytes_ { 64 * 1024 * 1024 };
    std::size_t maxKeyIdBytes_ { 256 };
    std::size_t maxAssociatedDataBytes_ { 64 * 1024 };
    std::size_t maxCiphertextBytes_ { 64 * 1024 * 1024 };
    bool rejectUnknownFields_ { true };
};

struct Ciphertext {
    EncryptionAlgorithm algorithm_ { EncryptionAlgorithm::AesGcm };
    std::string keyId_;
    Bytes nonce_;
    Bytes tag_;
    Bytes associatedData_;
    Bytes ciphertext_;
};

class IEncryptor {
public:
    virtual ~IEncryptor() = default;

    IEncryptor(const IEncryptor&) = delete;
    IEncryptor& operator=(const IEncryptor&) = delete;
    IEncryptor(IEncryptor&&) = delete;
    IEncryptor& operator=(IEncryptor&&) = delete;

protected:
    IEncryptor() = default;

public:
    [[nodiscard]] virtual bool supports(EncryptionAlgorithm algorithm) const noexcept = 0;

    [[nodiscard]] virtual Result<Ciphertext> encrypt(
        std::span<const std::uint8_t> plaintext,
        const EncryptionOptions& options = {}) const
        = 0;

    [[nodiscard]] virtual Result<Bytes> decrypt(
        const Ciphertext& payload,
        const DecryptionOptions& options) const
        = 0;
};

class AesGcm final : public IEncryptor {
public:
    explicit AesGcm(SecureBytes key, std::string defaultKeyId = {});

    [[nodiscard]] static Result<SecureBytes> generateKey(std::size_t keySizeBytes = 32);

    [[nodiscard]] bool supports(EncryptionAlgorithm algorithm) const noexcept override;

    [[nodiscard]] Result<Ciphertext> encrypt(
        std::span<const std::uint8_t> plaintext,
        const EncryptionOptions& options = {}) const override;

    [[nodiscard]] Result<Bytes> decrypt(
        const Ciphertext& payload,
        const DecryptionOptions& options) const override;

    [[nodiscard]] const std::string& defaultKeyId() const noexcept;

private:
    SecureBytes key_;
    std::string defaultKeyId_;
};

class SecureCheckpointCodec final : public ICheckpointCodec {
public:
    SecureCheckpointCodec(
        std::shared_ptr<ICheckpointCodec> inner,
        std::shared_ptr<IEncryptor> encryptor,
        std::string keyId = {});

    [[nodiscard]] Result<Payload> encode(const Checkpoint& checkpoint) const override;
    [[nodiscard]] Result<Checkpoint> decode(const Payload& payload) const override;
    [[nodiscard]] Result<Payload> encodeWrite(const CheckpointWrite& write) const override;
    [[nodiscard]] Result<CheckpointWrite> decodeWrite(const Payload& payload) const override;

private:
    std::shared_ptr<ICheckpointCodec> inner_;
    std::shared_ptr<IEncryptor> encryptor_;
    std::string keyId_;
};

class SecureStorage final : public IStorage {
public:
    SecureStorage(
        std::shared_ptr<IStorage> inner,
        std::shared_ptr<IEncryptor> encryptor,
        std::string keyId = {});

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
    std::shared_ptr<IStorage> inner_;
    std::shared_ptr<IEncryptor> encryptor_;
    std::string keyId_;
};

[[nodiscard]] std::string_view encryptionName(EncryptionAlgorithm algorithm) noexcept;

[[nodiscard]] Result<std::string> encodeEncrypted(
    const Ciphertext& payload,
    const EncryptedPayloadOptions& options = {});
[[nodiscard]] Result<Ciphertext> decodeEncrypted(
    std::string_view text,
    const EncryptedPayloadOptions& options = {});

[[nodiscard]] Result<Ciphertext> encryptText(
    const IEncryptor& encryptor,
    std::string_view plaintext,
    const EncryptionOptions& options = {});
[[nodiscard]] Result<std::string> decryptText(
    const IEncryptor& encryptor,
    const Ciphertext& payload,
    const DecryptionOptions& options = {});

} // namespace lc
