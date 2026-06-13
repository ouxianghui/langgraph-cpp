#include "foundation/threading/thread.hpp"

#include "foundation/logging/logger.hpp"
#include "foundation/threading/thread_context.hpp"

#include <asio/dispatch.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>

#include <atomic>
#include <condition_variable>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace lc {

namespace {

void warn(std::string_view message)
{
    try {
        BW_LOG_WARN("Thread", "{}", message);
    } catch (...) {
    }
}

[[nodiscard]] bool hasSourceLocation(const std::source_location& loc) noexcept
{
    return loc.line() != 0 && loc.file_name() != nullptr && loc.file_name()[0] != '\0';
}

void logUncaughtDispatch(const std::source_location& from, const std::exception* ex) noexcept
{
    try {
        if (hasSourceLocation(from)) {
            if (ex != nullptr) {
                BW_LOG_WARN("Thread",
                    "dispatched task threw std::exception at {}:{} `{}`: {}",
                    from.file_name(),
                    from.line(),
                    from.function_name(),
                    ex->what());
            } else {
                BW_LOG_WARN("Thread",
                    "dispatched task threw a non-std exception at {}:{} `{}`",
                    from.file_name(),
                    from.line(),
                    from.function_name());
            }
        } else if (ex != nullptr) {
            warn(std::string("dispatched task threw std::exception: ") + ex->what());
        } else {
            warn("dispatched task threw a non-std exception");
        }
    } catch (...) {
    }
}

} // namespace

ThreadStopped::ThreadStopped()
    : std::runtime_error("Thread is stopped")
{
}

struct DispatchedWork {
    std::function<void()> fn;
    std::source_location from;
};

struct Thread::Impl : public std::enable_shared_from_this<Impl> {
    struct Stats {
        std::atomic<uint64_t> asyncTasksDroppedWhileStopped_ { 0 };
        std::atomic<uint64_t> scheduleExceptions_ { 0 };
        std::atomic<uint64_t> dispatchedTaskExceptions_ { 0 };
        std::atomic<uint64_t> dispatchSyncRejectedWhileStopped_ { 0 };
        std::atomic<uint64_t> workerLoopExceptions_ { 0 };
        std::atomic<uint64_t> dispatchAccepted_ { 0 };
        std::atomic<uint64_t> dispatchRejectedQueueFull_ { 0 };
        std::atomic<uint64_t> tasksCompleted_ { 0 };
        std::atomic<uint64_t> waitIdleTimeouts_ { 0 };
        std::atomic<uint64_t> waitIdleEarlyReturnFromExecutorThread_ { 0 };
        std::atomic<uint64_t> shutdownIdleWaitTimeouts_ { 0 };
    } stats_;

    std::atomic<bool> running_ { false };

    std::unique_ptr<asio::io_context> ioLoop_;
    std::optional<asio::strand<asio::io_context::executor_type>> strand_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> workGuard_;
    std::thread worker_;
    std::thread::id workerThreadId_ {};
    const std::string name_;
    const std::size_t maxPendingDispatch_;

    mutable std::mutex mutex_;
    std::condition_variable joinerCv_;
    std::condition_variable idleCv_;
    bool joinerBusy_ { false };
    std::thread teardownJoiner_{};
    std::uint64_t outstanding_ { 0 };
    std::uint64_t peakOutstanding_ { 0 };

    explicit Impl(std::string name, std::size_t maxPendingDispatch);
    ~Impl();

    void runWorkerLoop();

    [[nodiscard]] asio::any_io_executor timerExecutor() const;

    [[nodiscard]] bool enqueueAsync(std::function<void()> task, std::source_location from);
    void enqueueDispatch(std::function<void()> task, std::source_location from);
    void dispatchSync(std::function<void()> task, std::source_location from);
    void dispatchAfter(
        std::chrono::steady_clock::duration delay, std::function<void()> task, std::source_location from);
    void shutdown() noexcept;

    [[nodiscard]] bool doShutdown(bool drainForever, std::chrono::steady_clock::duration drainBudget) noexcept;

    void awaitDrain(bool forever, std::chrono::steady_clock::duration timeout) noexcept;
    [[nodiscard]] bool awaitDrainFor(bool forever, std::chrono::steady_clock::duration timeout) noexcept;

