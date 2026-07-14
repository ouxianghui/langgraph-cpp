#include "langgraph/model/provider_chat_model.hpp"

#include <algorithm>
#include <exception>
#include <map>
#include <optional>
#include <utility>

namespace lc {
namespace {

[[nodiscard]] Result<void> validateOptions(const ProviderChatModelOptions& options)
{
    if (!options.httpClient_)
        return Status::invalidArgument("provider chat model requires an HTTP client");
    if (options.model_.empty())
        return Status::invalidArgument("provider chat model requires a model name");
    if (options.path_.empty() || options.path_.front() != '/')
        return Status::invalidArgument("provider chat model path must start with '/'");
    if (!options.extraRequestFields_.is_object())
        return Status::invalidArgument("provider chat model extra request fields must be an object");
    return okResult();
}

[[nodiscard]] Result<nlohmann::json> parseJsonResult(std::string_view text, std::string_view context)
{
    try {
        return nlohmann::json::parse(text);
    } catch (const std::exception& ex) {
        return Status::invalidArgument(std::string(context) + " JSON parse failed: " + ex.what());
    }
}

[[nodiscard]] bool httpStatusOk(int statusCode) noexcept
{
    return statusCode >= 200 && statusCode < 300;
}

[[nodiscard]] std::size_t messageBytes(const BaseMessage& message)
{
    auto total = message.content_.size()
        + message.contentBlocks_.dump().size()
        + message.toolCallId_.size()
        + message.name_.size()
        + messageTypeName(message.type_).size();
    for (const auto& toolCall : message.toolCalls_)
        total += toolCall.id_.size() + toolCall.name_.size() + toolCall.args_.dump().size();
    return total;
}

[[nodiscard]] nlohmann::json modelInputContentBlocks(const BaseMessage& message)
{
    nlohmann::json blocks = message.contentBlocks_.is_array() && !message.contentBlocks_.empty()
        ? message.contentBlocks_
        : contentBlocksFromText(message.content_);
    nlohmann::json filtered = nlohmann::json::array();
    for (const auto& block : blocks) {
        if (!block.is_object() || !block.contains("type")) {
            filtered.push_back(block);
            continue;
        }
        const auto type = block.at("type").is_string()
            ? block.at("type").get<std::string>()
            : std::string {};
        if (type == "tool_call" || type == "tool_call_chunk")
            continue;
        filtered.push_back(block);
    }
    return filtered;
}

[[nodiscard]] bool hasNonTextBlock(const nlohmann::json& blocks)
{
    if (!blocks.is_array())
        return false;
    for (const auto& block : blocks) {
        if (!block.is_object()
            || !block.contains("type")
            || block.at("type") != "text") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] nlohmann::json openAIContentBlock(const nlohmann::json& block)
{
    if (!block.is_object() || !block.contains("type") || !block.at("type").is_string())
        return block;
    const auto type = block.at("type").get<std::string>();
    if (type == "text")
        return nlohmann::json { { "type", "text" }, { "text", block.value("text", std::string {}) } };
    if (type == "image") {
        if (block.contains("url") && block.at("url").is_string()) {
            return nlohmann::json {
                { "type", "image_url" },
                { "image_url", { { "url", block.at("url") } } },
            };
        }
        if (block.contains("base64") && block.at("base64").is_string()) {
            const auto mime = block.value("mime_type", std::string("image/png"));
            return nlohmann::json {
                { "type", "image_url" },
                { "image_url", { { "url", "data:" + mime + ";base64," + block.at("base64").get<std::string>() } } },
            };
        }
    }
    if (type == "audio" && block.contains("base64") && block.at("base64").is_string()) {
        return nlohmann::json {
            { "type", "input_audio" },
            { "input_audio", {
                { "data", block.at("base64") },
                { "format", block.value("mime_type", std::string("audio/wav")) },
            } },
        };
    }
    return block;
}

[[nodiscard]] nlohmann::json openAIContentFromMessage(const BaseMessage& message)
{
    const auto blocks = modelInputContentBlocks(message);
    if (!hasNonTextBlock(blocks))
        return contentBlocksText(blocks);

    nlohmann::json content = nlohmann::json::array();
    for (const auto& block : blocks)
        content.push_back(openAIContentBlock(block));
    return content;
}

[[nodiscard]] nlohmann::json anthropicContentBlock(const nlohmann::json& block)
{
    if (!block.is_object() || !block.contains("type") || !block.at("type").is_string())
        return block;
    const auto type = block.at("type").get<std::string>();
    if (type == "text")
        return nlohmann::json { { "type", "text" }, { "text", block.value("text", std::string {}) } };
    if (type == "image" && block.contains("base64") && block.at("base64").is_string()) {
        return nlohmann::json {
            { "type", "image" },
            { "source", {
                { "type", "base64" },
                { "media_type", block.value("mime_type", std::string("image/png")) },
                { "data", block.at("base64") },
            } },
        };
    }
    if (type == "image" && block.contains("url") && block.at("url").is_string()) {
        return nlohmann::json {
            { "type", "image" },
            { "source", {
                { "type", "url" },
                { "url", block.at("url") },
            } },
        };
    }
    return block;
}

[[nodiscard]] nlohmann::json anthropicContentFromMessage(const BaseMessage& message)
{
    const auto blocks = modelInputContentBlocks(message);
    if (!hasNonTextBlock(blocks))
        return contentBlocksText(blocks);

    nlohmann::json content = nlohmann::json::array();
    for (const auto& block : blocks)
        content.push_back(anthropicContentBlock(block));
    return content;
}

[[nodiscard]] nlohmann::json openAIMessagesJson(const std::vector<BaseMessage>& messages)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& message : messages) {
        nlohmann::json item {
            { "content", openAIContentFromMessage(message) },
        };
        switch (message.type_) {
        case MessageType::System:
            item["role"] = "system";
            break;
        case MessageType::Human:
            item["role"] = "user";
            break;
        case MessageType::AI:
            item["role"] = "assistant";
            break;
        case MessageType::Tool:
            item["role"] = "tool";
            if (!message.toolCallId_.empty())
                item["tool_call_id"] = message.toolCallId_;
            if (!message.name_.empty())
                item["name"] = message.name_;
            break;
        }

        if (!message.toolCalls_.empty()) {
            auto toolCalls = nlohmann::json::array();
            for (const auto& toolCall : message.toolCalls_) {
                toolCalls.push_back({
                    { "id", toolCall.id_ },
                    { "type", "function" },
                    { "function", {
                          { "name", toolCall.name_ },
                          { "arguments", toolCall.args_.dump() },
                      } },
                });
            }
            item["tool_calls"] = std::move(toolCalls);
        }
        out.push_back(std::move(item));
    }
    return out;
}

[[nodiscard]] nlohmann::json anthropicMessagesJson(const std::vector<BaseMessage>& messages, std::string& systemPrompt)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& message : messages) {
        if (message.type_ == MessageType::System) {
            if (!systemPrompt.empty())
                systemPrompt.push_back('\n');
            systemPrompt += message.content_;
            continue;
        }

        auto role = message.type_ == MessageType::AI ? "assistant" : "user";
        out.push_back({
            { "role", role },
            { "content", anthropicContentFromMessage(message) },
        });
    }
    return out;
}

