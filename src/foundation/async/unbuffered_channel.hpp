#pragma once

#include "foundation/async/bounded_channel.hpp"

namespace lc {

/// Thread-safe rendezvous channel.
///
/// A send completes only after a receiver takes the value. Close aborts pending sends and lets
/// receivers finish with `std::nullopt`.
template <typename T>
class UnbufferedChannel final : public IChannel<T> {
public:
    using Duration = typename IChannel<T>::Duration;

    UnbufferedChannel() = default;

    UnbufferedChannel(const UnbufferedChannel&) = delete;
    UnbufferedChannel& operator=(const UnbufferedChannel&) = delete;
    UnbufferedChannel(UnbufferedChannel&&) = delete;
    UnbufferedChannel& operator=(UnbufferedChannel&&) = delete;

    [[nodiscard]] ChannelSender<T> sender() noexcept { return ChannelSender<T>(this); }
    [[nodiscard]] ChannelReceiver<T> receiver() noexcept { return ChannelReceiver<T>(this); }

    [[nodiscard]] Status send(T value) override
    {
        return send(std::move(value), CancellationToken::none());
    }

    [[nodiscard]] Status send(T value, const CancellationToken& token) override
    {
        if (auto status = token.check("channel send cancelled"); !status.isOk())
            return status;

        auto registration = token.onCancel([this] {
            notifyBlockedOperations();
        });

        std::unique_lock lock(mutex_);
        notFullCv_.wait(lock, [this, &token] {
            return closed_ || !handoff_.has_value() || token.cancelled();
        });
        if (auto status = token.check("channel send cancelled"); !status.isOk())
            return status;
        if (closed_)
            return rejectAfterCloseUnlocked();

        handoff_.emplace(std::move(value));
        notifyWaitersUnlocked();
        notEmptyCv_.notify_one();

        notFullCv_.wait(lock, [this, &token] {
            return closed_ || !handoff_.has_value() || token.cancelled();
        });
        if (auto status = token.check("channel send cancelled"); !status.isOk()) {
            if (handoff_.has_value())
                handoff_.reset();
            notifyWaitersUnlocked();
            notEmptyCv_.notify_all();
            notFullCv_.notify_all();
            return status;
        }
        return closed_ && handoff_.has_value() ? rejectAfterCloseUnlocked() : Status::ok();
    }

    [[nodiscard]] Status sendFor(T value, Duration timeout) override
    {
        return sendFor(std::move(value), timeout, CancellationToken::none());
    }

    [[nodiscard]] Status sendFor(T value, Duration timeout, const CancellationToken& token) override
    {
        if (auto status = token.check("channel send cancelled"); !status.isOk())
            return status;

        const auto deadline = timeout > Duration::zero()
            ? std::optional<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now() + timeout)
            : std::optional<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
        auto remaining = [&] {
            const auto now = std::chrono::steady_clock::now();
            return now >= *deadline ? Duration::zero() : *deadline - now;
        };

        auto registration = token.onCancel([this] {
            notifyBlockedOperations();
        });

        std::unique_lock lock(mutex_);
        if (!notFullCv_.wait_for(lock, remaining(), [this, &token] {
                return closed_ || !handoff_.has_value() || token.cancelled();
            })) {
            ++stats_.sendTimeouts_;
            return Status::deadlineExceeded("channel send timed out");
        }
        if (auto status = token.check("channel send cancelled"); !status.isOk())
            return status;
        if (closed_)
            return rejectAfterCloseUnlocked();

        handoff_.emplace(std::move(value));
        notifyWaitersUnlocked();
        notEmptyCv_.notify_one();

        if (!notFullCv_.wait_for(lock, remaining(), [this, &token] {
                return closed_ || !handoff_.has_value() || token.cancelled();
            })) {
            if (handoff_.has_value()) {
                handoff_.reset();
                notifyWaitersUnlocked();
                notEmptyCv_.notify_all();
            }
            ++stats_.sendTimeouts_;
            return Status::deadlineExceeded("channel send timed out");
        }
        if (auto status = token.check("channel send cancelled"); !status.isOk()) {
            if (handoff_.has_value())
                handoff_.reset();
            notifyWaitersUnlocked();
            notEmptyCv_.notify_all();
            notFullCv_.notify_all();
            return status;
        }
        return closed_ && handoff_.has_value() ? rejectAfterCloseUnlocked() : Status::ok();
    }

    [[nodiscard]] Status trySend(T value)
    {
        return trySendFrom(value);
    }

private:
    [[nodiscard]] Status trySendFrom(T& value) override
    {
        (void)value;
        std::lock_guard lock(mutex_);
        if (closed_)
            return rejectAfterCloseUnlocked();
        return Status::resourceExhausted("unbuffered channel send would block");
    }

public:
    [[nodiscard]] Result<std::optional<T>> receive() override
    {
        return receive(CancellationToken::none());
    }

