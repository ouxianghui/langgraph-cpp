#include "foundation/resource/resource_limits.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace lc {
namespace {

template <typename T>
[[nodiscard]] bool addWouldOverflow(T current, T amount) noexcept
{
    return amount > std::numeric_limits<T>::max() - current;
}

void updatePeak(std::atomic<std::size_t>& peak, std::size_t value) noexcept
{
    auto current = peak.load(std::memory_order_relaxed);
    while (value > current && !peak.compare_exchange_weak(
                                  current,
                                  value,
                                  std::memory_order_relaxed,
                                  std::memory_order_relaxed)) {
    }
}

[[nodiscard]] Status maxStepsExceeded(std::uint64_t limit)
{
    return Status::resourceExhausted("execution budget exceeded max steps: " + std::to_string(limit));
}

[[nodiscard]] Status maxRetriesExceeded(std::uint32_t limit)
{
    return Status::resourceExhausted("execution budget exceeded max retries: " + std::to_string(limit));
}

[[nodiscard]] Status maxMemoryExceeded(std::size_t limit)
{
    return Status::resourceExhausted(
        "execution budget exceeded max memory bytes: " + std::to_string(limit));
}

} // namespace

ResourceLimits ResourceLimits::unlimited() noexcept
{
    return ResourceLimits {};
}

ResourceLimits ResourceLimits::maxSteps(std::uint64_t value) const
{
    auto out = *this;
    out.maxSteps_ = value;
    return out;
}

ResourceLimits ResourceLimits::maxDuration(Clock::Duration value) const
{
    auto out = *this;
    out.maxDuration_ = value;
    return out;
}

ResourceLimits ResourceLimits::maxRetries(std::uint32_t value) const
{
    auto out = *this;
    out.maxRetries_ = value;
    return out;
}

ResourceLimits ResourceLimits::maxMemory(std::size_t value) const
{
    auto out = *this;
    out.maxMemoryBytes_ = value;
    return out;
}

Status validateResourceLimits(const ResourceLimits& limits)
{
    if (limits.maxDuration_.has_value() && *limits.maxDuration_ < Clock::Duration::zero())
        return Status::invalidArgument("execution max duration cannot be negative");
    return Status::ok();
}

ExecutionBudget::ExecutionBudget(ResourceLimits limits, const Clock& clock)
    : limits_(limits)
    , startedAt_(clock.now())
    , deadline_(Deadline::fromTimeout(clock, limits_.maxDuration_))
{
}

const ResourceLimits& ExecutionBudget::limits() const noexcept
{
    return limits_;
}

Clock::TimePoint ExecutionBudget::startedAt() const noexcept
{
    return startedAt_;
}

Deadline ExecutionBudget::deadline() const noexcept
{
    return deadline_;
}

ResourceUsage ExecutionBudget::usage() const noexcept
{
    return ResourceUsage {
        .steps_ = steps_.load(std::memory_order_relaxed),
        .retries_ = retries_.load(std::memory_order_relaxed),
        .memoryBytes_ = memoryBytes_.load(std::memory_order_relaxed),
        .peakMemoryBytes_ = peakMemoryBytes_.load(std::memory_order_relaxed),
    };
}

Clock::Duration ExecutionBudget::elapsed(const Clock& clock) const noexcept
{
    const auto now = clock.now();
    if (now <= startedAt_)
        return Clock::Duration::zero();
    return now - startedAt_;
}

Clock::Duration ExecutionBudget::remaining(const Clock& clock) const noexcept
{
    return deadline_.remaining(clock);
}

Status ExecutionBudget::check(const Clock& clock) const
{
    const auto current = usage();
    return statusForCounts(current.steps_, current.retries_, current.memoryBytes_, clock);
}