[[nodiscard]] nlohmann::json commonMetadata(
    ChatProviderKind provider,
    std::string_view model,
    nlohmann::json usage = nlohmann::json::object())
{
    nlohmann::json metadata {
        { "provider", chatProviderName(provider) },
        { "model", model },
    };
    if (!usage.empty())
        metadata["usage"] = std::move(usage);
    return metadata;
}

[[nodiscard]] nlohmann::json openAIToolsJson(const std::vector<ChatModelTool>& tools)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& tool : tools) {
        out.push_back({
            { "type", "function" },
            { "function", {
                  { "name", tool.name_ },
                  { "description", tool.description_ },
                  { "parameters", tool.inputSchema_.rawJson() },
              } },
        });
    }
    return out;
}

[[nodiscard]] nlohmann::json anthropicToolsJson(const std::vector<ChatModelTool>& tools)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& tool : tools) {
        out.push_back({
            { "name", tool.name_ },
            { "description", tool.description_ },
            { "input_schema", tool.inputSchema_.rawJson() },
        });
    }
    return out;
}

[[nodiscard]] Result<nlohmann::json> parseToolArguments(
    const nlohmann::json& value,
    std::string_view context)
{
    if (value.is_object())
        return value;
    if (value.is_null())
        return nlohmann::json::object();
    if (!value.is_string())
        return Status::invalidArgument(std::string(context) + " arguments must be object or JSON string");
    if (value.get<std::string>().empty())
        return nlohmann::json::object();
    return parseJsonResult(value.get<std::string>(), context);
}

