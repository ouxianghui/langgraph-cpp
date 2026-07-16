#include "langgraph/graph/graph_run.hh"

#include "langgraph/graph/state_graph.hpp"
#include "langgraph/graph/state_graph_common.hh"

#include "foundation/cancellation/cancellation_token.hpp"
#include "foundation/executor/concurrent_executor.hpp"
#include "foundation/id/id_generator.hpp"
#include "langgraph/graph/graph_namespace.hh"
#include "langgraph/graph/stream_projection.hh"
#include "langgraph/graph/stream_state.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace lgc {
namespace {

constexpr std::string_view kInterruptStateKey = "__interrupt__";
constexpr std::string_view kRunErrorStateKey = "__run_error__";

[[nodiscard]] Result<std::string> generatedId(std::string_view prefix)
{
    static std::atomic<std::uint64_t> counter { 1 };
    return std::string(prefix) + "-" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

void mergeJsonObject(nlohmann::json& target, const nlohmann::json& source)
{
    if (!target.is_object())
        target = nlohmann::json::object();
    if (!source.is_object())
        return;
    for (const auto& item : source.items())
        target[item.key()] = item.value();
}

[[nodiscard]] nlohmann::json tagsToJson(const std::vector<std::string>& tags)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& tag : tags)
        out.push_back(tag);
    return out;
}

[[nodiscard]] Status validateRunOptionsConfig(const RunOptions& options)
{
    if (!options.metadata_.is_object())
        return Status::invalidArgument("RunOptions metadata must be an object");
    if (!options.configurable_.is_object())
        return Status::invalidArgument("RunOptions configurable must be an object");
    return Status::ok();
}

void mergeEventTags(nlohmann::json& payload, const std::vector<std::string>& tags)
{
    if (tags.empty())
        return;
    nlohmann::json merged = tagsToJson(tags);
    if (payload.contains("tags") && payload.at("tags").is_array()) {
        for (const auto& tag : payload.at("tags")) {
            if (tag.is_string())
                merged.push_back(tag);
        }
    }
    payload["tags"] = std::move(merged);
}

void mergeEventMetadata(nlohmann::json& payload, const nlohmann::json& metadata)
{
    if (!metadata.is_object() || metadata.empty())
        return;
    nlohmann::json merged = metadata;
    if (payload.contains("metadata") && payload.at("metadata").is_object())
        mergeJsonObject(merged, payload.at("metadata"));
    payload["metadata"] = std::move(merged);
}

[[nodiscard]] std::int64_t unixMillis(std::chrono::system_clock::time_point time)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        time.time_since_epoch()).count();
}

[[nodiscard]] nlohmann::json stateUpdatePayload(const StateUpdate& update)
{
    return nlohmann::json {
        { "update", update.values() },
    };
}

[[nodiscard]] nlohmann::json runnableConfig(
    std::string_view threadId,
    std::string_view checkpointId,
    std::string_view checkpointNamespace)
{
    nlohmann::json configurable = nlohmann::json::object();
    if (!threadId.empty())
        configurable["thread_id"] = std::string(threadId);
    if (!checkpointId.empty())
        configurable["checkpoint_id"] = std::string(checkpointId);
    configurable["checkpoint_ns"] = std::string(checkpointNamespace);
    return nlohmann::json {
        { "configurable", std::move(configurable) },
    };
}

[[nodiscard]] nlohmann::json checkpointPayload(const Checkpoint& checkpoint)
{
    auto taskJson = [&](const CheckpointTask& task) {
        nlohmann::json out {
            { "id", task.taskId_ },
            { "name", task.nodeId_ },
        };
        if (task.error_.has_value())
            out["error"] = *task.error_;
        if (!task.interrupts_.empty())
            out["interrupts"] = task.interrupts_;
        out["state"] = task.state_.has_value() ? task.state_->view() : nlohmann::json(nullptr);
        return out;
    };

    nlohmann::json nextTasks = nlohmann::json::array();
    for (const auto& task : checkpoint.nextTasks_)
        nextTasks.push_back(taskJson(task));

    nlohmann::json metadata = checkpoint.metadata_;
    metadata["step"] = checkpoint.step_;
    metadata["created_at_ms"] = unixMillis(checkpoint.createdAt_);

    return nlohmann::json {
        { "values", checkpoint.state_.view() },
        { "next", checkpoint.nextNodes_ },
        { "tasks", std::move(nextTasks) },
        { "config", runnableConfig(
            checkpoint.threadId_,
            checkpoint.checkpointId_,
            checkpoint.checkpointNamespace_) },
        { "parent_config", checkpoint.parentCheckpointId_.has_value()
                ? runnableConfig(
                    checkpoint.threadId_,
                    *checkpoint.parentCheckpointId_,
                    checkpoint.checkpointNamespace_)
                : nlohmann::json(nullptr) },
        { "metadata", std::move(metadata) },
    };
}

[[nodiscard]] nlohmann::json taskRuntimePayload(const CheckpointTask& task, StepId step)
{
    nlohmann::json data {
        { "id", task.taskId_ },
        { "task_id", task.taskId_ },
        { "name", task.nodeId_ },
        { "node", task.nodeId_ },
        { "node_id", task.nodeId_ },
        { "step", step },
    };
    if (!task.checkpointNamespace_.empty()) {
        data["checkpoint_ns"] = task.checkpointNamespace_;
        data["ns"] = task.checkpointNamespace_;
        data["namespace"] = detail::namespacePathFromString(task.checkpointNamespace_);
    }
    if (task.order_.has_value())
        data["order"] = *task.order_;
    if (task.error_.has_value())
        data["error"] = *task.error_;
    if (!task.interrupts_.empty())
        data["interrupts"] = task.interrupts_;
    if (!task.metadata_.empty())
        data["metadata"] = task.metadata_;
    if (task.state_.has_value())
        data["state"] = task.state_->view();
    return data;
}

struct PendingInterrupt {
    Interrupt interrupt_;
    NodeId node_;
    std::optional<std::uint64_t> order_;
};

struct InterruptMetadata {
    std::string id_;
    NodeId node_;
    StepId step_ { 0 };
    nlohmann::json value_ { nlohmann::json::object() };
    nlohmann::json resumeValues_ { nlohmann::json::object() };
    std::optional<std::uint64_t> order_;
};

[[nodiscard]] std::string durabilityModeName(Durability mode)
{
    switch (mode) {
    case Durability::Sync:
        return "sync";
    case Durability::Async:
        return "async";
    case Durability::Exit:
        return "exit";
    }
    return "sync";
}

[[nodiscard]] Status exceptionStatus(std::string_view action, const std::exception& error)
{
    std::string message(action);
    message.append(" threw: ");
    message.append(error.what());
    return Status::internal(std::move(message));
}

[[nodiscard]] Status unknownExceptionStatus(std::string_view action)
{
    std::string message(action);
    message.append(" threw an unknown exception");
    return Status::internal(std::move(message));
}

/// Missing order sorts after numbered writes; matches InMemorySaver / StorageSaver.
[[nodiscard]] bool writeOrderLess(const CheckpointWrite& lhs, const CheckpointWrite& rhs)
{
    return lhs.order_.value_or(std::numeric_limits<std::uint64_t>::max())
        < rhs.order_.value_or(std::numeric_limits<std::uint64_t>::max());
}

