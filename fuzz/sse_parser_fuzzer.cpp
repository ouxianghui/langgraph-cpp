#include "fuzz_common.hpp"
#include "foundation/network/sse_parser.hh"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    try {
        const auto input = lc::fuzz::inputToString(data, size);
        lc::SseParser parser;
        std::size_t offset = 0;
        while (offset < input.size()) {
            const auto raw = static_cast<unsigned char>(input[offset]);
            const std::size_t chunkSize = std::min<std::size_t>(
                input.size() - offset,
                1U + (raw % 23U));
            const auto chunk = std::string_view(input).substr(offset, chunkSize);
            auto status = parser.feed(
                chunk,
                [](const lc::ServerSentEvent&) {
                    return lc::Status::ok();
                });
            if (!status.isOk())
                return 0;
            offset += chunkSize;
        }
        (void)parser.finish(
            [](const lc::ServerSentEvent&) {
                return lc::Status::ok();
            });
    } catch (...) {
    }
    return 0;
}
