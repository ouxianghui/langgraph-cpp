#include <langgraph_cpp/langgraph.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

void require(lc::Result<void> result)
{
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        std::exit(1);
    }
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
        .callable_ = [](const nlohmann::json& input) -> lc::Result<nlohmann::json> {
            return nlohmann::json {
                { "value", input.at("a").get<int>() + input.at("b").get<int>() },
            };
        },
    };
    require(registry->add(std::move(addTool)));

    lc::StateGraph graph;
    require(graph.addNode("model", lc::makeModelNode(model)));
    require(graph.addNode("tools", lc::ToolNode(registry)));
    require(graph.addEdge(std::string(lc::START), "model"));
    require(graph.addConditionalEdges(
        "model",
        [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::NodeId> {
            if (lc::toolsCondition(state))
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

    auto input = lc::State::fromJsonValue({
        { "messages", lc::messagesToJson({
            lc::BaseMessage::human("What is 2 + 3?"),
        }) },
    });
    if (!input.isOk()) {
        std::cerr << input.status() << '\n';
        return 1;
    }

    lc::RunOptions options;
    options.reducers_.set("messages", lc::ReducerKind::AddMessages);

    auto result = compiled->invoke(*input, options);
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        return 1;
    }

    std::cout << result->state_.json() << '\n';
    return 0;
}