[[nodiscard]] std::size_t fanOutConcurrency(
    std::size_t taskCount,
    std::size_t maxConcurrency,
    bool boundByHardware) noexcept
{
    if (taskCount <= 1U)
        return taskCount;
    if (maxConcurrency != 0U)
        return std::min(maxConcurrency, taskCount);
    if (!boundByHardware)
        return taskCount;
    const auto hardware = std::thread::hardware_concurrency();
    const std::size_t bound = hardware == 0U ? 4U : static_cast<std::size_t>(hardware);
    return std::min(taskCount, bound);
}

constexpr std::string_view kNodeTimeoutCancelReason = "node handler exceeded timeout";

/// Cooperative node-timeout helper: cancels the attempt token after the deadline.
class AttemptTimeoutWatch final {
public:
    AttemptTimeoutWatch(
        std::optional<std::chrono::steady_clock::time_point> deadline,
        CancellationSource& cancel)
        : cancel_(cancel)
    {
        if (!deadline.has_value())
            return;
        watch_ = std::thread([this, deadline]() {
            while (!stop_.load(std::memory_order_acquire)
                && std::chrono::steady_clock::now() < *deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!stop_.load(std::memory_order_acquire))
                cancel_.cancel(std::string(kNodeTimeoutCancelReason));
        });
    }

    AttemptTimeoutWatch(const AttemptTimeoutWatch&) = delete;
    AttemptTimeoutWatch& operator=(const AttemptTimeoutWatch&) = delete;

    ~AttemptTimeoutWatch()
    {
        stop();
    }

    void stop()
    {
        stop_.store(true, std::memory_order_release);
        if (watch_.joinable())
            watch_.join();
    }

private:
    CancellationSource& cancel_;
    std::atomic<bool> stop_ { false };
    std::thread watch_;
};

void appendPlainTask(std::vector<CheckpointTask>& tasks, CheckpointTask task)
{
    if (task.nodeId_.empty() || task.nodeId_ == END)
        return;
    if (task.order_.has_value()) {
        tasks.push_back(std::move(task));
        return;
    }
    const auto duplicate = std::ranges::find_if(tasks, [&](const CheckpointTask& existing) {
        return !existing.state_.has_value() && !existing.order_.has_value() && existing.nodeId_ == task.nodeId_;
    });
    if (duplicate != tasks.end())
        return;
    tasks.push_back(std::move(task));
}

void appendPlainTask(std::vector<CheckpointTask>& tasks, NodeId node)
{
    appendPlainTask(tasks, CheckpointTask {
                               .nodeId_ = std::move(node),
                           });
}

void appendSendTask(std::vector<CheckpointTask>& tasks, CheckpointTask task)
{
    if (task.nodeId_.empty() || task.nodeId_ == END)
        return;
    tasks.push_back(std::move(task));
}

void appendTask(std::vector<CheckpointTask>& tasks, CheckpointTask task)
{
    if (task.state_.has_value())
        appendSendTask(tasks, std::move(task));
    else
        appendPlainTask(tasks, std::move(task));
}

[[nodiscard]] std::vector<CheckpointTask> normalizeNextTasks(std::vector<CheckpointTask> tasks)
{
    std::vector<CheckpointTask> normalized;
    normalized.reserve(tasks.size());
    for (auto& task : tasks)
        appendTask(normalized, std::move(task));
    return normalized;
}

[[nodiscard]] std::vector<NodeId> nodeIdsFromTasks(const std::vector<CheckpointTask>& tasks)
{
    std::vector<NodeId> nodes;
    nodes.reserve(tasks.size());
    for (const auto& task : tasks) {
        if (!task.nodeId_.empty() && task.nodeId_ != END)
            nodes.push_back(task.nodeId_);
    }
    return nodes;
}

[[nodiscard]] bool isTerminalNextTasks(const std::vector<CheckpointTask>& nextTasks)
{
    return std::ranges::all_of(nextTasks, [](const CheckpointTask& task) {
        return task.nodeId_.empty() || task.nodeId_ == END;
    });
}

[[nodiscard]] Result<InterruptMetadata> interruptMetadataFromJson(
    const nlohmann::json& value,
    StepId fallbackStep)
{
    if (!value.is_object())
        return Status::invalidArgument("interrupt metadata entry must be an object");
    if (!value.contains("node") || !value.at("node").is_string())
        return Status::invalidArgument("interrupt metadata node must be a string");

    InterruptMetadata metadata {
        .node_ = value.at("node").get<std::string>(),
        .step_ = fallbackStep,
    };
    if (value.contains("id")) {
        if (!value.at("id").is_string())
            return Status::invalidArgument("interrupt metadata id must be a string");
        metadata.id_ = value.at("id").get<std::string>();
    }
    if (value.contains("step")) {
        if (!value.at("step").is_number_unsigned())
            return Status::invalidArgument("interrupt metadata step must be an unsigned integer");
        metadata.step_ = value.at("step").get<StepId>();
    }
    if (value.contains("value"))
        metadata.value_ = value.at("value");
    if (value.contains("resume_values")) {
        if (!value.at("resume_values").is_object())
            return Status::invalidArgument("interrupt metadata resume_values must be an object");
        metadata.resumeValues_ = value.at("resume_values");
    }
    if (value.contains("order")) {
        if (!value.at("order").is_number_unsigned())
            return Status::invalidArgument("interrupt metadata order must be an unsigned integer");
        metadata.order_ = value.at("order").get<std::uint64_t>();
    }
    return metadata;
}

[[nodiscard]] Result<std::vector<InterruptMetadata>> interruptMetadataFromState(const State& state)
{
    const auto& json = state.view();
    const auto found = json.find(std::string(kInterruptStateKey));
    if (found == json.end())
        return std::vector<InterruptMetadata> {};
    if (!found->is_object())
        return Status::invalidArgument("interrupt metadata must be an object");

    StepId step = 0;
    if (found->contains("step")) {
        if (!found->at("step").is_number_unsigned())
            return Status::invalidArgument("interrupt metadata step must be an unsigned integer");
        step = found->at("step").get<StepId>();
    }

    if (found->contains("interrupts")) {
        if (!found->at("interrupts").is_array())
            return Status::invalidArgument("interrupt metadata interrupts must be an array");
        std::vector<InterruptMetadata> out;
        out.reserve(found->at("interrupts").size());
        for (const auto& item : found->at("interrupts")) {
            auto parsed = interruptMetadataFromJson(item, step);
            if (!parsed.isOk())
                return parsed.status();
            out.push_back(std::move(*parsed));
        }
        return out;
    }

    auto parsed = interruptMetadataFromJson(*found, step);
    if (!parsed.isOk())
        return parsed.status();
    return std::vector<InterruptMetadata> { std::move(*parsed) };
}

[[nodiscard]] Result<nlohmann::json> resumeValueForInterrupt(
    const nlohmann::json& commandValue,
    const InterruptMetadata& metadata,
    std::size_t interruptCount)
{
    if (!metadata.resumeValues_.empty()) {
        nlohmann::json merged = metadata.resumeValues_;
        if (!merged.is_object())
            return Status::invalidArgument("interrupt metadata resume_values must be an object");
        if (commandValue.is_object()) {
            for (const auto& item : commandValue.items())
                merged[item.key()] = item.value();
        } else if (!metadata.id_.empty()) {
            merged[metadata.id_] = commandValue;
        } else {
            merged["value"] = commandValue;
        }
        return merged;
    }
    if (interruptCount <= 1U)
        return commandValue;
    if (!commandValue.is_object())
        return Status::invalidArgument("multi-interrupt resume value must be an object keyed by interrupt id or node");
    if (!metadata.id_.empty() && commandValue.contains(metadata.id_))
        return commandValue.at(metadata.id_);
    if (commandValue.contains(metadata.node_))
        return commandValue.at(metadata.node_);
    return Status::failedPrecondition("resume value missing for interrupted node: " + metadata.node_);
}

