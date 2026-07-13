#include "foundation/threading/thread.hpp"

#include "foundation/logging/logger.hpp"
#include "foundation/threading/thread_context.hpp"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>

namespace lc {
namespace {

thread_local const void* tlsThreadSelf = nullptr;

struct WorkerScope {
    explicit WorkerScope(const void* self, std::string_view name) noexcept
    {
        tlsThreadSelf = self;
        ThreadContext::setCurrentThreadName(name);
    }

    ~WorkerScope() { tlsThreadSelf = nullptr; }
};

[[nodiscard]] bool hasSourceLocation(const std::source_location& loc) noexcept
{
    return loc.line() != 0 && loc.file_name() != nullptr && loc.file_name()[0] != '\0';
}

void logTaskException(
    const std::shared_ptr<ILogger>& logger,
    std::string_view label,
    const std::source_location& from,
    const std::exception* ex) noexcept
{
    try {
        if (hasSourceLocation(from)) {
            if (ex != nullptr) {
                logTo(logger,
                    LogLevel::Warn,
                    "Thread",
                    "{} exception at {}:{} `{}`: {}",
                    __FILE__,
                    __LINE__,
                    label,
                    from.file_name(),
                    from.line(),
                    from.function_name(),
                    ex->what());
            } else {
                logTo(logger,
                    LogLevel::Warn,
                    "Thread",
                    "{} exception (non-std) at {}:{} `{}`",
                    __FILE__,
                    __LINE__,
                    label,
                    from.file_name(),
                    from.line(),
                    from.function_name());
            }
        } else if (ex != nullptr) {
            logTo(logger, LogLevel::Warn, "Thread", "{} exception: {}", __FILE__, __LINE__, label, ex->what());
        } else {
            logTo(logger, LogLevel::Warn, "Thread", "{} exception: (non-std)", __FILE__, __LINE__, label);
        }
    } catch (...) {
    }
}

} // namespace

ThreadStopped::ThreadStopped()
    : std::runtime_error("thread is stopped")
{
}

struct ScheduledWork {
    std::chrono::steady_clock::time_point due;
    std::uint64_t sequence { 0 };
    std::function<void()> task;
    std::source_location from;

