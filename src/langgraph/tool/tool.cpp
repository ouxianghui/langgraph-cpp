#include "langgraph/tool/tool.hpp"

#include <utility>

namespace lgc {
namespace {

[[nodiscard]] ToolSpec specFromTool(const Tool& tool)
{
    return ToolSpec {
        .name_ = tool.name_,
        .description_ = tool.description_,
        .inputSchema_ = tool.inputSchema_,
        .outputSchema_ = tool.outputSchema_,
        .enabled_ = tool.enabled_,
        .riskLevel_ = tool.riskLevel_,
    };
}

[[nodiscard]] Result<void> validateToolSpec(const ToolSpec& spec)
{
    if (spec.name_.empty())
        return Status::invalidArgument("tool name cannot be empty");
    return okResult();
}

[[nodiscard]] Result<void> validateTool(const Tool& tool)
{
    if (auto status = validateToolSpec(specFromTool(tool)); !status.isOk())
        return status.status();
    if (!tool.callable_)
        return Status::invalidArgument("tool callable cannot be empty");
    return okResult();
}

[[nodiscard]] Result<void> validateRuntimeTool(const std::shared_ptr<BaseTool>& tool)
{
    if (!tool)
        return Status::invalidArgument("tool cannot be null");
    return validateToolSpec(tool->spec());
}

[[nodiscard]] Result<BaseMessage> latestAssistantMessage(const std::vector<BaseMessage>& messages)
{
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->type_ == MessageType::AI)
            return *it;
    }
    return Status::notFound("assistant message not found");
}

[[nodiscard]] nlohmann::json toolEventPayload(
    const ToolRequest& request,
    const ToolSpec* spec,
    nlohmann::json extra = nlohmann::json::object())
{
    extra["call_id"] = request.callId_;
    extra["tool"] = request.name_;
    if (spec) {
        extra["enabled"] = spec->enabled_;
        extra["risk"] = toolRiskLevelName(spec->riskLevel_);
    }
    return extra;
}

} // namespace

ToolResult ToolResult::success(nlohmann::json result)
{
    return ToolResult {
        .ok_ = true,
        .result_ = std::move(result),
    };
}

ToolResult ToolResult::command(Command routeCommand, nlohmann::json result)
{
    return ToolResult {
        .ok_ = true,
        .result_ = std::move(result),
        .command_ = std::move(routeCommand),
    };
}

ToolResult ToolResult::failure(ToolError error)
{
    return ToolResult {
        .ok_ = false,
        .error_ = std::move(error),
    };
}

ToolResult ToolResult::failure(
    ToolErrorCode code,
    std::string message,
    nlohmann::json details)
{
    return failure(ToolError {
        .code_ = code,
        .message_ = std::move(message),
        .details_ = std::move(details),
    });
}

Result<nlohmann::json> ToolRuntime::interrupt(
    std::string id,
    nlohmann::json payload)
{
    if (!interrupt_)
        return Status::failedPrecondition("tool interrupt is not available in this context");
    return interrupt_(std::move(id), std::move(payload));
}

std::string_view toolErrorCodeName(ToolErrorCode code) noexcept
{
    switch (code) {
    case ToolErrorCode::ValidationError:
        return "validation_error";
    case ToolErrorCode::NotFound:
        return "not_found";
    case ToolErrorCode::Disabled:
        return "disabled";
    case ToolErrorCode::Timeout:
        return "timeout";
    case ToolErrorCode::RuntimeError:
        return "runtime_error";
    case ToolErrorCode::OutputValidationError:
        return "output_validation_error";
    case ToolErrorCode::Rejected:
        return "rejected";
    }
    return "runtime_error";
}

Result<ToolErrorCode> toolErrorCodeFromName(std::string_view name)
{
    if (name == "validation_error")
        return ToolErrorCode::ValidationError;
    if (name == "not_found")
        return ToolErrorCode::NotFound;
    if (name == "disabled")
        return ToolErrorCode::Disabled;
    if (name == "timeout")
        return ToolErrorCode::Timeout;
    if (name == "runtime_error")
        return ToolErrorCode::RuntimeError;
    if (name == "output_validation_error")
        return ToolErrorCode::OutputValidationError;
    if (name == "rejected")
        return ToolErrorCode::Rejected;
    return Status::invalidArgument("unknown tool error code");
}

std::string_view toolRiskLevelName(ToolRiskLevel risk) noexcept
{
    switch (risk) {
    case ToolRiskLevel::Safe:
        return "safe";
    case ToolRiskLevel::ReadOnly:
        return "read_only";
    case ToolRiskLevel::IdempotentWrite:
        return "idempotent_write";
    case ToolRiskLevel::ExternalSideEffect:
        return "external_side_effect";
    case ToolRiskLevel::Destructive:
        return "destructive";
    case ToolRiskLevel::HardwareControl:
        return "hardware_control";
    }
    return "safe";
}

