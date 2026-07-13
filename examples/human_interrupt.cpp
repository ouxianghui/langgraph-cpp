#include <langgraph_cpp/langgraph.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

namespace {

void require(lc::Result<void> result)
{
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    lc::StateGraph graph;
    require(graph.addNode("approve", [](const lc::State&, lc::Runtime& context) -> lc::Result<lc::NodeOutput> {
        if (!context.hasResumeValue()) {
            return lc::NodeOutput::interrupt(lc::Interrupt {
                .id_ = "approve_action",
                .value_ = {
                    { "action", "open_valve" },
                    { "reason", "operator approval required" },
                },
            });
        }

        const bool approved = context.resumeValue().value("approved", false);
        auto update = lc::StateUpdate::fromJsonValue({
            { "approved", approved },
        });
        if (!update.isOk())
            return update.status();
        return lc::NodeOutput::update(std::move(*update));
    }));
    require(graph.addNode("act", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"action_status":"completed"})");
    }));
    require(graph.addEdge(std::string(lc::START), "approve"));
    require(graph.addConditionalEdges(
        "approve",
        [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::NodeId> {
            auto json = state.toJson();
            if (!json.isOk())
                return json.status();
            if (json->value("approved", false))
                return std::string("act");
            return std::string(lc::END);
        },
        { "act", std::string(lc::END) }));
    require(graph.addEdge("act", std::string(lc::END)));

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions options;
    options.threadId_ = "human-interrupt-demo";
    options.checkpointer_ = checkpointer;

    auto input = lc::State::fromJson("{}");
    auto paused = compiled->invoke(*input, options);
    if (!paused.isOk()) {
        std::cerr << paused.status() << '\n';
        return 1;
    }
    if (paused->status_ != lc::RunStatus::Paused) {
        std::cerr << "expected paused run\n";
        return 1;
    }

    lc::RunOptions resumeOptions;
    resumeOptions.checkpointer_ = checkpointer;
    resumeOptions.command_ = lc::Command::resume({
        { "approved", true },
    });

    auto resumed = compiled->resume("human-interrupt-demo", resumeOptions);
    if (!resumed.isOk()) {
        std::cerr << resumed.status() << '\n';
        return 1;
    }

    std::cout << resumed->state_.json() << '\n';
    return 0;
}
