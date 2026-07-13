#include "foundation/crypto/encryption.hpp"

#include "foundation/crypto/encryption_common.hh"

#include <exception>
#include <limits>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace lc {
namespace {
using nlohmann::json;
using namespace encryption_detail;

[[nodiscard]] std::size_t maxHexLengthForBytes(std::size_t maxBytes) noexcept
{
    if (maxBytes > std::numeric_limits<std::size_t>::max() / 2U)
        return std::numeric_limits<std::size_t>::max();
    return maxBytes * 2U;
}

[[nodiscard]] bool encryptedPayloadFieldAllowed(std::string_view key) noexcept
{
    return key == "version"
        || key == "algorithm"
        || key == "key_id"
        || key == "nonce_hex"
        || key == "tag_hex"
        || key == "ciphertext_hex";
}

[[nodiscard]] Result<void> validateEncryptedPayloadSchema(
    const json& input,
    const EncryptedPayloadOptions& options)
{
    if (!input.is_object())
        return Status::invalidArgument("encrypted payload must be a JSON object");

    if (options.rejectUnknownFields_) {
        for (auto it = input.begin(); it != input.end(); ++it) {
            if (!encryptedPayloadFieldAllowed(it.key())) {
                std::string message("encrypted payload contains unknown field: ");
                message.append(it.key());
                return Status::invalidArgument(std::move(message));
            }
        }
    }

    if (!input.contains("version") || !input.at("version").is_number_unsigned())
        return Status::invalidArgument("encrypted payload version is required");
    if (input.at("version").get<std::uint64_t>() != kEncryptedPayloadVersion)
        return Status::unimplemented("unsupported encrypted payload version");

    return okResult();
}

[[nodiscard]] Result<std::string> requiredJsonString(
    const json& value,
    const char* key,
    std::size_t maxBytes,
    std::string_view label)
{
    if (!value.contains(key) || !value.at(key).is_string()) {
        std::string message("encrypted payload field is required string: ");
        message.append(key);
        return Status::invalidArgument(std::move(message));
    }
    auto text = value.at(key).get<std::string>();
    if (text.size() > maxBytes) {
        std::string message("encrypted payload ");
        message.append(label);
        message.append(" is too large");
        return Status::resourceExhausted(std::move(message));
    }
    return text;
}

[[nodiscard]] Result<Bytes> jsonHexBytes(
    const json& value,
    const char* key,
    std::size_t maxBytes,
    std::string_view label)
{
    auto text = requiredJsonString(value, key, maxHexLengthForBytes(maxBytes), label);
    if (!text.isOk())
        return text.status();
    auto decoded = fromHex(*text);
    if (!decoded.isOk())
        return decoded.status();
    if (decoded->size() > maxBytes) {
        std::string message("encrypted payload ");
        message.append(label);
        message.append(" is too large");
        return Status::resourceExhausted(std::move(message));
    }
    return decoded;
}

} // namespace

std::string_view encryptionName(EncryptionAlgorithm algorithm) noexcept
{
    switch (algorithm) {
    case EncryptionAlgorithm::AesGcm:
        return "aes-gcm";
    }
    return "unknown";
}

Result<std::string> encodeEncrypted(
    const Ciphertext& payload,
    const EncryptedPayloadOptions& options)
{
    if (payload.algorithm_ != EncryptionAlgorithm::AesGcm)
        return Status::unimplemented("unsupported encryption algorithm");
    if (payload.nonce_.size() != kAesGcmNonceSize)
        return Status::invalidArgument("AES-GCM nonce must be 12 bytes");
    if (payload.tag_.size() != kAesGcmTagSize)
        return Status::invalidArgument("AES-GCM tag must be 16 bytes");
    if (auto status = validateKeyId(payload.keyId_, options); !status.isOk())
        return status;
    if (auto status = validateCiphertextSize(payload.ciphertext_.size(), options); !status.isOk())
        return status;

    json out {
        { "version", kEncryptedPayloadVersion },
        { "algorithm", encryptionName(payload.algorithm_) },
        { "key_id", payload.keyId_ },
        { "nonce_hex", toHex(payload.nonce_) },
        { "tag_hex", toHex(payload.tag_) },
        { "ciphertext_hex", toHex(payload.ciphertext_) },
    };
    auto text = out.dump();
    if (text.size() > options.maxPayloadBytes_)
        return Status::resourceExhausted("encrypted payload json is too large");
    return text;
}

Result<Ciphertext> decodeEncrypted(
    std::string_view text,
    const EncryptedPayloadOptions& options)
{
    if (text.size() > options.maxPayloadBytes_)
        return Status::resourceExhausted("encrypted payload json is too large");

    json input;
    try {
        input = json::parse(text);
    } catch (const std::exception& error) {
        std::string message("failed to parse encrypted payload: ");
        message.append(error.what());
        return Status::invalidArgument(std::move(message));
    }

    if (auto status = validateEncryptedPayloadSchema(input, options); !status.isOk())
        return status.status();
    if (!input.contains("algorithm") || !input.at("algorithm").is_string())
        return Status::invalidArgument("encrypted payload algorithm is required");
    if (input.at("algorithm").get<std::string>() != encryptionName(EncryptionAlgorithm::AesGcm))
        return Status::unimplemented("unsupported encryption algorithm");

    auto keyId = requiredJsonString(input, "key_id", options.maxKeyIdBytes_, "key_id");
    if (!keyId.isOk())
        return keyId.status();
    auto nonce = jsonHexBytes(input, "nonce_hex", kAesGcmNonceSize, "nonce");
    if (!nonce.isOk())
        return nonce.status();
    if (nonce->size() != kAesGcmNonceSize)
        return Status::invalidArgument("AES-GCM nonce must be 12 bytes");
    auto tag = jsonHexBytes(input, "tag_hex", kAesGcmTagSize, "tag");
    if (!tag.isOk())
        return tag.status();
    if (tag->size() != kAesGcmTagSize)
        return Status::invalidArgument("AES-GCM tag must be 16 bytes");
    auto ciphertext = jsonHexBytes(input, "ciphertext_hex", options.maxCiphertextBytes_, "ciphertext");
    if (!ciphertext.isOk())
        return ciphertext.status();

    return Ciphertext {
        .algorithm_ = EncryptionAlgorithm::AesGcm,
        .keyId_ = *keyId,
        .nonce_ = std::move(*nonce),
        .tag_ = std::move(*tag),
        .ciphertext_ = std::move(*ciphertext),
    };
}

Result<Ciphertext> encryptText(
    const IEncryptor& encryptor,
    std::string_view plaintext,
    const EncryptionOptions& options)
{
    return encryptor.encrypt(bytesFromString(plaintext), options);
}

Result<std::string> decryptText(
    const IEncryptor& encryptor,
    const Ciphertext& payload,
    const DecryptionOptions& options)
{
    auto plaintext = encryptor.decrypt(payload, options);
    if (!plaintext.isOk())
        return plaintext.status();
    return stringFromBytes(*plaintext);
}

} // namespace lc
