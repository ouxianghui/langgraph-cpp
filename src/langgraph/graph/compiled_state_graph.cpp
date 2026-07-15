#include "langgraph/graph/state_graph.hpp"

#include "langgraph/graph/state_graph_common.hh"

#include "foundation/id/id_generator.hpp"
#include "langgraph/graph/graph_namespace.hh"
#include "langgraph/graph/stream_projection.hh"
#include "langgraph/graph/stream_state.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <future>
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

struct NodeExecution {
    CheckpointTask task_;
    std::optional<NodeOutput> output_;
    Status status_ { Status::ok() };
};

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

[[nodiscard]] std::vector<CheckpointTask> nextTasksFromTargets(const std::vector<NodeId>& targets)
{
    std::vector<CheckpointTask> tasks;
    tasks.reserve(targets.size());
    for (const auto& target : targets)
        appendPlainTask(tasks, target);
    return tasks;
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

[[nodiscard]] std::vector<CheckpointTask> nextTasksFromCheckpoint(const Checkpoint& checkpoint)
{
    if (!checkpoint.nextTasks_.empty())
        return checkpoint.nextTasks_;
    return nextTasksFromTargets(checkpoint.nextNodes_);
}

[[nodiscard]] bool isTerminalNextTasks(const std::vector<CheckpointTask>& nextTasks)
{
    return std::ranges::all_of(nextTasks, [](const CheckpointTask& task) {
        return task.nodeId_.empty() || task.nodeId_ == END;
    });
}

[[nodiscard]] StateSnapshot snapshotFromCheckpoint(const Checkpoint& checkpoint)
{
    return StateSnapshot {
        .values_ = checkpoint.state_,
        .next_ = checkpoint.nextNodes_,
        .tasks_ = nextTasksFromCheckpoint(checkpoint),
        .writes_ = checkpoint.writes_,
        .pendingWrites_ = checkpoint.pendingWrites_,
        .threadId_ = checkpoint.threadId_,
        .checkpointNamespace_ = checkpoint.checkpointNamespace_,
        .checkpointId_ = checkpoint.checkpointId_,
        .parentCheckpointId_ = checkpoint.parentCheckpointId_,
        .step_ = checkpoint.step_,
        .createdAt_ = checkpoint.createdAt_,
    };
}

[[nodiscard]] StepId replayStartStep(const Checkpoint& checkpoint)
{
    if (!checkpoint.pendingWrites_.empty()
        && !isTerminalNextTasks(nextTasksFromCheckpoint(checkpoint))
        && checkpoint.step_ > 0U) {
        return checkpoint.step_ - 1U;
    }
    return checkpoint.step_;
}

[[nodiscard]] Result<void> requireRunCheckpointer(const RunOptions& options, std::string_view action)
{
    if (!options.checkpointer_) {
        std::string message(action);
        message.append(" requires a checkpointer");
        return Status::invalidArgument(std::move(message));
    }
    return okResult();
}

[[nodiscard]] Result<bool> stateHasInterruptMetadata(const State& state)
{
    return state.view().contains(std::string(kInterruptStateKey));
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

CompiledStateGraph::CompiledStateGraph()
    : graph_(std::make_shared<StateGraph>())
{
}

CompiledStateGraph::CompiledStateGraph(StateGraph graph)
    : graph_(std::make_shared<StateGraph>(std::move(graph)))
{
}

Result<std::vector<CheckpointTask>> CompiledStateGraph::nextTasksAfter(
    const NodeId& node,
    const State& state,
    Runtime& context) const
{
    const auto router = graph_->routers_.find(node);
    if (router != graph_->routers_.end()) {
        auto routed = router->second(state, context);
        if (!routed.isOk())
            return routed.status();

        const auto destinations = graph_->routerDestinations_.find(node);
        std::vector<CheckpointTask> tasks;
        tasks.reserve(routed->size());
        for (auto& target : *routed) {
            if (!graph_->hasNodeOrEnd(target.node_))
                return Status::failedPrecondition("router returned unknown node: " + target.node_);
            if (destinations != graph_->routerDestinations_.end() && !destinations->second.empty()) {
                if (!destinations->second.contains(target.node_))
                    return Status::failedPrecondition("router returned a node outside declared destinations: " + target.node_);
            }
            if (target.arg_.has_value()) {
                if (target.node_ == END)
                    return Status::invalidArgument("Send target cannot be END");
                appendSendTask(tasks, CheckpointTask {
                    .nodeId_ = std::move(target.node_),
                    .state_ = std::move(target.arg_),
                });
            } else {
                appendPlainTask(tasks, std::move(target.node_));
            }
        }
        return tasks;
    }

    const auto edges = graph_->edges_.find(node);
    if (edges == graph_->edges_.end() || edges->second.empty())
        return std::vector<CheckpointTask> {};
    return nextTasksFromTargets(edges->second);
}

Result<RunResult> CompiledStateGraph::invoke(const State& input, RunOptions options) const
{
    const auto start = graph_->edges_.find(std::string(START));
    auto nextTasks = start == graph_->edges_.end()
        ? std::vector<CheckpointTask> {}
        : nextTasksFromTargets(start->second);
    return runFrom(
        input,
        std::move(nextTasks),
        {},
        0,
        std::nullopt,
        std::move(options),
        true,
        false);
}

Result<RunResult> CompiledStateGraph::stream(const State& input, RunOptions options) const
{
    options.collectEvents_ = true;
    return invoke(input, std::move(options));
}

Result<RunEventStream> CompiledStateGraph::streamEvents(
    const State& input,
    RunOptions options,
    RunStreamOptions streamOptions) const
{
    if (streamOptions.capacity_ == 0U)
        return Status::invalidArgument("run event stream capacity must be greater than zero");

    auto state = std::make_shared<RunEventStreamState>(streamOptions.capacity_);
    auto sender = state->channel_.sender();
    auto callback = std::move(options.eventCallback_);
    options.collectEvents_ = false;
    options.eventCallback_ = [sender, callback = std::move(callback)](RuntimeEvent event) mutable -> Status {
        if (callback) {
            if (auto status = callback(event); !status.isOk())
                return status;
        }
        return sender.send(std::move(event));
    };

    try {
        state->worker_ = std::thread(
            [compiled = *this, input, options = std::move(options), state, sender]() mutable {
                try {
                    auto result = compiled.invoke(input, std::move(options));
                    state->resultPromise_.set_value(std::move(result));
                } catch (const std::exception& error) {
                    state->resultPromise_.set_value(exceptionStatus("run event stream worker", error));
                } catch (...) {
                    state->resultPromise_.set_value(unknownExceptionStatus("run event stream worker"));
                }
                sender.close();
            });
    } catch (const std::exception& error) {
        state->close();
        return exceptionStatus("run event stream launch", error);
    } catch (...) {
        state->close();
        return unknownExceptionStatus("run event stream launch");
    }

    return RunEventStream(std::move(state));
}

Result<RunPartStream> CompiledStateGraph::streamProjected(
    const State& input,
    RunOptions options,
    RunProjectionOptions projectionOptions) const
{
    if (projectionOptions.capacity_ == 0U)
        return Status::invalidArgument("run part stream capacity must be greater than zero");

    auto state = std::make_shared<RunPartStreamState>(projectionOptions.capacity_);
    auto sender = state->channel_.sender();
    auto callback = std::move(options.eventCallback_);
    if (projectionOptions.modes_.empty())
        projectionOptions.modes_.push_back(StreamMode::Events);
    const auto rootNamespace = options.checkpointNamespace_;

    options.collectEvents_ = false;
    options.eventCallback_ = [
                                 sender,
                                 callback = std::move(callback),
                                 projectionOptions = std::move(projectionOptions),
                                 rootNamespace
                             ](RuntimeEvent event) mutable -> Status {
        if (callback) {
            if (auto status = callback(event); !status.isOk())
                return status;
        }
        for (auto& part : detail::projectEvent(event, projectionOptions, rootNamespace)) {
            if (auto status = sender.send(std::move(part)); !status.isOk())
                return status;
        }
        return Status::ok();
    };

    try {
        state->worker_ = std::thread(
            [compiled = *this, input, options = std::move(options), state, sender]() mutable {
                try {
                    auto result = compiled.invoke(input, std::move(options));
                    state->resultPromise_.set_value(std::move(result));
                } catch (const std::exception& error) {
                    state->resultPromise_.set_value(exceptionStatus("run part stream worker", error));
                } catch (...) {
                    state->resultPromise_.set_value(unknownExceptionStatus("run part stream worker"));
                }
                sender.close();
            });
    } catch (const std::exception& error) {
        state->close();
        return exceptionStatus("run part stream launch", error);
    } catch (...) {
        state->close();
        return unknownExceptionStatus("run part stream launch");
    }

    return RunPartStream(std::move(state));
}

Result<RunResult> CompiledStateGraph::resume(std::string_view threadId, RunOptions options) const
{
    if (threadId.empty())
        return Status::invalidArgument("resume thread_id cannot be empty");
    if (!options.threadId_.empty() && options.threadId_ != threadId)
        return Status::invalidArgument("resume thread_id conflicts with run options thread_id");
    if (!options.checkpointer_)
        return Status::invalidArgument("resume requires a checkpointer");

    auto latest = options.checkpointer_->getTuple(
        CheckpointQuery::latest(std::string(threadId), options.checkpointNamespace_));
    if (!latest.isOk())
        return latest.status();
    if (!latest->has_value())
        return Status::notFound("checkpoint not found for thread");

    const auto& latestRecord = **latest;
    const auto& latestCheckpoint = latestRecord.checkpoint_;
    auto hasInterrupt = stateHasInterruptMetadata(latestCheckpoint.state_);
    if (!hasInterrupt.isOk())
        return hasInterrupt.status();
    auto latestNextTasks = nextTasksFromCheckpoint(latestCheckpoint);
    if (*hasInterrupt && !isTerminalNextTasks(latestNextTasks) && !options.command_.has_value())
        return Status::failedPrecondition("resume of interrupted thread requires Command::resume");

    options.threadId_ = std::string(threadId);
    if (options.checkpointNamespace_.empty())
        options.checkpointNamespace_ = latestCheckpoint.checkpointNamespace_;
    if (options.command_.has_value() && !options.command_->resume_.has_value())
        return Status::invalidArgument("unsupported resume command");
    return runFrom(
        latestCheckpoint.state_,
        std::move(latestNextTasks),
        latestRecord.pendingWrites_,
        replayStartStep(latestCheckpoint),
        latestCheckpoint.checkpointId_,
        std::move(options),
        false,
        false);
}

Result<RunResult> CompiledStateGraph::resumeStream(std::string_view threadId, RunOptions options) const
{
    options.collectEvents_ = true;
    return resume(threadId, std::move(options));
}

Result<RunEventStream> CompiledStateGraph::resumeEvents(
    std::string_view threadId,
    RunOptions options,
    RunStreamOptions streamOptions) const
{
    if (threadId.empty())
        return Status::invalidArgument("resume thread_id cannot be empty");
    if (streamOptions.capacity_ == 0U)
        return Status::invalidArgument("run event stream capacity must be greater than zero");

    auto state = std::make_shared<RunEventStreamState>(streamOptions.capacity_);
    auto sender = state->channel_.sender();
    auto callback = std::move(options.eventCallback_);
    options.collectEvents_ = false;
    options.eventCallback_ = [sender, callback = std::move(callback)](RuntimeEvent event) mutable -> Status {
        if (callback) {
            if (auto status = callback(event); !status.isOk())
                return status;
        }
        return sender.send(std::move(event));
    };

    try {
        state->worker_ = std::thread(
            [compiled = *this, threadId = std::string(threadId), options = std::move(options), state, sender]() mutable {
                try {
                    auto result = compiled.resume(threadId, std::move(options));
                    state->resultPromise_.set_value(std::move(result));
                } catch (const std::exception& error) {
                    state->resultPromise_.set_value(exceptionStatus("run event stream worker", error));
                } catch (...) {
                    state->resultPromise_.set_value(unknownExceptionStatus("run event stream worker"));
                }
                sender.close();
            });
    } catch (const std::exception& error) {
        state->close();
        return exceptionStatus("run event stream launch", error);
    } catch (...) {
        state->close();
        return unknownExceptionStatus("run event stream launch");
    }

    return RunEventStream(std::move(state));
}

Result<RunPartStream> CompiledStateGraph::resumeProjected(
    std::string_view threadId,
    RunOptions options,
    RunProjectionOptions projectionOptions) const
{
    if (threadId.empty())
        return Status::invalidArgument("resume thread_id cannot be empty");
    if (projectionOptions.capacity_ == 0U)
        return Status::invalidArgument("run part stream capacity must be greater than zero");

    auto state = std::make_shared<RunPartStreamState>(projectionOptions.capacity_);
    auto sender = state->channel_.sender();
    auto callback = std::move(options.eventCallback_);
    if (projectionOptions.modes_.empty())
        projectionOptions.modes_.push_back(StreamMode::Events);
    const auto rootNamespace = options.checkpointNamespace_;

    options.collectEvents_ = false;
    options.eventCallback_ = [
                                 sender,
                                 callback = std::move(callback),
                                 projectionOptions = std::move(projectionOptions),
                                 rootNamespace
                             ](RuntimeEvent event) mutable -> Status {
        if (callback) {
            if (auto status = callback(event); !status.isOk())
                return status;
        }
        for (auto& part : detail::projectEvent(event, projectionOptions, rootNamespace)) {
            if (auto status = sender.send(std::move(part)); !status.isOk())
                return status;
        }
        return Status::ok();
    };

    try {
        state->worker_ = std::thread(
            [compiled = *this, threadId = std::string(threadId), options = std::move(options), state, sender]() mutable {
                try {
                    auto result = compiled.resume(threadId, std::move(options));
                    state->resultPromise_.set_value(std::move(result));
                } catch (const std::exception& error) {
                    state->resultPromise_.set_value(exceptionStatus("run part stream worker", error));
                } catch (...) {
                    state->resultPromise_.set_value(unknownExceptionStatus("run part stream worker"));
                }
                sender.close();
            });
    } catch (const std::exception& error) {
        state->close();
        return exceptionStatus("run part stream launch", error);
    } catch (...) {
        state->close();
        return unknownExceptionStatus("run part stream launch");
    }

    return RunPartStream(std::move(state));
}

Result<StateSnapshot> CompiledStateGraph::getState(
    std::string_view threadId,
    RunOptions options) const
{
    if (threadId.empty())
        return Status::invalidArgument("getState thread_id cannot be empty");
    if (auto status = requireRunCheckpointer(options, "getState"); !status.isOk())
        return status.status();

    auto record = options.checkpointer_->getTuple(
        CheckpointQuery::latest(std::string(threadId), options.checkpointNamespace_));
    if (!record.isOk())
        return record.status();
    if (!record->has_value())
        return Status::notFound("checkpoint not found for thread");
    return snapshotFromCheckpoint((*record)->checkpoint_);
}

Result<StateSnapshot> CompiledStateGraph::getState(
    std::string_view threadId,
    std::string_view checkpointId,
    RunOptions options) const
{
    if (threadId.empty())
        return Status::invalidArgument("getState thread_id cannot be empty");
    if (checkpointId.empty())
        return Status::invalidArgument("getState checkpoint_id cannot be empty");
    if (auto status = requireRunCheckpointer(options, "getState"); !status.isOk())
        return status.status();

    auto record = options.checkpointer_->getTuple(
        CheckpointQuery::at(std::string(threadId), std::string(checkpointId), options.checkpointNamespace_));
    if (!record.isOk())
        return record.status();
    if (!record->has_value())
        return Status::notFound("checkpoint not found");
    return snapshotFromCheckpoint((*record)->checkpoint_);
}

Result<std::vector<StateSnapshot>> CompiledStateGraph::getStateHistory(
    std::string_view threadId,
    RunOptions options) const
{
    if (threadId.empty())
        return Status::invalidArgument("getStateHistory thread_id cannot be empty");
    if (auto status = requireRunCheckpointer(options, "getStateHistory"); !status.isOk())
        return status.status();

    auto records = options.checkpointer_->list(CheckpointListOptions {
        .threadId_ = std::string(threadId),
        .checkpointNamespace_ = options.checkpointNamespace_,
        .order_ = CheckpointListOrder::NewestFirst,
    });
    if (!records.isOk())
        return records.status();

    std::vector<StateSnapshot> snapshots;
    snapshots.reserve(records->size());
    for (const auto& record : *records)
        snapshots.push_back(snapshotFromCheckpoint(record.checkpoint_));
    return snapshots;
}

Result<RunResult> CompiledStateGraph::replay(
    std::string_view threadId,
    std::string_view checkpointId,
    RunOptions options) const
{
    if (threadId.empty())
        return Status::invalidArgument("replay thread_id cannot be empty");
    if (checkpointId.empty())
        return Status::invalidArgument("replay checkpoint_id cannot be empty");
    if (!options.threadId_.empty() && options.threadId_ != threadId)
        return Status::invalidArgument("replay thread_id conflicts with run options thread_id");
    if (auto status = requireRunCheckpointer(options, "replay"); !status.isOk())
        return status.status();

    auto record = options.checkpointer_->getTuple(
        CheckpointQuery::at(std::string(threadId), std::string(checkpointId), options.checkpointNamespace_));
    if (!record.isOk())
        return record.status();
    if (!record->has_value())
        return Status::notFound("checkpoint not found");

    const auto& checkpoint = (*record)->checkpoint_;
    options.threadId_ = std::string(threadId);
    if (options.checkpointNamespace_.empty())
        options.checkpointNamespace_ = checkpoint.checkpointNamespace_;

    auto hasInterrupt = stateHasInterruptMetadata(checkpoint.state_);
    if (!hasInterrupt.isOk())
        return hasInterrupt.status();
    auto nextTasks = nextTasksFromCheckpoint(checkpoint);
    if (*hasInterrupt && !isTerminalNextTasks(nextTasks) && !options.command_.has_value()) {
        auto runId = options.runId_.empty() ? generatedId("run") : Result<std::string>(options.runId_);
        if (!runId.isOk())
            return runId.status();
        return RunResult {
            .state_ = checkpoint.state_,
            .runId_ = std::move(*runId),
            .threadId_ = std::string(threadId),
            .checkpointNamespace_ = checkpoint.checkpointNamespace_,
            .status_ = RunStatus::Paused,
            .step_ = checkpoint.step_,
        };
    }

    StepId startStep = replayStartStep(checkpoint);
    auto latest = options.checkpointer_->getTuple(
        CheckpointQuery::latest(std::string(threadId), options.checkpointNamespace_));
    if (!latest.isOk())
        return latest.status();
    if (latest->has_value() && (*latest)->checkpoint_.step_ >= startStep)
        startStep = (*latest)->checkpoint_.step_;

    return runFrom(
        checkpoint.state_,
        std::move(nextTasks),
        (*record)->pendingWrites_,
        startStep,
        checkpoint.checkpointId_,
        std::move(options),
        false,
        false);
}

Result<RunResult> CompiledStateGraph::invokeSubgraph(const State& input, RunOptions options) const
{
    const auto start = graph_->edges_.find(std::string(START));
    auto nextTasks = start == graph_->edges_.end()
        ? std::vector<CheckpointTask> {}
        : nextTasksFromTargets(start->second);
    return runFrom(
        input,
        std::move(nextTasks),
        {},
        0,
        std::nullopt,
        std::move(options),
        true,
        true);
}

Result<StateSnapshot> CompiledStateGraph::updateState(
    std::string_view threadId,
    StateUpdate update,
    RunOptions options,
    StateUpdateOptions updateOptions) const
{
    if (threadId.empty())
        return Status::invalidArgument("updateState thread_id cannot be empty");
    if (!options.threadId_.empty() && options.threadId_ != threadId)
        return Status::invalidArgument("updateState thread_id conflicts with run options thread_id");
    if (auto status = requireRunCheckpointer(options, "updateState"); !status.isOk())
        return status.status();

    Result<std::optional<CheckpointTuple>> base = updateOptions.checkpointId_.empty()
        ? options.checkpointer_->getTuple(
              CheckpointQuery::latest(std::string(threadId), options.checkpointNamespace_))
        : options.checkpointer_->getTuple(
              CheckpointQuery::at(std::string(threadId), updateOptions.checkpointId_, options.checkpointNamespace_));
    if (!base.isOk())
        return base.status();
    if (!base->has_value())
        return Status::notFound("checkpoint not found");
    const auto& baseCheckpoint = (*base)->checkpoint_;
    const std::string checkpointNamespace = options.checkpointNamespace_.empty()
        ? baseCheckpoint.checkpointNamespace_
        : options.checkpointNamespace_;

    StepId updateStep = baseCheckpoint.step_ + 1U;
    auto latestForStep = options.checkpointer_->getTuple(
        CheckpointQuery::latest(std::string(threadId), checkpointNamespace));
    if (!latestForStep.isOk())
        return latestForStep.status();
    if (latestForStep->has_value() && (*latestForStep)->checkpoint_.step_ >= updateStep)
        updateStep = (*latestForStep)->checkpoint_.step_ + 1U;

    auto baseState = withoutRunErrorMetadata(baseCheckpoint.state_);
    if (!baseState.isOk())
        return baseState.status();
    auto updatedState = applyStateUpdate(*baseState, update, options.reducers_);
    if (!updatedState.isOk())
        return updatedState.status();
    if (auto status = validateStateSchema(*updatedState, graph_->schemas_.stateSchema_, "state"); !status.isOk())
        return status.status();

    auto nextTasks = nextTasksFromCheckpoint(baseCheckpoint);
    if (updateOptions.asNode_.has_value()) {
        if (auto status = graph_detail::validateUserNodeId(*updateOptions.asNode_); !status.isOk())
            return status.status();
        if (!graph_->nodes_.contains(*updateOptions.asNode_))
            return Status::failedPrecondition("updateState asNode is unknown: " + *updateOptions.asNode_);

        auto updateRunId = generatedId("update-run");
        if (!updateRunId.isOk())
            return updateRunId.status();
        Runtime context(Runtime::Options {
            .runId_ = std::move(*updateRunId),
            .threadId_ = std::string(threadId),
            .checkpointNamespace_ = checkpointNamespace,
            .step_ = updateStep,
            .nodeId_ = *updateOptions.asNode_,
            .cancellationToken_ = options.cancellationToken_,
            .store_ = options.store_,
            .checkpointer_ = options.checkpointer_,
        });
        auto routed = nextTasksAfter(*updateOptions.asNode_, *updatedState, context);
        if (!routed.isOk())
            return routed.status();
        nextTasks = std::move(*routed);
    }

    auto checkpointId = generatedId("update");
    if (!checkpointId.isOk())
        return checkpointId.status();

    auto updateState = update.toState();
    if (!updateState.isOk())
        return updateState.status();

    Checkpoint checkpoint {
        .threadId_ = std::string(threadId),
        .checkpointId_ = *checkpointId,
        .checkpointNamespace_ = checkpointNamespace,
        .parentCheckpointId_ = baseCheckpoint.checkpointId_,
        .step_ = updateStep,
        .state_ = *updatedState,
        .nextNodes_ = nodeIdsFromTasks(nextTasks),
        .nextTasks_ = std::move(nextTasks),
        .writes_ = {
            CheckpointWrite {
                .nodeId_ = updateOptions.asNode_.value_or("__update__"),
                .update_ = *updateState,
            },
        },
        .createdAt_ = std::chrono::system_clock::now(),
    };

    if (auto stored = options.checkpointer_->put(checkpoint); !stored.isOk())
        return stored.status();
    return snapshotFromCheckpoint(checkpoint);
}

Result<RunResult> CompiledStateGraph::runFrom(
    State state,
    std::vector<CheckpointTask> nextTasks,
    std::vector<CheckpointWrite> pendingWrites,
    StepId step,
    std::optional<std::string> parentCheckpointId,
    RunOptions options,
    bool writeInitialCheckpoint,
    bool allowParentCommand) const
{
    if (auto status = validateRunOptionsConfig(options); !status.isOk())
        return status;
    nextTasks = normalizeNextTasks(std::move(nextTasks));

    RunResult result {
        .state_ = std::move(state),
        .step_ = step,
    };

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

    std::mutex publishMutex;
    auto publish = [&](RuntimeEvent event) -> Status {
        std::lock_guard lock(publishMutex);
        if (event.runId_.empty())
            event.runId_ = result.runId_;
        if (event.threadId_.empty())
            event.threadId_ = result.threadId_;
        if (!result.checkpointNamespace_.empty() && event.payload_.is_object()) {
            if (!event.payload_.contains("ns"))
                event.payload_["ns"] = result.checkpointNamespace_;
            if (!event.payload_.contains("checkpoint_ns"))
                event.payload_["checkpoint_ns"] = result.checkpointNamespace_;
        }
        if (event.payload_.is_object()) {
            mergeEventTags(event.payload_, options.tags_);
            mergeEventMetadata(event.payload_, options.metadata_);
        }
        if (!options.eventTypes_.empty() && !options.eventTypes_.contains(event.type_))
            return Status::ok();
        if (auto status = ensureRuntimeEventIdentity(event); !status.isOk())
            return status;
        if (auto status = validateRuntimeEvent(event); !status.isOk())
            return status;
        if (options.collectEvents_)
            result.events_.push_back(event);
        if (options.eventCallback_) {
            if (auto status = options.eventCallback_(event); !status.isOk())
                return status;
        }
        if (options.eventSink_)
            return options.eventSink_->publish(std::move(event));
        return Status::ok();
    };

    auto emit = [&](RuntimeEventType type, StepId step, std::string node, nlohmann::json payload = nlohmann::json::object(), std::string message = {}) -> Result<void> {
        auto event = RuntimeEvent::create(type);
        event.step_ = step;
        event.node_ = std::move(node);
        event.payload_ = std::move(payload);
        event.message_ = std::move(message);
        if (auto status = publish(std::move(event)); !status.isOk())
            return status;
        return okResult();
    };

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
            .publisher_ = publish,
            .cancellationToken_ = options.cancellationToken_,
            .store_ = options.store_,
            .checkpointer_ = options.checkpointer_,
        });
        auto routed = nextTasksAfter(std::string(START), result.state_, context);
        if (!routed.isOk())
            return routed.status();
        nextTasks = normalizeNextTasks(std::move(*routed));
    }

    if (auto emitted = emit(RuntimeEventType::RunStarted, result.step_, {}); !emitted.isOk())
        return emitted.status();

    auto annotateTasks = [&](std::vector<CheckpointTask>& tasks, StepId taskStep) {
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            if (tasks[i].nodeId_.empty() || tasks[i].nodeId_ == END)
                continue;
            if (tasks[i].checkpointNamespace_.empty())
                tasks[i].checkpointNamespace_ = result.checkpointNamespace_;
            if (!tasks[i].order_.has_value())
                tasks[i].order_ = static_cast<std::uint64_t>(i);
            if (tasks[i].taskId_.empty()) {
                tasks[i].taskId_ = result.runId_;
                tasks[i].taskId_.append(":");
                tasks[i].taskId_.append(std::to_string(taskStep));
                tasks[i].taskId_.append(":");
                tasks[i].taskId_.append(std::to_string(i));
                tasks[i].taskId_.append(":");
                tasks[i].taskId_.append(tasks[i].nodeId_);
            }
        }
    };

    auto putCheckpoint = [&](
                             StepId step,
                             std::vector<CheckpointTask> next,
                             std::vector<CheckpointWrite> writes,
                             std::vector<CheckpointWrite> pending = {},
                             std::string source = "step",
                             bool terminal = false) -> Result<void> {
        next = normalizeNextTasks(std::move(next));
        annotateTasks(next, step);
        const bool shouldStore = options.durability_ != Durability::Exit
            || source == "initial"
            || terminal;

        Checkpoint checkpoint {
            .threadId_ = result.threadId_,
            .checkpointId_ = source == "task_writes"
                ? result.runId_ + "-step-" + std::to_string(step) + "-writes-" + std::to_string(pending.size())
                : result.runId_ + "-step-" + std::to_string(step),
            .checkpointNamespace_ = result.checkpointNamespace_,
            .parentCheckpointId_ = parentCheckpointId,
            .step_ = step,
            .state_ = result.state_,
            .nextNodes_ = nodeIdsFromTasks(next),
            .nextTasks_ = std::move(next),
            .writes_ = std::move(writes),
            .pendingWrites_ = std::move(pending),
            .metadata_ = {
                { "source", std::move(source) },
                { "durability", durabilityModeName(options.durability_) },
            },
            .createdAt_ = std::chrono::system_clock::now(),
        };

        if (options.checkpointer_ && shouldStore) {
            if (auto stored = options.checkpointer_->put(checkpoint); !stored.isOk())
                return stored.status();
            if (!checkpoint.pendingWrites_.empty()) {
                auto storedWrites = options.checkpointer_->putWrites(CheckpointWriteSet {
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
            parentCheckpointId = checkpoint.checkpointId_;
            if (auto emitted = emit(RuntimeEventType::CheckpointCreated, step, {}, checkpointPayload(checkpoint)); !emitted.isOk())
                return emitted.status();
        }
        return okResult();
    };

    auto failRun = [&](
                       Status status,
                       StepId failedStep,
                       std::string node,
                       std::vector<CheckpointTask> failureNext = {},
                       std::vector<CheckpointWrite> failurePendingWrites = {}) -> Result<RunResult> {
        const auto failedNode = node;
        result.status_ = runStatusFromStatus(status);
        auto failedState = withRunErrorMetadata(
            result.state_,
            status,
            node,
            failedStep,
            result.status_);
        if (!failedState.isOk())
            return failedState.status();
        result.state_ = std::move(*failedState);

        failureNext = normalizeNextTasks(std::move(failureNext));
        if (failureNext.empty() && !node.empty())
            failureNext.push_back(CheckpointTask { .nodeId_ = failedNode });
        if (auto stored = putCheckpoint(
                failedStep,
                std::move(failureNext),
                {},
                std::move(failurePendingWrites),
                "failure",
                true);
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
        return status;
    };

    if (writeInitialCheckpoint) {
        if (auto stored = putCheckpoint(step, nextTasks, {}, {}, "initial", false); !stored.isOk())
            return stored.status();
    }

    std::map<std::size_t, nlohmann::json> resumeValuesByTask;
    if (!writeInitialCheckpoint) {
        auto cleaned = withoutRunErrorMetadata(result.state_);
        if (!cleaned.isOk())
            return cleaned.status();
        result.state_ = std::move(*cleaned);
    }
    if (options.command_.has_value()) {
        if (!options.command_->resume_.has_value())
            return Status::invalidArgument("unsupported command");
        auto interrupted = interruptMetadataFromState(result.state_);
        if (!interrupted.isOk())
            return interrupted.status();
        if (interrupted->empty()) {
            if (nextTasks.size() != 1U)
                return Status::failedPrecondition("resume command requires a single pending node when no interrupt metadata exists");
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
                    if (matched.has_value())
                        return Status::failedPrecondition("resume command matches multiple pending tasks: " + metadata.node_);
                    matched = i;
                }
                if (!matched.has_value())
                    return Status::failedPrecondition("resume node is not pending: " + metadata.node_);
                auto value = resumeValueForInterrupt(*options.command_->resume_, metadata, interrupted->size());
                if (!value.isOk())
                    return value.status();
                resumeValuesByTask.emplace(*matched, std::move(*value));
            }
        }
        auto cleaned = withoutInterruptMetadata(result.state_);
        if (!cleaned.isOk())
            return cleaned.status();
        result.state_ = std::move(*cleaned);
        auto resumedState = applyStateUpdate(result.state_, options.command_->update_, options.reducers_);
        if (!resumedState.isOk())
            return resumedState.status();
        result.state_ = std::move(*resumedState);
    }
    if (auto status = validateStateSchema(result.state_, graph_->schemas_.stateSchema_, "state"); !status.isOk())
        return status.status();

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
                return emitted.status();
            }
        }

        auto executeTask = [&](std::size_t index, CheckpointTask task) -> NodeExecution {
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
            if (const auto found = resumeValuesByTask.find(index); found != resumeValuesByTask.end())
                nodeResumeValue = found->second;

            const auto& spec = nodeFound->second;
            const auto& nodeOptions = spec.options_;
            const std::size_t attempts = std::max<std::size_t>(1U, nodeOptions.retry_.maxAttempts_);
            auto delay = nodeOptions.retry_.initialInterval_;
            Status finalStatus = Status::unknown("node handler did not run");

            for (std::size_t attempt = 1; attempt <= attempts; ++attempt) {
                if (auto status = options.cancellationToken_.check(); !status.isOk()) {
                    return NodeExecution {
                        .task_ = std::move(task),
                        .status_ = status,
                    };
                }

                const auto started = std::chrono::steady_clock::now();
                std::optional<std::chrono::steady_clock::time_point> deadline;
                if (nodeOptions.timeout_.has_value())
                    deadline = started + *nodeOptions.timeout_;
                Runtime context(Runtime::Options {
                    .runId_ = result.runId_,
                    .threadId_ = result.threadId_,
                    .checkpointNamespace_ = result.checkpointNamespace_,
                    .taskId_ = task.taskId_,
                    .step_ = nextStep,
                    .nodeId_ = task.nodeId_,
                    .publisher_ = publish,
                    .cancellationToken_ = options.cancellationToken_,
                    .resumeValue_ = nodeResumeValue,
                    .store_ = options.store_,
                    .checkpointer_ = options.checkpointer_,
                    .attempt_ = attempt,
                    .firstAttemptTime_ = started,
                    .deadline_ = deadline,
                });

                try {
                    auto output = spec.handler_(nodeInput, context);
                    if (nodeOptions.timeout_.has_value()
                        && std::chrono::steady_clock::now() - started > *nodeOptions.timeout_) {
                        finalStatus = Status::deadlineExceeded("node handler exceeded timeout");
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
                    finalStatus = exceptionStatus("node handler", error);
                } catch (...) {
                    finalStatus = unknownExceptionStatus("node handler");
                }

                if (attempt == attempts)
                    break;
                if (delay.count() > 0) {
                    std::this_thread::sleep_for(delay);
                    const auto scaled = static_cast<double>(delay.count()) * nodeOptions.retry_.backoffFactor_;
                    delay = std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(std::max(0.0, scaled)));
                }
            }

            if (nodeOptions.errorHandler_) {
                Runtime context(Runtime::Options {
                    .runId_ = result.runId_,
                    .threadId_ = result.threadId_,
                    .checkpointNamespace_ = result.checkpointNamespace_,
                    .taskId_ = task.taskId_,
                    .step_ = nextStep,
                    .nodeId_ = task.nodeId_,
                    .publisher_ = publish,
                    .cancellationToken_ = options.cancellationToken_,
                    .resumeValue_ = nodeResumeValue,
                    .store_ = options.store_,
                    .checkpointer_ = options.checkpointer_,
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
        };

        std::vector<NodeExecution> executions;
        executions.reserve(activeTasks.size());
        const std::size_t concurrency = options.maxConcurrency_ == 0U
            ? activeTasks.size()
            : std::min(options.maxConcurrency_, activeTasks.size());

        auto getExecution = [&](std::future<NodeExecution>& future, const CheckpointTask& task) -> NodeExecution {
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
                executions.push_back(executeTask(begin, activeTasks[begin]));
                continue;
            }

            if (options.executor_) {
                std::vector<std::optional<std::future<NodeExecution>>> futures(end - begin);
                std::vector<std::optional<NodeExecution>> immediate(end - begin);
                for (std::size_t i = begin; i < end; ++i) {
                    const std::size_t local = i - begin;
                    auto promise = std::make_shared<std::promise<NodeExecution>>();
                    futures[local] = promise->get_future();
                    auto status = options.executor_->execute(
                        [&, i, task = activeTasks[i], promise]() mutable {
                            promise->set_value(executeTask(i, std::move(task)));
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
                continue;
            }

            std::vector<std::future<NodeExecution>> futures;
            futures.reserve(end - begin);
            try {
                for (std::size_t i = begin; i < end; ++i)
                    futures.push_back(std::async(std::launch::async, executeTask, i, activeTasks[i]));
            } catch (const std::exception& error) {
                for (auto& future : futures) {
                    try {
                        (void)future.get();
                    } catch (...) {
                    }
                }
                return failRun(exceptionStatus("parallel node launch", error), nextStep, failureNode, activeTasks, pendingWrites);
            } catch (...) {
                for (auto& future : futures) {
                    try {
                        (void)future.get();
                    } catch (...) {
                    }
                }
                return failRun(unknownExceptionStatus("parallel node launch"), nextStep, failureNode, activeTasks, pendingWrites);
            }

            for (std::size_t local = 0; local < futures.size(); ++local) {
                const std::size_t i = begin + local;
                try {
                    executions.push_back(futures[local].get());
                } catch (const std::exception& error) {
                    executions.push_back(NodeExecution {
                        .task_ = activeTasks[i],
                        .status_ = exceptionStatus("parallel node task", error),
                    });
                } catch (...) {
                    executions.push_back(NodeExecution {
                        .task_ = activeTasks[i],
                        .status_ = unknownExceptionStatus("parallel node task"),
                    });
                }
            }
        }

        auto commandTasksAfter = [&](const NodeExecution& execution) -> Result<std::vector<CheckpointTask>> {
            std::vector<CheckpointTask> tasks;
            const auto& command = execution.output_->command_;
            if (!command.has_value())
                return tasks;
            if (command->graph_ == CommandGraph::Parent)
                return tasks;

            const auto destinations = graph_->commandDestinations_.find(execution.task_.nodeId_);
            for (const auto& target : command->goto_) {
                if (!graph_->hasNodeOrEnd(target))
                    return Status::failedPrecondition("command returned unknown node: " + target);
                if (destinations != graph_->commandDestinations_.end() && !destinations->second.empty()) {
                    if (!destinations->second.contains(target))
                        return Status::failedPrecondition("command returned a node outside declared destinations: " + target);
                }
                appendPlainTask(tasks, target);
            }
            return tasks;
        };

        auto writeFromExecution = [&](const NodeExecution& execution) -> Result<CheckpointWrite> {
            auto updateState = execution.output_->update_.toState();
            if (!updateState.isOk())
                return updateState.status();

            CheckpointWrite write {
                .taskId_ = execution.task_.taskId_,
                .nodeId_ = execution.task_.nodeId_,
                .checkpointNamespace_ = result.checkpointNamespace_,
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
        };

        auto applyWrite = [&](const CheckpointWrite& write) -> Result<void> {
            auto update = StateUpdate::fromJsonValue(write.update_.view());
            if (!update.isOk())
                return update.status();
            auto merged = applyStateUpdate(result.state_, *update, options.reducers_);
            if (!merged.isOk())
                return merged.status();
            result.state_ = std::move(*merged);
            if (auto status = validateStateSchema(result.state_, graph_->schemas_.stateSchema_, "state"); !status.isOk())
                return status.status();

            if (auto emitted = emit(
                    RuntimeEventType::StateUpdated,
                    nextStep,
                    write.nodeId_,
                    [&] {
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
                        return payload;
                    }());
                !emitted.isOk()) {
                return emitted.status();
            }
            return okResult();
        };

        auto routeFromWrite = [&](const CheckpointWrite& write, std::vector<CheckpointTask>& routedNext) -> Result<void> {
            if (write.hasNextTasks_) {
                for (auto task : write.nextTasks_)
                    appendTask(routedNext, std::move(task));
                return okResult();
            }

            Runtime routeContext(Runtime::Options {
                .runId_ = result.runId_,
                .threadId_ = result.threadId_,
                .checkpointNamespace_ = result.checkpointNamespace_,
                .step_ = nextStep,
                .nodeId_ = write.nodeId_,
                .publisher_ = publish,
                .cancellationToken_ = options.cancellationToken_,
                .store_ = options.store_,
                .checkpointer_ = options.checkpointer_,
            });
            auto routed = nextTasksAfter(write.nodeId_, result.state_, routeContext);
            if (!routed.isOk())
                return routed.status();
            for (auto& task : *routed)
                appendTask(routedNext, std::move(task));
            return okResult();
        };

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
                        return emitted.status();
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
                    return emitted.status();
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

        std::stable_sort(
            writes.begin(),
            writes.end(),
            [](const CheckpointWrite& lhs, const CheckpointWrite& rhs) {
                if (!lhs.order_.has_value() || !rhs.order_.has_value())
                    return false;
                return *lhs.order_ < *rhs.order_;
            });

        for (const auto& write : writes) {
            if (auto applied = applyWrite(write); !applied.isOk()) {
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

            if (auto stored = putCheckpoint(nextStep, nextTasks, std::move(writes), {}, "parent_command", true); !stored.isOk()) {
                (void)emit(RuntimeEventType::RunFailed, nextStep, executions[*parentCommandIndex].task_.nodeId_, {}, stored.status().toString());
                return stored.status();
            }

            if (auto status = validateStateSchema(result.state_, graph_->schemas_.outputSchema_, "output"); !status.isOk())
                return status.status();
            result.status_ = RunStatus::Completed;
            if (auto emitted = emit(
                    RuntimeEventType::RunCompleted,
                    result.step_,
                    {},
                    { { "state", result.state_.view() }, { "parent_command", true } });
                !emitted.isOk()) {
                return emitted.status();
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
                if (auto routed = routeFromWrite(write, routedNext); !routed.isOk()) {
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
                    return status;
            }

            result.status_ = RunStatus::Paused;
            return result;
        }

        for (const auto& write : writes) {
            if (auto routed = routeFromWrite(write, routedNext); !routed.isOk()) {
                (void)emit(RuntimeEventType::NodeFailed, nextStep, write.nodeId_, {}, routed.status().toString());
                return failRun(routed.status(), nextStep, write.nodeId_, activeTasks);
            }
        }
        nextTasks = std::move(routedNext);
        result.step_ = nextStep;
        pendingWrites.clear();

        if (auto stored = putCheckpoint(nextStep, nextTasks, std::move(writes), {}, isTerminalNextTasks(nextTasks) ? "completion" : "step", isTerminalNextTasks(nextTasks)); !stored.isOk()) {
            (void)emit(RuntimeEventType::RunFailed, nextStep, failureNode, {}, stored.status().toString());
            return stored.status();
        }

        resumeValuesByTask.clear();
    }

    result.status_ = RunStatus::Completed;
    if (auto status = validateStateSchema(result.state_, graph_->schemas_.outputSchema_, "output"); !status.isOk())
        return status.status();
    if (auto emitted = emit(
            RuntimeEventType::RunCompleted,
            result.step_,
            {},
            { { "state", result.state_.view() } });
        !emitted.isOk()) {
        return emitted.status();
    }

    return result;
}

} // namespace lgc
