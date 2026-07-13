#pragma once

#include "foundation/compression/compressor.hpp"
#include "foundation/crypto/encryption.hpp"
#include "foundation/serialization/json_limits.hpp"
#include "foundation/serialization/state_codec.hpp"
#include "foundation/status/result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lc {

using Bytes = std::vector<std::uint8_t>;

struct Checksum {
    std::string algorithm_ { "sha256" };
    std::string valueHex_;
};

struct EnvelopeEncryption {
    EncryptionAlgorithm algorithm_ { EncryptionAlgorithm::AesGcm };
    std::string keyId_;
    Bytes nonce_;
    Bytes tag_;
};

struct Envelope {
    std::uint32_t version_ { 1 };
    std::string contentType_;
    std::string encoding_ { "utf-8" };
    CompressionAlgorithm compression_ { CompressionAlgorithm::None };
    std::size_t originalSize_ { 0 };
    std::optional<EnvelopeEncryption> encryption_;
    std::optional<Checksum> checksum_;
    Bytes data_;
};

struct EnvelopeOptions {
    CompressionOptions compression_;
    bool encrypt_ { false };
    EncryptionOptions encryption_;
    bool checksum_ { true };
    std::size_t maxDecodedBytes_ { 0 };
    JsonDecodeLimits decodeLimits_;
};

class EnvelopeCodec final {
public:
    EnvelopeCodec(
        std::shared_ptr<ICompressor> compressor = std::make_shared<Compressor>(),
        std::shared_ptr<IEncryptor> encryptor = nullptr);

    [[nodiscard]] Result<Envelope> wrap(
        const Payload& payload,
        const EnvelopeOptions& options = {}) const;

    [[nodiscard]] Result<Payload> unwrap(
        const Envelope& envelope,
        const EnvelopeOptions& options = {}) const;

    [[nodiscard]] Result<Payload> encode(
        const Payload& payload,
        const EnvelopeOptions& options = {}) const;

    [[nodiscard]] Result<Payload> decode(
        const Payload& payload,
        const EnvelopeOptions& options = {}) const;

private:
    std::shared_ptr<ICompressor> compressor_;
    std::shared_ptr<IEncryptor> encryptor_;
};

class EnvelopedStateCodec final : public IStateCodec {
public:
    EnvelopedStateCodec(
        std::shared_ptr<IStateCodec> inner,
        EnvelopeCodec envelope = EnvelopeCodec(),
        EnvelopeOptions options = {});

    [[nodiscard]] Result<Payload> encode(const State& state) const override;
    [[nodiscard]] Result<State> decode(const Payload& payload) const override;

private:
    std::shared_ptr<IStateCodec> inner_;
    EnvelopeCodec envelope_;
    EnvelopeOptions options_;
};

class EnvelopedCheckpointCodec final : public ICheckpointCodec {
public:
    EnvelopedCheckpointCodec(
        std::shared_ptr<ICheckpointCodec> inner,
        EnvelopeCodec envelope = EnvelopeCodec(),
        EnvelopeOptions options = {});

    [[nodiscard]] Result<Payload> encode(const Checkpoint& checkpoint) const override;
    [[nodiscard]] Result<Checkpoint> decode(const Payload& payload) const override;
    [[nodiscard]] Result<Payload> encodeWrite(const CheckpointWrite& write) const override;
    [[nodiscard]] Result<CheckpointWrite> decodeWrite(const Payload& payload) const override;

private:
    std::shared_ptr<ICheckpointCodec> inner_;
    EnvelopeCodec envelope_;
    EnvelopeOptions options_;
};

[[nodiscard]] std::string_view envelopeContentType() noexcept;
[[nodiscard]] Result<std::string> serializeEnvelope(const Envelope& envelope);
[[nodiscard]] Result<Envelope> deserializeEnvelope(std::string_view text);
[[nodiscard]] Result<Envelope> deserializeEnvelope(std::string_view text, const JsonDecodeLimits& limits);
[[nodiscard]] bool isEnvelopePayload(const Payload& payload) noexcept;

} // namespace lc
