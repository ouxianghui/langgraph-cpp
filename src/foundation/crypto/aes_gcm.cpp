#include "foundation/crypto/encryption.hpp"

#include "foundation/crypto/encryption_common.hh"

#include <array>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <openssl/evp.h>

namespace lc {
namespace {
using namespace encryption_detail;

[[nodiscard]] const EVP_CIPHER* cipherForKeySize(std::size_t keySize) noexcept
{
    switch (keySize) {
    case 16:
        return EVP_aes_128_gcm();
    case 24:
        return EVP_aes_192_gcm();
    case 32:
        return EVP_aes_256_gcm();
    default:
        return nullptr;
    }
}

[[nodiscard]] Status validateAesGcmKey(std::span<const std::uint8_t> key)
{
    if (cipherForKeySize(key.size()) == nullptr)
        return Status::invalidArgument("AES-GCM key must be 16, 24, or 32 bytes");
    return Status::ok();
}

[[nodiscard]] Status validateEvpInputSize(std::size_t size, std::string_view label)
{
    if (size <= static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return Status::ok();

    std::string message(label);
    message.append(" is too large for AES-GCM");
    return Status::resourceExhausted(std::move(message));
}

class EvpCipherContext final {
public:
    EvpCipherContext() noexcept
        : context_(EVP_CIPHER_CTX_new())
    {
    }

    EvpCipherContext(const EvpCipherContext&) = delete;
    EvpCipherContext& operator=(const EvpCipherContext&) = delete;

    ~EvpCipherContext()
    {
        EVP_CIPHER_CTX_free(context_);
    }

    [[nodiscard]] EVP_CIPHER_CTX* get() const noexcept { return context_; }

private:
    EVP_CIPHER_CTX* context_ { nullptr };
};

} // namespace

AesGcm::AesGcm(SecureBytes key, std::string defaultKeyId)
    : key_(std::move(key))
    , defaultKeyId_(std::move(defaultKeyId))
{
    if (!validateAesGcmKey(key_.span()).isOk())
        throw std::invalid_argument("AES-GCM key must be 16, 24, or 32 bytes");
    if (!validateKeyId(defaultKeyId_, EncryptedPayloadOptions {}).isOk())
        throw std::invalid_argument("AES-GCM default key id is too large");
}

Result<SecureBytes> AesGcm::generateKey(std::size_t keySizeBytes)
{
    if (keySizeBytes != 16U && keySizeBytes != 24U && keySizeBytes != 32U)
        return Status::invalidArgument("AES-GCM key size must be 16, 24, or 32 bytes");
    auto key = random(keySizeBytes);
    if (!key.isOk())
        return key.status();
    return SecureBytes(std::move(*key));
}

bool AesGcm::supports(EncryptionAlgorithm algorithm) const noexcept
{
    return algorithm == EncryptionAlgorithm::AesGcm;
}

Result<Ciphertext> AesGcm::encrypt(
    std::span<const std::uint8_t> plaintext,
    const EncryptionOptions& options) const
{
    if (!supports(options.algorithm_))
        return Status::unimplemented("unsupported encryption algorithm");
    if (auto status = validateAesGcmKey(key_.span()); !status.isOk())
        return status;
    const EncryptedPayloadOptions limits;
    const auto& keyId = options.keyId_.empty() ? defaultKeyId_ : options.keyId_;
    if (auto status = validateKeyId(keyId, limits); !status.isOk())
        return status;
    if (auto status = validateAssociatedDataSize(options.associatedData_.size(), limits); !status.isOk())
        return status;
    if (auto status = validateCiphertextSize(plaintext.size(), limits); !status.isOk())
        return status;

    Bytes nonce;
    if (options.nonce_.has_value()) {
        nonce = *options.nonce_;
        if (nonce.size() != kAesGcmNonceSize)
            return Status::invalidArgument("AES-GCM nonce must be 12 bytes");
    } else {
        auto generated = random(kAesGcmNonceSize);
        if (!generated.isOk())
            return generated.status();
        nonce = std::move(*generated);
    }

    EvpCipherContext context;
    if (context.get() == nullptr)
        return Status::resourceExhausted("failed to allocate AES-GCM context");

    const EVP_CIPHER* cipher = cipherForKeySize(key_.size());
    if (EVP_EncryptInit_ex(context.get(), cipher, nullptr, nullptr, nullptr) != 1)
        return Status::internal("failed to initialize AES-GCM encryption");
    if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) != 1)
        return Status::internal("failed to configure AES-GCM nonce size");
    if (EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key_.data(), nonce.data()) != 1)
        return Status::internal("failed to set AES-GCM key and nonce");

    if (auto status = validateEvpInputSize(options.associatedData_.size(), "associated data"); !status.isOk())
        return status;
    if (auto status = validateEvpInputSize(plaintext.size(), "plaintext"); !status.isOk())
        return status;

    int outSize = 0;
    if (!options.associatedData_.empty()
        && EVP_EncryptUpdate(
               context.get(),
               nullptr,
               &outSize,
               options.associatedData_.data(),
               static_cast<int>(options.associatedData_.size()))
            != 1)
        return Status::internal("failed to authenticate AES-GCM associated data");

    Bytes ciphertext(plaintext.size());
    if (!plaintext.empty()
        && EVP_EncryptUpdate(
               context.get(),
               ciphertext.data(),
               &outSize,
               plaintext.data(),
               static_cast<int>(plaintext.size()))
            != 1)
        return Status::internal("failed to encrypt AES-GCM plaintext");
    int totalSize = outSize;

    std::array<std::uint8_t, 16> finalBuffer {};
    auto* finalOut = ciphertext.empty() ? finalBuffer.data() : ciphertext.data() + totalSize;
    if (EVP_EncryptFinal_ex(context.get(), finalOut, &outSize) != 1)
        return Status::internal("failed to finalize AES-GCM encryption");
    totalSize += outSize;
    ciphertext.resize(static_cast<std::size_t>(totalSize));

    Bytes tag(kAesGcmTagSize);
    if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()), tag.data()) != 1)
        return Status::internal("failed to read AES-GCM tag");

    return Ciphertext {
        .algorithm_ = EncryptionAlgorithm::AesGcm,
        .keyId_ = keyId,
        .nonce_ = std::move(nonce),
        .tag_ = std::move(tag),
        .associatedData_ = options.associatedData_,
        .ciphertext_ = std::move(ciphertext),
    };
}

