#include "foundation/network/i_http_client.hpp"
#include "langgraph/model/provider_chat_model.hpp"

#include <cassert>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

class FakeHttpClient final : public lgc::IHttpClient {
public:
    lgc::HttpResponse response_ {
        .statusCode_ = 200,
        .body_ = R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})",
    };
    std::vector<lgc::ServerSentEvent> events_;
    lgc::HttpRequest lastRequest_;
    lgc::HttpRequestOptions lastOptions_;
    int sendCalls_ { 0 };
    int streamCalls_ { 0 };
    bool closed_ { false };

    [[nodiscard]] lgc::HttpResult send(
        lgc::HttpRequest request,
        lgc::HttpRequestOptions options) override
    {
        ++sendCalls_;
        lastRequest_ = std::move(request);
        lastOptions_ = std::move(options);
        return response_;
    }

    [[nodiscard]] lgc::Status sendAsync(
        lgc::HttpRequest request,
        lgc::HttpRequestOptions options,
        lgc::HttpCallback callback) override
    {
        auto response = send(std::move(request), std::move(options));
        callback(std::move(response));
        return lgc::Status::ok();
    }

    [[nodiscard]] lgc::HttpResult sendStreaming(
        lgc::HttpRequest request,
        lgc::HttpRequestOptions options,
        lgc::HttpBodyChunkCallback,
        lgc::HttpStreamOptions = {}) override
    {
        lastRequest_ = std::move(request);
        lastOptions_ = std::move(options);
        return response_;
    }

    [[nodiscard]] lgc::HttpResult sendSse(
        lgc::HttpRequest request,
        lgc::HttpRequestOptions options,
        lgc::ServerSentEventCallback callback,
        lgc::HttpStreamOptions = {}) override
    {
        ++streamCalls_;
        lastRequest_ = std::move(request);
        lastOptions_ = std::move(options);
        for (const auto& event : events_) {
            auto status = callback(event);
            if (!status.isOk())
                return status;
        }
        return response_;
    }

    [[nodiscard]] std::shared_ptr<lgc::IAuthorizationProvider> authorizationProvider() const override
    {
        return nullptr;
    }

    [[nodiscard]] lgc::Status close() override
    {
        closed_ = true;
        return lgc::Status::ok();
    }

    [[nodiscard]] bool isClosed() const noexcept override { return closed_; }
};

class FakeTokenCounter final : public lgc::ITokenCounter {
public:
    int textCalls_ { 0 };
    int messageCalls_ { 0 };

    [[nodiscard]] lgc::Result<std::uint64_t> countTextTokens(std::string_view text) override
    {
        ++textCalls_;
        return static_cast<std::uint64_t>(text.size());
    }

    [[nodiscard]] lgc::Result<std::uint64_t> countMessageTokens(
        const std::vector<lgc::BaseMessage>& messages) override
    {
        ++messageCalls_;
        std::uint64_t total = 0;
        for (const auto& message : messages)
            total += message.content_.size();
        return total;
    }
};

[[nodiscard]] std::optional<std::string> headerValue(
    const lgc::HttpRequest& request,
    std::string_view name)
{
    for (const auto& [headerName, value] : request.headers_) {
        if (headerName == name)
            return value;
    }
    return std::nullopt;
}

