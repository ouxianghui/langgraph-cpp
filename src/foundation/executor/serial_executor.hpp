#pragma once

#include "foundation/executor/i_executor.hpp"
#include "foundation/logging/logger.hpp"
#include "foundation/threading/i_thread.hpp"

#include <cstddef>
#include <memory>
#include <string_view>

namespace lc {

/// `IExecutor` adapter backed by a single serial `Thread`.
class SerialExecutor final : public IExecutor {
public:
    explicit SerialExecutor(std::shared_ptr<IThread> thread);
    explicit SerialExecutor(
        std::size_t maxPendingDispatch = 0,
        std::shared_ptr<ILogger> logger = Logger::defaultLogger());
    SerialExecutor(
        std::string_view name,
        std::size_t maxPendingDispatch = 0,
        std::shared_ptr<ILogger> logger = Logger::defaultLogger());
    ~SerialExecutor() override = default;

    SerialExecutor(const SerialExecutor&) = delete;
    SerialExecutor& operator=(const SerialExecutor&) = delete;
    SerialExecutor(SerialExecutor&&) = delete;
    SerialExecutor& operator=(SerialExecutor&&) = delete;

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

    [[nodiscard]] std::shared_ptr<IThread> thread() const noexcept;

private:
    [[nodiscard]] Status enqueue(Task task, std::source_location from);
    [[nodiscard]] Status runTask(Task task);

    std::shared_ptr<IThread> thread_;
};

[[nodiscard]] std::shared_ptr<IExecutor> makeSerialExecutor(
    std::size_t maxPendingDispatch = 0,
    std::shared_ptr<ILogger> logger = Logger::defaultLogger());

[[nodiscard]] std::shared_ptr<IExecutor> makeSerialExecutor(
    std::string_view name,
    std::size_t maxPendingDispatch = 0,
    std::shared_ptr<ILogger> logger = Logger::defaultLogger());

[[nodiscard]] std::shared_ptr<IExecutor> makeSerialExecutor(
    std::shared_ptr<IThread> thread);

} // namespace lc
