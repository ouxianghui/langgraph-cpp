#include "foundation/rate_limit/circuit_breaker.hpp"
#include "foundation/rate_limit/rate_limiter.hpp"
#include "foundation/status/status.hpp"
#include "foundation/time/clock.hpp"

#include <cassert>
#include <chrono>
#include <limits>
#include <string_view>

int main()
{
    using namespace std::chrono_literals;

    lc::ManualClock clock;

    {
        auto policy = lc::RateLimitPolicy {
            .capacity_ = 2,
            .refill_ = 1,
            .interval_ = 1s,
        };
        assert(policy.validate().isOk());

        lc::TokenBucketRateLimiter limiter(policy, clock);
        auto result = limiter.acquire();
        assert(result.allowed_);
        assert(result.status_.isOk());
        assert(result.remaining_ == 1);

        result = limiter.acquire();
        assert(result.allowed_);
        assert(result.remaining_ == 0);

        result = limiter.acquire();
        assert(!result.allowed_);
        assert(result.status_.code() == lc::StatusCode::ResourceExhausted);
        assert(result.retryAfter_ == 1s);

        clock.advance(500ms);
        result = limiter.acquire();
        assert(!result.allowed_);
        assert(result.retryAfter_ == 500ms);

        clock.advance(500ms);
        result = limiter.acquire();
        assert(result.allowed_);
        assert(result.remaining_ == 0);

        limiter.reset();
        assert(limiter.available() == 2);
    }

    {
        auto policy = lc::RateLimitPolicy::perSecond(3, 5);
        assert(policy.capacity_ == 5);
        assert(policy.refill_ == 3);
        assert(policy.interval_ == 1s);

        lc::TokenBucketRateLimiter limiter(policy, clock);
        auto result = limiter.acquire(6);
        assert(!result.allowed_);
        assert(result.retryAfter_ == lc::Clock::Duration::max());
    }

    {
        lc::ManualClock overflowClock;
        const auto max = std::numeric_limits<std::uint64_t>::max();
        lc::TokenBucketRateLimiter limiter(lc::RateLimitPolicy {
            .capacity_ = max,
            .refill_ = max,
            .interval_ = 1s,
        },
            overflowClock);

        auto result = limiter.acquire(1);
        assert(result.allowed_);
        overflowClock.advance(1s);
        result = limiter.acquire(max);
        assert(result.allowed_);
        assert(result.remaining_ == 0);
    }

    {
        auto policy = lc::CircuitBreakerPolicy {
            .failureThreshold_ = 2,
            .successThreshold_ = 1,
            .halfOpenMaxCalls_ = 1,
            .openTimeout_ = 2s,
        };
        assert(policy.validate().isOk());

        lc::CircuitBreaker breaker(policy, clock);
        auto result = breaker.acquire();
        assert(result.allowed_);
        assert(result.state_ == lc::CircuitState::Closed);

        breaker.recordFailure();
        assert(breaker.state() == lc::CircuitState::Closed);
        assert(breaker.consecutiveFailures() == 1);

        breaker.recordFailure();
        assert(breaker.state() == lc::CircuitState::Open);

        result = breaker.acquire();
        assert(!result.allowed_);
        assert(result.state_ == lc::CircuitState::Open);
        assert(result.status_.code() == lc::StatusCode::Unavailable);
        assert(result.retryAfter_ == 2s);

        clock.advance(2s);
        result = breaker.acquire();
        assert(result.allowed_);
        assert(result.state_ == lc::CircuitState::HalfOpen);

        auto secondProbe = breaker.acquire();
        assert(!secondProbe.allowed_);
        assert(secondProbe.state_ == lc::CircuitState::HalfOpen);

        breaker.recordSuccess();
        assert(breaker.state() == lc::CircuitState::Closed);
    }

    {
        auto policy = lc::CircuitBreakerPolicy {
            .failureThreshold_ = 1,
            .successThreshold_ = 1,
            .halfOpenMaxCalls_ = 1,
            .openTimeout_ = 1s,
        };
        lc::CircuitBreaker breaker(policy, clock);

        assert(breaker.acquire().allowed_);
        breaker.recordFailure();
        assert(breaker.state() == lc::CircuitState::Open);

        clock.advance(1s);
        assert(breaker.acquire().allowed_);
        assert(breaker.state() == lc::CircuitState::HalfOpen);
        breaker.recordFailure();
        assert(breaker.state() == lc::CircuitState::Open);

        breaker.reset();
        assert(breaker.state() == lc::CircuitState::Closed);
        assert(lc::circuitStateName(lc::CircuitState::HalfOpen) == std::string_view("half_open"));
    }

    {
        auto policy = lc::CircuitBreakerPolicy {
            .failureThreshold_ = 1,
            .successThreshold_ = 1,
            .halfOpenMaxCalls_ = 1,
            .openTimeout_ = 1s,
            .halfOpenProbeTimeout_ = 50ms,
        };
        lc::CircuitBreaker breaker(policy, clock);

        assert(breaker.acquire().allowed_);
        breaker.recordFailure();
        clock.advance(1s);
        assert(breaker.acquire().allowed_);

        auto blocked = breaker.acquire();
        assert(!blocked.allowed_);
        assert(blocked.retryAfter_ == 50ms);

        clock.advance(50ms);
        auto recovered = breaker.acquire();
        assert(recovered.allowed_);
        breaker.recordSuccess();
        assert(breaker.state() == lc::CircuitState::Closed);
    }

    return 0;
}
