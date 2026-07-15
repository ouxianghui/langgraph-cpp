#include <langgraph_cpp/langgraph.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

void require(lgc::Result<void> result)
{
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        std::exit(1);
    }
}

std::string modelPathFromArgsOrEnv(int argc, char** argv)
{
    if (argc > 1)
        return argv[1];
    if (const char* value = std::getenv("LANGGRAPH_CPP_LLAMA_MODEL"))
        return value;
    return {};
}

std::string promptFromArgs(int argc, char** argv)
{
    if (argc > 2)
        return argv[2];
    return "Say hello from langgraph-cpp in one short sentence.";
}

} // namespace

int main(int argc, char** argv)
{
    const auto modelPath = modelPathFromArgsOrEnv(argc, argv);
    if (modelPath.empty()) {
        std::cout << nlohmann::json {
            { "error", "missing model path" },
            { "usage", "llama_cpp_chat <model.gguf> [prompt]" },
            { "env", "LANGGRAPH_CPP_LLAMA_MODEL" },
        }.dump() << '\n';
        return 0;
    }

    auto model = std::make_shared<lgc::LlamaCppChatModel>(lgc::LlamaCppChatModelOptions {
        .modelPath_ = modelPath,
        .contextSize_ = 2048,
        .temperature_ = 0.7F,
        .maxTokens_ = 128,
    });

    lgc::StateGraph graph;
    require(graph.addNode("model", lgc::makeModelNode(model)));
    require(graph.addEdge(std::string(lgc::START), "model"));
    require(graph.addEdge("model", std::string(lgc::END)));

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    auto input = lgc::State::fromJsonValue({
        { "messages", lgc::messagesToJson({
            lgc::BaseMessage::human(promptFromArgs(argc, argv)),
        }) },
    });
    if (!input.isOk()) {
        std::cerr << input.status() << '\n';
        return 1;
    }

    lgc::RunOptions options;
    options.reducers_.set("messages", lgc::ReducerKind::AddMessages);

    auto result = compiled->invoke(*input, options);
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        return 1;
    }

    std::cout << result->state_.json() << '\n';
    return 0;
}
