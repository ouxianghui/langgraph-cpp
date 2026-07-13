#pragma once

#include "foundation/cancellation/cancellation_token.hpp"
#include "foundation/executor/i_executor.hpp"
#include "foundation/logging/logger.hpp"
#include "foundation/observability/metrics.hpp"
#include "foundation/observability/tracing.hpp"
#include "foundation/retry/retry_policy.hpp"
#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"
#include "foundation/time/clock.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace lc {

namespace detail {
struct TaskRecord;
struct SchedulerState;
} // namespace detail

enum class ScheduledTaskState : std::uint8_t {
    Pending,
    Running,
    Cancelling,
    Completed,
    Failed,
    Cancelled,
};

enum class SchedulerEventType : std::uint8_t {
    Scheduled,
    Started,
    Completed,
    Cancelled,
    Failed,
    Retrying,
    Closed,
};

enum class PeriodicScheduleMode : std::uint8_t {
    FixedDelay,
    FixedRate,
};

enum class SchedulerClosePolicy : std::uint8_t {
    CancelPending,
    DrainPending,
};

struct SchedulerEvent {
    SchedulerEventType type_ { SchedulerEventType::Scheduled };
    std::uint64_t taskId_ { 0 };
    std::string taskName_;
    ScheduledTaskState taskState_ { ScheduledTaskState::Pending };
    std::uint32_t attempt_ { 0 };
    Status status_ { Status::ok() };
    std::chrono::system_clock::time_point timestamp_ { std::chrono::system_clock::now() };
};

class ISchedulerEventSink {
public:
    virtual ~ISchedulerEventSink() = default;

    ISchedulerEventSink(const ISchedulerEventSink&) = delete;
    ISchedulerEventSink& operator=(const ISchedulerEventSink&) = delete;
    ISchedulerEventSink(ISchedulerEventSink&&) = delete;
    ISchedulerEventSink& operator=(ISchedulerEventSink&&) = delete;

protected:
    ISchedulerEventSink() = default;

public:
    [[nodiscard]] virtual Status publish(SchedulerEvent event) = 0;
};

class SchedulerCallbackSink final : public ISchedulerEventSink {
public:
    using Callback = std::function<Status(SchedulerEvent)>;

    explicit SchedulerCallbackSink(Callback callback);

    [[nodiscard]] Status publish(SchedulerEvent event) override;

private:
    Callback callback_;
};

class ScheduledTask final {
public:
    ScheduledTask() = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint64_t id() const noexcept;
    [[nodiscard]] ScheduledTaskState state() const noexcept;
    [[nodiscard]] Status status() const;
    [[nodiscard]] bool cancel() noexcept;
    [[nodiscard]] bool isCancelled() const noexcept;
    [[nodiscard]] bool isFinished() const noexcept;

private:
    friend class TaskScheduler;

    ScheduledTask(
        std::shared_ptr<detail::TaskRecord> record,
        std::weak_ptr<detail::SchedulerState> scheduler) noexcept;

    std::shared_ptr<detail::TaskRecord> record_;
    std::weak_ptr<detail::SchedulerState> scheduler_;
};

struct ScheduleOptions {
    std::string name_;
    CancellationToken cancellation_;
};

struct PeriodicScheduleOptions : ScheduleOptions {
    /// Empty means run until cancelled.
    std::optional<std::uint64_t> maxRuns_;
    PeriodicScheduleMode mode_ { PeriodicScheduleMode::FixedDelay };
    bool startImmediately_ { false };
};

struct RetryScheduleOptions : ScheduleOptions {
    RetryPolicy policy_ { RetryPolicy::none() };
    bool runImmediately_ { true };
    Clock::Duration initialDelay_ { Clock::Duration::zero() };
};

struct RetryReport {
    std::uint32_t attempts_ { 0 };
    std::uint32_t retries_ { 0 };
    Status status_ { Status::ok() };
};

struct SchedulerOptions {
    std::shared_ptr<IExecutor> executor_;
    std::shared_ptr<ISchedulerEventSink> eventSink_;
    std::shared_ptr<IMetricRecorder> metricsRecorder_;
    std::shared_ptr<ITraceSink> traceSink_;
    std::shared_ptr<ILogger> logger_ { Logger::defaultLogger() };
    const Clock* clock_ { &SteadyClock::instance() };
    SchedulerClosePolicy closePolicy_ { SchedulerClosePolicy::CancelPending };
};