[[nodiscard]] Result<State> withInterruptMetadata(
    const State& state,
    const std::vector<PendingInterrupt>& interrupts,
    StepId step)
{
    if (interrupts.empty())
        return State::fromJsonValue(state.view());

    nlohmann::json entries = nlohmann::json::array();
    for (const auto& pending : interrupts) {
        nlohmann::json entry {
            { "id", pending.interrupt_.id_ },
            { "node", pending.node_ },
            { "step", step },
            { "value", pending.interrupt_.value_ },
        };
        if (!pending.interrupt_.resumeValues_.empty())
            entry["resume_values"] = pending.interrupt_.resumeValues_;
        if (pending.order_.has_value())
            entry["order"] = *pending.order_;
        entries.push_back(std::move(entry));
    }

    nlohmann::json json = state.view();
    json[std::string(kInterruptStateKey)] = {
        { "step", step },
        { "interrupts", std::move(entries) },
    };
    return State::fromJsonValue(json);
}

[[nodiscard]] Result<Interrupt> interruptFromRuntimeRequest(const nlohmann::json& request)
{
    if (!request.is_object())
        return Status::invalidArgument("runtime interrupt request must be an object");
    if (!request.contains("id") || !request.at("id").is_string())
        return Status::invalidArgument("runtime interrupt request id must be a string");

    Interrupt interrupt {
        .id_ = request.at("id").get<std::string>(),
    };
    if (request.contains("value"))
        interrupt.value_ = request.at("value");
    if (request.contains("resume_values")) {
        if (!request.at("resume_values").is_object())
            return Status::invalidArgument("runtime interrupt request resume_values must be an object");
        interrupt.resumeValues_ = request.at("resume_values");
    }
    return interrupt;
}

[[nodiscard]] Result<State> withoutInterruptMetadata(const State& state)
{
    nlohmann::json json = state.view();
    json.erase(std::string(kInterruptStateKey));
    return State::fromJsonValue(json);
}

[[nodiscard]] Result<State> withRunErrorMetadata(
    const State& state,
    const Status& status,
    std::string_view node,
    StepId step,
    RunStatus runStatus)
{
    nlohmann::json json = state.view();
    json[std::string(kRunErrorStateKey)] = {
        { "code", std::string(status.codeName()) },
        { "message", status.toString() },
        { "node", std::string(node) },
        { "step", step },
        { "status", runStatus == RunStatus::Cancelled
                ? "cancelled"
                : runStatus == RunStatus::MaxStepsExceeded
                    ? "max_steps_exceeded"
                    : "failed" },
    };
    return State::fromJsonValue(json);
}

[[nodiscard]] Result<State> withoutRunErrorMetadata(const State& state)
{
    nlohmann::json json = state.view();
    json.erase(std::string(kRunErrorStateKey));
    return State::fromJsonValue(json);
}

[[nodiscard]] Result<void> validateStateSchema(
    const State& state,
    const std::optional<JsonSchema>& schema,
    std::string_view label)
{
    if (!schema.has_value())
        return okResult();
    SchemaValidator validator;
    if (auto status = validator.check(state.view(), *schema); !status.isOk()) {
        std::string message(label);
        message.append(" schema validation failed: ");
        message.append(status.toString());
        return Status(status.code(), std::move(message));
    }
    return okResult();
}

} // namespace

namespace detail {

GraphRun::GraphRun(
    std::shared_ptr<const StateGraph> graph,
    NextTasksAfter nextTasksAfter,
    State state,
    std::vector<CheckpointTask> nextTasks,
    std::vector<CheckpointWrite> pendingWrites,
    StepId step,
    std::optional<std::string> parentCheckpointId,
    RunOptions options,
    bool writeInitialCheckpoint,
    bool allowParentCommand)
    : graph_(std::move(graph))
    , nextTasksAfter_(std::move(nextTasksAfter))
    , state_(std::move(state))
    , nextTasks_(std::move(nextTasks))
    , pendingWrites_(std::move(pendingWrites))
    , step_(step)
    , parentCheckpointId_(std::move(parentCheckpointId))
    , options_(std::move(options))
    , writeInitialCheckpoint_(writeInitialCheckpoint)
    , allowParentCommand_(allowParentCommand)
{
}

Status GraphRun::publish(RuntimeEvent event)
{
    if (event.runId_.empty())
        event.runId_ = result_.runId_;
    if (event.threadId_.empty())
        event.threadId_ = result_.threadId_;
    if (!result_.checkpointNamespace_.empty() && event.payload_.is_object()) {
        if (!event.payload_.contains("ns"))
            event.payload_["ns"] = result_.checkpointNamespace_;
        if (!event.payload_.contains("checkpoint_ns"))
            event.payload_["checkpoint_ns"] = result_.checkpointNamespace_;
    }
    if (event.payload_.is_object()) {
        mergeEventTags(event.payload_, options_.tags_);
        mergeEventMetadata(event.payload_, options_.metadata_);
    }
    if (!options_.eventTypes_.empty() && !options_.eventTypes_.contains(event.type_))
        return Status::ok();
    if (auto status = ensureRuntimeEventIdentity(event); !status.isOk())
        return status;
    if (auto status = validateRuntimeEvent(event); !status.isOk())
        return status;
    if (options_.collectEvents_) {
        std::lock_guard lock(publishMutex_);
        result_.events_.push_back(event);
    }
    // Callbacks/sinks run without publishMutex_ so node handlers cannot deadlock
    // by publishing while already inside an event callback.
    if (options_.eventCallback_) {
        if (auto status = options_.eventCallback_(event); !status.isOk())
            return status;
    }
    if (options_.eventSink_)
        return options_.eventSink_->publish(std::move(event));
    return Status::ok();
}

Result<void> GraphRun::emit(
    RuntimeEventType type,
    StepId step,
    std::string node,
    nlohmann::json payload,
    std::string message)
{
    auto event = RuntimeEvent::create(type);
    event.step_ = step;
    event.node_ = std::move(node);
    event.payload_ = std::move(payload);
    event.message_ = std::move(message);
    if (auto status = publish(std::move(event)); !status.isOk())
        return status;
    return okResult();
}

void GraphRun::annotateTasks(std::vector<CheckpointTask>& tasks, StepId taskStep)
{
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        if (tasks[i].nodeId_.empty() || tasks[i].nodeId_ == END)
            continue;
        if (tasks[i].checkpointNamespace_.empty())
            tasks[i].checkpointNamespace_ = result_.checkpointNamespace_;
        if (!tasks[i].order_.has_value())
            tasks[i].order_ = static_cast<std::uint64_t>(i);
        if (tasks[i].taskId_.empty()) {
            tasks[i].taskId_ = result_.runId_;
            tasks[i].taskId_.append(":");
            tasks[i].taskId_.append(std::to_string(taskStep));
            tasks[i].taskId_.append(":");
            tasks[i].taskId_.append(std::to_string(i));
            tasks[i].taskId_.append(":");
            tasks[i].taskId_.append(tasks[i].nodeId_);
        }
    }
}

