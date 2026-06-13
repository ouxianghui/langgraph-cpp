#include "foundation/timer/interval_timer.hpp"

#include "foundation/logging/logger.hpp"

#include <asio/bind_executor.hpp>
#include <asio/dispatch.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>

#include <atomic>
#include <exception>
#include <mutex>
#include <utility>

namespace lc {

namespace {

using Ms = ITimer::Milliseconds;

Ms coercePositive(Ms d) noexcept
{
    return d.count() > 0 ? d : Ms { 1 };
}

} // namespace

struct IntervalTimer::Impl final : std::enable_shared_from_this<Impl> {
    asio::strand<asio::any_io_executor> strand_;
    asio::steady_timer timer_;

    std::atomic<Ms::rep> intervalMs_ { 0 };
    std::atomic<bool> singleShot_ { false };

    std::atomic<bool> armed_ { false };
    std::atomic<bool> closed_ { false };

    mutable std::mutex handlerMutex_;
    ITimer::Callback handler_;

    explicit Impl(asio::any_io_executor ex)
        : strand_(asio::make_strand(std::move(ex)))
        , timer_(strand_)
    {
    }

    void startFromStrand()
    {
        if (closed_.load(std::memory_order_acquire)) {
            return;
        }
        const Ms::rep ms = intervalMs_.load(std::memory_order_relaxed);
        if (ms <= 0) {
            return;
        }

        timer_.cancel();

        armed_.store(true, std::memory_order_release);
        timer_.expires_after(Ms(ms));
        timer_.async_wait(asio::bind_executor(
            strand_, [self = shared_from_this()](const asio::error_code& ec) { self->onTimer(ec); }));
    }

    void stopFromStrand() noexcept
    {
        armed_.store(false, std::memory_order_release);
        timer_.cancel();
    }

