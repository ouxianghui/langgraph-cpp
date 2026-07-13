#pragma once

#include "foundation/async/channel_select.hpp"

namespace lc {

template <typename T>
class IChannel {
public:
    using Duration = std::chrono::steady_clock::duration;

    virtual ~IChannel() = default;

    IChannel(const IChannel&) = delete;
    IChannel& operator=(const IChannel&) = delete;
    IChannel(IChannel&&) = delete;
    IChannel& operator=(IChannel&&) = delete;

protected:
    IChannel() = default;

public:
    [[nodiscard]] virtual Status send(T value) = 0;
    [[nodiscard]] virtual Status send(T value, const CancellationToken& token) = 0;
    [[nodiscard]] virtual Status sendFor(T value, Duration timeout) = 0;
    [[nodiscard]] virtual Status sendFor(T value, Duration timeout, const CancellationToken& token) = 0;

    [[nodiscard]] virtual Result<std::optional<T>> receive() = 0;
    [[nodiscard]] virtual Result<std::optional<T>> receive(const CancellationToken& token) = 0;
    [[nodiscard]] virtual Result<std::optional<T>> receiveFor(Duration timeout) = 0;
    [[nodiscard]] virtual Result<std::optional<T>> receiveFor(Duration timeout, const CancellationToken& token) = 0;
    [[nodiscard]] virtual Result<std::optional<T>> tryReceive() = 0;

    virtual void close() noexcept = 0;
    [[nodiscard]] virtual bool isClosed() const noexcept = 0;
    virtual void addWaiter(std::shared_ptr<detail::ChannelWaiter> waiter) = 0;

protected:
    template <typename>
    friend class ChannelSender;

    [[nodiscard]] virtual Status trySendFrom(T& value) = 0;
};

template <typename T>
class ChannelSender final {
public:
    using Duration = typename IChannel<T>::Duration;

    ChannelSender() noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return channel_ != nullptr; }

    [[nodiscard]] Status send(T value) const
    {
        if (!channel_)
            return Status::failedPrecondition("channel sender is not bound");
        return channel_->send(std::move(value));
    }

    [[nodiscard]] Status send(T value, const CancellationToken& token) const
    {
        if (!channel_)
            return Status::failedPrecondition("channel sender is not bound");
        return channel_->send(std::move(value), token);
    }

    [[nodiscard]] Status sendFor(T value, Duration timeout) const
    {
        if (!channel_)
            return Status::failedPrecondition("channel sender is not bound");
        return channel_->sendFor(std::move(value), timeout);
    }

    [[nodiscard]] Status sendFor(T value, Duration timeout, const CancellationToken& token) const
    {
        if (!channel_)
            return Status::failedPrecondition("channel sender is not bound");
        return channel_->sendFor(std::move(value), timeout, token);
    }

    void close() const noexcept
    {
        if (channel_)
            channel_->close();
    }

    [[nodiscard]] bool isClosed() const noexcept
    {
        return channel_ == nullptr || channel_->isClosed();
    }

private:
    template <typename>
    friend class BoundedChannel;
    template <typename>
    friend class UnbufferedChannel;
    template <typename U, typename Handler>
    friend SelectOp send(ChannelSender<U> sender, U value, Handler handler);
    template <typename U>
    friend SelectOp send(ChannelSender<U> sender, U value);

    explicit ChannelSender(IChannel<T>* channel) noexcept
        : channel_(channel)
    {
    }

    [[nodiscard]] Status trySendFrom(T& value) const
    {
        if (!channel_)
            return Status::failedPrecondition("channel sender is not bound");
        return channel_->trySendFrom(value);
    }

    void addWaiter(std::shared_ptr<detail::ChannelWaiter> waiter) const
    {
        if (channel_)
            channel_->addWaiter(std::move(waiter));
    }

    IChannel<T>* channel_ { nullptr };
};

template <typename T>
class ChannelReceiver final {
public:
    using Duration = typename IChannel<T>::Duration;

    ChannelReceiver() noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return channel_ != nullptr; }

    [[nodiscard]] Result<std::optional<T>> receive() const
    {
        if (!channel_)
            return Status::failedPrecondition("channel receiver is not bound");
        return channel_->receive();
    }

    [[nodiscard]] Result<std::optional<T>> receive(const CancellationToken& token) const
    {
        if (!channel_)
            return Status::failedPrecondition("channel receiver is not bound");
        return channel_->receive(token);
    }

    [[nodiscard]] Result<std::optional<T>> receiveFor(Duration timeout) const
    {
        if (!channel_)
            return Status::failedPrecondition("channel receiver is not bound");
        return channel_->receiveFor(timeout);
    }

    [[nodiscard]] Result<std::optional<T>> receiveFor(Duration timeout, const CancellationToken& token) const
    {
        if (!channel_)
            return Status::failedPrecondition("channel receiver is not bound");
        return channel_->receiveFor(timeout, token);
    }

    [[nodiscard]] Result<std::optional<T>> tryReceive() const
    {
        if (!channel_)
            return Status::failedPrecondition("channel receiver is not bound");
        return channel_->tryReceive();
    }

    void close() const noexcept
    {
        if (channel_)
            channel_->close();
    }

    [[nodiscard]] bool isClosed() const noexcept
    {
        return channel_ == nullptr || channel_->isClosed();
    }

private:
    template <typename>
    friend class BoundedChannel;
    template <typename>
    friend class UnbufferedChannel;
    template <typename U, typename Handler>
    friend SelectOp recv(ChannelReceiver<U> receiver, Handler handler);

    explicit ChannelReceiver(IChannel<T>* channel) noexcept
        : channel_(channel)
    {
    }

    void addWaiter(std::shared_ptr<detail::ChannelWaiter> waiter) const
    {
        if (channel_)
            channel_->addWaiter(std::move(waiter));
    }

    IChannel<T>* channel_ { nullptr };
};

} // namespace lc