Result<void> GraphRun::putCheckpoint(
    StepId step,
    std::vector<CheckpointTask> next,
    std::vector<CheckpointWrite> writes,
    std::vector<CheckpointWrite> pending,
    std::string source,
    bool terminal,
    bool requireCheckpointEvent)
{
    next = normalizeNextTasks(std::move(next));
    annotateTasks(next, step);
    const bool shouldStore = options_.durability_ != Durability::Exit
        || source == "initial"
        || terminal;

    Checkpoint checkpoint {
        .threadId_ = result_.threadId_,
        .checkpointId_ = source == "task_writes"
            ? result_.runId_ + "-step-" + std::to_string(step) + "-writes-" + std::to_string(pending.size())
            : result_.runId_ + "-step-" + std::to_string(step),
        .checkpointNamespace_ = result_.checkpointNamespace_,
        .parentCheckpointId_ = parentCheckpointId_,
        .step_ = step,
        .state_ = result_.state_,
        .nextNodes_ = nodeIdsFromTasks(next),
        .nextTasks_ = std::move(next),
        .writes_ = std::move(writes),
        .pendingWrites_ = std::move(pending),
        .metadata_ = {
            { "source", std::move(source) },
            { "durability", durabilityModeName(options_.durability_) },
        },
        .createdAt_ = std::chrono::system_clock::now(),
    };

    if (options_.checkpointer_ && shouldStore) {
        if (auto stored = options_.checkpointer_->put(checkpoint); !stored.isOk())
            return stored.status();
        if (!checkpoint.pendingWrites_.empty()) {
            auto storedWrites = options_.checkpointer_->putWrites(CheckpointWriteSet {
                .threadId_ = checkpoint.threadId_,
                .checkpointNamespace_ = checkpoint.checkpointNamespace_,
                .checkpointId_ = checkpoint.checkpointId_,
                .taskId_ = checkpoint.checkpointId_,
                .taskPath_ = checkpoint.metadata_.value("source", ""),
                .writes_ = checkpoint.pendingWrites_,
            });
            if (!storedWrites.isOk())
                return storedWrites.status();
        }
    }

    if (shouldStore) {
        parentCheckpointId_ = checkpoint.checkpointId_;
        if (auto emitted = emit(RuntimeEventType::CheckpointCreated, step, {}, checkpointPayload(checkpoint));
            !emitted.isOk() && requireCheckpointEvent) {
            return emitted.status();
        }
    }
    return okResult();
}

Result<RunResult> GraphRun::failRun(
    Status status,
    StepId failedStep,
    std::string node,
    std::vector<CheckpointTask> failureNext,
    std::vector<CheckpointWrite> failurePendingWrites)
{
    const auto failedNode = node;
    result_.status_ = runStatusFromStatus(status);
    auto failedState = withRunErrorMetadata(
        result_.state_,
        status,
        node,
        failedStep,
        result_.status_);
    if (!failedState.isOk())
        return failedState.status();
    result_.state_ = std::move(*failedState);

    failureNext = normalizeNextTasks(std::move(failureNext));
    if (failureNext.empty() && !node.empty())
        failureNext.push_back(CheckpointTask { .nodeId_ = failedNode });
    if (auto stored = putCheckpoint(
            failedStep,
            std::move(failureNext),
            {},
            std::move(failurePendingWrites),
            "failure",
            true,
            false);
        !stored.isOk()) {
        return stored.status();
    }
    (void)emit(
        RuntimeEventType::RunFailed,
        failedStep,
        std::move(node),
        {
            { "node", failedNode },
            { "step", failedStep },
            { "status", std::string(status.codeName()) },
            { "error", {
                { "message", status.toString() },
                { "code", std::string(status.codeName()) },
            } },
        },
        status.toString());
    return result_;
}

GraphRun::NodeExecution GraphRun::executeTask(
    std::size_t index,
    CheckpointTask task,
    const State& inputState,
    StepId step)
{
    const auto nodeFound = graph_->nodes_.find(task.nodeId_);
    if (nodeFound == graph_->nodes_.end()) {
        auto status = Status::failedPrecondition("node is unknown: " + task.nodeId_);
        return NodeExecution {
            .task_ = std::move(task),
            .status_ = std::move(status),
        };
    }

    const State& nodeInput = task.state_.has_value() ? *task.state_ : inputState;
    std::optional<nlohmann::json> nodeResumeValue;
    if (const auto found = resumeValuesByTask_.find(index); found != resumeValuesByTask_.end())
        nodeResumeValue = found->second;

    const auto& spec = nodeFound->second;
    const auto& nodeOptions = spec.options_;
    const std::size_t attempts = std::max<std::size_t>(1U, nodeOptions.retry_.maxAttempts_);
    auto delay = nodeOptions.retry_.initialInterval_;
    Status finalStatus = Status::unknown("node handler did not run");

    for (std::size_t attempt = 1; attempt <= attempts; ++attempt) {
        if (auto status = options_.cancellationToken_.check(); !status.isOk()) {
            return NodeExecution {
                .task_ = std::move(task),
                .status_ = status,
            };
        }

        const auto started = std::chrono::steady_clock::now();
        std::optional<std::chrono::steady_clock::time_point> deadline;
        if (nodeOptions.timeout_.has_value())
            deadline = started + *nodeOptions.timeout_;

        CancellationSource attemptCancel;
        CancellationRegistration parentCancelLink;
        if (options_.cancellationToken_.cancellable()) {
            if (options_.cancellationToken_.cancelled()) {
                attemptCancel.cancel(options_.cancellationToken_.reason());
            } else {
                parentCancelLink = options_.cancellationToken_.onCancel(
                    [&attemptCancel]() { attemptCancel.cancel("operation cancelled"); });
            }
        }

        AttemptTimeoutWatch timeoutWatch(deadline, attemptCancel);
        Runtime context(Runtime::Options {
            .runId_ = result_.runId_,
            .threadId_ = result_.threadId_,
            .checkpointNamespace_ = result_.checkpointNamespace_,
            .taskId_ = task.taskId_,
            .step_ = step,
            .nodeId_ = task.nodeId_,
            .publisher_ = [this](RuntimeEvent event) { return publish(std::move(event)); },
            .cancellationToken_ = attemptCancel.token(),
            .resumeValue_ = nodeResumeValue,
            .store_ = options_.store_,
            .checkpointer_ = options_.checkpointer_,
            .attempt_ = attempt,
            .firstAttemptTime_ = started,
            .deadline_ = deadline,
        });

        try {
            auto output = spec.handler_(nodeInput, context);
            timeoutWatch.stop();
            const bool timedOut = deadline.has_value()
                && (std::chrono::steady_clock::now() >= *deadline
                    || attemptCancel.reason() == kNodeTimeoutCancelReason);
            if (timedOut) {
                finalStatus = Status::deadlineExceeded(std::string(kNodeTimeoutCancelReason));
            } else if (!output.isOk()) {
                if (output.status().code() == StatusCode::Aborted && !context.requestedInterrupts().empty()) {
                    auto interrupt = interruptFromRuntimeRequest(context.requestedInterrupts().front());
                    if (!interrupt.isOk()) {
                        finalStatus = interrupt.status();
                    } else {
                        return NodeExecution {
                            .task_ = std::move(task),
                            .output_ = NodeOutput::interrupt(std::move(*interrupt)),
                        };
                    }
                } else {
                    finalStatus = output.status();
                }
            } else {
                return NodeExecution {
                    .task_ = std::move(task),
                    .output_ = std::move(*output),
                };
            }
        } catch (const std::exception& error) {
            timeoutWatch.stop();
            finalStatus = exceptionStatus("node handler", error);
        } catch (...) {
            timeoutWatch.stop();
            finalStatus = unknownExceptionStatus("node handler");
        }

        if (attempt == attempts)
            break;
        if (delay.count() > 0) {
            const auto sleepUntil = std::chrono::steady_clock::now() + delay;
            while (true) {
                if (auto status = options_.cancellationToken_.check(); !status.isOk()) {
                    return NodeExecution {
                        .task_ = std::move(task),
                        .status_ = status,
                    };
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= sleepUntil)
                    break;
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(sleepUntil - now);
                std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(1)));
            }
            const auto scaled = static_cast<double>(delay.count()) * nodeOptions.retry_.backoffFactor_;
            delay = std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(std::max(0.0, scaled)));
        }
    }

    if (nodeOptions.errorHandler_) {
        Runtime context(Runtime::Options {
            .runId_ = result_.runId_,
            .threadId_ = result_.threadId_,
            .checkpointNamespace_ = result_.checkpointNamespace_,
            .taskId_ = task.taskId_,
            .step_ = step,
            .nodeId_ = task.nodeId_,
            .publisher_ = [this](RuntimeEvent event) { return publish(std::move(event)); },
            .cancellationToken_ = options_.cancellationToken_,
            .resumeValue_ = nodeResumeValue,
            .store_ = options_.store_,
            .checkpointer_ = options_.checkpointer_,
            .attempt_ = attempts + 1U,
        });
        try {
            auto output = nodeOptions.errorHandler_(finalStatus, nodeInput, context);
            if (!output.isOk()) {
                if (output.status().code() == StatusCode::Aborted && !context.requestedInterrupts().empty()) {
                    auto interrupt = interruptFromRuntimeRequest(context.requestedInterrupts().front());
                    if (!interrupt.isOk()) {
                        finalStatus = interrupt.status();
                    } else {
                        return NodeExecution {
                            .task_ = std::move(task),
                            .output_ = NodeOutput::interrupt(std::move(*interrupt)),
                        };
                    }
                } else {
                    finalStatus = output.status();
                }
            } else {
                return NodeExecution {
                    .task_ = std::move(task),
                    .output_ = std::move(*output),
                };
            }
        } catch (const std::exception& error) {
            finalStatus = exceptionStatus("node error handler", error);
        } catch (...) {
            finalStatus = unknownExceptionStatus("node error handler");
        }
    }

    return NodeExecution {
        .task_ = std::move(task),
        .status_ = std::move(finalStatus),
    };
}

