#pragma once

#include "foundation/executor/i_executor.hpp"
#include "foundation/logging/logger.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace lc {

/// Deterministic executor that runs accepted tasks synchronously on the caller thread.
class InlineExecutor final : public IExecutor {
public:
    explicit InlineExecutor(std::shared_ptr<ILogger> logger = Logger::defaultLogger());
    ~InlineExecutor() override = default;

    InlineExecutor(const InlineExecutor&) = delete;
    InlineExecutor& operator=(const InlineExecutor&) = delete;
    InlineExecutor(InlineExecutor&&) = delete;
    InlineExecutor& operator=(InlineExecutor&&) = delete;

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

private:
    [[nodiscard]] Status runAccepted(Task task, std::source_location from);
    [[nodiscard]] Status runTask(Task task, std::source_location from);
    void finishTask() noexcept;

    mutable std::mutex mutex_;
    std::condition_variable idleCv_;
    std::shared_ptr<ILogger> logger_;
    bool closed_ { false };
    std::uint64_t outstanding_ { 0 };
};

[[nodiscard]] std::shared_ptr<IExecutor> makeInlineExecutor(
    std::shared_ptr<ILogger> logger = Logger::defaultLogger());

} // namespace lc
