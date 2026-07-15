#include "core/lifecycle/lifecycle_components.hpp"
#include "foundation/event/memory_event_sink.hpp"
#include "foundation/event/runtime_event.hpp"
#include "foundation/executor/concurrent_executor.hpp"
#include "foundation/lifecycle/lifecycle.hpp"
#include "foundation/scheduler/scheduler.hpp"
#include "foundation/storage/memory_storage.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class RecordingLifecycleComponent final : public lgc::ILifecycle {
public:
    RecordingLifecycleComponent(
        std::string name,
        std::vector<std::string>& startLog,
        std::vector<std::string>& closeLog,
        bool failClose = false,
        bool failStart = false,
        bool throwClose = false)
        : name_(std::move(name))
        , startLog_(startLog)
        , closeLog_(closeLog)
        , failClose_(failClose)
        , failStart_(failStart)
        , throwClose_(throwClose)
    {
    }

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

    [[nodiscard]] lgc::Status start() override
    {
        startLog_.push_back(name_);
        if (failStart_)
            return lgc::Status::internal("start failed: " + name_);
        return lgc::Status::ok();
    }

    [[nodiscard]] lgc::Status waitIdle(lgc::Clock::Duration) override { return lgc::Status::ok(); }

    [[nodiscard]] lgc::Status close(lgc::Clock::Duration) override
    {
        if (throwClose_)
            throw std::runtime_error("close threw: " + name_);
        closeLog_.push_back(name_);
        closed_ = true;
        if (failClose_)
            return lgc::Status::internal("close failed: " + name_);
        return lgc::Status::ok();
    }

    [[nodiscard]] bool isClosed() const noexcept override { return closed_; }

private:
    std::string name_;
    std::vector<std::string>& startLog_;
    std::vector<std::string>& closeLog_;
    bool failClose_ { false };
    bool failStart_ { false };
    bool throwClose_ { false };
    std::atomic<bool> closed_ { false };
};

class BlockingStartComponent final : public lgc::ILifecycle {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "blocking-start"; }

    [[nodiscard]] lgc::Status start() override
    {
        std::unique_lock lock(mutex_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] {
            return released_;
        });
        return lgc::Status::ok();
    }

    [[nodiscard]] lgc::Status waitIdle(lgc::Clock::Duration) override { return lgc::Status::ok(); }

    [[nodiscard]] lgc::Status close(lgc::Clock::Duration) override
    {
        closed_.store(true, std::memory_order_relaxed);
        return lgc::Status::ok();
    }

    [[nodiscard]] bool isClosed() const noexcept override { return closed_.load(std::memory_order_relaxed); }

    void waitUntilEntered()
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] {
            return entered_;
        });
    }

    void release()
    {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool entered_ { false };
    bool released_ { false };
    std::atomic<bool> closed_ { false };
};

class BlockingCloseComponent final : public lgc::ILifecycle {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "blocking-close"; }
    [[nodiscard]] lgc::Status start() override { return lgc::Status::ok(); }
    [[nodiscard]] lgc::Status waitIdle(lgc::Clock::Duration) override { return lgc::Status::ok(); }

    [[nodiscard]] lgc::Status close(lgc::Clock::Duration) override
    {
        std::unique_lock lock(mutex_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] {
            return released_;
        });
        closed_.store(true, std::memory_order_relaxed);
        return lgc::Status::ok();
    }

    [[nodiscard]] bool isClosed() const noexcept override { return closed_.load(std::memory_order_relaxed); }

    void waitUntilEntered()
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] {
            return entered_;
        });
    }

    void release()
    {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool entered_ { false };
    bool released_ { false };
    std::atomic<bool> closed_ { false };
};

class CloseBudgetComponent final : public lgc::ILifecycle {
public:
    CloseBudgetComponent(
        std::string name,
        lgc::Clock::Duration sleep,
        std::vector<lgc::Clock::Duration>& seenTimeouts,
        std::mutex& seenMutex)
        : name_(std::move(name))
        , sleep_(sleep)
        , seenTimeouts_(seenTimeouts)
        , seenMutex_(seenMutex)
    {
    }

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    [[nodiscard]] lgc::Status start() override { return lgc::Status::ok(); }
    [[nodiscard]] lgc::Status waitIdle(lgc::Clock::Duration) override { return lgc::Status::ok(); }

    [[nodiscard]] lgc::Status close(lgc::Clock::Duration timeout) override
    {
        {
            std::lock_guard lock(seenMutex_);
            seenTimeouts_.push_back(timeout);
        }
        std::this_thread::sleep_for(sleep_);
        closed_.store(true, std::memory_order_relaxed);
        return lgc::Status::ok();
    }

    [[nodiscard]] bool isClosed() const noexcept override { return closed_.load(std::memory_order_relaxed); }

private:
    std::string name_;
    lgc::Clock::Duration sleep_;
    std::vector<lgc::Clock::Duration>& seenTimeouts_;
    std::mutex& seenMutex_;
    std::atomic<bool> closed_ { false };
};

} // namespace