std::vector<GraphRun::NodeExecution> GraphRun::dispatchExecutions(
    const std::vector<CheckpointTask>& activeTasks,
    const State& inputState,
    StepId step)
{
    std::vector<NodeExecution> executions;
    executions.reserve(activeTasks.size());
    std::shared_ptr<IExecutor> stepExecutor = options_.executor_;
    std::shared_ptr<IExecutor> ownedExecutor;
    const std::size_t concurrency = fanOutConcurrency(
        activeTasks.size(),
        options_.maxConcurrency_,
        !stepExecutor);
    if (!stepExecutor && concurrency > 1U) {
        ownedExecutor = makeConcurrentExecutor(concurrency);
        stepExecutor = ownedExecutor;
    }

    auto getExecution = [](std::future<NodeExecution>& future, const CheckpointTask& task) -> NodeExecution {
        try {
            return future.get();
        } catch (const std::exception& error) {
            return NodeExecution {
                .task_ = task,
                .status_ = exceptionStatus("parallel node task", error),
            };
        } catch (...) {
            return NodeExecution {
                .task_ = task,
                .status_ = unknownExceptionStatus("parallel node task"),
            };
        }
    };

    for (std::size_t begin = 0; begin < activeTasks.size(); begin += concurrency) {
        const std::size_t end = std::min(begin + concurrency, activeTasks.size());
        if (end - begin == 1U) {
            executions.push_back(executeTask(begin, activeTasks[begin], inputState, step));
            continue;
        }

        std::vector<std::optional<std::future<NodeExecution>>> futures(end - begin);
        std::vector<std::optional<NodeExecution>> immediate(end - begin);
        for (std::size_t i = begin; i < end; ++i) {
            const std::size_t local = i - begin;
            auto promise = std::make_shared<std::promise<NodeExecution>>();
            futures[local] = promise->get_future();
            auto status = stepExecutor->execute(
                [this, i, task = activeTasks[i], &inputState, step, promise]() mutable {
                    try {
                        promise->set_value(executeTask(i, std::move(task), inputState, step));
                    } catch (...) {
                        try {
                            promise->set_exception(std::current_exception());
                        } catch (...) {
                        }
                    }
                });
            if (!status.isOk()) {
                futures[local].reset();
                immediate[local] = NodeExecution {
                    .task_ = activeTasks[i],
                    .status_ = status,
                };
            }
        }
        for (std::size_t i = begin; i < end; ++i) {
            const std::size_t local = i - begin;
            if (immediate[local].has_value()) {
                executions.push_back(std::move(*immediate[local]));
                continue;
            }
            executions.push_back(getExecution(*futures[local], activeTasks[i]));
        }
    }
    if (ownedExecutor)
        (void)ownedExecutor->close(std::chrono::milliseconds(0));
    return executions;
}

Result<std::vector<CheckpointTask>> GraphRun::commandTasksAfter(
    const NodeExecution& execution) const
{
    std::vector<CheckpointTask> tasks;
    const auto& command = execution.output_->command_;
    if (!command.has_value() || command->graph_ == CommandGraph::Parent)
        return tasks;

    const auto destinations = graph_->commandDestinations_.find(execution.task_.nodeId_);
    for (const auto& target : command->goto_) {
        if (!graph_->hasNodeOrEnd(target))
            return Status::failedPrecondition("command returned unknown node: " + target);
        if (destinations != graph_->commandDestinations_.end() && !destinations->second.empty()
            && !destinations->second.contains(target)) {
            return Status::failedPrecondition("command returned a node outside declared destinations: " + target);
        }
        appendPlainTask(tasks, target);
    }
    return tasks;
}

Result<CheckpointWrite> GraphRun::writeFromExecution(
    const NodeExecution& execution) const
{
    auto updateState = execution.output_->update_.toState();
    if (!updateState.isOk())
        return updateState.status();

    CheckpointWrite write {
        .taskId_ = execution.task_.taskId_,
        .nodeId_ = execution.task_.nodeId_,
        .checkpointNamespace_ = result_.checkpointNamespace_,
        .update_ = *updateState,
        .order_ = execution.task_.order_,
        .metadata_ = {
            { "source", "node" },
        },
    };
    if (execution.output_->command_.has_value()) {
        auto commandTasks = commandTasksAfter(execution);
        if (!commandTasks.isOk())
            return commandTasks.status();
        write.hasNextTasks_ = true;
        write.nextTasks_ = std::move(*commandTasks);
    }
    return write;
}

Result<void> GraphRun::applyWrite(const CheckpointWrite& write, StepId step)
{
    auto update = StateUpdate::fromJsonValue(write.update_.view());
    if (!update.isOk())
        return update.status();
    auto merged = applyStateUpdate(result_.state_, *update, options_.reducers_);
    if (!merged.isOk())
        return merged.status();
    result_.state_ = std::move(*merged);
    if (auto status = validateStateSchema(result_.state_, graph_->schemas_.stateSchema_, "state"); !status.isOk())
        return status.status();

    auto payload = stateUpdatePayload(*update);
    if (!write.taskId_.empty())
        payload["task_id"] = write.taskId_;
    if (!write.checkpointNamespace_.empty()) {
        payload["checkpoint_ns"] = write.checkpointNamespace_;
        payload["ns"] = write.checkpointNamespace_;
        payload["namespace"] = detail::namespacePathFromString(write.checkpointNamespace_);
    }
    if (write.order_.has_value())
        payload["order"] = *write.order_;
    if (!write.metadata_.empty())
        payload["metadata"] = write.metadata_;
    return emit(RuntimeEventType::StateUpdated, step, write.nodeId_, std::move(payload));
}

