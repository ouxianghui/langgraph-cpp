#pragma once

#include "foundation/json/json_schema.hpp"
#include "foundation/status/result.hpp"
#include "langgraph/graph/state_graph.hpp"
#include "langgraph/message/message.hpp"
#include "langgraph/store/store.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lc {

/// Callable backing a tool. ToolNode validates inputSchema_ before invoking it.
using ToolCallable = std::function<Result<nlohmann::json>(const nlohmann::json&)>;
using ToolInterruptHandler = std::function<Result<nlohmann::json>(
    std::string id,
    nlohmann::json payload)>;

struct ToolRequest {
    std::string callId_;
    std::string name_;
    nlohmann::json arguments_ { nlohmann::json::object() };
};

struct ToolRuntime {
    std::string runId_;
    std::string threadId_;
    std::string nodeId_;
    std::string toolCallId_;
    StepId step_ { 0 };
    CancellationToken cancellationToken_ { CancellationToken::none() };
    RuntimeEventPublisher events_;
    std::shared_ptr<BaseStore> store_;
    ToolInterruptHandler interrupt_;

    /// Request a graph interrupt from inside a tool handler. On resume, returns
    /// the payload supplied through Command::resume().
    [[nodiscard]] Result<nlohmann::json> interrupt(
        std::string id,
        nlohmann::json payload = nlohmann::json::object());
};

/// Stable error categories serialized into tool result messages.
enum class ToolErrorCode : std::uint8_t {
    ValidationError,
    NotFound,
    Disabled,
    Timeout,
    RuntimeError,
    OutputValidationError,
    Rejected,
};

/// Structured tool failure payload.
struct ToolError {
    ToolErrorCode code_ { ToolErrorCode::RuntimeError };
    std::string message_;
    nlohmann::json details_ { nlohmann::json::object() };

    friend bool operator==(const ToolError&, const ToolError&) = default;
};

/// Tool execution envelope written into tool messages.
struct ToolResult {
    bool ok_ { true };
    nlohmann::json result_ { nlohmann::json::object() };
    std::optional<ToolError> error_;
    std::optional<Command> command_;

    [[nodiscard]] static ToolResult success(nlohmann::json result = nlohmann::json::object());
    [[nodiscard]] static ToolResult command(
        Command command,
        nlohmann::json result = nlohmann::json::object());
    [[nodiscard]] static ToolResult failure(ToolError error);
    [[nodiscard]] static ToolResult failure(
        ToolErrorCode code,
        std::string message,
        nlohmann::json details = nlohmann::json::object());
};

enum class ToolRiskLevel : std::uint8_t {
    Safe,
    ReadOnly,
    IdempotentWrite,
    ExternalSideEffect,
    Destructive,
    HardwareControl,
};

struct ToolSpec {
    std::string name_;
    std::string description_;
    JsonSchema inputSchema_ { JsonSchema::object() };
    JsonSchema outputSchema_ { JsonSchema::any() };
    bool enabled_ { true };
    ToolRiskLevel riskLevel_ { ToolRiskLevel::Safe };
};

/// Convenience function-backed tool description. Registered as a FunctionTool.
struct Tool {
    std::string name_;
    std::string description_;
    JsonSchema inputSchema_ { JsonSchema::object() };
    JsonSchema outputSchema_ { JsonSchema::any() };
    ToolCallable callable_;
    bool enabled_ { true };
    ToolRiskLevel riskLevel_ { ToolRiskLevel::Safe };
};

class BaseTool {
public:
    virtual ~BaseTool() = default;

    [[nodiscard]] virtual const ToolSpec& spec() const noexcept = 0;
    [[nodiscard]] virtual Result<ToolResult> invoke(
        const ToolRequest& request,
        ToolRuntime& context) = 0;
};

using ToolHandler = std::function<Result<ToolResult>(const ToolRequest&, ToolRuntime&)>;

class FunctionTool final : public BaseTool {
public:
    explicit FunctionTool(Tool tool);
    FunctionTool(ToolSpec spec, ToolHandler handler);