void testOpenAICompatibleInvokeTrimsPrompt()
{
    auto http = std::make_shared<FakeHttpClient>();
    http->response_.body_ = R"({"choices":[{"message":{"role":"assistant","content":"trimmed"}}],"usage":{"prompt_tokens":4,"completion_tokens":1}})";

    auto options = lgc::ProviderChatModelOptions::openAICompatible(http, "edge-model", "secret-token");
    options.prompt_.maxMessages_ = 2;
    options.requestOptions_.timeout_ = 150ms;
    options.requestOptions_.retryPolicy_ = lgc::HttpRetryPolicy {
        .maxRetries_ = 2,
        .delay_ = 5ms,
        .statusCodes_ = { 429, 503 },
    };
    options.extraRequestFields_ = { { "temperature", 0.2 } };
    lgc::ProviderChatModel model(std::move(options));

    auto response = model.invoke({
        lgc::BaseMessage::system("rules"),
        lgc::BaseMessage::human("old"),
        lgc::BaseMessage::human("new"),
    });
    assert(response.isOk());
    assert(response->content_ == "trimmed");
    assert(response->usageMetadata_.source_ == lgc::UsageMetadataSource::Provider);
    assert(response->usageMetadata_.provider_ == "openai-compatible");
    assert(response->usageMetadata_.model_ == "edge-model");
    assert(response->usageMetadata_.tokens_.inputTokens_.has_value());
    assert(*response->usageMetadata_.tokens_.inputTokens_ == 4);
    assert(response->usageMetadata_.tokens_.outputTokens_.has_value());
    assert(*response->usageMetadata_.tokens_.outputTokens_ == 1);
    assert(response->usageMetadata_.tokens_.totalTokens_.has_value());
    assert(*response->usageMetadata_.tokens_.totalTokens_ == 5);
    assert(response->usageMetadata_.raw_.at("prompt_tokens") == 4);
    assert(http->sendCalls_ == 1);
    assert(http->lastRequest_.pathAndQuery_ == "/v1/chat/completions");
    assert(http->lastOptions_.timeout_ == 150ms);
    assert(http->lastOptions_.retryPolicy_.has_value());
    assert(http->lastOptions_.retryPolicy_->maxRetries_ == 2);
    assert(headerValue(http->lastRequest_, "authorization") == "Bearer secret-token");

    auto body = nlohmann::json::parse(http->lastRequest_.body_);
    assert(body.at("model") == "edge-model");
    assert(body.at("temperature") == 0.2);
    assert(body.at("stream") == false);
    assert(body.at("messages").size() == 2);
    assert(body.at("messages").at(0).at("role") == "system");
    assert(body.at("messages").at(1).at("content") == "new");
}

void testOpenAICompatibleStreamUsageAndBackpressure()
{
    auto http = std::make_shared<FakeHttpClient>();
    http->events_ = {
        lgc::ServerSentEvent { .data_ = R"({"choices":[{"delta":{"content":"hel"}}]})" },
        lgc::ServerSentEvent { .data_ = R"({"choices":[{"delta":{"content":"lo"}}],"usage":{"total_tokens":5}})" },
        lgc::ServerSentEvent { .data_ = "[DONE]" },
    };

    lgc::ProviderChatModel model(
        lgc::ProviderChatModelOptions::openAICompatible(http, "edge-stream"));

    std::vector<std::string> deltas;
    bool sawDone = false;
    auto response = model.stream(
        { lgc::BaseMessage::human("say hi") },
        [&](const lgc::AIMessageChunk& chunk) -> lgc::Status {
            if (!chunk.text_.empty())
                deltas.push_back(chunk.text_);
            if (chunk.done_) {
                sawDone = true;
                assert(chunk.message_.has_value());
                assert(chunk.message_->content_ == "hello");
                assert(chunk.metadata_.at("usage").at("total_tokens") == 5);
                assert(chunk.usageMetadata_.tokens_.totalTokens_.has_value());
                assert(*chunk.usageMetadata_.tokens_.totalTokens_ == 5);
            }
            return lgc::Status::ok();
        });
    assert(response.isOk());
    assert(response->content_ == "hello");
    assert(response->usageMetadata_.tokens_.totalTokens_.has_value());
    assert(*response->usageMetadata_.tokens_.totalTokens_ == 5);
    assert((deltas == std::vector<std::string> { "hel", "lo" }));
    assert(sawDone);

    auto body = nlohmann::json::parse(http->lastRequest_.body_);
    assert(body.at("stream") == true);
    assert(body.at("stream_options").at("include_usage") == true);

    auto cancellingHttp = std::make_shared<FakeHttpClient>();
    cancellingHttp->events_ = {
        lgc::ServerSentEvent { .data_ = R"({"choices":[{"delta":{"content":"stop"}}]})" },
    };
    lgc::ProviderChatModel cancelling(
        lgc::ProviderChatModelOptions::openAICompatible(cancellingHttp, "edge-stream"));
    auto cancelled = cancelling.stream(
        { lgc::BaseMessage::human("cancel") },
        [](const lgc::AIMessageChunk&) -> lgc::Status {
            return lgc::Status::cancelled("downstream backpressure");
        });
    assert(!cancelled.isOk());
    assert(cancelled.status().code() == lgc::StatusCode::Cancelled);
}