    void tearDown() noexcept;

    [[nodiscard]] bool tryEnqueue(const std::shared_ptr<DispatchedWork>& work, bool asDispatch);

    void runInlineDispatch(std::function<void()> task, std::source_location from) noexcept;
    void runTask(const std::shared_ptr<DispatchedWork>& work) noexcept;

    [[nodiscard]] bool onExecutor() const noexcept
    {
        if (strand_.has_value() && strand_->running_in_this_thread()) {
            return true;
        }
        return isLoopThread();
    }

    void joinDeferredTeardown() noexcept;
    void stopContextJoinWorker() noexcept;

    [[nodiscard]] bool isRunning() const noexcept
    {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] asio::io_context* ioContext() noexcept { return ioLoop_.get(); }

    [[nodiscard]] bool isLoopThread() const noexcept
    {
        return std::this_thread::get_id() == workerThreadId_;
    }

    [[nodiscard]] ThreadStats snapshotStats() const noexcept
    {
        ThreadStats s;
        s.asyncTasksDroppedWhileStopped_ = stats_.asyncTasksDroppedWhileStopped_.load(std::memory_order_relaxed);
        s.scheduleExceptions_ = stats_.scheduleExceptions_.load(std::memory_order_relaxed);
        s.dispatchedTaskExceptions_ = stats_.dispatchedTaskExceptions_.load(std::memory_order_relaxed);
        s.dispatchSyncRejectedWhileStopped_ = stats_.dispatchSyncRejectedWhileStopped_.load(std::memory_order_relaxed);
        s.workerLoopExceptions_ = stats_.workerLoopExceptions_.load(std::memory_order_relaxed);
        s.dispatchAccepted_ = stats_.dispatchAccepted_.load(std::memory_order_relaxed);
        s.dispatchRejectedQueueFull_ = stats_.dispatchRejectedQueueFull_.load(std::memory_order_relaxed);
        s.tasksCompleted_ = stats_.tasksCompleted_.load(std::memory_order_relaxed);
        s.waitIdleTimeouts_ = stats_.waitIdleTimeouts_.load(std::memory_order_relaxed);
        s.waitIdleEarlyReturnFromExecutorThread_ = stats_.waitIdleEarlyReturnFromExecutorThread_.load(
            std::memory_order_relaxed);
        s.shutdownIdleWaitTimeouts_ = stats_.shutdownIdleWaitTimeouts_.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            s.peakOutstanding_ = peakOutstanding_;
        }
        return s;
    }
};

void Thread::Impl::joinDeferredTeardown() noexcept
{
    if (isLoopThread()) {
        return;
    }
    for (;;) {
        std::thread joiner;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!teardownJoiner_.joinable()) {
                break;
            }
            joiner = std::move(teardownJoiner_);
        }
        if (joiner.joinable()) {
            joiner.join();
        }
    }
}

void Thread::Impl::stopContextJoinWorker() noexcept
{
    workGuard_.reset();
    if (ioLoop_) {
        ioLoop_->stop();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    if (ioLoop_) {
        ioLoop_->restart();
    }
    try {
        BW_LOG_INFO("Thread", "serial shutdown complete");
    } catch (...) {
    }
}

asio::any_io_executor Thread::Impl::timerExecutor() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ioLoop_->get_executor();
}

Thread::Impl::Impl(std::string name, std::size_t maxPendingDispatch)
    : name_(std::move(name))
    , maxPendingDispatch_(maxPendingDispatch)
{
    running_.store(true, std::memory_order_release);

    ioLoop_ = std::make_unique<asio::io_context>();
    strand_.emplace(ioLoop_->get_executor());
    workGuard_.emplace(asio::make_work_guard(ioLoop_->get_executor()));
    worker_ = std::thread([this] { runWorkerLoop(); });
    try {
        if (maxPendingDispatch_ == 0U) {
            BW_LOG_INFO("Thread", "created name={} maxPendingDispatch=unbounded", name_);
        } else {
            BW_LOG_INFO("Thread", "created name={} maxPendingDispatch={}", name_, maxPendingDispatch_);
        }
    } catch (...) {
    }
}

Thread::Impl::~Impl() { (void)doShutdown(true, std::chrono::steady_clock::duration {}); }

