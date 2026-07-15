#include "foundation/scheduler/scheduler.hpp"

#include "foundation/scheduler/scheduler_state.hh"

#include <stdexcept>
#include <utility>

namespace lgc {
namespace {
using scheduler_detail::isTerminal;
using scheduler_detail::requestCancel;
}

ScheduledTask::ScheduledTask(
    std::shared_ptr<detail::TaskRecord> record,
    std::weak_ptr<detail::SchedulerState> scheduler) noexcept
    : record_(std::move(record))
    , scheduler_(std::move(scheduler))
{
}

SchedulerCallbackSink::SchedulerCallbackSink(Callback callback)
    : callback_(std::move(callback))
{
    if (!callback_)
        throw std::invalid_argument("SchedulerCallbackSink requires a callback");
}

Status SchedulerCallbackSink::publish(SchedulerEvent event)
{
    return callback_(std::move(event));
}

bool ScheduledTask::valid() const noexcept
{
    return record_ != nullptr;
}

std::uint64_t ScheduledTask::id() const noexcept
{
    if (!record_)
        return 0;
    std::lock_guard lock(record_->mutex_);
    return record_->id_;
}

ScheduledTaskState ScheduledTask::state() const noexcept
{
    if (!record_)
        return ScheduledTaskState::Cancelled;
    std::lock_guard lock(record_->mutex_);
    return record_->state_;
}

Status ScheduledTask::status() const
{
    if (!record_)
        return Status::failedPrecondition("scheduled task is not valid");
    std::lock_guard lock(record_->mutex_);
    return record_->status_;
}

bool ScheduledTask::cancel() noexcept
{
    if (!record_)
        return false;
    auto scheduler = scheduler_.lock();
    if (!scheduler)
        return false;
    return requestCancel(scheduler, record_);
}

bool ScheduledTask::isCancelled() const noexcept
{
    return state() == ScheduledTaskState::Cancelled;
}

bool ScheduledTask::isFinished() const noexcept
{
    return isTerminal(state());
}

} // namespace lgc
