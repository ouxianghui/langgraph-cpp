#include <langgraph_cpp/langgraph.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>

namespace {

class IEdgeDeviceAdapter {
public:
    virtual ~IEdgeDeviceAdapter() = default;

    [[nodiscard]] virtual lgc::Result<nlohmann::json> readTemperature(
        const nlohmann::json& input) = 0;
    [[nodiscard]] virtual lgc::Result<nlohmann::json> setRelay(
        const nlohmann::json& input) = 0;
};

class MockEdgeDeviceAdapter final : public IEdgeDeviceAdapter {
public:
    [[nodiscard]] lgc::Result<nlohmann::json> readTemperature(
        const nlohmann::json& input) override
    {
        return nlohmann::json {
            { "sensor", input.at("sensor").get<std::string>() },
            { "celsius", 72.5 },
            { "healthy", false },
        };
    }

    [[nodiscard]] lgc::Result<nlohmann::json> setRelay(
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

void require(lgc::Result<void> result)
{
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        std::exit(1);
    }
}

std::shared_ptr<lgc::ToolRegistry> makeEdgeToolRegistry(
    std::shared_ptr<IEdgeDeviceAdapter> adapter)
{
    auto registry = std::make_shared<lgc::ToolRegistry>();

    require(registry->add(lgc::Tool {
        .name_ = "edge.read_temperature",
        .description_ = "Read a temperature sensor on an edge device.",
        .inputSchema_ = lgc::JsonSchema::object()
                            .property("sensor", lgc::JsonSchema::string(), true)
                            .additionalProperties(false),
        .outputSchema_ = lgc::JsonSchema::object()
                             .property("sensor", lgc::JsonSchema::string(), true)
                             .property("celsius", lgc::JsonSchema::number(), true)
                             .property("healthy", lgc::JsonSchema::boolean(), true)
                             .additionalProperties(false),
        .callable_ = [device = adapter](
                         const nlohmann::json& input) -> lgc::Result<nlohmann::json> {
            return device->readTemperature(input);
        },
    }));

    require(registry->add(lgc::Tool {
        .name_ = "edge.set_relay",
        .description_ = "Set a relay on an edge device.",
        .inputSchema_ = lgc::JsonSchema::object()
                            .property("relay", lgc::JsonSchema::string(), true)
                            .property("enabled", lgc::JsonSchema::boolean(), true)
                            .additionalProperties(false),
        .outputSchema_ = lgc::JsonSchema::object()
                             .property("relay", lgc::JsonSchema::string(), true)
                             .property("enabled", lgc::JsonSchema::boolean(), true)
                             .additionalProperties(false),
        .callable_ = [device = adapter](
                         const nlohmann::json& input) -> lgc::Result<nlohmann::json> {
            return device->setRelay(input);
        },
    }));

    return registry;
}

} // namespace

int main()
{
    auto registry = makeEdgeToolRegistry(std::make_shared<MockEdgeDeviceAdapter>());

    lgc::StateGraph graph;
    require(graph.addNode(
        "tools",
        lgc::ToolNode(registry, lgc::ToolNodeOptions {
            .validateOutput_ = true,
        })));
    require(graph.addEdge(std::string(lgc::START), "tools"));
    require(graph.addEdge("tools", std::string(lgc::END)));

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    auto input = lgc::State::fromJsonValue({
        { "messages", lgc::messagesToJson({
            lgc::BaseMessage::human("Diagnose the cooling loop."),
            lgc::BaseMessage::ai(
                "",
                {
                    lgc::ToolCall {
                        .id_ = "call-temperature",
                        .name_ = "edge.read_temperature",
                        .args_ = {
                            { "sensor", "cooling-loop" },
                        },
                    },
                    lgc::ToolCall {
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