struct SchedulerCloseOptions {
    Clock::Duration timeout_ { Clock::Duration::zero() };
    SchedulerClosePolicy policy_ { SchedulerClosePolicy::CancelPending };
};

class ITaskScheduler {
public:
    using Duration = Clock::Duration;
    using TimePoint = Clock::TimePoint;
    using Task = std::function<void()>;
    using RetryOperation = std::function<Status(std::uint32_t attempt)>;
    using RetryCompletion = std::function<void(RetryReport report)>;

    virtual ~ITaskScheduler() = default;

    ITaskScheduler(const ITaskScheduler&) = delete;
    ITaskScheduler& operator=(const ITaskScheduler&) = delete;
    ITaskScheduler(ITaskScheduler&&) = delete;
    ITaskScheduler& operator=(ITaskScheduler&&) = delete;

protected:
    ITaskScheduler() = default;

public:
    [[nodiscard]] virtual Result<ScheduledTask> scheduleAt(
        TimePoint when,
        Task task,
        ScheduleOptions options = {}) = 0;

    [[nodiscard]] virtual Result<ScheduledTask> scheduleAfter(
        Duration delay,
        Task task,
        ScheduleOptions options = {}) = 0;

    [[nodiscard]] virtual Result<ScheduledTask> schedulePeriodic(
        Duration interval,
        Task task,
        PeriodicScheduleOptions options = {}) = 0;

    [[nodiscard]] virtual Result<ScheduledTask> scheduleRetry(
        RetryOperation operation,
        RetryCompletion completion,
        RetryScheduleOptions options = {}) = 0;

    [[nodiscard]] virtual Result<ScheduledTask> cancelAfter(
        Duration timeout,
        CancellationSource& source,
        std::string reason = "operation timed out",
        ScheduleOptions options = {}) = 0;

    [[nodiscard]] virtual Status waitIdle(Duration timeout) = 0;
    [[nodiscard]] virtual Status close(SchedulerCloseOptions options) = 0;
    [[nodiscard]] virtual Status close(Duration waitIdleTimeout) = 0;
    [[nodiscard]] virtual bool isClosed() const noexcept = 0;
};

/// Runtime-facing delayed/retry/periodic scheduler. Prefer this over `foundation/timer` in graph
/// runtime code so timers share cancellation, lifecycle, metrics, and close semantics.
class TaskScheduler final : public ITaskScheduler {
public:
    explicit TaskScheduler(SchedulerOptions options = {});
    ~TaskScheduler() override;

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;
    TaskScheduler(TaskScheduler&&) = delete;
    TaskScheduler& operator=(TaskScheduler&&) = delete;

    [[nodiscard]] Result<ScheduledTask> scheduleAt(
        TimePoint when,
        Task task,
        ScheduleOptions options = {}) override;

    [[nodiscard]] Result<ScheduledTask> scheduleAfter(
        Duration delay,
        Task task,
        ScheduleOptions options = {}) override;

    [[nodiscard]] Result<ScheduledTask> schedulePeriodic(
        Duration interval,
        Task task,
        PeriodicScheduleOptions options = {}) override;

    [[nodiscard]] Result<ScheduledTask> scheduleRetry(
        RetryOperation operation,
        RetryCompletion completion,
        RetryScheduleOptions options = {}) override;

    [[nodiscard]] Result<ScheduledTask> cancelAfter(
        Duration timeout,
        CancellationSource& source,
        std::string reason = "operation timed out",
        ScheduleOptions options = {}) override;

    [[nodiscard]] Status waitIdle(Duration timeout) override;
    [[nodiscard]] Status close(SchedulerCloseOptions options) override;
    [[nodiscard]] Status close(Duration waitIdleTimeout) override;
    [[nodiscard]] bool isClosed() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view taskStateName(ScheduledTaskState state) noexcept;
[[nodiscard]] std::string_view schedulerEventName(SchedulerEventType type) noexcept;
[[nodiscard]] std::string_view periodicModeName(PeriodicScheduleMode mode) noexcept;
[[nodiscard]] std::string_view closePolicyName(SchedulerClosePolicy policy) noexcept;

} // namespace lc
