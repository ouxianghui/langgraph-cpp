#include <langgraph_cpp/langgraph.hpp>

#include <iostream>
#include <memory>

int main()
{
    auto store = std::make_shared<lgc::InMemoryStore>();

    lgc::StateGraph graph;
    if (auto status = graph.addNode("profile_memory", [](const lgc::State& state, lgc::Runtime& context)
            -> lgc::Result<lgc::StateUpdate> {
            auto store = context.store();
            if (!store)
                return lgc::Status::failedPrecondition("store missing");

            const auto& input = state.view();
            const auto operatorId = input.value("operator_id", "operator-1");
            const lgc::StoreNamespace nameSpace { "operators", operatorId };

            if (input.contains("remember_profile")) {
                if (auto saved = store->put(nameSpace, "profile", input.at("remember_profile")); !saved.isOk())
                    return saved.status();
            }

            auto profile = store->get(nameSpace, "profile");
            if (!profile.isOk())
                return profile.status();

            nlohmann::json update {
                { "operator_id", operatorId },
                { "memory_hit", profile->has_value() },
            };
            if (profile->has_value()) {
                update["profile"] = (*profile)->value_;
                update["recommended_mode"] = (*profile)->value_.value("preferred_mode", "normal");
            }

            return lgc::StateUpdate::fromJsonValue(std::move(update));
        });
        !status.isOk()) {
        std::cerr << status.status() << '\n';
        return 1;
    }

    const auto edgeStatuses = {
        graph.addEdge(std::string(lgc::START), "profile_memory"),
        graph.addEdge("profile_memory", std::string(lgc::END)),
    };
    for (const auto& status : edgeStatuses) {
        if (!status.isOk()) {
            std::cerr << status.status() << '\n';
            return 1;
        }
    }

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    lgc::RunOptions firstRun;
    firstRun.threadId_ = "store-demo-onboarding";
    firstRun.store_ = store;

    auto firstInput = lgc::State::fromJson(R"({
        "operator_id": "operator-1",
        "remember_profile": {
            "name": "Mira",
            "preferred_mode": "quiet",
            "site": "lab-7"
        }
    })");
    if (!firstInput.isOk()) {
        std::cerr << firstInput.status() << '\n';
        return 1;
    }

    auto first = compiled->invoke(*firstInput, firstRun);
    if (!first.isOk()) {
        std::cerr << first.status() << '\n';
        return 1;
    }

    lgc::RunOptions secondRun;
    secondRun.threadId_ = "store-demo-followup";
    secondRun.store_ = store;

    auto secondInput = lgc::State::fromJson(R"({"operator_id":"operator-1"})");
    if (!secondInput.isOk()) {
        std::cerr << secondInput.status() << '\n';
        return 1;
    }

    auto second = compiled->invoke(*secondInput, secondRun);
    if (!second.isOk()) {
        std::cerr << second.status() << '\n';
        return 1;
    }

    auto memories = store->search(lgc::StoreSearchOptions {
        .namespacePrefix_ = { "operators" },
    });
    if (!memories.isOk()) {
        std::cerr << memories.status() << '\n';
        return 1;
    }

    nlohmann::json output {
        { "first_run", first->state_.view() },
        { "second_run", second->state_.view() },
        { "memory_count", memories->size() },
    };
    std::cout << output.dump() << '\n';
    return 0;
}