[[nodiscard]] Result<BaseMessage> parseOpenAIMessage(const nlohmann::json& body)
{
    if (!body.contains("choices") || !body.at("choices").is_array() || body.at("choices").empty())
        return Status::invalidArgument("OpenAI-compatible response requires choices[0]");
    const auto& choice = body.at("choices").at(0);
    std::vector<ToolCall> toolCalls;
    if (choice.contains("message") && choice.at("message").is_object()) {
        const auto& message = choice.at("message");
        if (message.contains("tool_calls")) {
            if (!message.at("tool_calls").is_array())
                return Status::invalidArgument("OpenAI-compatible tool_calls must be an array");
            for (const auto& item : message.at("tool_calls")) {
                if (!item.is_object())
                    return Status::invalidArgument("OpenAI-compatible tool call must be an object");
                if (!item.contains("function") || !item.at("function").is_object())
                    return Status::invalidArgument("OpenAI-compatible tool call requires function");
                const auto& function = item.at("function");
                if (!function.contains("name") || !function.at("name").is_string())
                    return Status::invalidArgument("OpenAI-compatible tool call requires function.name");
                auto arguments = parseToolArguments(
                    function.value("arguments", nlohmann::json::object()),
                    "OpenAI-compatible tool call");
                if (!arguments.isOk())
                    return arguments.status();
                toolCalls.push_back(ToolCall {
                    .id_ = item.value("id", std::string {}),
                    .name_ = function.at("name").get<std::string>(),
                    .args_ = std::move(*arguments),
                });
            }
        }

        std::string content;
        nlohmann::json contentBlocks = nlohmann::json::array();
        if (message.contains("content")) {
            if (message.at("content").is_string()) {
                content = message.at("content").get<std::string>();
                contentBlocks = contentBlocksFromText(content);
            } else if (message.at("content").is_array()) {
                auto normalized = normalizeContentBlocks(message.at("content"));
                if (!normalized.isOk())
                    return normalized.status();
                contentBlocks = std::move(*normalized);
                content = contentBlocksText(contentBlocks);
            } else if (!message.at("content").is_null()) {
                return Status::invalidArgument("OpenAI-compatible message content must be string, array, or null");
            }
        }
        auto ai = BaseMessage::ai(std::move(content), std::move(toolCalls));
        ai.contentBlocks_ = std::move(contentBlocks);
        return ai;
    }
    if (choice.contains("text") && choice.at("text").is_string())
        return BaseMessage::ai(choice.at("text").get<std::string>());
    return Status::invalidArgument("OpenAI-compatible response missing message content");
}

