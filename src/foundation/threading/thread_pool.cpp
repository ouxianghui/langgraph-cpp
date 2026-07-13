#include "foundation/threading/thread_pool.hpp"

#include "foundation/logging/logger.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <queue>
#include <source_location>
#include <thread>
#include <utility>
#include <vector>

namespace lc {
namespace {

thread_local const void* tlsPoolSelf = nullptr;

struct PoolTlsScope {
    explicit PoolTlsScope(const void* impl) noexcept { tlsPoolSelf = impl; }
    ~PoolTlsScope() { tlsPoolSelf = nullptr; }
};

[[nodiscard]] std::size_t resolveThreadCount(std::size_t requested)
{
    if (requested != 0U)
        return requested;

    const unsigned hc = std::thread::hardware_concurrency();
    return std::max<std::size_t>(1U, hc == 0U ? 1U : static_cast<std::size_t>(hc));
}

[[nodiscard]] bool hasSourceLocation(const std::source_location& loc) noexcept
{
    return loc.line() != 0 && loc.file_name() != nullptr && loc.file_name()[0] != '\0';
}

void logUncaughtSubmit(
    const std::shared_ptr<ILogger>& logger,
    const std::source_location& from,
    const std::exception* ex) noexcept
{
    try {
        if (hasSourceLocation(from)) {
            if (ex != nullptr) {
                logTo(logger,
                    LogLevel::Warn,
                    "ThreadPool",
                    "task exception at {}:{} `{}`: {}",
                    __FILE__,
                    __LINE__,
                    from.file_name(),
                    from.line(),
                    from.function_name(),
                    ex->what());
            } else {
                logTo(logger,
                    LogLevel::Warn,
                    "ThreadPool",
                    "task exception (non-std) at {}:{} `{}`",
                    __FILE__,
                    __LINE__,
                    from.file_name(),
                    from.line(),
                    from.function_name());
            }
        } else if (ex != nullptr) {
            logTo(logger, LogLevel::Warn, "ThreadPool", "task exception: {}", __FILE__, __LINE__, ex->what());
        } else {
            logTo(logger, LogLevel::Warn, "ThreadPool", "task exception: (non-std)", __FILE__, __LINE__);
        }
    } catch (...) {
    }
}

} // namespace

struct SubmittedWork {
    std::function<void()> fn;
    std::source_location from;
};

class ThreadPool::Impl : public std::enable_shared_from_this<Impl> {
public:
    Impl(std::size_t requestedThreadCount, std::size_t maxPendingSubmit, std::shared_ptr<ILogger> logger)
        : threadCount_(resolveThreadCount(requestedThreadCount))
        , maxPendingSubmit_(maxPendingSubmit)
        , logger_(std::move(logger))
    {
    }

    static std::shared_ptr<Impl> create(
        std::size_t requestedThreadCount,
        std::size_t maxPendingSubmit,
        std::shared_ptr<ILogger> logger)
    {
        struct Enabler final : Impl {
            Enabler(std::size_t requested, std::size_t maxPending, std::shared_ptr<ILogger> logger)
                : Impl(requested, maxPending, std::move(logger))
            {
            }
        };
        auto impl = std::make_shared<Enabler>(
            requestedThreadCount,
            maxPendingSubmit,
            std::move(logger));
        impl->start();
        return impl;
    }

    ~Impl() { (void)doShutdown(true, std::chrono::steady_clock::duration {}); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] std::size_t threadCount() const noexcept { return threadCount_; }
    [[nodiscard]] std::shared_ptr<ILogger> logger() const noexcept { return logger_; }

    void start()
    {
        {
            std::lock_guard lock(mutex_);
            running_ = true;
        }

        workers_.reserve(threadCount_);
        try {
            for (std::size_t i = 0; i < threadCount_; ++i)
                workers_.emplace_back([this] { workerLoop(); });
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                running_ = false;
            }
            workCv_.notify_all();
            joinWorkers();
            throw;
        }

