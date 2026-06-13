#pragma once

#include "foundation/threading/i_thread_pool.hpp"

#include <chrono>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <source_location>

namespace lc {

/// Default `IThreadPool` implementation (details behind `ThreadPool::Impl`).
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
    explicit ThreadPool(std::size_t threadCount = 0, std::size_t maxPendingSubmit = 0);
    ~ThreadPool() override;

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    [[nodiscard]] bool submit(std::function<void()> task,
        std::source_location from = std::source_location::current()) override;
    [[nodiscard]] bool waitIdle(std::chrono::steady_clock::duration timeout) override;
    [[nodiscard]] bool shutdown(std::chrono::steady_clock::duration waitIdleTimeout) noexcept override;
    [[nodiscard]] bool isRunning() const noexcept override;
    [[nodiscard]] std::size_t threadCount() const noexcept override;
    [[nodiscard]] ThreadPoolStats stats() const noexcept override;

    /// True when the current thread is executing a `submit` callback on this pool.
    [[nodiscard]] bool isExecutingOnThisPool() const noexcept;

    template <typename F>
        requires std::invocable<F>
    [[nodiscard]] bool submit(F&& f, std::source_location from = std::source_location::current())
    {
        return submit(std::function<void()>(std::forward<F>(f)), from);
    }

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

[[nodiscard]] inline std::unique_ptr<IThreadPool> makeThreadPool(std::size_t threadCount = 0,
    std::size_t maxPendingSubmit = 0)
{
    return std::make_unique<ThreadPool>(threadCount, maxPendingSubmit);
}

} // namespace lc