    [[nodiscard]] const ToolSpec& spec() const noexcept override;
    [[nodiscard]] Result<ToolResult> invoke(
        const ToolRequest& request,
        ToolRuntime& context) override;

private:
    ToolSpec spec_;
    ToolHandler handler_;
};

using ToolAuthorization = std::function<Result<void>(
    const ToolSpec& spec,
    const ToolRequest& request,
    ToolRuntime& context)>;

struct ToolPolicy {
    bool validateInput_ { true };
    bool validateOutput_ { false };
    bool emitEvents_ { true };
    ToolAuthorization authorize_;
};

[[nodiscard]] std::string_view toolErrorCodeName(ToolErrorCode code) noexcept;
[[nodiscard]] Result<ToolErrorCode> toolErrorCodeFromName(std::string_view name);
[[nodiscard]] std::string_view toolRiskLevelName(ToolRiskLevel risk) noexcept;

[[nodiscard]] nlohmann::json toolErrorToJson(const ToolError& error);
[[nodiscard]] Result<ToolError> toolErrorFromJson(const nlohmann::json& value);
[[nodiscard]] nlohmann::json toolResultToJson(const ToolResult& result);
[[nodiscard]] Result<ToolResult> toolResultFromJson(const nlohmann::json& value);

[[nodiscard]] BaseMessage toolResultMessage(
    std::string toolCallId,
    std::string toolName,
    ToolResult result);
[[nodiscard]] BaseMessage toolErrorMessage(
    std::string toolCallId,
    std::string toolName,
    ToolErrorCode code,
    std::string message,
    nlohmann::json details = nlohmann::json::object());

class ToolRegistry final {
public:
    /// Add a new tool name. Duplicate names are rejected.
    [[nodiscard]] Result<void> add(Tool tool);
    [[nodiscard]] Result<void> add(std::shared_ptr<BaseTool> tool);
    [[nodiscard]] Result<void> enable(std::string_view name);
    [[nodiscard]] Result<void> disable(std::string_view name);
    [[nodiscard]] Result<std::shared_ptr<BaseTool>> tool(std::string_view name) const;
    [[nodiscard]] Result<ToolSpec> spec(std::string_view name) const;
    [[nodiscard]] bool has(std::string_view name) const;
    [[nodiscard]] std::vector<ToolSpec> list() const;

private:
    struct Entry {
        std::shared_ptr<BaseTool> tool_;
        bool enabled_ { true };
    };

    [[nodiscard]] Result<void> setAvailability(std::string_view name, bool enabled);

    mutable std::mutex mutex_;
    std::map<std::string, Entry> tools_;
};

class ToolExecutor final {
public:
    explicit ToolExecutor(
        std::shared_ptr<ToolRegistry> registry,
        ToolPolicy policy = {});

    [[nodiscard]] Result<ToolResult> invoke(
        const ToolRequest& request,
        ToolRuntime& context) const;

private:
    [[nodiscard]] Status emit(
        RuntimeEventType type,
        const ToolRequest& request,
        const ToolSpec* spec,
        ToolRuntime& context,
        nlohmann::json payload = nlohmann::json::object(),
        std::string message = {}) const;

    std::shared_ptr<ToolRegistry> registry_;
    ToolPolicy policy_;
};

struct ToolNodeOptions {
    /// State field containing serialized BaseMessage values.
    std::string messagesKey_ { "messages" };
    /// When true, validate callable output against Tool::outputSchema_.
    bool validateOutput_ { false };
};

/// LangGraph-style prebuilt node that executes tool calls from the latest assistant message.
class ToolNode final {
public:
    explicit ToolNode(
        std::shared_ptr<ToolRegistry> registry,
        ToolNodeOptions options = {});

    [[nodiscard]] Result<NodeOutput> operator()(
        const State& state,
        Runtime& runtimeContext) const;

private:
    std::shared_ptr<ToolRegistry> registry_;
    ToolNodeOptions options_;
};

/// Convenience router predicate for model -> tools -> model loops.
[[nodiscard]] bool toolsCondition(
    const State& state,
    std::string_view messagesKey = "messages");

} // namespace lc
