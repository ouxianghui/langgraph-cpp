#include <langgraph_cpp/langgraph.hpp>

#include <iostream>

int main()
{
    lc::StateGraph graph;

    if (auto status = graph.addNode("sense_temperature", [](const lc::State&, lc::Runtime&) {
            return lc::StateUpdate::fromJson(R"({
                "checks": ["temperature"],
                "facts": {
                    "temperature_c": 42.5
                }
            })");
        });
        !status.isOk()) {
        std::cerr << status.status() << '\n';
        return 1;
    }

    if (auto status = graph.addNode("sense_power", [](const lc::State&, lc::Runtime&) {
            return lc::StateUpdate::fromJson(R"({
                "checks": ["power"],
                "facts": {
                    "voltage_v": 12.1
                }
            })");
        });
        !status.isOk()) {
        std::cerr << status.status() << '\n';
        return 1;
    }

    if (auto status = graph.addNode("decide", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
            const auto& facts = state.view().at("facts");
            const bool healthy = facts.at("temperature_c").get<double>() < 60.0
                && facts.at("voltage_v").get<double>() >= 12.0;
            return lc::StateUpdate::fromJsonValue({
                { "healthy", healthy },
                { "decision", healthy ? "continue" : "inspect" },
            });
        });
        !status.isOk()) {
        std::cerr << status.status() << '\n';
        return 1;
    }

    const auto edgeStatuses = {
        graph.addEdge(std::string(lc::START), "sense_temperature"),
        graph.addEdge(std::string(lc::START), "sense_power"),
        graph.addEdge("sense_temperature", "decide"),
        graph.addEdge("sense_power", "decide"),
        graph.addEdge("decide", std::string(lc::END)),
    };
    for (const auto& status : edgeStatuses) {
        if (!status.isOk()) {
            std::cerr << status.status() << '\n';
            return 1;
        }
    }

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    lc::RunOptions options;
    options.reducers_
        .set("checks", lc::ReducerKind::Append)
        .set("facts", lc::ReducerKind::MergeObject);

    auto input = lc::State::fromJson("{}");
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
