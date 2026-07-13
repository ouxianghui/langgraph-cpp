#include <langgraph_cpp/langgraph.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

lc::CompiledStateGraph buildGraph()
{
    lc::StateGraph graph;
    auto require = [](lc::Result<void> result) {
        if (!result.isOk())
            std::cerr << result.status() << '\n';
        return result.isOk();
    };

    if (!require(graph.addNode("tick", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
            auto json = state.toJson();
            if (!json.isOk())
                return json.status();
            return lc::StateUpdate::fromJsonValue({
                { "count", json->value("count", 0) + 1 },
            });
        })))
        std::exit(1);

    if (!require(graph.addEdge(std::string(lc::START), "tick")))
        std::exit(1);
    if (!require(graph.addConditionalEdges(
            "tick",
            [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::NodeId> {
                auto json = state.toJson();
                if (!json.isOk())
                    return json.status();
                if (json->value("count", 0) >= 3)
                    return std::string(lc::END);
                return std::string("tick");
            },
            { "tick", std::string(lc::END) })))
        std::exit(1);

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
    auto storage = std::make_shared<lc::MemoryStorage>();
    auto checkpointer = std::make_shared<lc::StorageSaver>(storage);
    auto graph = buildGraph();

    lc::RunOptions firstRun;
    firstRun.threadId_ = "checkpoint-resume-demo";
    firstRun.checkpointer_ = checkpointer;
    firstRun.limits_ = lc::ResourceLimits {}.maxSteps(2);

    auto input = lc::State::fromJson(R"({"count":0})");
    auto interrupted = graph.invoke(*input, firstRun);
    if (interrupted.isOk()) {
        std::cerr << "expected first run to stop at max steps\n";
        return 1;
    }

    lc::RunOptions resumeRun;
    resumeRun.checkpointer_ = checkpointer;
    resumeRun.limits_ = lc::ResourceLimits {}.maxSteps(10);

    auto resumed = graph.resume("checkpoint-resume-demo", resumeRun);
    if (!resumed.isOk()) {
        std::cerr << resumed.status() << '\n';
        return 1;
    }

    std::cout << resumed->state_.json() << '\n';
    return 0;
}
