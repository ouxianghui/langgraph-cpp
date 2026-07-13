#include <langgraph_cpp/langgraph.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

class IEdgeRepairAdapter {
public:
    virtual ~IEdgeRepairAdapter() = default;

    [[nodiscard]] virtual lc::Result<nlohmann::json> readTemperature(
        const nlohmann::json& input) = 0;
    [[nodiscard]] virtual lc::Result<nlohmann::json> setRelay(
        const nlohmann::json& input) = 0;
    [[nodiscard]] virtual lc::Result<nlohmann::json> manualReset(
        const nlohmann::json& input) = 0;
};

class MockEdgeRepairAdapter final : public IEdgeRepairAdapter {
public:
    [[nodiscard]] lc::Result<nlohmann::json> readTemperature(
        const nlohmann::json& input) override
    {
        const auto sensor = input.at("sensor").get<std::string>();

        if (!relayEnabled_) {
            return nlohmann::json {
                { "sensor", sensor },
                { "celsius", 82.5 },
                { "healthy", false },
                { "fault", "cooling_relay_disabled" },
            };
        }

        if (!manualResetDone_) {
            return nlohmann::json {
                { "sensor", sensor },
                { "celsius", 79.0 },
                { "healthy", false },
                { "fault", "fan_controller_latched" },
            };
        }

        return nlohmann::json {
            { "sensor", sensor },
            { "celsius", 41.5 },
            { "healthy", true },
            { "fault", nullptr },
        };
    }

    [[nodiscard]] lc::Result<nlohmann::json> setRelay(
        const nlohmann::json& input) override
    {
        relayEnabled_ = input.at("enabled").get<bool>();
        return nlohmann::json {
            { "relay", input.at("relay").get<std::string>() },
            { "enabled", relayEnabled_ },
            { "accepted", true },
        };
    }

    [[nodiscard]] lc::Result<nlohmann::json> manualReset(
        const nlohmann::json& input) override
    {
        manualResetDone_ = true;
        return nlohmann::json {
            { "component", input.at("component").get<std::string>() },
            { "reset", true },
        };
    }

private:
    bool relayEnabled_ { false };
    bool manualResetDone_ { false };
};

void require(lc::Result<void> result)
{
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        std::exit(1);
    }
}

[[nodiscard]] std::string runStatusName(lc::RunStatus status)
{
    switch (status) {
    case lc::RunStatus::Completed:
        return "completed";
    case lc::RunStatus::Paused:
        return "paused";
    case lc::RunStatus::Failed:
        return "failed";
    case lc::RunStatus::Cancelled:
        return "cancelled";
    case lc::RunStatus::MaxStepsExceeded:
        return "max_steps_exceeded";
    }
    return "failed";
}

[[nodiscard]] nlohmann::json eventToJson(const lc::RuntimeEvent& event)
{
    nlohmann::json value {
        { "type", std::string(lc::runtimeEventTypeName(event.type_)) },
        { "run_id", event.runId_ },
        { "thread_id", event.threadId_ },
        { "step", event.step_ },
        { "sequence", event.sequence_ },
    };
    if (!event.node_.empty())
        value["node"] = event.node_;
    if (!event.name_.empty())
        value["name"] = event.name_;
    if (!event.message_.empty())
        value["message"] = event.message_;
    if (!event.payload_.empty())
        value["payload"] = event.payload_;
    return value;
}

void appendEvents(nlohmann::json& target, const std::vector<lc::RuntimeEvent>& events)
{
    for (const auto& event : events)
        target.push_back(eventToJson(event));
}

[[nodiscard]] lc::Result<nlohmann::json> latestToolResult(
    const lc::State& state,
    std::string_view toolName)
{
    auto json = state.toJson();
    if (!json.isOk())
        return json.status();

    auto messages = lc::messagesFromStateJson(*json);
    if (!messages.isOk())
        return messages.status();

    for (auto it = messages->rbegin(); it != messages->rend(); ++it) {
        if (it->type_ != lc::MessageType::Tool || it->name_ != toolName)
            continue;

        auto content = nlohmann::json::parse(it->content_, nullptr, false);
        if (content.is_discarded())
            return lc::Status::invalidArgument("tool message content is not valid JSON");

        auto toolResult = lc::toolResultFromJson(content);
        if (!toolResult.isOk())
            return toolResult.status();
        if (!toolResult->ok_) {
            if (toolResult->error_.has_value())
                return lc::Status::failedPrecondition(toolResult->error_->message_);
            return lc::Status::failedPrecondition("tool returned an error");
        }
        return toolResult->result_;
    }

    return lc::Status::notFound("tool result not found");
}

[[nodiscard]] lc::Result<lc::StateUpdate> appendAssistantToolCall(
    lc::ToolCall toolCall,
    nlohmann::json extra = nlohmann::json::object())
{
    extra["messages"] = lc::messagesToJson({
        lc::BaseMessage::ai("", { std::move(toolCall) }),
    });
    return lc::StateUpdate::fromJsonValue(extra);
}

