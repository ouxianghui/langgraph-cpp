#pragma once

#include "foundation/network/http_client_types.hpp"

#include <algorithm>
#include <chrono>

namespace lc::http_client_detail {

[[nodiscard]] inline Deadline earliestDeadline(Deadline lhs, Deadline rhs) noexcept
{
    if (!lhs.hasDeadline())
        return rhs;
    if (!rhs.hasDeadline())
        return lhs;
    return *lhs.timePoint() <= *rhs.timePoint() ? lhs : rhs;
}

[[nodiscard]] inline Result<HttpRequestOptions> normalizeRequestOptions(
    HttpRequestOptions options,
    const Clock& clock)
{
    if (auto status = options.validate(); !status.isOk())
        return status;

    if (options.timeout_.has_value()) {
        options.deadline_ = earliestDeadline(
            options.deadline_,
            Deadline::after(clock, *options.timeout_));
        options.timeout_.reset();
    }
    return options;
}

[[nodiscard]] inline std::chrono::milliseconds ceilMilliseconds(Clock::Duration duration) noexcept
{
    if (duration <= Clock::Duration::zero())
        return std::chrono::milliseconds::zero();

    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    if (std::chrono::duration_cast<Clock::Duration>(millis) < duration
        && millis < std::chrono::milliseconds::max()) {
        ++millis;
    }
    if (millis == std::chrono::milliseconds::zero())
        return std::chrono::milliseconds(1);
    return millis;
}

[[nodiscard]] inline std::chrono::milliseconds capTimeout(
    std::chrono::milliseconds configured,
    Clock::Duration remaining) noexcept
{
    const auto remainingMs = ceilMilliseconds(remaining);
    if (remainingMs <= std::chrono::milliseconds::zero())
        return std::chrono::milliseconds(1);
    if (configured <= std::chrono::milliseconds::zero())
        return remainingMs;
    return std::min(configured, remainingMs);
}

[[nodiscard]] inline Status requestDeadlineStatus(const Deadline& deadline, const Clock& clock)
{
    return deadline.statusIfExpired(clock, "HTTP request deadline exceeded");
}

} // namespace lc::http_client_detail
