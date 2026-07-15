#include <langgraph_cpp/langgraph.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

namespace {

void require(lgc::Result<void> result)
{
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        std::exit(1);
    }
}

lgc::Result<std::vector<lgc::ToolCall>> latestToolCalls(const lgc::State& state)
{
    auto json = state.toJson();
    if (!json.isOk())
        return json.status();
    auto messages = lgc::messagesFromStateJson(*json);
    if (!messages.isOk())
        return messages.status();

    for (auto it = messages->rbegin(); it != messages->rend(); ++it) {
        if (it->type_ == lgc::MessageType::AI)
            return it->toolCalls_;
    }
    return std::vector<lgc::ToolCall> {};
}

} // namespace

int main()
{
    auto model = std::make_shared<lgc::FakeChatModel>(std::vector<lgc::BaseMessage> {
        lgc::BaseMessage::ai(
            "",
            {
                lgc::ToolCall {
                    .id_ = "call-1",
                    .name_ = "add",
                    .args_ = {
                        { "a", 8 },
                        { "b", 13 },
                    },
                },
            }),
        lgc::BaseMessage::ai("Approved tool result: 21."),
    });

    auto registry = std::make_shared<lgc::ToolRegistry>();
    require(registry->add(lgc::Tool {
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
    }));

    lgc::StateGraph graph;
    require(graph.addNode("model", lgc::makeModelNode(model)));
    require(graph.addNode("approve_tool", [](const lgc::State& state, lgc::Runtime& context) -> lgc::Result<lgc::NodeOutput> {
        auto toolCalls = latestToolCalls(state);
        if (!toolCalls.isOk())
            return toolCalls.status();
        if (toolCalls->empty())
            return lgc::NodeOutput::update(lgc::StateUpdate::empty());

        if (!context.hasResumeValue()) {
            nlohmann::json calls = nlohmann::json::array();
            for (const auto& toolCall : *toolCalls)
                calls.push_back(lgc::toolCallToJson(toolCall));
            return lgc::NodeOutput::interrupt(lgc::Interrupt {
                .id_ = "approve_tool_calls",
                .value_ = {
                    { "tool_calls", std::move(calls) },
                },
            });
        }

        const bool approved = context.resumeValue().value("approved", false);
        nlohmann::json update {
            { "tool_approved", approved },
        };

        if (!approved) {
            nlohmann::json rejectedMessages = nlohmann::json::array();
            for (const auto& toolCall : *toolCalls) {
                rejectedMessages.push_back(lgc::baseMessageToJson(lgc::BaseMessage::tool(
                    toolCall.id_,
                    toolCall.name_,
                    lgc::toolResultToJson(lgc::ToolResult::failure(
                        lgc::ToolErrorCode::Rejected,
                        "tool call rejected by operator")).dump())));
            }
            update["messages"] = std::move(rejectedMessages);
        }

        auto stateUpdate = lgc::StateUpdate::fromJsonValue(update);
        if (!stateUpdate.isOk())
            return stateUpdate.status();
        return lgc::NodeOutput::update(std::move(*stateUpdate));
    }));
    require(graph.addNode("tools", lgc::ToolNode(registry)));
    require(graph.addEdge(std::string(lgc::START), "model"));
    require(graph.addConditionalEdges(
        "model",
        [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::NodeId> {
            if (lgc::toolsCondition(state))
                return std::string("approve_tool");
            return std::string(lgc::END);
        },
        { "approve_tool", std::string(lgc::END) }));
    require(graph.addConditionalEdges(
        "approve_tool",
        [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::NodeId> {
            auto json = state.toJson();
            if (!json.isOk())
                return json.status();
            if (json->value("tool_approved", false))
                return std::string("tools");
            return std::string(lgc::END);
        },
        { "tools", std::string(lgc::END) }));
    require(graph.addEdge("tools", "model"));

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    auto checkpointer = std::make_shared<lgc::InMemorySaver>();
    lgc::RunOptions options;
    options.threadId_ = "tool-approval-demo";
    options.checkpointer_ = checkpointer;
    options.reducers_.set("messages", lgc::ReducerKind::AddMessages);

    auto input = lgc::State::fromJsonValue({
        { "messages", lgc::messagesToJson({ lgc::BaseMessage::human("Add 8 and 13.") }) },
    });
    auto paused = compiled->invoke(*input, options);
    if (!paused.isOk()) {
        std::cerr << paused.status() << '\n';
        return 1;
    }

    lgc::RunOptions resumeOptions;
    resumeOptions.checkpointer_ = checkpointer;
    resumeOptions.reducers_.set("messages", lgc::ReducerKind::AddMessages);
    resumeOptions.command_ = lgc::Command::resume({
        { "approved", true },
    });

    auto result = compiled->resume("tool-approval-demo", resumeOptions);
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        return 1;
    }

    std::cout << result->state_.json() << '\n';
    return 0;
}
