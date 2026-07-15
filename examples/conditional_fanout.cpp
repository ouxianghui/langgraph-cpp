#include <langgraph_cpp/langgraph.hpp>

#include <iostream>
#include <string>
#include <vector>

int main()
{
    lgc::StateGraph graph;

    auto status = graph.addNode("triage", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::fromJson(R"({"decision":"inspect"})");
    });
    if (!status.isOk()) {
        std::cerr << status.status().toString() << '\n';
        return 1;
    }
    status = graph.addNode("temperature", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::fromJson(R"({"checks":["temperature"],"facts":{"temperature_c":42.5}})");
    });
    if (!status.isOk()) {
        std::cerr << status.status().toString() << '\n';
        return 1;
    }
    status = graph.addNode("power", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::fromJson(R"({"checks":["power"],"facts":{"voltage_v":12.1}})");
    });
    if (!status.isOk()) {
        std::cerr << status.status().toString() << '\n';
        return 1;
    }
    status = graph.addNode("join", [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::StateUpdate> {
        auto json = state.toJson();
        if (!json.isOk())
            return json.status();

        const bool healthy = json->at("facts").at("temperature_c").get<double>() < 80.0
            && json->at("facts").at("voltage_v").get<double>() > 11.0;
        return lgc::StateUpdate::fromJsonValue({
            { "healthy", healthy },
        });
    });
    if (!status.isOk()) {
        std::cerr << status.status().toString() << '\n';
        return 1;
    }

    status = graph.addEdge(std::string(lgc::START), "triage");
    if (!status.isOk()) {
        std::cerr << status.status().toString() << '\n';
        return 1;
    }
    status = graph.addConditionalEdges(
        "triage",
        [](const lgc::State&, lgc::Runtime&) -> lgc::Result<std::vector<lgc::NodeId>> {
            return std::vector<lgc::NodeId> { "temperature", "power" };
        },
        { "temperature", "power" });
    if (!status.isOk()) {
        std::cerr << status.status().toString() << '\n';
        return 1;
    }
    status = graph.addEdge("temperature", "join");
    if (!status.isOk()) {
        std::cerr << status.status().toString() << '\n';
        return 1;
    }
    status = graph.addEdge("power", "join");
    if (!status.isOk()) {
        std::cerr << status.status().toString() << '\n';
        return 1;
    }
    status = graph.addEdge("join", std::string(lgc::END));
    if (!status.isOk()) {
        std::cerr << status.status().toString() << '\n';
        return 1;
    }

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status().toString() << '\n';
        return 1;
    }

    lgc::RunOptions options;
    options.reducers_.set("checks", lgc::ReducerKind::Append);
    options.reducers_.set("facts", lgc::ReducerKind::MergeObject);

    auto input = lgc::State::fromJson("{}");
    if (!input.isOk()) {
        std::cerr << input.status().toString() << '\n';
        return 1;
    }

    auto result = compiled->invoke(*input, options);
    if (!result.isOk()) {
        std::cerr << result.status().toString() << '\n';
        return 1;
    }

    auto json = result->state_.toJson();
    if (!json.isOk()) {
        std::cerr << json.status().toString() << '\n';
        return 1;
    }

    std::cout << json->dump() << '\n';
    return 0;
}
