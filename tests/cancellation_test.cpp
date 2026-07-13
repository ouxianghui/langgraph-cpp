#include "foundation/cancellation/cancellation_token.hpp"
#include "foundation/status/status.hpp"
#include "foundation/time/clock.hpp"
#include "foundation/time/deadline.hpp"

#include <cassert>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

int main()
{
    using namespace std::chrono_literals;

    const auto none = lc::CancellationToken::none();
    assert(!none.cancellable());
    assert(!none.cancelled());
    assert(none.reason().empty());
    assert(none.check().isOk());

    lc::CancellationSource source;
    auto token = source.token();
    assert(token.cancellable());
    assert(!token.cancelled());
    assert(token.check().isOk());
#if LC_HAS_STD_STOP_TOKEN
    assert(token.nativeToken().stop_possible());
    assert(!token.nativeToken().stop_requested());
#endif

    auto copied = token;
    assert(source.cancel("user stopped graph run"));
    assert(!source.cancel("second reason is ignored"));
    assert(source.cancelled());
    assert(copied.cancelled());
    assert(source.reason() == "user stopped graph run");
    assert(copied.reason() == "user stopped graph run");
#if LC_HAS_STD_STOP_TOKEN
    assert(copied.nativeToken().stop_requested());
#endif

    const auto cancelled = lc::checkCancelled(copied);
    assert(!cancelled.isOk());
    assert(cancelled.code() == lc::StatusCode::Cancelled);
    assert(cancelled.message() == "user stopped graph run");

    bool threwCancelled = false;
    try {
        copied.throwIfCancelled();
    } catch (const lc::OperationInterrupted& error) {
        threwCancelled = true;
        assert(error.status().code() == lc::StatusCode::Cancelled);
        assert(std::string(error.what()).find("user stopped graph run") != std::string::npos);
    }
    assert(threwCancelled);

    lc::ManualClock clock;
    const auto future = lc::Deadline::after(clock, 10ms);
    assert(lc::checkCancelled(none, clock, future).isOk());

    clock.advance(10ms);
    const auto timedOut = lc::checkCancelled(
        none,
        clock,
        future,
        "cancelled",
        "node deadline reached");
    assert(!timedOut.isOk());
    assert(timedOut.code() == lc::StatusCode::DeadlineExceeded);
    assert(timedOut.message() == "node deadline reached");

    bool threwDeadline = false;
    try {
        none.throwIfCancelledOrDeadlineExceeded(clock, future);
    } catch (const lc::OperationInterrupted& error) {
        threwDeadline = true;
        assert(error.status().code() == lc::StatusCode::DeadlineExceeded);
    }
    assert(threwDeadline);

    lc::CancellationSource cancelledAndExpired;
    auto cancelledAndExpiredToken = cancelledAndExpired.token();
    assert(cancelledAndExpired.cancel("caller cancelled first"));
    const auto preferredCancel = cancelledAndExpiredToken.check(
        clock,
        future);
    assert(preferredCancel.code() == lc::StatusCode::Cancelled);
    assert(preferredCancel.message() == "caller cancelled first");

    lc::CancellationSource callbackSource;
    auto callbackToken = callbackSource.token();
    int callbackCount = 0;
    {
        auto registration = callbackToken.onCancel([&] {
            ++callbackCount;
        });
        assert(registration.registered());
        assert(callbackSource.cancel("wake waiters"));
        assert(callbackCount == 1);
        assert(!registration.registered());
    }

    int immediateCallbackCount = 0;
    auto immediateRegistration = callbackToken.onCancel([&] {
        ++immediateCallbackCount;
    });
    assert(!immediateRegistration.registered());
    assert(immediateCallbackCount == 1);

    lc::CancellationSource threaded;
    auto threadedToken = threaded.token();
    std::vector<std::thread> workers;
    workers.reserve(4);
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&threaded, i] {
            threaded.cancel("reason " + std::to_string(i));
        });
    }
    for (auto& worker : workers)
        worker.join();

    assert(threadedToken.cancelled());
    assert(!threadedToken.reason().empty());

    lc::CancellationSource unregisterSource;
    auto unregisterToken = unregisterSource.token();
    std::atomic<int> unregisteredCallbacks { 0 };
    auto removed = unregisterToken.onCancel([&] {
        unregisteredCallbacks.fetch_add(1, std::memory_order_relaxed);
    });
    assert(removed.registered());
    removed.unregister();
    assert(!removed.registered());
    assert(unregisterSource.cancel("after unregister"));
    assert(unregisteredCallbacks.load(std::memory_order_relaxed) == 0);

    lc::CancellationSource manyWaitersSource;
    auto manyWaitersToken = manyWaitersSource.token();
    std::atomic<int> callbackHits { 0 };
    std::vector<lc::CancellationRegistration> registrations;
    registrations.reserve(32);
    for (int i = 0; i < 32; ++i) {
        registrations.push_back(manyWaitersToken.onCancel([&] {
            callbackHits.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    std::vector<std::thread> cancellers;
    cancellers.reserve(8);
    for (int i = 0; i < 8; ++i) {
        cancellers.emplace_back([&manyWaitersSource, i] {
            manyWaitersSource.cancel("parallel cancel " + std::to_string(i));
        });
    }
    for (auto& canceller : cancellers)
        canceller.join();
    assert(manyWaitersToken.cancelled());
    assert(callbackHits.load(std::memory_order_relaxed) == 32);

    return 0;
}