std::shared_ptr<lc::ToolRegistry> makeToolRegistry(
    std::shared_ptr<IEdgeRepairAdapter> adapter)
{
    auto registry = std::make_shared<lc::ToolRegistry>();

    require(registry->add(lc::Tool {
        .name_ = "edge.read_temperature",
        .description_ = "Read the cooling-loop temperature sensor.",
        .inputSchema_ = lc::JsonSchema::object()
                            .property("sensor", lc::JsonSchema::string(), true)
                            .additionalProperties(false),
        .outputSchema_ = lc::JsonSchema::object()
                             .property("sensor", lc::JsonSchema::string(), true)
                             .property("celsius", lc::JsonSchema::number(), true)
                             .property("healthy", lc::JsonSchema::boolean(), true)
                             .property("fault", lc::JsonSchema::any(), true)
                             .additionalProperties(false),
        .callable_ = [device = adapter](const nlohmann::json& input) -> lc::Result<nlohmann::json> {
            return device->readTemperature(input);
        },
    }));

    require(registry->add(lc::Tool {
        .name_ = "edge.set_relay",
        .description_ = "Enable or disable an edge relay.",
        .inputSchema_ = lc::JsonSchema::object()
                            .property("relay", lc::JsonSchema::string(), true)
                            .property("enabled", lc::JsonSchema::boolean(), true)
                            .additionalProperties(false),
        .outputSchema_ = lc::JsonSchema::object()
                             .property("relay", lc::JsonSchema::string(), true)
                             .property("enabled", lc::JsonSchema::boolean(), true)
                             .property("accepted", lc::JsonSchema::boolean(), true)
                             .additionalProperties(false),
        .callable_ = [device = adapter](const nlohmann::json& input) -> lc::Result<nlohmann::json> {
            return device->setRelay(input);
        },
    }));

    require(registry->add(lc::Tool {
        .name_ = "edge.manual_reset",
        .description_ = "Simulate an operator-assisted controller reset.",
        .inputSchema_ = lc::JsonSchema::object()
                            .property("component", lc::JsonSchema::string(), true)
                            .additionalProperties(false),
        .outputSchema_ = lc::JsonSchema::object()
                             .property("component", lc::JsonSchema::string(), true)
                             .property("reset", lc::JsonSchema::boolean(), true)
                             .additionalProperties(false),
        .callable_ = [device = std::move(adapter)](
                         const nlohmann::json& input) -> lc::Result<nlohmann::json> {
            return device->manualReset(input);
        },
    }));

    return registry;
}

[[nodiscard]] lc::CompiledStateGraph buildGraph(std::shared_ptr<lc::ToolRegistry> registry)
{
    lc::StateGraph graph;

    require(graph.addNode("diagnose", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        auto json = state.toJson();
        if (!json.isOk())
            return json.status();

        const int cycle = json->value("diagnostic_cycles", 0) + 1;
        return appendAssistantToolCall(
            lc::ToolCall {
                .id_ = "read-temperature-" + std::to_string(cycle),
                .name_ = "edge.read_temperature",
                .args_ = {
                    { "sensor", "cooling-loop" },
                },
            },
            {
                { "diagnostic_cycles", cycle },
                { "repair_status", "diagnosing" },
            });
    }));

    require(graph.addNode(
        "read_tools",
        lc::ToolNode(registry, lc::ToolNodeOptions {
            .validateOutput_ = true,
        })));

    require(graph.addNode("evaluate", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        auto stateJson = state.toJson();
        if (!stateJson.isOk())
            return stateJson.status();

        auto reading = latestToolResult(state, "edge.read_temperature");
        if (!reading.isOk())
            return reading.status();

        const bool healthy = reading->value("healthy", false);
        const int attempts = stateJson->value("repair_attempts", 0);
        nlohmann::json update {
            { "last_temperature_c", reading->value("celsius", 0.0) },
            { "device_healthy", healthy },
            { "fault", reading->contains("fault") ? reading->at("fault") : nlohmann::json(nullptr) },
            { "repair_status", healthy ? "repaired" : (attempts == 0 ? "needs_auto_repair" : "needs_operator") },
        };

        return lc::StateUpdate::fromJsonValue(update);
    }));

    require(graph.addNode("auto_repair", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        auto json = state.toJson();
        if (!json.isOk())
            return json.status();

        const int attempts = json->value("repair_attempts", 0) + 1;
        return appendAssistantToolCall(
            lc::ToolCall {
                .id_ = "enable-relay-" + std::to_string(attempts),
                .name_ = "edge.set_relay",
                .args_ = {
                    { "relay", "cooling-fan" },
                    { "enabled", true },
                },
            },
            {
                { "repair_attempts", attempts },
                { "repair_status", "auto_repairing" },
            });
    }));

    require(graph.addNode(
        "repair_tools",
        lc::ToolNode(registry, lc::ToolNodeOptions {
            .validateOutput_ = true,
        })));

    require(graph.addNode("operator_gate", [](const lc::State& state, lc::Runtime& context) -> lc::Result<lc::NodeOutput> {
        if (!context.hasResumeValue()) {
            auto json = state.toJson();
            if (!json.isOk())
                return json.status();

            return lc::NodeOutput::interrupt(lc::Interrupt {
                .id_ = "operator_manual_reset_required",
                .value_ = {
                    { "component", "fan-controller" },
                    { "fault", json->value("fault", nlohmann::json(nullptr)) },
                    { "last_temperature_c", json->value("last_temperature_c", 0.0) },
                    { "reason", "automatic relay enable did not clear the fault" },
                },
            });
        }

        const bool approved = context.resumeValue().value("approved", false);
        auto update = lc::StateUpdate::fromJsonValue({
            { "operator_approved", approved },
            { "repair_status", approved ? "operator_approved" : "operator_rejected" },
        });
        if (!update.isOk())
            return update.status();
        return lc::NodeOutput::update(std::move(*update));
    }));

    require(graph.addNode("manual_reset", [](const lc::State&, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        return appendAssistantToolCall(
            lc::ToolCall {
                .id_ = "manual-reset-1",
                .name_ = "edge.manual_reset",
                .args_ = {
                    { "component", "fan-controller" },
                },
            },
            {
                { "repair_status", "manual_resetting" },
            });
    }));

    require(graph.addNode(
        "reset_tools",
        lc::ToolNode(registry, lc::ToolNodeOptions {
            .validateOutput_ = true,
        })));

    require(graph.addEdge(std::string(lc::START), "diagnose"));
    require(graph.addEdge("diagnose", "read_tools"));
    require(graph.addEdge("read_tools", "evaluate"));
    require(graph.addConditionalEdges(
        "evaluate",
        [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::NodeId> {
            auto json = state.toJson();
            if (!json.isOk())
                return json.status();
            if (json->value("device_healthy", false))
                return std::string(lc::END);
            if (json->value("repair_attempts", 0) == 0)
                return std::string("auto_repair");
            return std::string("operator_gate");
        },
        { "auto_repair", "operator_gate", std::string(lc::END) }));
    require(graph.addEdge("auto_repair", "repair_tools"));
    require(graph.addEdge("repair_tools", "diagnose"));
    require(graph.addConditionalEdges(
        "operator_gate",
        [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::NodeId> {
            auto json = state.toJson();
            if (!json.isOk())
                return json.status();
            if (json->value("operator_approved", false))
                return std::string("manual_reset");
            return std::string(lc::END);
        },
        { "manual_reset", std::string(lc::END) }));
    require(graph.addEdge("manual_reset", "reset_tools"));
    require(graph.addEdge("reset_tools", "diagnose"));

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        std::exit(1);
    }
    return *compiled;
}

} // namespace

