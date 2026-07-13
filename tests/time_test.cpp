#include "foundation/time/clock.hpp"
#include "foundation/time/deadline.hpp"

#include <cassert>
#include <chrono>

int main()
{
    using namespace std::chrono_literals;

    lc::ManualClock clock;
    assert(clock.now() == lc::Clock::TimePoint(lc::Clock::Duration::zero()));

    clock.advance(10ms);
    assert(clock.now() == lc::Clock::TimePoint(10ms));

    const auto noDeadline = lc::Deadline::none();
    assert(!noDeadline.hasDeadline());
    assert(!noDeadline.isExpired(clock));
    assert(noDeadline.remaining(clock) == lc::Clock::Duration::max());
    assert(noDeadline.statusIfExpired(clock).isOk());

    const auto noTimeout = lc::Deadline::fromTimeout(clock, std::nullopt);
    assert(!noTimeout.hasDeadline());

    const auto deadline = lc::Deadline::after(clock, 50ms);
    assert(deadline.hasDeadline());
    assert(!deadline.isExpired(clock));
    assert(deadline.remaining(clock) == 50ms);

    clock.advance(49ms);
    assert(!deadline.isExpired(clock));
    assert(deadline.remaining(clock) == 1ms);

    clock.advance(1ms);
    assert(deadline.isExpired(clock));
    assert(deadline.remaining(clock) == 0ms);
    const auto status = deadline.statusIfExpired(clock, "graph run timed out");
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::DeadlineExceeded);

    const auto immediate = lc::Deadline::after(clock, 0ms);
    assert(immediate.isExpired(clock));

    const auto fromTimeout = lc::Deadline::fromTimeout(clock, std::optional<lc::Clock::Duration>(5ms));
    assert(fromTimeout.hasDeadline());
    assert(fromTimeout.remaining(clock) == 5ms);

    clock.set(lc::Clock::TimePoint(100ms));
    const auto absolute = lc::Deadline::at(lc::Clock::TimePoint(110ms));
    assert(absolute.remaining(clock) == 10ms);

    const auto& steady = lc::SteadyClock::instance();
    assert(steady.now().time_since_epoch().count() > 0);

    lc::ManualWallClock wallClock(lc::WallClock::TimePoint(std::chrono::seconds(10)));
    assert(wallClock.now() == lc::WallClock::TimePoint(std::chrono::seconds(10)));
    wallClock.advance(5s);
    assert(wallClock.now() == lc::WallClock::TimePoint(std::chrono::seconds(15)));
    wallClock.set(lc::WallClock::TimePoint(std::chrono::seconds(20)));
    assert(wallClock.now() == lc::WallClock::TimePoint(std::chrono::seconds(20)));

    const auto& systemWall = lc::SystemWallClock::instance();
    assert(systemWall.now().time_since_epoch().count() > 0);

    return 0;
}
