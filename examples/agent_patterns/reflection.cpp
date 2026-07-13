#include <langgraph_cpp/langgraph.hpp>

#include <cstdlib>
#include <iostream>
#include <utility>

namespace {

void require(lc::Result<void> result)
{
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        std::exit(1);
    }
}

lc::Result<lc::NodeOutput> updateFromJson(nlohmann::json update)
{
    auto stateUpdate = lc::StateUpdate::fromJsonValue(std::move(update));
    if (!stateUpdate.isOk())
        return stateUpdate.status();
    return lc::NodeOutput::update(std::move(*stateUpdate));
}

} // namespace

int main()
{
    lc::StateGraph graph;

    require(graph.addNode("draft", [](const lc::State&, lc::Runtime&) {
        return updateFromJson({
            { "pattern", "reflection" },
            { "draft", "Use the client runtime to orchestrate AI lab workflows." },
            { "reflection_trace", nlohmann::json::array() },
            { "revision_count", 0 },
        });
    }));

    require(graph.addNode("critic", [](const lc::State& state, lc::Runtime&) {
        auto snapshot = state.toJson();
        if (!snapshot.isOk())
            return lc::Result<lc::NodeOutput>(snapshot.status());

        auto trace = snapshot->value("reflection_trace", nlohmann::json::array());
        trace.push_back({
            { "role", "critic" },
            { "note", "The draft needs a concrete benefit and a client-side deployment angle." },
        });

        return updateFromJson({
            { "critique", "Add persistence, local execution, and edge-client reliability benefits." },
            { "reflection_trace", trace },
        });
    }));

    require(graph.addNode("revise", [](const lc::State& state, lc::Runtime&) {
        auto snapshot = state.toJson();
        if (!snapshot.isOk())
            return lc::Result<lc::NodeOutput>(snapshot.status());

        auto trace = snapshot->value("reflection_trace", nlohmann::json::array());
        trace.push_back({
            { "role", "reviser" },
            { "note", "Rewrote the answer with operational value and recovery guarantees." },
        });

        return updateFromJson({
            { "final_answer",
              "Use langgraph-cpp when an AI Lab client needs local graph execution, checkpointed recovery, and explicit tool orchestration." },
            { "reflection_trace", trace },
            { "revision_count", 1 },
        });
    }));

    require(graph.addEdge(std::string(lc::START), "draft"));
    require(graph.addEdge("draft", "critic"));
    require(graph.addEdge("critic", "revise"));
    require(graph.addEdge("revise", std::string(lc::END)));

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    auto input = lc::State::fromJsonValue({
        { "task", "Explain why a desktop client should use langgraph-cpp." },
    });
    if (!input.isOk()) {
        std::cerr << input.status() << '\n';
        return 1;
    }

    auto result = compiled->invoke(*input);
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        return 1;
    }

    std::cout << result->state_.json() << '\n';
    return 0;
}
