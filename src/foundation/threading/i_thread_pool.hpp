#pragma once

#include "foundation/status/status.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <source_location>

namespace lgc {

/// Internal substrate for concurrent executor implementations.
///
/// Runtime and graph code should use `IExecutor` / `ConcurrentExecutor`. `IThreadPool` remains a
/// low-level worker-pool contract for adapters, tests, and components that explicitly need pool
/// ownership.

/// Relaxed counters for logs / metrics (not synchronized with each other as a single snapshot).
struct ThreadPoolStats {
    std::uint64_t submitAccepted = 0;
    std::uint64_t submitRejectedWhileStopped = 0;
    std::uint64_t submitRejectedQueueFull = 0;
    std::uint64_t tasksCompleted = 0;
    std::uint64_t taskUncaughtExceptions = 0;
    std::uint64_t peakOutstanding = 0;
    std::uint64_t waitIdleEarlyReturnFromPoolThread = 0;
    std::uint64_t waitIdleTimeouts = 0;
    std::uint64_t shutdownInvokedFromPoolThread = 0;
    std::uint64_t shutdownIdleWaitTimeouts = 0;
};

/// Fixed-size worker pool: enqueue work, wait for quiescence, shut down. Concrete implementations
/// choose their own scheduling backend.
///
/// **Contracts**
/// - `submit` is thread-safe. After `shutdown(timeout)`, `submit` returns `FailedPrecondition` and does not run the task.
/// - `shutdown(timeout)`: calling from a task on the same pool uses deferred worker joining on a
///   helper thread (best-effort). Destroying `ThreadPool` from a pool
///   worker schedules full teardown on a separate thread so the destructor can return without
///   joining the pool on the worker stack; do not use the `ThreadPool*` after `delete` returns.
/// - `waitIdle(timeout)`: must not be called from a pool worker thread (deadlock guard:
///   early return + metric).
class IThreadPool {
public:
    virtual ~IThreadPool() = default;

    IThreadPool(const IThreadPool&) = delete;
    IThreadPool& operator=(const IThreadPool&) = delete;
    IThreadPool(IThreadPool&&) = delete;
    IThreadPool& operator=(IThreadPool&&) = delete;

protected:
    IThreadPool() = default;

public:
    /// Enqueue `task` on a pool thread. Returns an error if the pool is not accepting work or the
    /// bounded queue is full.
    /// \param from Call site for tracing (defaults to `std::source_location::current()`).
    [[nodiscard]] virtual Status submit(std::function<void()> task,
        std::source_location from = std::source_location::current()) = 0;

    /// Block until all work that was successfully `submit`ted has finished executing, or until `timeout`.
    /// Returns `DeadlineExceeded` on timeout or `FailedPrecondition` when called from a worker thread.
    [[nodiscard]] virtual Status waitIdle(std::chrono::steady_clock::duration timeout) = 0;

    /// Stop accepting new work, wait up to `waitIdleTimeout` for in-flight `submit` work, then join
    /// threads. Returns `DeadlineExceeded` if the idle wait timed out or `FailedPrecondition` if
    /// shutdown was initiated from a worker thread (deferred join). Join itself may still block
    /// until the implementation finishes tearing down workers.
    [[nodiscard]] virtual Status shutdown(std::chrono::steady_clock::duration waitIdleTimeout) = 0;

    [[nodiscard]] virtual bool isRunning() const noexcept = 0;

    /// True when the current thread is executing a task submitted to this pool.
    [[nodiscard]] virtual bool isWorkerThread() const noexcept = 0;

    [[nodiscard]] virtual std::size_t threadCount() const noexcept = 0;

    [[nodiscard]] virtual ThreadPoolStats stats() const noexcept = 0;
};

} // namespace lgc