void Thread::Impl::runTask(const std::shared_ptr<DispatchedWork>& work) noexcept
{
    try {
        if (work && work->fn) {
            work->fn();
            stats_.tasksCompleted_.fetch_add(1, std::memory_order_relaxed);
        }
    } catch (const std::exception& ex) {
        stats_.dispatchedTaskExceptions_.fetch_add(1, std::memory_order_relaxed);
        logUncaughtDispatch(work ? work->from : std::source_location {}, &ex);
    } catch (...) {
        stats_.dispatchedTaskExceptions_.fetch_add(1, std::memory_order_relaxed);
        logUncaughtDispatch(work ? work->from : std::source_location {}, nullptr);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (outstanding_ > 0ULL) {
            --outstanding_;
        }
        idleCv_.notify_all();
    }
}

void Thread::Impl::runWorkerLoop()
{
    workerThreadId_ = std::this_thread::get_id();
    ThreadContext::setCurrentThreadName(name_);
    try {
        if (maxPendingDispatch_ == 0U) {
            BW_LOG_INFO("Thread", "started name={} maxPendingDispatch=unbounded", name_);
        } else {
            BW_LOG_INFO("Thread", "started name={} maxPendingDispatch={}", name_, maxPendingDispatch_);
        }
    } catch (...) {
    }
    for (;;) {
        try {
            ioLoop_->run();
            return;
        } catch (const std::exception& ex) {
            stats_.workerLoopExceptions_.fetch_add(1, std::memory_order_relaxed);
            try {
                BW_LOG_WARN("Thread", "io_context::run std::exception (retrying): {}", ex.what());
            } catch (...) {
            }
        } catch (...) {
            stats_.workerLoopExceptions_.fetch_add(1, std::memory_order_relaxed);
            try {
                BW_LOG_WARN("Thread", "io_context::run non-std exception (retrying)");
            } catch (...) {
            }
        }
    }
}

