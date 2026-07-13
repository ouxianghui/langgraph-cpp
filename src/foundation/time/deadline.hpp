#pragma once

#include "foundation/status/status.hpp"
#include "foundation/time/clock.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace lc {

/// Optional monotonic deadline.
///
/// `Deadline::none()` means no timeout. Expiration and remaining-time calculations are evaluated
/// against an injected `Clock`, which keeps graph/runtime tests deterministic.
class Deadline final {
public:
    using Duration = Clock::Duration;
    using TimePoint = Clock::TimePoint;

    [[nodiscard]] static Deadline none() noexcept;
    [[nodiscard]] static Deadline at(TimePoint timePoint) noexcept;
    [[nodiscard]] static Deadline after(const Clock& clock, Duration timeout) noexcept;
    [[nodiscard]] static Deadline fromTimeout(
        const Clock& clock,
        std::optional<Duration> timeout) noexcept;

    [[nodiscard]] bool hasDeadline() const noexcept;
    [[nodiscard]] const std::optional<TimePoint>& timePoint() const noexcept;

    [[nodiscard]] bool isExpired(const Clock& clock) const noexcept;
    [[nodiscard]] Duration remaining(const Clock& clock) const noexcept;
    [[nodiscard]] Status statusIfExpired(
        const Clock& clock,
        std::string message = "deadline exceeded") const;

    friend bool operator==(const Deadline&, const Deadline&) = default;

private:
    explicit Deadline(std::optional<TimePoint> timePoint) noexcept;

    std::optional<TimePoint> timePoint_;
};

} // namespace lc
