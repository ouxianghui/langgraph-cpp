#pragma once

#include "foundation/cancellation/cancellation_token.hpp"
#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"

#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace lc {

namespace detail {

class ChannelWaiter final {
public:
    void notify() noexcept
    {
        {
            std::lock_guard lock(mutex_);
            ++generation_;
        }
        cv_.notify_all();
    }

    [[nodiscard]] std::uint64_t generation() const noexcept
    {
        std::lock_guard lock(mutex_);
        return generation_;
    }

    void wait(std::uint64_t observed)
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] {
            return generation_ != observed;
        });
    }

    template <typename Duration>
    [[nodiscard]] bool waitFor(std::uint64_t observed, Duration timeout)
    {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return generation_ != observed;
        });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::uint64_t generation_ { 0 };
};

} // namespace detail

struct ChannelStats {
    std::size_t capacity_ { 0 };
    std::size_t size_ { 0 };
    bool closed_ { false };
    std::uint64_t sent_ { 0 };
    std::uint64_t received_ { 0 };
    std::uint64_t sendTimeouts_ { 0 };
    std::uint64_t receiveTimeouts_ { 0 };
    std::uint64_t rejectedAfterClose_ { 0 };
};

template <typename T>
class ChannelSender;

template <typename T>
class ChannelReceiver;

class SelectOp;

template <typename T>
struct ChannelReceive {
    std::optional<T> value_;
    bool closed_ { false };
};

namespace detail {

template <typename>
inline constexpr bool dependentFalse = false;

struct SelectAttempt {
    bool ready_ { false };
    Status status_;
};

[[nodiscard]] inline std::uint64_t mixSelectSeed(std::uint64_t value) noexcept
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] inline std::uint64_t initialSelectSeed() noexcept
{
    static std::atomic<std::uint64_t> counter { 0x9e3779b97f4a7c15ULL };
    const auto tick = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto sequence = counter.fetch_add(0x9e3779b97f4a7c15ULL, std::memory_order_relaxed);
    return mixSelectSeed(tick ^ sequence);
}

[[nodiscard]] inline std::uint64_t nextSelectRandom() noexcept
{
    thread_local std::uint64_t state = initialSelectSeed();
    state = mixSelectSeed(state);
    return state;
}

[[nodiscard]] inline std::size_t uniformSelectIndex(std::size_t upperBound) noexcept
{
    if (upperBound == 0)
        return 0;

    const auto bound = static_cast<std::uint64_t>(upperBound);
    const auto limit = std::numeric_limits<std::uint64_t>::max()
        - (std::numeric_limits<std::uint64_t>::max() % bound);

    std::uint64_t value = 0;
    do {
        value = nextSelectRandom();
    } while (value >= limit);
    return static_cast<std::size_t>(value % bound);
}

inline void shuffleSelectOrder(std::vector<std::size_t>& order) noexcept
{
    if (order.size() < 2)
        return;

    for (std::size_t i = order.size() - 1; i > 0; --i) {
        const auto j = uniformSelectIndex(i + 1);
        using std::swap;
        swap(order[i], order[j]);
    }
}

template <typename Handler, typename... Args>
[[nodiscard]] Status invokeSelectHandler(Handler& handler, Args&&... args)
{
    try {
        if constexpr (std::same_as<std::invoke_result_t<Handler&, Args...>, Status>) {
            return std::invoke(handler, std::forward<Args>(args)...);
        } else if constexpr (std::is_void_v<std::invoke_result_t<Handler&, Args...>>) {
            std::invoke(handler, std::forward<Args>(args)...);
            return Status::ok();
        } else {
            static_assert(dependentFalse<Handler>, "select case handlers must return void or lc::Status");
        }
    } catch (const std::exception& e) {
        return Status::internal(std::string("select case handler threw: ") + e.what());
    } catch (...) {
        return Status::internal("select case handler threw");
    }
}

template <typename Handler, typename T>
[[nodiscard]] Status invokeReceiveHandler(Handler& handler, ChannelReceive<T> received)
{
    if constexpr (std::is_invocable_v<Handler&, ChannelReceive<T>>) {
        return invokeSelectHandler(handler, std::move(received));
    } else if constexpr (std::is_invocable_v<Handler&, std::optional<T>>) {
        return invokeSelectHandler(handler, std::move(received.value_));
    } else if constexpr (std::is_invocable_v<Handler&, T>) {
        if (!received.value_.has_value())
            return Status::unavailable("channel is closed");
        return invokeSelectHandler(handler, std::move(*received.value_));
    } else {
        static_assert(
            dependentFalse<Handler>,
            "recv handlers must accept lc::ChannelReceive<T>, std::optional<T>, or T");
    }
}

template <typename Handler>
[[nodiscard]] Status invokeCancelledHandler(Handler& handler, const CancellationToken& token)
{
    if constexpr (std::is_invocable_v<Handler&, const CancellationToken&>) {
        return invokeSelectHandler(handler, token);
    } else if constexpr (std::is_invocable_v<Handler&, std::string>) {
        return invokeSelectHandler(handler, token.reason());
    } else if constexpr (std::is_invocable_v<Handler&>) {
        return invokeSelectHandler(handler);
    } else {
        static_assert(
            dependentFalse<Handler>,
            "cancelled handlers must accept no arguments, lc::CancellationToken, or std::string");
    }
}

} // namespace detail

} // namespace lc