    [[nodiscard]] bool operator>(const ScheduledWork& other) const noexcept
    {
        if (due == other.due)
            return sequence > other.sequence;
        return due > other.due;
    }
};

struct Thread::Impl {
    Impl(std::string name, std::size_t maxPendingDispatch, std::shared_ptr<ILogger> logger)
        : name_(std::move(name))
        , maxPendingDispatch_(maxPendingDispatch)
        , logger_(std::move(logger))
    {
        if (name_.empty())
            name_ = "lc-thread";
        running_ = true;
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~Impl() { (void)shutdown(std::chrono::steady_clock::duration::max()); }

    [[nodiscard]] bool onWorker() const noexcept
    {
        return tlsThreadSelf == static_cast<const void*>(this);
    }

    void dispatchAsync(std::function<void()> task, std::source_location from)
    {
        if (!enqueue(std::chrono::steady_clock::now(), std::move(task), from))
            asyncTasksDroppedWhileStopped_.fetch_add(1, std::memory_order_relaxed);
    }

    void dispatchAfter(
        std::chrono::steady_clock::duration delay,
        std::function<void()> task,
        std::source_location from)
    {
        const auto due = delay <= std::chrono::steady_clock::duration::zero()
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::now() + delay;
        if (!enqueue(due, std::move(task), from))
            asyncTasksDroppedWhileStopped_.fetch_add(1, std::memory_order_relaxed);
    }

    void dispatch(std::function<void()> task, std::source_location from)
    {
        if (onWorker()) {
            runTask(std::move(task), from);
            return;
        }
        dispatchAsync(std::move(task), from);
    }

    void dispatchSync(std::function<void()> task, std::source_location from)
    {
        if (!task)
            return;
        if (onWorker()) {
            runTask(std::move(task), from);
            return;
        }

        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        const auto accepted = enqueue(
            std::chrono::steady_clock::now(),
            [task = std::move(task), promise, from, this]() mutable {
                try {
                    runTask(std::move(task), from);
                    promise->set_value();
                } catch (...) {
                    try {
                        promise->set_exception(std::current_exception());
                    } catch (...) {
                    }
                }
            },
            from);
        if (!accepted) {
            dispatchSyncRejectedWhileStopped_.fetch_add(1, std::memory_order_relaxed);
            throw ThreadStopped();
        }

        future.get();
    }

    [[nodiscard]] Status waitIdle(std::chrono::steady_clock::duration timeout)
    {
        if (onWorker()) {
            waitIdleEarlyReturnFromExecutorThread_.fetch_add(1, std::memory_order_relaxed);
            return Status::failedPrecondition("waitIdle cannot be called from the executor thread");
        }

        std::unique_lock lock(mutex_);
        if (timeout <= std::chrono::steady_clock::duration::zero()) {
            const bool ok = outstanding_ == 0;
            if (!ok)
                waitIdleTimeouts_.fetch_add(1, std::memory_order_relaxed);
            return ok ? Status::ok() : Status::deadlineExceeded("thread did not become idle before timeout");
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        if (!idleCv_.wait_until(lock, deadline, [this] { return outstanding_ == 0; })) {
            waitIdleTimeouts_.fetch_add(1, std::memory_order_relaxed);
            return Status::deadlineExceeded("thread did not become idle before timeout");
        }
        return Status::ok();
    }

    [[nodiscard]] Status shutdown(std::chrono::steady_clock::duration waitIdleTimeout)
    {
        if (onWorker()) {
            shutdownIdleWaitTimeouts_.fetch_add(1, std::memory_order_relaxed);
            try {
                std::thread([this] { (void)shutdown(std::chrono::steady_clock::duration::max()); }).detach();
            } catch (...) {
                std::terminate();
            }
            return Status::failedPrecondition("shutdown cannot join the executor thread from itself");
        }

        {
            std::lock_guard lock(mutex_);
            if (!running_ && !worker_.joinable())
                return Status::ok();
            running_ = false;
        }

        Status idle = Status::ok();
        if (waitIdleTimeout == std::chrono::steady_clock::duration::max()) {
            std::unique_lock lock(mutex_);
            idleCv_.wait(lock, [this] { return outstanding_ == 0; });
        } else {
            idle = waitIdle(waitIdleTimeout);
            if (!idle.isOk())
                shutdownIdleWaitTimeouts_.fetch_add(1, std::memory_order_relaxed);
        }

        {
            std::lock_guard lock(mutex_);
            while (!queue_.empty()) {
                queue_.pop();
                if (outstanding_ > 0)
                    --outstanding_;
            }
            stopping_ = true;
        }
        cv_.notify_all();
        idleCv_.notify_all();

        if (worker_.joinable())
            worker_.join();
        return idle;
    }

    [[nodiscard]] bool isRunning() const noexcept
    {
        std::lock_guard lock(mutex_);
        return running_ && !stopping_;
    }

    [[nodiscard]] ThreadStats stats() const noexcept
    {
        ThreadStats s;
        s.asyncTasksDroppedWhileStopped_ = asyncTasksDroppedWhileStopped_.load(std::memory_order_relaxed);
        s.scheduleExceptions_ = scheduleExceptions_.load(std::memory_order_relaxed);
        s.dispatchedTaskExceptions_ = dispatchedTaskExceptions_.load(std::memory_order_relaxed);
        s.dispatchSyncRejectedWhileStopped_ = dispatchSyncRejectedWhileStopped_.load(std::memory_order_relaxed);
        s.workerLoopExceptions_ = workerLoopExceptions_.load(std::memory_order_relaxed);
        s.dispatchAccepted_ = dispatchAccepted_.load(std::memory_order_relaxed);
        s.dispatchRejectedQueueFull_ = dispatchRejectedQueueFull_.load(std::memory_order_relaxed);
        s.tasksCompleted_ = tasksCompleted_.load(std::memory_order_relaxed);
        s.waitIdleTimeouts_ = waitIdleTimeouts_.load(std::memory_order_relaxed);
        s.waitIdleEarlyReturnFromExecutorThread_ = waitIdleEarlyReturnFromExecutorThread_.load(std::memory_order_relaxed);
        s.shutdownIdleWaitTimeouts_ = shutdownIdleWaitTimeouts_.load(std::memory_order_relaxed);
        {
            std::lock_guard lock(mutex_);
            s.peakOutstanding_ = peakOutstanding_;
        }
        return s;
    }

private:
    [[nodiscard]] bool enqueue(
        std::chrono::steady_clock::time_point due,
        std::function<void()> task,
        std::source_location from)
    {
        if (!task)
            return false;

        {
            std::lock_guard lock(mutex_);
            if (!running_ || stopping_)
                return false;
            if (maxPendingDispatch_ != 0U && outstanding_ >= maxPendingDispatch_) {
                dispatchRejectedQueueFull_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            queue_.push(ScheduledWork {
                .due = due,
                .sequence = nextSequence_++,
                .task = std::move(task),
                .from = from,
            });
            ++outstanding_;
            if (outstanding_ > peakOutstanding_)
                peakOutstanding_ = outstanding_;
        }
        dispatchAccepted_.fetch_add(1, std::memory_order_relaxed);
        cv_.notify_all();
        return true;
    }

    void workerLoop() noexcept
    {
        WorkerScope scope(this, name_);

        for (;;) {
            ScheduledWork work;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });

                if (stopping_ && queue_.empty())
                    break;

                const auto due = queue_.top().due;
                if (due > std::chrono::steady_clock::now()) {
                    cv_.wait_until(lock, due);
                    continue;
                }

                work = std::move(const_cast<ScheduledWork&>(queue_.top()));
                queue_.pop();
            }

            try {
                runTask(std::move(work.task), work.from);
            } catch (...) {
                workerLoopExceptions_.fetch_add(1, std::memory_order_relaxed);
            }

            {
                std::lock_guard lock(mutex_);
                if (outstanding_ > 0)
                    --outstanding_;
            }
            idleCv_.notify_all();
        }
    }

