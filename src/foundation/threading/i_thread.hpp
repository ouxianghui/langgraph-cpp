#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <source_location>

namespace lc {

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

    /// Prefer running `task` immediately if already running on this executor's thread; otherwise enqueue.
    /// \param from Call site for tracing (defaults to `std::source_location::current()`).
    virtual void dispatch(std::function<void()> task,
        std::source_location from = std::source_location::current()) = 0;

    /// Run `task` on this executor and block until it completes (`dispatchSync`).
    virtual void dispatchSync(std::function<void()> task,
        std::source_location from = std::source_location::current()) = 0;

    /// Queue `task` for later execution (never runs on the caller's stack before returning).
    virtual void dispatchAsync(std::function<void()> task,
        std::source_location from = std::source_location::current()) = 0;

    /// Run `task` after `delay` (monotonic / steady clock semantics).
    virtual void dispatchAfter(std::chrono::steady_clock::duration delay, std::function<void()> task,
        std::source_location from = std::source_location::current()) = 0;

    /// Block until in-flight work scheduled through this executor has finished, or until `timeout`.
    /// Returns `false` on timeout or when the call is skipped as unsafe from the current thread.
    [[nodiscard]] virtual bool waitIdle(std::chrono::steady_clock::duration timeout) = 0;

    /// Stop accepting work, wait up to `waitIdleTimeout` for in-flight work, then stop the executor.
    /// Returns `false` if the idle wait timed out or shutdown was initiated from the executor thread.
    /// Join itself may still block afterward.
    [[nodiscard]] virtual bool shutdown(std::chrono::steady_clock::duration waitIdleTimeout) noexcept = 0;

    [[nodiscard]] virtual bool isRunning() const noexcept = 0;

    /// `true` when the current thread is running this executor's callbacks (e.g. `Thread`'s
    /// `io_context` / strand worker, or an equivalent notion for other implementations).
    [[nodiscard]] virtual bool isExecutorThread() const noexcept = 0;

    /// Relaxed-atomic metrics snapshot (fields are implementation-defined but shared as `ThreadStats`
    /// for the concrete `Thread` type).
    [[nodiscard]] virtual ThreadStats stats() const noexcept = 0;
};

} // namespace lc
