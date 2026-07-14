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

class FakeHttpClient final : public lc::IHttpClient {
public:
    lc::HttpResponse response_ {
        .statusCode_ = 200,
        .body_ = R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})",
    };
    std::vector<lc::ServerSentEvent> events_;
    lc::HttpRequest lastRequest_;
    lc::HttpRequestOptions lastOptions_;
    int sendCalls_ { 0 };
    int streamCalls_ { 0 };
    bool closed_ { false };

    [[nodiscard]] lc::HttpResult send(
        lc::HttpRequest request,
        lc::HttpRequestOptions options) override
    {
        ++sendCalls_;
        lastRequest_ = std::move(request);
        lastOptions_ = std::move(options);
        return response_;
    }

    [[nodiscard]] lc::Status sendAsync(
        lc::HttpRequest request,
        lc::HttpRequestOptions options,
        lc::HttpCallback callback) override
    {
        auto response = send(std::move(request), std::move(options));
        callback(std::move(response));
        return lc::Status::ok();
    }

    [[nodiscard]] lc::HttpResult sendStreaming(
        lc::HttpRequest request,
        lc::HttpRequestOptions options,
        lc::HttpBodyChunkCallback,
        lc::HttpStreamOptions = {}) override
    {
        lastRequest_ = std::move(request);
        lastOptions_ = std::move(options);
        return response_;
    }

    [[nodiscard]] lc::HttpResult sendSse(
        lc::HttpRequest request,
        lc::HttpRequestOptions options,
        lc::ServerSentEventCallback callback,
        lc::HttpStreamOptions = {}) override
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

    [[nodiscard]] std::shared_ptr<lc::IAuthorizationProvider> authorizationProvider() const override
    {
        return nullptr;
    }

    [[nodiscard]] lc::Status close() override
    {
        closed_ = true;
        return lc::Status::ok();
    }

    [[nodiscard]] bool isClosed() const noexcept override { return closed_; }
};

[[nodiscard]] std::optional<std::string> headerValue(
    const lc::HttpRequest& request,
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

    auto options = lc::ProviderChatModelOptions::openAICompatible(http, "edge-model", "secret-token");
    options.prompt_.maxMessages_ = 2;
    options.requestOptions_.timeout_ = 150ms;
    options.requestOptions_.retryPolicy_ = lc::HttpRetryPolicy {
        .maxRetries_ = 2,
        .delay_ = 5ms,
        .statusCodes_ = { 429, 503 },
    };
    options.extraRequestFields_ = { { "temperature", 0.2 } };
    lc::ProviderChatModel model(std::move(options));

    auto response = model.invoke({
        lc::BaseMessage::system("rules"),
        lc::BaseMessage::human("old"),
        lc::BaseMessage::human("new"),
    });
    assert(response.isOk());
    assert(response->content_ == "trimmed");
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
        lc::ServerSentEvent { .data_ = R"({"choices":[{"delta":{"content":"hel"}}]})" },
        lc::ServerSentEvent { .data_ = R"({"choices":[{"delta":{"content":"lo"}}],"usage":{"total_tokens":5}})" },
        lc::ServerSentEvent { .data_ = "[DONE]" },
    };

    lc::ProviderChatModel model(
        lc::ProviderChatModelOptions::openAICompatible(http, "edge-stream"));

    std::vector<std::string> deltas;
    bool sawDone = false;
    auto response = model.stream(
        { lc::BaseMessage::human("say hi") },
        [&](const lc::AIMessageChunk& chunk) -> lc::Status {
            if (!chunk.text_.empty())
                deltas.push_back(chunk.text_);
            if (chunk.done_) {
                sawDone = true;
                assert(chunk.message_.has_value());
                assert(chunk.message_->content_ == "hello");
                assert(chunk.metadata_.at("usage").at("total_tokens") == 5);
            }
            return lc::Status::ok();
        });
    assert(response.isOk());
    assert(response->content_ == "hello");
    assert((deltas == std::vector<std::string> { "hel", "lo" }));
    assert(sawDone);

    auto body = nlohmann::json::parse(http->lastRequest_.body_);
    assert(body.at("stream") == true);
    assert(body.at("stream_options").at("include_usage") == true);

    auto cancellingHttp = std::make_shared<FakeHttpClient>();
    cancellingHttp->events_ = {
        lc::ServerSentEvent { .data_ = R"({"choices":[{"delta":{"content":"stop"}}]})" },
    };
    lc::ProviderChatModel cancelling(
        lc::ProviderChatModelOptions::openAICompatible(cancellingHttp, "edge-stream"));
    auto cancelled = cancelling.stream(
        { lc::BaseMessage::human("cancel") },
        [](const lc::AIMessageChunk&) -> lc::Status {
            return lc::Status::cancelled("downstream backpressure");
        });
    assert(!cancelled.isOk());
    assert(cancelled.status().code() == lc::StatusCode::Cancelled);
}

