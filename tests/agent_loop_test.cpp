#include <langgraph_cpp/langgraph.hpp>

#include <cassert>
#include <memory>
#include <string>

namespace {

lgc::State stateFromMessages(std::vector<lgc::BaseMessage> messages)
{
    auto state = lgc::State::fromJsonValue({
        { "messages", lgc::messagesToJson(messages) },
    });
    assert(state.isOk());
    return *state;
}

void testMessageRoundTrip()
{
    lgc::BaseMessage message = lgc::BaseMessage::ai(
        "calling",
        {
            lgc::ToolCall {
                .id_ = "call-1",
                .name_ = "add",
                .args_ = {
                    { "a", 1 },
                    { "b", 2 },
                },
            },
        });
    message.usageMetadata_.source_ = lgc::UsageMetadataSource::Provider;
    message.usageMetadata_.provider_ = "openai-compatible";
    message.usageMetadata_.model_ = "edge-model";
    message.usageMetadata_.tokens_.inputTokens_ = 2;
    message.usageMetadata_.tokens_.outputTokens_ = 3;
    message.usageMetadata_.tokens_.totalTokens_ = 5;
    message.usageMetadata_.raw_ = {
        { "prompt_tokens", 2 },
        { "completion_tokens", 3 },
    };

    auto encoded = lgc::baseMessageToJson(message);
    assert(encoded.at("usage_metadata").at("input_tokens") == 2);
    assert(encoded.at("usage_metadata").at("output_tokens") == 3);
    assert(encoded.at("usage_metadata").at("total_tokens") == 5);
    assert(encoded.at("usage_metadata").at("source") == "provider");

    auto decoded = lgc::baseMessageFromJson(encoded);
    assert(decoded.isOk());
    assert(*decoded == message);

    auto messages = lgc::messagesFromJson(lgc::messagesToJson({
        lgc::BaseMessage::system("system"),
        lgc::BaseMessage::human("hello"),
        message,
        lgc::BaseMessage::tool("call-1", "add", R"({"ok":true})"),
    }));
    assert(messages.isOk());
    assert(messages->size() == 4);

    auto invalid = lgc::baseMessageFromJson({
        { "role", "tool" },
        { "content", "{}" },
    });
    assert(!invalid.isOk());
    assert(invalid.status().code() == lgc::StatusCode::InvalidArgument);
}

void testContentBlocksRoundTripAndMultimodal()
{
    lgc::BaseMessage message = lgc::BaseMessage::human("");
    message.contentBlocks_ = {
        {
            { "type", "text" },
            { "text", "Describe this image." },
        },
        {
            { "type", "image" },
            { "url", "https://example.com/image.jpg" },
        },
    };
    message.content_ = lgc::contentBlocksText(message.contentBlocks_);

    auto encoded = lgc::baseMessageToJson(message);
    assert(encoded.at("content") == "Describe this image.");
    assert(encoded.at("content_blocks").at(0).at("type") == "text");
    assert(encoded.at("content_blocks").at(1).at("type") == "image");

    auto decoded = lgc::baseMessageFromJson(encoded);
    assert(decoded.isOk());
    assert(decoded->content_ == "Describe this image.");
    assert(decoded->contentBlocks_.size() == 2);
    assert(decoded->contentBlocks_.at(1).at("url") == "https://example.com/image.jpg");

    auto providerNative = lgc::baseMessageFromJson({
        { "role", "user" },
        { "content", nlohmann::json::array({
            {
                { "type", "text" },
                { "text", "Look" },
            },
            {
                { "type", "image_url" },
                { "image_url", {
                    { "url", "https://example.com/native.jpg" },
                } },
            },
        }) },
    });
    assert(providerNative.isOk());
    assert(providerNative->content_ == "Look");
    assert(providerNative->contentBlocks_.at(1).at("type") == "image");
    assert(providerNative->contentBlocks_.at(1).at("url") == "https://example.com/native.jpg");

    lgc::BaseMessage ai = lgc::BaseMessage::ai(
        "",
        {
            lgc::ToolCall {
                .id_ = "call-1",
                .name_ = "lookup",
                .args_ = { { "q", "edge" } },
            },
        });
    auto blocks = lgc::messageContentBlocks(ai);
    assert(blocks.size() == 1);
    assert(blocks.front().at("type") == "tool_call");
    assert(blocks.front().at("args").at("q") == "edge");
}

void testFakeChatModel()
{
    lgc::FakeChatModel model({
        lgc::BaseMessage::ai("first"),
    });
    auto first = model.invoke({ lgc::BaseMessage::human("hello") });
    assert(first.isOk());
    assert(first->content_ == "first");
    assert(model.calls() == 1);
    auto second = model.invoke({});
    assert(!second.isOk());
    assert(second.status().code() == lgc::StatusCode::OutOfRange);
}

void testToolRegistry()
{
    lgc::ToolRegistry registry;
    lgc::Tool tool {
        .name_ = "echo",
        .description_ = "Echo input.",
        .callable_ = [](const nlohmann::json& input) -> lgc::Result<nlohmann::json> {
            return input;
        },
    };
    assert(registry.add(tool).isOk());
    assert(registry.has("echo"));
    assert(!registry.add(tool).isOk());
    assert(registry.disable("echo").isOk());
    auto disabled = registry.spec("echo");
    assert(disabled.isOk());
    assert(!disabled->enabled_);
    assert(!registry.tool("missing").isOk());
}

void testToolExecutorPolicyAndContext()
{
    auto registry = std::make_shared<lgc::ToolRegistry>();
    int calls = 0;

    auto tool = std::make_shared<lgc::FunctionTool>(
        lgc::ToolSpec {
            .name_ = "context.echo",
            .description_ = "Echo context.",
            .inputSchema_ = lgc::JsonSchema::object()
                                .property("value", lgc::JsonSchema::string(), true)
                                .additionalProperties(false),
            .outputSchema_ = lgc::JsonSchema::object()
                                 .property("thread", lgc::JsonSchema::string(), true)
                                 .property("value", lgc::JsonSchema::string(), true)
                                 .additionalProperties(false),
        },
        [&](const lgc::ToolRequest& request, lgc::ToolRuntime& context) -> lgc::Result<lgc::ToolResult> {
            ++calls;
            assert(context.toolCallId_ == request.callId_);
            return lgc::ToolResult::success({
                { "thread", context.threadId_ },
                { "value", request.arguments_.at("value") },
            });
        });
    assert(registry->add(tool).isOk());

    std::vector<lgc::RuntimeEventType> events;
    lgc::ToolRuntime context {
        .runId_ = "run",
        .threadId_ = "thread",
        .nodeId_ = "tools",
        .step_ = 1,
        .events_ = [&](lgc::RuntimeEvent event) {
            events.push_back(event.type_);
            return lgc::Status::ok();
        },
    };

    lgc::ToolExecutor executor(
        registry,
        lgc::ToolPolicy {
            .validateInput_ = true,
            .validateOutput_ = true,
            .authorize_ = [](const lgc::ToolSpec&, const lgc::ToolRequest& request, lgc::ToolRuntime&) -> lgc::Result<void> {
                if (request.arguments_.value("value", "") == "deny")
                    return lgc::Status::permissionDenied("blocked by test policy");
                return lgc::okResult();
            },
        });

    auto accepted = executor.invoke(
        lgc::ToolRequest {
            .callId_ = "call-1",
            .name_ = "context.echo",
            .arguments_ = { { "value", "ok" } },
        },
        context);
    assert(accepted.isOk());
    assert(accepted->ok_);
    assert(accepted->result_.at("thread") == "thread");
    assert(calls == 1);
    assert(events.size() == 2);
    assert(events.front() == lgc::RuntimeEventType::ToolCallStarted);
    assert(events.back() == lgc::RuntimeEventType::ToolCallCompleted);

    auto rejected = executor.invoke(
        lgc::ToolRequest {
            .callId_ = "call-2",
            .name_ = "context.echo",
            .arguments_ = { { "value", "deny" } },
        },
        context);
    assert(rejected.isOk());
    assert(!rejected->ok_);
    assert(rejected->error_->code_ == lgc::ToolErrorCode::Rejected);
    assert(calls == 1);
}

void testToolResultRoundTrip()
{
    auto success = lgc::ToolResult::success({
        { "value", 42 },
    });
    auto decodedSuccess = lgc::toolResultFromJson(lgc::toolResultToJson(success));
    assert(decodedSuccess.isOk());
    assert(decodedSuccess->ok_);
    assert(decodedSuccess->result_.at("value") == 42);

    auto failure = lgc::ToolResult::failure(
        lgc::ToolErrorCode::Rejected,
        "operator rejected",
        { { "reason", "unsafe" } });
    auto decodedFailure = lgc::toolResultFromJson(lgc::toolResultToJson(failure));
    assert(decodedFailure.isOk());
    assert(!decodedFailure->ok_);
    assert(decodedFailure->error_.has_value());
    assert(decodedFailure->error_->code_ == lgc::ToolErrorCode::Rejected);
    assert(decodedFailure->error_->details_.at("reason") == "unsafe");

    auto message = lgc::toolErrorMessage(
        "call-1",
        "dangerous",
        lgc::ToolErrorCode::Disabled,
        "tool is disabled");
    auto decodedMessagePayload = lgc::toolResultFromJson(nlohmann::json::parse(message.content_));
    assert(decodedMessagePayload.isOk());
    assert(!decodedMessagePayload->ok_);
    assert(decodedMessagePayload->error_->code_ == lgc::ToolErrorCode::Disabled);

    auto code = lgc::toolErrorCodeFromName("validation_error");
    assert(code.isOk());
    assert(*code == lgc::ToolErrorCode::ValidationError);
    assert(!lgc::toolErrorCodeFromName("mystery").isOk());
}

void testToolValidationDoesNotCallCallable()
{
    auto registry = std::make_shared<lgc::ToolRegistry>();
    int calls = 0;
    lgc::Tool tool {
        .name_ = "add",
        .description_ = "Add two integers.",
        .inputSchema_ = lgc::JsonSchema::object()
                            .property("a", lgc::JsonSchema::integer(), true)
                            .property("b", lgc::JsonSchema::integer(), true)
                            .additionalProperties(false),
        .callable_ = [&](const nlohmann::json&) -> lgc::Result<nlohmann::json> {
            ++calls;
            return nlohmann::json::object();
        },
    };
    assert(registry->add(std::move(tool)).isOk());

    auto node = lgc::ToolNode(registry);
    lgc::Runtime context(lgc::Runtime::Options {
        .runId_ = "run",
        .threadId_ = "thread",
        .step_ = 1,
        .nodeId_ = "tools",
    });
    auto output = node(
        stateFromMessages({
            lgc::BaseMessage::ai(
                "",
                {
                    lgc::ToolCall {
                        .id_ = "call-1",
                        .name_ = "add",
                        .args_ = {
                            { "a", 1 },
                        },
                    },
                }),
        }),
        context);
    assert(output.isOk());
    assert(!output->command_.has_value());
    assert(calls == 0);

    auto messages = lgc::messagesFromJson(output->update_.values().at("messages"));
    assert(messages.isOk());
    assert(messages->size() == 1);
    assert(messages->front().type_ == lgc::MessageType::Tool);
    auto content = lgc::toolResultFromJson(nlohmann::json::parse(messages->front().content_));
    assert(content.isOk());
    assert(!content->ok_);
    assert(content->error_.has_value());
    assert(content->error_->code_ == lgc::ToolErrorCode::ValidationError);

    auto typeMismatch = node(
        stateFromMessages({
            lgc::BaseMessage::ai(
                "",
                {
                    lgc::ToolCall {
                        .id_ = "call-2",
                        .name_ = "add",
                        .args_ = {
                            { "a", "1" },
                            { "b", 2 },
                        },
                    },
                }),
        }),
        context);
    assert(typeMismatch.isOk());
    assert(!typeMismatch->command_.has_value());
    assert(calls == 0);

    auto typeMismatchMessages = lgc::messagesFromJson(typeMismatch->update_.values().at("messages"));
    assert(typeMismatchMessages.isOk());
    assert(typeMismatchMessages->size() == 1);
    auto typeMismatchContent = lgc::toolResultFromJson(nlohmann::json::parse(typeMismatchMessages->front().content_));
    assert(typeMismatchContent.isOk());
    assert(!typeMismatchContent->ok_);
    assert(typeMismatchContent->error_.has_value());
    assert(typeMismatchContent->error_->code_ == lgc::ToolErrorCode::ValidationError);
}

void testToolCallGrammarAndParsing()
{
    auto registry = std::make_shared<lgc::ToolRegistry>();
    lgc::Tool addTool {
        .name_ = "add",
        .description_ = "Add two integers.",
        .inputSchema_ = lgc::JsonSchema::object()
                            .property("a", lgc::JsonSchema::integer(), true)
                            .property("b", lgc::JsonSchema::integer(), true)
                            .additionalProperties(false),
        .callable_ = [](const nlohmann::json& input) -> lgc::Result<nlohmann::json> {
            return nlohmann::json {
                { "value", input.at("a").get<int>() + input.at("b").get<int>() },
            };
        },
    };
    assert(registry->add(std::move(addTool)).isOk());

    auto grammar = lgc::toolCallJsonGrammar(*registry);
    assert(grammar.isOk());
    assert(grammar->find("root ::=") != std::string::npos);
    assert(grammar->find("\\\"tool_calls\\\"") != std::string::npos);
    assert(grammar->find("\\\"args\\\"") != std::string::npos);
    assert(grammar->find("\\\"add\\\"") != std::string::npos);

    auto message = lgc::assistantMessageFromToolCallJson(R"({
        "tool_calls": [
            {
                "id": "call-1",
                "name": "add",
                "args": {"a": 2, "b": 3}
            }
        ]
    })");
    assert(message.isOk());
    assert(message->type_ == lgc::MessageType::AI);
    assert(message->toolCalls_.size() == 1);
    assert(message->toolCalls_.front().name_ == "add");
    assert(message->toolCalls_.front().args_.at("a") == 2);