[[nodiscard]] Result<BaseMessage> parseAnthropicMessage(const nlohmann::json& body)
{
    if (!body.contains("content") || !body.at("content").is_array())
        return Status::invalidArgument("Anthropic response requires content array");

    std::string content;
    nlohmann::json contentBlocks = nlohmann::json::array();
    std::vector<ToolCall> toolCalls;
    for (const auto& item : body.at("content")) {
        if (item.is_object()
            && (!item.contains("type") || item.at("type") == "text")
            && item.contains("text")
            && item.at("text").is_string()) {
            const auto text = item.at("text").get<std::string>();
            content += text;
            contentBlocks.push_back(textContentBlock(text));
        } else if (item.is_object()
            && item.contains("type")
            && (item.at("type") == "thinking" || item.at("type") == "reasoning")) {
            nlohmann::json block {
                { "type", "reasoning" },
            };
            if (item.contains("thinking") && item.at("thinking").is_string())
                block["reasoning"] = item.at("thinking");
            if (item.contains("text") && item.at("text").is_string())
                block["reasoning"] = item.at("text");
            if (item.contains("signature") && item.at("signature").is_string())
                block["extras"] = { { "signature", item.at("signature") } };
            contentBlocks.push_back(std::move(block));
        } else if (item.is_object()
            && item.contains("type")
            && item.at("type") == "tool_use") {
            if (!item.contains("name") || !item.at("name").is_string())
                return Status::invalidArgument("Anthropic tool_use requires name");
            toolCalls.push_back(ToolCall {
                .id_ = item.value("id", std::string {}),
                .name_ = item.at("name").get<std::string>(),
                .args_ = item.value("input", nlohmann::json::object()),
            });
        }
    }
    auto ai = BaseMessage::ai(std::move(content), std::move(toolCalls));
    ai.contentBlocks_ = std::move(contentBlocks);
    return ai;
}

[[nodiscard]] std::optional<std::string> openAIStreamDelta(const nlohmann::json& body)
{
    if (!body.contains("choices") || !body.at("choices").is_array() || body.at("choices").empty())
        return std::nullopt;
    const auto& choice = body.at("choices").at(0);
    if (!choice.contains("delta") || !choice.at("delta").is_object())
        return std::nullopt;
    const auto& delta = choice.at("delta");
    if (!delta.contains("content") || !delta.at("content").is_string())
        return std::nullopt;
    return delta.at("content").get<std::string>();
}

[[nodiscard]] std::optional<std::string> anthropicStreamDelta(const nlohmann::json& body)
{
    if (!body.contains("type") || !body.at("type").is_string())
        return std::nullopt;
    if (body.at("type") != "content_block_delta")
        return std::nullopt;
    if (!body.contains("delta") || !body.at("delta").is_object())
        return std::nullopt;
    const auto& delta = body.at("delta");
    if (!delta.contains("text") || !delta.at("text").is_string())
        return std::nullopt;
    return delta.at("text").get<std::string>();
}

[[nodiscard]] std::optional<std::string> anthropicReasoningDelta(const nlohmann::json& body)
{
    if (!body.contains("type") || !body.at("type").is_string())
        return std::nullopt;
    if (body.at("type") != "content_block_delta")
        return std::nullopt;
    if (!body.contains("delta") || !body.at("delta").is_object())
        return std::nullopt;
    const auto& delta = body.at("delta");
    if (!delta.contains("type") || delta.at("type") != "thinking_delta")
        return std::nullopt;
    if (!delta.contains("thinking") || !delta.at("thinking").is_string())
        return std::nullopt;
    return delta.at("thinking").get<std::string>();
}

[[nodiscard]] std::vector<ToolCallChunk> openAIToolCallChunks(const nlohmann::json& body)
{
    if (!body.contains("choices") || !body.at("choices").is_array() || body.at("choices").empty())
        return {};
    const auto& choice = body.at("choices").at(0);
    if (!choice.contains("delta") || !choice.at("delta").is_object())
        return {};
    const auto& delta = choice.at("delta");
    if (!delta.contains("tool_calls") || !delta.at("tool_calls").is_array())
        return {};

    std::vector<ToolCallChunk> chunks;
    for (const auto& item : delta.at("tool_calls")) {
        if (!item.is_object() || !item.contains("function") || !item.at("function").is_object())
            continue;
        const auto& function = item.at("function");
        std::optional<std::size_t> index;
        if (item.contains("index") && item.at("index").is_number_unsigned())
            index = item.at("index").get<std::size_t>();
        chunks.push_back(ToolCallChunk {
            .index_ = index,
            .id_ = item.value("id", std::string {}),
            .name_ = function.value("name", std::string {}),
            .argumentsDelta_ = function.contains("arguments") && function.at("arguments").is_string()
                ? function.at("arguments").get<std::string>()
                : std::string {},
        });
    }
    return chunks;
}

