#include "foundation/time/clock.hpp"
#include "foundation/time/deadline.hpp"

#include <cassert>
#include <chrono>

int main()
{
    using namespace std::chrono_literals;

    lgc::ManualClock clock;
    assert(clock.now() == lgc::Clock::TimePoint(lgc::Clock::Duration::zero()));

    clock.advance(10ms);
    assert(clock.now() == lgc::Clock::TimePoint(10ms));

    const auto noDeadline = lgc::Deadline::none();
    assert(!noDeadline.hasDeadline());
    assert(!noDeadline.isExpired(clock));
    assert(noDeadline.remaining(clock) == lgc::Clock::Duration::max());
    assert(noDeadline.statusIfExpired(clock).isOk());

    const auto noTimeout = lgc::Deadline::fromTimeout(clock, std::nullopt);
    assert(!noTimeout.hasDeadline());

    const auto deadline = lgc::Deadline::after(clock, 50ms);
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
    assert(status.code() == lgc::StatusCode::DeadlineExceeded);

    const auto immediate = lgc::Deadline::after(clock, 0ms);
    assert(immediate.isExpired(clock));

    const auto fromTimeout = lgc::Deadline::fromTimeout(clock, std::optional<lgc::Clock::Duration>(5ms));
    assert(fromTimeout.hasDeadline());
    assert(fromTimeout.remaining(clock) == 5ms);

    clock.set(lgc::Clock::TimePoint(100ms));
    const auto absolute = lgc::Deadline::at(lgc::Clock::TimePoint(110ms));
    assert(absolute.remaining(clock) == 10ms);

    const auto& steady = lgc::SteadyClock::instance();
    assert(steady.now().time_since_epoch().count() > 0);

    lgc::ManualWallClock wallClock(lgc::WallClock::TimePoint(std::chrono::seconds(10)));
    assert(wallClock.now() == lgc::WallClock::TimePoint(std::chrono::seconds(10)));
    wallClock.advance(5s);
    assert(wallClock.now() == lgc::WallClock::TimePoint(std::chrono::seconds(15)));
    wallClock.set(lgc::WallClock::TimePoint(std::chrono::seconds(20)));
    assert(wallClock.now() == lgc::WallClock::TimePoint(std::chrono::seconds(20)));

    const auto& systemWall = lgc::SystemWallClock::instance();
    assert(systemWall.now().time_since_epoch().count() > 0);

    return 0;
}