nlohmann::json toolErrorToJson(const ToolError& error)
{
    nlohmann::json value {
        { "code", toolErrorCodeName(error.code_) },
        { "message", error.message_ },
    };
    if (!error.details_.empty())
        value["details"] = error.details_;
    return value;
}

Result<ToolError> toolErrorFromJson(const nlohmann::json& value)
{
    if (!value.is_object())
        return Status::invalidArgument("tool error must be a JSON object");
    if (!value.contains("code") || !value.at("code").is_string())
        return Status::invalidArgument("tool error code is required");
    if (!value.contains("message") || !value.at("message").is_string())
        return Status::invalidArgument("tool error message is required");

    auto code = toolErrorCodeFromName(value.at("code").get<std::string>());
    if (!code.isOk())
        return code.status();

    ToolError error {
        .code_ = *code,
        .message_ = value.at("message").get<std::string>(),
    };
    if (value.contains("details"))
        error.details_ = value.at("details");
    return error;
}

nlohmann::json toolResultToJson(const ToolResult& result)
{
    if (result.ok_) {
        return nlohmann::json {
            { "ok", true },
            { "result", result.result_ },
        };
    }

    return nlohmann::json {
        { "ok", false },
        { "error", result.error_.has_value()
                ? toolErrorToJson(*result.error_)
                : toolErrorToJson(ToolError {
                    .code_ = ToolErrorCode::RuntimeError,
                    .message_ = "tool failed",
                }) },
    };
}

Result<ToolResult> toolResultFromJson(const nlohmann::json& value)
{
    if (!value.is_object())
        return Status::invalidArgument("tool result must be a JSON object");
    if (!value.contains("ok") || !value.at("ok").is_boolean())
        return Status::invalidArgument("tool result ok is required");

    const auto ok = value.at("ok").get<bool>();
    if (ok) {
        if (!value.contains("result"))
            return ToolResult::success();
        return ToolResult::success(value.at("result"));
    }

    if (!value.contains("error"))
        return Status::invalidArgument("tool result error is required");
    auto error = toolErrorFromJson(value.at("error"));
    if (!error.isOk())
        return error.status();
    return ToolResult::failure(std::move(*error));
}

BaseMessage toolResultMessage(
    std::string toolCallId,
    std::string toolName,
    ToolResult result)
{
    return BaseMessage::tool(
        std::move(toolCallId),
        std::move(toolName),
        toolResultToJson(result).dump());
}

BaseMessage toolErrorMessage(
    std::string toolCallId,
    std::string toolName,
    ToolErrorCode code,
    std::string message,
    nlohmann::json details)
{
    return toolResultMessage(
        std::move(toolCallId),
        std::move(toolName),
        ToolResult::failure(code, std::move(message), std::move(details)));
}

FunctionTool::FunctionTool(Tool tool)
    : spec_(specFromTool(tool))
{
    auto callable = std::move(tool.callable_);
    handler_ = [callable = std::move(callable)](
                   const ToolRequest& request,
                   ToolRuntime& context) -> Result<ToolResult> {
        (void)context;
        auto output = callable(request.arguments_);
        if (!output.isOk())
            return output.status();
        return ToolResult::success(std::move(*output));
    };
}

FunctionTool::FunctionTool(ToolSpec spec, ToolHandler handler)
    : spec_(std::move(spec))
    , handler_(std::move(handler))
{
}

const ToolSpec& FunctionTool::spec() const noexcept
{
    return spec_;
}

Result<ToolResult> FunctionTool::invoke(
    const ToolRequest& request,
    ToolRuntime& context)
{
    if (!handler_)
        return Status::invalidArgument("function tool handler cannot be empty");
    return handler_(request, context);
}

Result<void> ToolRegistry::add(Tool tool)
{
    if (auto result = validateTool(tool); !result.isOk())
        return result.status();

    return add(std::make_shared<FunctionTool>(std::move(tool)));
}

Result<void> ToolRegistry::add(std::shared_ptr<BaseTool> tool)
{
    if (auto result = validateRuntimeTool(tool); !result.isOk())
        return result.status();

    std::lock_guard lock(mutex_);
    const auto& spec = tool->spec();
    if (tools_.contains(spec.name_))
        return Status::alreadyExists("tool already registered: " + spec.name_);
    tools_.emplace(spec.name_, Entry {
        .tool_ = std::move(tool),
        .enabled_ = spec.enabled_,
    });
    return okResult();
}

