#include "foundation/serialization/content_envelope.hpp"

#include "foundation/serialization/content_envelope_common.hh"
#include "foundation/versioning/versioning.hpp"

#include <array>
#include <charconv>
#include <exception>
#include <limits>
#include <span>
#include <utility>

#include <nlohmann/json.hpp>

namespace lgc::content_envelope_detail {

[[nodiscard]] std::span<const std::uint8_t> bytesView(const Bytes& bytes) noexcept
{
    return std::span<const std::uint8_t>(bytes.data(), bytes.size());
}

[[nodiscard]] std::span<const std::byte> byteSpan(const Bytes& bytes) noexcept
{
    return std::as_bytes(bytesView(bytes));
}

[[nodiscard]] Bytes bytesFromString(std::string_view text)
{
    return Bytes(text.begin(), text.end());
}

[[nodiscard]] std::string stringFromBytes(const Bytes& bytes)
{
    return std::string(bytes.begin(), bytes.end());
}

[[nodiscard]] std::string compressionNameJson(CompressionAlgorithm algorithm)
{
    return std::string(lgc::compressionName(algorithm));
}

[[nodiscard]] Result<CompressionAlgorithm> compressionFromName(std::string_view name)
{
    if (name == lgc::compressionName(CompressionAlgorithm::None)
        || name == compressionEncoding(CompressionAlgorithm::None)) {
        return CompressionAlgorithm::None;
    }
    if (name == lgc::compressionName(CompressionAlgorithm::Gzip)
        || name == compressionEncoding(CompressionAlgorithm::Gzip)) {
        return CompressionAlgorithm::Gzip;
    }
    if (name == lgc::compressionName(CompressionAlgorithm::Zstd)
        || name == compressionEncoding(CompressionAlgorithm::Zstd)) {
        return CompressionAlgorithm::Zstd;
    }
    return Status::unimplemented("unsupported envelope compression algorithm");
}
} // namespace lgc::content_envelope_detail

