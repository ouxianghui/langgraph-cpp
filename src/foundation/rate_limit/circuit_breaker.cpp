#include "foundation/rate_limit/circuit_breaker.hpp"

#include <utility>

namespace lgc {

const char* circuitStateName(CircuitState state) noexcept
{
    switch (state) {
    case CircuitState::Closed:
        return "closed";
    case CircuitState::Open:
        return "open";
    case CircuitState::HalfOpen:
        return "half_open";
    }
    return "unknown";
}

Status CircuitBreakerPolicy::validate() const
{
    if (failureThreshold_ == 0)
        return Status::invalidArgument("circuit breaker failure threshold must be greater than 0");
    if (successThreshold_ == 0)
        return Status::invalidArgument("circuit breaker success threshold must be greater than 0");
    if (halfOpenMaxCalls_ == 0)
        return Status::invalidArgument("circuit breaker half-open max calls must be greater than 0");
    if (openTimeout_ <= Clock::Duration::zero())
        return Status::invalidArgument("circuit breaker open timeout must be positive");
    if (halfOpenProbeTimeout_ <= Clock::Duration::zero())
        return Status::invalidArgument("circuit breaker half-open probe timeout must be positive");
    return Status::ok();
}

CircuitBreaker::CircuitBreaker(
    CircuitBreakerPolicy policy,
    const Clock& clock,
    std::shared_ptr<ILogger> logger)
    : policy_(std::move(policy))
    , clock_(&clock)
    , logger_(std::move(logger))
{
}

CircuitBreakerResult CircuitBreaker::acquire() noexcept
{
    std::lock_guard lock(mutex_);

    if (auto status = policy_.validate(); !status.isOk()) {
        return CircuitBreakerResult {
            .allowed_ = false,
            .state_ = state_,
            .retryAfter_ = Clock::Duration::zero(),
            .status_ = status,
        };
    }

    const auto now = clock_->now();
    if (state_ == CircuitState::Open) {
        const auto retryAfter = remainingOpenTime(now);
        if (retryAfter > Clock::Duration::zero()) {
            return CircuitBreakerResult {
                .allowed_ = false,
                .state_ = state_,
                .retryAfter_ = retryAfter,
                .status_ = Status::unavailable("circuit breaker is open"),
            };
        }
        enterHalfOpen();
    }

    if (state_ == CircuitState::HalfOpen)
        expireHalfOpenProbes(now);

    if (state_ == CircuitState::HalfOpen && halfOpenProbes_.size() >= policy_.halfOpenMaxCalls_) {
        return CircuitBreakerResult {
            .allowed_ = false,
            .state_ = state_,
            .retryAfter_ = halfOpenRetryAfter(now),
            .status_ = Status::unavailable("circuit breaker half-open probe limit reached"),
        };
    }

    if (state_ == CircuitState::HalfOpen)
        halfOpenProbes_.push_back(now);

    return CircuitBreakerResult {
        .allowed_ = true,
        .state_ = state_,
        .retryAfter_ = Clock::Duration::zero(),
        .status_ = Status::ok(),
    };
}

void CircuitBreaker::recordSuccess() noexcept
{
    std::lock_guard lock(mutex_);

    if (state_ == CircuitState::Open)
        return;

    consecutiveFailures_ = 0;
    if (state_ == CircuitState::HalfOpen) {
        expireHalfOpenProbes(clock_->now());
        if (halfOpenProbes_.empty())
            return;
        releaseHalfOpenProbe();
        ++consecutiveSuccesses_;
        if (consecutiveSuccesses_ >= policy_.successThreshold_)
            close();
        return;
    }

    consecutiveSuccesses_ = 0;
}

void CircuitBreaker::recordFailure() noexcept
{
    std::lock_guard lock(mutex_);

    const auto now = clock_->now();
    if (state_ == CircuitState::Open)
        return;

    if (state_ == CircuitState::HalfOpen) {
        expireHalfOpenProbes(now);
        if (halfOpenProbes_.empty())
            return;
        open(now);
        return;
    }

    ++consecutiveFailures_;
    if (consecutiveFailures_ >= policy_.failureThreshold_)
        open(now);
}

void CircuitBreaker::reset() noexcept
{
    std::lock_guard lock(mutex_);
    close();
}

CircuitState CircuitBreaker::state() const noexcept
{
    std::lock_guard lock(mutex_);
    return state_;
}

CircuitBreakerPolicy CircuitBreaker::policy() const noexcept
{
    std::lock_guard lock(mutex_);
    return policy_;
}

std::uint32_t CircuitBreaker::consecutiveFailures() const noexcept
{
    std::lock_guard lock(mutex_);
    return consecutiveFailures_;
}

void CircuitBreaker::open(Clock::TimePoint now) noexcept
{
    const auto previous = state_;
    state_ = CircuitState::Open;
    openedAt_ = now;
    consecutiveFailures_ = 0;
    consecutiveSuccesses_ = 0;
    halfOpenProbes_.clear();
    if (previous != CircuitState::Open) {
        logTo(logger_,
            LogLevel::Warn,
            "CircuitBreaker",
            "state transition from={} to={}",
            __FILE__,
            __LINE__,
            circuitStateName(previous),
            circuitStateName(state_));
    }
}

void CircuitBreaker::close() noexcept
{
    const auto previous = state_;
    state_ = CircuitState::Closed;
    consecutiveFailures_ = 0;
    consecutiveSuccesses_ = 0;
    halfOpenProbes_.clear();
    openedAt_ = {};
    if (previous != CircuitState::Closed) {
        logTo(logger_,
            LogLevel::Info,
            "CircuitBreaker",
            "state transition from={} to={}",
            __FILE__,
            __LINE__,
            circuitStateName(previous),
            circuitStateName(state_));
    }
}

void CircuitBreaker::enterHalfOpen() noexcept
{
    const auto previous = state_;
    state_ = CircuitState::HalfOpen;
    consecutiveFailures_ = 0;
    consecutiveSuccesses_ = 0;
    halfOpenProbes_.clear();
    if (previous != CircuitState::HalfOpen) {
        logTo(logger_,
            LogLevel::Info,
            "CircuitBreaker",
            "state transition from={} to={}",
            __FILE__,
            __LINE__,
            circuitStateName(previous),
            circuitStateName(state_));
    }
}

void CircuitBreaker::expireHalfOpenProbes(Clock::TimePoint now) noexcept
{
    while (!halfOpenProbes_.empty() && now - halfOpenProbes_.front() >= policy_.halfOpenProbeTimeout_)
        halfOpenProbes_.pop_front();
}

void CircuitBreaker::releaseHalfOpenProbe() noexcept
{
    if (!halfOpenProbes_.empty())
        halfOpenProbes_.pop_front();
}

Clock::Duration CircuitBreaker::remainingOpenTime(Clock::TimePoint now) const noexcept
{
    const auto elapsed = now - openedAt_;
    if (elapsed >= policy_.openTimeout_)
        return Clock::Duration::zero();
    return policy_.openTimeout_ - elapsed;
}

Clock::Duration CircuitBreaker::halfOpenRetryAfter(Clock::TimePoint now) const noexcept
{
    if (halfOpenProbes_.empty())
        return Clock::Duration::zero();
    const auto elapsed = now - halfOpenProbes_.front();
    if (elapsed >= policy_.halfOpenProbeTimeout_)
        return Clock::Duration::zero();
    return policy_.halfOpenProbeTimeout_ - elapsed;
}

} // namespace lgc
