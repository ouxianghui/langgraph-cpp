#include <langgraph_cpp/langgraph.hpp>

#include <iostream>
#include <string>

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

    if (!ok(graph.addNode("decide", [](const lc::State&, lc::Runtime&) -> lc::Result<lc::NodeOutput> {
            auto update = lc::StateUpdate::fromJson(R"({"decision":"repair"})");
            if (!update.isOk())
                return update.status();
            return lc::NodeOutput::command(lc::Command::gotoNode("repair", std::move(*update)));
        }))) {
        return 1;
    }
    if (!ok(graph.addNode("repair", [](const lc::State&, lc::Runtime&) {
            return lc::StateUpdate::fromJson(R"({"repaired":true})");
        }))) {
        return 1;
    }

    if (!ok(graph.addEdge(std::string(lc::START), "decide")))
        return 1;
    if (!ok(graph.addCommandRoute("decide", { "repair" })))
        return 1;
    if (!ok(graph.addEdge("repair", std::string(lc::END))))
        return 1;

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status().toString() << '\n';
        return 1;
    }

    auto input = lc::State::fromJson("{}");
    if (!input.isOk()) {
        std::cerr << input.status().toString() << '\n';
        return 1;
    }

    auto result = compiled->invoke(*input);
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
