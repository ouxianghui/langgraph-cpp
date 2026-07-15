#pragma once

#include "foundation/scheduler/scheduler.hpp"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace lgc {

namespace detail {

struct TaskRecord {
    explicit TaskRecord(std::uint64_t id, std::string name)
        : id_(id)
        , name_(std::move(name))
    {
    }

    mutable std::mutex mutex_;
    std::uint64_t id_ { 0 };
    std::string name_;
    std::string kind_ { "task" };
    ScheduledTaskState state_ { ScheduledTaskState::Pending };
    Status status_ { Status::ok() };
    bool cancelRequested_ { false };
    bool running_ { false };
    bool active_ { true };
    CancellationRegistration cancellationRegistration_;
    Clock::TimePoint startedAt_ {};
};

struct SchedulerState {
    struct ScheduledEntry {
        Clock::TimePoint due_;
        std::uint64_t sequence_ { 0 };
        std::shared_ptr<detail::TaskRecord> task_;
        std::function<void()> run_;

        [[nodiscard]] bool operator>(const ScheduledEntry& other) const noexcept
        {
            if (due_ == other.due_)
                return sequence_ > other.sequence_;
            return due_ > other.due_;
        }
    };

    mutable std::mutex mutex_;
    std::condition_variable wakeCv_;
    std::condition_variable idleCv_;
    std::priority_queue<ScheduledEntry, std::vector<ScheduledEntry>, std::greater<ScheduledEntry>> queue_;
    std::uint64_t nextTaskId_ { 1 };
    std::uint64_t nextEntryId_ { 1 };
    std::uint64_t active_ { 0 };
    bool closed_ { false };
    std::shared_ptr<IExecutor> executor_;
    std::shared_ptr<ISchedulerEventSink> eventSink_;
    std::shared_ptr<IMetricRecorder> metricsRecorder_;
    std::shared_ptr<ITraceSink> traceSink_;
    std::shared_ptr<ILogger> logger_;
    const Clock* clock_ { &SteadyClock::instance() };
    SchedulerClosePolicy closePolicy_ { SchedulerClosePolicy::CancelPending };
};

} // namespace detail

namespace scheduler_detail {

using Duration = Clock::Duration;
using TimePoint = Clock::TimePoint;

struct RetryTask {
    RetryPolicy policy_;
    ITaskScheduler::RetryOperation operation_;
    ITaskScheduler::RetryCompletion completion_;
    CancellationToken cancellation_;
    std::uint32_t attempts_ { 0 };
};

[[nodiscard]] TimePoint clockNow(const Clock& clock) noexcept;
[[nodiscard]] TimePoint dueAfter(const Clock& clock, Duration delay) noexcept;
[[nodiscard]] Status currentExceptionStatus(std::string_view fallback);
[[nodiscard]] bool isTerminal(ScheduledTaskState state) noexcept;
[[nodiscard]] std::string taskName(const detail::TaskRecord& state);
[[nodiscard]] SchedulerEvent makeSchedulerEvent(
    const std::shared_ptr<detail::TaskRecord>& state,
    SchedulerEventType type,
    Status status = Status::ok(),
    std::uint32_t attempt = 0);
void publish(const std::shared_ptr<detail::SchedulerState>& scheduler, SchedulerEvent event) noexcept;
void countMetric(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    std::string name,
    const detail::TaskRecord& state) noexcept;
void recordDuration(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const detail::TaskRecord& state,
    Duration duration) noexcept;
void recordSpan(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const detail::TaskRecord& state,
    Duration duration,
    SpanStatus spanStatus,
    const Status& status) noexcept;
[[nodiscard]] bool startTask(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const std::shared_ptr<detail::TaskRecord>& state);
void finishTask(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const std::shared_ptr<detail::TaskRecord>& state,
    ScheduledTaskState finalState,
    Status status = Status::ok(),
    std::uint32_t attempt = 0) noexcept;
[[nodiscard]] bool requestCancel(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const std::shared_ptr<detail::TaskRecord>& state,
    Status status = Status::cancelled("scheduled task cancelled")) noexcept;
[[nodiscard]] bool reschedule(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const std::shared_ptr<detail::TaskRecord>& state,
    TimePoint due,
    std::function<void()> fire);
[[nodiscard]] Status scheduleEntry(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    TimePoint due,
    std::shared_ptr<detail::TaskRecord> state,
    std::function<void()> fire);
void attachCancellation(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const std::shared_ptr<detail::TaskRecord>& state,
    const CancellationToken& token);
[[nodiscard]] Status postToExecutor(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    IExecutor::Task task);
void dispatchRetry(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const std::shared_ptr<detail::TaskRecord>& state,
    std::shared_ptr<RetryTask> retry);

} // namespace scheduler_detail

} // namespace lgc
