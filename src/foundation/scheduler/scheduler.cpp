#include "foundation/scheduler/scheduler.hpp"

namespace lc {

std::string_view taskStateName(ScheduledTaskState state) noexcept
{
    switch (state) {
    case ScheduledTaskState::Pending:
        return "pending";
    case ScheduledTaskState::Running:
        return "running";
    case ScheduledTaskState::Cancelling:
        return "cancelling";
    case ScheduledTaskState::Completed:
        return "completed";
    case ScheduledTaskState::Failed:
        return "failed";
    case ScheduledTaskState::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

std::string_view schedulerEventName(SchedulerEventType type) noexcept
{
    switch (type) {
    case SchedulerEventType::Scheduled:
        return "scheduled";
    case SchedulerEventType::Started:
        return "started";
    case SchedulerEventType::Completed:
        return "completed";
    case SchedulerEventType::Cancelled:
        return "cancelled";
    case SchedulerEventType::Failed:
        return "failed";
    case SchedulerEventType::Retrying:
        return "retrying";
    case SchedulerEventType::Closed:
        return "closed";
    }
    return "unknown";
}

std::string_view periodicModeName(PeriodicScheduleMode mode) noexcept
{
    switch (mode) {
    case PeriodicScheduleMode::FixedDelay:
        return "fixed_delay";
    case PeriodicScheduleMode::FixedRate:
        return "fixed_rate";
    }
    return "unknown";
}

std::string_view closePolicyName(SchedulerClosePolicy policy) noexcept
{
    switch (policy) {
    case SchedulerClosePolicy::CancelPending:
        return "cancel_pending";
    case SchedulerClosePolicy::DrainPending:
        return "drain_pending";
    }
    return "unknown";
}

} // namespace lc
