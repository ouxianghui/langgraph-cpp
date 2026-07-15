#include "foundation/threading/thread.hpp"
#include "foundation/threading/thread_pool.hpp"
#include "foundation/timer/interval_timer.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

int main()
{
    using namespace std::chrono_literals;

    {
        lgc::Thread thread("test-thread");
        std::vector<int> values;
        std::mutex mutex;

        thread.dispatchAsync([&] {
            std::lock_guard lock(mutex);
            values.push_back(1);
        });
        thread.dispatchSync([&] {
            std::lock_guard lock(mutex);
            values.push_back(2);
        });
        thread.dispatchAfter(5ms, [&] {
            std::lock_guard lock(mutex);
            values.push_back(3);
        });

        assert(thread.waitIdle(1s).isOk());
        {
            std::lock_guard lock(mutex);
            assert((values == std::vector<int> { 1, 2, 3 }));
        }
        assert(thread.shutdown(100ms).isOk());
        assert(!thread.isRunning());
        bool rejected = false;
        try {
            thread.dispatchSync([] {});
        } catch (...) {
            rejected = true;
        }
        assert(rejected);
    }

    {
        lgc::Thread thread("busy-thread");
        std::mutex mutex;
        std::condition_variable cv;
        bool release = false;

        thread.dispatchAsync([&] {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return release; });
        });

        auto idle = thread.waitIdle(1ms);
        assert(!idle.isOk());
        assert(idle.code() == lgc::StatusCode::DeadlineExceeded);
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        cv.notify_all();
        assert(thread.shutdown(1s).isOk());
    }

    {
        lgc::ThreadPool pool(2, 2);
        std::atomic<int> completed { 0 };
        std::mutex mutex;
        std::condition_variable cv;
        bool release = false;

        assert(pool.submit([&] {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return release; });
            completed.fetch_add(1, std::memory_order_relaxed);
        }).isOk());
        assert(pool.submit([&] {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return release; });
            completed.fetch_add(1, std::memory_order_relaxed);
        }).isOk());
        auto rejected = pool.submit([] {});
        assert(!rejected.isOk());
        assert(rejected.code() == lgc::StatusCode::ResourceExhausted);
        auto poolIdle = pool.waitIdle(1ms);
        assert(!poolIdle.isOk());
        assert(poolIdle.code() == lgc::StatusCode::DeadlineExceeded);

        {
            std::lock_guard lock(mutex);
            release = true;
        }
        cv.notify_all();
        assert(pool.shutdown(1s).isOk());
        assert(completed.load(std::memory_order_relaxed) == 2);
        auto afterShutdown = pool.submit([] {});
        assert(!afterShutdown.isOk());
        assert(afterShutdown.code() == lgc::StatusCode::FailedPrecondition);
    }

    {
        lgc::IntervalTimer timer;
        std::mutex mutex;
        std::condition_variable cv;
        int count = 0;

        timer.setSingleShot(true);
        timer.setHandler([&] {
            std::lock_guard lock(mutex);
            ++count;
            cv.notify_all();
        });
        assert(timer.start(5ms).isOk());

        std::unique_lock lock(mutex);
        assert(cv.wait_for(lock, 1s, [&] { return count == 1; }));
        lock.unlock();
        assert(timer.waitIdle(1s).isOk());
        assert(!timer.active());
        assert(timer.close(1s).isOk());
        assert(timer.isClosed());
        auto afterClose = timer.start(5ms);
        assert(!afterClose.isOk());
        assert(afterClose.code() == lgc::StatusCode::FailedPrecondition);
    }

    {
        lgc::IntervalTimer timer;
        std::mutex mutex;
        std::condition_variable cv;
        int count = 0;

        timer.setHandler([&] {
            std::lock_guard lock(mutex);
            ++count;
            cv.notify_all();
        });
        assert(timer.start(5ms).isOk());

        std::unique_lock lock(mutex);
        assert(cv.wait_for(lock, 1s, [&] { return count >= 2; }));
        lock.unlock();
        assert(timer.stop().isOk());
        assert(timer.waitIdle(1s).isOk());
        assert(!timer.active());
        assert(timer.close(1s).isOk());
    }

    {
        std::mutex mutex;
        std::condition_variable cv;
        bool fired = false;
        auto handle = lgc::IntervalTimer::singleShot(5ms, [&] {
            std::lock_guard lock(mutex);
            fired = true;
            cv.notify_all();
        });
        assert(handle.valid());

        std::unique_lock lock(mutex);
        assert(cv.wait_for(lock, 1s, [&] { return fired; }));
        lock.unlock();
        assert(handle.wait(1s).isOk());
        assert(!handle.active());
    }

    {
        bool fired = false;
        auto handle = lgc::IntervalTimer::singleShot(100ms, [&] {
            fired = true;
        });
        assert(handle.cancel().isOk());
        assert(handle.wait(1s).isOk());
        assert(!fired);
    }

    return 0;
}
