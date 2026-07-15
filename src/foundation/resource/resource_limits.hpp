#pragma once

#include "foundation/status/status.hpp"
#include "foundation/time/clock.hpp"
#include "foundation/time/deadline.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace lgc {

struct ResourceLimits {
    std::optional<std::uint64_t> maxSteps_;
    std::optional<Clock::Duration> maxDuration_;
    std::optional<std::uint32_t> maxRetries_;
    std::optional<std::size_t> maxMemoryBytes_;

    [[nodiscard]] static ResourceLimits unlimited() noexcept;

    [[nodiscard]] ResourceLimits maxSteps(std::uint64_t value) const;
    [[nodiscard]] ResourceLimits maxDuration(Clock::Duration value) const;
    [[nodiscard]] ResourceLimits maxRetries(std::uint32_t value) const;
    [[nodiscard]] ResourceLimits maxMemory(std::size_t value) const;
};

struct ResourceUsage {
    std::uint64_t steps_ { 0 };
    std::uint32_t retries_ { 0 };
    std::size_t memoryBytes_ { 0 };
    std::size_t peakMemoryBytes_ { 0 };
};

[[nodiscard]] Status validateResourceLimits(const ResourceLimits& limits);

/// Cooperative execution budget consumed by a graph run.
///
/// This is not an OS-level sandbox and does not enforce CPU, heap, thread, file-descriptor, or
/// process limits by itself. It is a thread-safe accounting boundary: graph runtime code must call
/// `consumeStep`, `consumeRetry`, or `recordMemory` at natural scheduling points and stop work when
/// a limit returns a non-OK `Status`. Hard isolation belongs in the runtime host or operating
/// system layer.
class ExecutionBudget final {
public:
    ExecutionBudget(ResourceLimits limits = {}, const Clock& clock = SteadyClock::instance());

    [[nodiscard]] const ResourceLimits& limits() const noexcept;
    [[nodiscard]] Clock::TimePoint startedAt() const noexcept;
    [[nodiscard]] Deadline deadline() const noexcept;

    [[nodiscard]] ResourceUsage usage() const noexcept;
    [[nodiscard]] Clock::Duration elapsed(const Clock& clock) const noexcept;
    [[nodiscard]] Clock::Duration remaining(const Clock& clock) const noexcept;

    [[nodiscard]] Status check(const Clock& clock = SteadyClock::instance()) const;
    [[nodiscard]] Status checkStep(std::uint64_t amount = 1) const;
    [[nodiscard]] Status checkRetry(std::uint32_t amount = 1) const;
    [[nodiscard]] Status checkMemory(std::size_t memoryBytes) const;

    [[nodiscard]] Status consumeStep(
        std::uint64_t amount = 1,
        const Clock& clock = SteadyClock::instance());

    [[nodiscard]] Status consumeRetry(
        std::uint32_t amount = 1,
        const Clock& clock = SteadyClock::instance());

    [[nodiscard]] Status recordMemory(
        std::size_t memoryBytes,
        const Clock& clock = SteadyClock::instance());

private:
    [[nodiscard]] Status statusForCounts(
        std::uint64_t steps,
        std::uint32_t retries,
        std::size_t memoryBytes,
        const Clock& clock) const;

    ResourceLimits limits_;
    Clock::TimePoint startedAt_;
    Deadline deadline_;
    std::atomic<std::uint64_t> steps_ { 0 };
    std::atomic<std::uint32_t> retries_ { 0 };
    std::atomic<std::size_t> memoryBytes_ { 0 };
    std::atomic<std::size_t> peakMemoryBytes_ { 0 };
};

} // namespace lgc
