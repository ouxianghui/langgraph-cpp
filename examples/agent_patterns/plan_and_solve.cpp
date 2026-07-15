#include <langgraph_cpp/langgraph.hpp>

#include <cstdlib>
#include <iostream>
#include <utility>

namespace {

void require(lgc::Result<void> result)
{
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        std::exit(1);
    }
}

lgc::Result<lgc::NodeOutput> updateFromJson(nlohmann::json update)
{
    auto stateUpdate = lgc::StateUpdate::fromJsonValue(std::move(update));
    if (!stateUpdate.isOk())
        return stateUpdate.status();
    return lgc::NodeOutput::update(std::move(*stateUpdate));
}

} // namespace

int main()
{
    lgc::StateGraph graph;

    require(graph.addNode("planner", [](const lgc::State&, lgc::Runtime&) {
        return updateFromJson({
            { "pattern", "plan_and_solve" },
            { "plan", nlohmann::json::array({
                          "inspect the task",
                          "collect required context",
                          "compose the final response",
                      }) },
            { "next_step", 0 },
            { "completed_steps", nlohmann::json::array() },
        });
    }));

    require(graph.addNode("execute_step", [](const lgc::State& state, lgc::Runtime&) {
        auto snapshot = state.toJson();
        if (!snapshot.isOk())
            return lgc::Result<lgc::NodeOutput>(snapshot.status());

        const auto& plan = snapshot->at("plan");
        const auto nextStep = snapshot->value("next_step", 0U);
        auto completed = snapshot->value("completed_steps", nlohmann::json::array());

        if (!plan.is_array() || nextStep >= plan.size()) {
            return updateFromJson({
                { "execution_error", "no remaining plan step" },
            });
        }

        completed.push_back({
            { "step", plan.at(nextStep) },
            { "status", "done" },
        });

        return updateFromJson({
            { "completed_steps", completed },
            { "next_step", nextStep + 1 },
        });
    }));

    require(graph.addNode("solver", [](const lgc::State& state, lgc::Runtime&) {
        auto snapshot = state.toJson();
        if (!snapshot.isOk())
            return lgc::Result<lgc::NodeOutput>(snapshot.status());

        const auto completed = snapshot->value("completed_steps", nlohmann::json::array());
        return updateFromJson({
            { "answer", "Plan completed: provide a concise lab-client recommendation." },
            { "completed_count", completed.size() },
            { "solved", true },
        });
    }));

    require(graph.addEdge(std::string(lgc::START), "planner"));
    require(graph.addEdge("planner", "execute_step"));
    require(graph.addConditionalEdges(
        "execute_step",
        [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::NodeId> {
            auto snapshot = state.toJson();
            if (!snapshot.isOk())
                return snapshot.status();

            const auto& plan = snapshot->at("plan");
            const auto nextStep = snapshot->value("next_step", 0U);
            if (plan.is_array() && nextStep < plan.size())
                return std::string("execute_step");
            return std::string("solver");
        },
        { "execute_step", "solver" }));
    require(graph.addEdge("solver", std::string(lgc::END)));

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    auto input = lgc::State::fromJsonValue({
        { "task", "Prepare a client-side AI lab workflow recommendation." },
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