    [[nodiscard]] Result<std::optional<T>> receive(const CancellationToken& token) override
    {
        if (auto status = token.check("channel receive cancelled"); !status.isOk())
            return status;

        auto registration = token.onCancel([this] {
            notifyBlockedOperations();
        });

        std::unique_lock lock(mutex_);
        notEmptyCv_.wait(lock, [this, &token] {
            return closed_ || handoff_.has_value() || token.cancelled();
        });
        if (auto status = token.check("channel receive cancelled"); !status.isOk())
            return status;
        return popUnlocked();
    }

    [[nodiscard]] Result<std::optional<T>> receiveFor(Duration timeout) override
    {
        return receiveFor(timeout, CancellationToken::none());
    }

    [[nodiscard]] Result<std::optional<T>> receiveFor(Duration timeout, const CancellationToken& token) override
    {
        if (auto status = token.check("channel receive cancelled"); !status.isOk())
            return status;

        auto registration = token.onCancel([this] {
            notifyBlockedOperations();
        });

        std::unique_lock lock(mutex_);
        if (timeout <= Duration::zero()) {
            if (!handoff_.has_value() && !closed_) {
                ++stats_.receiveTimeouts_;
                return Status::deadlineExceeded("channel receive timed out");
            }
        } else if (!notEmptyCv_.wait_for(lock, timeout, [this, &token] {
                       return closed_ || handoff_.has_value() || token.cancelled();
                   })) {
            ++stats_.receiveTimeouts_;
            return Status::deadlineExceeded("channel receive timed out");
        }
        if (auto status = token.check("channel receive cancelled"); !status.isOk())
            return status;
        return popUnlocked();
    }

    [[nodiscard]] Result<std::optional<T>> tryReceive() override
    {
        std::lock_guard lock(mutex_);
        return popUnlocked();
    }

    void close() noexcept override
    {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            if (handoff_.has_value()) {
                handoff_.reset();
                ++stats_.rejectedAfterClose_;
            }
            notifyWaitersUnlocked();
        }
        notEmptyCv_.notify_all();
        notFullCv_.notify_all();
    }

    [[nodiscard]] bool isClosed() const noexcept override
    {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return 0; }

    [[nodiscard]] std::size_t size() const noexcept
    {
        std::lock_guard lock(mutex_);
        return handoff_.has_value() ? 1U : 0U;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        std::lock_guard lock(mutex_);
        return !handoff_.has_value();
    }

    [[nodiscard]] ChannelStats stats() const noexcept
    {
        std::lock_guard lock(mutex_);
        auto out = stats_;
        out.capacity_ = 0;
        out.size_ = handoff_.has_value() ? 1U : 0U;
        out.closed_ = closed_;
        return out;
    }

    void addWaiter(std::shared_ptr<detail::ChannelWaiter> waiter) override
    {
        if (!waiter)
            return;
        std::lock_guard lock(mutex_);
        waiters_.emplace_back(waiter);
    }

