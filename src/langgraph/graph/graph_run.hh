#pragma once

#include "langgraph/graph/state_graph_types.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lgc {

class StateGraph;

namespace detail {

/// Owns the run-local super-step loop for an immutable compiled graph.
class GraphRun final {
public:
    using NextTasksAfter = std::function<Result<std::vector<CheckpointTask>>(
        const NodeId&,
        const State&,
        Runtime&)>;

    GraphRun(
        std::shared_ptr<const StateGraph> graph,
        NextTasksAfter nextTasksAfter,
        State state,
        std::vector<CheckpointTask> nextTasks,
        std::vector<CheckpointWrite> pendingWrites,
        StepId step,
        std::optional<std::string> parentCheckpointId,
        RunOptions options,
        bool writeInitialCheckpoint,
        bool allowParentCommand);

    [[nodiscard]] Result<RunResult> run();

private:
    struct NodeExecution {
        CheckpointTask task_;
        std::optional<NodeOutput> output_;
        Status status_ { Status::ok() };
    };

    [[nodiscard]] Status publish(RuntimeEvent event);
    [[nodiscard]] Result<void> emit(
        RuntimeEventType type,
        StepId step,
        std::string node,
        nlohmann::json payload = nlohmann::json::object(),
        std::string message = {});
    void annotateTasks(std::vector<CheckpointTask>& tasks, StepId taskStep);
    [[nodiscard]] Result<void> putCheckpoint(
        StepId step,
        std::vector<CheckpointTask> next,
        std::vector<CheckpointWrite> writes,
        std::vector<CheckpointWrite> pending = {},
        std::string source = "step",
        bool terminal = false,
        bool requireCheckpointEvent = true);
    [[nodiscard]] Result<RunResult> failRun(
        Status status,
        StepId failedStep,
        std::string node,
        std::vector<CheckpointTask> failureNext = {},
        std::vector<CheckpointWrite> failurePendingWrites = {});
    [[nodiscard]] NodeExecution executeTask(
        std::size_t index,
        CheckpointTask task,
        const State& inputState,
        StepId step);
    [[nodiscard]] std::vector<NodeExecution> dispatchExecutions(
        const std::vector<CheckpointTask>& activeTasks,
        const State& inputState,
        StepId step);
    [[nodiscard]] Result<std::vector<CheckpointTask>> commandTasksAfter(
        const NodeExecution& execution) const;
    [[nodiscard]] Result<CheckpointWrite> writeFromExecution(
        const NodeExecution& execution) const;
    [[nodiscard]] Result<void> applyWrite(
        const CheckpointWrite& write,
        StepId step);
    [[nodiscard]] Result<void> routeFromWrite(
        const CheckpointWrite& write,
        std::vector<CheckpointTask>& routedNext,
        StepId step);

private:
    std::shared_ptr<const StateGraph> graph_;
    NextTasksAfter nextTasksAfter_;
    State state_;
    std::vector<CheckpointTask> nextTasks_;
    std::vector<CheckpointWrite> pendingWrites_;
    StepId step_;
    std::optional<std::string> parentCheckpointId_;
    RunOptions options_;
    bool writeInitialCheckpoint_;
    bool allowParentCommand_;
    RunResult result_;
    std::mutex publishMutex_;
    std::map<std::size_t, nlohmann::json> resumeValuesByTask_;
};

} // namespace detail
} // namespace lgc
