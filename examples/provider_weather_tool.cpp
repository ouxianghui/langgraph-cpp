#include <langgraph_cpp/langgraph.hpp>

#include "foundation/network/http_client.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(lgc::Result<void> result)
{
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        std::exit(1);
    }
}

std::string envOr(const char* name, std::string fallback)
{
    if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0')
        return value;
    return fallback;
}

std::string requireEnv(const char* name)
{
    if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0')
        return value;
    return {};
}

std::string toLower(std::string value)
{
    for (char& ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

void logProviderToolBinding(const lgc::BaseChatModel& model, bool nativeTools)
{
    const auto* provider = dynamic_cast<const lgc::ProviderChatModel*>(&model);
    if (!nativeTools || provider == nullptr) {
        std::cerr << "[provider_weather_tool] http request will NOT include OpenAI tools "
                     "(json content-protocol)\n";
        return;
    }

    const auto& binding = provider->options().toolBinding_;
    if (binding.tools_.empty()) {
        std::cerr << "[provider_weather_tool] http request will NOT include OpenAI tools "
                     "(tool binding is empty)\n";
        return;
    }

    nlohmann::json toolsSummary = nlohmann::json::array();
    for (const auto& tool : binding.tools_) {
        toolsSummary.push_back({
            { "name", tool.name_ },
            { "description", tool.description_ },
            { "input_schema", tool.inputSchema_.rawJson() },
        });
    }

    std::cerr << "[provider_weather_tool] http request WILL include OpenAI tools: "
              << nlohmann::json {
                     { "tool_choice", binding.toolChoice_ },
                     { "tools", std::move(toolsSummary) },
                 }.dump(2)
              << '\n';
}

nlohmann::json mockWeather(std::string_view location)
{
    const auto key = toLower(std::string(location));
    if (key.find("beijing") != std::string::npos || key.find("北京") != std::string::npos) {
        return {
            { "location", "Beijing" },
            { "temperature_c", 28 },
            { "condition", "partly cloudy" },
            { "humidity_pct", 55 },
        };
    }
    if (key.find("shanghai") != std::string::npos || key.find("上海") != std::string::npos) {
        return {
            { "location", "Shanghai" },
            { "temperature_c", 31 },
            { "condition", "humid and sunny" },
            { "humidity_pct", 70 },
        };
    }
    if (key.find("san francisco") != std::string::npos) {
        return {
            { "location", "San Francisco" },
            { "temperature_c", 18 },
            { "condition", "foggy" },
            { "humidity_pct", 80 },
        };
    }
    return {
        { "location", std::string(location) },
        { "temperature_c", 22 },
        { "condition", "clear" },
        { "humidity_pct", 45 },
    };
}

void printUsage()
{
    std::cout << nlohmann::json {
        { "error", "missing OpenAI-compatible API key" },
        { "usage",
          "export LANGGRAPH_CPP_OPENAI_API_KEY=... && "
          "build/unix-debug/examples/provider_weather_tool [question]" },
        { "env",
          {
              { "LANGGRAPH_CPP_OPENAI_API_KEY", "required" },
              { "LANGGRAPH_CPP_OPENAI_BASE_URL",
                "optional, default https://api.openai.com" },
              { "LANGGRAPH_CPP_OPENAI_MODEL", "optional, default gpt-4o-mini" },
              { "LANGGRAPH_CPP_TOOL_MODE",
                "optional: native (default, OpenAI tool_calls) or json (content-protocol)" },
          } },
        { "note",
          "Optional example: hits a real OpenAI-compatible HTTP endpoint. "
          "Not part of the default smoke gate. Use TOOL_MODE=json for proxies that "
          "accept tools but do not emit native tool_calls." },
    }.dump(2)
              << '\n';
}

/// Promote a content-protocol tool request into structured ToolCall fields so ToolNode can run.
[[nodiscard]] lgc::Result<lgc::BaseMessage> promoteJsonToolCalls(lgc::BaseMessage message)
{
    if (!message.toolCalls_.empty() || message.content_.empty())
        return message;

    auto trimmed = message.content_;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
        trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
        trimmed.pop_back();
    if (trimmed.size() >= 7 && trimmed.starts_with("```")) {
        const auto firstNl = trimmed.find('\n');
        if (firstNl == std::string::npos)
            return message;
        trimmed = trimmed.substr(firstNl + 1);
        if (trimmed.ends_with("```"))
            trimmed.resize(trimmed.size() - 3);
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
            trimmed.pop_back();
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(trimmed);
    } catch (const nlohmann::json::exception&) {
        return message;
    }
    if (!parsed.is_object() || !parsed.contains("tool_calls") || !parsed.at("tool_calls").is_array())
        return message;

    std::vector<lgc::ToolCall> toolCalls;
    for (const auto& item : parsed.at("tool_calls")) {
        if (!item.is_object() || !item.contains("name") || !item.at("name").is_string())
            return lgc::Status::invalidArgument("json tool protocol requires tool_calls[].name");
        nlohmann::json args = nlohmann::json::object();
        if (item.contains("arguments")) {
            if (item.at("arguments").is_object()) {
                args = item.at("arguments");
            } else if (item.at("arguments").is_string()) {
                try {
                    args = nlohmann::json::parse(item.at("arguments").get<std::string>());
                } catch (const nlohmann::json::exception& error) {
                    return lgc::Status::invalidArgument(
                        std::string("json tool protocol arguments: ") + error.what());
                }
            } else {
                return lgc::Status::invalidArgument(
                    "json tool protocol arguments must be object or JSON string");
            }
        }
        toolCalls.push_back(lgc::ToolCall {
            .id_ = item.value("id", std::string {}),
            .name_ = item.at("name").get<std::string>(),
            .args_ = std::move(args),
        });
    }
    if (toolCalls.empty())
        return message;

    auto promoted = lgc::BaseMessage::ai({}, std::move(toolCalls));
    promoted.responseMetadata_ = message.responseMetadata_;
    promoted.usageMetadata_ = message.usageMetadata_;
    return promoted;
}

[[nodiscard]] lgc::NodeHandler makeProviderModelNode(
    std::shared_ptr<lgc::BaseChatModel> model,
    bool promoteJsonProtocol)
{
    return [model = std::move(model), promoteJsonProtocol](
               const lgc::State& state,
               lgc::Runtime&) -> lgc::Result<lgc::StateUpdate> {
        auto messages = lgc::messagesFromStateJson(state.view());
        if (!messages.isOk())
            return messages.status();

        auto response = model->invoke(*messages);
        if (!response.isOk())
            return response.status();

        lgc::BaseMessage assistant = std::move(*response);
        if (promoteJsonProtocol) {
            auto promoted = promoteJsonToolCalls(std::move(assistant));
            if (!promoted.isOk())
                return promoted.status();
            assistant = std::move(*promoted);
        }

        return lgc::StateUpdate::fromJsonValue({
            { "messages", lgc::messagesToJson({ assistant }) },
        });
    };
}

} // namespace

int main(int argc, char** argv)
{
    const auto apiKey = requireEnv("LANGGRAPH_CPP_OPENAI_API_KEY");
    if (apiKey.empty()) {
        printUsage();
        return 0;
    }

    const auto baseUrl = envOr("LANGGRAPH_CPP_OPENAI_BASE_URL", "https://api.openai.com");
    const auto modelName = envOr("LANGGRAPH_CPP_OPENAI_MODEL", "qwen3.6:latest");
    const auto toolMode = toLower(envOr("LANGGRAPH_CPP_TOOL_MODE", "native"));
    const bool nativeTools = toolMode != "json";
    const std::string question = argc > 1 ? argv[1] : "What is the weather in Beijing right now?";

    auto httpConfig = lgc::HttpClientConfig::fromOrigin(baseUrl);
    if (!httpConfig.isOk()) {
        std::cerr << httpConfig.status() << '\n';
        return 1;
    }
    httpConfig->readTimeout_ = std::chrono::milliseconds { 120'000 };
    httpConfig->connectTimeout_ = std::chrono::milliseconds { 30'000 };
    httpConfig->logOptions_.logBodies_ = true;
    httpConfig->logOptions_.maxBodyBytes_ = 16 * 1024;

    auto http = std::make_shared<lgc::HttpClient>(std::move(*httpConfig));
    auto options = lgc::ProviderChatModelOptions::openAICompatible(
        http,
        modelName,
        apiKey);
    options.maxOutputTokens_ = 1024;
    if (!nativeTools) {
        // Some Qwen-style proxies spend the budget on reasoning unless thinking is disabled.
        options.extraRequestFields_["enable_thinking"] = false;
    }

    const lgc::JsonSchema weatherSchema = lgc::JsonSchema::object()
                                              .property("location", lgc::JsonSchema::string(), true)
                                              .additionalProperties(false);

    std::shared_ptr<lgc::BaseChatModel> model;
    if (nativeTools) {
        lgc::ProviderChatModel baseModel(std::move(options));
        auto bound = baseModel.bindTools(lgc::ChatModelToolBinding {
            .tools_ = {
                lgc::ChatModelTool {
                    .name_ = "get_weather",
                    .description_ = "Get the current weather for a city or location.",
                    .inputSchema_ = weatherSchema,
                },
            },
            .toolChoice_ = "auto",
        });
        if (!bound.isOk()) {
            std::cerr << bound.status() << '\n';
            return 1;
        }
        model = *bound;
    } else {
        model = std::make_shared<lgc::ProviderChatModel>(std::move(options));
    }
    logProviderToolBinding(*model, nativeTools);

    auto registry = std::make_shared<lgc::ToolRegistry>();
    require(registry->add(lgc::Tool {
        .name_ = "get_weather",
        .description_ = "Get the current weather for a city or location.",
        .inputSchema_ = weatherSchema,
        .callable_ = [](const nlohmann::json& input) -> lgc::Result<nlohmann::json> {
            return mockWeather(input.at("location").get<std::string>());
        },
    }));

    lgc::StateGraph graph;
    require(graph.addNode("model", makeProviderModelNode(model, /*promoteJsonProtocol=*/!nativeTools)));
    require(graph.addNode("tools", lgc::ToolNode(registry)));
    require(graph.addEdge(std::string(lgc::START), "model"));
    require(graph.addConditionalEdges(
        "model",
        [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::NodeId> {
            if (lgc::toolsCondition(state))
                return std::string("tools");
            return std::string(lgc::END);
        },
        { "tools", std::string(lgc::END) }));
    require(graph.addEdge("tools", "model"));

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    std::vector<lgc::BaseMessage> seedMessages;
    if (nativeTools) {
        seedMessages = {
            lgc::BaseMessage::system(
                "You are a helpful assistant. When the user asks about weather, "
                "call the get_weather tool with a location, then answer briefly "
                "using the tool result."),
            lgc::BaseMessage::human(question),
        };
    } else {
        seedMessages = {
            lgc::BaseMessage::system(
                "You are a tool-using assistant. When you need weather data, reply with "
                "ONLY this JSON (no markdown, no extra text):\n"
                "{\"tool_calls\":[{\"id\":\"call-1\",\"name\":\"get_weather\","
                "\"arguments\":{\"location\":\"CITY\"}}]}\n"
                "When you already have the tool result, reply with a short plain-text answer."),
            lgc::BaseMessage::human(question),
        };
    }

    auto input = lgc::State::fromJsonValue({
        { "messages", lgc::messagesToJson(seedMessages) },
    });
    if (!input.isOk()) {
        std::cerr << input.status() << '\n';
        return 1;
    }

    lgc::RunOptions runOptions;
    runOptions.reducers_.set("messages", lgc::ReducerKind::AddMessages);
    runOptions.limits_ = lgc::ResourceLimits {}.maxSteps(8);

    auto result = compiled->invoke(*input, runOptions);
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        return 1;
    }
    if (result->status_ != lgc::RunStatus::Completed) {
        std::cerr << "run finished with status=" << static_cast<int>(result->status_) << '\n';
        std::cerr << result->state_.json() << '\n';
        return 1;
    }

    const auto& stateJson = result->state_.view();
    const auto messages = lgc::messagesFromJson(stateJson.value("messages", nlohmann::json::array()));
    if (!messages.isOk()) {
        std::cerr << messages.status() << '\n';
        return 1;
    }

    std::string finalAnswer;
    for (auto it = messages->rbegin(); it != messages->rend(); ++it) {
        if (it->type_ == lgc::MessageType::AI && it->toolCalls_.empty() && !it->content_.empty()) {
            finalAnswer = it->content_;
            break;
        }
    }

    std::cout << nlohmann::json {
        { "provider", "openai_compatible" },
        { "base_url", baseUrl },
        { "model", modelName },
        { "tool_mode", nativeTools ? "native" : "json" },
        { "question", question },
        { "final_answer", finalAnswer },
        { "messages", stateJson.value("messages", nlohmann::json::array()) },
    }.dump(2)
              << '\n';

    if (finalAnswer.empty()) {
        std::cerr
            << "error: no final assistant text answer "
               "(model may not have emitted tool_calls / plain-text reply; "
               "try LANGGRAPH_CPP_TOOL_MODE=json for proxies without native tools)\n";
        return 1;
    }
    return 0;
}