Status ExecutionBudget::checkStep(std::uint64_t amount) const
{
    const auto current = steps_.load(std::memory_order_relaxed);
    if (addWouldOverflow(current, amount))
        return Status::resourceExhausted("execution step counter overflow");
    const auto next = current + amount;
    if (limits_.maxSteps_.has_value() && next > *limits_.maxSteps_)
        return maxStepsExceeded(*limits_.maxSteps_);
    return Status::ok();
}

Status ExecutionBudget::checkRetry(std::uint32_t amount) const
{
    const auto current = retries_.load(std::memory_order_relaxed);
    if (addWouldOverflow(current, amount))
        return Status::resourceExhausted("execution retry counter overflow");
    const auto next = current + amount;
    if (limits_.maxRetries_.has_value() && next > *limits_.maxRetries_)
        return maxRetriesExceeded(*limits_.maxRetries_);
    return Status::ok();
}

Status ExecutionBudget::checkMemory(std::size_t memoryBytes) const
{
    if (limits_.maxMemoryBytes_.has_value() && memoryBytes > *limits_.maxMemoryBytes_)
        return maxMemoryExceeded(*limits_.maxMemoryBytes_);
    return Status::ok();
}

Status ExecutionBudget::consumeStep(std::uint64_t amount, const Clock& clock)
{
    if (auto limitStatus = check(clock); !limitStatus.isOk())
        return limitStatus;

    for (;;) {
        auto current = steps_.load(std::memory_order_relaxed);
        if (addWouldOverflow(current, amount))
            return Status::resourceExhausted("execution step counter overflow");

        const auto next = current + amount;
        if (limits_.maxSteps_.has_value() && next > *limits_.maxSteps_)
            return maxStepsExceeded(*limits_.maxSteps_);

        if (steps_.compare_exchange_weak(
                current,
                next,
                std::memory_order_relaxed,
                std::memory_order_relaxed))
            return Status::ok();
    }
}

Status ExecutionBudget::consumeRetry(std::uint32_t amount, const Clock& clock)
{
    if (auto limitStatus = check(clock); !limitStatus.isOk())
        return limitStatus;

    for (;;) {
        auto current = retries_.load(std::memory_order_relaxed);
        if (addWouldOverflow(current, amount))
            return Status::resourceExhausted("execution retry counter overflow");

        const auto next = current + amount;
        if (limits_.maxRetries_.has_value() && next > *limits_.maxRetries_)
            return maxRetriesExceeded(*limits_.maxRetries_);

        if (retries_.compare_exchange_weak(
                current,
                next,
                std::memory_order_relaxed,
                std::memory_order_relaxed))
            return Status::ok();
    }
}

Status ExecutionBudget::recordMemory(std::size_t memoryBytes, const Clock& clock)
{
    if (auto limitStatus = check(clock); !limitStatus.isOk())
        return limitStatus;

    if (auto status = checkMemory(memoryBytes); !status.isOk())
        return status;

    memoryBytes_.store(memoryBytes, std::memory_order_relaxed);
    updatePeak(peakMemoryBytes_, memoryBytes);
    return Status::ok();
}

Status ExecutionBudget::statusForCounts(
    std::uint64_t steps,
    std::uint32_t retries,
    std::size_t memoryBytes,
    const Clock& clock) const
{
    if (auto status = validateResourceLimits(limits_); !status.isOk())
        return status;

    if (auto status = deadline_.statusIfExpired(clock, "execution budget exceeded max duration"); !status.isOk())
        return status;

    if (limits_.maxSteps_.has_value() && steps > *limits_.maxSteps_)
        return maxStepsExceeded(*limits_.maxSteps_);

    if (limits_.maxRetries_.has_value() && retries > *limits_.maxRetries_)
        return maxRetriesExceeded(*limits_.maxRetries_);

    if (limits_.maxMemoryBytes_.has_value() && memoryBytes > *limits_.maxMemoryBytes_)
        return maxMemoryExceeded(*limits_.maxMemoryBytes_);

    return Status::ok();
}

} // namespace lc