void testOpenAIRequestUsesStandardMultimodalContentBlocks()
{
    auto http = std::make_shared<FakeHttpClient>();
    http->response_.body_ = R"({"choices":[{"message":{"role":"assistant","content":"seen"}}]})";

    lgc::ProviderChatModel model(
        lgc::ProviderChatModelOptions::openAICompatible(http, "vision-model"));

    lgc::BaseMessage message = lgc::BaseMessage::human("");
    message.contentBlocks_ = {
        {
            { "type", "text" },
            { "text", "Describe this." },
        },
        {
            { "type", "image" },
            { "url", "https://example.com/image.jpg" },
        },
    };
    message.content_ = lgc::contentBlocksText(message.contentBlocks_);

    auto response = model.invoke({ message });
    assert(response.isOk());
    auto body = nlohmann::json::parse(http->lastRequest_.body_);
    const auto& content = body.at("messages").at(0).at("content");
    assert(content.is_array());
    assert(content.at(0).at("type") == "text");
    assert(content.at(0).at("text") == "Describe this.");
    assert(content.at(1).at("type") == "image_url");
    assert(content.at(1).at("image_url").at("url") == "https://example.com/image.jpg");
}

void testAnthropicProfileSeparatesSystemAndStreams()
{
    auto http = std::make_shared<FakeHttpClient>();
    http->events_ = {
        lgc::ServerSentEvent { .data_ = R"({"type":"content_block_delta","delta":{"type":"text_delta","text":"edge "}})" },
        lgc::ServerSentEvent { .data_ = R"({"type":"content_block_delta","delta":{"type":"text_delta","text":"ok"}})" },
        lgc::ServerSentEvent { .data_ = R"({"type":"message_delta","usage":{"output_tokens":2}})" },
    };

    lgc::ProviderChatModel model(
        lgc::ProviderChatModelOptions::anthropic(http, "claude-edge", "anthropic-key"));
    auto response = model.stream(
        {
            lgc::BaseMessage::system("be terse"),
            lgc::BaseMessage::human("status"),
        },
        nullptr);
    assert(response.isOk());
    assert(response->content_ == "edge ok");
    assert(response->usageMetadata_.source_ == lgc::UsageMetadataSource::Provider);
    assert(response->usageMetadata_.provider_ == "anthropic");
    assert(response->usageMetadata_.tokens_.outputTokens_.has_value());
    assert(*response->usageMetadata_.tokens_.outputTokens_ == 2);
    assert(http->streamCalls_ == 1);
    assert(http->lastRequest_.pathAndQuery_ == "/v1/messages");
    assert(headerValue(http->lastRequest_, "x-api-key") == "anthropic-key");
    assert(headerValue(http->lastRequest_, "anthropic-version").has_value());

    auto body = nlohmann::json::parse(http->lastRequest_.body_);
    assert(body.at("system") == "be terse");
    assert(body.at("messages").size() == 1);
    assert(body.at("messages").at(0).at("role") == "user");
}

void testLocalTokenCounterFillsMissingUsage()
{
    auto http = std::make_shared<FakeHttpClient>();
    http->response_.body_ = R"({"choices":[{"message":{"role":"assistant","content":"counted"}}]})";
    auto counter = std::make_shared<FakeTokenCounter>();

    auto options = lgc::ProviderChatModelOptions::openAICompatible(http, "counter-model");
    options.tokenCounter_ = counter;
    lgc::ProviderChatModel model(std::move(options));

    auto response = model.invoke({ lgc::BaseMessage::human("hello") });
    assert(response.isOk());
    assert(response->usageMetadata_.source_ == lgc::UsageMetadataSource::Local);
    assert(response->usageMetadata_.tokens_.inputTokens_.has_value());
    assert(*response->usageMetadata_.tokens_.inputTokens_ == 5);
    assert(response->usageMetadata_.tokens_.outputTokens_.has_value());
    assert(*response->usageMetadata_.tokens_.outputTokens_ == 7);
    assert(response->usageMetadata_.tokens_.totalTokens_.has_value());
    assert(*response->usageMetadata_.tokens_.totalTokens_ == 12);
    assert(counter->messageCalls_ == 1);
    assert(counter->textCalls_ == 1);
}

