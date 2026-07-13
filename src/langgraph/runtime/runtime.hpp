#pragma once

#include "foundation/cancellation/cancellation_token.hpp"
#include "foundation/event/runtime_event.hpp"
#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"
#include "langgraph/checkpoint/checkpointer.hpp"
#include "langgraph/core/ids.hpp"
#include "langgraph/store/store.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lc {

using RuntimeEventPublisher = std::function<Status(RuntimeEvent)>;

/// Read-only execution metadata for the current node run.
struct ExecutionInfo {
    std::string checkpointId_;
    std::string checkpointNamespace_;
    std::string taskId_;
    std::string threadId_;
    std::string runId_;
    std::string nodeId_;
    StepId step_ { 0 };
    std::size_t nodeAttempt_ { 1 };
    std::optional<std::chrono::steady_clock::time_point> nodeFirstAttemptTime_;
};

/// Run-scoped cooperative drain signal.
///
/// This small shared state is protected by its own mutex because it can be
/// observed by node code while a graph run is being drained.
class RunControl final {
public:
    RunControl() = default;

    void requestDrain(std::string reason = "shutdown");
    [[nodiscard]] bool drainRequested() const;
    [[nodiscard]] std::optional<std::string> drainReason() const;

private:
    mutable std::mutex mutex_;
    std::optional<std::string> drainReason_;
};

/// Lightweight writer for custom runtime events emitted by node code.
class StreamWriter final {
public:
    struct Options {
        std::string runId_;
        std::string threadId_;
        std::string checkpointNamespace_;
        StepId step_ { 0 };
        std::string nodeId_;
        RuntimeEventPublisher publisher_;
    };

    StreamWriter() = default;
    explicit StreamWriter(Options options);

    [[nodiscard]] Status write(
        std::string name,
        nlohmann::json payload = nlohmann::json::object(),
        std::string message = {}) const;
    [[nodiscard]] Status publish(RuntimeEvent event) const;

private:
    std::string runId_;
    std::string threadId_;
    std::string checkpointNamespace_;
    StepId step_ { 0 };
    std::string nodeId_;
    RuntimeEventPublisher publisher_;
};

/// Per-node runtime object passed to handlers.
///
/// Runtime is scoped to one node attempt. Handlers must not retain references
/// to Runtime, State, or StreamWriter beyond the handler call unless they copy
/// the required values and provide their own lifetime/synchronization boundary.
class Runtime final {
public:
    struct Options {
        std::string runId_;
        std::string threadId_;
        std::string checkpointId_;
        std::string checkpointNamespace_;
        std::string taskId_;
        StepId step_ { 0 };
        std::string nodeId_;
        RuntimeEventPublisher publisher_;
        CancellationToken cancellationToken_ { CancellationToken::none() };
        std::optional<nlohmann::json> resumeValue_;
        nlohmann::json context_ = nullptr;
        nlohmann::json previous_ = nullptr;
        std::shared_ptr<BaseStore> store_;
        std::shared_ptr<BaseCheckpointSaver> checkpointer_;
        std::shared_ptr<RunControl> control_;
        std::size_t attempt_ { 1 };
        std::optional<std::chrono::steady_clock::time_point> firstAttemptTime_;
        std::optional<std::chrono::steady_clock::time_point> deadline_;
    };

    explicit Runtime(Options options);

    [[nodiscard]] const nlohmann::json& context() const noexcept;
    [[nodiscard]] const CancellationToken& cancellationToken() const noexcept;
    [[nodiscard]] std::shared_ptr<BaseStore> store() const noexcept;
    [[nodiscard]] std::shared_ptr<BaseCheckpointSaver> checkpointer() const noexcept;
    [[nodiscard]] const nlohmann::json& previous() const noexcept;
    [[nodiscard]] const ExecutionInfo& executionInfo() const noexcept;
    [[nodiscard]] bool drainRequested() const;
    [[nodiscard]] std::optional<std::string> drainReason() const;
    void heartbeat() const noexcept;
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> deadline() const noexcept;
    [[nodiscard]] const StreamWriter& streamWriter() const noexcept;
    /// True when this node is being re-entered from Command::resume.
    [[nodiscard]] bool hasResumeValue() const noexcept;
    /// Resume payload supplied by the caller. Only valid when hasResumeValue() is true.
    [[nodiscard]] const nlohmann::json& resumeValue() const;
    /// LangGraph-style function interrupt. Returns a resume value when available;
    /// otherwise records an interrupt request and returns Status::aborted().
    [[nodiscard]] Result<nlohmann::json> interrupt(
        std::string id,
        nlohmann::json value = nlohmann::json::object());
    [[nodiscard]] const std::vector<nlohmann::json>& requestedInterrupts() const noexcept;

private:
    nlohmann::json context_ = nullptr;
    nlohmann::json previous_ = nullptr;
    ExecutionInfo executionInfo_;
    CancellationToken cancellationToken_ { CancellationToken::none() };
    StreamWriter streamWriter_;
    std::optional<nlohmann::json> resumeValue_;
    std::vector<nlohmann::json> requestedInterrupts_;
    nlohmann::json fulfilledInterruptValues_ { nlohmann::json::object() };
    std::size_t interruptCursor_ { 0 };
    std::shared_ptr<BaseStore> store_;
    std::shared_ptr<BaseCheckpointSaver> checkpointer_;
    std::shared_ptr<RunControl> control_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
};

} // namespace lc