private:
    [[nodiscard]] Status rejectAfterCloseUnlocked()
    {
        ++stats_.rejectedAfterClose_;
        return Status::unavailable("channel is closed");
    }

    [[nodiscard]] Result<std::optional<T>> popUnlocked()
    {
        if (!handoff_.has_value()) {
            if (closed_)
                return std::optional<T> {};
            return Status::unavailable("channel is empty");
        }

        T value = std::move(*handoff_);
        handoff_.reset();
        ++stats_.sent_;
        ++stats_.received_;
        notifyWaitersUnlocked();
        notFullCv_.notify_one();
        return std::optional<T>(std::move(value));
    }

    void notifyBlockedOperations() noexcept
    {
        notEmptyCv_.notify_all();
        notFullCv_.notify_all();
        std::lock_guard lock(mutex_);
        notifyWaitersUnlocked();
    }

    void notifyWaitersUnlocked() noexcept
    {
        for (auto it = waiters_.begin(); it != waiters_.end();) {
            auto waiter = it->lock();
            if (!waiter) {
                it = waiters_.erase(it);
                continue;
            }
            waiter->notify();
            ++it;
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable notEmptyCv_;
    std::condition_variable notFullCv_;
    std::optional<T> handoff_;
    bool closed_ { false };
    ChannelStats stats_;
    std::vector<std::weak_ptr<detail::ChannelWaiter>> waiters_;
};

template <typename T, typename Handler>
[[nodiscard]] SelectOp recv(ChannelReceiver<T> receiver, Handler handler)
{
    auto state = std::make_shared<Handler>(std::move(handler));
    return SelectOp(
        [receiver](const std::shared_ptr<detail::ChannelWaiter>& waiter) mutable -> Status {
            if (!receiver.valid())
                return Status::invalidArgument("recv operation receiver is not bound");
            receiver.addWaiter(waiter);
            return Status::ok();
        },
        [receiver, state]() mutable -> detail::SelectAttempt {
            auto value = receiver.tryReceive();
            if (!value.isOk()) {
                if (value.status().code() == StatusCode::Unavailable)
                    return { .ready_ = false, .status_ = Status::ok() };
                return { .ready_ = true, .status_ = value.status() };
            }

            ChannelReceive<T> received {
                .value_ = std::move(*value),
                .closed_ = !value->has_value(),
            };
            return {
                .ready_ = true,
                .status_ = detail::invokeReceiveHandler(*state, std::move(received)),
            };
        },
        {},
        false);
}

template <typename T, typename Handler>
[[nodiscard]] SelectOp send(ChannelSender<T> sender, T value, Handler handler)
{
    struct State {
        std::optional<T> value_;
        Handler handler_;
    };

    auto state = std::make_shared<State>(State {
        .value_ = std::move(value),
        .handler_ = std::move(handler),
    });

    return SelectOp(
        [sender](const std::shared_ptr<detail::ChannelWaiter>& waiter) mutable -> Status {
            if (!sender.valid())
                return Status::invalidArgument("send operation sender is not bound");
            sender.addWaiter(waiter);
            return Status::ok();
        },
        [sender, state]() mutable -> detail::SelectAttempt {
            if (!state->value_.has_value()) {
                return {
                    .ready_ = true,
                    .status_ = Status::failedPrecondition("send operation value has already been consumed"),
                };
            }

            auto status = sender.trySendFrom(*state->value_);
            if (!status.isOk()) {
                if (status.code() == StatusCode::ResourceExhausted)
                    return { .ready_ = false, .status_ = Status::ok() };
                return { .ready_ = true, .status_ = status };
            }

            state->value_.reset();
            return {
                .ready_ = true,
                .status_ = detail::invokeSelectHandler(state->handler_),
            };
        },
        {},
        false);
}

template <typename T>
[[nodiscard]] SelectOp send(ChannelSender<T> sender, T value)
{
    return send(std::move(sender), std::move(value), [] {});
}

template <typename Rep, typename Period, typename Handler>
[[nodiscard]] SelectOp after(std::chrono::duration<Rep, Period> delay, Handler handler)
{
    using Duration = std::chrono::steady_clock::duration;
    struct State {
        Duration delay_;
        std::optional<SelectOp::TimePoint> deadline_;
        Handler handler_;
    };

    auto state = std::make_shared<State>(State {
        .delay_ = std::chrono::duration_cast<Duration>(delay),
        .deadline_ = std::nullopt,
        .handler_ = std::move(handler),
    });

    return SelectOp(
        [state](const std::shared_ptr<detail::ChannelWaiter>&) -> Status {
            const auto now = std::chrono::steady_clock::now();
            state->deadline_ = state->delay_ <= Duration::zero() ? now : now + state->delay_;
            return Status::ok();
        },
        [state]() mutable -> detail::SelectAttempt {
            if (!state->deadline_.has_value())
                state->deadline_ = std::chrono::steady_clock::now() + state->delay_;
            if (std::chrono::steady_clock::now() < *state->deadline_)
                return { .ready_ = false, .status_ = Status::ok() };
            return {
                .ready_ = true,
                .status_ = detail::invokeSelectHandler(state->handler_),
            };
        },
        [state]() -> std::optional<SelectOp::TimePoint> {
            return state->deadline_;
        },
        false);
}

template <typename Rep, typename Period>
[[nodiscard]] SelectOp after(std::chrono::duration<Rep, Period> delay)
{
    return after(delay, [] {});
}

template <typename Handler>
[[nodiscard]] SelectOp cancelled(const CancellationToken& token, Handler handler)
{
    struct State {
        CancellationToken token_;
        Handler handler_;
        CancellationRegistration registration_;
    };

    auto state = std::make_shared<State>(State {
        .token_ = token,
        .handler_ = std::move(handler),
        .registration_ = {},
    });

    return SelectOp(
        [state](const std::shared_ptr<detail::ChannelWaiter>& waiter) mutable -> Status {
            if (!state->token_.cancellable() && !state->token_.cancelled())
                return Status::ok();
            state->registration_ = state->token_.onCancel([waiter] {
                waiter->notify();
            });
            return Status::ok();
        },
        [state]() mutable -> detail::SelectAttempt {
            if (!state->token_.cancelled())
                return { .ready_ = false, .status_ = Status::ok() };
            return {
                .ready_ = true,
                .status_ = detail::invokeCancelledHandler(state->handler_, state->token_),
            };
        },
        {},
        false);
}

[[nodiscard]] inline SelectOp cancelled(const CancellationToken& token)
{
    return cancelled(token, [] {});
}

template <typename Handler>
[[nodiscard]] SelectOp otherwise(Handler handler)
{
    auto state = std::make_shared<Handler>(std::move(handler));
    return SelectOp(
        {},
        [state]() mutable -> detail::SelectAttempt {
            return {
                .ready_ = true,
                .status_ = detail::invokeSelectHandler(*state),
            };
        },
        {},
        true);
}

[[nodiscard]] inline SelectOp otherwise()
{
    return otherwise([] {});
}

} // namespace lc
