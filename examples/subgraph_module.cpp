#include <langgraph_cpp/langgraph.hpp>

#include <iostream>
#include <memory>

namespace {

lc::Result<lc::CompiledStateGraph> buildDiagnosticSubgraph()
{
    lc::StateGraph child;
    if (auto status = child.addNode("sense", [](const lc::State&, lc::Runtime& context)
            -> lc::Result<lc::StateUpdate> {
            if (auto written = context.streamWriter().write("diagnostic-progress", { { "phase", "sense" } });
                !written.isOk())
                return written;
            return lc::StateUpdate::fromJson(R"({
                "diagnosis": {
                    "fault": "overheat",
                    "temperature_c": 72.0
                }
            })");
        });
        !status.isOk())
        return status.status();

    if (auto status = child.addNode("recommend", [](const lc::State& state, lc::Runtime&)
            -> lc::Result<lc::StateUpdate> {
            const auto& diagnosis = state.view().at("diagnosis");
            const auto severity = diagnosis.at("temperature_c").get<double>() > 70.0 ? "high" : "normal";
            return lc::StateUpdate::fromJsonValue({
                { "repair_plan",
                    {
                        { "action", "reduce_load" },
                        { "severity", severity },
                    } },
            });
        });
        !status.isOk())
        return status.status();

    const auto edgeStatuses = {
        child.addEdge(std::string(lc::START), "sense"),
        child.addEdge("sense", "recommend"),
        child.addEdge("recommend", std::string(lc::END)),
    };
    for (const auto& status : edgeStatuses) {
        if (!status.isOk())
            return status.status();
    }

    return child.compile();
}

} // namespace

int main()
{
    auto diagnostic = buildDiagnosticSubgraph();
    if (!diagnostic.isOk()) {
        std::cerr << diagnostic.status() << '\n';
        return 1;
    }

    lc::StateGraph parent;
    lc::SubgraphOptions subgraphOptions;
    subgraphOptions.persistence_ = lc::SubgraphPersistenceMode::PerThread;
    subgraphOptions.checkpointNamespace_ = "diagnostic";
    if (auto status = parent.addSubgraph(
            "diagnose_device",
            std::make_shared<lc::CompiledStateGraph>(*diagnostic),
            subgraphOptions);
        !status.isOk()) {
        std::cerr << status.status() << '\n';
        return 1;
    }

    if (auto status = parent.addNode("dispatch", [](const lc::State& state, lc::Runtime&)
            -> lc::Result<lc::StateUpdate> {
            const auto& plan = state.view().at("repair_plan");
            return lc::StateUpdate::fromJsonValue({
                { "ticket",
                    {
                        { "route", "edge-maintenance" },
                        { "action", plan.at("action") },
                        { "priority", plan.at("severity") },
                    } },
                { "status", "queued" },
            });
        });
        !status.isOk()) {
        std::cerr << status.status() << '\n';
        return 1;
    }

    const auto edgeStatuses = {
        parent.addEdge(std::string(lc::START), "diagnose_device"),
        parent.addEdge("diagnose_device", "dispatch"),
        parent.addEdge("dispatch", std::string(lc::END)),
    };
    for (const auto& status : edgeStatuses) {
        if (!status.isOk()) {
            std::cerr << status.status() << '\n';
            return 1;
        }
    }

    auto compiled = parent.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    lc::RunOptions options;
    options.threadId_ = "subgraph-module-demo";
    options.checkpointer_ = std::make_shared<lc::InMemorySaver>();
    options.checkpointNamespace_ = "parent";

    auto input = lc::State::fromJson(R"({"device_id":"pump-17"})");
    if (!input.isOk()) {
        std::cerr << input.status() << '\n';
        return 1;
    }

    auto result = compiled->invoke(*input, options);
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        return 1;
    }

    std::cout << result->state_.json() << '\n';
    return 0;
}
