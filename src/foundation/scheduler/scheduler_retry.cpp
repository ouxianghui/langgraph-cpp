#include "foundation/scheduler/scheduler_state.hh"

#include <utility>

namespace lc::scheduler_detail {

void finishRetry(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const std::shared_ptr<detail::TaskRecord>& state,
    const std::shared_ptr<RetryTask>& retry,
    Status finalStatus,
    ScheduledTaskState finalState)
{
    if (retry->completion_) {
        try {
            retry->completion_(RetryReport {
                .attempts_ = retry->attempts_,
                .retries_ = retry->attempts_ == 0 ? 0U : retry->attempts_ - 1U,
                .status_ = finalStatus,
            });
        } catch (...) {
        }
    }
    finishTask(scheduler, state, finalState, finalStatus, retry->attempts_);
}

void dispatchRetry(
    const std::shared_ptr<detail::SchedulerState>& scheduler,
    const std::shared_ptr<detail::TaskRecord>& state,
    std::shared_ptr<RetryTask> retry)
{
    if (!startTask(scheduler, state))
        return;

    auto status = postToExecutor(scheduler, [scheduler, state, retry]() mutable {
        if (retry->cancellation_.cancelled()) {
            finishRetry(scheduler, state, retry, Status::cancelled(retry->cancellation_.reason()), ScheduledTaskState::Cancelled);
            return;
        }

        const auto attempt = ++retry->attempts_;
        Status result;
        try {
            result = retry->operation_(attempt);
        } catch (...) {
            result = currentExceptionStatus("retry operation failed");
        }

        if (result.isOk()) {
            finishRetry(scheduler, state, retry, Status::ok(), ScheduledTaskState::Completed);
            return;
        }

        auto decision = retry->policy_.decide(result, retry->attempts_);
        if (!decision.isOk()) {
            finishRetry(scheduler, state, retry, decision.status(), ScheduledTaskState::Failed);
            return;
        }
        if (!decision->retry_) {
            finishRetry(scheduler, state, retry, result, ScheduledTaskState::Completed);
            return;
        }

        publish(scheduler, makeSchedulerEvent(state, SchedulerEventType::Retrying, result, decision->nextAttempt_));
        countMetric(scheduler, "scheduler.task.retrying", *state);

        auto fire = [scheduler, state, retry]() mutable {
            dispatchRetry(scheduler, state, std::move(retry));
        };
        (void)reschedule(scheduler, state, dueAfter(*scheduler->clock_, decision->delay_), std::move(fire));
    });

    if (!status.isOk()) {
        auto failure = status;
        finishRetry(scheduler, state, retry, failure, ScheduledTaskState::Failed);
    }
}

} // namespace lc::scheduler_detail
