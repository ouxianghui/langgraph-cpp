#include "foundation/scheduler/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <source_location>
#include <thread>
#include <vector>

namespace {

class RecordingExecutor final : public lc::IExecutor {
public:
    ~RecordingExecutor() override
    {
        (void)waitIdle(std::chrono::seconds(1));
    }

    [[nodiscard]] lc::Status post(Task task, std::source_location = std::source_location::current()) override
    {
        if (!task)
            return lc::Status::invalidArgument("task cannot be empty");
        if (closed_.load(std::memory_order_acquire))
            return lc::Status::unavailable("executor is closed");

        postCalls_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(mutex_);
        threads_.emplace_back([task = std::move(task)]() mutable {
            task();
        });
        return lc::Status::ok();
    }

    [[nodiscard]] lc::Status postDelayed(
        Duration,
        Task,
        std::source_location = std::source_location::current()) override
    {
        return lc::Status::unimplemented("not supported");
    }

    [[nodiscard]] lc::Status execute(Task, std::source_location = std::source_location::current()) override
    {
        executeCalls_.fetch_add(1, std::memory_order_relaxed);
        return lc::Status::internal("scheduler should use post");
    }

    [[nodiscard]] lc::Status executeAndWait(Task, std::source_location = std::source_location::current()) override
    {
        executeAndWaitCalls_.fetch_add(1, std::memory_order_relaxed);
        return lc::Status::internal("scheduler should use post");
    }

    [[nodiscard]] lc::Status waitIdle(Duration) override
    {
        std::vector<std::thread> threads;
        {
            std::lock_guard lock(mutex_);
            threads.swap(threads_);
        }
        for (auto& thread : threads) {
            if (thread.joinable())
                thread.join();
        }
        return lc::Status::ok();
    }

    [[nodiscard]] lc::Status close(Duration timeout) override
    {
        closed_.store(true, std::memory_order_release);
        return waitIdle(timeout);
    }

