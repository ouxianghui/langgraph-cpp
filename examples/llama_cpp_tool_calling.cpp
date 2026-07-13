#include <langgraph_cpp/langgraph.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

void require(lc::Result<void> result)
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

std::shared_ptr<lc::ToolRegistry> makeRegistry()
{
    auto registry = std::make_shared<lc::ToolRegistry>();
    require(registry->add(lc::Tool {
        .name_ = "add",
        .description_ = "Add two integers.",
        .inputSchema_ = lc::JsonSchema::object()
                            .property("a", lc::JsonSchema::integer(), true)
                            .property("b", lc::JsonSchema::integer(), true)
                            .additionalProperties(false),
        .outputSchema_ = lc::JsonSchema::object()
                             .property("value", lc::JsonSchema::integer(), true)
                             .additionalProperties(false),
        .callable_ = [](const nlohmann::json& input) -> lc::Result<nlohmann::json> {
            return nlohmann::json {
                { "value", input.at("a").get<int>() + input.at("b").get<int>() },
            };
        },
    }));
    return registry;
}

} // namespace

int main(int argc, char** argv)
{
    const auto modelPath = modelPathFromArgsOrEnv(argc, argv);
    if (modelPath.empty()) {
        std::cout << nlohmann::json {
            { "error", "missing model path" },
            { "usage", "llama_cpp_tool_calling <model.gguf>" },
            { "env", "LANGGRAPH_CPP_LLAMA_MODEL" },
        }.dump() << '\n';
        return 0;
    }

    auto registry = makeRegistry();
    auto grammar = lc::toolCallJsonGrammar(*registry);
    if (!grammar.isOk()) {
        std::cerr << grammar.status() << '\n';
        return 1;
    }

    auto model = std::make_shared<lc::LlamaCppChatModel>(lc::LlamaCppChatModelOptions {
        .modelPath_ = modelPath,
        .contextSize_ = 2048,
        .temperature_ = 0.0F,
        .maxTokens_ = 192,
        .grammar_ = *grammar,
        .parseToolCallJson_ = true,
    });

    lc::StateGraph graph;
    require(graph.addNode("model", lc::makeModelNode(model)));
    require(graph.addNode("tools", lc::ToolNode(
        registry,
        lc::ToolNodeOptions { .validateOutput_ = true })));
    require(graph.addEdge(std::string(lc::START), "model"));
    require(graph.addEdge("model", "tools"));
    require(graph.addEdge("tools", std::string(lc::END)));

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    auto input = lc::State::fromJsonValue({
        { "messages", lc::messagesToJson({
            lc::BaseMessage::system(
                "Return only JSON matching this exact shape: "
                "{\"tool_calls\":[{\"id\":\"call-1\",\"name\":\"add\",\"arguments\":{\"a\":2,\"b\":3}}]}. "
                "Use tool name add and integer arguments."),
            lc::BaseMessage::human("Use the add tool to calculate 2 + 3."),
        }) },
    });
    if (!input.isOk()) {
        std::cerr << input.status() << '\n';
        return 1;
    }

    lc::RunOptions options;
    options.reducers_.set("messages", lc::ReducerKind::AddMessages);

    auto result = compiled->invoke(*input, options);
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        return 1;
    }

    std::cout << result->state_.json() << '\n';
    return 0;
}
