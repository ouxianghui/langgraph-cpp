#include <langgraph_cpp/langgraph.hpp>

#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

namespace {

lgc::Result<lgc::CompiledStateGraph> buildCountingGraph()
{
    lgc::StateGraph graph;
    if (auto status = graph.addNode("tick", [](const lgc::State& state, lgc::Runtime&)
            -> lgc::Result<lgc::StateUpdate> {
            return lgc::StateUpdate::fromJsonValue({
                { "count", state.view().value("count", 0) + 1 },
            });
        });
        !status.isOk())
        return status.status();

    if (auto status = graph.addEdge(std::string(lgc::START), "tick"); !status.isOk())
        return status.status();

    auto routed = graph.addConditionalEdges(
        "tick",
        [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::NodeId> {
            if (state.view().value("count", 0) >= 3)
                return std::string(lgc::END);
            return std::string("tick");
        },
        { "tick", std::string(lgc::END) });
    if (!routed.isOk())
        return routed.status();

    return graph.compile();
}

} // namespace

int main()
{
    auto compiled = buildCountingGraph();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    auto checkpointer = std::make_shared<lgc::InMemorySaver>();
    lgc::RunOptions options;
    options.threadId_ = "time-travel-demo";
    options.checkpointer_ = checkpointer;

    auto input = lgc::State::fromJson(R"({"count":0})");
    if (!input.isOk()) {
        std::cerr << input.status() << '\n';
        return 1;
    }

    auto result = compiled->invoke(*input, options);
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        return 1;
    }

    auto history = compiled->getStateHistory("time-travel-demo", options);
    if (!history.isOk()) {
        std::cerr << history.status() << '\n';
        return 1;
    }

    auto stepOne = std::find_if(
        history->begin(),
        history->end(),
        [](const lgc::StateSnapshot& snapshot) {
            return snapshot.step_ == 1;
        });
    if (stepOne == history->end()) {
        std::cerr << "step 1 checkpoint missing\n";
        return 1;
    }

    auto replayed = compiled->replay("time-travel-demo", stepOne->checkpointId_, options);
    if (!replayed.isOk()) {
        std::cerr << replayed.status() << '\n';
        return 1;
    }

    auto update = lgc::StateUpdate::fromJson(R"({"count":10})");
    if (!update.isOk()) {
        std::cerr << update.status() << '\n';
        return 1;
    }

    lgc::StateUpdateOptions updateOptions;
    updateOptions.checkpointId_ = stepOne->checkpointId_;
    updateOptions.asNode_ = "tick";
    auto forked = compiled->updateState("time-travel-demo", *update, options, updateOptions);
    if (!forked.isOk()) {
        std::cerr << forked.status() << '\n';
        return 1;
    }

    std::vector<int> historySteps;
    historySteps.reserve(history->size());
    for (const auto& snapshot : *history)
        historySteps.push_back(static_cast<int>(snapshot.step_));

    nlohmann::json output {
        { "final_count", result->state_.view().at("count") },
        { "history_steps", historySteps },
        { "replayed_count", replayed->state_.view().at("count") },
        { "forked_count", forked->values_.view().at("count") },
        { "fork_parent_checkpoint_id", forked->parentCheckpointId_.value_or("") },
    };
    std::cout << output.dump() << '\n';
    return 0;
}