        try {
            if (maxPendingSubmit_ == 0U)
                logTo(logger_,
                    LogLevel::Info,
                    "ThreadPool",
                    "started threads={} maxPendingSubmit=unbounded",
                    __FILE__,
                    __LINE__,
                    threadCount_);
            else
                logTo(logger_,
                    LogLevel::Info,
                    "ThreadPool",
                    "started threads={} maxPendingSubmit={}",
                    __FILE__,
                    __LINE__,
                    threadCount_,
                    maxPendingSubmit_);
        } catch (...) {
        }
    }

    [[nodiscard]] bool onWorker() const noexcept
    {
        return tlsPoolSelf == static_cast<const void*>(this);
    }

    [[nodiscard]] Status submit(std::function<void()> task, std::source_location from)
    {
        if (!task)
            return Status::invalidArgument("thread pool task cannot be empty");

        {
            std::lock_guard lock(mutex_);
            if (!running_) {
                submitRejectedWhileStopped_.fetch_add(1, std::memory_order_relaxed);
                return Status::failedPrecondition("thread pool is stopped");
            }
            if (maxPendingSubmit_ != 0U && outstanding_ >= maxPendingSubmit_) {
                submitRejectedQueueFull_.fetch_add(1, std::memory_order_relaxed);
                return Status::resourceExhausted("thread pool queue is full");
            }

            queue_.push(SubmittedWork {
                .fn = std::move(task),
                .from = from,
            });
            ++outstanding_;
            if (outstanding_ > peakOutstanding_)
                peakOutstanding_ = outstanding_;
        }

        submitAccepted_.fetch_add(1, std::memory_order_relaxed);
        workCv_.notify_one();
        return Status::ok();
    }

    [[nodiscard]] Status awaitDrainFor(bool forever, std::chrono::steady_clock::duration timeout)
    {
        if (onWorker()) {
            waitIdleEarlyReturnFromPoolThread_.fetch_add(1, std::memory_order_relaxed);
            try {
                logTo(logger_,
                    LogLevel::Warn,
                    "ThreadPool",
                    "waitIdle: called from pool worker - skipping wait (deadlock guard)",
                    __FILE__,
                    __LINE__);
            } catch (...) {
            }
            return Status::failedPrecondition("waitIdle cannot be called from a thread pool worker");
        }

        std::unique_lock lock(mutex_);
        if (forever) {
            idleCv_.wait(lock, [this] { return outstanding_ == 0ULL; });
            return Status::ok();
        }
        if (timeout <= std::chrono::steady_clock::duration::zero()) {
            const bool ok = outstanding_ == 0ULL;
            if (!ok)
                waitIdleTimeouts_.fetch_add(1, std::memory_order_relaxed);
            return ok ? Status::ok() : Status::deadlineExceeded("thread pool did not become idle before timeout");
        }

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        if (!idleCv_.wait_until(lock, deadline, [this] { return outstanding_ == 0ULL; })) {
            waitIdleTimeouts_.fetch_add(1, std::memory_order_relaxed);
            return Status::deadlineExceeded("thread pool did not become idle before timeout");
        }
        return Status::ok();
    }

