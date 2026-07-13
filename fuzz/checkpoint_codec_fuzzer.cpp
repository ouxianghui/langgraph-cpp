#include "fuzz_common.hpp"
#include "foundation/serialization/state_codec.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    try {
        const auto input = lc::fuzz::inputToString(data, size);
        const auto parts = lc::fuzz::splitInput(input, 2);
        const lc::JsonDecodeLimits limits {
            .maxBytes_ = 64 * 1024,
            .maxDepth_ = 32,
            .maxStringBytes_ = 16 * 1024,
            .maxArrayItems_ = 4096,
            .maxObjectFields_ = 4096,
            .maxNodes_ = 16 * 1024,
        };
        lc::JsonCheckpointCodec codec(limits);

        const lc::Payload checkpointPayload {
            .contentType_ = "application/vnd.langgraph.checkpoint+json",
            .data_ = parts[0],
        };
        auto checkpoint = codec.decode(checkpointPayload);
        if (checkpoint.isOk()) {
            auto encoded = codec.encode(*checkpoint);
            if (encoded.isOk())
                (void)codec.decode(*encoded);
        }

        const lc::Payload writePayload {
            .contentType_ = "application/vnd.langgraph.checkpoint.write+json",
            .data_ = parts[1],
        };
        auto write = codec.decodeWrite(writePayload);
        if (write.isOk()) {
            auto encoded = codec.encodeWrite(*write);
            if (encoded.isOk())
                (void)codec.decodeWrite(*encoded);
        }

        auto state = lc::State::fromJson(parts[0], limits);
        if (state.isOk()) {
            auto synthetic = codec.encode(lc::Checkpoint {
                .threadId_ = "fuzz-thread",
                .checkpointId_ = "fuzz-checkpoint",
                .step_ = 1,
                .state_ = *state,
                .createdAt_ = std::chrono::system_clock::now(),
            });
            if (synthetic.isOk())
                (void)codec.decode(*synthetic);
        }
    } catch (...) {
    }
    return 0;
}
