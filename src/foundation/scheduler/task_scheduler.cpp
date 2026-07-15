#include "foundation/scheduler/scheduler.hpp"

#include "foundation/executor/serial_executor.hpp"
#include "foundation/scheduler/scheduler_state.hh"

#include <thread>
#include <utility>
#include <vector>

namespace lgc::scheduler_detail {

void dispatchOnce(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const std::shared_ptr<detail::TaskRecord>& state,
    ITaskScheduler::Task task,
    CancellationToken cancellation)
{
    if (!startTask(scheduler, state))
        return;

    auto status = postToExecutor(scheduler, [scheduler, state, task = std::move(task), cancellation = std::move(cancellation)]() mutable {
        if (cancellation.cancelled()) {
            finishTask(scheduler, state, ScheduledTaskState::Cancelled, Status::cancelled(cancellation.reason()));
            return;
        }
        try {
            task();
            finishTask(scheduler, state, ScheduledTaskState::Completed);
        } catch (...) {
            auto failure = currentExceptionStatus("scheduled task failed");
            finishTask(scheduler, state, ScheduledTaskState::Completed, failure);
        }
    });

    if (!status.isOk())
        finishTask(scheduler, state, ScheduledTaskState::Failed, status);
}

void dispatchPeriodic(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const std::shared_ptr<detail::TaskRecord>& state,
    ITaskScheduler::Task task,
    CancellationToken cancellation,
    Duration interval,
    std::optional<std::uint64_t> maxRuns,
    std::shared_ptr<std::uint64_t> runs,
    std::shared_ptr<TimePoint> nextDue,
    PeriodicScheduleMode mode)
{
    if (!startTask(scheduler, state))
        return;

    auto status = postToExecutor(scheduler, [scheduler, state, task, cancellation = std::move(cancellation), interval, maxRuns, runs, nextDue, mode]() mutable {
        if (cancellation.cancelled()) {
            finishTask(scheduler, state, ScheduledTaskState::Cancelled, Status::cancelled(cancellation.reason()));
            return;
        }

        Status runStatus = Status::ok();
        try {
            task();
        } catch (...) {
            runStatus = currentExceptionStatus("periodic task failed");
            publish(scheduler, makeSchedulerEvent(state, SchedulerEventType::Failed, runStatus));
            countMetric(scheduler, "scheduler.task.failed", *state);
        }

        ++(*runs);
        if (maxRuns.has_value() && *runs >= *maxRuns) {
            finishTask(scheduler, state, ScheduledTaskState::Completed);
            return;
        }

        TimePoint due;
        if (mode == PeriodicScheduleMode::FixedRate) {
            *nextDue += interval;
            while (*nextDue < clockNow(*scheduler->clock_))
                *nextDue += interval;
            due = *nextDue;
        } else {
            due = dueAfter(*scheduler->clock_, interval);
            *nextDue = due;
        }

        auto fire = [scheduler, state, task, cancellation, interval, maxRuns, runs, nextDue, mode]() mutable {
            dispatchPeriodic(scheduler, state, std::move(task), std::move(cancellation), interval, maxRuns, runs, nextDue, mode);
        };
        (void)reschedule(scheduler, state, due, std::move(fire));
    });

    if (!status.isOk())
        finishTask(scheduler, state, ScheduledTaskState::Failed, status);
}

} // namespace lgc::scheduler_detail

