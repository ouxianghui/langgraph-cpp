#pragma once

#include "foundation/logging/logger.hpp"
#include "foundation/status/status.hpp"
#include "foundation/time/clock.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

namespace lc {

enum class CircuitState : std::uint8_t {
    Closed,
    Open,
    HalfOpen,
};

[[nodiscard]] const char* circuitStateName(CircuitState state) noexcept;

struct CircuitBreakerPolicy {
    std::uint32_t failureThreshold_ { 5 };
    std::uint32_t successThreshold_ { 1 };
    std::uint32_t halfOpenMaxCalls_ { 1 };
    Clock::Duration openTimeout_ { std::chrono::seconds(30) };
    Clock::Duration halfOpenProbeTimeout_ { std::chrono::seconds(30) };

    [[nodiscard]] Status validate() const;
};

struct CircuitBreakerResult {
    bool allowed_ { false };
    CircuitState state_ { CircuitState::Closed };
    Clock::Duration retryAfter_ { Clock::Duration::zero() };
    Status status_ { Status::unavailable("circuit breaker is open") };
};

class CircuitBreaker final {
public:
    explicit CircuitBreaker(
        CircuitBreakerPolicy policy = {},
        const Clock& clock = SteadyClock::instance(),
        std::shared_ptr<ILogger> logger = Logger::defaultLogger());

    [[nodiscard]] CircuitBreakerResult acquire() noexcept;

    void recordSuccess() noexcept;
    void recordFailure() noexcept;
    void reset() noexcept;

    [[nodiscard]] CircuitState state() const noexcept;
    [[nodiscard]] CircuitBreakerPolicy policy() const noexcept;
    [[nodiscard]] std::uint32_t consecutiveFailures() const noexcept;

private:
    void open(Clock::TimePoint now) noexcept;
    void close() noexcept;
    void enterHalfOpen() noexcept;
    void expireHalfOpenProbes(Clock::TimePoint now) noexcept;
    void releaseHalfOpenProbe() noexcept;
    [[nodiscard]] Clock::Duration remainingOpenTime(Clock::TimePoint now) const noexcept;
    [[nodiscard]] Clock::Duration halfOpenRetryAfter(Clock::TimePoint now) const noexcept;

    CircuitBreakerPolicy policy_;
    const Clock* clock_;
    std::shared_ptr<ILogger> logger_;
    mutable std::mutex mutex_;
    CircuitState state_ { CircuitState::Closed };
    std::uint32_t consecutiveFailures_ { 0 };
    std::uint32_t consecutiveSuccesses_ { 0 };
    std::deque<Clock::TimePoint> halfOpenProbes_;
    Clock::TimePoint openedAt_ {};
};

} // namespace lc