void testOpenAIRequestUsesStandardMultimodalContentBlocks()
{
    auto http = std::make_shared<FakeHttpClient>();
    http->response_.body_ = R"({"choices":[{"message":{"role":"assistant","content":"seen"}}]})";

    lc::ProviderChatModel model(
        lc::ProviderChatModelOptions::openAICompatible(http, "vision-model"));

    lc::BaseMessage message = lc::BaseMessage::human("");
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
    message.content_ = lc::contentBlocksText(message.contentBlocks_);

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
        lc::ServerSentEvent { .data_ = R"({"type":"content_block_delta","delta":{"type":"text_delta","text":"edge "}})" },
        lc::ServerSentEvent { .data_ = R"({"type":"content_block_delta","delta":{"type":"text_delta","text":"ok"}})" },
        lc::ServerSentEvent { .data_ = R"({"type":"message_delta","usage":{"output_tokens":2}})" },
    };

    lc::ProviderChatModel model(
        lc::ProviderChatModelOptions::anthropic(http, "claude-edge", "anthropic-key"));
    auto response = model.stream(
        {
            lc::BaseMessage::system("be terse"),
            lc::BaseMessage::human("status"),
        },
        nullptr);
    assert(response.isOk());
    assert(response->content_ == "edge ok");
    assert(http->streamCalls_ == 1);
    assert(http->lastRequest_.pathAndQuery_ == "/v1/messages");
    assert(headerValue(http->lastRequest_, "x-api-key") == "anthropic-key");
    assert(headerValue(http->lastRequest_, "anthropic-version").has_value());

    auto body = nlohmann::json::parse(http->lastRequest_.body_);
    assert(body.at("system") == "be terse");
    assert(body.at("messages").size() == 1);
    assert(body.at("messages").at(0).at("role") == "user");
}

