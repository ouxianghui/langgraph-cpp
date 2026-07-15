#include <langgraph_cpp/langgraph.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

namespace {

void require(lgc::Result<void> result)
{
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    lgc::StateGraph graph;
    require(graph.addNode("approve", [](const lgc::State&, lgc::Runtime& context) -> lgc::Result<lgc::NodeOutput> {
        if (!context.hasResumeValue()) {
            return lgc::NodeOutput::interrupt(lgc::Interrupt {
                .id_ = "approve_action",
                .value_ = {
                    { "action", "open_valve" },
                    { "reason", "operator approval required" },
                },
            });
        }

        const bool approved = context.resumeValue().value("approved", false);
        auto update = lgc::StateUpdate::fromJsonValue({
            { "approved", approved },
        });
        if (!update.isOk())
            return update.status();
        return lgc::NodeOutput::update(std::move(*update));
    }));
    require(graph.addNode("act", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::fromJson(R"({"action_status":"completed"})");
    }));
    require(graph.addEdge(std::string(lgc::START), "approve"));
    require(graph.addConditionalEdges(
        "approve",
        [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::NodeId> {
            auto json = state.toJson();
            if (!json.isOk())
                return json.status();
            if (json->value("approved", false))
                return std::string("act");
            return std::string(lgc::END);
        },
        { "act", std::string(lgc::END) }));
    require(graph.addEdge("act", std::string(lgc::END)));

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    auto checkpointer = std::make_shared<lgc::InMemorySaver>();
    lgc::RunOptions options;
    options.threadId_ = "human-interrupt-demo";
    options.checkpointer_ = checkpointer;

    auto input = lgc::State::fromJson("{}");
    auto paused = compiled->invoke(*input, options);
    if (!paused.isOk()) {
        std::cerr << paused.status() << '\n';
        return 1;
    }
    if (paused->status_ != lgc::RunStatus::Paused) {
        std::cerr << "expected paused run\n";
        return 1;
    }

    lgc::RunOptions resumeOptions;
    resumeOptions.checkpointer_ = checkpointer;
    resumeOptions.command_ = lgc::Command::resume({
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
