#pragma once

#include "foundation/executor/i_executor.hpp"
#include "foundation/logging/logger.hpp"
#include "foundation/threading/i_thread_pool.hpp"

#include <atomic>
#include <cstddef>
#include <memory>

namespace lgc {

/// `IExecutor` adapter backed by an `IThreadPool`.
class ConcurrentExecutor final : public IExecutor {
public:
    explicit ConcurrentExecutor(std::shared_ptr<IThreadPool> pool);
    explicit ConcurrentExecutor(
        std::size_t threadCount = 0,
        std::size_t maxPendingSubmit = 0,
        std::shared_ptr<ILogger> logger = Logger::defaultLogger());
    ~ConcurrentExecutor() override = default;

    ConcurrentExecutor(const ConcurrentExecutor&) = delete;
    ConcurrentExecutor& operator=(const ConcurrentExecutor&) = delete;
    ConcurrentExecutor(ConcurrentExecutor&&) = delete;
    ConcurrentExecutor& operator=(ConcurrentExecutor&&) = delete;

    [[nodiscard]] Status post(Task task, std::source_location from = std::source_location::current()) override;
    [[nodiscard]] Status postDelayed(
        Duration delay,
        Task task,
        std::source_location from = std::source_location::current()) override;
    [[nodiscard]] Status execute(Task task, std::source_location from = std::source_location::current()) override;
    [[nodiscard]] Status executeAndWait(Task task, std::source_location from = std::source_location::current()) override;

    [[nodiscard]] Status waitIdle(Duration timeout) override;
    [[nodiscard]] Status close(Duration waitIdleTimeout) override;
    [[nodiscard]] bool isClosed() const noexcept override;
    [[nodiscard]] bool isExecutorThread() const noexcept override;

    [[nodiscard]] std::shared_ptr<IThreadPool> threadPool() const noexcept;

private:
    [[nodiscard]] Status enqueue(Task task, std::source_location from);

    std::shared_ptr<IThreadPool> pool_;
    std::atomic<bool> closed_ { false };
};

[[nodiscard]] std::shared_ptr<IExecutor> makeConcurrentExecutor(
    std::size_t threadCount = 0,
    std::size_t maxPendingSubmit = 0,
    std::shared_ptr<ILogger> logger = Logger::defaultLogger());

[[nodiscard]] std::shared_ptr<IExecutor> makeConcurrentExecutor(
    std::shared_ptr<IThreadPool> pool);

} // namespace lgc
