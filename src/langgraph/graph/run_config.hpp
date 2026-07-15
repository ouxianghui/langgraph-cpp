#pragma once

#include "foundation/cancellation/cancellation_token.hpp"
#include "foundation/event/i_event_sink.hpp"
#include "foundation/event/runtime_event.hpp"
#include "foundation/executor/i_executor.hpp"
#include "foundation/resource/resource_limits.hpp"
#include "foundation/serialization/state_codec.hpp"
#include "foundation/status/result.hpp"
#include "langgraph/checkpoint/checkpointer.hpp"
#include "langgraph/core/ids.hpp"
#include "langgraph/runtime/runtime.hpp"
#include "langgraph/state/reducer.hpp"
#include "langgraph/state/state_update.hpp"
#include "langgraph/store/store.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lgc {

enum class CommandGraph {
    Current,
    Parent,
};

enum class Durability {
    /// Persist every committed super-step and task-level writes before applying them.
    Sync,
    /// Persist committed super-steps without task-level write checkpoints.
    Async,
    /// Persist only initial/terminal/pause/failure checkpoints for the run.
    Exit,
};

/// External command supplied to a resumed run.
struct Command {
    CommandGraph graph_ { CommandGraph::Current };
    StateUpdate update_ { StateUpdate::empty() };
    /// Resume value exposed to the interrupted node through Runtime.
    std::optional<nlohmann::json> resume_;
    std::vector<NodeId> goto_;

    [[nodiscard]] static Command resume(
        nlohmann::json value = nlohmann::json::object(),
        StateUpdate update = StateUpdate::empty());
    [[nodiscard]] static Command gotoNode(
        NodeId target,
        StateUpdate update = StateUpdate::empty());
    [[nodiscard]] static Command gotoNodes(
        std::vector<NodeId> targets,
        StateUpdate update = StateUpdate::empty());
    [[nodiscard]] static Command gotoParentNode(
        NodeId target,
        StateUpdate update = StateUpdate::empty());
    [[nodiscard]] static Command gotoParentNodes(
        std::vector<NodeId> targets,
        StateUpdate update = StateUpdate::empty());
};

enum class RunStatus {
    Completed,
    Paused,
    Failed,
    Cancelled,
    MaxStepsExceeded,
};

/// JSON-compatible LangChain/LangGraph RunnableConfig bridge.
///
/// This mirrors the public config fields used by Python LangGraph while keeping
/// execution-specific C++ services in RunOptions. Unknown JSON top-level fields
/// are preserved in configurable_, matching LangChain's ensure_config behavior.
struct RunnableConfig {
    std::vector<std::string> tags_;
    nlohmann::json metadata_ { nlohmann::json::object() };
    nlohmann::json callbacks_ = nullptr;
    std::optional<std::uint64_t> recursionLimit_;
    std::optional<std::size_t> maxConcurrency_;
    std::string runName_;
    std::string runId_;
    nlohmann::json configurable_ { nlohmann::json::object() };

    [[nodiscard]] static Result<RunnableConfig> fromJson(const nlohmann::json& value);
    [[nodiscard]] nlohmann::json toJson() const;
};

/// Per-run execution options. Leave fields empty to use runtime defaults.
struct RunOptions {
    /// Optional caller-supplied run id. The runtime creates one when empty.
    std::string runId_;
    /// Logical conversation/workflow id used for checkpoint persistence.
    std::string threadId_;
    /// Optional namespace under the thread used to isolate checkpoints for subgraphs.
    std::string checkpointNamespace_;
    Durability durability_ { Durability::Async };
    ReducerRegistry reducers_;
    ResourceLimits limits_ { ResourceLimits {}.maxSteps(100) };
    CancellationToken cancellationToken_ { CancellationToken::none() };
    std::shared_ptr<BaseCheckpointSaver> checkpointer_;
    std::shared_ptr<BaseStore> store_;
    std::shared_ptr<IEventSink> eventSink_;
    std::shared_ptr<IExecutor> executor_;
    /// Maximum graph node tasks to run concurrently within one super-step.
    /// Zero means no explicit cap beyond the selected executor/runtime backend.
    std::size_t maxConcurrency_ { 0 };
    RuntimeEventPublisher eventCallback_;
    std::set<RuntimeEventType> eventTypes_;
    std::optional<Command> command_;
    std::vector<std::string> tags_;
    nlohmann::json metadata_ { nlohmann::json::object() };
    nlohmann::json configurable_ { nlohmann::json::object() };
    nlohmann::json callbacks_ = nullptr;
    std::string runName_;
    /// Retain emitted events in RunResult::events_. Off for invoke()/resume() so a
    /// long run does not accumulate every event in memory; stream()/resumeStream()
    /// enable it. Event callbacks and sinks always fire regardless of this flag.
    bool collectEvents_ { false };

    [[nodiscard]] static RunOptions onlyEvents(std::set<RuntimeEventType> eventTypes);
    [[nodiscard]] static RunOptions onlyEvents(std::initializer_list<RuntimeEventType> eventTypes);
    [[nodiscard]] static RunOptions streamingDefaults();
    [[nodiscard]] static RunOptions debugEvents();
};

[[nodiscard]] Result<RunnableConfig> mergeRunnableConfigs(
    std::vector<RunnableConfig> configs);
[[nodiscard]] Result<RunnableConfig> patchRunnableConfig(
    RunnableConfig config,
    const nlohmann::json& patch);
[[nodiscard]] Result<RunOptions> applyRunnableConfig(
    RunOptions options,
    const RunnableConfig& config);

/// Run output for completed or paused executions. Failed runs are returned through Result status.
struct RunResult {
    State state_;
    std::string runId_;
    std::string threadId_;
    std::string checkpointNamespace_;
    RunStatus status_ { RunStatus::Completed };
    StepId step_ { 0 };
    std::vector<RuntimeEvent> events_;
    std::optional<Command> parentCommand_;
};

} // namespace lgc
