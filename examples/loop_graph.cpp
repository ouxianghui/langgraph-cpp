#include <langgraph_cpp/langgraph.hpp>

#include <iostream>

int main()
{
    auto require = [](lgc::Result<void> result) {
        if (!result.isOk())
            std::cerr << result.status() << '\n';
        return result.isOk();
    };

    lgc::StateGraph graph;
    if (!require(graph.addNode("tick", [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::StateUpdate> {
        auto json = state.toJson();
        if (!json.isOk())
            return json.status();
        const auto count = json->value("count", 0);
        return lgc::StateUpdate::fromJsonValue({
            { "count", count + 1 },
        });
    }))) {
        return 1;
    }

    if (!require(graph.addEdge(std::string(lgc::START), "tick"))) {
        return 1;
    }
    if (!require(graph.addConditionalEdges("tick", [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::NodeId> {
        auto json = state.toJson();
        if (!json.isOk()) {
            return json.status();
        }
        if (json->value("count", 0) >= 3) {
            return std::string(lgc::END);
        }
            return std::string("tick");
        }, { "tick", std::string(lgc::END) }))
        ) {
            return 1;
    }

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    auto input = lgc::State::fromJson(R"({"count":0})");
    auto result = compiled->invoke(*input);
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        return 1;
    }

    std::cout << result->state_.json() << '\n';
    return 0;
}
