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
    if (!require(graph.addNode("classify", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::fromJson(R"({"priority":"high"})");
    })))
        return 1;
    if (!require(graph.addNode("fast_path", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::fromJson(R"({"route":"fast"})");
    })))
        return 1;
    if (!require(graph.addNode("normal_path", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::fromJson(R"({"route":"normal"})");
    })))
        return 1;

    if (!require(graph.addEdge(std::string(lgc::START), "classify")))
        return 1;
    if (!require(graph.addConditionalEdges(
        "classify",
        [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::NodeId> {
            auto json = state.toJson();
            if (!json.isOk())
                return json.status();
            if (json->value("priority", "") == "high")
                return std::string("fast_path");
            return std::string("normal_path");
        },
        { "fast_path", "normal_path" })))
        return 1;
    if (!require(graph.addEdge("fast_path", std::string(lgc::END))))
        return 1;
    if (!require(graph.addEdge("normal_path", std::string(lgc::END))))
        return 1;

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    auto input = lgc::State::fromJson("{}");
    auto result = compiled->invoke(*input);
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        return 1;
    }

    std::cout << result->state_.json() << '\n';
    return 0;
}