int main()
{
    using namespace std::chrono_literals;

    {
        lgc::Lifecycle manager;
        std::vector<std::string> startLog;
        std::vector<std::string> closeLog;

        assert(manager.add(std::make_shared<RecordingLifecycleComponent>(
                   "storage",
                   startLog,
                   closeLog))
            .isOk());
        assert(manager.add(std::make_shared<RecordingLifecycleComponent>(
                   "events",
                   startLog,
                   closeLog))
            .isOk());
        assert(manager.add(std::make_shared<RecordingLifecycleComponent>(
                   "executor",
                   startLog,
                   closeLog))
            .isOk());
        auto duplicate = manager.add(std::make_shared<RecordingLifecycleComponent>(
            "executor",
            startLog,
            closeLog));
        assert(!duplicate.isOk());
        assert(duplicate.code() == lgc::StatusCode::AlreadyExists);

        assert(manager.start().isOk());
        assert((startLog == std::vector<std::string> { "storage", "events", "executor" }));
        assert(manager.waitIdle(100ms).isOk());

        assert(manager.close(lgc::CloseOptions {
                   .timeout_ = 100ms,
               })
                   .isOk());
        assert((closeLog == std::vector<std::string> { "executor", "events", "storage" }));
        assert(manager.isClosed());
        assert(manager.state() == lgc::LifecycleState::Closed);
        assert(lgc::stateName(manager.state()) == "closed");

        const auto snapshot = manager.components();
        assert(snapshot.size() == 3);
        assert(std::all_of(snapshot.begin(), snapshot.end(), [](const auto& item) {
            return item.closed_ && item.state_ == lgc::LifecycleState::Closed;
        }));
    }

    {
        lgc::Lifecycle manager;
        std::vector<std::string> startLog;
        std::vector<std::string> closeLog;

        assert(manager.add(std::make_shared<RecordingLifecycleComponent>(
                   "first",
                   startLog,
                   closeLog))
            .isOk());
        assert(manager.add(std::make_shared<RecordingLifecycleComponent>(
                   "failing",
                   startLog,
                   closeLog,
                   true))
            .isOk());
        assert(manager.add(std::make_shared<RecordingLifecycleComponent>(
                   "last",
                   startLog,
                   closeLog))
            .isOk());

        assert(manager.start().isOk());
        auto status = manager.close(lgc::CloseOptions {
            .timeout_ = 100ms,
            .continueOnError_ = true,
        });
        assert(!status.isOk());
        assert(status.code() == lgc::StatusCode::Internal);
        assert(manager.state() == lgc::LifecycleState::Failed);
        assert((closeLog == std::vector<std::string> { "last", "failing", "first" }));
    }

    {
        lgc::Lifecycle manager;
        std::vector<std::string> startLog;
        std::vector<std::string> closeLog;

        assert(manager.add(std::make_shared<RecordingLifecycleComponent>(
                   "first",
                   startLog,
                   closeLog))
            .isOk());
        assert(manager.add(std::make_shared<RecordingLifecycleComponent>(
                   "failing-start",
                   startLog,
                   closeLog,
                   false,
                   true))
            .isOk());
        assert(manager.add(std::make_shared<RecordingLifecycleComponent>(
                   "never-started",
                   startLog,
                   closeLog))
            .isOk());

        auto status = manager.start();
        assert(!status.isOk());
        assert(status.code() == lgc::StatusCode::Internal);
        assert((startLog == std::vector<std::string> { "first", "failing-start" }));
        assert((closeLog == std::vector<std::string> { "first" }));
        assert(manager.state() == lgc::LifecycleState::Failed);

        const auto snapshot = manager.components();
        auto first = std::find_if(snapshot.begin(), snapshot.end(), [](const auto& item) {
            return item.name_ == "first";
        });
        auto failing = std::find_if(snapshot.begin(), snapshot.end(), [](const auto& item) {
            return item.name_ == "failing-start";
        });
        auto neverStarted = std::find_if(snapshot.begin(), snapshot.end(), [](const auto& item) {
            return item.name_ == "never-started";
        });
        assert(first != snapshot.end());
        assert(failing != snapshot.end());
        assert(neverStarted != snapshot.end());
        assert(first->state_ == lgc::LifecycleState::Closed);
        assert(failing->state_ == lgc::LifecycleState::Failed);
        assert(neverStarted->state_ == lgc::LifecycleState::Created);
        assert(manager.close(lgc::CloseOptions { .timeout_ = 100ms }).isOk());
    }

    {
        lgc::Lifecycle manager;
        auto blocker = std::make_shared<BlockingStartComponent>();
        assert(manager.add(blocker).isOk());

        auto startFuture = std::async(std::launch::async, [&] {
            return manager.start();
        });
        blocker->waitUntilEntered();

        auto secondStart = std::async(std::launch::async, [&] {
            return manager.start();
        });
        std::this_thread::sleep_for(20ms);
        assert(secondStart.wait_for(0ms) == std::future_status::timeout);
        assert(manager.state() == lgc::LifecycleState::Starting);

        auto closeDuringStart = manager.close(lgc::CloseOptions {
            .timeout_ = 10ms,
        });
        assert(!closeDuringStart.isOk());
        assert(closeDuringStart.code() == lgc::StatusCode::DeadlineExceeded);

        blocker->release();
        assert(startFuture.get().isOk());
        assert(secondStart.get().isOk());
        assert(manager.close(lgc::CloseOptions { .timeout_ = 1s }).isOk());
    }

    {
        lgc::Lifecycle manager;
        auto blocker = std::make_shared<BlockingCloseComponent>();
        assert(manager.add(blocker).isOk());
        assert(manager.start().isOk());

        auto closeFuture = std::async(std::launch::async, [&] {
            return manager.close(lgc::CloseOptions {
                .timeout_ = 1s,
            });
        });
        blocker->waitUntilEntered();

        auto concurrentClose = manager.close(lgc::CloseOptions {
            .timeout_ = 10ms,
        });
        assert(!concurrentClose.isOk());
        assert(concurrentClose.code() == lgc::StatusCode::DeadlineExceeded);

        blocker->release();
        assert(closeFuture.get().isOk());
        assert(manager.close(lgc::CloseOptions { .timeout_ = 10ms }).isOk());
    }

    {
        lgc::Lifecycle manager;
        std::vector<lgc::Clock::Duration> seenTimeouts;
        std::mutex seenMutex;

        assert(manager.add(std::make_shared<CloseBudgetComponent>(
                   "first",
                   0ms,
                   seenTimeouts,
                   seenMutex))
            .isOk());
        assert(manager.add(std::make_shared<CloseBudgetComponent>(
                   "second",
                   30ms,
                   seenTimeouts,
                   seenMutex))
            .isOk());

        assert(manager.start().isOk());
        assert(manager.close(lgc::CloseOptions { .timeout_ = 200ms }).isOk());
        assert(seenTimeouts.size() == 2);
        assert(seenTimeouts[1] < seenTimeouts[0]);
    }

    {
        lgc::Lifecycle manager;
        std::vector<std::string> startLog;
        std::vector<std::string> closeLog;
        assert(manager.add(std::make_shared<RecordingLifecycleComponent>(
                   "throwing-close",
                   startLog,
                   closeLog,
                   false,
                   false,
                   true))
            .isOk());
        assert(manager.start().isOk());
        auto status = manager.close(lgc::CloseOptions { .timeout_ = 100ms });
        assert(!status.isOk());
        assert(status.code() == lgc::StatusCode::Internal);
        assert(manager.state() == lgc::LifecycleState::Failed);
        const auto snapshot = manager.components();
        assert(snapshot.size() == 1);
        assert(snapshot.front().state_ == lgc::LifecycleState::Failed);
        assert(!snapshot.front().status_.isOk());
    }

    {
        auto executor = lgc::makeConcurrentExecutor(1);
        auto scheduler = std::make_shared<lgc::TaskScheduler>();
        auto sink = std::make_shared<lgc::MemoryEventSink>();
        auto storage = std::make_shared<lgc::MemoryStorage>();

        lgc::Lifecycle manager;
        assert(manager.add(lgc::makeLifecycleComponent("storage", storage)).isOk());
        assert(manager.add(lgc::makeLifecycleComponent("events", sink)).isOk());
        assert(manager.add(lgc::makeLifecycleComponent("executor", executor)).isOk());
        assert(manager.add(lgc::makeLifecycleComponent("scheduler", scheduler)).isOk());

        assert(manager.start().isOk());

        std::atomic<int> count { 0 };
        assert(executor->execute([&] {
            count.fetch_add(1, std::memory_order_relaxed);
        })
                   .isOk());

        assert(scheduler
                   ->scheduleAfter(5ms, [&] {
                       count.fetch_add(1, std::memory_order_relaxed);
                   })
                   .isOk());

        auto event = lgc::RuntimeEvent::create(lgc::RuntimeEventType::Custom);
        event.name_ = "lifecycle.note";
        assert(sink->publish(std::move(event)).isOk());
        assert(manager.waitIdle(1s).isOk());
        assert(count.load(std::memory_order_relaxed) == 2);

        assert(manager.close(lgc::CloseOptions {
                   .timeout_ = 1s,
               })
                   .isOk());
        assert(manager.isClosed());
        assert(executor->isClosed());
        assert(scheduler->isClosed());
        assert(sink->isClosed());

        const auto snapshot = manager.components();
        assert(snapshot.size() == 4);
        auto storageStatus = std::find_if(snapshot.begin(), snapshot.end(), [](const auto& item) {
            return item.name_ == "storage";
        });
        assert(storageStatus != snapshot.end());
        assert(storageStatus->closed_);
    }

    return 0;
}