namespace lgc {
namespace {
using scheduler_detail::Duration;
using scheduler_detail::TimePoint;
using scheduler_detail::RetryTask;
using scheduler_detail::attachCancellation;
using scheduler_detail::clockNow;
using scheduler_detail::currentExceptionStatus;
using scheduler_detail::dispatchRetry;
using scheduler_detail::dispatchOnce;
using scheduler_detail::dispatchPeriodic;
using scheduler_detail::dueAfter;
using scheduler_detail::finishTask;
using scheduler_detail::postToExecutor;
using scheduler_detail::publish;
using scheduler_detail::requestCancel;
using scheduler_detail::reschedule;
using scheduler_detail::scheduleEntry;
using scheduler_detail::startTask;
}

struct TaskScheduler::Impl {
    explicit Impl(SchedulerOptions options)
        : state_(std::make_shared<detail::SchedulerState>())
    {
        state_->eventSink_ = std::move(options.eventSink_);
        state_->metricsRecorder_ = std::move(options.metricsRecorder_);
        state_->traceSink_ = std::move(options.traceSink_);
        state_->logger_ = std::move(options.logger_);
        state_->clock_ = options.clock_ == nullptr ? &SteadyClock::instance() : options.clock_;
        state_->closePolicy_ = options.closePolicy_;
        if (options.executor_) {
            state_->executor_ = std::move(options.executor_);
        } else {
            state_->executor_ = makeSerialExecutor("task-scheduler", 0, state_->logger_);
            ownsExecutor_ = true;
        }
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~Impl()
    {
        (void)close(SchedulerCloseOptions {
            .timeout_ = Duration::zero(),
            .policy_ = SchedulerClosePolicy::CancelPending,
        });
        if (worker_.joinable())
            worker_.join();
        closeExecutor(Duration::zero());
    }

    void closeExecutor(Duration timeout) noexcept
    {
        if (!ownsExecutor_)
            return;
        std::shared_ptr<IExecutor> executor;
        {
            std::lock_guard lock(state_->mutex_);
            executor = state_->executor_;
        }
        if (executor)
            (void)executor->close(timeout);
    }

    [[nodiscard]] std::shared_ptr<detail::TaskRecord> makeTask(std::string name, std::string kind)
    {
        std::lock_guard lock(state_->mutex_);
        auto record = std::make_shared<detail::TaskRecord>(state_->nextTaskId_++, std::move(name));
        record->kind_ = std::move(kind);
        return record;
    }

    [[nodiscard]] Status close(SchedulerCloseOptions options)
    {
        auto drainQueue = [this] {
            std::vector<std::shared_ptr<detail::TaskRecord>> records;
            while (!state_->queue_.empty()) {
                auto record = state_->queue_.top().task_;
                state_->queue_.pop();
                records.push_back(std::move(record));
            }
            return records;
        };

        auto cancelRecords = [this](const std::vector<std::shared_ptr<detail::TaskRecord>>& records) {
            const auto status = Status::cancelled("scheduler closed");
            for (const auto& record : records)
                (void)requestCancel(state_, record, status);
        };

        std::vector<std::shared_ptr<detail::TaskRecord>> queued;
        {
            std::lock_guard lock(state_->mutex_);
            if (!state_->closed_) {
                state_->closed_ = true;
                if (options.policy_ == SchedulerClosePolicy::CancelPending) {
                    queued = drainQueue();
                }
            }
        }
        state_->wakeCv_.notify_all();
        state_->idleCv_.notify_all();

        cancelRecords(queued);

        publish(state_, SchedulerEvent {
            .type_ = SchedulerEventType::Closed,
            .timestamp_ = std::chrono::system_clock::now(),
        });

        auto idle = waitIdle(options.timeout_);
        if (!idle.isOk()) {
            logTo(state_->logger_,
                LogLevel::Warn,
                "TaskScheduler",
                "close waitIdle failed policy={} status={}",
                __FILE__,
                __LINE__,
                closePolicyName(options.policy_),
                idle.toString());
            {
                std::lock_guard lock(state_->mutex_);
                queued = drainQueue();
            }
            state_->wakeCv_.notify_all();
            state_->idleCv_.notify_all();
            cancelRecords(queued);
            closeExecutor(Duration::zero());
            return idle;
        }
        closeExecutor(options.timeout_);
        return Status::ok();
    }

    [[nodiscard]] Status waitIdle(Duration timeout)
    {
        std::unique_lock lock(state_->mutex_);
        if (timeout <= Duration::zero()) {
            return state_->active_ == 0
                ? Status::ok()
                : Status::deadlineExceeded("scheduler did not become idle before timeout");
        }
        if (!state_->idleCv_.wait_for(lock, timeout, [&] {
            return state_->active_ == 0;
        }))
            return Status::deadlineExceeded("scheduler did not become idle before timeout");
        return Status::ok();
    }

    [[nodiscard]] bool isClosed() const noexcept
    {
        std::lock_guard lock(state_->mutex_);
        return state_->closed_;
    }

    void workerLoop()
    {
        std::unique_lock lock(state_->mutex_);
        for (;;) {
            if (state_->closed_ && state_->queue_.empty())
                break;

            if (state_->queue_.empty()) {
                state_->wakeCv_.wait(lock, [&] {
                    return state_->closed_ || !state_->queue_.empty();
                });
                continue;
            }

            const auto due = state_->queue_.top().due_;
            const auto current = clockNow(*state_->clock_);
            if (due > current) {
                state_->wakeCv_.wait_for(lock, due - current);
                continue;
            }

            auto entry = state_->queue_.top();
            state_->queue_.pop();
            lock.unlock();
            try {
                entry.run_();
            } catch (...) {
                auto failure = currentExceptionStatus("scheduled fire callback failed");
                logTo(state_->logger_,
                    LogLevel::Warn,
                    "TaskScheduler",
                    "fire callback threw id={} status={}",
                    __FILE__,
                    __LINE__,
                    entry.task_->id_,
                    failure.toString());
                finishTask(state_, entry.task_, ScheduledTaskState::Completed, failure);
            }
            lock.lock();
        }
    }

    std::shared_ptr<detail::SchedulerState> state_;
    std::thread worker_;
    bool ownsExecutor_ { false };
};

TaskScheduler::TaskScheduler(SchedulerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{
}

TaskScheduler::~TaskScheduler() = default;

Result<ScheduledTask> TaskScheduler::scheduleAt(
    TimePoint when,
    Task task,
    ScheduleOptions options)
{
    if (!task)
        return Status::invalidArgument("scheduled task callback cannot be empty");
    if (options.cancellation_.cancelled())
        return Status::cancelled(options.cancellation_.reason());

    auto record = impl_->makeTask(std::move(options.name_), "delayed");
    auto cancellation = options.cancellation_;
    auto entry = [scheduler = impl_->state_, record, task = std::move(task), cancellation]() mutable {
        dispatchOnce(scheduler, record, std::move(task), std::move(cancellation));
    };
    const auto status = scheduleEntry(impl_->state_, when, record, std::move(entry));
    if (!status.isOk())
        return status;

    ScheduledTask scheduled(record, impl_->state_);
    attachCancellation(impl_->state_, record, cancellation);
    return scheduled;
}

Result<ScheduledTask> TaskScheduler::scheduleAfter(
    Duration delay,
    Task task,
    ScheduleOptions options)
{
    return scheduleAt(dueAfter(*impl_->state_->clock_, delay), std::move(task), std::move(options));
}

Result<ScheduledTask> TaskScheduler::schedulePeriodic(
    Duration interval,
    Task task,
    PeriodicScheduleOptions options)
{
    if (!task)
        return Status::invalidArgument("periodic task callback cannot be empty");
    if (interval <= Duration::zero())
        return Status::invalidArgument("periodic task interval must be positive");
    if (options.maxRuns_.has_value() && *options.maxRuns_ == 0U)
        return Status::invalidArgument("periodic task maxRuns must be greater than zero");
    if (options.cancellation_.cancelled())
        return Status::cancelled(options.cancellation_.reason());

    auto record = impl_->makeTask(std::move(options.name_), "periodic");
    auto runs = std::make_shared<std::uint64_t>(0);
    const auto firstDue = options.startImmediately_
        ? clockNow(*impl_->state_->clock_)
        : dueAfter(*impl_->state_->clock_, interval);
    auto nextDue = std::make_shared<TimePoint>(firstDue);
    auto cancellation = options.cancellation_;
    auto entry = [scheduler = impl_->state_, record, task = std::move(task), cancellation, interval, maxRuns = options.maxRuns_, runs, nextDue, mode = options.mode_]() mutable {
        dispatchPeriodic(scheduler, record, std::move(task), std::move(cancellation), interval, maxRuns, runs, nextDue, mode);
    };
    const auto status = scheduleEntry(impl_->state_, firstDue, record, std::move(entry));
    if (!status.isOk())
        return status;

    ScheduledTask scheduled(record, impl_->state_);
    attachCancellation(impl_->state_, record, cancellation);
    return scheduled;
}

Result<ScheduledTask> TaskScheduler::scheduleRetry(
    RetryOperation operation,
    RetryCompletion completion,
    RetryScheduleOptions options)
{
    if (!operation)
        return Status::invalidArgument("retry operation cannot be empty");
    if (auto status = options.policy_.validate(); !status.isOk())
        return status;
    if (options.cancellation_.cancelled())
        return Status::cancelled(options.cancellation_.reason());

    auto record = impl_->makeTask(std::move(options.name_), "retry");

    auto retry = std::make_shared<RetryTask>();
    retry->policy_ = std::move(options.policy_);
    retry->operation_ = std::move(operation);
    retry->completion_ = std::move(completion);
    retry->cancellation_ = options.cancellation_;

    auto entry = [scheduler = impl_->state_, record, retry]() mutable {
        dispatchRetry(scheduler, record, std::move(retry));
    };
    const auto due = options.runImmediately_
        ? clockNow(*impl_->state_->clock_)
        : dueAfter(*impl_->state_->clock_, options.initialDelay_);
    const auto status = scheduleEntry(impl_->state_, due, record, std::move(entry));
    if (!status.isOk())
        return status;

    ScheduledTask scheduled(record, impl_->state_);
    attachCancellation(impl_->state_, record, retry->cancellation_);
    return scheduled;
}

Result<ScheduledTask> TaskScheduler::cancelAfter(
    Duration timeout,
    CancellationSource& source,
    std::string reason,
    ScheduleOptions options)
{
    auto task = [source, reason = std::move(reason)]() mutable {
        source.cancel(reason);
    };
    return scheduleAfter(timeout, std::move(task), std::move(options));
}

Status TaskScheduler::waitIdle(Duration timeout)
{
    return impl_->waitIdle(timeout);
}

Status TaskScheduler::close(Duration waitIdleTimeout)
{
    return impl_->close(SchedulerCloseOptions {
        .timeout_ = waitIdleTimeout,
        .policy_ = impl_->state_->closePolicy_,
    });
}

Status TaskScheduler::close(SchedulerCloseOptions options)
{
    return impl_->close(options);
}

bool TaskScheduler::isClosed() const noexcept
{
    return impl_->isClosed();
}

} // namespace lgc
