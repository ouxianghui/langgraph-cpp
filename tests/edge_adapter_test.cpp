#include <langgraph_cpp/langgraph.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

namespace {

class MockGpioAdapter final : public lgc::IGpioAdapter {
public:
    [[nodiscard]] lgc::Result<void> configurePin(const lgc::GpioPinConfig& config) override
    {
        directions_[config.line_] = config.direction_;
        if (config.initialLevel_.has_value())
            levels_[config.line_] = *config.initialLevel_;
        return lgc::okResult();
    }

    [[nodiscard]] lgc::Result<lgc::GpioLevel> readPin(std::string line) override
    {
        const auto found = levels_.find(line);
        if (found == levels_.end())
            return lgc::Status::notFound("gpio line not configured");
        return found->second;
    }

    [[nodiscard]] lgc::Result<void> writePin(std::string line, lgc::GpioLevel level) override
    {
        const auto found = directions_.find(line);
        if (found == directions_.end())
            return lgc::Status::notFound("gpio line not configured");
        if (found->second != lgc::GpioDirection::Output)
            return lgc::Status::failedPrecondition("gpio line is not an output");
        levels_[line] = level;
        return lgc::okResult();
    }

private:
    std::map<std::string, lgc::GpioDirection> directions_;
    std::map<std::string, lgc::GpioLevel> levels_;
};

lgc::State stateFromMessages(std::vector<lgc::BaseMessage> messages)
{
    auto state = lgc::State::fromJsonValue({
        { "messages", lgc::messagesToJson(messages) },
    });
    assert(state.isOk());
    return *state;
}

std::filesystem::path temporarySysfsRoot()
{
    return std::filesystem::temp_directory_path()
        / ("langgraph_cpp_gpio_sysfs_"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

void writeFile(const std::filesystem::path& path, std::string_view value)
{
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    assert(out);
    out << value;
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream in(path);
    assert(in);
    std::string value;
    in >> value;
    return value;
}

std::shared_ptr<lgc::ToolRegistry> makeGpioRegistry(std::shared_ptr<lgc::IGpioAdapter> adapter)
{
    auto registry = std::make_shared<lgc::ToolRegistry>();

    auto registered = registry->add(lgc::Tool {
        .name_ = "edge.gpio_write",
        .description_ = "Write a GPIO line through an edge adapter.",
        .inputSchema_ = lgc::JsonSchema::object()
                            .property("line", lgc::JsonSchema::string(), true)
                            .property("high", lgc::JsonSchema::boolean(), true)
                            .additionalProperties(false),
        .outputSchema_ = lgc::JsonSchema::object()
                             .property("line", lgc::JsonSchema::string(), true)
                             .property("high", lgc::JsonSchema::boolean(), true)
                             .additionalProperties(false),
        .callable_ = [adapter = std::move(adapter)](const nlohmann::json& input) -> lgc::Result<nlohmann::json> {
            const auto line = input.at("line").get<std::string>();
            const auto high = input.at("high").get<bool>();
            auto status = adapter->writePin(line, high ? lgc::GpioLevel::High : lgc::GpioLevel::Low);
            if (!status.isOk())
                return status.status();
            return nlohmann::json {
                { "line", line },
                { "high", high },
            };
        },
    });
    assert(registered.isOk());

    return registry;
}

void testMockGpioToolExecutesThroughRegistry()
{
    auto gpio = std::make_shared<MockGpioAdapter>();
    auto configured = gpio->configurePin(lgc::GpioPinConfig {
        .line_ = "cooling-fan-enable",
        .direction_ = lgc::GpioDirection::Output,
        .initialLevel_ = lgc::GpioLevel::Low,
    });
    assert(configured.isOk());

    auto registry = makeGpioRegistry(gpio);
    auto node = lgc::ToolNode(
        registry,
        lgc::ToolNodeOptions {
            .validateOutput_ = true,
        });

    lgc::Runtime context(lgc::Runtime::Options {
        .runId_ = "run",
        .threadId_ = "thread",
        .step_ = 1,
        .nodeId_ = "tools",
    });
    auto output = node(
        stateFromMessages({
            lgc::BaseMessage::ai(
                "",
                {
                    lgc::ToolCall {
                        .id_ = "call-gpio",
                        .name_ = "edge.gpio_write",
                        .args_ = {
                            { "line", "cooling-fan-enable" },
                            { "high", true },
                        },
                    },
                }),
        }),
        context);
    assert(output.isOk());
    assert(!output->command_.has_value());

    auto level = gpio->readPin("cooling-fan-enable");
    assert(level.isOk());
    assert(*level == lgc::GpioLevel::High);

    auto messages = lgc::messagesFromJson(output->update_.values().at("messages"));
    assert(messages.isOk());
    assert(messages->size() == 1);
    auto result = lgc::toolResultFromJson(nlohmann::json::parse(messages->front().content_));
    assert(result.isOk());
    assert(result->ok_);
    assert(result->result_.at("line") == "cooling-fan-enable");
    assert(result->result_.at("high") == true);
}

void testSysfsGpioAdapterUsesFilesystemProtocol()
{
    const auto root = temporarySysfsRoot();
    const auto lineDir = root / "gpio17";
    std::filesystem::create_directories(lineDir);
    writeFile(lineDir / "direction", "in");
    writeFile(lineDir / "value", "0");

    lgc::SysfsGpioAdapter adapter(lgc::SysfsGpioAdapterOptions {
        .root_ = root,
    });
    auto configured = adapter.configurePin(lgc::GpioPinConfig {
        .line_ = "gpio17",
        .direction_ = lgc::GpioDirection::Output,
        .initialLevel_ = lgc::GpioLevel::High,
    });
    assert(configured.isOk());
    assert(readFile(lineDir / "direction") == "out");
    assert(readFile(lineDir / "value") == "1");

    auto high = adapter.readPin("gpio17");
    assert(high.isOk());
    assert(*high == lgc::GpioLevel::High);

    assert(adapter.writePin("gpio17", lgc::GpioLevel::Low).isOk());
    assert(readFile(lineDir / "value") == "0");
    auto low = adapter.readPin("gpio17");
    assert(low.isOk());
    assert(*low == lgc::GpioLevel::Low);

    auto invalid = adapter.readPin("../gpio17");
    assert(!invalid.isOk());
    assert(invalid.status().code() == lgc::StatusCode::InvalidArgument);

    std::filesystem::remove_all(root);
}

void testEdgeAdapterRegistryReportsCapabilities()
{
    lgc::EdgeAdapterRegistry adapters;
    assert(!adapters.capabilities().any());
    auto missing = adapters.require<lgc::IGpioAdapter>();
    assert(!missing.isOk());
    assert(missing.status().code() == lgc::StatusCode::NotFound);

    auto gpio = std::make_shared<MockGpioAdapter>();
    adapters.set(gpio);
    auto capabilities = adapters.capabilities();
    assert(capabilities.any());
    assert(capabilities.gpio_);
    assert(!capabilities.uart_);

    auto registered = adapters.require<lgc::IGpioAdapter>();
    assert(registered.isOk());
    assert(*registered == gpio);
    assert(adapters.find<lgc::IGpioAdapter>() == gpio);

    adapters.set(std::shared_ptr<lgc::IGpioAdapter> {});
    assert(!adapters.capabilities().gpio_);
}

} // namespace

int main()
{
    testMockGpioToolExecutesThroughRegistry();
    testSysfsGpioAdapterUsesFilesystemProtocol();
    testEdgeAdapterRegistryReportsCapabilities();
    return 0;
}
