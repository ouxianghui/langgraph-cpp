#include <langgraph_cpp/langgraph.hpp>

#include <cassert>
#include <memory>
#include <string>

namespace {

lc::State stateFromMessages(std::vector<lc::BaseMessage> messages)
{
    auto state = lc::State::fromJsonValue({
        { "messages", lc::messagesToJson(messages) },
    });
    assert(state.isOk());
    return *state;
}

void testMessageRoundTrip()
{
    lc::BaseMessage message = lc::BaseMessage::ai(
        "calling",
        {
            lc::ToolCall {
                .id_ = "call-1",
                .name_ = "add",
                .args_ = {
                    { "a", 1 },
                    { "b", 2 },
                },
            },
        });
    message.usageMetadata_.source_ = lc::UsageMetadataSource::Provider;
    message.usageMetadata_.provider_ = "openai-compatible";
    message.usageMetadata_.model_ = "edge-model";
    message.usageMetadata_.tokens_.inputTokens_ = 2;
    message.usageMetadata_.tokens_.outputTokens_ = 3;
    message.usageMetadata_.tokens_.totalTokens_ = 5;
    message.usageMetadata_.raw_ = {
        { "prompt_tokens", 2 },
        { "completion_tokens", 3 },
    };

    auto encoded = lc::baseMessageToJson(message);
    assert(encoded.at("usage_metadata").at("input_tokens") == 2);
    assert(encoded.at("usage_metadata").at("output_tokens") == 3);
    assert(encoded.at("usage_metadata").at("total_tokens") == 5);
    assert(encoded.at("usage_metadata").at("source") == "provider");

    auto decoded = lc::baseMessageFromJson(encoded);
    assert(decoded.isOk());
    assert(*decoded == message);

    auto messages = lc::messagesFromJson(lc::messagesToJson({
        lc::BaseMessage::system("system"),
        lc::BaseMessage::human("hello"),
        message,
        lc::BaseMessage::tool("call-1", "add", R"({"ok":true})"),
    }));
    assert(messages.isOk());
    assert(messages->size() == 4);

    auto invalid = lc::baseMessageFromJson({
        { "role", "tool" },
        { "content", "{}" },
    });
    assert(!invalid.isOk());
    assert(invalid.status().code() == lc::StatusCode::InvalidArgument);
}

void testContentBlocksRoundTripAndMultimodal()
{
    lc::BaseMessage message = lc::BaseMessage::human("");
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
    message.content_ = lc::contentBlocksText(message.contentBlocks_);

    auto encoded = lc::baseMessageToJson(message);
    assert(encoded.at("content") == "Describe this image.");
    assert(encoded.at("content_blocks").at(0).at("type") == "text");
    assert(encoded.at("content_blocks").at(1).at("type") == "image");

    auto decoded = lc::baseMessageFromJson(encoded);
    assert(decoded.isOk());
    assert(decoded->content_ == "Describe this image.");
    assert(decoded->contentBlocks_.size() == 2);
    assert(decoded->contentBlocks_.at(1).at("url") == "https://example.com/image.jpg");

    auto providerNative = lc::baseMessageFromJson({
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

    lc::BaseMessage ai = lc::BaseMessage::ai(
        "",
        {
            lc::ToolCall {
                .id_ = "call-1",
                .name_ = "lookup",
                .args_ = { { "q", "edge" } },
            },
        });
    auto blocks = lc::messageContentBlocks(ai);
    assert(blocks.size() == 1);
    assert(blocks.front().at("type") == "tool_call");
    assert(blocks.front().at("args").at("q") == "edge");
}

void testFakeChatModel()
{
    lc::FakeChatModel model({
        lc::BaseMessage::ai("first"),
    });
    auto first = model.invoke({ lc::BaseMessage::human("hello") });
    assert(first.isOk());
    assert(first->content_ == "first");
    assert(model.calls() == 1);
    auto second = model.invoke({});
    assert(!second.isOk());
    assert(second.status().code() == lc::StatusCode::OutOfRange);
}

void testToolRegistry()
{
    lc::ToolRegistry registry;
    lc::Tool tool {
        .name_ = "echo",
        .description_ = "Echo input.",
        .callable_ = [](const nlohmann::json& input) -> lc::Result<nlohmann::json> {
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
    auto registry = std::make_shared<lc::ToolRegistry>();
    int calls = 0;

    auto tool = std::make_shared<lc::FunctionTool>(
        lc::ToolSpec {
            .name_ = "context.echo",
            .description_ = "Echo context.",
            .inputSchema_ = lc::JsonSchema::object()
                                .property("value", lc::JsonSchema::string(), true)
                                .additionalProperties(false),
            .outputSchema_ = lc::JsonSchema::object()
                                 .property("thread", lc::JsonSchema::string(), true)
                                 .property("value", lc::JsonSchema::string(), true)
                                 .additionalProperties(false),
        },
        [&](const lc::ToolRequest& request, lc::ToolRuntime& context) -> lc::Result<lc::ToolResult> {
            ++calls;
            assert(context.toolCallId_ == request.callId_);
            return lc::ToolResult::success({
                { "thread", context.threadId_ },
                { "value", request.arguments_.at("value") },
            });
        });
    assert(registry->add(tool).isOk());

    std::vector<lc::RuntimeEventType> events;
    lc::ToolRuntime context {
        .runId_ = "run",
        .threadId_ = "thread",
        .nodeId_ = "tools",
        .step_ = 1,
        .events_ = [&](lc::RuntimeEvent event) {
            events.push_back(event.type_);
            return lc::Status::ok();
        },
    };

    lc::ToolExecutor executor(
        registry,
        lc::ToolPolicy {
            .validateInput_ = true,
            .validateOutput_ = true,
            .authorize_ = [](const lc::ToolSpec&, const lc::ToolRequest& request, lc::ToolRuntime&) -> lc::Result<void> {
                if (request.arguments_.value("value", "") == "deny")
                    return lc::Status::permissionDenied("blocked by test policy");
                return lc::okResult();
            },
        });

    auto accepted = executor.invoke(
        lc::ToolRequest {
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
    assert(events.front() == lc::RuntimeEventType::ToolCallStarted);
    assert(events.back() == lc::RuntimeEventType::ToolCallCompleted);

    auto rejected = executor.invoke(
        lc::ToolRequest {
            .callId_ = "call-2",
            .name_ = "context.echo",
            .arguments_ = { { "value", "deny" } },
        },
        context);
    assert(rejected.isOk());
    assert(!rejected->ok_);
    assert(rejected->error_->code_ == lc::ToolErrorCode::Rejected);
    assert(calls == 1);
}

void testToolResultRoundTrip()
{
    auto success = lc::ToolResult::success({
        { "value", 42 },
    });
    auto decodedSuccess = lc::toolResultFromJson(lc::toolResultToJson(success));
    assert(decodedSuccess.isOk());
    assert(decodedSuccess->ok_);
    assert(decodedSuccess->result_.at("value") == 42);

    auto failure = lc::ToolResult::failure(
        lc::ToolErrorCode::Rejected,
        "operator rejected",
        { { "reason", "unsafe" } });
    auto decodedFailure = lc::toolResultFromJson(lc::toolResultToJson(failure));
    assert(decodedFailure.isOk());
    assert(!decodedFailure->ok_);
    assert(decodedFailure->error_.has_value());
    assert(decodedFailure->error_->code_ == lc::ToolErrorCode::Rejected);
    assert(decodedFailure->error_->details_.at("reason") == "unsafe");

    auto message = lc::toolErrorMessage(
        "call-1",
        "dangerous",
        lc::ToolErrorCode::Disabled,
        "tool is disabled");
    auto decodedMessagePayload = lc::toolResultFromJson(nlohmann::json::parse(message.content_));
    assert(decodedMessagePayload.isOk());
    assert(!decodedMessagePayload->ok_);
    assert(decodedMessagePayload->error_->code_ == lc::ToolErrorCode::Disabled);

    auto code = lc::toolErrorCodeFromName("validation_error");
    assert(code.isOk());
    assert(*code == lc::ToolErrorCode::ValidationError);
    assert(!lc::toolErrorCodeFromName("mystery").isOk());
}

void testToolValidationDoesNotCallCallable()
{
    auto registry = std::make_shared<lc::ToolRegistry>();
    int calls = 0;
    lc::Tool tool {
        .name_ = "add",
        .description_ = "Add two integers.",
        .inputSchema_ = lc::JsonSchema::object()
                            .property("a", lc::JsonSchema::integer(), true)
                            .property("b", lc::JsonSchema::integer(), true)
                            .additionalProperties(false),
        .callable_ = [&](const nlohmann::json&) -> lc::Result<nlohmann::json> {
            ++calls;
            return nlohmann::json::object();
        },
    };
    assert(registry->add(std::move(tool)).isOk());

    auto node = lc::ToolNode(registry);
    lc::Runtime context(lc::Runtime::Options {
        .runId_ = "run",
        .threadId_ = "thread",
        .step_ = 1,
        .nodeId_ = "tools",
    });
    auto output = node(
        stateFromMessages({
            lc::BaseMessage::ai(
                "",
                {
                    lc::ToolCall {
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

    auto messages = lc::messagesFromJson(output->update_.values().at("messages"));
    assert(messages.isOk());
    assert(messages->size() == 1);
    assert(messages->front().type_ == lc::MessageType::Tool);
    auto content = lc::toolResultFromJson(nlohmann::json::parse(messages->front().content_));
    assert(content.isOk());
    assert(!content->ok_);
    assert(content->error_.has_value());
    assert(content->error_->code_ == lc::ToolErrorCode::ValidationError);

    auto typeMismatch = node(
        stateFromMessages({
            lc::BaseMessage::ai(
                "",
                {
                    lc::ToolCall {
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

    auto typeMismatchMessages = lc::messagesFromJson(typeMismatch->update_.values().at("messages"));
    assert(typeMismatchMessages.isOk());
    assert(typeMismatchMessages->size() == 1);
    auto typeMismatchContent = lc::toolResultFromJson(nlohmann::json::parse(typeMismatchMessages->front().content_));
    assert(typeMismatchContent.isOk());
    assert(!typeMismatchContent->ok_);
    assert(typeMismatchContent->error_.has_value());
    assert(typeMismatchContent->error_->code_ == lc::ToolErrorCode::ValidationError);
}

void testToolCallGrammarAndParsing()
{
    auto registry = std::make_shared<lc::ToolRegistry>();
    lc::Tool addTool {
        .name_ = "add",
        .description_ = "Add two integers.",
        .inputSchema_ = lc::JsonSchema::object()
                            .property("a", lc::JsonSchema::integer(), true)
                            .property("b", lc::JsonSchema::integer(), true)
                            .additionalProperties(false),
        .callable_ = [](const nlohmann::json& input) -> lc::Result<nlohmann::json> {
            return nlohmann::json {
                { "value", input.at("a").get<int>() + input.at("b").get<int>() },
            };
        },
    };
    assert(registry->add(std::move(addTool)).isOk());

    auto grammar = lc::toolCallJsonGrammar(*registry);
    assert(grammar.isOk());
    assert(grammar->find("root ::=") != std::string::npos);
    assert(grammar->find("\\\"tool_calls\\\"") != std::string::npos);
    assert(grammar->find("\\\"args\\\"") != std::string::npos);
    assert(grammar->find("\\\"add\\\"") != std::string::npos);

    auto message = lc::assistantMessageFromToolCallJson(R"({
        "tool_calls": [
            {
                "id": "call-1",
                "name": "add",
                "args": {"a": 2, "b": 3}
            }
        ]
    })");
    assert(message.isOk());
    assert(message->type_ == lc::MessageType::AI);
    assert(message->toolCalls_.size() == 1);
    assert(message->toolCalls_.front().name_ == "add");
    assert(message->toolCalls_.front().args_.at("a") == 2);

    auto invalid = lc::assistantMessageFromToolCallJson(R"({"tool_calls": []})");
    assert(!invalid.isOk());
    assert(invalid.status().code() == lc::StatusCode::InvalidArgument);
}

void testModelToolModelLoop()
{
    auto model = std::make_shared<lc::FakeChatModel>(std::vector<lc::BaseMessage> {
        lc::BaseMessage::ai(
            "",
            {
                lc::ToolCall {
                    .id_ = "call-1",
                    .name_ = "add",
                    .args_ = {
                        { "a", 2 },
                        { "b", 3 },
                    },
                },
            }),
        lc::BaseMessage::ai("The answer is 5."),
    });

    auto registry = std::make_shared<lc::ToolRegistry>();
    lc::Tool addTool {
        .name_ = "add",
        .description_ = "Add two integers.",
        .inputSchema_ = lc::JsonSchema::object()
                            .property("a", lc::JsonSchema::integer(), true)
                            .property("b", lc::JsonSchema::integer(), true)
                            .additionalProperties(false),
        .outputSchema_ = lc::JsonSchema::object()
                             .property("value", lc::JsonSchema::integer(), true)
                             .additionalProperties(false),
        .callable_ = [](const nlohmann::json& input) -> lc::Result<nlohmann::json> {
            return nlohmann::json {
                { "value", input.at("a").get<int>() + input.at("b").get<int>() },
            };
        },
    };
    assert(registry->add(std::move(addTool)).isOk());

    lc::StateGraph graph;
    assert(graph.addNode("model", lc::makeModelNode(model)).isOk());
    assert(graph.addNode("tools", lc::ToolNode(registry, lc::ToolNodeOptions { .validateOutput_ = true })).isOk());
    assert(graph.addEdge(std::string(lc::START), "model").isOk());
    assert(graph.addConditionalEdges(
        "model",
        [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::NodeId> {
            if (lc::toolsCondition(state))
                return std::string("tools");
            return std::string(lc::END);
        },
        { "tools", std::string(lc::END) }).isOk());
    assert(graph.addEdge("tools", "model").isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    lc::RunOptions options;
    options.threadId_ = "agent-loop";
    options.checkpointer_ = std::make_shared<lc::InMemorySaver>();
    options.reducers_.set("messages", lc::ReducerKind::AddMessages);

    auto result = compiled->invoke(
        stateFromMessages({ lc::BaseMessage::human("What is 2 + 3?") }),
        options);
    assert(result.isOk());
    assert(result->step_ == 3);
    assert(model->calls() == 2);

    auto finalState = result->state_.toJson();
    assert(finalState.isOk());
    auto messages = lc::messagesFromStateJson(*finalState);
    assert(messages.isOk());
    assert(messages->size() == 4);
    assert(messages->at(0).type_ == lc::MessageType::Human);
    assert(messages->at(1).toolCalls_.size() == 1);
    assert(messages->at(2).type_ == lc::MessageType::Tool);
    assert(messages->at(3).content_ == "The answer is 5.");

    auto toolPayload = lc::toolResultFromJson(nlohmann::json::parse(messages->at(2).content_));
    assert(toolPayload.isOk());
    assert(toolPayload->ok_);
    assert(toolPayload->result_.at("value") == 5);

    auto checkpoints = options.checkpointer_->list(lc::CheckpointListOptions {
        .threadId_ = "agent-loop",
        .checkpointNamespace_ = std::string(),
        .order_ = lc::CheckpointListOrder::OldestFirst,
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
