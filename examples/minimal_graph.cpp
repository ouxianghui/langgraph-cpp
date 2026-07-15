#include <langgraph_cpp/langgraph.hpp>

#include <iostream>

int main()
{
    lgc::StateGraph graph;
    auto status = graph.addNode("hello", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::fromJson(R"({"message":"hello from langgraph-cpp"})");
    });
    if (!status.isOk()) {
        std::cerr << status.status() << '\n';
        return 1;
    }
    status = graph.addEdge(std::string(lgc::START), "hello");
    if (!status.isOk()) {
        std::cerr << status.status() << '\n';
        return 1;
    }
    status = graph.addEdge("hello", std::string(lgc::END));
    if (!status.isOk()) {
        std::cerr << status.status() << '\n';
        return 1;
    }

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    auto input = lgc::State::fromJson("{}");
    auto result = compiled->invoke(*input);
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        return 1;
    }

    std::cout << result->state_.json() << '\n';
    return 0;
}
