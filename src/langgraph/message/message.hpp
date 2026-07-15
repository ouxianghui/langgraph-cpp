#pragma once

#include "foundation/status/result.hpp"
#include "langgraph/message/usage_metadata.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lc {

/// LangChain-style message type used by model and tool nodes.
enum class MessageType : std::uint8_t {
    System,
    Human,
    AI,
    Tool,
};

/// Structured request from an assistant message to invoke a registered tool.
struct ToolCall {
    std::string id_;
    std::string name_;
    nlohmann::json args_ { nlohmann::json::object() };

    friend bool operator==(const ToolCall&, const ToolCall&) = default;
};

/// Portable message format stored in state as JSON.
struct BaseMessage {
    /// Optional stable id used by add_messages to replace existing messages.
    std::string id_;
    MessageType type_ { MessageType::Human };
    std::string content_;
    /// LangChain standard content blocks. Empty means derive from content_.
    nlohmann::json contentBlocks_ { nlohmann::json::array() };
    /// AIMessage-only tool call requests.
    std::vector<ToolCall> toolCalls_;
    /// Tool-only call id this message answers.
    std::string toolCallId_;
    /// Tool-only tool name.
    std::string name_;
    /// AIMessage usage metadata, normalized across providers when available.
    UsageMetadata usageMetadata_;
    /// Provider response metadata, when supplied by a provider.
    nlohmann::json responseMetadata_ = nullptr;
    /// ToolMessage artifact payload kept out of string content.
    nlohmann::json artifact_ = nullptr;

    [[nodiscard]] static BaseMessage system(std::string content);
    [[nodiscard]] static BaseMessage human(std::string content);
    [[nodiscard]] static BaseMessage ai(std::string content, std::vector<ToolCall> toolCalls = {});
    [[nodiscard]] static BaseMessage tool(std::string toolCallId, std::string name, std::string content);

    friend bool operator==(const BaseMessage&, const BaseMessage&) = default;
};

[[nodiscard]] std::string_view messageTypeName(MessageType type) noexcept;
[[nodiscard]] Result<MessageType> messageTypeFromName(std::string_view name);

[[nodiscard]] nlohmann::json toolCallToJson(const ToolCall& toolCall);
[[nodiscard]] Result<ToolCall> toolCallFromJson(const nlohmann::json& value);

[[nodiscard]] nlohmann::json textContentBlock(std::string text);
[[nodiscard]] nlohmann::json contentBlocksFromText(std::string_view text);
[[nodiscard]] nlohmann::json toolCallContentBlock(const ToolCall& toolCall);
[[nodiscard]] Result<nlohmann::json> normalizeContentBlocks(const nlohmann::json& value);
[[nodiscard]] std::string contentBlocksText(const nlohmann::json& blocks);
[[nodiscard]] nlohmann::json messageContentBlocks(const BaseMessage& message);

[[nodiscard]] nlohmann::json baseMessageToJson(const BaseMessage& message);
[[nodiscard]] Result<BaseMessage> baseMessageFromJson(const nlohmann::json& value);

[[nodiscard]] nlohmann::json messagesToJson(const std::vector<BaseMessage>& messages);
[[nodiscard]] Result<std::vector<BaseMessage>> messagesFromJson(const nlohmann::json& value);

[[nodiscard]] Result<std::vector<BaseMessage>> messagesFromStateJson(
    const nlohmann::json& state,
    std::string_view field = "messages");

} // namespace lc