    void onTimer(const asio::error_code& ec)
    {
        if (closed_.load(std::memory_order_acquire)) {
            return;
        }
        if (ec == asio::error::operation_aborted) {
            return;
        }
        if (ec) {
            BW_LOG_WARN("IntervalTimer", "steady_timer async_wait: {}", ec.message());
            armed_.store(false, std::memory_order_release);
            return;
        }

        if (!armed_.load(std::memory_order_acquire)) {
            return;
        }

        ITimer::Callback cb;
        {
            std::lock_guard<std::mutex> lock(handlerMutex_);
            cb = handler_;
        }
        if (cb) {
            try {
                cb();
            } catch (const std::exception& ex) {
                BW_LOG_WARN("IntervalTimer", "handler threw: {}", ex.what());
            } catch (...) {
                BW_LOG_WARN("IntervalTimer", "handler threw non-std exception");
            }
        }

        if (closed_.load(std::memory_order_acquire)) {
            return;
        }

        if (singleShot_.load(std::memory_order_acquire)) {
            armed_.store(false, std::memory_order_release);
            return;
        }

        if (!armed_.load(std::memory_order_acquire)) {
            return;
        }

        const Ms::rep ms = intervalMs_.load(std::memory_order_relaxed);
        if (ms <= 0) {
            armed_.store(false, std::memory_order_release);
            return;
        }

        timer_.expires_after(Ms(ms));
        timer_.async_wait(asio::bind_executor(
            strand_, [self = shared_from_this()](const asio::error_code& ec2) { self->onTimer(ec2); }));
    }
};

IntervalTimer::IntervalTimer(asio::any_io_executor executor)
    : impl_(std::make_shared<Impl>(std::move(executor)))
{
}

IntervalTimer::IntervalTimer(IntervalTimer&&) noexcept = default;
IntervalTimer& IntervalTimer::operator=(IntervalTimer&&) noexcept = default;

IntervalTimer::~IntervalTimer()
{
    std::shared_ptr<Impl> impl = std::exchange(impl_, {});
    if (!impl) {
        return;
    }
    asio::dispatch(impl->strand_, [impl]() {
        impl->closed_.store(true, std::memory_order_release);
        impl->armed_.store(false, std::memory_order_release);
        impl->timer_.cancel();
    });
}

void IntervalTimer::setInterval(Milliseconds interval) noexcept
{
    std::shared_ptr<Impl> impl = impl_;
    if (!impl) {
        return;
    }
    impl->intervalMs_.store(interval.count(), std::memory_order_relaxed);
}

ITimer::Milliseconds IntervalTimer::interval() const noexcept
{
    std::shared_ptr<Impl> impl = impl_;
    if (!impl) {
        return Milliseconds { 0 };
    }
    return Milliseconds(impl->intervalMs_.load(std::memory_order_relaxed));
}

void IntervalTimer::setSingleShot(bool singleShot) noexcept
{
    std::shared_ptr<Impl> impl = impl_;
    if (!impl) {
        return;
    }
    impl->singleShot_.store(singleShot, std::memory_order_relaxed);
}

bool IntervalTimer::isSingleShot() const noexcept
{
    std::shared_ptr<Impl> impl = impl_;
    if (!impl) {
        return false;
    }
    return impl->singleShot_.load(std::memory_order_relaxed);
}

void IntervalTimer::setTimeoutHandler(Callback handler)
{
    std::shared_ptr<Impl> impl = impl_;
    if (!impl) {
        return;
    }
    asio::post(impl->strand_, [impl, h = std::move(handler)]() mutable {
        if (impl->closed_.load(std::memory_order_acquire)) {
            return;
        }
        std::lock_guard<std::mutex> lock(impl->handlerMutex_);
        impl->handler_ = std::move(h);
    });
}

void IntervalTimer::start()
{
    std::shared_ptr<Impl> impl = impl_;
    if (!impl) {
        return;
    }
    asio::post(impl->strand_,
        [impl]() {
            if (impl->closed_.load(std::memory_order_acquire)) {
                return;
            }
            impl->startFromStrand();
        });
}

void IntervalTimer::start(Milliseconds interval)
{
    std::shared_ptr<Impl> impl = impl_;
    if (!impl) {
        return;
    }
    const Milliseconds use = coercePositive(interval);
    asio::post(impl->strand_,
        [impl, use]() {
            if (impl->closed_.load(std::memory_order_acquire)) {
                return;
            }
            impl->intervalMs_.store(use.count(), std::memory_order_relaxed);
            impl->startFromStrand();
        });
}

void IntervalTimer::stop() noexcept
{
    std::shared_ptr<Impl> impl = impl_;
    if (!impl) {
        return;
    }
    asio::post(impl->strand_, [impl]() { impl->stopFromStrand(); });
}

bool IntervalTimer::isActive() const noexcept
{
    std::shared_ptr<Impl> impl = impl_;
    if (!impl) {
        return false;
    }
    return impl->armed_.load(std::memory_order_acquire);
}

void IntervalTimer::singleShot(asio::any_io_executor executor, Milliseconds delay, Callback handler)
{
    auto strand = std::make_shared<asio::strand<asio::any_io_executor>>(asio::make_strand(executor));
    auto timer = std::make_shared<asio::steady_timer>(*strand);
    const Milliseconds when = coercePositive(delay);

    timer->expires_after(when);
    timer->async_wait(asio::bind_executor(*strand, [timer, h = std::move(handler)](const asio::error_code& ec) mutable {
        if (ec) {
            if (ec != asio::error::operation_aborted) {
                BW_LOG_WARN("IntervalTimer", "singleShot async_wait: {}", ec.message());
            }
            return;
        }
        if (h) {
            try {
                h();
            } catch (const std::exception& ex) {
                BW_LOG_WARN("IntervalTimer", "singleShot handler threw: {}", ex.what());
            } catch (...) {
                BW_LOG_WARN("IntervalTimer", "singleShot handler threw non-std exception");
            }
        }
    }));
}

} // namespace lc
