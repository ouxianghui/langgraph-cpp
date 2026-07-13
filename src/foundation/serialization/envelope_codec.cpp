#include "foundation/serialization/content_envelope.hpp"

#include "foundation/crypto/crypto.hpp"
#include "foundation/serialization/content_envelope_common.hh"
#include "foundation/versioning/versioning.hpp"

#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace lc {
namespace {
using nlohmann::json;
using namespace content_envelope_detail;

#if LANGGRAPH_CPP_WITH_CRYPTO
[[nodiscard]] json envelopeAadJson(const Envelope& envelope)
{
    json checksum = nullptr;
    if (envelope.checksum_.has_value()) {
        checksum = json {
            { "algorithm", envelope.checksum_->algorithm_ },
            { "value_hex", envelope.checksum_->valueHex_ },
        };
    }

    return json {
        { "version", envelope.version_ },
        { "content_type", envelope.contentType_ },
        { "encoding", envelope.encoding_ },
        { "compression", {
                               { "algorithm", compressionNameJson(envelope.compression_) },
                               { "original_size", envelope.originalSize_ },
                           } },
        { "checksum", std::move(checksum) },
    };
}

[[nodiscard]] Bytes buildEnvelopeAssociatedData(const Envelope& envelope)
{
    return bytesFromString(envelopeAadJson(envelope).dump());
}
#endif

[[nodiscard]] Result<Checksum> computeChecksum(const Bytes& bytes)
{
#if LANGGRAPH_CPP_WITH_CRYPTO
    auto digest = digestHex(HashAlgorithm::Sha256, bytesView(bytes));
    if (!digest.isOk())
        return digest.status();
    return Checksum {
        .algorithm_ = std::string(kChecksumSha256),
        .valueHex_ = std::move(*digest),
    };
#else
    (void)bytes;
    return Status::unimplemented("content envelope checksum requires crypto support");
#endif
}

[[nodiscard]] Result<void> verifyChecksum(const Envelope& envelope, const Bytes& bytes)
{
    if (!envelope.checksum_.has_value())
        return okResult();

    if (envelope.checksum_->algorithm_ != kChecksumSha256)
        return Status::unimplemented("unsupported content envelope checksum algorithm");

    auto actual = computeChecksum(bytes);
    if (!actual.isOk())
        return actual.status();
    if (actual->valueHex_ != envelope.checksum_->valueHex_)
        return Status::dataLoss("content envelope checksum mismatch");
    return okResult();
}

} // namespace

EnvelopeCodec::EnvelopeCodec(
    std::shared_ptr<ICompressor> compressor,
    std::shared_ptr<IEncryptor> encryptor)
    : compressor_(std::move(compressor))
    , encryptor_(std::move(encryptor))
{
    if (!compressor_)
        throw std::invalid_argument("EnvelopeCodec requires a compressor");
}

Result<Envelope> EnvelopeCodec::wrap(
    const Payload& payload,
    const EnvelopeOptions& options) const
{
    if (payload.contentType_.empty())
        return Status::invalidArgument("payload content type cannot be empty");

    Bytes original = bytesFromString(payload.data_);

    Envelope envelope {
        .version_ = kContentEnvelopeVersion,
        .contentType_ = payload.contentType_,
        .encoding_ = std::string(kUtf8Encoding),
        .compression_ = options.compression_.algorithm_,
        .originalSize_ = original.size(),
    };

    if (options.checksum_) {
        auto checksum = computeChecksum(original);
        if (!checksum.isOk())
            return checksum.status();
        envelope.checksum_ = std::move(*checksum);
    }

    auto compressed = compressor_->compress(byteSpan(original), options.compression_);
    if (!compressed.isOk())
        return compressed.status();
    envelope.compression_ = compressed->algorithm_;
    envelope.originalSize_ = original.size();

    Bytes data;
    data.reserve(compressed->data_.size());
    for (const auto byte : compressed->data_)
        data.push_back(static_cast<std::uint8_t>(byte));

    if (options.encrypt_) {
#if LANGGRAPH_CPP_WITH_CRYPTO
        if (!encryptor_)
            return Status::failedPrecondition("content envelope encryption requires an encryptor");

        auto aad = buildEnvelopeAssociatedData(envelope);
        auto encryptionOptions = options.encryption_;
        encryptionOptions.associatedData_ = aad;

        auto encrypted = encryptor_->encrypt(bytesView(data), encryptionOptions);
        if (!encrypted.isOk())
            return encrypted.status();

        envelope.encryption_ = EnvelopeEncryption {
            .algorithm_ = encrypted->algorithm_,
            .keyId_ = encrypted->keyId_,
            .nonce_ = std::move(encrypted->nonce_),
            .tag_ = std::move(encrypted->tag_),
        };
        envelope.data_ = std::move(encrypted->ciphertext_);
#else
        return Status::unimplemented("content envelope encryption requires crypto support");
#endif
    } else {
        envelope.data_ = std::move(data);
    }

    return envelope;
}

