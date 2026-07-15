#pragma once

#include "foundation/network/http_client_types.hpp"

#include <httplib.h>

#include <algorithm>
#include <chrono>

namespace lgc::http_client_detail {

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

/// Prefer `DeadlineExceeded` when a request deadline explains a transport failure.
///
/// After a successful write, cpp-httplib rewrites response-wait timeouts to
/// `Error::Read` ("Failed to read connection") instead of `Error::Timeout`.
/// Socket SO_RCVTIMEO / SO_SNDTIMEO failures also commonly surface as Read/Write.
/// When this request was deadline-bounded, treat those codes as timeouts.
[[nodiscard]] inline Status statusOrDeadline(
    httplib::Error err,
    Status transportStatus,
    const Deadline& deadline,
    const Clock& clock)
{
    switch (transportStatus.code()) {
    case StatusCode::Ok:
    case StatusCode::Cancelled:
    case StatusCode::ResourceExhausted:
    case StatusCode::DeadlineExceeded:
    case StatusCode::InvalidArgument:
    case StatusCode::FailedPrecondition:
    case StatusCode::Unauthenticated:
        return transportStatus;
    default:
        break;
    }

    if (auto deadlineStatus = requestDeadlineStatus(deadline, clock); !deadlineStatus.isOk())
        return deadlineStatus;

    const bool timeoutLike = err == httplib::Error::Read
        || err == httplib::Error::Write
        || err == httplib::Error::Timeout
        || err == httplib::Error::ConnectionTimeout;
    if (deadline.hasDeadline() && timeoutLike)
        return Status::deadlineExceeded("HTTP request timed out");
    return transportStatus;
}

} // namespace lgc::http_client_detail
