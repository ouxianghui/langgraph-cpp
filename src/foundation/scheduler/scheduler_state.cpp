#include "foundation/scheduler/scheduler_state.hh"

#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

namespace lc::scheduler_detail {

[[nodiscard]] TimePoint clockNow(const Clock& clock) noexcept
{
    return clock.now();
}

[[nodiscard]] TimePoint dueAfter(const Clock& clock, Duration delay) noexcept
{
    if (delay <= Duration::zero())
        return clockNow(clock);
    return clockNow(clock) + delay;
}

[[nodiscard]] Status currentExceptionStatus(std::string_view fallback)
{
    try {
        throw;
    } catch (const OperationInterrupted& error) {
        return error.status();
    } catch (const std::exception& error) {
        std::string message(fallback);
        message.append(": ");
        message.append(error.what());
        return Status::unknown(std::move(message));
    } catch (...) {
        return Status::unknown(std::string(fallback));
    }
}

[[nodiscard]] bool isTerminal(ScheduledTaskState state) noexcept
{
    return state == ScheduledTaskState::Completed
        || state == ScheduledTaskState::Failed
        || state == ScheduledTaskState::Cancelled;
}

[[nodiscard]] std::string taskName(const detail::TaskRecord& state)
{
    return state.name_.empty() ? state.kind_ : state.name_;
}

[[nodiscard]] SchedulerEvent makeSchedulerEvent(
    const std::shared_ptr<detail::TaskRecord>& state,
    SchedulerEventType type,
    Status status,
    std::uint32_t attempt)
{
    std::lock_guard lock(state->mutex_);
    return SchedulerEvent {
        .type_ = type,
        .taskId_ = state->id_,
        .taskName_ = taskName(*state),
        .taskState_ = state->state_,
        .attempt_ = attempt,
        .status_ = std::move(status),
        .timestamp_ = std::chrono::system_clock::now(),
    };
}

void publish(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    SchedulerEvent event) noexcept
{
    std::shared_ptr<ISchedulerEventSink> sink;
    {
        std::lock_guard lock(scheduler->mutex_);
        sink = scheduler->eventSink_;
    }
    if (!sink)
        return;
    try {
        (void)sink->publish(std::move(event));
    } catch (...) {
    }
}

[[nodiscard]] MetricTags metricTags(const detail::TaskRecord& state)
{
    MetricTags tags {
        MetricTag { .key_ = "kind", .value_ = state.kind_ },
    };
    if (!state.name_.empty()) {
        tags.push_back(MetricTag {
            .key_ = "name",
            .value_ = state.name_,
        });
    }
    return tags;
}

void countMetric(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    std::string name,
    const detail::TaskRecord& state) noexcept
{
    std::shared_ptr<IMetricRecorder> recorder;
    {
        std::lock_guard lock(scheduler->mutex_);
        recorder = scheduler->metricsRecorder_;
    }
    if (!recorder)
        return;
    try {
        (void)recorder->incrementCounter(std::move(name), 1.0, metricTags(state));
    } catch (...) {
    }
}

void recordDuration(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const detail::TaskRecord& state,
    Duration duration) noexcept
{
    std::shared_ptr<IMetricRecorder> recorder;
    {
        std::lock_guard lock(scheduler->mutex_);
        recorder = scheduler->metricsRecorder_;
    }
    if (!recorder)
        return;
    try {
        (void)recorder->recordDuration("scheduler.task.duration", duration, metricTags(state));
    } catch (...) {
    }
}

void recordSpan(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const detail::TaskRecord& state,
    Duration duration,
    SpanStatus spanStatus,
    const Status& status) noexcept
{
    std::shared_ptr<ITraceSink> sink;
    {
        std::lock_guard lock(scheduler->mutex_);
        sink = scheduler->traceSink_;
    }
    if (!sink)
        return;

    try {
        const auto endedAt = clockNow(*scheduler->clock_);
        const auto startedAt = endedAt - duration;
        auto context = makeRootContext();
        if (!context.isOk())
            return;
        (void)sink->recordSpan(SpanRecord {
            .context_ = std::move(*context),
            .name_ = "scheduler." + state.kind_,
            .attributes_ = nlohmann::json {
                { "task.id", state.id_ },
                { "task.name", taskName(state) },
                { "task.state", taskStateName(state.state_) },
            },
            .startedAt_ = startedAt,
            .endedAt_ = endedAt,
            .status_ = spanStatus,
            .statusMessage_ = status.isOk() ? std::string() : status.toString(),
        });
    } catch (...) {
    }
}

[[nodiscard]] bool startTask(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const std::shared_ptr<detail::TaskRecord>& state)
{
    {
        std::scoped_lock lock(scheduler->mutex_, state->mutex_);
        if (!state->active_ || state->cancelRequested_ || isTerminal(state->state_))
            return false;
        state->running_ = true;
        state->state_ = ScheduledTaskState::Running;
        state->startedAt_ = clockNow(*scheduler->clock_);
    }
    publish(scheduler, makeSchedulerEvent(state, SchedulerEventType::Started));
    countMetric(scheduler, "scheduler.task.started", *state);
    return true;
}

void finishTask(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const std::shared_ptr<detail::TaskRecord>& state,
    ScheduledTaskState finalState,
    Status status,
    std::uint32_t attempt) noexcept
{
    if (finalState == ScheduledTaskState::Completed && !status.isOk())
        finalState = ScheduledTaskState::Failed;
    if (finalState == ScheduledTaskState::Cancelled && status.isOk())
        status = Status::cancelled("scheduled task cancelled");

    Duration duration = Duration::zero();
    {
        std::scoped_lock lock(scheduler->mutex_, state->mutex_);
        state->running_ = false;
        if (!state->active_)
            return;

        state->state_ = finalState;
        state->status_ = status;
        state->active_ = false;
        state->cancellationRegistration_.unregister();
        if (state->startedAt_ != TimePoint {})
            duration = clockNow(*scheduler->clock_) - state->startedAt_;
        if (scheduler->active_ > 0)
            --scheduler->active_;
    }
    const auto eventType = finalState == ScheduledTaskState::Cancelled
        ? SchedulerEventType::Cancelled
        : (finalState == ScheduledTaskState::Failed ? SchedulerEventType::Failed : SchedulerEventType::Completed);
    auto event = makeSchedulerEvent(state, eventType, status, attempt);
    publish(scheduler, event);
    if (!status.isOk()) {
        logTo(scheduler->logger_,
            LogLevel::Warn,
            "TaskScheduler",
            "task finished with failure id={} name={} state={} attempt={} status={}",
            __FILE__,
            __LINE__,
            event.taskId_,
            event.taskName_,
            taskStateName(event.taskState_),
            attempt,
            status.toString());
    }
    countMetric(scheduler, finalState == ScheduledTaskState::Cancelled
            ? "scheduler.task.cancelled"
            : (finalState == ScheduledTaskState::Failed ? "scheduler.task.failed" : "scheduler.task.completed"),
        *state);
    recordDuration(scheduler, *state, duration);
    recordSpan(scheduler, *state, duration, finalState == ScheduledTaskState::Cancelled
            ? SpanStatus::Cancelled
            : (finalState == ScheduledTaskState::Failed ? SpanStatus::Error : SpanStatus::Ok),
        status);
    scheduler->idleCv_.notify_all();
    scheduler->wakeCv_.notify_all();
}

[[nodiscard]] bool requestCancel(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const std::shared_ptr<detail::TaskRecord>& state,
    Status status) noexcept
{
    if (status.isOk())
        status = Status::cancelled("scheduled task cancelled");

    bool completedNow = false;
    {
        std::scoped_lock lock(scheduler->mutex_, state->mutex_);
        if (!state->active_ || isTerminal(state->state_))
            return false;

        state->cancelRequested_ = true;
        if (!state->running_) {
            state->state_ = ScheduledTaskState::Cancelled;
            state->status_ = status;
            state->active_ = false;
            state->cancellationRegistration_.unregister();
            if (scheduler->active_ > 0)
                --scheduler->active_;
            completedNow = true;
        } else {
            state->state_ = ScheduledTaskState::Cancelling;
        }
    }

    if (completedNow) {
        publish(scheduler, makeSchedulerEvent(state, SchedulerEventType::Cancelled, status));
        countMetric(scheduler, "scheduler.task.cancelled", *state);
        scheduler->idleCv_.notify_all();
    }
    scheduler->wakeCv_.notify_all();
    return true;
}

[[nodiscard]] bool reschedule(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const std::shared_ptr<detail::TaskRecord>& state,
    TimePoint due,
    std::function<void()> fire)
{
    {
        std::scoped_lock lock(scheduler->mutex_, state->mutex_);
        state->running_ = false;
        if (scheduler->closed_ || !state->active_ || state->cancelRequested_) {
            if (state->active_) {
                state->state_ = ScheduledTaskState::Cancelled;
                state->status_ = Status::cancelled("scheduler closed");
                state->cancellationRegistration_.unregister();
                state->active_ = false;
                if (scheduler->active_ > 0)
                    --scheduler->active_;
            }
            scheduler->idleCv_.notify_all();
            scheduler->wakeCv_.notify_all();
            return false;
        }

        state->state_ = ScheduledTaskState::Pending;
        scheduler->queue_.push(detail::SchedulerState::ScheduledEntry {
            .due_ = due,
            .sequence_ = scheduler->nextEntryId_++,
            .task_ = state,
            .run_ = std::move(fire),
        });
    }
    scheduler->wakeCv_.notify_all();
    return true;
}

[[nodiscard]] Status scheduleEntry(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    TimePoint due,
    std::shared_ptr<detail::TaskRecord> state,
    std::function<void()> fire)
{
    bool closed = false;
    {
        std::lock_guard lock(scheduler->mutex_);
        if (scheduler->closed_) {
            closed = true;
        } else {
            scheduler->queue_.push(detail::SchedulerState::ScheduledEntry {
                .due_ = due,
                .sequence_ = scheduler->nextEntryId_++,
                .task_ = state,
                .run_ = std::move(fire),
            });
            ++scheduler->active_;
        }
    }

    if (closed) {
        const auto failure = Status::failedPrecondition("scheduler is closed");
        {
            std::lock_guard stateLock(state->mutex_);
            state->state_ = ScheduledTaskState::Failed;
            state->status_ = failure;
            state->active_ = false;
        }
        auto event = makeSchedulerEvent(state, SchedulerEventType::Failed, failure);
        logTo(scheduler->logger_,
            LogLevel::Warn,
            "TaskScheduler",
            "schedule rejected id={} name={} status={}",
            __FILE__,
            __LINE__,
            event.taskId_,
            event.taskName_,
            event.status_.toString());
        return event.status_;
    }
    scheduler->wakeCv_.notify_all();
    publish(scheduler, makeSchedulerEvent(state, SchedulerEventType::Scheduled));
    countMetric(scheduler, "scheduler.task.scheduled", *state);
    return Status::ok();
}

void attachCancellation(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const std::shared_ptr<detail::TaskRecord>& state,
    const CancellationToken& token)
{
    if (!token.cancellable())
        return;

    auto registration = token.onCancel([weakScheduler = std::weak_ptr<detail::SchedulerState>(scheduler),
                                                   weakState = std::weak_ptr<detail::TaskRecord>(state),
                                                   token] {
        auto lockedScheduler = weakScheduler.lock();
        auto lockedState = weakState.lock();
        if (lockedScheduler && lockedState)
            (void)requestCancel(lockedScheduler, lockedState, Status::cancelled(token.reason()));
    });

    if (!registration.registered())
        return;

    std::lock_guard lock(state->mutex_);
    if (state->active_ && !state->cancelRequested_)
        state->cancellationRegistration_ = std::move(registration);
}

[[nodiscard]] Status postToExecutor(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    IExecutor::Task task)
{
    std::shared_ptr<IExecutor> executor;
    {
        std::lock_guard lock(scheduler->mutex_);
        executor = scheduler->executor_;
    }

    if (!executor)
        return Status::failedPrecondition("scheduler executor is not configured");

    return executor->post(std::move(task));
}

} // namespace lc::scheduler_detail