[[nodiscard]] std::vector<ToolCallChunk> anthropicToolCallChunks(const nlohmann::json& body)
{
    if (!body.contains("type") || !body.at("type").is_string())
        return {};

    std::optional<std::size_t> index;
    if (body.contains("index") && body.at("index").is_number_unsigned())
        index = body.at("index").get<std::size_t>();

    if (body.at("type") == "content_block_start"
        && body.contains("content_block")
        && body.at("content_block").is_object()) {
        const auto& block = body.at("content_block");
        if (!block.contains("type") || block.at("type") != "tool_use")
            return {};
        return {
            ToolCallChunk {
                .index_ = index,
                .id_ = block.value("id", std::string {}),
                .name_ = block.value("name", std::string {}),
            },
        };
    }

    if (body.at("type") != "content_block_delta"
        || !body.contains("delta")
        || !body.at("delta").is_object()) {
        return {};
    }
    const auto& delta = body.at("delta");
    if (!delta.contains("type") || delta.at("type") != "input_json_delta")
        return {};
    return {
        ToolCallChunk {
            .index_ = index,
            .argumentsDelta_ = delta.value("partial_json", std::string {}),
        },
    };
}

[[nodiscard]] nlohmann::json streamContentBlocks(
    std::string_view text,
    std::optional<std::string> reasoning)
{
    nlohmann::json blocks = contentBlocksFromText(text);
    if (reasoning.has_value() && !reasoning->empty()) {
        blocks.push_back({
            { "type", "reasoning" },
            { "reasoning", *reasoning },
        });
    }
    return blocks;
}

[[nodiscard]] nlohmann::json toolCallChunkContentBlock(const ToolCallChunk& chunk)
{
    return {
        { "type", "tool_call_chunk" },
        { "id", chunk.id_.empty() ? nlohmann::json(nullptr) : nlohmann::json(chunk.id_) },
        { "name", chunk.name_.empty() ? nlohmann::json(nullptr) : nlohmann::json(chunk.name_) },
        { "args", chunk.argumentsDelta_ },
        { "index", chunk.index_.has_value() ? nlohmann::json(*chunk.index_) : nlohmann::json(nullptr) },
    };
}

[[nodiscard]] nlohmann::json usageFromBody(const nlohmann::json& body)
{
    if (body.is_object() && body.contains("usage"))
        return body.at("usage");
    return nlohmann::json::object();
}

} // namespace

std::string_view chatProviderName(ChatProviderKind provider) noexcept
{
    switch (provider) {
    case ChatProviderKind::OpenAICompatible:
        return "openai-compatible";
    case ChatProviderKind::Anthropic:
        return "anthropic";
    case ChatProviderKind::Qwen:
        return "qwen";
    case ChatProviderKind::DeepSeek:
        return "deepseek";
    }
    return "openai-compatible";
}

ProviderChatModelOptions ProviderChatModelOptions::openAICompatible(
    std::shared_ptr<IHttpClient> httpClient,
    std::string model,
    std::string apiKey)
{
    return ProviderChatModelOptions {
        .provider_ = ChatProviderKind::OpenAICompatible,
        .model_ = std::move(model),
        .apiKey_ = std::move(apiKey),
        .httpClient_ = std::move(httpClient),
    };
}

ProviderChatModelOptions ProviderChatModelOptions::anthropic(
    std::shared_ptr<IHttpClient> httpClient,
    std::string model,
    std::string apiKey)
{
    return ProviderChatModelOptions {
        .provider_ = ChatProviderKind::Anthropic,
        .model_ = std::move(model),
        .path_ = "/v1/messages",
        .apiKey_ = std::move(apiKey),
        .headers_ = { { "anthropic-version", "2023-06-01" } },
        .httpClient_ = std::move(httpClient),
    };
}

ProviderChatModelOptions ProviderChatModelOptions::qwen(
    std::shared_ptr<IHttpClient> httpClient,
    std::string model,
    std::string apiKey)
{
    auto options = openAICompatible(std::move(httpClient), std::move(model), std::move(apiKey));
    options.provider_ = ChatProviderKind::Qwen;
    return options;
}

