#include "foundation/timer/interval_timer.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace lgc {
namespace {

[[nodiscard]] ITimer::Duration normalizeDelay(ITimer::Duration delay) noexcept
{
    return std::max(delay, ITimer::Duration(1));
}

void invokeTimerHandler(const std::shared_ptr<ILogger>& logger, const ITimer::Callback& handler) noexcept
{
    if (!handler)
        return;
    try {
        handler();
    } catch (const std::exception& ex) {
        try {
            logTo(logger, LogLevel::Warn, "IntervalTimer", "handler threw: {}", __FILE__, __LINE__, ex.what());
        } catch (...) {
        }
    } catch (...) {
        try {
            logTo(logger, LogLevel::Warn, "IntervalTimer", "handler threw non-std exception", __FILE__, __LINE__);
        } catch (...) {
        }
    }
}

} // namespace

struct TimerHandle::State {
    explicit State(std::shared_ptr<ILogger> logger)
        : logger_(std::move(logger))
    {
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::shared_ptr<ILogger> logger_;
    std::thread worker_;
    bool cancelled_ { false };
    bool running_ { false };
    bool done_ { false };
};

TimerHandle::TimerHandle(std::shared_ptr<State> state)
    : state_(std::move(state))
{
}

TimerHandle::~TimerHandle()
{
    if (state_) {
        (void)cancel();
        if (state_->worker_.joinable() && state_->worker_.get_id() == std::this_thread::get_id()) {
            state_->worker_.detach();
        } else {
            (void)wait(Duration::max());
        }
    }
}

TimerHandle::TimerHandle(TimerHandle&&) noexcept = default;

TimerHandle& TimerHandle::operator=(TimerHandle&& other) noexcept
{
    if (this == &other)
        return *this;
    if (state_) {
        (void)cancel();
        if (state_->worker_.joinable() && state_->worker_.get_id() == std::this_thread::get_id()) {
            state_->worker_.detach();
        } else {
            (void)wait(Duration::max());
        }
    }
    state_ = std::move(other.state_);
    return *this;
}

bool TimerHandle::valid() const noexcept
{
    return static_cast<bool>(state_);
}

bool TimerHandle::active() const noexcept
{
    if (!state_)
        return false;
    std::lock_guard lock(state_->mutex_);
    return !state_->done_ && !state_->cancelled_;
}

Status TimerHandle::cancel()
{
    if (!state_)
        return Status::ok();
    {
        std::lock_guard lock(state_->mutex_);
        if (state_->done_)
            return Status::ok();
        state_->cancelled_ = true;
    }
    state_->cv_.notify_all();
    return Status::ok();
}

Status TimerHandle::wait(Duration timeout)
{
    if (!state_)
        return Status::ok();
    if (state_->worker_.joinable() && state_->worker_.get_id() == std::this_thread::get_id())
        return Status::failedPrecondition("timer handle cannot wait from its own worker thread");

    std::unique_lock lock(state_->mutex_);
    const auto done = [&] { return state_->done_; };
    if (timeout == Duration::max()) {
        state_->cv_.wait(lock, done);
    } else if (timeout <= Duration::zero()) {
        if (!done())
            return Status::deadlineExceeded("timer handle wait timed out");
    } else if (!state_->cv_.wait_for(lock, timeout, done)) {
        return Status::deadlineExceeded("timer handle wait timed out");
    }
    lock.unlock();

    if (state_->worker_.joinable())
        state_->worker_.join();
    return Status::ok();
}

struct IntervalTimer::Impl {
    struct State {
        explicit State(std::shared_ptr<ILogger> logger)
            : logger_(std::move(logger))
        {
        }

        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::shared_ptr<ILogger> logger_;
        Duration interval_ { Duration(1000) };
        Callback handler_;
        bool active_ { false };
        bool singleShot_ { false };
        bool stopping_ { false };
        bool closed_ { false };
        bool callbackRunning_ { false };
        bool workerDone_ { false };
        std::uint64_t generation_ { 0 };
        std::thread worker_;
    };

    explicit Impl(std::shared_ptr<ILogger> logger)
        : state_(std::make_shared<State>(std::move(logger)))
    {
        state_->worker_ = std::thread([state = state_] { run(std::move(state)); });
    }

    ~Impl()
    {
        (void)close(Duration::max());
    }

    static void run(std::shared_ptr<State> state) noexcept
    {
        std::unique_lock lock(state->mutex_);
        for (;;) {
            state->cv_.wait(lock, [&] { return state->stopping_ || state->active_; });
            if (state->stopping_)
                break;

            const auto due = std::chrono::steady_clock::now() + state->interval_;
            const auto generation = state->generation_;
            if (state->cv_.wait_until(lock, due, [&] {
                    return state->stopping_ || !state->active_ || state->generation_ != generation;
                })) {
                continue;
            }

            auto handler = state->handler_;
            const bool singleShot = state->singleShot_;
            state->callbackRunning_ = true;
            if (singleShot) {
                state->active_ = false;
                ++state->generation_;
            }

            lock.unlock();
            invokeTimerHandler(state->logger_, handler);
            lock.lock();
            state->callbackRunning_ = false;
            state->cv_.notify_all();
        }
        state->active_ = false;
        state->workerDone_ = true;
        state->cv_.notify_all();
    }

    [[nodiscard]] Status waitIdle(Duration timeout)
    {
        auto state = state_;
        std::unique_lock lock(state->mutex_);
        const auto idle = [&] { return !state->active_ && !state->callbackRunning_; };
        if (timeout == Duration::max()) {
            state->cv_.wait(lock, idle);
            return Status::ok();
        }
        if (timeout <= Duration::zero()) {
            if (idle())
                return Status::ok();
            return Status::deadlineExceeded("timer did not become idle before timeout");
        }
        if (!state->cv_.wait_for(lock, timeout, idle))
            return Status::deadlineExceeded("timer did not become idle before timeout");
        return Status::ok();
    }