    auto invalid = lgc::assistantMessageFromToolCallJson(R"({"tool_calls": []})");
    assert(!invalid.isOk());
    assert(invalid.status().code() == lgc::StatusCode::InvalidArgument);
}

void testModelToolModelLoop()
{
    auto model = std::make_shared<lgc::FakeChatModel>(std::vector<lgc::BaseMessage> {
        lgc::BaseMessage::ai(
            "",
            {
                lgc::ToolCall {
                    .id_ = "call-1",
                    .name_ = "add",
                    .args_ = {
                        { "a", 2 },
                        { "b", 3 },
                    },
                },
            }),
        lgc::BaseMessage::ai("The answer is 5."),
    });

    auto registry = std::make_shared<lgc::ToolRegistry>();
    lgc::Tool addTool {
        .name_ = "add",
        .description_ = "Add two integers.",
        .inputSchema_ = lgc::JsonSchema::object()
                            .property("a", lgc::JsonSchema::integer(), true)
                            .property("b", lgc::JsonSchema::integer(), true)
                            .additionalProperties(false),
        .outputSchema_ = lgc::JsonSchema::object()
                             .property("value", lgc::JsonSchema::integer(), true)
                             .additionalProperties(false),
        .callable_ = [](const nlohmann::json& input) -> lgc::Result<nlohmann::json> {
            return nlohmann::json {
                { "value", input.at("a").get<int>() + input.at("b").get<int>() },
            };
        },
    };
    assert(registry->add(std::move(addTool)).isOk());

    lgc::StateGraph graph;
    assert(graph.addNode("model", lgc::makeModelNode(model)).isOk());
    assert(graph.addNode("tools", lgc::ToolNode(registry, lgc::ToolNodeOptions { .validateOutput_ = true })).isOk());
    assert(graph.addEdge(std::string(lgc::START), "model").isOk());
    assert(graph.addConditionalEdges(
        "model",
        [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::NodeId> {
            if (lgc::toolsCondition(state))
                return std::string("tools");
            return std::string(lgc::END);
        },
        { "tools", std::string(lgc::END) }).isOk());
    assert(graph.addEdge("tools", "model").isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    lgc::RunOptions options;
    options.threadId_ = "agent-loop";
    options.checkpointer_ = std::make_shared<lgc::InMemorySaver>();
    options.reducers_.set("messages", lgc::ReducerKind::AddMessages);

    auto result = compiled->invoke(
        stateFromMessages({ lgc::BaseMessage::human("What is 2 + 3?") }),
        options);
    assert(result.isOk());
    assert(result->step_ == 3);
    assert(model->calls() == 2);

    auto finalState = result->state_.toJson();
    assert(finalState.isOk());
    auto messages = lgc::messagesFromStateJson(*finalState);
    assert(messages.isOk());
    assert(messages->size() == 4);
    assert(messages->at(0).type_ == lgc::MessageType::Human);
    assert(messages->at(1).toolCalls_.size() == 1);
    assert(messages->at(2).type_ == lgc::MessageType::Tool);
    assert(messages->at(3).content_ == "The answer is 5.");

    auto toolPayload = lgc::toolResultFromJson(nlohmann::json::parse(messages->at(2).content_));
    assert(toolPayload.isOk());
    assert(toolPayload->ok_);
    assert(toolPayload->result_.at("value") == 5);

    auto checkpoints = options.checkpointer_->list(lgc::CheckpointListOptions {
        .threadId_ = "agent-loop",
        .checkpointNamespace_ = std::string(),
        .order_ = lgc::CheckpointListOrder::OldestFirst,
    });
    assert(checkpoints.isOk());
    assert(checkpoints->size() == 4);
    assert(checkpoints->back().checkpoint_.nextNodes_.empty());
}

} // namespace

int main()
{
    testMessageRoundTrip();
    testContentBlocksRoundTripAndMultimodal();
    testFakeChatModel();
    testToolRegistry();
    testToolExecutorPolicyAndContext();
    testToolResultRoundTrip();
    testToolValidationDoesNotCallCallable();
    testToolCallGrammarAndParsing();
    testModelToolModelLoop();
    return 0;
}