    [[nodiscard]] Status doShutdown(bool drainForever, std::chrono::steady_clock::duration drainBudget)
    {
        joinDeferredTeardown();

        Status idleDrained = Status::ok();
        {
            std::unique_lock lock(mutex_);
            if (!running_ && workers_.empty())
                return Status::ok();

            running_ = false;

            if (onWorker()) {
                shutdownInvokedFromPoolThread_.fetch_add(1, std::memory_order_relaxed);
                try {
                    logTo(logger_,
                        LogLevel::Warn,
                        "ThreadPool",
                        "shutdown: called from pool worker - deferring join to a helper thread",
                        __FILE__,
                        __LINE__);
                } catch (...) {
                }

                if (teardownJoiner_.joinable()) {
                    std::terminate();
                }

                teardownJoiner_ = std::thread([self = shared_from_this()] {
                    self->workCv_.notify_all();
                    self->joinWorkers();
                    try {
                        logTo(self->logger_,
                            LogLevel::Info,
                            "ThreadPool",
                            "deferred pool join finished",
                            __FILE__,
                            __LINE__);
                    } catch (...) {
                    }
                });
                return Status::failedPrecondition("shutdown cannot join a thread pool from its worker");
            }

            if (drainForever) {
                idleCv_.wait(lock, [this] { return outstanding_ == 0ULL; });
            } else if (drainBudget <= std::chrono::steady_clock::duration::zero()) {
                if (outstanding_ != 0ULL) {
                    idleDrained = Status::deadlineExceeded("thread pool did not become idle before shutdown timeout");
                    shutdownIdleWaitTimeouts_.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                const auto deadline = std::chrono::steady_clock::now() + drainBudget;
                if (!idleCv_.wait_until(lock, deadline, [this] { return outstanding_ == 0ULL; })) {
                    idleDrained = Status::deadlineExceeded("thread pool did not become idle before shutdown timeout");
                    shutdownIdleWaitTimeouts_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        workCv_.notify_all();
        joinWorkers();

        try {
            logTo(logger_,
                LogLevel::Info,
                "ThreadPool",
                "shutdown complete (threads were {})",
                __FILE__,
                __LINE__,
                threadCount_);
        } catch (...) {
        }

        return idleDrained;
    }

    [[nodiscard]] bool isRunning() const noexcept
    {
        std::lock_guard lock(mutex_);
        return running_;
    }

    [[nodiscard]] ThreadPoolStats stats() const noexcept
    {
        ThreadPoolStats s;
        s.submitAccepted = submitAccepted_.load(std::memory_order_relaxed);
        s.submitRejectedWhileStopped = submitRejectedWhileStopped_.load(std::memory_order_relaxed);
        s.submitRejectedQueueFull = submitRejectedQueueFull_.load(std::memory_order_relaxed);
        s.tasksCompleted = tasksCompleted_.load(std::memory_order_relaxed);
        s.taskUncaughtExceptions = taskUncaughtExceptions_.load(std::memory_order_relaxed);
        {
            std::lock_guard lock(mutex_);
            s.peakOutstanding = peakOutstanding_;
        }
        s.waitIdleEarlyReturnFromPoolThread = waitIdleEarlyReturnFromPoolThread_.load(std::memory_order_relaxed);
        s.waitIdleTimeouts = waitIdleTimeouts_.load(std::memory_order_relaxed);
        s.shutdownInvokedFromPoolThread = shutdownInvokedFromPoolThread_.load(std::memory_order_relaxed);
        s.shutdownIdleWaitTimeouts = shutdownIdleWaitTimeouts_.load(std::memory_order_relaxed);
        return s;
    }

private:
    void workerLoop() noexcept
    {
        PoolTlsScope tlsGuard(static_cast<const void*>(this));

        for (;;) {
            SubmittedWork work;
            {
                std::unique_lock lock(mutex_);
                workCv_.wait(lock, [this] {
                    return !running_ || !queue_.empty();
                });

                if (!running_ && queue_.empty())
                    break;

                work = std::move(queue_.front());
                queue_.pop();
            }

            runTask(std::move(work));
        }
    }

    void joinDeferredTeardown() noexcept
    {
        if (onWorker())
            return;

        std::thread joiner;
        {
            std::lock_guard lock(mutex_);
            if (teardownJoiner_.joinable())
                joiner = std::move(teardownJoiner_);
        }
        if (joiner.joinable())
            joiner.join();
    }

    void joinWorkers() noexcept
    {
        std::vector<std::thread> workers;
        {
            std::lock_guard lock(mutex_);
            workers = std::move(workers_);
        }
        for (auto& worker : workers) {
            if (worker.joinable())
                worker.join();
        }
    }

    void runTask(SubmittedWork work) noexcept
    {
        try {
            if (work.fn)
                work.fn();
            tasksCompleted_.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& ex) {
            taskUncaughtExceptions_.fetch_add(1, std::memory_order_relaxed);
            logUncaughtSubmit(logger_, work.from, &ex);
        } catch (...) {
            taskUncaughtExceptions_.fetch_add(1, std::memory_order_relaxed);
            logUncaughtSubmit(logger_, work.from, nullptr);
        }

        {
            std::lock_guard lock(mutex_);
            if (outstanding_ > 0ULL)
                --outstanding_;
        }
        idleCv_.notify_all();
    }

    const std::size_t threadCount_;
    const std::size_t maxPendingSubmit_;
    std::shared_ptr<ILogger> logger_;

    mutable std::mutex mutex_;
    std::condition_variable workCv_;
    std::condition_variable idleCv_;
    std::queue<SubmittedWork> queue_;
    std::vector<std::thread> workers_;
    std::thread teardownJoiner_ {};
    bool running_ { false };
    std::uint64_t outstanding_ { 0 };

    std::atomic<std::uint64_t> submitAccepted_ { 0 };
    std::atomic<std::uint64_t> submitRejectedWhileStopped_ { 0 };
    std::atomic<std::uint64_t> submitRejectedQueueFull_ { 0 };
    std::atomic<std::uint64_t> tasksCompleted_ { 0 };
    std::atomic<std::uint64_t> taskUncaughtExceptions_ { 0 };
    std::uint64_t peakOutstanding_ { 0 };
    std::atomic<std::uint64_t> waitIdleEarlyReturnFromPoolThread_ { 0 };
    std::atomic<std::uint64_t> waitIdleTimeouts_ { 0 };
    std::atomic<std::uint64_t> shutdownInvokedFromPoolThread_ { 0 };
    std::atomic<std::uint64_t> shutdownIdleWaitTimeouts_ { 0 };
};

ThreadPool::ThreadPool(std::size_t threadCount, std::size_t maxPendingSubmit, std::shared_ptr<ILogger> logger)
    : impl_(Impl::create(threadCount, maxPendingSubmit, std::move(logger)))
{
}

ThreadPool::~ThreadPool()
{
    if (!impl_)
        return;

    if (impl_->onWorker()) {
        auto logger = impl_->logger();
        std::shared_ptr<Impl> keep = std::move(impl_);
        impl_.reset();
        try {
            std::thread([keep = std::move(keep)]() mutable {
                (void)keep->doShutdown(true, std::chrono::steady_clock::duration {});
            }).detach();
        } catch (...) {
            try {
                logTo(logger,
                    LogLevel::Error,
                    "ThreadPool",
                    "failed to spawn async teardown thread after delete from pool worker",
                    __FILE__,
                    __LINE__);
            } catch (...) {
            }
            std::terminate();
        }
        return;
    }

    impl_.reset();
}

Status ThreadPool::submit(std::function<void()> task, std::source_location from)
{
    return impl_->submit(std::move(task), from);
}

Status ThreadPool::waitIdle(std::chrono::steady_clock::duration timeout)
{
    return impl_->awaitDrainFor(false, timeout);
}

Status ThreadPool::shutdown(std::chrono::steady_clock::duration waitIdleTimeout)
{
    return impl_->doShutdown(false, waitIdleTimeout);
}

bool ThreadPool::isRunning() const noexcept
{
    return impl_->isRunning();
}

bool ThreadPool::isWorkerThread() const noexcept
{
    return impl_ && impl_->onWorker();
}

std::size_t ThreadPool::threadCount() const noexcept
{
    return impl_->threadCount();
}

ThreadPoolStats ThreadPool::stats() const noexcept
{
    return impl_->stats();
}

} // namespace lc
