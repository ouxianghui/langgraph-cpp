#include "foundation/crypto/encryption.hpp"

#include "foundation/crypto/encryption_common.hh"

#include <exception>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace lgc {
namespace {
using nlohmann::json;
using namespace encryption_detail;

[[nodiscard]] Result<void> requireEncryptedCheckpointContentType(const Payload& payload)
{
    if (payload.contentType_ != kEncryptedCheckpointContentType)
        return Status::invalidArgument("payload content type must be encrypted checkpoint json");
    return okResult();
}

[[nodiscard]] Result<Payload> encryptCheckpointCodecPayload(
    const IEncryptor& encryptor,
    const Payload& payload,
    std::string_view keyId)
{
    EncryptionOptions options {
        .keyId_ = std::string(keyId),
        .associatedData_ = byteVectorFromString(payload.contentType_),
    };
    auto encrypted = encryptText(encryptor, payload.data_, options);
    if (!encrypted.isOk())
        return encrypted.status();

    auto encryptedText = encodeEncrypted(*encrypted);
    if (!encryptedText.isOk())
        return encryptedText.status();

    json envelope {
        { "version", 1 },
        { "inner_content_type", payload.contentType_ },
        { "encrypted", json::parse(*encryptedText) },
    };

    return Payload {
        .contentType_ = std::string(kEncryptedCheckpointContentType),
        .data_ = envelope.dump(),
    };
}

[[nodiscard]] Result<Payload> decryptCheckpointCodecPayload(
    const IEncryptor& encryptor,
    const Payload& payload)
{
    if (auto result = requireEncryptedCheckpointContentType(payload); !result.isOk())
        return result.status();

    json envelope;
    try {
        envelope = json::parse(payload.data_);
    } catch (const std::exception& error) {
        std::string message("failed to parse encrypted checkpoint payload: ");
        message.append(error.what());
        return Status::invalidArgument(std::move(message));
    }

    if (!envelope.is_object())
        return Status::invalidArgument("encrypted checkpoint payload must be an object");
    if (!envelope.contains("inner_content_type") || !envelope.at("inner_content_type").is_string())
        return Status::invalidArgument("encrypted checkpoint inner_content_type is required");
    if (!envelope.contains("encrypted") || !envelope.at("encrypted").is_object())
        return Status::invalidArgument("encrypted checkpoint encrypted payload is required");

    auto innerContentType = envelope.at("inner_content_type").get<std::string>();
    auto encrypted = decodeEncrypted(envelope.at("encrypted").dump());
    if (!encrypted.isOk())
        return encrypted.status();

    auto decrypted = decryptText(
        encryptor,
        *encrypted,
        DecryptionOptions {
            .associatedData_ = byteVectorFromString(innerContentType),
        });
    if (!decrypted.isOk())
        return decrypted.status();

    return Payload {
        .contentType_ = std::move(innerContentType),
        .data_ = std::move(*decrypted),
    };
}

} // namespace

SecureCheckpointCodec::SecureCheckpointCodec(
    std::shared_ptr<ICheckpointCodec> inner,
    std::shared_ptr<IEncryptor> encryptor,
    std::string keyId)
    : inner_(std::move(inner))
    , encryptor_(std::move(encryptor))
    , keyId_(std::move(keyId))
{
    if (!inner_)
        throw std::invalid_argument("SecureCheckpointCodec requires an inner codec");
    if (!encryptor_)
        throw std::invalid_argument("SecureCheckpointCodec requires an encryptor");
}

Result<Payload> SecureCheckpointCodec::encode(const Checkpoint& checkpoint) const
{
    auto encoded = inner_->encode(checkpoint);
    if (!encoded.isOk())
        return encoded.status();
    return encryptCheckpointCodecPayload(*encryptor_, *encoded, keyId_);
}

Result<Checkpoint> SecureCheckpointCodec::decode(const Payload& payload) const
{
    auto decoded = decryptCheckpointCodecPayload(*encryptor_, payload);
    if (!decoded.isOk())
        return decoded.status();
    return inner_->decode(*decoded);
}

Result<Payload> SecureCheckpointCodec::encodeWrite(const CheckpointWrite& write) const
{
    auto encoded = inner_->encodeWrite(write);
    if (!encoded.isOk())
        return encoded.status();
    return encryptCheckpointCodecPayload(*encryptor_, *encoded, keyId_);
}

Result<CheckpointWrite> SecureCheckpointCodec::decodeWrite(const Payload& payload) const
{
    auto decoded = decryptCheckpointCodecPayload(*encryptor_, payload);
    if (!decoded.isOk())
        return decoded.status();
    return inner_->decodeWrite(*decoded);
}

} // namespace lgc
