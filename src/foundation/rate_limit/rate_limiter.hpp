#pragma once

#include "foundation/status/status.hpp"
#include "foundation/time/clock.hpp"

#include <cstdint>
#include <mutex>

namespace lc {

struct RateLimitPolicy {
    std::uint64_t capacity_ { 1 };
    std::uint64_t refill_ { 1 };
    Clock::Duration interval_ { std::chrono::seconds(1) };

    [[nodiscard]] static RateLimitPolicy perSecond(std::uint64_t rate, std::uint64_t burst = 0) noexcept;
    [[nodiscard]] Status validate() const;
};

struct RateLimitResult {
    bool allowed_ { false };
    Clock::Duration retryAfter_ { Clock::Duration::zero() };
    std::uint64_t remaining_ { 0 };
    Status status_ { Status::resourceExhausted("rate limit exceeded") };
};

class IRateLimiter {
public:
    virtual ~IRateLimiter() = default;

    [[nodiscard]] virtual RateLimitResult acquire(std::uint64_t permits = 1) noexcept = 0;
    virtual void reset() noexcept = 0;
};

class TokenBucketRateLimiter final : public IRateLimiter {
public:
    explicit TokenBucketRateLimiter(
        RateLimitPolicy policy,
        const Clock& clock = SteadyClock::instance());

    [[nodiscard]] RateLimitResult acquire(std::uint64_t permits = 1) noexcept override;
    void reset() noexcept override;

    [[nodiscard]] RateLimitPolicy policy() const noexcept;
    [[nodiscard]] std::uint64_t available() const noexcept;

private:
    void refill(Clock::TimePoint now) noexcept;
    [[nodiscard]] Clock::Duration retryAfter(std::uint64_t permits, Clock::TimePoint now) const noexcept;

    RateLimitPolicy policy_;
    const Clock* clock_;
    mutable std::mutex mutex_;
    std::uint64_t tokens_;
    Clock::TimePoint updatedAt_;
};

} // namespace lc
