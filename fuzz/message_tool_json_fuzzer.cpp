#include "fuzz_common.hpp"
#include "langgraph/message/message.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    try {
        const auto input = lgc::fuzz::inputToString(data, size);
        const auto value = lgc::fuzz::parseJsonOrDiscard(input);
        if (value.is_discarded())
            return 0;

        (void)lgc::normalizeContentBlocks(value);
        if (value.is_object()) {
            auto toolCall = lgc::toolCallFromJson(value);
            if (toolCall.isOk())
                (void)lgc::toolCallToJson(*toolCall);

            auto message = lgc::baseMessageFromJson(value);
            if (message.isOk()) {
                (void)lgc::baseMessageToJson(*message);
                (void)lgc::messageContentBlocks(*message);
            }
        }
        if (value.is_array()) {
            auto messages = lgc::messagesFromJson(value);
            if (messages.isOk())
                (void)lgc::messagesToJson(*messages);
        }
    } catch (...) {
    }
    return 0;
}
