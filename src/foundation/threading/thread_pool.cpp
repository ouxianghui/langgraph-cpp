#include "foundation/threading/thread_pool.hpp"

#include "foundation/logging/logger.hpp"

#include <asio/post.hpp>
#include <asio/thread_pool.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <thread>
#include <utility>

namespace lc {

namespace {

thread_local const void* tlsPoolSelf = nullptr;

struct PoolTlsScope {
    explicit PoolTlsScope(const void* impl) noexcept { tlsPoolSelf = impl; }

    ~PoolTlsScope() { tlsPoolSelf = nullptr; }
};

[[nodiscard]] std::size_t resolveThreadCount(std::size_t requested)
{
    if (requested != 0U) {
        return requested;
    }
    const unsigned hc = std::thread::hardware_concurrency();
    return std::max<std::size_t>(1U, hc == 0U ? 1U : static_cast<std::size_t>(hc));
}

[[nodiscard]] bool hasSourceLocation(const std::source_location& loc) noexcept
{
    return loc.line() != 0 && loc.file_name() != nullptr && loc.file_name()[0] != '\0';
}

void logUncaughtSubmit(const std::source_location& from, const std::exception* ex) noexcept
{
    try {
        if (hasSourceLocation(from)) {
            if (ex != nullptr) {
                BW_LOG_WARN("ThreadPool",
                    "task exception at {}:{} `{}`: {}",
                    from.file_name(),
                    from.line(),
                    from.function_name(),
                    ex->what());
            } else {
                BW_LOG_WARN("ThreadPool",
                    "task exception (non-std) at {}:{} `{}`",
                    from.file_name(),
                    from.line(),
                    from.function_name());
            }
        } else if (ex != nullptr) {
            BW_LOG_WARN("ThreadPool", "task exception: {}", ex->what());
        } else {
            BW_LOG_WARN("ThreadPool", "task exception: (non-std)");
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
    Impl(std::size_t requestedThreadCount, std::size_t maxPendingSubmit)
        : threadCount_(resolveThreadCount(requestedThreadCount))
        , maxPendingSubmit_(maxPendingSubmit)
        , pool_(std::make_unique<asio::thread_pool>(threadCount_))
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = true;
        }
        try {
            if (maxPendingSubmit_ == 0U) {
                BW_LOG_INFO("ThreadPool", "started threads={} maxPendingSubmit=unbounded", threadCount_);
            } else {
                BW_LOG_INFO("ThreadPool", "started threads={} maxPendingSubmit={}", threadCount_, maxPendingSubmit_);
            }
        } catch (...) {
        }
    }

    ~Impl() { (void)doShutdown(true, std::chrono::steady_clock::duration {}); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] std::size_t threadCount() const noexcept { return threadCount_; }

    [[nodiscard]] bool onWorker() const noexcept
    {
        return tlsPoolSelf == static_cast<const void*>(this);
    }

    [[nodiscard]] bool submit(std::function<void()> task, std::source_location from)
    {
        if (!task) {
            return false;
        }

        auto work = std::make_shared<SubmittedWork>(std::move(task), from);

        std::shared_ptr<Impl> self;
        std::optional<decltype(std::declval<asio::thread_pool&>().get_executor())> executorForPost;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ || !pool_) {
                submitRejectedWhileStopped_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (maxPendingSubmit_ != 0U && outstanding_ >= maxPendingSubmit_) {
                submitRejectedQueueFull_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            ++outstanding_;
            if (outstanding_ > peakOutstanding_) {
                peakOutstanding_ = outstanding_;
            }
            self = shared_from_this();
            executorForPost = pool_->get_executor();
        }

        try {
            asio::post(*executorForPost,
                [self = std::move(self), work]() noexcept { self->runTask(std::move(work)); });
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (outstanding_ > 0ULL) {
                --outstanding_;
            }
            idleCv_.notify_all();
            return false;
        }

        submitAccepted_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void awaitDrain(bool forever, std::chrono::steady_clock::duration timeout)
    {
        (void)awaitDrainFor(forever, timeout);
    }

    [[nodiscard]] bool awaitDrainFor(bool forever, std::chrono::steady_clock::duration timeout)
    {
        if (onWorker()) {
            waitIdleEarlyReturnFromPoolThread_.fetch_add(1, std::memory_order_relaxed);
            try {
                BW_LOG_WARN("ThreadPool", "waitIdle: called from pool worker — skipping wait (deadlock guard)");
            } catch (...) {
            }
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
                waitIdleTimeouts_.fetch_add(1, std::memory_order_relaxed);
            }
            return ok;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        if (!idleCv_.wait_until(lock, deadline, [this] { return outstanding_ == 0ULL; })) {
            waitIdleTimeouts_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool doShutdown(bool drainForever, std::chrono::steady_clock::duration drainBudget) noexcept
    {
        joinDeferredTeardown();

        std::unique_ptr<asio::thread_pool> poolToJoin;
        bool idleDrained = true;

        {
            std::unique_lock<std::mutex> lock(mutex_);

            if (!onWorker()) {
                joinerCv_.wait(lock, [this] { return !joinerBusy_; });
            }

            if (!pool_) {
                if (onWorker()) {
                    if (joinerBusy_ || teardownJoiner_.joinable()) {
                        return false;
                    }
                    return true;
                }
                if (teardownJoiner_.joinable()) {
                    lock.unlock();
                    joinDeferredTeardown();
                    return doShutdown(drainForever, drainBudget);
                }
                return true;
            }

            running_ = false;

            if (onWorker()) {
                shutdownInvokedFromPoolThread_.fetch_add(1, std::memory_order_relaxed);
                try {
                    BW_LOG_WARN("ThreadPool",
                        "shutdown: called from pool worker — deferring pool::join() to a helper thread");
                } catch (...) {
                }

                if (teardownJoiner_.joinable()) {
                    joinerBusy_ = false;
                    joinerCv_.notify_all();
                    try {
                        BW_LOG_ERROR("ThreadPool",
                            "shutdown from pool worker while deferred joiner already exists");
                    } catch (...) {
                    }
                    std::terminate();
                }

                joinerBusy_ = true;
                poolToJoin = std::move(pool_);
                lock.unlock();

                std::thread helper([p = std::move(poolToJoin)]() mutable {
                    if (p) {
                        p->join();
                    }
                    try {
                        BW_LOG_INFO("ThreadPool", "deferred pool join finished");
                    } catch (...) {
                    }
                });

                {
                    std::lock_guard<std::mutex> gl(mutex_);
                    teardownJoiner_ = std::move(helper);
                    joinerBusy_ = false;
                }
                joinerCv_.notify_all();
                return false;
            }

            if (drainForever) {
                idleCv_.wait(lock, [this] { return outstanding_ == 0ULL; });
            } else if (drainBudget <= std::chrono::steady_clock::duration::zero()) {
                idleDrained = (outstanding_ == 0ULL);
                if (!idleDrained) {
                    shutdownIdleWaitTimeouts_.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                const auto deadline = std::chrono::steady_clock::now() + drainBudget;
                idleDrained = idleCv_.wait_until(lock, deadline, [this] { return outstanding_ == 0ULL; });
                if (!idleDrained) {
                    shutdownIdleWaitTimeouts_.fetch_add(1, std::memory_order_relaxed);
                }
            }

            poolToJoin = std::move(pool_);
        }

        if (poolToJoin) {
            poolToJoin->join();
        }

        try {
            BW_LOG_INFO("ThreadPool", "shutdown complete (threads were {})", threadCount_);
        } catch (...) {
        }

        return idleDrained;
    }

    [[nodiscard]] bool isRunning() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_ && static_cast<bool>(pool_);
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
            std::lock_guard<std::mutex> lock(mutex_);
            s.peakOutstanding = peakOutstanding_;
        }
        s.waitIdleEarlyReturnFromPoolThread = waitIdleEarlyReturnFromPoolThread_.load(std::memory_order_relaxed);
        s.waitIdleTimeouts = waitIdleTimeouts_.load(std::memory_order_relaxed);
        s.shutdownInvokedFromPoolThread = shutdownInvokedFromPoolThread_.load(std::memory_order_relaxed);
        s.shutdownIdleWaitTimeouts = shutdownIdleWaitTimeouts_.load(std::memory_order_relaxed);
        return s;
    }

private:
    void joinDeferredTeardown() noexcept
    {
        if (onWorker()) {
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

    void runTask(std::shared_ptr<SubmittedWork> work) noexcept
    {
        PoolTlsScope tlsGuard(static_cast<const void*>(this));

        if (!work || !work->fn) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (outstanding_ > 0ULL) {
                --outstanding_;
            }
            idleCv_.notify_all();
            return;
        }

        try {
            work->fn();
            tasksCompleted_.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& ex) {
            taskUncaughtExceptions_.fetch_add(1, std::memory_order_relaxed);
            logUncaughtSubmit(work->from, &ex);
        } catch (...) {
            taskUncaughtExceptions_.fetch_add(1, std::memory_order_relaxed);
            logUncaughtSubmit(work->from, nullptr);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (outstanding_ > 0ULL) {
                --outstanding_;
            }
            idleCv_.notify_all();
        }
    }

    const std::size_t threadCount_;
    const std::size_t maxPendingSubmit_;
    std::unique_ptr<asio::thread_pool> pool_;

    mutable std::mutex mutex_;
    std::condition_variable idleCv_;
    std::condition_variable joinerCv_;
    bool joinerBusy_ { false };
    std::thread teardownJoiner_{};
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

ThreadPool::ThreadPool(std::size_t threadCount, std::size_t maxPendingSubmit)
    : impl_(std::make_shared<Impl>(threadCount, maxPendingSubmit))
{
}

ThreadPool::~ThreadPool()
{
    if (!impl_) {
        return;
    }
    if (impl_->onWorker()) {
        std::shared_ptr<Impl> keep = std::move(impl_);
        impl_.reset();
        try {
            std::thread([keep]() mutable {
                (void)keep->doShutdown(true, std::chrono::steady_clock::duration {});
            }).detach();
        } catch (...) {
            try {
                BW_LOG_ERROR("ThreadPool",
                    "failed to spawn async teardown thread after delete from pool worker; terminating");
            } catch (...) {
            }
            std::terminate();
        }
        return;
    }
    impl_.reset();
}

bool ThreadPool::submit(std::function<void()> task, std::source_location from)
{
    return impl_->submit(std::move(task), from);
}

bool ThreadPool::waitIdle(std::chrono::steady_clock::duration timeout)
{
    return impl_->awaitDrainFor(false, timeout);
}

bool ThreadPool::shutdown(std::chrono::steady_clock::duration waitIdleTimeout) noexcept
{
    return impl_->doShutdown(false, waitIdleTimeout);
}

bool ThreadPool::isRunning() const noexcept
{
    return impl_->isRunning();
}

std::size_t ThreadPool::threadCount() const noexcept
{
    return impl_->threadCount();
}

ThreadPoolStats ThreadPool::stats() const noexcept
{
    return impl_->stats();
}

bool ThreadPool::isExecutingOnThisPool() const noexcept
{
    return impl_ && impl_->onWorker();
}

} // namespace lc
