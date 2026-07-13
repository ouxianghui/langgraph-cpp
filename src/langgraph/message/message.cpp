#include "langgraph/message/message.hpp"

#include <algorithm>
#include <utility>

namespace lc {
namespace {

[[nodiscard]] std::string_view providerRoleName(MessageType type) noexcept
{
    switch (type) {
    case MessageType::System:
        return "system";
    case MessageType::Human:
        return "user";
    case MessageType::AI:
        return "assistant";
    case MessageType::Tool:
        return "tool";
    }
    return "user";
}

[[nodiscard]] Result<MessageType> messageTypeFromRole(std::string_view role)
{
    if (role == "system")
        return MessageType::System;
    if (role == "user" || role == "human")
        return MessageType::Human;
    if (role == "assistant" || role == "ai")
        return MessageType::AI;
    if (role == "tool")
        return MessageType::Tool;
    return Status::invalidArgument("unknown message role");
}

[[nodiscard]] nlohmann::json makeTextContentBlock(std::string text)
{
    return nlohmann::json {
        { "type", "text" },
        { "text", std::move(text) },
    };
}

[[nodiscard]] bool isKnownMultimodalBlock(std::string_view type) noexcept
{
    return type == "image"
        || type == "audio"
        || type == "video"
        || type == "file";
}

[[nodiscard]] Result<nlohmann::json> normalizeProviderContentBlock(const nlohmann::json& value)
{
    const auto type = value.at("type").get<std::string>();
    if (type == "image_url") {
        if (!value.contains("image_url"))
            return Status::invalidArgument("image_url content block requires image_url");
        const auto& image = value.at("image_url");
        nlohmann::json block {
            { "type", "image" },
        };
        if (image.is_string()) {
            block["url"] = image.get<std::string>();
        } else if (image.is_object() && image.contains("url") && image.at("url").is_string()) {
            block["url"] = image.at("url");
            nlohmann::json extras = image;
            extras.erase("url");
            if (!extras.empty())
                block["extras"] = std::move(extras);
        } else {
            return Status::invalidArgument("image_url content block requires image_url.url");
        }
        return block;
    }

    if (type == "input_audio") {
        if (!value.contains("input_audio") || !value.at("input_audio").is_object())
            return Status::invalidArgument("input_audio content block requires input_audio object");
        const auto& audio = value.at("input_audio");
        nlohmann::json block {
            { "type", "audio" },
        };
        if (audio.contains("data") && audio.at("data").is_string())
            block["base64"] = audio.at("data");
        if (audio.contains("format") && audio.at("format").is_string())
            block["mime_type"] = "audio/" + audio.at("format").get<std::string>();
        return block;
    }

    return value;
}

[[nodiscard]] Result<nlohmann::json> normalizeContentBlock(const nlohmann::json& value)
{
    if (!value.is_object())
        return Status::invalidArgument("content block must be a JSON object");
    if (!value.contains("type") || !value.at("type").is_string())
        return Status::invalidArgument("content block type is required");

    auto normalized = normalizeProviderContentBlock(value);
    if (!normalized.isOk())
        return normalized.status();

    auto block = std::move(*normalized);
    const auto type = block.at("type").get<std::string>();
    if (type == "text") {
        if (!block.contains("text") || !block.at("text").is_string())
            return Status::invalidArgument("text content block requires text");
        return block;
    }
    if (type == "reasoning") {
        if (block.contains("text") && !block.contains("reasoning"))
            block["reasoning"] = block.at("text");
        if (block.contains("reasoning") && !block.at("reasoning").is_string())
            return Status::invalidArgument("reasoning content block reasoning must be a string");
        return block;
    }
    if (isKnownMultimodalBlock(type)) {
        if (block.contains("base64")) {
            if (!block.at("base64").is_string())
                return Status::invalidArgument("multimodal content block base64 must be a string");
            if (!block.contains("mime_type") || !block.at("mime_type").is_string())
                return Status::invalidArgument("base64 multimodal content block requires mime_type");
        }
        for (const auto* key : { "url", "file_id", "id", "mime_type" }) {
            if (block.contains(key) && !block.at(key).is_string())
                return Status::invalidArgument("multimodal content block string field has invalid type");
        }
        return block;
    }
    if (type == "tool_call") {
        if (!block.contains("name") || !block.at("name").is_string())
            return Status::invalidArgument("tool_call content block requires name");
        if (!block.contains("args"))
            block["args"] = nlohmann::json::object();
        if (!block.at("args").is_object())
            return Status::invalidArgument("tool_call content block args must be an object");
        if (block.contains("id") && !block.at("id").is_string() && !block.at("id").is_null())
            return Status::invalidArgument("tool_call content block id must be a string or null");
        return block;
    }
    if (type == "tool_call_chunk") {
        if (block.contains("id") && !block.at("id").is_string() && !block.at("id").is_null())
            return Status::invalidArgument("tool_call_chunk id must be a string or null");
        if (block.contains("name") && !block.at("name").is_string() && !block.at("name").is_null())
            return Status::invalidArgument("tool_call_chunk name must be a string or null");
        if (block.contains("args") && !block.at("args").is_string())
            return Status::invalidArgument("tool_call_chunk args must be a string");
        if (block.contains("index") && !block.at("index").is_number_unsigned() && !block.at("index").is_null())
            return Status::invalidArgument("tool_call_chunk index must be unsigned or null");
        return block;
    }

    // Preserve future LangChain/provider block types as typed JSON objects.
    return block;
}

[[nodiscard]] nlohmann::json makeContentBlocksFromText(std::string_view text)
{
    nlohmann::json blocks = nlohmann::json::array();
    if (!text.empty())
        blocks.push_back(makeTextContentBlock(std::string(text)));
    return blocks;
}

} // namespace

BaseMessage BaseMessage::system(std::string content)
{
    auto blocks = makeContentBlocksFromText(content);
    return BaseMessage {
        .type_ = MessageType::System,
        .content_ = std::move(content),
        .contentBlocks_ = std::move(blocks),
    };
}

BaseMessage BaseMessage::human(std::string content)
{
    auto blocks = makeContentBlocksFromText(content);
    return BaseMessage {
        .type_ = MessageType::Human,
        .content_ = std::move(content),
        .contentBlocks_ = std::move(blocks),
    };
}

BaseMessage BaseMessage::ai(std::string content, std::vector<ToolCall> toolCalls)
{
    auto blocks = makeContentBlocksFromText(content);
    return BaseMessage {
        .type_ = MessageType::AI,
        .content_ = std::move(content),
        .contentBlocks_ = std::move(blocks),
        .toolCalls_ = std::move(toolCalls),
    };
}

BaseMessage BaseMessage::tool(std::string toolCallId, std::string name, std::string content)
{
    auto blocks = makeContentBlocksFromText(content);
    return BaseMessage {
        .type_ = MessageType::Tool,
        .content_ = std::move(content),
        .contentBlocks_ = std::move(blocks),
        .toolCallId_ = std::move(toolCallId),
        .name_ = std::move(name),
    };
}

std::string_view messageTypeName(MessageType type) noexcept
{
    switch (type) {
    case MessageType::System:
        return "system";
    case MessageType::Human:
        return "human";
    case MessageType::AI:
        return "ai";
    case MessageType::Tool:
        return "tool";
    }
    return "human";
}

Result<MessageType> messageTypeFromName(std::string_view name)
{
    if (name == "system")
        return MessageType::System;
    if (name == "human")
        return MessageType::Human;
    if (name == "ai")
        return MessageType::AI;
    if (name == "tool")
        return MessageType::Tool;
    return Status::invalidArgument("unknown message type");
}

nlohmann::json toolCallToJson(const ToolCall& toolCall)
{
    return nlohmann::json {
        { "id", toolCall.id_ },
        { "name", toolCall.name_ },
        { "args", toolCall.args_ },
    };
}

Result<ToolCall> toolCallFromJson(const nlohmann::json& value)
{
    if (!value.is_object())
        return Status::invalidArgument("tool call must be a JSON object");
    if (!value.contains("id") || !value.at("id").is_string())
        return Status::invalidArgument("tool call id is required");
    if (!value.contains("name") || !value.at("name").is_string())
        return Status::invalidArgument("tool call name is required");
    if (!value.contains("args"))
        return Status::invalidArgument("tool call args are required");
    if (!value.at("args").is_object())
        return Status::invalidArgument("tool call args must be a JSON object");

    ToolCall toolCall {
        .id_ = value.at("id").get<std::string>(),
        .name_ = value.at("name").get<std::string>(),
        .args_ = value.at("args"),
    };
    if (toolCall.id_.empty())
        return Status::invalidArgument("tool call id cannot be empty");
    if (toolCall.name_.empty())
        return Status::invalidArgument("tool call name cannot be empty");
    return toolCall;
}

nlohmann::json textContentBlock(std::string text)
{
    return makeTextContentBlock(std::move(text));
}

nlohmann::json contentBlocksFromText(std::string_view text)
{
    return makeContentBlocksFromText(text);
}

nlohmann::json toolCallContentBlock(const ToolCall& toolCall)
{
    return nlohmann::json {
        { "type", "tool_call" },
        { "id", toolCall.id_.empty() ? nlohmann::json(nullptr) : nlohmann::json(toolCall.id_) },
        { "name", toolCall.name_ },
        { "args", toolCall.args_ },
    };
}

Result<nlohmann::json> normalizeContentBlocks(const nlohmann::json& value)
{
    if (!value.is_array())
        return Status::invalidArgument("content_blocks must be an array");

    nlohmann::json blocks = nlohmann::json::array();
    for (const auto& item : value) {
        auto block = normalizeContentBlock(item);
        if (!block.isOk())
            return block.status();
        blocks.push_back(std::move(*block));
    }
    return blocks;
}

std::string contentBlocksText(const nlohmann::json& blocks)
{
    if (!blocks.is_array())
        return {};

    std::string text;
    for (const auto& block : blocks) {
        if (!block.is_object()
            || !block.contains("type")
            || block.at("type") != "text"
            || !block.contains("text")
            || !block.at("text").is_string()) {
            continue;
        }
        text += block.at("text").get<std::string>();
    }
    return text;
}

nlohmann::json messageContentBlocks(const BaseMessage& message)
{
    nlohmann::json blocks = message.contentBlocks_.is_array() && !message.contentBlocks_.empty()
        ? message.contentBlocks_
        : contentBlocksFromText(message.content_);
    if (message.type_ == MessageType::AI) {
        bool hasToolCallBlock = false;
        for (const auto& block : blocks) {
            if (block.is_object()
                && block.contains("type")
                && block.at("type") == "tool_call") {
                hasToolCallBlock = true;
                break;
            }
        }
        if (!hasToolCallBlock) {
            for (const auto& toolCall : message.toolCalls_)
                blocks.push_back(toolCallContentBlock(toolCall));
        }
    }
    return blocks;
}

nlohmann::json baseMessageToJson(const BaseMessage& message)
{
    nlohmann::json value {
        { "type", messageTypeName(message.type_) },
        { "role", providerRoleName(message.type_) },
        { "content", message.content_.empty() ? contentBlocksText(message.contentBlocks_) : message.content_ },
    };

    auto blocks = messageContentBlocks(message);
    if (!blocks.empty())
        value["content_blocks"] = std::move(blocks);
    if (!message.id_.empty())
        value["id"] = message.id_;
    if (!message.toolCalls_.empty()) {
        auto toolCalls = nlohmann::json::array();
        for (const auto& toolCall : message.toolCalls_)
            toolCalls.push_back(toolCallToJson(toolCall));
        value["tool_calls"] = std::move(toolCalls);
    }
    if (!message.toolCallId_.empty())
        value["tool_call_id"] = message.toolCallId_;
    if (!message.name_.empty())
        value["name"] = message.name_;
    if (!message.usageMetadata_.is_null())
        value["usage_metadata"] = message.usageMetadata_;
    if (!message.responseMetadata_.is_null())
        value["response_metadata"] = message.responseMetadata_;
    if (!message.artifact_.is_null())
        value["artifact"] = message.artifact_;
    return value;
}

Result<BaseMessage> baseMessageFromJson(const nlohmann::json& value)
{
    if (!value.is_object())
        return Status::invalidArgument("message must be a JSON object");
    if (!value.contains("type") && !value.contains("role"))
        return Status::invalidArgument("message type or role is required");
    if (value.contains("type") && !value.at("type").is_string())
        return Status::invalidArgument("message type must be a string");
    if (value.contains("role") && !value.at("role").is_string())
        return Status::invalidArgument("message role must be a string");
    if (value.contains("content")
        && !value.at("content").is_string()
        && !value.at("content").is_array()
        && !value.at("content").is_null()) {
        return Status::invalidArgument("message content must be a string, array, or null");
    }
    if (value.contains("content_blocks") && !value.at("content_blocks").is_array())
        return Status::invalidArgument("message content_blocks must be an array");

    Result<MessageType> type = value.contains("type")
        ? messageTypeFromName(value.at("type").get<std::string>())
        : messageTypeFromRole(value.at("role").get<std::string>());
    if (!type.isOk())
        return type.status();

    std::string content;
    nlohmann::json blocks = nlohmann::json::array();
    if (value.contains("content_blocks")) {
        auto normalized = normalizeContentBlocks(value.at("content_blocks"));
        if (!normalized.isOk())
            return normalized.status();
        blocks = std::move(*normalized);
    } else if (value.contains("content") && value.at("content").is_array()) {
        auto normalized = normalizeContentBlocks(value.at("content"));
        if (!normalized.isOk())
            return normalized.status();
        blocks = std::move(*normalized);
    }

    if (value.contains("content") && value.at("content").is_string()) {
        content = value.at("content").get<std::string>();
        if (blocks.empty())
            blocks = contentBlocksFromText(content);
    } else if (!blocks.empty()) {
        content = contentBlocksText(blocks);
    }

    BaseMessage message {
        .type_ = *type,
        .content_ = std::move(content),
        .contentBlocks_ = std::move(blocks),
    };

    if (value.contains("id")) {
        if (!value.at("id").is_string())
            return Status::invalidArgument("message id must be a string");
        message.id_ = value.at("id").get<std::string>();
    }
    if (value.contains("tool_calls")) {
        if (!value.at("tool_calls").is_array())
            return Status::invalidArgument("message tool_calls must be an array");
        for (const auto& item : value.at("tool_calls")) {
            auto toolCall = toolCallFromJson(item);
            if (!toolCall.isOk())
                return toolCall.status();
            message.toolCalls_.push_back(std::move(*toolCall));
        }
    }
    if (message.type_ == MessageType::AI && message.contentBlocks_.is_array() && !message.contentBlocks_.empty()) {
        nlohmann::json filteredBlocks = nlohmann::json::array();
        for (const auto& block : message.contentBlocks_) {
            if (!block.is_object() || !block.contains("type") || block.at("type") != "tool_call") {
                filteredBlocks.push_back(block);
                continue;
            }
            if (!block.contains("id") || !block.at("id").is_string()
                || !block.contains("name") || !block.at("name").is_string()
                || !block.contains("args") || !block.at("args").is_object()) {
                return Status::invalidArgument("tool_call content block requires id, name, and object args");
            }
            const auto id = block.at("id").get<std::string>();
            const auto name = block.at("name").get<std::string>();
            const auto exists = std::ranges::any_of(message.toolCalls_, [&](const ToolCall& toolCall) {
                return toolCall.id_ == id && toolCall.name_ == name;
            });
            if (!exists) {
                message.toolCalls_.push_back(ToolCall {
                    .id_ = id,
                    .name_ = name,
                    .args_ = block.at("args"),
                });
            }
        }
        message.contentBlocks_ = std::move(filteredBlocks);
    }
    if (value.contains("tool_call_id")) {
        if (!value.at("tool_call_id").is_string())
            return Status::invalidArgument("message tool_call_id must be a string");
        message.toolCallId_ = value.at("tool_call_id").get<std::string>();
    }
    if (value.contains("name")) {
        if (!value.at("name").is_string())
            return Status::invalidArgument("message name must be a string");
        message.name_ = value.at("name").get<std::string>();
    }
    if (value.contains("usage_metadata"))
        message.usageMetadata_ = value.at("usage_metadata");
    if (value.contains("response_metadata"))
        message.responseMetadata_ = value.at("response_metadata");
    if (value.contains("artifact"))
        message.artifact_ = value.at("artifact");

    if (message.type_ == MessageType::Tool && message.toolCallId_.empty())
        return Status::invalidArgument("tool message requires tool_call_id");
    if (message.content_.empty()
        && message.contentBlocks_.empty()
        && message.toolCalls_.empty()
        && message.type_ != MessageType::AI) {
        return Status::invalidArgument("message content is required");
    }
    return message;
}

nlohmann::json messagesToJson(const std::vector<BaseMessage>& messages)
{
    auto value = nlohmann::json::array();
    for (const auto& message : messages)
        value.push_back(baseMessageToJson(message));
    return value;
}

Result<std::vector<BaseMessage>> messagesFromJson(const nlohmann::json& value)
{
    if (!value.is_array())
        return Status::invalidArgument("messages must be a JSON array");

    std::vector<BaseMessage> messages;
    messages.reserve(value.size());
    for (const auto& item : value) {
        auto message = baseMessageFromJson(item);
        if (!message.isOk())
            return message.status();
        messages.push_back(std::move(*message));
    }
    return messages;
}

Result<std::vector<BaseMessage>> messagesFromStateJson(
    const nlohmann::json& state,
    std::string_view field)
{
    if (!state.is_object())
        return Status::invalidArgument("state must be a JSON object");
    const auto key = std::string(field);
    if (!state.contains(key))
        return std::vector<BaseMessage> {};
    return messagesFromJson(state.at(key));
}

} // namespace lc
