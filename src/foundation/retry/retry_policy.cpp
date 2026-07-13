#include "foundation/retry/retry_policy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <string>
#include <utility>

namespace lc {
namespace {

using Duration = Clock::Duration;

[[nodiscard]] Duration clampDurationCount(long double count, Duration maxDelay) noexcept
{
    if (count <= 0.0L)
        return Duration::zero();

    const auto maxCount = static_cast<long double>(maxDelay.count());
    if (count >= maxCount)
        return maxDelay;

    const auto repMax = static_cast<long double>(std::numeric_limits<Duration::rep>::max());
    if (count >= repMax)
        return Duration::max();

    return Duration(static_cast<Duration::rep>(count));
}

[[nodiscard]] Result<std::uint64_t> randomSeed(IRandomSource& source)
{
    std::array<std::byte, sizeof(std::uint64_t)> bytes {};
    if (auto status = source.fill(bytes); !status.isOk())
        return status.status();

    std::uint64_t seed = 0;
    for (const auto byte : bytes)
        seed = (seed << 8U) | static_cast<std::uint64_t>(std::to_integer<unsigned char>(byte));
    return seed;
}

[[nodiscard]] bool containsStatusCode(const std::vector<StatusCode>& codes, StatusCode code) noexcept
{
    return std::ranges::find(codes, code) != codes.end();
}

} // namespace

Backoff::Backoff(
    BackoffType type,
    Duration initialDelay,
    double multiplier,
    Duration maxDelay,
    JitterMode jitterMode) noexcept
    : type_(type)
    , initialDelay_(initialDelay)
    , multiplier_(multiplier)
    , maxDelay_(maxDelay)
    , jitterMode_(jitterMode)
{
}

Backoff Backoff::fixed(Duration delay) noexcept
{
    return Backoff(BackoffType::Fixed, delay, 1.0, delay, JitterMode::None);
}

Backoff Backoff::exponential(Duration initialDelay, double multiplier, Duration maxDelay) noexcept
{
    return Backoff(BackoffType::Exponential, initialDelay, multiplier, maxDelay, JitterMode::None);
}

Backoff Backoff::jitter(JitterMode mode) const noexcept
{
    auto out = *this;
    out.jitterMode_ = mode;
    return out;
}

BackoffType Backoff::type() const noexcept
{
    return type_;
}

Duration Backoff::initialDelay() const noexcept
{
    return initialDelay_;
}

double Backoff::multiplier() const noexcept
{
    return multiplier_;
}

Duration Backoff::maxDelay() const noexcept
{
    return maxDelay_;
}

JitterMode Backoff::jitterMode() const noexcept
{
    return jitterMode_;
}

Result<Duration> Backoff::delay(std::uint32_t retryIndex) const
{
    const auto baseDelay = this->baseDelay(retryIndex);
    if (!baseDelay.isOk())
        return baseDelay.status();
    if (jitterMode_ == JitterMode::None)
        return baseDelay;

    OsRandomSource source;
    return delay(retryIndex, source);
}

Result<Duration> Backoff::delay(std::uint32_t retryIndex, std::uint64_t jitterSeed) const
{
    const auto baseDelay = this->baseDelay(retryIndex);
    if (!baseDelay.isOk())
        return baseDelay.status();
    if (jitterMode_ == JitterMode::None)
        return baseDelay;
    return applyJitter(*baseDelay, jitterSeed);
}

Result<Duration> Backoff::delay(std::uint32_t retryIndex, IRandomSource& randomSource) const
{
    const auto baseDelay = this->baseDelay(retryIndex);
    if (!baseDelay.isOk())
        return baseDelay.status();
    if (jitterMode_ == JitterMode::None)
        return baseDelay;

    auto seed = randomSeed(randomSource);
    if (!seed.isOk())
        return seed.status();
    return applyJitter(*baseDelay, *seed);
}

Status Backoff::validate() const
{
    if (initialDelay_ < Duration::zero())
        return Status::invalidArgument("backoff initial delay cannot be negative");
    if (maxDelay_ < Duration::zero())
        return Status::invalidArgument("backoff max delay cannot be negative");
    if (type_ == BackoffType::Exponential && (!std::isfinite(multiplier_) || multiplier_ < 1.0))
        return Status::invalidArgument("exponential backoff multiplier must be finite and >= 1");
    return Status::ok();
}

Result<Duration> Backoff::baseDelay(std::uint32_t retryIndex) const
{
    if (retryIndex == 0)
        return Duration::zero();
    if (auto status = validate(); !status.isOk())
        return status;

    if (type_ == BackoffType::Fixed)
        return std::min(initialDelay_, maxDelay_);

    long double count = static_cast<long double>(initialDelay_.count());
    for (std::uint32_t i = 1; i < retryIndex; ++i) {
        count *= static_cast<long double>(multiplier_);
        if (count >= static_cast<long double>(maxDelay_.count()))
            return maxDelay_;
    }
    return clampDurationCount(count, maxDelay_);
}

Duration Backoff::applyJitter(Duration baseDelay, std::uint64_t seed) const
{
    if (baseDelay <= Duration::zero())
        return Duration::zero();

    const auto baseCount = baseDelay.count();
    Duration::rep minCount = 0;
    if (jitterMode_ == JitterMode::Equal)
        minCount = baseCount / 2;

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<Duration::rep> distribution(minCount, baseCount);
    return Duration(distribution(rng));
}

RetryPolicy RetryPolicy::none()
{
    return RetryPolicy {};
}

RetryPolicy RetryPolicy::fixed(std::uint32_t maxAttempts, Clock::Duration delay)
{
    return RetryPolicy {}.maxAttempts(maxAttempts).backoff(Backoff::fixed(delay));
}

RetryPolicy RetryPolicy::exponential(
    std::uint32_t maxAttempts,
    Clock::Duration initialDelay,
    double multiplier,
    Clock::Duration maxDelay)
{
    return RetryPolicy {}
        .maxAttempts(maxAttempts)
        .backoff(Backoff::exponential(initialDelay, multiplier, maxDelay));
}

std::uint32_t RetryPolicy::maxAttempts() const noexcept
{
    return maxAttempts_;
}

const Backoff& RetryPolicy::backoff() const noexcept
{
    return backoff_;
}

const std::vector<StatusCode>& RetryPolicy::retryableStatusCodes() const noexcept
{
    return retryableStatusCodes_;
}

bool RetryPolicy::retryOnAnyError() const noexcept
{
    return retryOnAnyError_;
}

RetryPolicy RetryPolicy::maxAttempts(std::uint32_t value) const noexcept
{
    auto out = *this;
    out.maxAttempts_ = value;
    return out;
}

RetryPolicy RetryPolicy::backoff(Backoff strategy) const noexcept
{
    auto out = *this;
    out.backoff_ = strategy;
    return out;
}

RetryPolicy RetryPolicy::retryOn(std::vector<StatusCode> codes) const
{
    std::vector<StatusCode> unique;
    unique.reserve(codes.size());
    for (const auto code : codes) {
        if (code == StatusCode::Ok)
            continue;
        if (!containsStatusCode(unique, code))
            unique.push_back(code);
    }

    auto out = *this;
    out.retryableStatusCodes_ = std::move(unique);
    return out;
}

RetryPolicy RetryPolicy::retryOn(StatusCode code) const
{
    if (code == StatusCode::Ok || containsStatusCode(retryableStatusCodes_, code))
        return *this;

    auto out = *this;
    out.retryableStatusCodes_.push_back(code);
    return out;
}

RetryPolicy RetryPolicy::retryOnAnyError(bool enabled) const noexcept
{
    auto out = *this;
    out.retryOnAnyError_ = enabled;
    return out;
}

RetryPolicy RetryPolicy::randomSource(std::shared_ptr<IRandomSource> source) const noexcept
{
    auto out = *this;
    out.randomSource_ = std::move(source);
    return out;
}

bool RetryPolicy::retryable(const Status& status) const noexcept
{
    if (status.isOk())
        return false;
    if (retryOnAnyError_)
        return true;
    return containsStatusCode(retryableStatusCodes_, status.code());
}

bool RetryPolicy::shouldRetry(const Status& status, std::uint32_t attemptsSoFar) const noexcept
{
    if (attemptsSoFar == 0)
        return false;
    if (attemptsSoFar >= maxAttempts_)
        return false;
    return retryable(status);
}

Result<Clock::Duration> RetryPolicy::delay(std::uint32_t attemptsSoFar) const
{
    if (backoff_.jitterMode() == JitterMode::None)
        return backoff_.delay(attemptsSoFar, 0);
    if (randomSource_)
        return backoff_.delay(attemptsSoFar, *randomSource_);
    return backoff_.delay(attemptsSoFar);
}

Result<Clock::Duration> RetryPolicy::delay(std::uint32_t attemptsSoFar, std::uint64_t jitterSeed) const
{
    return backoff_.delay(attemptsSoFar, jitterSeed);
}

Result<RetryDecision> RetryPolicy::decide(const Status& status, std::uint32_t attemptsSoFar) const
{
    if (!shouldRetry(status, attemptsSoFar)) {
        return RetryDecision {
            .retry_ = false,
            .nextAttempt_ = attemptsSoFar,
            .retriesUsed_ = attemptsSoFar == 0 ? 0 : attemptsSoFar - 1,
            .delay_ = Clock::Duration::zero(),
            .reason_ = status,
        };
    }

    auto nextDelay = delay(attemptsSoFar);
    if (!nextDelay.isOk())
        return nextDelay.status();

    return RetryDecision {
        .retry_ = true,
        .nextAttempt_ = attemptsSoFar + 1,
        .retriesUsed_ = attemptsSoFar,
        .delay_ = *nextDelay,
        .reason_ = status,
    };
}

Result<RetryDecision> RetryPolicy::decide(
    const Status& status,
    std::uint32_t attemptsSoFar,
    std::uint64_t jitterSeed) const
{
    if (!shouldRetry(status, attemptsSoFar)) {
        return RetryDecision {
            .retry_ = false,
            .nextAttempt_ = attemptsSoFar,
            .retriesUsed_ = attemptsSoFar == 0 ? 0 : attemptsSoFar - 1,
            .delay_ = Clock::Duration::zero(),
            .reason_ = status,
        };
    }

    auto nextDelay = delay(attemptsSoFar, jitterSeed);
    if (!nextDelay.isOk())
        return nextDelay.status();

    return RetryDecision {
        .retry_ = true,
        .nextAttempt_ = attemptsSoFar + 1,
        .retriesUsed_ = attemptsSoFar,
        .delay_ = *nextDelay,
        .reason_ = status,
    };
}

Status RetryPolicy::validate() const
{
    if (maxAttempts_ == 0)
        return Status::invalidArgument("retry max attempts must be at least 1");
    if (auto status = backoff_.validate(); !status.isOk())
        return status;
    if (backoff_.jitterMode() != JitterMode::None && !randomSource_) {
        OsRandomSource source;
        std::array<std::byte, sizeof(std::uint64_t)> probe {};
        if (auto status = source.fill(probe); !status.isOk())
            return status.status();
    }
    return Status::ok();
}

} // namespace lc