    [[nodiscard]] bool isClosed() const noexcept override
    {
        return closed_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool isExecutorThread() const noexcept override
    {
        return false;
    }

    [[nodiscard]] int postCalls() const noexcept { return postCalls_.load(std::memory_order_relaxed); }
    [[nodiscard]] int executeCalls() const noexcept { return executeCalls_.load(std::memory_order_relaxed); }
    [[nodiscard]] int executeAndWaitCalls() const noexcept { return executeAndWaitCalls_.load(std::memory_order_relaxed); }

private:
    mutable std::mutex mutex_;
    std::vector<std::thread> threads_;
    std::atomic<bool> closed_ { false };
    std::atomic<int> postCalls_ { 0 };
    std::atomic<int> executeCalls_ { 0 };
    std::atomic<int> executeAndWaitCalls_ { 0 };
};

} // namespace

int main()
{
    using namespace std::chrono_literals;

    {
        lc::TaskScheduler scheduler;
        std::atomic<int> count { 0 };

        auto scheduled = scheduler.scheduleAfter(10ms, [&] {
            ++count;
        });
        assert(scheduled.isOk());
        auto task = scheduled.value();
        assert(task.valid());
        assert(task.id() != 0);
        assert(scheduler.waitIdle(1s).isOk());
        assert(count == 1);
        assert(task.state() == lc::ScheduledTaskState::Completed);
        assert(task.status().isOk());
        assert(lc::taskStateName(task.state()) == "completed");
    }

    {
        lc::TaskScheduler scheduler;

        auto scheduled = scheduler.scheduleAfter(1ms, [] {
            throw std::runtime_error("boom");
        });
        assert(scheduled.isOk());
        auto task = scheduled.value();
        assert(scheduler.waitIdle(1s).isOk());
        assert(task.state() == lc::ScheduledTaskState::Failed);
        assert(!task.status().isOk());
        assert(lc::taskStateName(task.state()) == "failed");
    }

    {
        lc::TaskScheduler scheduler;
        std::atomic<int> count { 0 };

        auto scheduled = scheduler.schedulePeriodic(
            5ms,
            [&] {
                ++count;
            },
            lc::PeriodicScheduleOptions {
                .maxRuns_ = 3,
            });
        assert(scheduled.isOk());
        auto task = scheduled.value();
        assert(scheduler.waitIdle(1s).isOk());
        assert(count == 3);
        assert(task.isFinished());
    }

    {
        lc::TaskScheduler scheduler;
        std::atomic<int> count { 0 };

        auto scheduled = scheduler.schedulePeriodic(
            5ms,
            [&] {
                ++count;
            },
            lc::PeriodicScheduleOptions {
                .maxRuns_ = 3,
                .mode_ = lc::PeriodicScheduleMode::FixedRate,
                .startImmediately_ = true,
            });
        assert(scheduled.isOk());
        assert(scheduler.waitIdle(1s).isOk());
        assert(count == 3);
        assert(lc::periodicModeName(lc::PeriodicScheduleMode::FixedRate) == "fixed_rate");
    }

    {
        lc::TaskScheduler scheduler;
        std::atomic<int> count { 0 };

        auto scheduled = scheduler.scheduleAfter(200ms, [&] {
            ++count;
        });
        assert(scheduled.isOk());
        auto task = scheduled.value();
        assert(task.cancel());
        assert(task.isCancelled());
        assert(scheduler.waitIdle(100ms).isOk());
        assert(count == 0);
    }

    {
        lc::TaskScheduler scheduler;
        lc::CancellationSource source;
        std::atomic<int> count { 0 };

        auto scheduled = scheduler.scheduleAfter(
            1h,
            [&] {
                ++count;
            },
            lc::ScheduleOptions {
                .cancellation_ = source.token(),
            });
        assert(scheduled.isOk());
        auto task = scheduled.value();
        assert(source.cancel("caller stopped run"));
        assert(scheduler.waitIdle(100ms).isOk());
        assert(count == 0);
        assert(task.state() == lc::ScheduledTaskState::Cancelled);
        assert(task.status().code() == lc::StatusCode::Cancelled);
        assert(task.status().message() == "caller stopped run");
    }

    {
        lc::TaskScheduler scheduler;
        std::atomic<int> attempts { 0 };
        std::atomic<int> completionCalls { 0 };
        lc::RetryReport finalReport;

        auto scheduled = scheduler.scheduleRetry(
            [&](std::uint32_t attempt) {
                ++attempts;
                if (attempt < 3)
                    return lc::Status::unavailable("temporary");
                return lc::Status::ok();
            },
            [&](lc::RetryReport report) {
                finalReport = report;
                ++completionCalls;
            },
            lc::RetryScheduleOptions {
                .policy_ = lc::RetryPolicy::fixed(3, 5ms),
            });
        assert(scheduled.isOk());
        auto task = scheduled.value();
        assert(scheduler.waitIdle(1s).isOk());
        assert(attempts == 3);
        assert(completionCalls == 1);
        assert(finalReport.attempts_ == 3);
        assert(finalReport.retries_ == 2);
        assert(finalReport.status_.isOk());
        assert(task.state() == lc::ScheduledTaskState::Completed);
    }

    {
        lc::TaskScheduler scheduler;
        std::atomic<int> attempts { 0 };
        lc::RetryReport finalReport;

        auto scheduled = scheduler.scheduleRetry(
            [&](std::uint32_t) {
                ++attempts;
                return lc::Status::unavailable("still down");
            },
            [&](lc::RetryReport report) {
                finalReport = report;
            },
            lc::RetryScheduleOptions {
                .policy_ = lc::RetryPolicy::fixed(2, 5ms),
            });
        assert(scheduled.isOk());
        auto task = scheduled.value();
        assert(scheduler.waitIdle(1s).isOk());
        assert(attempts == 2);
        assert(finalReport.attempts_ == 2);
        assert(finalReport.retries_ == 1);
        assert(finalReport.status_.code() == lc::StatusCode::Unavailable);
        assert(task.state() == lc::ScheduledTaskState::Failed);
        assert(task.status().code() == lc::StatusCode::Unavailable);
    }

    {
        lc::TaskScheduler scheduler;
        lc::CancellationSource source;

        auto scheduled = scheduler.cancelAfter(10ms, source, "node timed out");
        assert(scheduled.isOk());
        auto timeout = scheduled.value();
        assert(scheduler.waitIdle(1s).isOk());
        assert(source.cancelled());
        assert(source.reason() == "node timed out");
        assert(timeout.state() == lc::ScheduledTaskState::Completed);
    }

    {
        lc::TaskScheduler scheduler;
        lc::CancellationToken token;
        lc::ScheduledTask timeout;
        {
            lc::CancellationSource source;
            token = source.token();
            auto scheduled = scheduler.cancelAfter(10ms, source, "scoped source timed out");
            assert(scheduled.isOk());
            timeout = scheduled.value();
        }
        assert(scheduler.waitIdle(1s).isOk());
        assert(token.cancelled());
        assert(token.reason() == "scoped source timed out");
        assert(timeout.state() == lc::ScheduledTaskState::Completed);
    }

    {
        lc::TaskScheduler scheduler;
        std::mutex mutex;
        std::condition_variable cv;
        bool started = false;
        bool release = false;

        auto scheduled = scheduler.scheduleAfter(0ms, [&] {
            {
                std::lock_guard lock(mutex);
                started = true;
            }
            cv.notify_all();

            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return release; });
        });
        assert(scheduled.isOk());
        auto task = scheduled.value();

