#include "foundation/time/clock.hpp"

#include <chrono>

namespace lgc {

const SteadyClock& SteadyClock::instance() noexcept
{
    static const SteadyClock clock;
    return clock;
}

Clock::TimePoint SteadyClock::now() const noexcept
{
    return std::chrono::steady_clock::now();
}

ManualClock::ManualClock(TimePoint initial) noexcept
    : now_(initial)
{
}

Clock::TimePoint ManualClock::now() const noexcept
{
    std::lock_guard lock(mutex_);
    return now_;
}

void ManualClock::set(TimePoint value) noexcept
{
    std::lock_guard lock(mutex_);
    now_ = value;
}

void ManualClock::advance(Duration delta) noexcept
{
    std::lock_guard lock(mutex_);
    now_ += delta;
}

const SystemWallClock& SystemWallClock::instance() noexcept
{
    static const SystemWallClock clock;
    return clock;
}

WallClock::TimePoint SystemWallClock::now() const noexcept
{
    return std::chrono::system_clock::now();
}

ManualWallClock::ManualWallClock(TimePoint initial) noexcept
    : now_(initial)
{
}

WallClock::TimePoint ManualWallClock::now() const noexcept
{
    std::lock_guard lock(mutex_);
    return now_;
}

void ManualWallClock::set(TimePoint value) noexcept
{
    std::lock_guard lock(mutex_);
    now_ = value;
}

void ManualWallClock::advance(Duration delta) noexcept
{
    std::lock_guard lock(mutex_);
    now_ += delta;
}

} // namespace lgc
