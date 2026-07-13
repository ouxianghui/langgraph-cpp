#include <langgraph_cpp/langgraph.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

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
            "Reasoning note: inspect live sensor data before answering.",
            {
                lc::ToolCall {
                    .id_ = "call-sensor-1",
                    .name_ = "read_sensor",
                    .args_ = {
                        { "sensor", "cooling-loop-temperature" },
                    },
                },
            }),
        lc::BaseMessage::ai(
            "Final: cooling-loop-temperature is 42.5 C; schedule a cooling inspection."),
    });

    auto registry = std::make_shared<lc::ToolRegistry>();
    lc::Tool sensorTool {
        .name_ = "read_sensor",
        .description_ = "Read a named lab device sensor.",
        .inputSchema_ = lc::JsonSchema::object()
                            .property("sensor", lc::JsonSchema::string(), true)
                            .additionalProperties(false),
        .callable_ = [](const nlohmann::json& input) -> lc::Result<nlohmann::json> {
            return nlohmann::json {
                { "sensor", input.at("sensor").get<std::string>() },
                { "reading_celsius", 42.5 },
                { "status", "warm" },
            };
        },
    };
    require(registry->add(std::move(sensorTool)));

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
        { "pattern", "react" },
        { "messages", lc::messagesToJson({
                          lc::BaseMessage::human(
                              "Check the cooling loop and recommend the next action."),
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