Result<void> ToolRegistry::enable(std::string_view name)
{
    return setAvailability(name, true);
}

Result<void> ToolRegistry::disable(std::string_view name)
{
    return setAvailability(name, false);
}

Result<void> ToolRegistry::setAvailability(std::string_view name, bool enabled)
{
    std::lock_guard lock(mutex_);
    const auto found = tools_.find(std::string(name));
    if (found == tools_.end())
        return Status::notFound("tool not found");
    found->second.enabled_ = enabled;
    return okResult();
}

Result<std::shared_ptr<BaseTool>> ToolRegistry::tool(std::string_view name) const
{
    std::lock_guard lock(mutex_);
    const auto found = tools_.find(std::string(name));
    if (found == tools_.end())
        return Status::notFound("tool not found: " + std::string(name));
    return found->second.tool_;
}

Result<ToolSpec> ToolRegistry::spec(std::string_view name) const
{
    std::lock_guard lock(mutex_);
    const auto found = tools_.find(std::string(name));
    if (found == tools_.end())
        return Status::notFound("tool not found: " + std::string(name));
    auto spec = found->second.tool_->spec();
    spec.enabled_ = found->second.enabled_;
    return spec;
}

bool ToolRegistry::has(std::string_view name) const
{
    std::lock_guard lock(mutex_);
    return tools_.contains(std::string(name));
}

std::vector<ToolSpec> ToolRegistry::list() const
{
    std::lock_guard lock(mutex_);
    std::vector<ToolSpec> out;
    out.reserve(tools_.size());
    for (const auto& [name, entry] : tools_) {
        (void)name;
        auto spec = entry.tool_->spec();
        spec.enabled_ = entry.enabled_;
        out.push_back(std::move(spec));
    }
    return out;
}

ToolExecutor::ToolExecutor(
    std::shared_ptr<ToolRegistry> registry,
    ToolPolicy policy)
    : registry_(std::move(registry))
    , policy_(std::move(policy))
{
}

Status ToolExecutor::emit(
    RuntimeEventType type,
    const ToolRequest& request,
    const ToolSpec* spec,
    ToolRuntime& context,
    nlohmann::json payload,
    std::string message) const
{
    if (!policy_.emitEvents_ || !context.events_)
        return Status::ok();

    auto event = RuntimeEvent::create(type);
    event.runId_ = context.runId_;
    event.threadId_ = context.threadId_;
    event.step_ = context.step_;
    event.node_ = context.nodeId_;
    event.name_ = request.name_;
    event.message_ = std::move(message);
    event.payload_ = toolEventPayload(request, spec, std::move(payload));
    return context.events_(std::move(event));
}

Result<ToolResult> ToolExecutor::invoke(
    const ToolRequest& request,
    ToolRuntime& context) const
{
    if (!registry_)
        return Status::invalidArgument("tool executor requires a tool registry");
    if (request.name_.empty()) {
        return ToolResult::failure(
            ToolErrorCode::ValidationError,
            "tool request name cannot be empty");
    }
    if (auto status = context.cancellationToken_.check(); !status.isOk())
        return status;
    context.toolCallId_ = request.callId_;

    auto tool = registry_->tool(request.name_);
    if (!tool.isOk()) {
        auto result = ToolResult::failure(
            ToolErrorCode::NotFound,
            tool.status().toString());
        (void)emit(RuntimeEventType::ToolCallFailed, request, nullptr, context, {}, tool.status().toString());
        return result;
    }

    auto spec = registry_->spec(request.name_);
    if (!spec.isOk())
        return spec.status();

    auto fail = [&](ToolErrorCode code, std::string message, nlohmann::json details = nlohmann::json::object()) -> Result<ToolResult> {
        auto result = ToolResult::failure(code, message, std::move(details));
        (void)emit(RuntimeEventType::ToolCallFailed, request, &*spec, context, {}, message);
        return result;
    };

    if (!spec->enabled_)
        return fail(ToolErrorCode::Disabled, "tool is disabled");

    SchemaValidator validator;
    if (policy_.validateInput_) {
        if (auto status = validator.check(request.arguments_, spec->inputSchema_); !status.isOk())
            return fail(ToolErrorCode::ValidationError, status.toString());
    }

    if (policy_.authorize_) {
        if (auto authorized = policy_.authorize_(*spec, request, context); !authorized.isOk())
            return fail(ToolErrorCode::Rejected, authorized.status().toString());
    }

    if (auto status = emit(RuntimeEventType::ToolCallStarted, request, &*spec, context); !status.isOk())
        return status;

    auto output = (*tool)->invoke(request, context);
    if (!output.isOk()) {
        if (output.status().code() == StatusCode::Aborted)
            return output.status();
        return fail(ToolErrorCode::RuntimeError, output.status().toString());
    }

    if (!output->ok_) {
        const auto message = output->error_.has_value()
            ? output->error_->message_
            : std::string("tool failed");
        (void)emit(RuntimeEventType::ToolCallFailed, request, &*spec, context, {}, message);
        return output;
    }

    if (policy_.validateOutput_) {
        if (auto status = validator.check(output->result_, spec->outputSchema_); !status.isOk())
            return fail(ToolErrorCode::OutputValidationError, status.toString());
    }

    if (auto status = emit(
            RuntimeEventType::ToolCallCompleted,
            request,
            &*spec,
            context,
            { { "ok", true } }); !status.isOk()) {
        return status;
    }

    return output;
}