        {
            std::unique_lock lock(mutex);
            assert(cv.wait_for(lock, 1s, [&] { return started; }));
        }
        assert(task.cancel());
        assert(task.state() == lc::ScheduledTaskState::Cancelling);
        assert(lc::taskStateName(task.state()) == "cancelling");
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        cv.notify_all();
        assert(scheduler.waitIdle(1s).isOk());
        assert(task.state() == lc::ScheduledTaskState::Completed);
        assert(task.status().isOk());
    }

    {
        std::mutex eventsMutex;
        std::vector<lc::SchedulerEventType> events;
        auto eventSink = std::make_shared<lc::SchedulerCallbackSink>(
            [&](lc::SchedulerEvent event) {
                std::lock_guard lock(eventsMutex);
                events.push_back(event.type_);
                return lc::Status::ok();
            });
        auto metrics = std::make_shared<lc::InMemoryMetricRecorder>();
        auto traces = std::make_shared<lc::InMemoryTraceSink>();
        lc::TaskScheduler scheduler(lc::SchedulerOptions {
            .eventSink_ = eventSink,
            .metricsRecorder_ = metrics,
            .traceSink_ = traces,
        });

        auto scheduled = scheduler.scheduleAfter(5ms, [] {}, lc::ScheduleOptions {
            .name_ = "observed",
        });
        assert(scheduled.isOk());
        assert(scheduler.waitIdle(1s).isOk());

        std::vector<lc::SchedulerEventType> snapshot;
        {
            std::lock_guard lock(eventsMutex);
            snapshot = events;
        }
        assert(std::find(snapshot.begin(), snapshot.end(), lc::SchedulerEventType::Scheduled) != snapshot.end());
        assert(std::find(snapshot.begin(), snapshot.end(), lc::SchedulerEventType::Started) != snapshot.end());
        assert(std::find(snapshot.begin(), snapshot.end(), lc::SchedulerEventType::Completed) != snapshot.end());
        assert(!metrics->snapshots().empty());
        assert(!traces->spans().empty());
        assert(lc::schedulerEventName(lc::SchedulerEventType::Retrying) == "retrying");
    }

    {
        lc::TaskScheduler scheduler;
        std::atomic<int> count { 0 };

        auto scheduled = scheduler.scheduleAfter(10ms, [&] {
            ++count;
        });
        assert(scheduled.isOk());
        auto task = scheduled.value();
        auto closed = scheduler.close(lc::SchedulerCloseOptions {
            .timeout_ = 1s,
            .policy_ = lc::SchedulerClosePolicy::DrainPending,
        });
        assert(closed.isOk());
        assert(count == 1);
        assert(task.state() == lc::ScheduledTaskState::Completed);
        assert(lc::closePolicyName(lc::SchedulerClosePolicy::DrainPending) == "drain_pending");
    }

    {
        lc::TaskScheduler scheduler;
        std::atomic<int> count { 0 };
        auto pendingResult = scheduler.scheduleAfter(1h, [&] {
            ++count;
        });
        assert(pendingResult.isOk());
        auto pending = pendingResult.value();

        auto closed = scheduler.close(lc::SchedulerCloseOptions {
            .timeout_ = 100ms,
            .policy_ = lc::SchedulerClosePolicy::CancelPending,
        });
        assert(closed.isOk());
        assert(scheduler.isClosed());
        assert(count == 0);
        assert(pending.state() == lc::ScheduledTaskState::Cancelled);
        assert(pending.status().code() == lc::StatusCode::Cancelled);

        auto scheduled = scheduler.scheduleAfter(1ms, [] {});
        assert(!scheduled.isOk());
        assert(scheduled.status().code() == lc::StatusCode::FailedPrecondition);
    }

    {
        lc::TaskScheduler scheduler;
        std::atomic<int> count { 0 };
        auto pendingResult = scheduler.scheduleAfter(200ms, [&] {
            ++count;
        });
        assert(pendingResult.isOk());
        auto pending = pendingResult.value();

        auto closed = scheduler.close(lc::SchedulerCloseOptions {
            .timeout_ = 10ms,
            .policy_ = lc::SchedulerClosePolicy::DrainPending,
        });
        assert(closed.code() == lc::StatusCode::DeadlineExceeded);
        assert(pending.state() == lc::ScheduledTaskState::Cancelled);
        std::this_thread::sleep_for(250ms);
        assert(count == 0);
    }

    {
        lc::TaskScheduler scheduler;
        auto emptyTask = scheduler.scheduleAfter(1ms, {});
        assert(emptyTask.status().code() == lc::StatusCode::InvalidArgument);
        auto invalidPeriodic = scheduler.schedulePeriodic(0ms, [] {});
        assert(invalidPeriodic.status().code() == lc::StatusCode::InvalidArgument);
        auto invalidRetry = scheduler.scheduleRetry({}, {});
        assert(invalidRetry.status().code() == lc::StatusCode::InvalidArgument);
    }

    {
        auto executor = std::make_shared<RecordingExecutor>();
        lc::TaskScheduler scheduler(lc::SchedulerOptions {
            .executor_ = executor,
        });
        std::atomic<int> count { 0 };

        auto scheduled = scheduler.scheduleAfter(0ms, [&] {
            count.fetch_add(1, std::memory_order_relaxed);
        });
        assert(scheduled.isOk());
        assert(scheduler.waitIdle(1s).isOk());
        assert(executor->waitIdle(1s).isOk());
        assert(count.load(std::memory_order_relaxed) == 1);
        assert(executor->postCalls() >= 1);
        assert(executor->executeCalls() == 0);
        assert(executor->executeAndWaitCalls() == 0);
    }

    {
        auto executor = std::make_shared<RecordingExecutor>();
        assert(executor->close(1s).isOk());
        lc::TaskScheduler scheduler(lc::SchedulerOptions {
            .executor_ = executor,
        });

        auto scheduled = scheduler.scheduleAfter(0ms, [] {});
        assert(scheduled.isOk());
        auto task = scheduled.value();
        assert(scheduler.waitIdle(1s).isOk());
        assert(task.state() == lc::ScheduledTaskState::Failed);
        assert(task.status().code() == lc::StatusCode::Unavailable);
    }

    {
        lc::ManualClock clock(lc::Clock::TimePoint(100ms));
        lc::TaskScheduler scheduler(lc::SchedulerOptions {
            .clock_ = &clock,
        });
        std::atomic<int> count { 0 };

        auto delayedResult = scheduler.scheduleAt(clock.now() + 50ms, [&] {
            count.fetch_add(1, std::memory_order_relaxed);
        });
        assert(delayedResult.isOk());
        std::this_thread::sleep_for(20ms);
        assert(count.load(std::memory_order_relaxed) == 0);

        clock.advance(50ms);
        assert(scheduler.scheduleAfter(0ms, [] {}).isOk());
        assert(scheduler.waitIdle(1s).isOk());
        assert(count.load(std::memory_order_relaxed) == 1);
    }

    {
        lc::TaskScheduler scheduler;
        std::atomic<int> count { 0 };
        std::vector<std::thread> producers;
        producers.reserve(4);
        for (int i = 0; i < 4; ++i) {
            producers.emplace_back([&] {
                for (int j = 0; j < 10; ++j) {
                    auto scheduled = scheduler.scheduleAfter(1ms, [&] {
                        count.fetch_add(1, std::memory_order_relaxed);
                    });
                    assert(scheduled.isOk());
                }
            });
        }
        for (auto& producer : producers)
            producer.join();
        assert(scheduler.waitIdle(2s).isOk());
        assert(count.load(std::memory_order_relaxed) == 40);
    }

    return 0;
}
