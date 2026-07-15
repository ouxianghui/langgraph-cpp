#pragma once

#include "foundation/status/status.hpp"

#include <concepts>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace lgc {

namespace detail {

[[nodiscard]] inline Status normalizeResultError(Status status)
{
    if (status.isOk())
        return Status::internal("Result error status cannot be ok");
    return status;
}

[[nodiscard]] inline const Status& okStatusSingleton()
{
    static const Status ok = Status::ok();
    return ok;
}

} // namespace detail

template <typename T>
class Result final {
    static_assert(!std::is_reference_v<T>, "Result<T> does not support reference types");
    static_assert(!std::same_as<std::remove_cv_t<T>, Status>, "Result<Status> is not supported");

public:
    using value_type = T;

    Result(const T& value)
        requires std::copy_constructible<T>
        : storage_(value)
    {
    }

    Result(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : storage_(std::move(value))
    {
    }

    Result(Status status)
        : storage_(detail::normalizeResultError(std::move(status)))
    {
    }

    template <typename... Args>
        requires std::constructible_from<T, Args...>
    [[nodiscard]] static Result fromValue(Args&&... args)
    {
        return Result(T(std::forward<Args>(args)...));
    }

    [[nodiscard]] static Result fromStatus(Status status)
    {
        return Result(std::move(status));
    }

    [[nodiscard]] bool isOk() const noexcept
    {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] explicit operator bool() const noexcept { return isOk(); }

    [[nodiscard]] const Status& status() const noexcept
    {
        if (const auto* status = std::get_if<Status>(&storage_))
            return *status;
        return detail::okStatusSingleton();
    }

    [[nodiscard]] T& value() &
    {
        ensureValue();
        return std::get<T>(storage_);
    }

    [[nodiscard]] const T& value() const&
    {
        ensureValue();
        return std::get<T>(storage_);
    }

    [[nodiscard]] T&& value() &&
    {
        ensureValue();
        return std::move(std::get<T>(storage_));
    }

    [[nodiscard]] const T&& value() const&&
    {
        ensureValue();
        return std::move(std::get<T>(storage_));
    }

    [[nodiscard]] T* operator->()
    {
        return &value();
    }

    [[nodiscard]] const T* operator->() const
    {
        return &value();
    }

    [[nodiscard]] T& operator*() &
    {
        return value();
    }

    [[nodiscard]] const T& operator*() const&
    {
        return value();
    }

    [[nodiscard]] T&& operator*() &&
    {
        return std::move(*this).value();
    }

private:
    void ensureValue() const
    {
        if (!isOk())
            throw std::logic_error(status().toString());
    }

    std::variant<T, Status> storage_;
};

template <>
class Result<void> final {
public:
    using value_type = void;

    Result() = default;

    Result(Status status)
        : status_(detail::normalizeResultError(std::move(status)))
    {
    }

    [[nodiscard]] static Result ok() noexcept { return Result {}; }

    [[nodiscard]] static Result fromStatus(Status status)
    {
        return Result(std::move(status));
    }

    [[nodiscard]] bool isOk() const noexcept { return status_.isOk(); }
    [[nodiscard]] explicit operator bool() const noexcept { return isOk(); }

    [[nodiscard]] const Status& status() const noexcept { return status_; }

    void value() const
    {
        if (!isOk())
            throw std::logic_error(status_.toString());
    }

private:
    Status status_;
};

template <typename T>
[[nodiscard]] Result<std::decay_t<T>> okResult(T&& value)
{
    return Result<std::decay_t<T>>(std::forward<T>(value));
}

[[nodiscard]] inline Result<void> okResult() noexcept
{
    return Result<void>::ok();
}

template <typename T>
[[nodiscard]] Result<T> errorResult(Status status)
{
    return Result<T>::fromStatus(std::move(status));
}

} // namespace lgc
