#include "foundation/resource/resource_limits.hpp"
#include "foundation/status/status.hpp"
#include "foundation/time/clock.hpp"

#include <cassert>
#include <chrono>
#include <thread>
#include <vector>

int main()
{
    using namespace std::chrono_literals;

    lc::ManualClock clock;
    auto limits = lc::ResourceLimits::unlimited()
                      .maxSteps(2)
                      .maxDuration(10ms)
                      .maxRetries(1)
                      .maxMemory(1024);

    assert(lc::validateResourceLimits(limits).isOk());

    lc::ExecutionBudget budget(limits, clock);
    assert(budget.check(clock).isOk());
    assert(budget.startedAt() == clock.now());
    assert(budget.deadline().hasDeadline());
    assert(budget.remaining(clock) == 10ms);

    auto status = budget.consumeStep(1, clock);
    assert(status.isOk());
    assert(budget.usage().steps_ == 1);

    status = budget.consumeStep(1, clock);
    assert(status.isOk());
    assert(budget.usage().steps_ == 2);

    status = budget.consumeStep(1, clock);
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::ResourceExhausted);
    assert(budget.usage().steps_ == 2);

    assert(budget.consumeRetry(1, clock).isOk());
    status = budget.consumeRetry(1, clock);
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::ResourceExhausted);

    assert(budget.recordMemory(512, clock).isOk());
    assert(budget.usage().memoryBytes_ == 512);
    assert(budget.usage().peakMemoryBytes_ == 512);
    assert(budget.recordMemory(256, clock).isOk());
    assert(budget.usage().memoryBytes_ == 256);
    assert(budget.usage().peakMemoryBytes_ == 512);
    status = budget.recordMemory(2048, clock);
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::ResourceExhausted);

    clock.advance(10ms);
    status = budget.check(clock);
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::DeadlineExceeded);
    assert(budget.elapsed(clock) == 10ms);
    assert(budget.remaining(clock) == 0ms);

    lc::ResourceLimits bad;
    bad.maxDuration_ = -1ms;
    assert(lc::validateResourceLimits(bad).code() == lc::StatusCode::InvalidArgument);
    lc::ExecutionBudget badBudget(bad, clock);
    assert(badBudget.check(clock).code() == lc::StatusCode::InvalidArgument);

    lc::ExecutionBudget unlimited;
    assert(unlimited.check().isOk());
    assert(unlimited.consumeStep(100).isOk());
    assert(unlimited.consumeRetry(10).isOk());
    assert(unlimited.recordMemory(1024 * 1024).isOk());

    lc::ManualClock threadedClock;
    lc::ExecutionBudget threaded(lc::ResourceLimits::unlimited().maxSteps(1000), threadedClock);
    std::vector<std::thread> workers;
    workers.reserve(4);
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&threaded, &threadedClock] {
            for (int j = 0; j < 100; ++j)
                assert(threaded.consumeStep(1, threadedClock).isOk());
        });
    }
    for (auto& worker : workers)
        worker.join();
    assert(threaded.usage().steps_ == 400);

    return 0;
}
