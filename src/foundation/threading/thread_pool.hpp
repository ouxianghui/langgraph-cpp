#pragma once

#include "foundation/logging/logger.hpp"
#include "foundation/threading/i_thread_pool.hpp"

#include <chrono>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <source_location>
#include <utility>

namespace lc {

/// Internal fixed-size worker pool used by `ConcurrentExecutor`.
///
/// Prefer `ConcurrentExecutor` from `foundation/executor` in runtime-facing code. This class remains
/// available for low-level adapters, tests, and components that explicitly own worker-pool details.
///
/// Work items posted via `submit` keep the implementation alive until they finish. Destroying a
/// `ThreadPool` from inside a task on the same pool is supported: `~ThreadPool` returns while a
/// helper thread completes shutdown; do not dereference the `ThreadPool*` after `delete` returns.
///
/// \param maxPendingSubmit Maximum in-flight `submit` work items (queued + running). `0` means
///        unbounded (no `submitRejectedQueueFull` backpressure).
class ThreadPool final : public IThreadPool {
public:
    /// \param threadCount Number of worker threads; `0` uses `max(1, hardware_concurrency())`.
    explicit ThreadPool(
        std::size_t threadCount = 0,
        std::size_t maxPendingSubmit = 0,
        std::shared_ptr<ILogger> logger = Logger::defaultLogger());
    ~ThreadPool() override;

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    [[nodiscard]] Status submit(std::function<void()> task,
        std::source_location from = std::source_location::current()) override;
    [[nodiscard]] Status waitIdle(std::chrono::steady_clock::duration timeout) override;
    [[nodiscard]] Status shutdown(std::chrono::steady_clock::duration waitIdleTimeout) override;
    [[nodiscard]] bool isRunning() const noexcept override;
    [[nodiscard]] bool isWorkerThread() const noexcept override;
    [[nodiscard]] std::size_t threadCount() const noexcept override;
    [[nodiscard]] ThreadPoolStats stats() const noexcept override;

    template <typename F>
        requires std::invocable<F>
    [[nodiscard]] Status submit(F&& f, std::source_location from = std::source_location::current())
    {
        return submit(std::function<void()>(std::forward<F>(f)), from);
    }

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

[[nodiscard]] inline std::unique_ptr<IThreadPool> makeThreadPool(std::size_t threadCount = 0,
    std::size_t maxPendingSubmit = 0,
    std::shared_ptr<ILogger> logger = Logger::defaultLogger())
{
    return std::make_unique<ThreadPool>(threadCount, maxPendingSubmit, std::move(logger));
}

} // namespace lc
