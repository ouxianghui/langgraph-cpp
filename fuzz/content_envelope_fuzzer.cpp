#include "fuzz_common.hpp"
#include "foundation/serialization/content_envelope.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    try {
        const auto input = lc::fuzz::inputToString(data, size);
        const auto parts = lc::fuzz::splitInput(input, 2);
        lc::EnvelopeCodec codec;
        const lc::EnvelopeOptions options {
            .checksum_ = true,
            .maxDecodedBytes_ = 128 * 1024,
            .decodeLimits_ = {
                .maxBytes_ = 128 * 1024,
                .maxDepth_ = 32,
                .maxStringBytes_ = 32 * 1024,
                .maxArrayItems_ = 4096,
                .maxObjectFields_ = 4096,
                .maxNodes_ = 32 * 1024,
            },
        };

        auto envelope = lc::deserializeEnvelope(parts[0], options.decodeLimits_);
        if (envelope.isOk())
            (void)codec.unwrap(*envelope, options);

        auto decoded = codec.decode(
            lc::Payload {
                .contentType_ = std::string(lc::envelopeContentType()),
                .data_ = parts[0],
            },
            options);
        (void)decoded;

        auto wrapped = codec.encode(
            lc::Payload {
                .contentType_ = "application/octet-stream",
                .data_ = parts[1],
            },
            options);
        if (wrapped.isOk()) {
            (void)lc::deserializeEnvelope(wrapped->data_, options.decodeLimits_);
            (void)codec.decode(*wrapped, options);
        }
    } catch (...) {
    }
    return 0;
}
