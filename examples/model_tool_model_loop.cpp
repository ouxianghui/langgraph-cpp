#include <langgraph_cpp/langgraph.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

void require(lgc::Result<void> result)
{
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        std::exit(1);
    }
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
        .callable_ = [](const nlohmann::json& input) -> lgc::Result<nlohmann::json> {
            return nlohmann::json {
                { "value", input.at("a").get<int>() + input.at("b").get<int>() },
            };
        },
    };
    require(registry->add(std::move(addTool)));

    lgc::StateGraph graph;
    require(graph.addNode("model", lgc::makeModelNode(model)));
    require(graph.addNode("tools", lgc::ToolNode(registry)));
    require(graph.addEdge(std::string(lgc::START), "model"));
    require(graph.addConditionalEdges(
        "model",
        [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::NodeId> {
            if (lgc::toolsCondition(state))
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

    auto input = lgc::State::fromJsonValue({
        { "messages", lgc::messagesToJson({
            lgc::BaseMessage::human("What is 2 + 3?"),
        }) },
    });
    if (!input.isOk()) {
        std::cerr << input.status() << '\n';
        return 1;
    }

    lgc::RunOptions options;
    options.reducers_.set("messages", lgc::ReducerKind::AddMessages);

    auto result = compiled->invoke(*input, options);
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        return 1;
    }

    std::cout << result->state_.json() << '\n';
    return 0;
}
