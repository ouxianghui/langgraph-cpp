#pragma once

#include "foundation/logging/logger.hpp"
#include "foundation/threading/i_thread.hpp"

#include <chrono>
#include <concepts>
#include <functional>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lc {

/// Thrown when `dispatchSync` is called while this dispatcher is not accepting work (`shutdown(timeout)`
/// completed or shutdown in progress). Async APIs (`dispatchAsync`, `dispatch`, `dispatchAfter`)
/// fail silently (task dropped) instead.
class ThreadStopped : public std::runtime_error {
public:
    ThreadStopped();
};

/// Internal serial worker: one worker thread + a monotonic-time priority queue for strict serial
/// execution and delayed tasks.
///
/// Prefer `SerialExecutor` / `OwnerExecutor` from `foundation/executor` in runtime-facing code.
/// This class is the reusable substrate those executors build on, not the graph runtime API.
///
/// **Lifecycle contract**
/// - Call `shutdown(timeout)` before destroying the object if you rely on the worker thread being joined
///   before leaving scope; `shutdown(timeout)` runs teardown on the calling thread (unless
///   invoked from this dispatcher's worker / strand, in which case join is deferred internally).
/// - `ThreadPool` can run `~Impl` synchronously from `~ThreadPool` on client threads because pool
///   workers clear `onWorker()` TLS before the last `shared_ptr` is released. This `Thread` owns a
///   joinable `std::thread worker_`; if `~Impl` ran on that worker after a posted handler dropped
///   the final ref, member destruction could still see `worker_` as joinable on the wrong thread.
///   So `~Thread` always schedules teardown on a detached helper (same pattern as
///   `~ThreadPool` when `onWorker()` is true). Prefer explicit `shutdown(timeout)` before destruction if you
///   need join-before-return on non-executor threads.
/// - After `shutdown(timeout)`, `isRunning()` is false: `dispatchAsync` / `dispatch` / `dispatchAfter`
///   drop work (see `ThreadStats`), and `dispatchSync` throws `ThreadStopped`.
/// - Destroying `Thread` or calling `shutdown(timeout)` from the serial worker thread defers
///   worker joining to a helper thread (same idea as `ThreadPool` from a pool worker thread). Do
///   not use the `Thread*` after `delete` returns until teardown completes.
/// - `dispatchSync` runs inline when already on this dispatcher's executor thread, avoiding deadlocks
///   when nesting sync work.
/// - Do not call `dispatchSync` from a thread that cannot unwind while another thread may call
///   `shutdown(timeout)` unless you accept possible blocking until shutdown ordering is resolved.
///
/// Besides the `IThread` virtuals (which take `std::function<void()>`), this class provides
/// template overloads that convert callables to `std::function` for ergonomics. That conversion
/// may allocate; there is still no standard `function_view` in C++20 — use a lambda that forwards to
/// your own non-owning wrapper if you need zero-allocation hot paths.
class Thread final : public IThread {
public:
    /// \param maxPendingDispatch Maximum in-flight dispatched tasks (queued + running). `0` means
    ///        unbounded (no `dispatchRejectedQueueFull_` backpressure).
    explicit Thread(
        std::size_t maxPendingDispatch = 0,
        std::shared_ptr<ILogger> logger = Logger::defaultLogger());

    Thread(
        std::string_view name,
        std::size_t maxPendingDispatch = 0,
        std::shared_ptr<ILogger> logger = Logger::defaultLogger());

    ~Thread() override;

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&&) = delete;
    Thread& operator=(Thread&&) = delete;

    void dispatchAsync(std::function<void()> task,
        std::source_location from = std::source_location::current()) override;

    void dispatchAfter(std::chrono::steady_clock::duration delay, std::function<void()> task,
        std::source_location from = std::source_location::current()) override;

    void dispatch(std::function<void()> task,
        std::source_location from = std::source_location::current()) override;

    void dispatchSync(std::function<void()> task,
        std::source_location from = std::source_location::current()) override;

    [[nodiscard]] Status waitIdle(std::chrono::steady_clock::duration timeout) override;

    [[nodiscard]] Status shutdown(std::chrono::steady_clock::duration waitIdleTimeout) override;

    [[nodiscard]] bool isRunning() const noexcept override;

    [[nodiscard]] bool isCurrentThread() const noexcept override;

    [[nodiscard]] ThreadStats stats() const noexcept override;

    template <std::invocable F>
    void dispatchAsync(F&& f, std::source_location from = std::source_location::current())
    {
        dispatchAsync(std::function<void()>(std::forward<F>(f)), from);
    }

    template <std::invocable F>
    void dispatchAfter(std::chrono::steady_clock::duration delay, F&& f,
        std::source_location from = std::source_location::current())
    {
        dispatchAfter(delay, std::function<void()>(std::forward<F>(f)), from);
    }

    template <std::invocable F>
    void dispatch(F&& f, std::source_location from = std::source_location::current())
    {
        dispatch(std::function<void()>(std::forward<F>(f)), from);
    }

    template <std::invocable F>
    void dispatchSync(F&& f, std::source_location from = std::source_location::current())
    {
        dispatchSync(std::function<void()>(std::forward<F>(f)), from);
    }

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace lc