ToolNode::ToolNode(
    std::shared_ptr<ToolRegistry> registry,
    ToolNodeOptions options)
    : registry_(std::move(registry))
    , options_(std::move(options))
{
}

Result<NodeOutput> ToolNode::operator()(
    const State& state,
    Runtime& runtimeContext) const
{
    if (!registry_)
        return Status::invalidArgument("tool node requires a tool registry");

    auto messages = messagesFromStateJson(state.view(), options_.messagesKey_);
    if (!messages.isOk())
        return messages.status();

    auto assistant = latestAssistantMessage(*messages);
    if (!assistant.isOk())
        return assistant.status();
    if (assistant->toolCalls_.empty())
        return NodeOutput::update(StateUpdate::empty());

    ToolExecutor executor(
        registry_,
        ToolPolicy {
            .validateInput_ = true,
            .validateOutput_ = options_.validateOutput_,
        });
    ToolRuntime toolRuntime {
        .runId_ = std::string(runtimeContext.executionInfo().runId_),
        .threadId_ = std::string(runtimeContext.executionInfo().threadId_),
        .nodeId_ = std::string(runtimeContext.executionInfo().nodeId_),
        .step_ = runtimeContext.executionInfo().step_,
        .cancellationToken_ = runtimeContext.cancellationToken(),
        .events_ = [&runtimeContext](RuntimeEvent event) {
            return runtimeContext.streamWriter().publish(std::move(event));
        },
        .store_ = runtimeContext.store(),
        .interrupt_ = [&runtimeContext](
                          std::string id,
                          nlohmann::json payload) -> Result<nlohmann::json> {
            return runtimeContext.interrupt(std::move(id), std::move(payload));
        },
    };

    std::vector<BaseMessage> toolMessages;
    toolMessages.reserve(assistant->toolCalls_.size());
    std::optional<Command> routeCommand;

    for (const auto& toolCall : assistant->toolCalls_) {
        auto result = executor.invoke(
            ToolRequest {
                .callId_ = toolCall.id_,
                .name_ = toolCall.name_,
                .arguments_ = toolCall.args_,
            },
            toolRuntime);
        if (!result.isOk())
            return result.status();

        if (result->command_.has_value()) {
            if (routeCommand.has_value())
                return Status::failedPrecondition("multiple tool-returned commands are not supported in one tool node");
            routeCommand = std::move(result->command_);
        }

        toolMessages.push_back(toolResultMessage(
            toolCall.id_,
            toolCall.name_,
            std::move(*result)));
    }

    auto messagesUpdate = StateUpdate::fromJsonValue({
        { options_.messagesKey_, messagesToJson(toolMessages) },
    });
    if (!messagesUpdate.isOk())
        return messagesUpdate.status();

    if (!routeCommand.has_value())
        return NodeOutput::update(std::move(*messagesUpdate));

    auto commandState = routeCommand->update_.toState();
    if (!commandState.isOk())
        return commandState.status();
    auto messageState = messagesUpdate->toState();
    if (!messageState.isOk())
        return messageState.status();

    nlohmann::json merged = commandState->view();
    for (const auto& item : messageState->view().items())
        merged[item.key()] = item.value();
    auto mergedUpdate = StateUpdate::fromJsonValue(merged);
    if (!mergedUpdate.isOk())
        return mergedUpdate.status();
    routeCommand->update_ = std::move(*mergedUpdate);
    return NodeOutput::command(std::move(*routeCommand));
}

bool toolsCondition(
    const State& state,
    std::string_view messagesKey)
{
    auto messages = messagesFromStateJson(state.view(), messagesKey);
    if (!messages.isOk())
        return false;
    auto assistant = latestAssistantMessage(*messages);
    if (!assistant.isOk())
        return false;
    return !assistant->toolCalls_.empty();
}

} // namespace lgc
