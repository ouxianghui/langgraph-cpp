#pragma once

#include "foundation/status/status.hpp"

#include <chrono>
#include <functional>
#include <source_location>

namespace lgc {

/// Runtime-facing execution abstraction for graph nodes and background work.
///
/// Runtime code should depend on this surface. The lower-level `foundation/threading` types are
/// implementation substrate for concrete executors, not graph/runtime integration points.
///
/// Execution methods report whether a task was accepted or completed. Asynchronous task results
/// should be carried by the graph runtime's own future/result channel; executor implementations
/// only own scheduling, lifecycle, and uncaught-exception containment.
///
/// The base surface is intentionally small but still covers serial executors: implementations can
/// report whether the caller is already running on the executor and can optionally support delayed
/// execution.
class IExecutor {
public:
    using Task = std::function<void()>;
    using Duration = std::chrono::steady_clock::duration;

    virtual ~IExecutor() = default;

    IExecutor(const IExecutor&) = delete;
    IExecutor& operator=(const IExecutor&) = delete;
    IExecutor(IExecutor&&) = delete;
    IExecutor& operator=(IExecutor&&) = delete;

protected:
    IExecutor() = default;

public:
    /// Post `task` for later execution. Implementations must not run `task` on the caller's stack
    /// before this method returns. Direct executors that cannot defer may return `Unimplemented`.
    [[nodiscard]] virtual Status post(
        Task task,
        std::source_location from = std::source_location::current()) = 0;

    /// Post `task` after `delay`. Implementations must not run `task` on the caller's stack before
    /// this method returns. Executors without timer support may return `Unimplemented` for positive
    /// delays; zero / negative delays should behave like `post()` when posting is supported.
    [[nodiscard]] virtual Status postDelayed(
        Duration delay,
        Task task,
        std::source_location from = std::source_location::current()) = 0;

    /// Run `task` inline when already executing on this executor; otherwise post it.
    [[nodiscard]] virtual Status execute(Task task, std::source_location from = std::source_location::current()) = 0;

    /// Run `task` inline when already executing on this executor; otherwise execute it and wait for completion.
    [[nodiscard]] virtual Status executeAndWait(
        Task task,
        std::source_location from = std::source_location::current()) = 0;

    /// Wait until tasks accepted through this executor have completed, or until `timeout`.
    [[nodiscard]] virtual Status waitIdle(Duration timeout) = 0;

    /// Stop accepting new tasks and wait up to `waitIdleTimeout` for accepted work to finish.
    [[nodiscard]] virtual Status close(Duration waitIdleTimeout) = 0;

    [[nodiscard]] virtual bool isClosed() const noexcept = 0;

    /// True while the current thread is executing a task accepted by this executor.
    [[nodiscard]] virtual bool isExecutorThread() const noexcept = 0;
};

} // namespace lgc
