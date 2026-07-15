#include <langgraph_cpp/langgraph.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

lgc::CompiledStateGraph buildGraph()
{
    lgc::StateGraph graph;
    auto require = [](lgc::Result<void> result) {
        if (!result.isOk())
            std::cerr << result.status() << '\n';
        return result.isOk();
    };

    if (!require(graph.addNode("tick", [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::StateUpdate> {
            auto json = state.toJson();
            if (!json.isOk())
                return json.status();
            return lgc::StateUpdate::fromJsonValue({
                { "count", json->value("count", 0) + 1 },
            });
        })))
        std::exit(1);

    if (!require(graph.addEdge(std::string(lgc::START), "tick")))
        std::exit(1);
    if (!require(graph.addConditionalEdges(
            "tick",
            [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::NodeId> {
                auto json = state.toJson();
                if (!json.isOk())
                    return json.status();
                if (json->value("count", 0) >= 3)
                    return std::string(lgc::END);
                return std::string("tick");
            },
            { "tick", std::string(lgc::END) })))
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
    auto storage = std::make_shared<lgc::MemoryStorage>();
    auto checkpointer = std::make_shared<lgc::StorageSaver>(storage);
    auto graph = buildGraph();

    lgc::RunOptions firstRun;
    firstRun.threadId_ = "checkpoint-resume-demo";
    firstRun.checkpointer_ = checkpointer;
    firstRun.limits_ = lgc::ResourceLimits {}.maxSteps(2);

    auto input = lgc::State::fromJson(R"({"count":0})");
    auto interrupted = graph.invoke(*input, firstRun);
    if (interrupted.isOk()) {
        std::cerr << "expected first run to stop at max steps\n";
        return 1;
    }

    lgc::RunOptions resumeRun;
    resumeRun.checkpointer_ = checkpointer;
    resumeRun.limits_ = lgc::ResourceLimits {}.maxSteps(10);

    auto resumed = graph.resume("checkpoint-resume-demo", resumeRun);
    if (!resumed.isOk()) {
        std::cerr << resumed.status() << '\n';
        return 1;
    }

    std::cout << resumed->state_.json() << '\n';
    return 0;
}