    [[nodiscard]] Status close(Duration waitIdleTimeout)
    {
        std::lock_guard closeLock(closeMutex_);
        auto state = state_;
        const bool onWorker = state->worker_.joinable() && state->worker_.get_id() == std::this_thread::get_id();

        {
            std::lock_guard lock(state->mutex_);
            if (state->closed_)
                return Status::ok();
            state->stopping_ = true;
            state->active_ = false;
            ++state->generation_;
        }
        state->cv_.notify_all();

        if (onWorker) {
            if (state->worker_.joinable())
                state->worker_.detach();
            std::lock_guard lock(state->mutex_);
            state->closed_ = true;
            return Status::ok();
        }

        std::unique_lock lock(state->mutex_);
        const auto done = [&] { return state->workerDone_; };
        if (waitIdleTimeout == Duration::max()) {
            state->cv_.wait(lock, done);
        } else if (waitIdleTimeout <= Duration::zero()) {
            if (!done())
                return Status::deadlineExceeded("timer close timed out");
        } else if (!state->cv_.wait_for(lock, waitIdleTimeout, done)) {
            return Status::deadlineExceeded("timer close timed out");
        }
        lock.unlock();

        if (state->worker_.joinable())
            state->worker_.join();
        {
            std::lock_guard guard(state->mutex_);
            state->closed_ = true;
        }
        return Status::ok();
    }

    mutable std::mutex closeMutex_;
    std::shared_ptr<State> state_;
};

IntervalTimer::IntervalTimer(std::shared_ptr<ILogger> logger)
    : impl_(std::make_shared<Impl>(std::move(logger)))
{
}

IntervalTimer::IntervalTimer(IntervalTimer&&) noexcept = default;
IntervalTimer& IntervalTimer::operator=(IntervalTimer&&) noexcept = default;

IntervalTimer::~IntervalTimer() = default;

void IntervalTimer::setInterval(Duration value) noexcept
{
    auto state = impl_->state_;
    std::lock_guard lock(state->mutex_);
    if (state->closed_)
        return;
    state->interval_ = value;
    if (state->active_)
        ++state->generation_;
    state->cv_.notify_all();
}

ITimer::Duration IntervalTimer::interval() const noexcept
{
    auto state = impl_->state_;
    std::lock_guard lock(state->mutex_);
    return state->interval_;
}

void IntervalTimer::setSingleShot(bool value) noexcept
{
    auto state = impl_->state_;
    std::lock_guard lock(state->mutex_);
    if (state->closed_)
        return;
    state->singleShot_ = value;
}

bool IntervalTimer::singleShot() const noexcept
{
    auto state = impl_->state_;
    std::lock_guard lock(state->mutex_);
    return state->singleShot_;
}

void IntervalTimer::setHandler(Callback value)
{
    auto state = impl_->state_;
    std::lock_guard lock(state->mutex_);
    if (state->closed_)
        return;
    state->handler_ = std::move(value);
}

Status IntervalTimer::start()
{
    auto state = impl_->state_;
    std::lock_guard lock(state->mutex_);
    if (state->closed_ || state->stopping_)
        return Status::failedPrecondition("timer is closed");
    if (state->interval_ <= Duration::zero())
        return Status::invalidArgument("timer interval must be positive");
    state->active_ = true;
    ++state->generation_;
    state->cv_.notify_all();
    return Status::ok();
}

Status IntervalTimer::start(Duration interval)
{
    auto state = impl_->state_;
    std::lock_guard lock(state->mutex_);
    if (state->closed_ || state->stopping_)
        return Status::failedPrecondition("timer is closed");
    state->interval_ = normalizeDelay(interval);
    state->active_ = true;
    ++state->generation_;
    state->cv_.notify_all();
    return Status::ok();
}

Status IntervalTimer::stop()
{
    auto state = impl_->state_;
    std::lock_guard lock(state->mutex_);
    if (state->closed_)
        return Status::ok();
    state->active_ = false;
    ++state->generation_;
    state->cv_.notify_all();
    return Status::ok();
}

bool IntervalTimer::active() const noexcept
{
    auto state = impl_->state_;
    std::lock_guard lock(state->mutex_);
    return state->active_;
}

Status IntervalTimer::waitIdle(Duration timeout)
{
    return impl_->waitIdle(timeout);
}

Status IntervalTimer::close(Duration waitIdleTimeout)
{
    return impl_->close(waitIdleTimeout);
}

bool IntervalTimer::isClosed() const noexcept
{
    auto state = impl_->state_;
    std::lock_guard lock(state->mutex_);
    return state->closed_;
}

TimerHandle IntervalTimer::singleShot(Duration delay, Callback handler, std::shared_ptr<ILogger> logger)
{
    if (!handler)
        return TimerHandle();

    auto state = std::make_shared<TimerHandle::State>(std::move(logger));
    state->worker_ = std::thread([state, delay = normalizeDelay(delay), handler = std::move(handler)] {
        std::unique_lock lock(state->mutex_);
        const auto cancelled = [&] { return state->cancelled_; };
        if (state->cv_.wait_for(lock, delay, cancelled)) {
            state->done_ = true;
            lock.unlock();
            state->cv_.notify_all();
            return;
        }
        state->running_ = true;
        lock.unlock();

        invokeTimerHandler(state->logger_, handler);

        lock.lock();
        state->running_ = false;
        state->done_ = true;
        lock.unlock();
        state->cv_.notify_all();
    });

    return TimerHandle(std::move(state));
}

} // namespace lgc
