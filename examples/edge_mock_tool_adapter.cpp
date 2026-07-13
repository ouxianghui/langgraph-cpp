#include <langgraph_cpp/langgraph.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>

namespace {

class IEdgeDeviceAdapter {
public:
    virtual ~IEdgeDeviceAdapter() = default;

    [[nodiscard]] virtual lc::Result<nlohmann::json> readTemperature(
        const nlohmann::json& input) = 0;
    [[nodiscard]] virtual lc::Result<nlohmann::json> setRelay(
        const nlohmann::json& input) = 0;
};

class MockEdgeDeviceAdapter final : public IEdgeDeviceAdapter {
public:
    [[nodiscard]] lc::Result<nlohmann::json> readTemperature(
        const nlohmann::json& input) override
    {
        return nlohmann::json {
            { "sensor", input.at("sensor").get<std::string>() },
            { "celsius", 72.5 },
            { "healthy", false },
        };
    }

    [[nodiscard]] lc::Result<nlohmann::json> setRelay(
        const nlohmann::json& input) override
    {
        relayState_ = input.at("enabled").get<bool>();
        return nlohmann::json {
            { "relay", input.at("relay").get<std::string>() },
            { "enabled", relayState_ },
        };
    }

private:
    bool relayState_ { false };
};

void require(lc::Result<void> result)
{
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        std::exit(1);
    }
}

std::shared_ptr<lc::ToolRegistry> makeEdgeToolRegistry(
    std::shared_ptr<IEdgeDeviceAdapter> adapter)
{
    auto registry = std::make_shared<lc::ToolRegistry>();

    require(registry->add(lc::Tool {
        .name_ = "edge.read_temperature",
        .description_ = "Read a temperature sensor on an edge device.",
        .inputSchema_ = lc::JsonSchema::object()
                            .property("sensor", lc::JsonSchema::string(), true)
                            .additionalProperties(false),
        .outputSchema_ = lc::JsonSchema::object()
                             .property("sensor", lc::JsonSchema::string(), true)
                             .property("celsius", lc::JsonSchema::number(), true)
                             .property("healthy", lc::JsonSchema::boolean(), true)
                             .additionalProperties(false),
        .callable_ = [device = adapter](
                         const nlohmann::json& input) -> lc::Result<nlohmann::json> {
            return device->readTemperature(input);
        },
    }));

    require(registry->add(lc::Tool {
        .name_ = "edge.set_relay",
        .description_ = "Set a relay on an edge device.",
        .inputSchema_ = lc::JsonSchema::object()
                            .property("relay", lc::JsonSchema::string(), true)
                            .property("enabled", lc::JsonSchema::boolean(), true)
                            .additionalProperties(false),
        .outputSchema_ = lc::JsonSchema::object()
                             .property("relay", lc::JsonSchema::string(), true)
                             .property("enabled", lc::JsonSchema::boolean(), true)
                             .additionalProperties(false),
        .callable_ = [device = adapter](
                         const nlohmann::json& input) -> lc::Result<nlohmann::json> {
            return device->setRelay(input);
        },
    }));

    return registry;
}

} // namespace

int main()
{
    auto registry = makeEdgeToolRegistry(std::make_shared<MockEdgeDeviceAdapter>());

    lc::StateGraph graph;
    require(graph.addNode(
        "tools",
        lc::ToolNode(registry, lc::ToolNodeOptions {
            .validateOutput_ = true,
        })));
    require(graph.addEdge(std::string(lc::START), "tools"));
    require(graph.addEdge("tools", std::string(lc::END)));

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    auto input = lc::State::fromJsonValue({
        { "messages", lc::messagesToJson({
            lc::BaseMessage::human("Diagnose the cooling loop."),
            lc::BaseMessage::ai(
                "",
                {
                    lc::ToolCall {
                        .id_ = "call-temperature",
                        .name_ = "edge.read_temperature",
                        .args_ = {
                            { "sensor", "cooling-loop" },
                        },
                    },
                    lc::ToolCall {
                        .id_ = "call-relay",
                        .name_ = "edge.set_relay",
                        .args_ = {
                            { "relay", "cooling-fan" },
                            { "enabled", true },
                        },
                    },
                }),
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
