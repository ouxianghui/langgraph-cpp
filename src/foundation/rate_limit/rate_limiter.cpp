#include "foundation/rate_limit/rate_limiter.hpp"

#include <algorithm>
#include <limits>

namespace lc {
namespace {

[[nodiscard]] std::uint64_t intervalsForTokens(std::uint64_t tokens, std::uint64_t refill) noexcept
{
    if (tokens == 0 || refill == 0)
        return 0;
    return (tokens + refill - 1) / refill;
}

} // namespace

RateLimitPolicy RateLimitPolicy::perSecond(std::uint64_t rate, std::uint64_t burst) noexcept
{
    return RateLimitPolicy {
        .capacity_ = burst == 0 ? rate : burst,
        .refill_ = rate,
        .interval_ = std::chrono::seconds(1),
    };
}

Status RateLimitPolicy::validate() const
{
    if (capacity_ == 0)
        return Status::invalidArgument("rate limit capacity must be greater than 0");
    if (refill_ == 0)
        return Status::invalidArgument("rate limit refill must be greater than 0");
    if (interval_ <= Clock::Duration::zero())
        return Status::invalidArgument("rate limit interval must be positive");
    return Status::ok();
}

TokenBucketRateLimiter::TokenBucketRateLimiter(RateLimitPolicy policy, const Clock& clock)
    : policy_(policy)
    , clock_(&clock)
    , tokens_(policy.capacity_)
    , updatedAt_(clock.now())
{
}

RateLimitResult TokenBucketRateLimiter::acquire(std::uint64_t permits) noexcept
{
    std::lock_guard lock(mutex_);

    if (auto status = policy_.validate(); !status.isOk()) {
        return RateLimitResult {
            .allowed_ = false,
            .retryAfter_ = Clock::Duration::zero(),
            .remaining_ = tokens_,
            .status_ = status,
        };
    }
    if (permits == 0) {
        return RateLimitResult {
            .allowed_ = true,
            .retryAfter_ = Clock::Duration::zero(),
            .remaining_ = tokens_,
            .status_ = Status::ok(),
        };
    }
    if (permits > policy_.capacity_) {
        return RateLimitResult {
            .allowed_ = false,
            .retryAfter_ = Clock::Duration::max(),
            .remaining_ = tokens_,
            .status_ = Status::resourceExhausted("requested permits exceed rate limit capacity"),
        };
    }

    const auto now = clock_->now();
    refill(now);
    if (tokens_ >= permits) {
        tokens_ -= permits;
        return RateLimitResult {
            .allowed_ = true,
            .retryAfter_ = Clock::Duration::zero(),
            .remaining_ = tokens_,
            .status_ = Status::ok(),
        };
    }

    return RateLimitResult {
        .allowed_ = false,
        .retryAfter_ = retryAfter(permits, now),
        .remaining_ = tokens_,
        .status_ = Status::resourceExhausted("rate limit exceeded"),
    };
}

void TokenBucketRateLimiter::reset() noexcept
{
    std::lock_guard lock(mutex_);
    tokens_ = policy_.capacity_;
    updatedAt_ = clock_->now();
}

RateLimitPolicy TokenBucketRateLimiter::policy() const noexcept
{
    std::lock_guard lock(mutex_);
    return policy_;
}

std::uint64_t TokenBucketRateLimiter::available() const noexcept
{
    std::lock_guard lock(mutex_);
    return tokens_;
}

void TokenBucketRateLimiter::refill(Clock::TimePoint now) noexcept
{
    if (now <= updatedAt_)
        return;

    const auto elapsed = now - updatedAt_;
    const auto elapsedIntervals = elapsed / policy_.interval_;
    if (elapsedIntervals <= 0)
        return;

    const auto add = static_cast<std::uint64_t>(elapsedIntervals) > std::numeric_limits<std::uint64_t>::max() / policy_.refill_
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(elapsedIntervals) * policy_.refill_;
    if (add >= policy_.capacity_ || tokens_ > policy_.capacity_ - add)
        tokens_ = policy_.capacity_;
    else
        tokens_ += add;
    updatedAt_ += policy_.interval_ * elapsedIntervals;
}

Clock::Duration TokenBucketRateLimiter::retryAfter(std::uint64_t permits, Clock::TimePoint now) const noexcept
{
    if (tokens_ >= permits)
        return Clock::Duration::zero();

    const auto intervals = intervalsForTokens(permits - tokens_, policy_.refill_);
    const auto maxCount = std::numeric_limits<Clock::Duration::rep>::max();
    if (policy_.interval_.count() > 0 && intervals > static_cast<std::uint64_t>(maxCount / policy_.interval_.count()))
        return Clock::Duration::max();

    const auto readyAt = updatedAt_ + policy_.interval_ * intervals;
    if (readyAt <= now)
        return Clock::Duration::zero();
    return readyAt - now;
}

} // namespace lc
