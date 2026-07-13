#include "fuzz_common.hpp"
#include "langgraph/message/message.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    try {
        const auto input = lc::fuzz::inputToString(data, size);
        const auto value = lc::fuzz::parseJsonOrDiscard(input);
        if (value.is_discarded())
            return 0;

        (void)lc::normalizeContentBlocks(value);
        if (value.is_object()) {
            auto toolCall = lc::toolCallFromJson(value);
            if (toolCall.isOk())
                (void)lc::toolCallToJson(*toolCall);

            auto message = lc::baseMessageFromJson(value);
            if (message.isOk()) {
                (void)lc::baseMessageToJson(*message);
                (void)lc::messageContentBlocks(*message);
            }
        }
        if (value.is_array()) {
            auto messages = lc::messagesFromJson(value);
            if (messages.isOk())
                (void)lc::messagesToJson(*messages);
        }
    } catch (...) {
    }
    return 0;
}
