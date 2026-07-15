#include "foundation/retry/retry_policy.hpp"
#include "foundation/status/status.hpp"

#include <cassert>
#include <chrono>
#include <memory>
#include <span>
#include <vector>

namespace {

class FailingRandomSource final : public lgc::IRandomSource {
public:
    lgc::Result<void> fill(std::span<std::byte>) override
    {
        return lgc::Status::unavailable("random unavailable");
    }
};

} // namespace

int main()
{
    using namespace std::chrono_literals;

    auto fixed = lgc::Backoff::fixed(200ms);
    assert(fixed.validate().isOk());
    assert(*fixed.delay(0) == 0ms);
    assert(*fixed.delay(1) == 200ms);
    assert(*fixed.delay(5) == 200ms);

    auto exponential = lgc::Backoff::exponential(100ms, 2.0, 500ms);
    assert(exponential.validate().isOk());
    assert(*exponential.delay(1) == 100ms);
    assert(*exponential.delay(2) == 200ms);
    assert(*exponential.delay(3) == 400ms);
    assert(*exponential.delay(4) == 500ms);

    auto fullJitter = exponential.jitter(lgc::JitterMode::Full);
    const auto jitterA = *fullJitter.delay(3, 1234);
    const auto jitterB = *fullJitter.delay(3, 1234);
    assert(jitterA == jitterB);
    assert(jitterA >= 0ms);
    assert(jitterA <= 400ms);

    auto equalJitter = exponential.jitter(lgc::JitterMode::Equal);
    const auto equal = *equalJitter.delay(3, 5678);
    assert(equal >= 200ms);
    assert(equal <= 400ms);

    auto invalidBackoff = lgc::Backoff::exponential(-1ms);
    assert(invalidBackoff.validate().code() == lgc::StatusCode::InvalidArgument);
    invalidBackoff = lgc::Backoff::exponential(1ms, 0.5);
    assert(invalidBackoff.validate().code() == lgc::StatusCode::InvalidArgument);
    assert(!invalidBackoff.delay(1).isOk());

    auto disabled = lgc::RetryPolicy::none();
    assert(disabled.maxAttempts() == 1);
    assert(!disabled.shouldRetry(lgc::Status::unavailable("down"), 1));
    assert(!disabled.shouldRetry(lgc::Status::ok(), 1));

    auto policy = lgc::RetryPolicy::fixed(3, 50ms);
    assert(policy.validate().isOk());
    assert(policy.maxAttempts() == 3);
    assert(policy.shouldRetry(lgc::Status::unavailable("down"), 1));
    assert(policy.shouldRetry(lgc::Status::unavailable("down"), 2));
    assert(!policy.shouldRetry(lgc::Status::unavailable("down"), 3));
    assert(!policy.shouldRetry(lgc::Status::invalidArgument("bad input"), 1));

    auto decision = policy.decide(lgc::Status::unavailable("down"), 1);
    assert(decision.isOk());
    assert(decision->retry_);
    assert(decision->nextAttempt_ == 2);
    assert(decision->retriesUsed_ == 1);
    assert(decision->delay_ == 50ms);
    assert(decision->reason_.code() == lgc::StatusCode::Unavailable);

    decision = policy.decide(lgc::Status::unavailable("down"), 3);
    assert(decision.isOk());
    assert(!decision->retry_);
    assert(decision->nextAttempt_ == 3);

    auto broadPolicy = policy.retryOnAnyError(true);
    assert(broadPolicy.shouldRetry(lgc::Status::invalidArgument("bad input"), 1));

    auto customPolicy = lgc::RetryPolicy::none()
                            .maxAttempts(3)
                            .backoff(exponential.jitter(lgc::JitterMode::Full))
                            .retryOn(std::vector<lgc::StatusCode> { lgc::StatusCode::Internal })
                            .retryOn(lgc::StatusCode::Internal);
    assert(customPolicy.maxAttempts() == 3);
    assert(customPolicy.retryableStatusCodes().size() == 1);
    auto customDecision = customPolicy.decide(lgc::Status::internal("tool failed"), 1, 42);
    assert(customDecision.isOk());
    assert(customDecision->retry_);
    assert(customDecision->delay_ >= 0ms);
    assert(customDecision->delay_ <= 100ms);

    auto invalidPolicy = policy.maxAttempts(0);
    assert(invalidPolicy.validate().code() == lgc::StatusCode::InvalidArgument);

    auto expPolicy = lgc::RetryPolicy::exponential(4, 10ms, 3.0, 100ms);
    assert(*expPolicy.delay(1) == 10ms);
    assert(*expPolicy.delay(2) == 30ms);
    assert(*expPolicy.delay(3) == 90ms);

    auto failingJitter = lgc::RetryPolicy::fixed(2, 10ms)
                             .backoff(lgc::Backoff::fixed(10ms).jitter(lgc::JitterMode::Full))
                             .randomSource(std::make_shared<FailingRandomSource>());
    auto failedDecision = failingJitter.decide(lgc::Status::unavailable("down"), 1);
    assert(!failedDecision.isOk());
    assert(failedDecision.status().code() == lgc::StatusCode::Unavailable);

    return 0;
}