ProviderChatModelOptions ProviderChatModelOptions::deepSeek(
    std::shared_ptr<IHttpClient> httpClient,
    std::string model,
    std::string apiKey)
{
    auto options = openAICompatible(std::move(httpClient), std::move(model), std::move(apiKey));
    options.provider_ = ChatProviderKind::DeepSeek;
    return options;
}

ProviderChatModel::ProviderChatModel(ProviderChatModelOptions options)
    : options_(std::move(options))
{
}

const ProviderChatModelOptions& ProviderChatModel::options() const noexcept
{
    return options_;
}

Result<std::vector<BaseMessage>> ProviderChatModel::prepareMessages(
    const std::vector<BaseMessage>& messages) const
{
    auto status = validateOptions(options_);
    if (!status.isOk())
        return status.status();
    if (messages.empty())
        return Status::invalidArgument("provider chat model requires at least one message");

    std::vector<BaseMessage> trimmed = messages;
    if (options_.prompt_.maxMessages_ > 0 && trimmed.size() > options_.prompt_.maxMessages_) {
        std::vector<BaseMessage> byCount;
        byCount.reserve(options_.prompt_.maxMessages_);
        const auto keepSystem = trimmed.front().type_ == MessageType::System
            && options_.prompt_.maxMessages_ > 1;
        if (keepSystem)
            byCount.push_back(trimmed.front());
        const auto tailCount = options_.prompt_.maxMessages_ - byCount.size();
        byCount.insert(
            byCount.end(),
            trimmed.end() - static_cast<std::ptrdiff_t>(tailCount),
            trimmed.end());
        trimmed = std::move(byCount);
    }

    if (options_.prompt_.maxBytes_ == 0)
        return trimmed;

    std::size_t total = 0;
    for (const auto& message : trimmed)
        total += messageBytes(message);
    if (total <= options_.prompt_.maxBytes_)
        return trimmed;

    std::vector<BaseMessage> byBytesReversed;
    std::size_t used = 0;
    const auto hasSystemPrefix = !trimmed.empty() && trimmed.front().type_ == MessageType::System;
    const auto systemCost = hasSystemPrefix ? messageBytes(trimmed.front()) : 0U;
    const auto startIndex = hasSystemPrefix ? 1U : 0U;

    for (std::size_t i = trimmed.size(); i > startIndex; --i) {
        const auto& message = trimmed[i - 1];
        const auto cost = messageBytes(message);
        if (byBytesReversed.empty() && cost > options_.prompt_.maxBytes_)
            return Status::resourceExhausted("latest provider prompt message exceeds byte limit");
        if (used + cost > options_.prompt_.maxBytes_)
            break;
        used += cost;
        byBytesReversed.push_back(message);
    }
    if (byBytesReversed.empty())
        return Status::resourceExhausted("provider prompt byte limit removed all messages");

    std::reverse(byBytesReversed.begin(), byBytesReversed.end());
    if (hasSystemPrefix && used + systemCost <= options_.prompt_.maxBytes_)
        byBytesReversed.insert(byBytesReversed.begin(), trimmed.front());
    return byBytesReversed;
}