Result<Payload> EnvelopeCodec::unwrap(
    const Envelope& envelope,
    const EnvelopeOptions& options) const
{
    if (auto status = requireContentEnvelopeVersion(envelope.version_); !status.isOk())
        return status;
    if (envelope.contentType_.empty())
        return Status::invalidArgument("content envelope content_type cannot be empty");
    if (envelope.encoding_ != kUtf8Encoding)
        return Status::unimplemented("unsupported content envelope encoding");

    Bytes data = envelope.data_;

    if (envelope.encryption_.has_value()) {
#if LANGGRAPH_CPP_WITH_CRYPTO
        if (!encryptor_)
            return Status::failedPrecondition("content envelope decryption requires an encryptor");

        const auto expectedAad = buildEnvelopeAssociatedData(envelope);

        Ciphertext encrypted {
            .algorithm_ = envelope.encryption_->algorithm_,
            .keyId_ = envelope.encryption_->keyId_,
            .nonce_ = envelope.encryption_->nonce_,
            .tag_ = envelope.encryption_->tag_,
            .associatedData_ = expectedAad,
            .ciphertext_ = std::move(data),
        };

        auto decrypted = encryptor_->decrypt(
            encrypted,
            DecryptionOptions {
                .associatedData_ = expectedAad,
            });
        if (!decrypted.isOk())
            return decrypted.status();
        data = std::move(*decrypted);
#else
        return Status::unimplemented("content envelope decryption requires crypto support");
#endif
    }

    std::vector<std::byte> compressedBytes;
    compressedBytes.reserve(data.size());
    for (const auto byte : data)
        compressedBytes.push_back(static_cast<std::byte>(byte));

    const DecompressionOptions decompressionOptions {
        .maxOutputBytes_ = options.maxDecodedBytes_ == 0U ? kDefaultMaxDecompressedBytes : options.maxDecodedBytes_,
    };
    auto decompressed = compressor_->decompress(
        CompressedData {
            .algorithm_ = envelope.compression_,
            .data_ = std::move(compressedBytes),
            .originalBytes_ = envelope.originalSize_,
        },
        decompressionOptions);
    if (!decompressed.isOk())
        return decompressed.status();

    Bytes original;
    original.reserve(decompressed->size());
    for (const auto byte : *decompressed)
        original.push_back(static_cast<std::uint8_t>(byte));

    if (auto status = verifyChecksum(envelope, original); !status.isOk())
        return status.status();

    return Payload {
        .contentType_ = envelope.contentType_,
        .data_ = stringFromBytes(original),
    };
}

Result<Payload> EnvelopeCodec::encode(
    const Payload& payload,
    const EnvelopeOptions& options) const
{
    auto envelope = wrap(payload, options);
    if (!envelope.isOk())
        return envelope.status();

    auto serialized = serializeEnvelope(*envelope);
    if (!serialized.isOk())
        return serialized.status();

    return Payload {
        .contentType_ = std::string(kEnvelopeContentType),
        .data_ = std::move(*serialized),
    };
}

Result<Payload> EnvelopeCodec::decode(
    const Payload& payload,
    const EnvelopeOptions& options) const
{
    if (!isEnvelopePayload(payload))
        return Status::invalidArgument("payload content type must be content envelope json");

    auto envelope = deserializeEnvelope(payload.data_, options.decodeLimits_);
    if (!envelope.isOk())
        return envelope.status();
    return unwrap(*envelope, options);
}

} // namespace lc