void testBatchUsesInjectedTransport()
{
    auto http = std::make_shared<FakeHttpClient>();
    http->response_.body_ = R"({"choices":[{"message":{"role":"assistant","content":"batched"}}]})";
    lc::ProviderChatModel model(
        lc::ProviderChatModelOptions::deepSeek(http, "deepseek-chat"));

    auto responses = model.batch({
        { lc::BaseMessage::human("one") },
        { lc::BaseMessage::human("two") },
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

    lc::ProviderChatModel model(
        lc::ProviderChatModelOptions::openAICompatible(http, "tool-model"));
    auto bound = model.bindTools(lc::ChatModelToolBinding {
        .tools_ = {
            lc::ChatModelTool {
                .name_ = "get_weather",
                .description_ = "Get weather.",
                .inputSchema_ = lc::JsonSchema::object()
                                    .property("location", lc::JsonSchema::string(), true)
                                    .additionalProperties(false),
            },
        },
        .toolChoice_ = "auto",
        .parallelToolCalls_ = false,
    });
    assert(bound.isOk());

    auto response = (*bound)->invoke({ lc::BaseMessage::human("weather?") });
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
        lc::ServerSentEvent { .data_ = R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call-1","function":{"name":"lookup","arguments":"{\"q\""}}]}}]})" },
        lc::ServerSentEvent { .data_ = R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":":\"edge\"}"}}]}}]})" },
        lc::ServerSentEvent { .data_ = "[DONE]" },
    };

    lc::ProviderChatModel model(
        lc::ProviderChatModelOptions::openAICompatible(http, "tool-stream"));
    std::size_t chunks = 0;
    std::size_t contentBlockChunks = 0;
    auto response = model.stream(
        { lc::BaseMessage::human("lookup") },
        [&](const lc::AIMessageChunk& chunk) -> lc::Status {
            chunks += chunk.toolCallChunks_.size();
            for (const auto& block : chunk.contentBlocks_) {
                if (block.at("type") == "tool_call_chunk") {
                    ++contentBlockChunks;
                    assert(block.contains("args"));
                    assert(block.contains("index"));
                }
            }
            return lc::Status::ok();
        });
    assert(response.isOk());
    assert(chunks == 2);
    assert(contentBlockChunks == 2);
    assert(response->toolCalls_.size() == 1);
    assert(response->toolCalls_.front().id_ == "call-1");
    assert(response->toolCalls_.front().name_ == "lookup");
    assert(response->toolCalls_.front().args_.at("q") == "edge");
    auto blocks = lc::messageContentBlocks(*response);
    assert(blocks.size() == 1);
    assert(blocks.front().at("type") == "tool_call");
}

void testAnthropicStreamToolUseDeltas()
{
    auto http = std::make_shared<FakeHttpClient>();
    http->events_ = {
        lc::ServerSentEvent { .data_ = R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"Need tool."}})" },
        lc::ServerSentEvent { .data_ = R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"toolu_1","name":"inspect","input":{}}})" },
        lc::ServerSentEvent { .data_ = R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"target\""}})" },
        lc::ServerSentEvent { .data_ = R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":":\"motor\"}"}})" },
    };

    lc::ProviderChatModel model(
        lc::ProviderChatModelOptions::anthropic(http, "claude-tools"));

    std::size_t reasoningBlocks = 0;
    std::size_t toolChunkBlocks = 0;
    auto response = model.stream(
        { lc::BaseMessage::human("inspect") },
        [&](const lc::AIMessageChunk& chunk) -> lc::Status {
            for (const auto& block : chunk.contentBlocks_) {
                if (block.at("type") == "reasoning")
                    ++reasoningBlocks;
                if (block.at("type") == "tool_call_chunk")
                    ++toolChunkBlocks;
            }
            return lc::Status::ok();
        });
    assert(response.isOk());
    assert(reasoningBlocks == 1);
    assert(toolChunkBlocks == 3);
    assert(response->toolCalls_.size() == 1);
    assert(response->toolCalls_.front().id_ == "toolu_1");
    assert(response->toolCalls_.front().name_ == "inspect");
    assert(response->toolCalls_.front().args_.at("target") == "motor");

    auto blocks = lc::messageContentBlocks(*response);
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

    lc::ProviderChatModel model(
        lc::ProviderChatModelOptions::anthropic(http, "claude-tools"));
    auto bound = model.bindTools(lc::ChatModelToolBinding {
        .tools_ = {
            lc::ChatModelTool {
                .name_ = "inspect",
                .description_ = "Inspect target.",
                .inputSchema_ = lc::JsonSchema::object()
                                    .property("target", lc::JsonSchema::string(), true),
            },
        },
    });
    assert(bound.isOk());

    auto response = (*bound)->invoke({ lc::BaseMessage::human("inspect motor") });
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
    testBatchUsesInjectedTransport();
    testBindToolsBuildsOpenAIRequestAndParsesToolCalls();
    testOpenAIStreamToolCallChunks();
    testAnthropicStreamToolUseDeltas();
    testAnthropicBindToolsAndToolUseResponse();
    return 0;
}
