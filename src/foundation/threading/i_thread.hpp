#pragma once

#include "foundation/status/status.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <source_location>

namespace lc {

/// Internal substrate for executor implementations.
///
/// Runtime and graph code should depend on `foundation/executor` instead of this interface. `IThread`
/// exists to keep the standard-library worker queue reusable and testable underneath
/// `SerialExecutor` / `OwnerExecutor`.

/// Best-effort counters for observability (lossy relaxed atomics; suitable for logs / gauges).
struct ThreadStats {
    uint64_t asyncTasksDroppedWhileStopped_ = 0;
    uint64_t scheduleExceptions_ = 0;
    uint64_t dispatchedTaskExceptions_ = 0;
    uint64_t dispatchSyncRejectedWhileStopped_ = 0;
    uint64_t workerLoopExceptions_ = 0;
    uint64_t dispatchAccepted_ = 0;
    uint64_t dispatchRejectedQueueFull_ = 0;
    uint64_t tasksCompleted_ = 0;
    uint64_t peakOutstanding_ = 0;
    uint64_t waitIdleTimeouts_ = 0;
    uint64_t waitIdleEarlyReturnFromExecutorThread_ = 0;
    uint64_t shutdownIdleWaitTimeouts_ = 0;
};

/// Minimal executor abstraction (similar role to a GCD dispatch queue target): schedule work on a
/// dedicated execution context. Implementations are responsible for thread lifetime and ordering
/// semantics.
///
/// **After `shutdown(timeout)`** (see `Thread` for a concrete contract): new work is not accepted;
/// `dispatchSync` may throw, while fire-and-forget entry points may drop tasks without running them.
///
/// The virtual surface uses `std::function<void()>` so implementations can be swapped without
/// templates. There is no standard non-owning callable view in C++20 (see `Thread` for convenience
/// overloads). C++26 adds `std::function_ref`.
class IThread {
public:
    virtual ~IThread() = default;

    /// Queue `task` for later execution (never runs on the caller's stack before returning).
    virtual void dispatchAsync(std::function<void()> task,
        std::source_location from = std::source_location::current()) = 0;

    /// Run `task` after `delay` (monotonic / steady clock semantics).
    virtual void dispatchAfter(std::chrono::steady_clock::duration delay, std::function<void()> task,
        std::source_location from = std::source_location::current()) = 0;

    /// Prefer running `task` immediately if already running on this thread; otherwise enqueue.
    /// \param from Call site for tracing (defaults to `std::source_location::current()`).
    virtual void dispatch(std::function<void()> task,
        std::source_location from = std::source_location::current()) = 0;

    /// Run `task` on this executor and block until it completes (`dispatchSync`).
    virtual void dispatchSync(std::function<void()> task,
        std::source_location from = std::source_location::current()) = 0;

    /// Block until in-flight work scheduled through this executor has finished, or until `timeout`.
    /// Returns `DeadlineExceeded` on timeout or `FailedPrecondition` when called from this thread.
    [[nodiscard]] virtual Status waitIdle(std::chrono::steady_clock::duration timeout) = 0;

    /// Stop accepting work, wait up to `waitIdleTimeout` for in-flight work, then stop the executor.
    /// Returns `DeadlineExceeded` if the idle wait timed out or `FailedPrecondition` when shutdown
    /// was initiated from this thread.
    /// Join itself may still block afterward.
    [[nodiscard]] virtual Status shutdown(std::chrono::steady_clock::duration waitIdleTimeout) = 0;

    [[nodiscard]] virtual bool isRunning() const noexcept = 0;

    /// `true` when the current thread is running this thread's callbacks.
    [[nodiscard]] virtual bool isCurrentThread() const noexcept = 0;

    /// Relaxed-atomic metrics snapshot (fields are implementation-defined but shared as `ThreadStats`
    /// for the concrete `Thread` type).
    [[nodiscard]] virtual ThreadStats stats() const noexcept = 0;
};

} // namespace lc
