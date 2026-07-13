#pragma once

#include "langgraph/graph/state_graph_types.hpp"
#include "langgraph/graph/stream.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lc {

class StateGraph;

/// Immutable executable graph.
///
/// A CompiledStateGraph can be shared across runs. Each invoke/resume/replay
/// call owns its run-local state, task queue, and stream projection state.
/// Node tasks may execute concurrently when RunOptions provides an executor,
/// but reducer merge and checkpoint writes happen at runtime-defined
/// super-step boundaries.
class CompiledStateGraph final {
public:
    CompiledStateGraph();

    /// Start a new run from input state.
    [[nodiscard]] Result<RunResult> invoke(
        const State& input,
        RunOptions options = {}) const;
    /// Start a new run and collect runtime events into RunResult::events_.
    [[nodiscard]] Result<RunResult> stream(
        const State& input,
        RunOptions options = {}) const;
    /// Start a new run and return an event stream that can be consumed while the
    /// graph is still executing.
    [[nodiscard]] Result<RunEventStream> streamEvents(
        const State& input,
        RunOptions options = {},
        RunStreamOptions streamOptions = {}) const;
    [[nodiscard]] Result<RunPartStream> streamProjected(
        const State& input,
        RunOptions options = {},
        RunProjectionOptions projectionOptions = {}) const;
    /// Resume from the latest checkpoint for threadId.
    [[nodiscard]] Result<RunResult> resume(
        std::string_view threadId,
        RunOptions options = {}) const;
    /// Resume and collect runtime events into RunResult::events_.
    [[nodiscard]] Result<RunResult> resumeStream(
        std::string_view threadId,
        RunOptions options = {}) const;
    /// Resume and return an event stream that can be consumed while the graph is
    /// still executing.
    [[nodiscard]] Result<RunEventStream> resumeEvents(
        std::string_view threadId,
        RunOptions options = {},
        RunStreamOptions streamOptions = {}) const;
    [[nodiscard]] Result<RunPartStream> resumeProjected(
        std::string_view threadId,
        RunOptions options = {},
        RunProjectionOptions projectionOptions = {}) const;
    /// Return the latest checkpoint as a LangGraph-style state snapshot.
    [[nodiscard]] Result<StateSnapshot> getState(
        std::string_view threadId,
        RunOptions options = {}) const;
    /// Return a specific checkpoint as a LangGraph-style state snapshot.
    [[nodiscard]] Result<StateSnapshot> getState(
        std::string_view threadId,
        std::string_view checkpointId,
        RunOptions options = {}) const;
    /// Return all known snapshots for a thread in checkpointer order.
    [[nodiscard]] Result<std::vector<StateSnapshot>> getStateHistory(
        std::string_view threadId,
        RunOptions options = {}) const;
    /// Replay execution from a specific checkpoint. Steps before that checkpoint
    /// are not re-executed.
    [[nodiscard]] Result<RunResult> replay(
        std::string_view threadId,
        std::string_view checkpointId,
        RunOptions options = {}) const;
    /// Create a new checkpoint by applying an external state update. When
    /// updateOptions.checkpointId_ is set, this forks from that historical
    /// checkpoint and makes the fork the latest checkpoint for the thread.
    [[nodiscard]] Result<StateSnapshot> updateState(
        std::string_view threadId,
        StateUpdate update,
        RunOptions options = {},
        StateUpdateOptions updateOptions = {}) const;

private:
    friend class StateGraph;

    explicit CompiledStateGraph(StateGraph graph);

    [[nodiscard]] Result<std::vector<CheckpointTask>> nextTasksAfter(
        const NodeId& node,
        const State& state,
        Runtime& context) const;
    [[nodiscard]] Result<RunResult> invokeSubgraph(
        const State& input,
        RunOptions options = {}) const;
    [[nodiscard]] Result<RunResult> runFrom(
        State state,
        std::vector<CheckpointTask> nextTasks,
        std::vector<CheckpointWrite> pendingWrites,
        StepId step,
        std::optional<std::string> parentCheckpointId,
        RunOptions options,
        bool writeInitialCheckpoint,
        bool allowParentCommand) const;

    std::shared_ptr<const StateGraph> graph_;
};

} // namespace lc
