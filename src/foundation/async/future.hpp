#pragma once

#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"

#include <chrono>
#include <concepts>
#include <future>
#include <memory>
#include <mutex>
#include <utility>

namespace lgc {

template <typename T>
class Promise;

template <typename T>
class Future;

namespace detail {

[[nodiscard]] inline Status normalizeFutureError(Status status)
{
    if (status.isOk())
        return Status::internal("future error status cannot be ok");
    return status;
}

template <typename T>
struct FutureState {
    using StoredResult = Result<T>;
    using StoredPtr = std::shared_ptr<StoredResult>;

    FutureState()
        : future_(promise_.get_future().share())
    {
    }

    mutable std::mutex mutex_;
    std::promise<StoredPtr> promise_;
    std::shared_future<StoredPtr> future_;
    bool fulfilled_ { false };
    bool consumed_ { false };
};

template <>
struct FutureState<void> {
    using StoredResult = Result<void>;
    using StoredPtr = std::shared_ptr<StoredResult>;

    FutureState()
        : future_(promise_.get_future().share())
    {
    }

    mutable std::mutex mutex_;
    std::promise<StoredPtr> promise_;
    std::shared_future<StoredPtr> future_;
    bool fulfilled_ { false };
};

} // namespace detail

/// Read side of a one-shot asynchronous result.
///
/// This is a thin Status/Result wrapper around `std::promise` + `std::shared_future`. `Future<T>`
/// is copyable; `get()` is available for copyable values and `take()` moves the value exactly once.
template <typename T>
class Future final {
public:
    using value_type = T;
    using Duration = std::chrono::steady_clock::duration;

    Future() = default;

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }

    [[nodiscard]] bool ready() const
    {
        if (!state_)
            return false;
        return state_->future_.wait_for(Duration::zero()) == std::future_status::ready;
    }

    void wait() const
    {
        if (state_)
            state_->future_.wait();
    }

    [[nodiscard]] bool wait(Duration timeout) const
    {
        if (!state_)
            return false;
        if (timeout <= Duration::zero())
            return ready();
        return state_->future_.wait_for(timeout) == std::future_status::ready;
    }

    [[nodiscard]] Result<T> get() const
        requires std::copy_constructible<T>
    {
        if (!state_)
            return Status::failedPrecondition("future is not valid");

        auto stored = state_->future_.get();
        if (!stored)
            return Status::internal("future is ready without a result");
        if (!stored->isOk())
            return stored->status();
        return stored->value();
    }

    [[nodiscard]] Result<T> take()
    {
        if (!state_)
            return Status::failedPrecondition("future is not valid");

        auto stored = state_->future_.get();
        if (!stored)
            return Status::internal("future is ready without a result");
        if (!stored->isOk())
            return stored->status();

        std::lock_guard lock(state_->mutex_);
        if (state_->consumed_)
            return Status::failedPrecondition("future value has already been consumed");

        state_->consumed_ = true;
        return std::move(stored->value());
    }

private:
    friend class Promise<T>;

    explicit Future(std::shared_ptr<detail::FutureState<T>> state)
        : state_(std::move(state))
    {
    }

    std::shared_ptr<detail::FutureState<T>> state_;
};

template <>
class Future<void> final {
public:
    using value_type = void;
    using Duration = std::chrono::steady_clock::duration;

    Future() = default;

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }

    [[nodiscard]] bool ready() const
    {
        if (!state_)
            return false;
        return state_->future_.wait_for(Duration::zero()) == std::future_status::ready;
    }

    void wait() const
    {
        if (state_)
            state_->future_.wait();
    }

    [[nodiscard]] bool wait(Duration timeout) const
    {
        if (!state_)
            return false;
        if (timeout <= Duration::zero())
            return ready();
        return state_->future_.wait_for(timeout) == std::future_status::ready;
    }

    [[nodiscard]] Result<void> get() const
    {
        if (!state_)
            return Status::failedPrecondition("future is not valid");

        auto stored = state_->future_.get();
        if (!stored)
            return Status::internal("future is ready without a result");
        if (!stored->isOk())
            return stored->status();
        return okResult();
    }