Result<void> GraphRun::routeFromWrite(
    const CheckpointWrite& write,
    std::vector<CheckpointTask>& routedNext,
    StepId step)
{
    if (write.hasNextTasks_) {
        for (auto task : write.nextTasks_)
            appendTask(routedNext, std::move(task));
        return okResult();
    }

    Runtime routeContext(Runtime::Options {
        .runId_ = result_.runId_,
        .threadId_ = result_.threadId_,
        .checkpointNamespace_ = result_.checkpointNamespace_,
        .step_ = step,
        .nodeId_ = write.nodeId_,
        .publisher_ = [this](RuntimeEvent event) { return publish(std::move(event)); },
        .cancellationToken_ = options_.cancellationToken_,
        .store_ = options_.store_,
        .checkpointer_ = options_.checkpointer_,
    });
    auto routed = nextTasksAfter_(write.nodeId_, result_.state_, routeContext);
    if (!routed.isOk())
        return routed.status();
    for (auto& task : *routed)
        appendTask(routedNext, std::move(task));
    return okResult();
}

Result<RunResult> GraphRun::run()
{
    State state = std::move(state_);
    std::vector<CheckpointTask> nextTasks = std::move(nextTasks_);
    std::vector<CheckpointWrite> pendingWrites = std::move(pendingWrites_);
    StepId step = step_;
    RunOptions& options = options_;
    const bool writeInitialCheckpoint = writeInitialCheckpoint_;
    const bool allowParentCommand = allowParentCommand_;

    if (auto status = validateRunOptionsConfig(options); !status.isOk())
        return status;
    nextTasks = normalizeNextTasks(std::move(nextTasks));

    result_ = RunResult {
        .state_ = std::move(state),
        .step_ = step,
    };
    RunResult& result = result_;

    auto runId = options.runId_.empty() ? generatedId("run") : Result<std::string>(options.runId_);
    if (!runId.isOk())
        return runId.status();
    auto threadId = options.threadId_.empty() ? generatedId("thread") : Result<std::string>(options.threadId_);
    if (!threadId.isOk())
        return threadId.status();

    result.runId_ = std::move(*runId);
    result.threadId_ = std::move(*threadId);
    result.checkpointNamespace_ = options.checkpointNamespace_;

    if (writeInitialCheckpoint) {
        if (auto status = validateStateSchema(result.state_, graph_->schemas_.inputSchema_, "input"); !status.isOk())
            return status.status();
    }

    auto taskFailurePayload = [](const CheckpointTask& task, StepId taskStep, const Status& status) {
        auto payload = taskRuntimePayload(task, taskStep);
        payload["error"] = {
            { "message", status.toString() },
            { "code", std::string(status.codeName()) },
        };
        return payload;
    };

    if (writeInitialCheckpoint && graph_->routers_.contains(std::string(START))) {
        Runtime context(Runtime::Options {
            .runId_ = result.runId_,
            .threadId_ = result.threadId_,
            .checkpointNamespace_ = result.checkpointNamespace_,
            .step_ = result.step_,
            .nodeId_ = std::string(START),
            .publisher_ = [this](RuntimeEvent event) { return publish(std::move(event)); },
            .cancellationToken_ = options.cancellationToken_,
            .store_ = options.store_,
            .checkpointer_ = options.checkpointer_,
        });
        auto routed = nextTasksAfter_(std::string(START), result.state_, context);
        if (!routed.isOk())
            return routed.status();
        nextTasks = normalizeNextTasks(std::move(*routed));
    }

    if (auto emitted = emit(RuntimeEventType::RunStarted, result.step_, {}); !emitted.isOk())
        return failRun(emitted.status(), result.step_, {}, nextTasks, pendingWrites);

    if (writeInitialCheckpoint) {
        if (auto stored = putCheckpoint(step, nextTasks, {}, {}, "initial", false); !stored.isOk())
            return stored.status();
    }

    resumeValuesByTask_.clear();
    auto& resumeValuesByTask = resumeValuesByTask_;
    if (!writeInitialCheckpoint) {
        auto cleaned = withoutRunErrorMetadata(result.state_);
        if (!cleaned.isOk())
            return failRun(cleaned.status(), result.step_, {}, nextTasks, pendingWrites);
        result.state_ = std::move(*cleaned);
    }
    if (options.command_.has_value()) {
        if (!options.command_->resume_.has_value())
            return failRun(Status::invalidArgument("unsupported command"), result.step_, {}, nextTasks, pendingWrites);
        auto interrupted = interruptMetadataFromState(result.state_);
        if (!interrupted.isOk())
            return failRun(interrupted.status(), result.step_, {}, nextTasks, pendingWrites);
        if (interrupted->empty()) {
            if (nextTasks.size() != 1U) {
                return failRun(
                    Status::failedPrecondition(
                        "resume command requires a single pending node when no interrupt metadata exists"),
                    result.step_,
                    {},
                    nextTasks,
                    pendingWrites);
            }
            resumeValuesByTask.emplace(0U, *options.command_->resume_);
        } else {
            for (const auto& metadata : *interrupted) {
                std::optional<std::size_t> matched;
                for (std::size_t i = 0; i < nextTasks.size(); ++i) {
                    if (resumeValuesByTask.contains(i))
                        continue;
                    if (nextTasks[i].nodeId_ != metadata.node_)
                        continue;
                    if (metadata.order_.has_value() && nextTasks[i].order_.has_value()
                        && *metadata.order_ != *nextTasks[i].order_) {
                        continue;
                    }
                    if (matched.has_value()) {
                        return failRun(
                            Status::failedPrecondition("resume command matches multiple pending tasks: " + metadata.node_),
                            result.step_,
                            metadata.node_,
                            nextTasks,
                            pendingWrites);
                    }
                    matched = i;
                }
                if (!matched.has_value()) {
                    return failRun(
                        Status::failedPrecondition("resume node is not pending: " + metadata.node_),
                        result.step_,
                        metadata.node_,
                        nextTasks,
                        pendingWrites);
                }
                auto value = resumeValueForInterrupt(*options.command_->resume_, metadata, interrupted->size());
                if (!value.isOk())
                    return failRun(value.status(), result.step_, metadata.node_, nextTasks, pendingWrites);
                resumeValuesByTask.emplace(*matched, std::move(*value));
            }
        }
        auto cleaned = withoutInterruptMetadata(result.state_);
        if (!cleaned.isOk())
            return failRun(cleaned.status(), result.step_, {}, nextTasks, pendingWrites);
        result.state_ = std::move(*cleaned);
        auto resumedState = applyStateUpdate(result.state_, options.command_->update_, options.reducers_);
        if (!resumedState.isOk())
            return failRun(resumedState.status(), result.step_, {}, nextTasks, pendingWrites);
        result.state_ = std::move(*resumedState);
    }
    if (auto status = validateStateSchema(result.state_, graph_->schemas_.stateSchema_, "state"); !status.isOk())
        return failRun(status.status(), result.step_, {}, nextTasks, pendingWrites);

    ExecutionBudget budget(options.limits_);

    while (!isTerminalNextTasks(nextTasks)) {
        nextTasks = normalizeNextTasks(std::move(nextTasks));
        std::vector<CheckpointTask> activeTasks = nextTasks;
        const StepId nextStep = result.step_ + 1;
        annotateTasks(activeTasks, nextStep);
        const NodeId failureNode = activeTasks.empty() ? NodeId {} : activeTasks.front().nodeId_;

        if (auto status = options.cancellationToken_.check(); !status.isOk()) {
            return failRun(status, nextStep, failureNode, activeTasks, pendingWrites);
        }
        if (auto status = budget.consumeStep(); !status.isOk()) {
            return failRun(status, nextStep, failureNode, activeTasks, pendingWrites);
        }

        const State inputState = result.state_;

        for (const auto& task : activeTasks) {
            if (auto emitted = emit(
                    RuntimeEventType::NodeStarted,
                    nextStep,
                    task.nodeId_,
                    [&] {
                        auto payload = taskRuntimePayload(task, nextStep);
                        payload["input"] = task.state_.has_value()
                            ? task.state_->view()
                            : inputState.view();
                        payload["triggers"] = nlohmann::json::array();
                        if (nextStep == 1U)
                            payload["triggers"].push_back(std::string(START));
                        return payload;
                    }());
                !emitted.isOk()) {
                return failRun(emitted.status(), nextStep, task.nodeId_, activeTasks, pendingWrites);
            }
        }

        auto executions = dispatchExecutions(activeTasks, inputState, nextStep);

        std::vector<std::size_t> failedIndexes;
        failedIndexes.reserve(executions.size());
        for (std::size_t i = 0; i < executions.size(); ++i) {
            if (!executions[i].status_.isOk()) {
                (void)emit(
                    RuntimeEventType::NodeFailed,
                    nextStep,
                    executions[i].task_.nodeId_,
                    taskFailurePayload(executions[i].task_, nextStep, executions[i].status_),
                    executions[i].status_.toString());
                failedIndexes.push_back(i);
            }
        }

        for (std::size_t i = 0; i < executions.size(); ++i) {
            if (!executions[i].status_.isOk())
                continue;
            if (!executions[i].output_.has_value()) {
                auto status = Status::internal("node execution missing output");
                (void)emit(RuntimeEventType::NodeFailed, nextStep, executions[i].task_.nodeId_, {}, status.toString());
                failedIndexes.push_back(i);
                executions[i].status_ = status;
                continue;
            }
            if (executions[i].output_->interrupt_.has_value() && executions[i].output_->command_.has_value()) {
                auto status = Status::invalidArgument("node output cannot combine interrupt and command routing");
                (void)emit(RuntimeEventType::NodeFailed, nextStep, executions[i].task_.nodeId_, {}, status.toString());
                failedIndexes.push_back(i);
                executions[i].status_ = status;
                continue;
            }
            if (executions[i].output_->command_.has_value()
                && executions[i].output_->command_->resume_.has_value()) {
                auto status = Status::invalidArgument("node returned unsupported resume command");
                (void)emit(RuntimeEventType::NodeFailed, nextStep, executions[i].task_.nodeId_, {}, status.toString());
                failedIndexes.push_back(i);
                executions[i].status_ = status;
                continue;
            }
            if (executions[i].output_->command_.has_value()
                && executions[i].output_->command_->graph_ == CommandGraph::Parent
                && !allowParentCommand) {
                auto status = Status::failedPrecondition("node returned parent command outside a subgraph");
                (void)emit(RuntimeEventType::NodeFailed, nextStep, executions[i].task_.nodeId_, {}, status.toString());
                failedIndexes.push_back(i);
                executions[i].status_ = status;
                continue;
            }
        }

        std::vector<std::size_t> interruptIndexes;
        std::optional<std::size_t> parentCommandIndex;
        for (std::size_t i = 0; i < executions.size(); ++i) {
            if (!executions[i].status_.isOk())
                continue;
            if (executions[i].output_->interrupt_.has_value())
                interruptIndexes.push_back(i);
            if (executions[i].output_->command_.has_value()
                && executions[i].output_->command_->graph_ == CommandGraph::Parent) {
                if (parentCommandIndex.has_value()) {
                    auto status = Status::failedPrecondition("multiple parent commands in one super-step are not supported");
                    (void)emit(RuntimeEventType::NodeFailed, nextStep, executions[i].task_.nodeId_, {}, status.toString());
                    failedIndexes.push_back(i);
                    executions[i].status_ = status;
                    continue;
                }
                parentCommandIndex = i;
            }
        }
        if (parentCommandIndex.has_value() && !interruptIndexes.empty()) {
            auto status = Status::failedPrecondition("parent command cannot be combined with interrupts in one super-step");
            (void)emit(RuntimeEventType::NodeFailed, nextStep, executions[*parentCommandIndex].task_.nodeId_, {}, status.toString());
            failedIndexes.push_back(*parentCommandIndex);
            executions[*parentCommandIndex].status_ = status;
        }

        if (!failedIndexes.empty()) {
            std::vector<CheckpointTask> failureNext;
            failureNext.reserve(failedIndexes.size());
            for (const auto index : failedIndexes)
                appendTask(failureNext, executions[index].task_);

            std::vector<CheckpointWrite> failurePendingWrites = pendingWrites;
            const bool canStoreSuccessfulWrites = interruptIndexes.empty();
            if (canStoreSuccessfulWrites) {
                for (std::size_t i = 0; i < executions.size(); ++i) {
                    if (!executions[i].status_.isOk())
                        continue;
                    if (auto emitted = emit(
                            RuntimeEventType::NodeCompleted,
                            nextStep,
                            executions[i].task_.nodeId_,
                            [&] {
                                auto payload = taskRuntimePayload(executions[i].task_, nextStep);
                                payload["result"] = stateUpdatePayload(executions[i].output_->update_);
                                payload["update"] = executions[i].output_->update_.values();
                                return payload;
                            }());
                        !emitted.isOk()) {
                        return failRun(
                            emitted.status(),
                            nextStep,
                            executions[i].task_.nodeId_,
                            std::move(failureNext),
                            std::move(failurePendingWrites));
                    }
                    auto write = writeFromExecution(executions[i]);
                    if (!write.isOk()) {
                        (void)emit(RuntimeEventType::NodeFailed, nextStep, executions[i].task_.nodeId_, {}, write.status().toString());
                        failureNext.clear();
                        appendTask(failureNext, executions[i].task_);
                        return failRun(write.status(), nextStep, executions[i].task_.nodeId_, std::move(failureNext), std::move(failurePendingWrites));
                    }
                    failurePendingWrites.push_back(std::move(*write));
                }
            } else {
                failureNext = activeTasks;
            }

            const auto firstFailure = failedIndexes.front();
            return failRun(
                executions[firstFailure].status_,
                nextStep,
                executions[firstFailure].task_.nodeId_,
                std::move(failureNext),
                std::move(failurePendingWrites));
        }

        for (const auto& execution : executions) {
            if (!execution.output_->interrupt_.has_value()) {
                if (auto emitted = emit(
                        RuntimeEventType::NodeCompleted,
                        nextStep,
                        execution.task_.nodeId_,
                        [&] {
                            auto payload = taskRuntimePayload(execution.task_, nextStep);
                            payload["result"] = stateUpdatePayload(execution.output_->update_);
                            payload["update"] = execution.output_->update_.values();
                            return payload;
                        }());
                    !emitted.isOk()) {
                    return failRun(emitted.status(), nextStep, execution.task_.nodeId_, activeTasks, pendingWrites);
                }
            }
        }

        std::vector<CheckpointWrite> writes = pendingWrites;
        writes.reserve(pendingWrites.size() + executions.size());
        const bool persistTaskWrites = options.durability_ == Durability::Sync
            && interruptIndexes.empty()
            && !parentCommandIndex.has_value();
        for (std::size_t i = 0; i < executions.size(); ++i) {
            const auto& execution = executions[i];
            auto write = writeFromExecution(execution);
            if (!write.isOk()) {
                (void)emit(RuntimeEventType::NodeFailed, nextStep, execution.task_.nodeId_, {}, write.status().toString());
                return failRun(write.status(), nextStep, execution.task_.nodeId_, { execution.task_ }, pendingWrites);
            }
            writes.push_back(std::move(*write));
            if (persistTaskWrites && i + 1U < executions.size()) {
                std::vector<CheckpointTask> remainingTasks;
                remainingTasks.reserve(executions.size() - i - 1U);
                for (std::size_t j = i + 1U; j < executions.size(); ++j)
                    appendTask(remainingTasks, executions[j].task_);
                if (auto stored = putCheckpoint(
                        nextStep,
                        std::move(remainingTasks),
                        {},
                        writes,
                        "task_writes",
                        false);
                    !stored.isOk()) {
                    (void)emit(RuntimeEventType::RunFailed, nextStep, execution.task_.nodeId_, {}, stored.status().toString());
                    return stored.status();
                }
            }
        }

        std::stable_sort(writes.begin(), writes.end(), writeOrderLess);

        for (const auto& write : writes) {
            if (auto applied = applyWrite(write, nextStep); !applied.isOk()) {
                (void)emit(RuntimeEventType::NodeFailed, nextStep, write.nodeId_, {}, applied.status().toString());
                return failRun(applied.status(), nextStep, write.nodeId_, activeTasks, pendingWrites);
            }
        }

        std::vector<CheckpointTask> routedNext;
        if (parentCommandIndex.has_value()) {
            result.step_ = nextStep;
            pendingWrites.clear();
            nextTasks.clear();
            result.parentCommand_ = executions[*parentCommandIndex].output_->command_;

            if (auto status = validateStateSchema(result.state_, graph_->schemas_.outputSchema_, "output"); !status.isOk())
                return failRun(status.status(), result.step_, executions[*parentCommandIndex].task_.nodeId_);
            result.status_ = RunStatus::Completed;
            if (auto emitted = emit(
                    RuntimeEventType::RunCompleted,
                    result.step_,
                    {},
                    { { "state", result.state_.view() }, { "parent_command", true } });
                !emitted.isOk()) {
                return failRun(emitted.status(), result.step_, executions[*parentCommandIndex].task_.nodeId_);
            }
            if (auto stored = putCheckpoint(nextStep, nextTasks, std::move(writes), {}, "parent_command", true); !stored.isOk()) {
                (void)emit(RuntimeEventType::RunFailed, nextStep, executions[*parentCommandIndex].task_.nodeId_, {}, stored.status().toString());
                return stored.status();
            }
            return result;
        }

        if (!interruptIndexes.empty()) {
            for (const auto& write : writes) {
                bool isInterruptedWrite = false;
                for (const auto index : interruptIndexes) {
                    if (write.nodeId_ == executions[index].task_.nodeId_
                        && write.order_ == executions[index].task_.order_) {
                        isInterruptedWrite = true;
                        break;
                    }
                }
                if (isInterruptedWrite)
                    continue;
                if (auto routed = routeFromWrite(write, routedNext, nextStep); !routed.isOk()) {
                    (void)emit(RuntimeEventType::NodeFailed, nextStep, write.nodeId_, {}, routed.status().toString());
                    return failRun(routed.status(), nextStep, write.nodeId_, activeTasks);
                }
            }

            std::vector<CheckpointTask> pausedNext;
            std::vector<PendingInterrupt> pendingInterrupts;
            pendingInterrupts.reserve(interruptIndexes.size());
            for (const auto index : interruptIndexes) {
                appendTask(pausedNext, executions[index].task_);
                pendingInterrupts.push_back(PendingInterrupt {
                    .interrupt_ = *executions[index].output_->interrupt_,
                    .node_ = executions[index].task_.nodeId_,
                    .order_ = executions[index].task_.order_,
                });
            }
            for (auto& task : routedNext)
                appendTask(pausedNext, std::move(task));

            nextTasks = std::move(pausedNext);
            result.step_ = nextStep;
            auto interruptedState = withInterruptMetadata(
                result.state_,
                pendingInterrupts,
                nextStep);
            if (!interruptedState.isOk()) {
                return failRun(interruptedState.status(), nextStep, executions[interruptIndexes.front()].task_.nodeId_);
            }
            result.state_ = std::move(*interruptedState);

            pendingWrites.clear();
            if (auto stored = putCheckpoint(nextStep, nextTasks, std::move(writes), {}, "interrupt", true); !stored.isOk()) {
                (void)emit(RuntimeEventType::RunFailed, nextStep, executions[interruptIndexes.front()].task_.nodeId_, {}, stored.status().toString());
                return stored.status();
            }

            for (const auto index : interruptIndexes) {
                auto event = RuntimeEvent::create(RuntimeEventType::InterruptRequested);
                event.step_ = nextStep;
                event.node_ = executions[index].task_.nodeId_;
                event.name_ = executions[index].output_->interrupt_->id_;
                event.payload_ = {
                    { "id", executions[index].output_->interrupt_->id_ },
                    { "node", executions[index].task_.nodeId_ },
                    { "step", nextStep },
                    { "value", executions[index].output_->interrupt_->value_ },
                };
                if (!executions[index].output_->interrupt_->resumeValues_.empty())
                    event.payload_["resume_values"] = executions[index].output_->interrupt_->resumeValues_;
                if (auto status = publish(std::move(event)); !status.isOk())
                    return failRun(status, nextStep, executions[index].task_.nodeId_, nextTasks);
            }

            result.status_ = RunStatus::Paused;
            return result;
        }

        for (const auto& write : writes) {
            if (auto routed = routeFromWrite(write, routedNext, nextStep); !routed.isOk()) {
                (void)emit(RuntimeEventType::NodeFailed, nextStep, write.nodeId_, {}, routed.status().toString());
                return failRun(routed.status(), nextStep, write.nodeId_, activeTasks);
            }
        }
        nextTasks = std::move(routedNext);
        result.step_ = nextStep;
        pendingWrites.clear();

        const bool terminal = isTerminalNextTasks(nextTasks);
        if (terminal) {
            if (auto status = validateStateSchema(result.state_, graph_->schemas_.outputSchema_, "output"); !status.isOk())
                return failRun(status.status(), nextStep, failureNode, nextTasks);
            result.status_ = RunStatus::Completed;
            if (auto emitted = emit(
                    RuntimeEventType::RunCompleted,
                    result.step_,
                    {},
                    { { "state", result.state_.view() } });
                !emitted.isOk()) {
                return failRun(emitted.status(), nextStep, failureNode, nextTasks);
            }
        }
        if (auto stored = putCheckpoint(
                nextStep,
                nextTasks,
                std::move(writes),
                {},
                terminal ? "completion" : "step",
                terminal);
            !stored.isOk()) {
            (void)emit(RuntimeEventType::RunFailed, nextStep, failureNode, {}, stored.status().toString());
            return stored.status();
        }

        resumeValuesByTask.clear();
    }

    if (result.status_ != RunStatus::Completed) {
        result.status_ = RunStatus::Completed;
        if (auto status = validateStateSchema(result.state_, graph_->schemas_.outputSchema_, "output"); !status.isOk())
            return failRun(status.status(), result.step_, {});
        if (auto emitted = emit(
                RuntimeEventType::RunCompleted,
                result.step_,
                {},
                { { "state", result.state_.view() } });
            !emitted.isOk()) {
            return failRun(emitted.status(), result.step_, {});
        }
    }

    return result;
}

} // namespace detail
} // namespace lgc
