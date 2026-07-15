#pragma once

#include "foundation/id/id_generator.hpp"
#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"
#include "foundation/time/clock.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace lgc {

enum class BackoffType : std::uint8_t {
    Fixed,
    Exponential,
};

enum class JitterMode : std::uint8_t {
    None,
    Full,
    Equal,
};

class Backoff final {
public:
    using Duration = Clock::Duration;

    [[nodiscard]] static Backoff fixed(Duration delay) noexcept;
    [[nodiscard]] static Backoff exponential(
        Duration initialDelay,
        double multiplier = 2.0,
        Duration maxDelay = Duration::max()) noexcept;

    [[nodiscard]] Backoff jitter(JitterMode mode) const noexcept;

    [[nodiscard]] BackoffType type() const noexcept;
    [[nodiscard]] Duration initialDelay() const noexcept;
    [[nodiscard]] double multiplier() const noexcept;
    [[nodiscard]] Duration maxDelay() const noexcept;
    [[nodiscard]] JitterMode jitterMode() const noexcept;

    /// Retry index is 1-based: the first retry after the first failed attempt is `1`.
    [[nodiscard]] Result<Duration> delay(std::uint32_t retryIndex) const;
    [[nodiscard]] Result<Duration> delay(std::uint32_t retryIndex, std::uint64_t jitterSeed) const;
    [[nodiscard]] Result<Duration> delay(std::uint32_t retryIndex, IRandomSource& randomSource) const;
    [[nodiscard]] Status validate() const;

private:
    Backoff(
        BackoffType type,
        Duration initialDelay,
        double multiplier,
        Duration maxDelay,
        JitterMode jitterMode) noexcept;

    [[nodiscard]] Result<Duration> baseDelay(std::uint32_t retryIndex) const;
    [[nodiscard]] Duration applyJitter(Duration baseDelay, std::uint64_t seed) const;

    BackoffType type_ { BackoffType::Fixed };
    Duration initialDelay_ { Duration::zero() };
    double multiplier_ { 1.0 };
    Duration maxDelay_ { Duration::max() };
    JitterMode jitterMode_ { JitterMode::None };
};

struct RetryDecision {
    bool retry_ { false };
    std::uint32_t nextAttempt_ { 0 };
    std::uint32_t retriesUsed_ { 0 };
    Clock::Duration delay_ { Clock::Duration::zero() };
    Status reason_ { Status::ok() };
};

class RetryPolicy final {
public:
    [[nodiscard]] static RetryPolicy none();
    [[nodiscard]] static RetryPolicy fixed(std::uint32_t maxAttempts, Clock::Duration delay);
    [[nodiscard]] static RetryPolicy exponential(
        std::uint32_t maxAttempts,
        Clock::Duration initialDelay,
        double multiplier = 2.0,
        Clock::Duration maxDelay = Clock::Duration::max());

    [[nodiscard]] std::uint32_t maxAttempts() const noexcept;
    [[nodiscard]] const Backoff& backoff() const noexcept;
    [[nodiscard]] const std::vector<StatusCode>& retryableStatusCodes() const noexcept;
    [[nodiscard]] bool retryOnAnyError() const noexcept;

    [[nodiscard]] RetryPolicy maxAttempts(std::uint32_t value) const noexcept;
    [[nodiscard]] RetryPolicy backoff(Backoff strategy) const noexcept;
    [[nodiscard]] RetryPolicy retryOn(std::vector<StatusCode> codes) const;
    [[nodiscard]] RetryPolicy retryOn(StatusCode code) const;
    [[nodiscard]] RetryPolicy retryOnAnyError(bool enabled) const noexcept;

    [[nodiscard]] bool retryable(const Status& status) const noexcept;
    [[nodiscard]] bool shouldRetry(const Status& status, std::uint32_t attemptsSoFar) const noexcept;
    [[nodiscard]] Result<Clock::Duration> delay(std::uint32_t attemptsSoFar) const;
    [[nodiscard]] Result<Clock::Duration> delay(std::uint32_t attemptsSoFar, std::uint64_t jitterSeed) const;
    [[nodiscard]] RetryPolicy randomSource(std::shared_ptr<IRandomSource> source) const noexcept;
    [[nodiscard]] Result<RetryDecision> decide(const Status& status, std::uint32_t attemptsSoFar) const;
    [[nodiscard]] Result<RetryDecision> decide(
        const Status& status,
        std::uint32_t attemptsSoFar,
        std::uint64_t jitterSeed) const;
    [[nodiscard]] Status validate() const;

private:
    std::uint32_t maxAttempts_ { 1 };
    Backoff backoff_ { Backoff::fixed(Clock::Duration::zero()) };
    std::shared_ptr<IRandomSource> randomSource_;
    std::vector<StatusCode> retryableStatusCodes_ {
        StatusCode::Unavailable,
        StatusCode::ResourceExhausted,
        StatusCode::DeadlineExceeded,
        StatusCode::Aborted,
    };
    bool retryOnAnyError_ { false };
};

} // namespace lgc
