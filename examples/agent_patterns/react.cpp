#include <langgraph_cpp/langgraph.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

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
            "Reasoning note: inspect live sensor data before answering.",
            {
                lgc::ToolCall {
                    .id_ = "call-sensor-1",
                    .name_ = "read_sensor",
                    .args_ = {
                        { "sensor", "cooling-loop-temperature" },
                    },
                },
            }),
        lgc::BaseMessage::ai(
            "Final: cooling-loop-temperature is 42.5 C; schedule a cooling inspection."),
    });

    auto registry = std::make_shared<lgc::ToolRegistry>();
    lgc::Tool sensorTool {
        .name_ = "read_sensor",
        .description_ = "Read a named lab device sensor.",
        .inputSchema_ = lgc::JsonSchema::object()
                            .property("sensor", lgc::JsonSchema::string(), true)
                            .additionalProperties(false),
        .callable_ = [](const nlohmann::json& input) -> lgc::Result<nlohmann::json> {
            return nlohmann::json {
                { "sensor", input.at("sensor").get<std::string>() },
                { "reading_celsius", 42.5 },
                { "status", "warm" },
            };
        },
    };
    require(registry->add(std::move(sensorTool)));

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
        { "pattern", "react" },
        { "messages", lgc::messagesToJson({
                          lgc::BaseMessage::human(
                              "Check the cooling loop and recommend the next action."),
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