void testBatchUsesInjectedTransport()
{
    auto http = std::make_shared<FakeHttpClient>();
    http->response_.body_ = R"({"choices":[{"message":{"role":"assistant","content":"batched"}}]})";
    lgc::ProviderChatModel model(
        lgc::ProviderChatModelOptions::deepSeek(http, "deepseek-chat"));

    auto responses = model.batch({
        { lgc::BaseMessage::human("one") },
        { lgc::BaseMessage::human("two") },
    });
    assert(responses.isOk());
    assert(responses->size() == 2);
    assert(responses->at(0).content_ == "batched");
    assert(http->sendCalls_ == 2);
}

void testBindToolsBuildsOpenAIRequestAndParsesToolCalls()
{
    auto http = std::make_shared<FakeHttpClient>();
    http->response_.body_ = R"({
        "choices":[{
            "message":{
                "role":"assistant",
                "content":null,
                "tool_calls":[{
                    "id":"call-1",
                    "type":"function",
                    "function":{
                        "name":"get_weather",
                        "arguments":"{\"location\":\"Boston\"}"
                    }
                }]
            }
        }]
    })";

    lgc::ProviderChatModel model(
        lgc::ProviderChatModelOptions::openAICompatible(http, "tool-model"));
    auto bound = model.bindTools(lgc::ChatModelToolBinding {
        .tools_ = {
            lgc::ChatModelTool {
                .name_ = "get_weather",
                .description_ = "Get weather.",
                .inputSchema_ = lgc::JsonSchema::object()
                                    .property("location", lgc::JsonSchema::string(), true)
                                    .additionalProperties(false),
            },
        },
        .toolChoice_ = "auto",
        .parallelToolCalls_ = false,
    });
    assert(bound.isOk());

    auto response = (*bound)->invoke({ lgc::BaseMessage::human("weather?") });
    assert(response.isOk());
    assert(response->content_.empty());
    assert(response->toolCalls_.size() == 1);
    assert(response->toolCalls_.front().id_ == "call-1");
    assert(response->toolCalls_.front().name_ == "get_weather");
    assert(response->toolCalls_.front().args_.at("location") == "Boston");

    auto body = nlohmann::json::parse(http->lastRequest_.body_);
    assert(body.at("tools").size() == 1);
    assert(body.at("tools").at(0).at("type") == "function");
    assert(body.at("tools").at(0).at("function").at("name") == "get_weather");
    assert(body.at("tools").at(0).at("function").at("parameters").at("required").at(0) == "location");
    assert(body.at("tool_choice") == "auto");
    assert(body.at("parallel_tool_calls") == false);
}

void testOpenAIStreamToolCallChunks()
{
    auto http = std::make_shared<FakeHttpClient>();
    http->events_ = {
        lgc::ServerSentEvent { .data_ = R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call-1","function":{"name":"lookup","arguments":"{\"q\""}}]}}]})" },
        lgc::ServerSentEvent { .data_ = R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":":\"edge\"}"}}]}}]})" },
        lgc::ServerSentEvent { .data_ = "[DONE]" },
    };

    lgc::ProviderChatModel model(
        lgc::ProviderChatModelOptions::openAICompatible(http, "tool-stream"));
    std::size_t chunks = 0;
    std::size_t contentBlockChunks = 0;
    auto response = model.stream(
        { lgc::BaseMessage::human("lookup") },
        [&](const lgc::AIMessageChunk& chunk) -> lgc::Status {
            chunks += chunk.toolCallChunks_.size();
            for (const auto& block : chunk.contentBlocks_) {
                if (block.at("type") == "tool_call_chunk") {
                    ++contentBlockChunks;
                    assert(block.contains("args"));
                    assert(block.contains("index"));
                }
            }
            return lgc::Status::ok();
        });
    assert(response.isOk());
    assert(chunks == 2);
    assert(contentBlockChunks == 2);
    assert(response->toolCalls_.size() == 1);
    assert(response->toolCalls_.front().id_ == "call-1");
    assert(response->toolCalls_.front().name_ == "lookup");
    assert(response->toolCalls_.front().args_.at("q") == "edge");
    auto blocks = lgc::messageContentBlocks(*response);
    assert(blocks.size() == 1);
    assert(blocks.front().at("type") == "tool_call");
}

