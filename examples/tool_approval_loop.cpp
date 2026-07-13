#include <langgraph_cpp/langgraph.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

namespace {

void require(lc::Result<void> result)
{
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        std::exit(1);
    }
}

lc::Result<std::vector<lc::ToolCall>> latestToolCalls(const lc::State& state)
{
    auto json = state.toJson();
    if (!json.isOk())
        return json.status();
    auto messages = lc::messagesFromStateJson(*json);
    if (!messages.isOk())
        return messages.status();

    for (auto it = messages->rbegin(); it != messages->rend(); ++it) {
        if (it->type_ == lc::MessageType::AI)
            return it->toolCalls_;
    }
    return std::vector<lc::ToolCall> {};
}

} // namespace

int main()
{
    auto model = std::make_shared<lc::FakeChatModel>(std::vector<lc::BaseMessage> {
        lc::BaseMessage::ai(
            "",
            {
                lc::ToolCall {
                    .id_ = "call-1",
                    .name_ = "add",
                    .args_ = {
                        { "a", 8 },
                        { "b", 13 },
                    },
                },
            }),
        lc::BaseMessage::ai("Approved tool result: 21."),
    });

    auto registry = std::make_shared<lc::ToolRegistry>();
    require(registry->add(lc::Tool {
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
    }));

    lc::StateGraph graph;
    require(graph.addNode("model", lc::makeModelNode(model)));
    require(graph.addNode("approve_tool", [](const lc::State& state, lc::Runtime& context) -> lc::Result<lc::NodeOutput> {
        auto toolCalls = latestToolCalls(state);
        if (!toolCalls.isOk())
            return toolCalls.status();
        if (toolCalls->empty())
            return lc::NodeOutput::update(lc::StateUpdate::empty());

        if (!context.hasResumeValue()) {
            nlohmann::json calls = nlohmann::json::array();
            for (const auto& toolCall : *toolCalls)
                calls.push_back(lc::toolCallToJson(toolCall));
            return lc::NodeOutput::interrupt(lc::Interrupt {
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
                rejectedMessages.push_back(lc::baseMessageToJson(lc::BaseMessage::tool(
                    toolCall.id_,
                    toolCall.name_,
                    lc::toolResultToJson(lc::ToolResult::failure(
                        lc::ToolErrorCode::Rejected,
                        "tool call rejected by operator")).dump())));
            }
            update["messages"] = std::move(rejectedMessages);
        }

        auto stateUpdate = lc::StateUpdate::fromJsonValue(update);
        if (!stateUpdate.isOk())
            return stateUpdate.status();
        return lc::NodeOutput::update(std::move(*stateUpdate));
    }));
    require(graph.addNode("tools", lc::ToolNode(registry)));
    require(graph.addEdge(std::string(lc::START), "model"));
    require(graph.addConditionalEdges(
        "model",
        [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::NodeId> {
            if (lc::toolsCondition(state))
                return std::string("approve_tool");
            return std::string(lc::END);
        },
        { "approve_tool", std::string(lc::END) }));
    require(graph.addConditionalEdges(
        "approve_tool",
        [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::NodeId> {
            auto json = state.toJson();
            if (!json.isOk())
                return json.status();
            if (json->value("tool_approved", false))
                return std::string("tools");
            return std::string(lc::END);
        },
        { "tools", std::string(lc::END) }));
    require(graph.addEdge("tools", "model"));

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions options;
    options.threadId_ = "tool-approval-demo";
    options.checkpointer_ = checkpointer;
    options.reducers_.set("messages", lc::ReducerKind::AddMessages);

    auto input = lc::State::fromJsonValue({
        { "messages", lc::messagesToJson({ lc::BaseMessage::human("Add 8 and 13.") }) },
    });
    auto paused = compiled->invoke(*input, options);
    if (!paused.isOk()) {
        std::cerr << paused.status() << '\n';
        return 1;
    }

    lc::RunOptions resumeOptions;
    resumeOptions.checkpointer_ = checkpointer;
    resumeOptions.reducers_.set("messages", lc::ReducerKind::AddMessages);
    resumeOptions.command_ = lc::Command::resume({
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
