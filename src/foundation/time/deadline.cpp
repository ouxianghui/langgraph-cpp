#include "foundation/time/deadline.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace lgc {

Deadline::Deadline(std::optional<TimePoint> timePoint) noexcept
    : timePoint_(std::move(timePoint))
{
}

Deadline Deadline::none() noexcept
{
    return Deadline(std::nullopt);
}

Deadline Deadline::at(TimePoint timePoint) noexcept
{
    return Deadline(timePoint);
}

Deadline Deadline::after(const Clock& clock, Duration timeout) noexcept
{
    if (timeout <= Duration::zero())
        return Deadline(clock.now());

    const auto now = clock.now();
    const auto maxDelta = TimePoint::max() - now;
    if (timeout > maxDelta)
        return Deadline(TimePoint::max());

    return Deadline(now + timeout);
}

Deadline Deadline::fromTimeout(const Clock& clock, std::optional<Duration> timeout) noexcept
{
    if (!timeout.has_value())
        return none();
    return after(clock, *timeout);
}

bool Deadline::hasDeadline() const noexcept
{
    return timePoint_.has_value();
}

const std::optional<Deadline::TimePoint>& Deadline::timePoint() const noexcept
{
    return timePoint_;
}

bool Deadline::isExpired(const Clock& clock) const noexcept
{
    return timePoint_.has_value() && clock.now() >= *timePoint_;
}

Deadline::Duration Deadline::remaining(const Clock& clock) const noexcept
{
    if (!timePoint_.has_value())
        return Duration::max();

    const auto now = clock.now();
    if (now >= *timePoint_)
        return Duration::zero();

    return *timePoint_ - now;
}

Status Deadline::statusIfExpired(const Clock& clock, std::string message) const
{
    if (!isExpired(clock))
        return Status::ok();
    return Status::deadlineExceeded(std::move(message));
}

} // namespace lgc