bool Thread::Impl::tryEnqueue(const std::shared_ptr<DispatchedWork>& work, bool asDispatch)
{
    if (!work || !work->fn) {
        return false;
    }
    std::shared_ptr<Impl> self = shared_from_this();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load(std::memory_order_relaxed)) {
            stats_.asyncTasksDroppedWhileStopped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (maxPendingDispatch_ != 0U && outstanding_ >= maxPendingDispatch_) {
            stats_.dispatchRejectedQueueFull_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ++outstanding_;
        if (outstanding_ > peakOutstanding_) {
            peakOutstanding_ = outstanding_;
        }
    }

    try {
        if (asDispatch) {
            asio::dispatch(*strand_, [self, work]() noexcept { self->runTask(work); });
        } else {
            asio::post(*strand_, [self, work]() noexcept { self->runTask(work); });
        }
        stats_.dispatchAccepted_.fetch_add(1, std::memory_order_relaxed);
        return true;
    } catch (const std::exception& ex) {
        stats_.scheduleExceptions_.fetch_add(1, std::memory_order_relaxed);
        try {
            BW_LOG_WARN("Thread", "schedule async task failed: {}", ex.what());
        } catch (...) {
        }
    } catch (...) {
        stats_.scheduleExceptions_.fetch_add(1, std::memory_order_relaxed);
        try {
            BW_LOG_WARN("Thread", "schedule async task failed (non-std exception)");
        } catch (...) {
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (outstanding_ > 0ULL) {
            --outstanding_;
        }
        idleCv_.notify_all();
    }
    return false;
}

bool Thread::Impl::enqueueAsync(std::function<void()> task, std::source_location from)
{
    return tryEnqueue(std::make_shared<DispatchedWork>(std::move(task), from), false);
}

void Thread::Impl::enqueueDispatch(std::function<void()> task, std::source_location from)
{
    if (onExecutor()) {
        runInlineDispatch(std::move(task), from);
        return;
    }
    (void)tryEnqueue(std::make_shared<DispatchedWork>(std::move(task), from), true);
}

void Thread::Impl::runInlineDispatch(std::function<void()> task, std::source_location from) noexcept
{
    if (!task) {
        return;
    }
    if (!running_.load(std::memory_order_acquire)) {
        stats_.asyncTasksDroppedWhileStopped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    try {
        task();
        stats_.tasksCompleted_.fetch_add(1, std::memory_order_relaxed);
    } catch (const std::exception& ex) {
        stats_.dispatchedTaskExceptions_.fetch_add(1, std::memory_order_relaxed);
        logUncaughtDispatch(from, &ex);
    } catch (...) {
        stats_.dispatchedTaskExceptions_.fetch_add(1, std::memory_order_relaxed);
        logUncaughtDispatch(from, nullptr);
    }
}

void Thread::Impl::dispatchSync(std::function<void()> task, std::source_location from)
{
    if (onExecutor()) {
        if (!running_.load(std::memory_order_acquire)) {
            stats_.dispatchSyncRejectedWhileStopped_.fetch_add(1, std::memory_order_relaxed);
            try {
                BW_LOG_WARN("Thread", "dispatchSync rejected: Thread is stopped");
            } catch (...) {
            }
            throw ThreadStopped {};
        }
        if (task) {
            try {
                task();
            } catch (const std::exception& ex) {
                stats_.dispatchedTaskExceptions_.fetch_add(1, std::memory_order_relaxed);
                logUncaughtDispatch(from, &ex);
                throw;
            } catch (...) {
                stats_.dispatchedTaskExceptions_.fetch_add(1, std::memory_order_relaxed);
                logUncaughtDispatch(from, nullptr);
                throw;
            }
        }
        return;
    }
    if (!running_.load(std::memory_order_acquire)) {
        stats_.dispatchSyncRejectedWhileStopped_.fetch_add(1, std::memory_order_relaxed);
        try {
            BW_LOG_WARN("Thread", "dispatchSync rejected: Thread is stopped");
        } catch (...) {
        }
        throw ThreadStopped {};
    }
    std::promise<void> done;
    auto fut = done.get_future();
    const bool ok = enqueueAsync(
        [&task, &done]() {
            try {
                if (task) {
                    task();
                }
                done.set_value();
            } catch (...) {
                done.set_exception(std::current_exception());
            }
        },
        from);
    if (!ok) {
        stats_.dispatchSyncRejectedWhileStopped_.fetch_add(1, std::memory_order_relaxed);
        try {
            BW_LOG_WARN("Thread", "dispatchSync rejected: could not enqueue (stopped)");
        } catch (...) {
        }
        throw ThreadStopped {};
    }
    try {
        fut.get();
    } catch (const std::exception& ex) {
        stats_.dispatchedTaskExceptions_.fetch_add(1, std::memory_order_relaxed);
        logUncaughtDispatch(from, &ex);
        throw;
    } catch (...) {
        stats_.dispatchedTaskExceptions_.fetch_add(1, std::memory_order_relaxed);
        logUncaughtDispatch(from, nullptr);
        throw;
    }
}

void Thread::Impl::dispatchAfter(std::chrono::steady_clock::duration delay,
    std::function<void()> task,
    std::source_location from)
{
    if (!running_.load(std::memory_order_acquire)) {
        stats_.asyncTasksDroppedWhileStopped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::shared_ptr<Impl> self = shared_from_this();
    auto timer = std::make_shared<asio::steady_timer>(timerExecutor());
    timer->expires_after(delay);
    timer->async_wait([self, timer, task = std::move(task), from](const asio::error_code& ec) mutable {
        if (ec || !self->running_.load(std::memory_order_acquire)) {
            return;
        }
        (void)self->enqueueAsync(std::move(task), from);
    });
}

void Thread::Impl::awaitDrain(bool forever, std::chrono::steady_clock::duration timeout) noexcept
{
    (void)awaitDrainFor(forever, timeout);
}

bool Thread::Impl::awaitDrainFor(bool forever, std::chrono::steady_clock::duration timeout) noexcept
{
    if (onExecutor()) {
        stats_.waitIdleEarlyReturnFromExecutorThread_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (forever) {
        idleCv_.wait(lock, [this] { return outstanding_ == 0ULL; });
        return true;
    }
    if (timeout <= std::chrono::steady_clock::duration::zero()) {
        const bool ok = (outstanding_ == 0ULL);
        if (!ok) {
            stats_.waitIdleTimeouts_.fetch_add(1, std::memory_order_relaxed);
        }
        return ok;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (!idleCv_.wait_until(lock, deadline, [this] { return outstanding_ == 0ULL; })) {
        stats_.waitIdleTimeouts_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void Thread::Impl::tearDown() noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!isLoopThread()) {
        joinerCv_.wait(lock, [this] { return !joinerBusy_; });
    }
    if (isLoopThread()) {
        if (teardownJoiner_.joinable()) {
            joinerBusy_ = false;
            joinerCv_.notify_all();
            try {
                BW_LOG_ERROR("Thread", "concurrent deferred teardown join (worker)");
            } catch (...) {
            }
            std::terminate();
        }
        joinerBusy_ = true;
        lock.unlock();

        std::shared_ptr<Impl> self = shared_from_this();
        std::thread helper([self]() mutable {
            self->stopContextJoinWorker();
            {
                std::lock_guard<std::mutex> gl(self->mutex_);
                self->joinerBusy_ = false;
            }
            self->joinerCv_.notify_all();
        });

        {
            std::lock_guard<std::mutex> gl(mutex_);
            teardownJoiner_ = std::move(helper);
            joinerBusy_ = false;
        }
        joinerCv_.notify_all();
        return;
    }
    lock.unlock();
    stopContextJoinWorker();
}

bool Thread::Impl::doShutdown(bool drainForever, std::chrono::steady_clock::duration drainBudget) noexcept
{
    joinDeferredTeardown();

    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        joinDeferredTeardown();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (worker_.joinable() && !isLoopThread()) {
                worker_.join();
            }
        }
        return true;
    }

    try {
        BW_LOG_INFO("Thread", "shutdown()");
    } catch (...) {
    }

    bool idleDrained = true;
    if (!onExecutor()) {
        idleDrained = awaitDrainFor(drainForever,
            drainForever ? std::chrono::steady_clock::duration {} : drainBudget);
        if (!drainForever && !idleDrained) {
            stats_.shutdownIdleWaitTimeouts_.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        idleDrained = false;
    }

    tearDown();
    return idleDrained;
}

void Thread::Impl::shutdown() noexcept
{
    (void)doShutdown(true, std::chrono::steady_clock::duration {});
}

Thread::Thread(std::size_t maxPendingDispatch)
    : Thread("thread", maxPendingDispatch)
{
}

Thread::Thread(std::string_view name, std::size_t maxPendingDispatch)
    : impl_(std::make_shared<Impl>(std::string(name.empty() ? "thread" : name), maxPendingDispatch))
{
}

Thread::~Thread()
{
    if (!impl_) {
        return;
    }
    std::shared_ptr<Impl> keep = std::move(impl_);
    impl_.reset();
    try {
        std::thread([keep = std::move(keep)]() mutable { keep->shutdown(); }).detach();
    } catch (...) {
        try {
            BW_LOG_ERROR("Thread",
                "failed to spawn async teardown thread in destructor; terminating");
        } catch (...) {
        }
        std::terminate();
    }
}

void Thread::dispatch(std::function<void()> task, std::source_location from)
{
    impl_->enqueueDispatch(std::move(task), from);
}

void Thread::dispatchSync(std::function<void()> task, std::source_location from)
{
    impl_->dispatchSync(std::move(task), from);
}

void Thread::dispatchAsync(std::function<void()> task, std::source_location from)
{
    (void)impl_->enqueueAsync(std::move(task), from);
}

void Thread::dispatchAfter(std::chrono::steady_clock::duration delay,
    std::function<void()> task,
    std::source_location from)
{
    impl_->dispatchAfter(delay, std::move(task), from);
}

bool Thread::waitIdle(std::chrono::steady_clock::duration timeout)
{
    return impl_->awaitDrainFor(false, timeout);
}

bool Thread::shutdown(std::chrono::steady_clock::duration waitIdleTimeout) noexcept
{
    return impl_->doShutdown(false, waitIdleTimeout);
}

bool Thread::isRunning() const noexcept { return impl_->isRunning(); }

asio::io_context* Thread::ioContext() noexcept { return impl_->ioContext(); }

bool Thread::isExecutorThread() const noexcept { return impl_->onExecutor(); }

ThreadStats Thread::stats() const noexcept { return impl_->snapshotStats(); }

} // namespace lc