Result<HttpRequest> ProviderChatModel::buildRequest(
    const std::vector<BaseMessage>& messages,
    bool stream) const
{
    auto prepared = prepareMessages(messages);
    if (!prepared.isOk())
        return prepared.status();

    nlohmann::json body {
        { "model", options_.model_ },
        { "stream", stream },
    };
    if (options_.maxOutputTokens_ > 0) {
        const auto key = options_.provider_ == ChatProviderKind::Anthropic
            ? "max_tokens"
            : "max_tokens";
        body[key] = options_.maxOutputTokens_;
    }
    if (stream && options_.includeUsage_ && options_.provider_ != ChatProviderKind::Anthropic)
        body["stream_options"] = { { "include_usage", true } };

    if (options_.provider_ == ChatProviderKind::Anthropic) {
        std::string systemPrompt;
        body["messages"] = anthropicMessagesJson(*prepared, systemPrompt);
        if (!systemPrompt.empty())
            body["system"] = std::move(systemPrompt);
        if (!options_.toolBinding_.tools_.empty())
            body["tools"] = anthropicToolsJson(options_.toolBinding_.tools_);
    } else {
        body["messages"] = openAIMessagesJson(*prepared);
        if (!options_.toolBinding_.tools_.empty())
            body["tools"] = openAIToolsJson(options_.toolBinding_.tools_);
        if (options_.toolBinding_.parallelToolCalls_.has_value())
            body["parallel_tool_calls"] = *options_.toolBinding_.parallelToolCalls_;
    }
    if (!options_.toolBinding_.toolChoice_.is_null())
        body["tool_choice"] = options_.toolBinding_.toolChoice_;

    for (auto it = options_.extraRequestFields_.begin(); it != options_.extraRequestFields_.end(); ++it)
        body[it.key()] = it.value();

    HttpRequest request;
    request.method_ = HttpMethod::Post;
    request.pathAndQuery_ = options_.path_;
    request.body_ = body.dump();
    request.contentType_ = "application/json";
    request.headers_.push_back({ "accept", stream ? "text/event-stream" : "application/json" });
    if (!options_.apiKey_.empty()) {
        if (options_.provider_ == ChatProviderKind::Anthropic)
            request.headers_.push_back({ "x-api-key", options_.apiKey_ });
        else
            request.headers_.push_back({ "authorization", "Bearer " + options_.apiKey_ });
    }
    for (const auto& [name, value] : options_.headers_)
        request.headers_.push_back({ name, value });
    return request;
}

Result<BaseMessage> ProviderChatModel::parseResponse(const HttpResponse& response) const
{
    if (!httpStatusOk(response.statusCode_))
        return Status::unavailable("provider returned HTTP " + std::to_string(response.statusCode_));

    auto body = parseJsonResult(response.body_, "provider response");
    if (!body.isOk())
        return body.status();

    auto parsed = options_.provider_ == ChatProviderKind::Anthropic
        ? parseAnthropicMessage(*body)
        : parseOpenAIMessage(*body);
    if (!parsed.isOk())
        return parsed.status();

    const auto usage = usageFromBody(*body);
    if (!usage.empty())
        parsed->usageMetadata_ = usage;
    parsed->responseMetadata_ = {
        { "provider", std::string(chatProviderName(options_.provider_)) },
        { "model", options_.model_ },
    };
    return parsed;
}

Result<BaseMessage> ProviderChatModel::invoke(const std::vector<BaseMessage>& messages)
{
    auto request = buildRequest(messages, false);
    if (!request.isOk())
        return request.status();

    auto response = options_.httpClient_->send(std::move(*request), options_.requestOptions_);
    if (!response.isOk())
        return response.status();
    return parseResponse(*response);
}

