#include "langgraph/graph/state_graph.hpp"

#include "langgraph/graph/state_graph_common.hh"
#include "langgraph/graph/graph_run.hh"

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
    return detail::GraphRun(
        graph_,
        [this](const NodeId& node, const State& routeState, Runtime& context) {
            return nextTasksAfter(node, routeState, context);
        },
        std::move(state),
        std::move(nextTasks),
        std::move(pendingWrites),
        step,
        std::move(parentCheckpointId),
        std::move(options),
        writeInitialCheckpoint,
        allowParentCommand)
        .run();
}


} // namespace lgc