    void runTask(std::function<void()> task, std::source_location from) noexcept
    {
        if (!task)
            return;
        try {
            task();
            tasksCompleted_.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& ex) {
            dispatchedTaskExceptions_.fetch_add(1, std::memory_order_relaxed);
            logTaskException(logger_, "task", from, &ex);
        } catch (...) {
            dispatchedTaskExceptions_.fetch_add(1, std::memory_order_relaxed);
            logTaskException(logger_, "task", from, nullptr);
        }
    }

    std::string name_;
    const std::size_t maxPendingDispatch_;
    std::shared_ptr<ILogger> logger_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable idleCv_;
    std::priority_queue<ScheduledWork, std::vector<ScheduledWork>, std::greater<ScheduledWork>> queue_;
    std::thread worker_;
    bool running_ { false };
    bool stopping_ { false };
    std::uint64_t outstanding_ { 0 };
    std::uint64_t peakOutstanding_ { 0 };
    std::uint64_t nextSequence_ { 1 };

    std::atomic<std::uint64_t> asyncTasksDroppedWhileStopped_ { 0 };
    std::atomic<std::uint64_t> scheduleExceptions_ { 0 };
    std::atomic<std::uint64_t> dispatchedTaskExceptions_ { 0 };
    std::atomic<std::uint64_t> dispatchSyncRejectedWhileStopped_ { 0 };
    std::atomic<std::uint64_t> workerLoopExceptions_ { 0 };
    std::atomic<std::uint64_t> dispatchAccepted_ { 0 };
    std::atomic<std::uint64_t> dispatchRejectedQueueFull_ { 0 };
    std::atomic<std::uint64_t> tasksCompleted_ { 0 };
    std::atomic<std::uint64_t> waitIdleTimeouts_ { 0 };
    std::atomic<std::uint64_t> waitIdleEarlyReturnFromExecutorThread_ { 0 };
    std::atomic<std::uint64_t> shutdownIdleWaitTimeouts_ { 0 };
};

Thread::Thread(std::size_t maxPendingDispatch, std::shared_ptr<ILogger> logger)
    : Thread("lc-thread", maxPendingDispatch, std::move(logger))
{
}

Thread::Thread(std::string_view name, std::size_t maxPendingDispatch, std::shared_ptr<ILogger> logger)
    : impl_(std::make_shared<Impl>(std::string(name), maxPendingDispatch, std::move(logger)))
{
}

Thread::~Thread()
{
    if (!impl_)
        return;

    if (impl_->onWorker()) {
        auto keep = std::move(impl_);
        impl_.reset();
        try {
            std::thread([keep = std::move(keep)]() mutable {
                (void)keep->shutdown(std::chrono::steady_clock::duration::max());
            }).detach();
        } catch (...) {
            std::terminate();
        }
        return;
    }

    impl_.reset();
}

void Thread::dispatchAsync(std::function<void()> task, std::source_location from)
{
    impl_->dispatchAsync(std::move(task), from);
}

void Thread::dispatchAfter(
    std::chrono::steady_clock::duration delay,
    std::function<void()> task,
    std::source_location from)
{
    impl_->dispatchAfter(delay, std::move(task), from);
}

void Thread::dispatch(std::function<void()> task, std::source_location from)
{
    impl_->dispatch(std::move(task), from);
}

void Thread::dispatchSync(std::function<void()> task, std::source_location from)
{
    impl_->dispatchSync(std::move(task), from);
}

Status Thread::waitIdle(std::chrono::steady_clock::duration timeout)
{
    return impl_->waitIdle(timeout);
}

Status Thread::shutdown(std::chrono::steady_clock::duration waitIdleTimeout)
{
    return impl_->shutdown(waitIdleTimeout);
}

bool Thread::isRunning() const noexcept
{
    return impl_->isRunning();
}

bool Thread::isCurrentThread() const noexcept
{
    return impl_->onWorker();
}

ThreadStats Thread::stats() const noexcept
{
    return impl_->stats();
}

} // namespace lc
