#include "foundation/executor/concurrent_executor.hpp"
#include "foundation/executor/inline_executor.hpp"
#include "foundation/executor/owner_executor.hpp"
#include "foundation/executor/serial_executor.hpp"
#include "foundation/status/status.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <stdexcept>
#include <thread>

int main()
{
    using namespace std::chrono_literals;

    lgc::InlineExecutor inlineExecutor;
    assert(!inlineExecutor.isExecutorThread());
    int value = 0;
    auto status = inlineExecutor.execute([&] {
        assert(inlineExecutor.isExecutorThread());
        value = 42;
    });
    assert(status.isOk());
    assert(value == 42);
    assert(!inlineExecutor.isExecutorThread());
    assert(inlineExecutor.waitIdle(0ms).isOk());

    status = inlineExecutor.postDelayed(0ms, [&] { value = 43; });
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::Unimplemented);
    assert(value == 42);

    status = inlineExecutor.postDelayed(1ms, [] {});
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::Unimplemented);

    status = inlineExecutor.post([] {});
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::Unimplemented);

    status = inlineExecutor.execute([&] {
        assert(inlineExecutor.isExecutorThread());
        value = 44;
    });
    assert(status.isOk());
    assert(value == 44);

    status = inlineExecutor.executeAndWait([&] {
        assert(inlineExecutor.isExecutorThread());
        value = 45;
    });
    assert(status.isOk());
    assert(value == 45);

    status = inlineExecutor.execute([&] {
        assert(inlineExecutor.isExecutorThread());
        auto nested = inlineExecutor.executeAndWait([&] {
            assert(inlineExecutor.isExecutorThread());
            value = 46;
        });
        assert(nested.isOk());
    });
    assert(status.isOk());
    assert(value == 46);

    status = inlineExecutor.execute({});
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::InvalidArgument);

    status = inlineExecutor.execute([] { throw std::runtime_error("boom"); });
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::Internal);
    assert(inlineExecutor.waitIdle(0ms).isOk());

    status = inlineExecutor.close(0ms);
    assert(status.isOk());
    assert(inlineExecutor.isClosed());

    status = inlineExecutor.execute([] {});
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::Unavailable);

    status = inlineExecutor.post([] {});
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::Unavailable);

    status = inlineExecutor.postDelayed(0ms, [] {});
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::Unavailable);

    status = inlineExecutor.executeAndWait([] {});
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::Unavailable);

    lgc::SerialExecutor serialExecutor("test-serial-executor");
    assert(!serialExecutor.isExecutorThread());
    std::atomic<int> serialCounter { 0 };
    status = serialExecutor.execute([&] {
        assert(serialExecutor.isExecutorThread());
        serialCounter.fetch_add(1, std::memory_order_relaxed);
    });
    assert(status.isOk());
    status = serialExecutor.postDelayed(5ms, [&] {
        assert(serialExecutor.isExecutorThread());
        serialCounter.fetch_add(1, std::memory_order_relaxed);
    });
    assert(status.isOk());
    assert(serialExecutor.waitIdle(1s).isOk());
    assert(serialCounter.load(std::memory_order_relaxed) == 2);

    std::atomic<bool> serialCallerFinished { false };
    std::atomic<bool> serialPostObservedAfterCaller { false };
    status = serialExecutor.executeAndWait([&] {
        assert(serialExecutor.isExecutorThread());
        auto posted = serialExecutor.post([&] {
            assert(serialExecutor.isExecutorThread());
            serialPostObservedAfterCaller.store(
                serialCallerFinished.load(std::memory_order_acquire),
                std::memory_order_release);
            serialCounter.fetch_add(1, std::memory_order_relaxed);
        });
        assert(posted.isOk());
        assert(!serialPostObservedAfterCaller.load(std::memory_order_acquire));
        serialCallerFinished.store(true, std::memory_order_release);
    });
    assert(status.isOk());
    assert(serialExecutor.waitIdle(1s).isOk());
    assert(serialPostObservedAfterCaller.load(std::memory_order_acquire));
    assert(serialCounter.load(std::memory_order_relaxed) == 3);

    status = serialExecutor.executeAndWait([&] {
        assert(serialExecutor.isExecutorThread());
        auto nested = serialExecutor.execute([&] {
            assert(serialExecutor.isExecutorThread());
            serialCounter.fetch_add(1, std::memory_order_relaxed);
        });
        assert(nested.isOk());
    });
    assert(status.isOk());
    assert(serialCounter.load(std::memory_order_relaxed) == 4);

    status = serialExecutor.executeAndWait([] {
        throw std::runtime_error("serial boom");
    });
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::Internal);

    lgc::OwnerExecutor ownerExecutor(serialExecutor.thread(), "ExecutorTest", "owner");
    status = ownerExecutor.executeAndWait([&] {
        ownerExecutor.check();
        assert(ownerExecutor.isCurrent());
        serialCounter.fetch_add(1, std::memory_order_relaxed);
    });
    assert(status.isOk());
    assert(serialCounter.load(std::memory_order_relaxed) == 5);

    status = ownerExecutor.post([&] {
        ownerExecutor.check();
        assert(ownerExecutor.isCurrent());
        serialCounter.fetch_add(1, std::memory_order_relaxed);
    });
    assert(status.isOk());
    assert(serialExecutor.waitIdle(1s).isOk());
    assert(serialCounter.load(std::memory_order_relaxed) == 6);

    assert(serialExecutor.close(1s).isOk());
    assert(serialExecutor.isClosed());
    status = serialExecutor.execute([] {});
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::Unavailable);

    lgc::ConcurrentExecutor poolExecutor(2);
    assert(!poolExecutor.isExecutorThread());
    std::atomic<int> counter { 0 };
    std::atomic<bool> observedPoolThread { false };
    for (int i = 0; i < 8; ++i) {
        status = poolExecutor.execute([&] {
            if (poolExecutor.isExecutorThread())
                observedPoolThread.store(true, std::memory_order_release);
            counter.fetch_add(1, std::memory_order_relaxed);
        });
        assert(status.isOk());
    }

    assert(poolExecutor.waitIdle(2s).isOk());
    assert(counter.load(std::memory_order_relaxed) == 8);
    assert(observedPoolThread.load(std::memory_order_acquire));
    assert(!poolExecutor.isExecutorThread());
    assert(!poolExecutor.isClosed());

    status = poolExecutor.post([&counter] {
        counter.fetch_add(1, std::memory_order_relaxed);
    });
    assert(status.isOk());
    assert(poolExecutor.waitIdle(2s).isOk());
    assert(counter.load(std::memory_order_relaxed) == 9);

    status = poolExecutor.postDelayed(0ms, [&counter] {
        counter.fetch_add(1, std::memory_order_relaxed);
    });
    assert(status.isOk());
    assert(poolExecutor.waitIdle(2s).isOk());
    assert(counter.load(std::memory_order_relaxed) == 10);

    status = poolExecutor.postDelayed(1ms, [] {});
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::Unimplemented);

    status = poolExecutor.execute([&counter] {
        counter.fetch_add(1, std::memory_order_relaxed);
    });
    assert(status.isOk());
    assert(poolExecutor.waitIdle(2s).isOk());
    assert(counter.load(std::memory_order_relaxed) == 11);

    status = poolExecutor.executeAndWait([&counter] {
        counter.fetch_add(1, std::memory_order_relaxed);
    });
    assert(status.isOk());
    assert(counter.load(std::memory_order_relaxed) == 12);

    status = poolExecutor.executeAndWait([] {
        throw std::runtime_error("wait boom");
    });
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::Internal);

    std::atomic<bool> nestedExecuteRanInline { false };
    std::atomic<bool> nestedExecuteAndWaitRanInline { false };
    status = poolExecutor.execute([&] {
        assert(poolExecutor.isExecutorThread());

        auto nested = poolExecutor.execute([&] {
            assert(poolExecutor.isExecutorThread());
            nestedExecuteRanInline.store(true, std::memory_order_release);
        });
        assert(nested.isOk());
        assert(nestedExecuteRanInline.load(std::memory_order_acquire));

        nested = poolExecutor.executeAndWait([&] {
            assert(poolExecutor.isExecutorThread());
            nestedExecuteAndWaitRanInline.store(true, std::memory_order_release);
        });
        assert(nested.isOk());
        assert(nestedExecuteAndWaitRanInline.load(std::memory_order_acquire));
    });
    assert(status.isOk());
    assert(poolExecutor.waitIdle(2s).isOk());
    assert(nestedExecuteRanInline.load(std::memory_order_acquire));
    assert(nestedExecuteAndWaitRanInline.load(std::memory_order_acquire));

    lgc::ConcurrentExecutor singlePoolExecutor(1);
    std::atomic<bool> poolCallerFinished { false };
    std::atomic<bool> poolPostObservedAfterCaller { false };
    status = singlePoolExecutor.executeAndWait([&] {
        assert(singlePoolExecutor.isExecutorThread());
        auto posted = singlePoolExecutor.post([&] {
            assert(singlePoolExecutor.isExecutorThread());
            poolPostObservedAfterCaller.store(
                poolCallerFinished.load(std::memory_order_acquire),
                std::memory_order_release);
        });
        assert(posted.isOk());
        assert(!poolPostObservedAfterCaller.load(std::memory_order_acquire));
        poolCallerFinished.store(true, std::memory_order_release);
    });
    assert(status.isOk());
    assert(singlePoolExecutor.waitIdle(2s).isOk());
    assert(poolPostObservedAfterCaller.load(std::memory_order_acquire));
    assert(singlePoolExecutor.close(2s).isOk());

    status = poolExecutor.close(2s);
    assert(status.isOk());
    assert(poolExecutor.isClosed());

    status = poolExecutor.execute([] {});
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::Unavailable);

    status = poolExecutor.post([] {});
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::Unavailable);

    status = poolExecutor.postDelayed(0ms, [] {});
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::Unavailable);

    status = poolExecutor.executeAndWait([] {});
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::Unavailable);

    auto sharedExecutor = lgc::makeConcurrentExecutor(1);
    std::atomic<bool> ran { false };
    status = sharedExecutor->execute([&ran] {
        ran.store(true, std::memory_order_release);
    });
    assert(status.isOk());
    assert(sharedExecutor->waitIdle(2s).isOk());
    assert(ran.load(std::memory_order_acquire));
    assert(sharedExecutor->close(2s).isOk());

    return 0;
}
