#pragma once

#include "foundation/async/channel_sender_receiver.hpp"

namespace lc {

/// Thread-safe bounded FIFO channel for streaming and backpressure.
///
/// `send` blocks while the channel is full. `receive` returns `std::nullopt` only when the channel
/// is closed and drained.
template <typename T>
class BoundedChannel final : public IChannel<T> {
public:
    using Duration = typename IChannel<T>::Duration;

    explicit BoundedChannel(std::size_t capacity)
        : capacity_(capacity)
    {
        if (capacity_ == 0)
            throw std::invalid_argument("BoundedChannel capacity must be greater than zero");
    }

    BoundedChannel(const BoundedChannel&) = delete;
    BoundedChannel& operator=(const BoundedChannel&) = delete;
    BoundedChannel(BoundedChannel&&) = delete;
    BoundedChannel& operator=(BoundedChannel&&) = delete;

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
            return closed_ || queue_.size() < capacity_ || token.cancelled();
        });
        if (auto status = token.check("channel send cancelled"); !status.isOk())
            return status;
        return pushUnlocked(std::move(value));
    }

    [[nodiscard]] Status sendFor(T value, Duration timeout) override
    {
        return sendFor(std::move(value), timeout, CancellationToken::none());
    }

    [[nodiscard]] Status sendFor(T value, Duration timeout, const CancellationToken& token) override
    {
        if (auto status = token.check("channel send cancelled"); !status.isOk())
            return status;

        auto registration = token.onCancel([this] {
            notifyBlockedOperations();
        });

        std::unique_lock lock(mutex_);
        if (timeout <= Duration::zero()) {
            if (queue_.size() >= capacity_ && !closed_) {
                ++stats_.sendTimeouts_;
                return Status::deadlineExceeded("channel send timed out");
            }
        } else if (!notFullCv_.wait_for(lock, timeout, [this, &token] {
                       return closed_ || queue_.size() < capacity_ || token.cancelled();
                   })) {
            ++stats_.sendTimeouts_;
            return Status::deadlineExceeded("channel send timed out");
        }
        if (auto status = token.check("channel send cancelled"); !status.isOk())
            return status;
        return pushUnlocked(std::move(value));
    }

    [[nodiscard]] Status trySend(T value)
    {
        return trySendFrom(value);
    }

private:
    [[nodiscard]] Status trySendFrom(T& value) override
    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            ++stats_.rejectedAfterClose_;
            return Status::unavailable("channel is closed");
        }
        if (queue_.size() >= capacity_)
            return Status::resourceExhausted("channel is full");
        return pushUnlocked(std::move(value));
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
            return closed_ || !queue_.empty() || token.cancelled();
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
            if (queue_.empty() && !closed_) {
                ++stats_.receiveTimeouts_;
                return Status::deadlineExceeded("channel receive timed out");
            }
        } else if (!notEmptyCv_.wait_for(lock, timeout, [this, &token] {
                       return closed_ || !queue_.empty() || token.cancelled();
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

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] std::size_t size() const noexcept
    {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] bool empty() const noexcept
    {
        std::lock_guard lock(mutex_);
        return queue_.empty();
    }

    [[nodiscard]] ChannelStats stats() const noexcept
    {
        std::lock_guard lock(mutex_);
        auto out = stats_;
        out.capacity_ = capacity_;
        out.size_ = queue_.size();
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
    [[nodiscard]] Status pushUnlocked(T value)
    {
        if (closed_) {
            ++stats_.rejectedAfterClose_;
            return Status::unavailable("channel is closed");
        }
        if (queue_.size() >= capacity_)
            return Status::resourceExhausted("channel is full");

        queue_.push_back(std::move(value));
        ++stats_.sent_;
        notifyWaitersUnlocked();
        notEmptyCv_.notify_one();
        return Status::ok();
    }

    [[nodiscard]] Result<std::optional<T>> popUnlocked()
    {
        if (queue_.empty()) {
            if (closed_)
                return std::optional<T> {};
            return Status::unavailable("channel is empty");
        }

        T value = std::move(queue_.front());
        queue_.pop_front();
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

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable notEmptyCv_;
    std::condition_variable notFullCv_;
    std::deque<T> queue_;
    bool closed_ { false };
    ChannelStats stats_;
    std::vector<std::weak_ptr<detail::ChannelWaiter>> waiters_;
};

} // namespace lc