private:
    friend class Promise<void>;

    explicit Future(std::shared_ptr<detail::FutureState<void>> state)
        : state_(std::move(state))
    {
    }

    std::shared_ptr<detail::FutureState<void>> state_;
};

/// Write side of a one-shot asynchronous result.
template <typename T>
class Promise final {
public:
    Promise()
        : state_(std::make_shared<detail::FutureState<T>>())
    {
    }

    ~Promise() noexcept
    {
        abandon();
    }

    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;
    Promise(Promise&&) noexcept = default;
    Promise& operator=(Promise&&) noexcept = default;

    [[nodiscard]] Future<T> future() const { return Future<T>(state_); }

    [[nodiscard]] Status resolve(T value)
    {
        if (tryResolve(std::move(value)))
            return Status::ok();
        return Status::failedPrecondition("promise is already fulfilled");
    }

    [[nodiscard]] bool tryResolve(T value)
    {
        if (!state_)
            return false;

        auto result = std::make_shared<Result<T>>(std::move(value));
        return fulfill(std::move(result));
    }

    [[nodiscard]] Status reject(Status status)
    {
        if (tryReject(std::move(status)))
            return Status::ok();
        return Status::failedPrecondition("promise is already fulfilled");
    }

    [[nodiscard]] bool tryReject(Status status)
    {
        if (!state_)
            return false;

        auto result = std::make_shared<Result<T>>(detail::normalizeFutureError(std::move(status)));
        return fulfill(std::move(result));
    }

private:
    void abandon() noexcept
    {
        if (!state_)
            return;

        try {
            (void)tryReject(Status(StatusCode::Cancelled));
        } catch (...) {
            state_.reset();
        }
    }

    [[nodiscard]] bool fulfill(typename detail::FutureState<T>::StoredPtr result)
    {
        std::lock_guard lock(state_->mutex_);
        if (state_->fulfilled_)
            return false;

        state_->fulfilled_ = true;
        state_->promise_.set_value(std::move(result));
        return true;
    }

    std::shared_ptr<detail::FutureState<T>> state_;
};

template <>
class Promise<void> final {
public:
    Promise()
        : state_(std::make_shared<detail::FutureState<void>>())
    {
    }

    ~Promise() noexcept
    {
        abandon();
    }

    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;
    Promise(Promise&&) noexcept = default;
    Promise& operator=(Promise&&) noexcept = default;

    [[nodiscard]] Future<void> future() const { return Future<void>(state_); }

    [[nodiscard]] Status resolve()
    {
        if (tryResolve())
            return Status::ok();
        return Status::failedPrecondition("promise is already fulfilled");
    }

    [[nodiscard]] bool tryResolve()
    {
        if (!state_)
            return false;

        auto result = std::make_shared<Result<void>>(okResult());
        return fulfill(std::move(result));
    }

    [[nodiscard]] Status reject(Status status)
    {
        if (tryReject(std::move(status)))
            return Status::ok();
        return Status::failedPrecondition("promise is already fulfilled");
    }

    [[nodiscard]] bool tryReject(Status status)
    {
        if (!state_)
            return false;

        auto result = std::make_shared<Result<void>>(detail::normalizeFutureError(std::move(status)));
        return fulfill(std::move(result));
    }

private:
    void abandon() noexcept
    {
        if (!state_)
            return;

        try {
            (void)tryReject(Status(StatusCode::Cancelled));
        } catch (...) {
            state_.reset();
        }
    }

    [[nodiscard]] bool fulfill(typename detail::FutureState<void>::StoredPtr result)
    {
        std::lock_guard lock(state_->mutex_);
        if (state_->fulfilled_)
            return false;

        state_->fulfilled_ = true;
        state_->promise_.set_value(std::move(result));
        return true;
    }

    std::shared_ptr<detail::FutureState<void>> state_;
};

} // namespace lgc
