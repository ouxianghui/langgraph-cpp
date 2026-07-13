#include <langgraph_cpp/langgraph.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace {

bool ok(const lc::Result<void>& result)
{
    if (result.isOk())
        return true;
    std::cerr << result.status().toString() << '\n';
    return false;
}

} // namespace

int main()
{
    lc::StateGraph graph;

    if (!ok(graph.addNode("plan", [](const lc::State&, lc::Runtime&) {
            return lc::StateUpdate::fromJson(R"({"subjects":["edge","robot","sensor"]})");
        }))) {
        return 1;
    }
    if (!ok(graph.addNode("generate", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
            auto json = state.toJson();
            if (!json.isOk())
                return json.status();
            const auto subject = json->at("subject").get<std::string>();
            return lc::StateUpdate::fromJsonValue({
                { "drafts", nlohmann::json::array({ subject + "-draft" }) },
            });
        }))) {
        return 1;
    }
    if (!ok(graph.addNode("join", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
            auto json = state.toJson();
            if (!json.isOk())
                return json.status();
            return lc::StateUpdate::fromJsonValue({
                { "draft_count", json->at("drafts").size() },
                { "joined", true },
            });
        }))) {
        return 1;
    }

    if (!ok(graph.addEdge(std::string(lc::START), "plan")))
        return 1;
    if (!ok(graph.addConditionalEdges(
            "plan",
            [](const lc::State& state, lc::Runtime&) -> lc::Result<std::vector<lc::Send>> {
                auto json = state.toJson();
                if (!json.isOk())
                    return json.status();

                std::vector<lc::Send> sends;
                for (const auto& subject : json->at("subjects")) {
                    auto branch = lc::State::fromJsonValue({
                        { "subject", subject },
                    });
                    if (!branch.isOk())
                        return branch.status();
                    sends.push_back(lc::Send("generate", std::move(*branch)));
                }
                return sends;
            },
            { "generate" }))) {
        return 1;
    }
    if (!ok(graph.addEdge("generate", "join")))
        return 1;
    if (!ok(graph.addEdge("join", std::string(lc::END))))
        return 1;

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status().toString() << '\n';
        return 1;
    }

    lc::RunOptions options;
    options.reducers_.set("drafts", lc::ReducerKind::Append);

    auto input = lc::State::fromJson("{}");
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