Result<BaseMessage> ProviderChatModel::stream(
    const std::vector<BaseMessage>& messages,
    AIMessageChunkHandler onChunk)
{
    auto request = buildRequest(messages, true);
    if (!request.isOk())
        return request.status();

    std::string content;
    struct ToolCallBuilder {
        std::string id_;
        std::string name_;
        std::string arguments_;
    };
    std::map<std::size_t, ToolCallBuilder> toolCallBuilders;
    nlohmann::json streamedContentBlocks = nlohmann::json::array();
    nlohmann::json usage = nlohmann::json::object();
    Status callbackStatus = Status::ok();
    auto response = options_.httpClient_->sendSse(
        std::move(*request),
        options_.requestOptions_,
        [&](const ServerSentEvent& event) -> Status {
            if (event.data_ == "[DONE]")
                return Status::ok();
            auto parsed = parseJsonResult(event.data_, "provider SSE event");
            if (!parsed.isOk())
                return parsed.status();

            const auto eventUsage = usageFromBody(*parsed);
            if (!eventUsage.empty())
                usage = eventUsage;

            auto delta = options_.provider_ == ChatProviderKind::Anthropic
                ? anthropicStreamDelta(*parsed)
                : openAIStreamDelta(*parsed);
            auto reasoning = options_.provider_ == ChatProviderKind::Anthropic
                ? anthropicReasoningDelta(*parsed)
                : std::optional<std::string> {};

            auto toolCallChunks = options_.provider_ == ChatProviderKind::Anthropic
                ? anthropicToolCallChunks(*parsed)
                : openAIToolCallChunks(*parsed);
            for (std::size_t i = 0; i < toolCallChunks.size(); ++i) {
                const auto& chunk = toolCallChunks[i];
                const std::size_t index = chunk.index_.value_or(i);
                auto& builder = toolCallBuilders[index];
                if (!chunk.id_.empty())
                    builder.id_ = chunk.id_;
                if (!chunk.name_.empty())
                    builder.name_ = chunk.name_;
                builder.arguments_ += chunk.argumentsDelta_;
            }

            auto contentBlocks = streamContentBlocks(delta.value_or(std::string {}), reasoning);
            for (const auto& chunk : toolCallChunks)
                contentBlocks.push_back(toolCallChunkContentBlock(chunk));
            for (const auto& block : contentBlocks) {
                if (block.is_object()
                    && block.contains("type")
                    && block.at("type") == "tool_call_chunk") {
                    continue;
                }
                streamedContentBlocks.push_back(block);
            }
            if ((!delta.has_value() || delta->empty())
                && (!reasoning.has_value() || reasoning->empty())
                && toolCallChunks.empty()) {
                return Status::ok();
            }

            if (delta.has_value())
                content += *delta;
            if (onChunk) {
                callbackStatus = onChunk(AIMessageChunk {
                    .text_ = delta.value_or(std::string {}),
                    .contentBlocks_ = std::move(contentBlocks),
                    .toolCallChunks_ = std::move(toolCallChunks),
                    .metadata_ = commonMetadata(options_.provider_, options_.model_, usage),
                });
                return callbackStatus;
            }
            return Status::ok();
        },
        options_.streamOptions_);
    if (!callbackStatus.isOk())
        return callbackStatus;
    if (!response.isOk())
        return response.status();
    if (!httpStatusOk(response->statusCode_))
        return Status::unavailable("provider stream returned HTTP " + std::to_string(response->statusCode_));

    std::vector<ToolCall> toolCalls;
    toolCalls.reserve(toolCallBuilders.size());
    for (auto& [index, builder] : toolCallBuilders) {
        (void)index;
        if (builder.name_.empty())
            continue;
        auto arguments = parseToolArguments(
            nlohmann::json(builder.arguments_),
            "OpenAI-compatible streaming tool call");
        if (!arguments.isOk())
            return arguments.status();
        toolCalls.push_back(ToolCall {
            .id_ = std::move(builder.id_),
            .name_ = std::move(builder.name_),
            .args_ = std::move(*arguments),
        });
    }

    auto message = BaseMessage::ai(std::move(content), std::move(toolCalls));
    message.contentBlocks_ = !streamedContentBlocks.empty()
        ? std::move(streamedContentBlocks)
        : contentBlocksFromText(message.content_);
    if (!usage.empty())
        message.usageMetadata_ = usage;
    message.responseMetadata_ = {
        { "provider", std::string(chatProviderName(options_.provider_)) },
        { "model", options_.model_ },
    };
    if (onChunk) {
        auto doneStatus = onChunk(AIMessageChunk {
            .message_ = message,
            .metadata_ = commonMetadata(options_.provider_, options_.model_, usage),
            .done_ = true,
        });
        if (!doneStatus.isOk())
            return doneStatus;
    }
    return message;
}

Result<std::vector<BaseMessage>> ProviderChatModel::batch(
    const std::vector<std::vector<BaseMessage>>& inputs)
{
    return BaseChatModel::batch(inputs);
}

Result<std::shared_ptr<BaseChatModel>> ProviderChatModel::bindTools(
    ChatModelToolBinding binding) const
{
    for (const auto& tool : binding.tools_) {
        if (tool.name_.empty())
            return Status::invalidArgument("chat model tool name cannot be empty");
        if (!tool.inputSchema_.rawJson().is_object())
            return Status::invalidArgument("chat model tool schema must be an object");
    }

    auto options = options_;
    options.toolBinding_ = std::move(binding);
    std::shared_ptr<BaseChatModel> model = std::make_shared<ProviderChatModel>(std::move(options));
    return model;
}

} // namespace lc