Result<Bytes> AesGcm::decrypt(
    const Ciphertext& payload,
    const DecryptionOptions& options) const
{
    if (!supports(payload.algorithm_))
        return Status::unimplemented("unsupported encryption algorithm");
    if (auto status = validateAesGcmKey(key_.span()); !status.isOk())
        return status;
    if (payload.nonce_.size() != kAesGcmNonceSize)
        return Status::invalidArgument("AES-GCM nonce must be 12 bytes");
    if (payload.tag_.size() != kAesGcmTagSize)
        return Status::invalidArgument("AES-GCM tag must be 16 bytes");
    const EncryptedPayloadOptions limits;
    if (auto status = validateKeyId(payload.keyId_, limits); !status.isOk())
        return status;
    if (auto status = validateAssociatedDataSize(options.associatedData_.size(), limits); !status.isOk())
        return status;
    if (auto status = validateCiphertextSize(payload.ciphertext_.size(), limits); !status.isOk())
        return status;
    if (auto status = validateEvpInputSize(options.associatedData_.size(), "associated data"); !status.isOk())
        return status;
    if (auto status = validateEvpInputSize(payload.ciphertext_.size(), "ciphertext"); !status.isOk())
        return status;

    EvpCipherContext context;
    if (context.get() == nullptr)
        return Status::resourceExhausted("failed to allocate AES-GCM context");

    const EVP_CIPHER* cipher = cipherForKeySize(key_.size());
    if (EVP_DecryptInit_ex(context.get(), cipher, nullptr, nullptr, nullptr) != 1)
        return Status::internal("failed to initialize AES-GCM decryption");
    if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(payload.nonce_.size()), nullptr) != 1)
        return Status::internal("failed to configure AES-GCM nonce size");
    if (EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key_.data(), payload.nonce_.data()) != 1)
        return Status::internal("failed to set AES-GCM key and nonce");

    int outSize = 0;
    if (!options.associatedData_.empty()
        && EVP_DecryptUpdate(
               context.get(),
               nullptr,
               &outSize,
               options.associatedData_.data(),
               static_cast<int>(options.associatedData_.size()))
            != 1)
        return Status::internal("failed to authenticate AES-GCM associated data");

    Bytes plaintext(payload.ciphertext_.size());
    if (!payload.ciphertext_.empty()
        && EVP_DecryptUpdate(
               context.get(),
               plaintext.data(),
               &outSize,
               payload.ciphertext_.data(),
               static_cast<int>(payload.ciphertext_.size()))
            != 1)
        return Status::internal("failed to decrypt AES-GCM ciphertext");
    int totalSize = outSize;

    if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(payload.tag_.size()), const_cast<std::uint8_t*>(payload.tag_.data())) != 1)
        return Status::internal("failed to set AES-GCM tag");

    std::array<std::uint8_t, 16> finalBuffer {};
    auto* finalOut = plaintext.empty() ? finalBuffer.data() : plaintext.data() + totalSize;
    const int finalResult = EVP_DecryptFinal_ex(context.get(), finalOut, &outSize);
    if (finalResult != 1)
        return Status::unauthenticated("encrypted payload authentication failed");

    totalSize += outSize;
    plaintext.resize(static_cast<std::size_t>(totalSize));
    return plaintext;
}

const std::string& AesGcm::defaultKeyId() const noexcept
{
    return defaultKeyId_;
}

} // namespace lc