void testAnthropicStreamToolUseDeltas()
{
    auto http = std::make_shared<FakeHttpClient>();
    http->events_ = {
        lgc::ServerSentEvent { .data_ = R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"Need tool."}})" },
        lgc::ServerSentEvent { .data_ = R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"toolu_1","name":"inspect","input":{}}})" },
        lgc::ServerSentEvent { .data_ = R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"target\""}})" },
        lgc::ServerSentEvent { .data_ = R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":":\"motor\"}"}})" },
    };

    lgc::ProviderChatModel model(
        lgc::ProviderChatModelOptions::anthropic(http, "claude-tools"));

    std::size_t reasoningBlocks = 0;
    std::size_t toolChunkBlocks = 0;
    auto response = model.stream(
        { lgc::BaseMessage::human("inspect") },
        [&](const lgc::AIMessageChunk& chunk) -> lgc::Status {
            for (const auto& block : chunk.contentBlocks_) {
                if (block.at("type") == "reasoning")
                    ++reasoningBlocks;
                if (block.at("type") == "tool_call_chunk")
                    ++toolChunkBlocks;
            }
            return lgc::Status::ok();
        });
    assert(response.isOk());
    assert(reasoningBlocks == 1);
    assert(toolChunkBlocks == 3);
    assert(response->toolCalls_.size() == 1);
    assert(response->toolCalls_.front().id_ == "toolu_1");
    assert(response->toolCalls_.front().name_ == "inspect");
    assert(response->toolCalls_.front().args_.at("target") == "motor");

    auto blocks = lgc::messageContentBlocks(*response);
    assert(blocks.size() == 2);
    assert(blocks.at(0).at("type") == "reasoning");
    assert(blocks.at(1).at("type") == "tool_call");
}

void testAnthropicBindToolsAndToolUseResponse()
{
    auto http = std::make_shared<FakeHttpClient>();
    http->response_.body_ = R"({
        "content":[
            {"type":"text","text":"checking"},
            {"type":"tool_use","id":"toolu_1","name":"inspect","input":{"target":"motor"}}
        ]
    })";

    lgc::ProviderChatModel model(
        lgc::ProviderChatModelOptions::anthropic(http, "claude-tools"));
    auto bound = model.bindTools(lgc::ChatModelToolBinding {
        .tools_ = {
            lgc::ChatModelTool {
                .name_ = "inspect",
                .description_ = "Inspect target.",
                .inputSchema_ = lgc::JsonSchema::object()
                                    .property("target", lgc::JsonSchema::string(), true),
            },
        },
    });
    assert(bound.isOk());

    auto response = (*bound)->invoke({ lgc::BaseMessage::human("inspect motor") });
    assert(response.isOk());
    assert(response->content_ == "checking");
    assert(response->toolCalls_.size() == 1);
    assert(response->toolCalls_.front().id_ == "toolu_1");
    assert(response->toolCalls_.front().name_ == "inspect");
    assert(response->toolCalls_.front().args_.at("target") == "motor");

    auto body = nlohmann::json::parse(http->lastRequest_.body_);
    assert(body.at("tools").at(0).at("name") == "inspect");
    assert(body.at("tools").at(0).at("input_schema").at("properties").contains("target"));
}

} // namespace

int main()
{
    testOpenAICompatibleInvokeTrimsPrompt();
    testOpenAICompatibleStreamUsageAndBackpressure();
    testOpenAIRequestUsesStandardMultimodalContentBlocks();
    testAnthropicProfileSeparatesSystemAndStreams();
    testLocalTokenCounterFillsMissingUsage();
    testBatchUsesInjectedTransport();
    testBindToolsBuildsOpenAIRequestAndParsesToolCalls();
    testOpenAIStreamToolCallChunks();
    testAnthropicStreamToolUseDeltas();
    testAnthropicBindToolsAndToolUseResponse();
    return 0;
}
