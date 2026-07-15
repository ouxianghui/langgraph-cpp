#pragma once

#include <chrono>
#include <mutex>

namespace lgc {

/// Monotonic time source used for deadlines, retries, timeouts, and tests.
///
/// This intentionally uses `std::chrono::steady_clock` semantics. Wall-clock timestamps should use
/// `std::chrono::system_clock` at the serialization/storage boundary where real calendar time is
/// needed.
class Clock {
public:
    using Duration = std::chrono::steady_clock::duration;
    using TimePoint = std::chrono::steady_clock::time_point;

    virtual ~Clock() = default;

    [[nodiscard]] virtual TimePoint now() const noexcept = 0;
};

class SteadyClock final : public Clock {
public:
    [[nodiscard]] static const SteadyClock& instance() noexcept;

    [[nodiscard]] TimePoint now() const noexcept override;
};

/// Controllable monotonic clock for deterministic tests.
class ManualClock final : public Clock {
public:
    explicit ManualClock(TimePoint initial = TimePoint(Duration::zero())) noexcept;

    [[nodiscard]] TimePoint now() const noexcept override;

    void set(TimePoint value) noexcept;
    void advance(Duration delta) noexcept;

private:
    mutable std::mutex mutex_;
    TimePoint now_;
};

/// Wall-clock source for expiry timestamps and external protocol dates.
class WallClock {
public:
    using Duration = std::chrono::system_clock::duration;
    using TimePoint = std::chrono::system_clock::time_point;

    virtual ~WallClock() = default;

    [[nodiscard]] virtual TimePoint now() const noexcept = 0;
};

class SystemWallClock final : public WallClock {
public:
    [[nodiscard]] static const SystemWallClock& instance() noexcept;

    [[nodiscard]] TimePoint now() const noexcept override;
};

class ManualWallClock final : public WallClock {
public:
    explicit ManualWallClock(TimePoint initial = TimePoint(Duration::zero())) noexcept;

    [[nodiscard]] TimePoint now() const noexcept override;

    void set(TimePoint value) noexcept;
    void advance(Duration delta) noexcept;

private:
    mutable std::mutex mutex_;
    TimePoint now_;
};

} // namespace lgc