int main()
{
    auto registry = makeToolRegistry(std::make_shared<MockEdgeRepairAdapter>());
    auto graph = buildGraph(registry);
    auto checkpointer = std::make_shared<lc::InMemorySaver>();

    lc::RunOptions options;
    options.threadId_ = "mock-edge-repair-demo";
    options.checkpointer_ = checkpointer;
    options.reducers_.set("messages", lc::ReducerKind::AddMessages);

    auto input = lc::State::fromJsonValue({
        { "repair_attempts", 0 },
        { "diagnostic_cycles", 0 },
        { "messages", lc::messagesToJson({
            lc::BaseMessage::human("Repair the cooling loop and pause for operator help if automation fails."),
        }) },
    });
    if (!input.isOk()) {
        std::cerr << input.status() << '\n';
        return 1;
    }

    auto paused = graph.stream(*input, options);
    if (!paused.isOk()) {
        std::cerr << paused.status() << '\n';
        return 1;
    }
    if (paused->status_ != lc::RunStatus::Paused) {
        std::cerr << "expected workflow to pause for operator approval\n";
        return 1;
    }

    lc::RunOptions resumeOptions;
    resumeOptions.checkpointer_ = checkpointer;
    resumeOptions.reducers_.set("messages", lc::ReducerKind::AddMessages);
    resumeOptions.command_ = lc::Command::resume({
        { "approved", true },
    });

    auto repaired = graph.resumeStream("mock-edge-repair-demo", resumeOptions);
    if (!repaired.isOk()) {
        std::cerr << repaired.status() << '\n';
        return 1;
    }

    auto checkpoints = checkpointer->list(lc::CheckpointListOptions {
        .threadId_ = "mock-edge-repair-demo",
        .checkpointNamespace_ = std::string(),
        .order_ = lc::CheckpointListOrder::OldestFirst,
    });
    if (!checkpoints.isOk()) {
        std::cerr << checkpoints.status() << '\n';
        return 1;
    }

    auto finalState = repaired->state_.toJson();
    if (!finalState.isOk()) {
        std::cerr << finalState.status() << '\n';
        return 1;
    }

    nlohmann::json events = nlohmann::json::array();
    appendEvents(events, paused->events_);
    appendEvents(events, repaired->events_);

    std::cout << nlohmann::json {
        { "checkpoint_count", checkpoints->size() },
        { "events", std::move(events) },
        { "first_run_status", runStatusName(paused->status_) },
        { "run_status", runStatusName(repaired->status_) },
        { "state", *finalState },
    }.dump() << '\n';
    return 0;
}
