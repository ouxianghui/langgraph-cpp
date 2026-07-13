#include "foundation/serialization/content_envelope.hpp"

#include <cassert>
#include <chrono>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

int main()
{
    using namespace std::chrono_literals;

    lc::EnvelopeCodec pipeline;

    {
        lc::EnvelopeOptions options;
#if !LANGGRAPH_CPP_WITH_CRYPTO
        options.checksum_ = false;
#endif

        const lc::Payload payload {
            .contentType_ = "application/json",
            .data_ = R"({"message":"hello","step":1})",
        };

        auto encoded = pipeline.encode(payload, options);
        assert(encoded.isOk());
        assert(encoded->contentType_ == lc::envelopeContentType());
        assert(encoded->data_.find("content_type") != std::string::npos);

        auto envelope = lc::deserializeEnvelope(encoded->data_);
        assert(envelope.isOk());
        assert(envelope->contentType_ == payload.contentType_);
        assert(envelope->encoding_ == "utf-8");
        assert(envelope->compression_ == lc::CompressionAlgorithm::None);
        assert(envelope->originalSize_ == payload.data_.size());

        auto decoded = pipeline.decode(*encoded, options);
        assert(decoded.isOk());
        assert(decoded->contentType_ == payload.contentType_);
        assert(decoded->data_ == payload.data_);

#if LANGGRAPH_CPP_WITH_CRYPTO
        auto tamperedJson = nlohmann::json::parse(encoded->data_);
        auto dataHex = tamperedJson.at("data_hex").get<std::string>();
        dataHex.back() = dataHex.back() == '0' ? '1' : '0';
        tamperedJson["data_hex"] = dataHex;

        auto tampered = *encoded;
        tampered.data_ = tamperedJson.dump();
        auto tamperedResult = pipeline.decode(tampered, options);
        assert(!tamperedResult.isOk());
        assert(tamperedResult.status().code() == lc::StatusCode::DataLoss);
#endif

        auto unknownFieldJson = nlohmann::json::parse(encoded->data_);
        unknownFieldJson["unexpected"] = true;
        auto unknownField = lc::deserializeEnvelope(unknownFieldJson.dump());
        assert(!unknownField.isOk());
        assert(unknownField.status().code() == lc::StatusCode::InvalidArgument);

        lc::JsonDecodeLimits tinyLimits;
        tinyLimits.maxBytes_ = 8;
        auto tooLargeEnvelope = lc::deserializeEnvelope(encoded->data_, tinyLimits);
        assert(!tooLargeEnvelope.isOk());
        assert(tooLargeEnvelope.status().code() == lc::StatusCode::ResourceExhausted);
    }

    {
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

        lc::EnvelopeOptions options;
#if !LANGGRAPH_CPP_WITH_CRYPTO
        options.checksum_ = false;
#endif

        lc::EnvelopedCheckpointCodec codec(std::make_shared<lc::JsonCheckpointCodec>(), pipeline, options);
        auto encoded = codec.encode(checkpoint);
        assert(encoded.isOk());
        assert(encoded->contentType_ == lc::envelopeContentType());

        auto decoded = codec.decode(*encoded);
        assert(decoded.isOk());
        assert(decoded->threadId_ == checkpoint.threadId_);
        assert(decoded->checkpointId_ == checkpoint.checkpointId_);
        assert(decoded->state_ == checkpoint.state_);

        lc::CheckpointWrite write {
            .nodeId_ = "planner",
            .update_ = *lc::State::fromJson(R"({"messages":["write"]})"),
            .order_ = 0,
        };
        auto encodedWrite = codec.encodeWrite(write);
        assert(encodedWrite.isOk());
        assert(encodedWrite->contentType_ == lc::envelopeContentType());
        auto decodedWrite = codec.decodeWrite(*encodedWrite);
        assert(decodedWrite.isOk());
        assert(*decodedWrite == write);
    }

#if LANGGRAPH_CPP_HAS_ZLIB
    {
        const lc::Payload payload {
            .contentType_ = "application/json",
            .data_ = R"({"text":"checkpoint checkpoint checkpoint checkpoint checkpoint"})",
        };
        lc::EnvelopeOptions options {
            .compression_ = lc::CompressionOptions {
                .algorithm_ = lc::CompressionAlgorithm::Gzip,
                .level_ = 6,
            },
        };
#if !LANGGRAPH_CPP_WITH_CRYPTO
        options.checksum_ = false;
#endif

        auto encoded = pipeline.encode(payload, options);
        assert(encoded.isOk());
        auto envelope = lc::deserializeEnvelope(encoded->data_);
        assert(envelope.isOk());
        assert(envelope->compression_ == lc::CompressionAlgorithm::Gzip);

        auto decoded = pipeline.decode(*encoded, options);
        assert(decoded.isOk());
        assert(decoded->data_ == payload.data_);
    }
#endif

#if LANGGRAPH_CPP_WITH_CRYPTO
    {
        auto key = lc::AesGcm::generateKey();
        assert(key.isOk());

        auto encryptor = std::make_shared<lc::AesGcm>(std::move(*key), "envelope-key");
        lc::EnvelopeCodec encryptedPipeline(std::make_shared<lc::Compressor>(), encryptor);
        lc::EnvelopeOptions options {
            .encrypt_ = true,
            .encryption_ = lc::EncryptionOptions {
                .keyId_ = "envelope-key",
            },
        };

        const lc::Payload payload {
            .contentType_ = "application/json",
            .data_ = R"({"secret":"value","step":2})",
        };

        auto encoded = encryptedPipeline.encode(payload, options);
        assert(encoded.isOk());
        assert(encoded->data_.find("secret") == std::string::npos);
        assert(encoded->data_.find("ciphertext") == std::string::npos);

        auto envelope = lc::deserializeEnvelope(encoded->data_);
        assert(envelope.isOk());
        assert(envelope->encryption_.has_value());
        assert(envelope->encryption_->keyId_ == "envelope-key");
        assert(encoded->data_.find("aad_hex") == std::string::npos);

        auto decoded = encryptedPipeline.decode(*encoded, options);
        assert(decoded.isOk());
        assert(decoded->contentType_ == payload.contentType_);
        assert(decoded->data_ == payload.data_);

        auto tampered = *envelope;
        tampered.contentType_ = "application/json+tampered";
        auto tamperedResult = encryptedPipeline.unwrap(tampered, options);
        assert(!tamperedResult.isOk());
        assert(tamperedResult.status().code() == lc::StatusCode::Unauthenticated);
    }
#endif

    return 0;
}
