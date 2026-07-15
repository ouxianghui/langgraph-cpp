#pragma once

#include "foundation/json/json_schema.hpp"
#include "foundation/serialization/state_codec.hpp"
#include "foundation/status/result.hpp"
#include "langgraph/core/ids.hpp"
#include "langgraph/graph/run_config.hpp"
#include "langgraph/runtime/runtime.hpp"
#include "langgraph/state/state_update.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lgc {

using NodeHandler = std::function<Result<StateUpdate>(const State&, Runtime&)>;
struct NodeOutput;
using NodeOutputHandler = std::function<Result<NodeOutput>(const State&, Runtime&)>;
using RouterHandler = std::function<Result<NodeId>(const State&, Runtime&)>;
using MultiRouterHandler = std::function<Result<std::vector<NodeId>>(const State&, Runtime&)>;
struct Send;
using SendRouterHandler = std::function<Result<std::vector<Send>>(const State&, Runtime&)>;

/// Dynamic fan-out target with a branch-local state for the destination node.
struct Send {
    NodeId node_;
    State arg_;

    Send(NodeId node, State arg);
};

/// Request for the runtime to persist the current state and pause execution.
struct Interrupt {
    /// Stable application-level interrupt id used by callers to identify the pause reason.
    std::string id_;
    /// JSON value returned to the caller, for example an approval request or form data.
    nlohmann::json value_ { nlohmann::json::object() };
    /// Internal function-interrupt resume scratchpad used to replay prior answers.
    nlohmann::json resumeValues_ { nlohmann::json::object() };
};

enum class SubgraphPersistenceMode {
    /// Child runs use a per-parent-invocation checkpoint namespace.
    PerInvocation,
    /// Child runs reuse a stable namespace for the parent thread and subgraph node.
    PerThread,
    /// Child runs do not inherit the parent checkpointer.
    Stateless,
};

/// Node result that can update state, interrupt execution, command routing, or combine them atomically.
struct NodeOutput {
    StateUpdate update_ { StateUpdate::empty() };
    std::optional<Interrupt> interrupt_;
    std::optional<Command> command_;

    [[nodiscard]] static NodeOutput update(StateUpdate update);
    [[nodiscard]] static NodeOutput command(Command command);
    [[nodiscard]] static NodeOutput interrupt(
        Interrupt interrupt,
        StateUpdate update = StateUpdate::empty());
};

using NodeErrorHandler = std::function<Result<NodeOutput>(
    const Status& status,
    const State& state,
    Runtime& context)>;

struct NodeRetryPolicy {
    /// Total attempts, including the first try.
    std::size_t maxAttempts_ { 1 };
    std::chrono::milliseconds initialInterval_ { 0 };
    double backoffFactor_ { 2.0 };
};

struct NodeOptions {
    NodeRetryPolicy retry_;
    /// Best-effort post-call deadline check for synchronous node handlers.
    std::optional<std::chrono::steady_clock::duration> timeout_;
    NodeErrorHandler errorHandler_;
};

struct StateSchemaOptions {
    std::optional<JsonSchema> inputSchema_;
    std::optional<JsonSchema> stateSchema_;
    std::optional<JsonSchema> outputSchema_;
};

struct StateSnapshot {
    State values_;
    std::vector<NodeId> next_;
    std::vector<CheckpointTask> tasks_;
    std::vector<CheckpointWrite> writes_;
    std::vector<CheckpointWrite> pendingWrites_;
    std::string threadId_;
    std::string checkpointNamespace_;
    std::string checkpointId_;
    std::optional<std::string> parentCheckpointId_;
    StepId step_ { 0 };
    std::chrono::system_clock::time_point createdAt_;
};

struct StateUpdateOptions {
    /// Empty means update the latest checkpoint for the thread.
    std::string checkpointId_;
    /// When set, route as if this node produced the update.
    std::optional<NodeId> asNode_;
};

struct SubgraphOptions {
    RunOptions options_;
    bool inheritThreadId_ { true };
    std::string threadIdSuffix_;
    SubgraphPersistenceMode persistence_ { SubgraphPersistenceMode::PerInvocation };
    std::string checkpointNamespace_;
};

} // namespace lgc
