#include <langgraph_cpp/langgraph.hpp>

#include <iostream>

int main()
{
    auto require = [](lc::Result<void> result) {
        if (!result.isOk())
            std::cerr << result.status() << '\n';
        return result.isOk();
    };

    lc::StateGraph graph;
    if (!require(graph.addNode("tick", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        auto json = state.toJson();
        if (!json.isOk())
            return json.status();
        const auto count = json->value("count", 0);
        return lc::StateUpdate::fromJsonValue({
            { "count", count + 1 },
        });
    }))) {
        return 1;
    }

    if (!require(graph.addEdge(std::string(lc::START), "tick"))) {
        return 1;
    }
    if (!require(graph.addConditionalEdges("tick", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::NodeId> {
        auto json = state.toJson();
        if (!json.isOk()) {
            return json.status();
        }
        if (json->value("count", 0) >= 3) {
            return std::string(lc::END);
        }
            return std::string("tick");
        }, { "tick", std::string(lc::END) }))
        ) {
            return 1;
    }

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    auto input = lc::State::fromJson(R"({"count":0})");
    auto result = compiled->invoke(*input);
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        return 1;
    }

    std::cout << result->state_.json() << '\n';
    return 0;
}