namespace lgc {
namespace {
using nlohmann::json;
using namespace content_envelope_detail;

[[nodiscard]] std::string hexEncode(std::span<const std::uint8_t> bytes)
{
    constexpr std::array<char, 16> kHex {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };

    std::string out;
    out.resize(bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[i * 2] = kHex[(bytes[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[bytes[i] & 0x0F];
    }
    return out;
}

[[nodiscard]] Result<Bytes> hexDecode(
    std::string_view text,
    std::string_view label = "hex string",
    std::size_t maxOutputBytes = 0)
{
    if ((text.size() % 2U) != 0U)
        return Status::invalidArgument(std::string(label) + " must have even length");
    if (maxOutputBytes != 0U && text.size() / 2U > maxOutputBytes)
        return Status::resourceExhausted(std::string(label) + " is too large");

    Bytes out(text.size() / 2U);
    for (std::size_t i = 0; i < out.size(); ++i) {
        unsigned value = 0;
        const auto first = text.data() + i * 2;
        const auto last = first + 2;
        const auto [ptr, ec] = std::from_chars(first, last, value, 16);
        if (ec != std::errc() || ptr != last)
            return Status::invalidArgument(std::string(label) + " contains invalid characters");
        out[i] = static_cast<std::uint8_t>(value);
    }
    return out;
}

[[nodiscard]] Result<EncryptionAlgorithm> encryptionFromName(std::string_view name)
{
    if (name == kAesGcmName)
        return EncryptionAlgorithm::AesGcm;
    return Status::unimplemented("unsupported envelope encryption algorithm");
}

[[nodiscard]] Result<std::string> requiredString(const json& input, const char* key)
{
    if (!input.contains(key) || !input.at(key).is_string()) {
        std::string message("content envelope field is required string: ");
        message.append(key);
        return Status::invalidArgument(std::move(message));
    }
    return input.at(key).get<std::string>();
}

[[nodiscard]] Result<std::uint64_t> requiredUnsigned(const json& input, const char* key)
{
    if (!input.contains(key) || !input.at(key).is_number_unsigned()) {
        std::string message("content envelope field is required unsigned integer: ");
        message.append(key);
        return Status::invalidArgument(std::move(message));
    }
    return input.at(key).get<std::uint64_t>();
}
[[nodiscard]] Result<EnvelopeEncryption> encryptionFromJson(const json& input, const JsonDecodeLimits& limits)
{
    if (!input.is_object())
        return Status::invalidArgument("content envelope encryption must be an object");
    if (auto status = rejectUnknownJsonFields(
            input,
            { "algorithm", "key_id", "nonce_hex", "tag_hex" },
            "content envelope encryption",
            limits);
        !status.isOk()) {
        return status;
    }

    auto algorithmName = requiredString(input, "algorithm");
    if (!algorithmName.isOk())
        return algorithmName.status();
    auto algorithm = encryptionFromName(*algorithmName);
    if (!algorithm.isOk())
        return algorithm.status();

    auto keyId = requiredString(input, "key_id");
    if (!keyId.isOk())
        return keyId.status();
    auto nonceHex = requiredString(input, "nonce_hex");
    if (!nonceHex.isOk())
        return nonceHex.status();
    auto tagHex = requiredString(input, "tag_hex");
    if (!tagHex.isOk())
        return tagHex.status();

    auto nonce = hexDecode(*nonceHex, "content envelope nonce", 64);
    if (!nonce.isOk())
        return nonce.status();
    auto tag = hexDecode(*tagHex, "content envelope tag", 64);
    if (!tag.isOk())
        return tag.status();
    if (nonce->size() != 12U)
        return Status::invalidArgument("content envelope AES-GCM nonce must be 12 bytes");
    if (tag->size() != 16U)
        return Status::invalidArgument("content envelope AES-GCM tag must be 16 bytes");

    return EnvelopeEncryption {
        .algorithm_ = *algorithm,
        .keyId_ = std::move(*keyId),
        .nonce_ = std::move(*nonce),
        .tag_ = std::move(*tag),
    };
}

[[nodiscard]] json encryptionToJson(const EnvelopeEncryption& encryption)
{
    return json {
        { "algorithm", kAesGcmName },
        { "key_id", encryption.keyId_ },
        { "nonce_hex", hexEncode(encryption.nonce_) },
        { "tag_hex", hexEncode(encryption.tag_) },
    };
}


} // namespace

std::string_view envelopeContentType() noexcept
{
    return kEnvelopeContentType;
}

Result<std::string> serializeEnvelope(const Envelope& envelope)
{
    if (auto status = requireContentEnvelopeVersion(envelope.version_); !status.isOk())
        return status;
    if (envelope.contentType_.empty())
        return Status::invalidArgument("content envelope content_type cannot be empty");
    if (envelope.encoding_.empty())
        return Status::invalidArgument("content envelope encoding cannot be empty");

    json checksum = nullptr;
    if (envelope.checksum_.has_value()) {
        if (envelope.checksum_->algorithm_.empty())
            return Status::invalidArgument("content envelope checksum algorithm cannot be empty");
        if (envelope.checksum_->valueHex_.empty())
            return Status::invalidArgument("content envelope checksum value cannot be empty");
        checksum = json {
            { "algorithm", envelope.checksum_->algorithm_ },
            { "value_hex", envelope.checksum_->valueHex_ },
        };
    }

    json encryption = nullptr;
    if (envelope.encryption_.has_value()) {
        if (envelope.encryption_->keyId_.empty())
            return Status::invalidArgument("content envelope encryption key_id cannot be empty");
        if (envelope.encryption_->nonce_.size() != 12U)
            return Status::invalidArgument("content envelope AES-GCM nonce must be 12 bytes");
        if (envelope.encryption_->tag_.size() != 16U)
            return Status::invalidArgument("content envelope AES-GCM tag must be 16 bytes");
        encryption = encryptionToJson(*envelope.encryption_);
    }

    json out {
        { "version", envelope.version_ },
        { "content_type", envelope.contentType_ },
        { "encoding", envelope.encoding_ },
        { "compression", {
                               { "algorithm", compressionNameJson(envelope.compression_) },
                               { "original_size", envelope.originalSize_ },
                           } },
        { "encryption", std::move(encryption) },
        { "checksum", std::move(checksum) },
        { "data_hex", hexEncode(envelope.data_) },
    };

    return out.dump();
}

Result<Envelope> deserializeEnvelope(std::string_view text)
{
    return deserializeEnvelope(text, JsonDecodeLimits {});
}

Result<Envelope> deserializeEnvelope(std::string_view text, const JsonDecodeLimits& limits)
{
    auto parsed = parseJsonWithLimits(text, "content envelope", limits);
    if (!parsed.isOk())
        return parsed.status();
    auto input = std::move(*parsed);

    if (!input.is_object())
        return Status::invalidArgument("content envelope must be a JSON object");
    if (auto status = rejectUnknownJsonFields(
            input,
            { "version", "content_type", "encoding", "compression", "encryption", "checksum", "data_hex" },
            "content envelope",
            limits);
        !status.isOk()) {
        return status;
    }

    auto version = requiredUnsigned(input, "version");
    if (!version.isOk())
        return version.status();
    if (*version > std::numeric_limits<std::uint32_t>::max())
        return Status::resourceExhausted("content envelope version is too large");
    if (auto status = requireContentEnvelopeVersion(static_cast<Version>(*version)); !status.isOk())
        return status;

    auto contentType = requiredString(input, "content_type");
    if (!contentType.isOk())
        return contentType.status();
    auto encoding = requiredString(input, "encoding");
    if (!encoding.isOk())
        return encoding.status();

    if (!input.contains("compression") || !input.at("compression").is_object())
        return Status::invalidArgument("content envelope compression is required");
    const auto& compressionJson = input.at("compression");
    if (auto status = rejectUnknownJsonFields(
            compressionJson,
            { "algorithm", "original_size" },
            "content envelope compression",
            limits);
        !status.isOk()) {
        return status;
    }
    auto compressionAlgorithm = requiredString(compressionJson, "algorithm");
    if (!compressionAlgorithm.isOk())
        return compressionAlgorithm.status();
    auto compression = compressionFromName(*compressionAlgorithm);
    if (!compression.isOk())
        return compression.status();
    auto originalSize = requiredUnsigned(compressionJson, "original_size");
    if (!originalSize.isOk())
        return originalSize.status();
    if (*originalSize > std::numeric_limits<std::size_t>::max())
        return Status::resourceExhausted("content envelope original size is too large");

    auto dataHex = requiredString(input, "data_hex");
    if (!dataHex.isOk())
        return dataHex.status();
    auto data = hexDecode(*dataHex, "content envelope data", limits.maxBytes_);
    if (!data.isOk())
        return data.status();

    Envelope envelope {
        .version_ = static_cast<std::uint32_t>(*version),
        .contentType_ = std::move(*contentType),
        .encoding_ = std::move(*encoding),
        .compression_ = *compression,
        .originalSize_ = static_cast<std::size_t>(*originalSize),
        .data_ = std::move(*data),
    };

    if (input.contains("checksum") && !input.at("checksum").is_null()) {
        if (!input.at("checksum").is_object())
            return Status::invalidArgument("content envelope checksum must be an object or null");
        const auto& checksumJson = input.at("checksum");
        if (auto status = rejectUnknownJsonFields(
                checksumJson,
                { "algorithm", "value_hex" },
                "content envelope checksum",
                limits);
            !status.isOk()) {
            return status;
        }
        auto algorithm = requiredString(checksumJson, "algorithm");
        if (!algorithm.isOk())
            return algorithm.status();
        auto value = requiredString(checksumJson, "value_hex");
        if (!value.isOk())
            return value.status();
        envelope.checksum_ = Checksum {
            .algorithm_ = std::move(*algorithm),
            .valueHex_ = std::move(*value),
        };
        if (envelope.checksum_->algorithm_ != kChecksumSha256)
            return Status::unimplemented("unsupported content envelope checksum algorithm");
        if (auto digestBytes = hexDecode(envelope.checksum_->valueHex_, "content envelope checksum", 64);
            !digestBytes.isOk()) {
            return digestBytes.status();
        } else if (digestBytes->size() != 32U) {
            return Status::invalidArgument("content envelope SHA-256 checksum must be 32 bytes");
        }
    }

    if (input.contains("encryption") && !input.at("encryption").is_null()) {
        auto encryption = encryptionFromJson(input.at("encryption"), limits);
        if (!encryption.isOk())
            return encryption.status();
        envelope.encryption_ = std::move(*encryption);
    }

    return envelope;
}

bool isEnvelopePayload(const Payload& payload) noexcept
{
    return payload.contentType_ == kEnvelopeContentType;
}

} // namespace lgc
