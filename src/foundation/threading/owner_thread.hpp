#pragma once

#include "foundation/threading/thread_checks.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace lgc {

/// Small rule object for owner-thread state machines.
///
/// Components keep one of these instead of a raw `IThread` when their mutable state belongs to a
/// single executor. Public entry points dispatch through it; private state-mutating methods call
/// `check()` at the top. The class is intentionally foundation-only: names are diagnostic labels,
/// not business concepts such as "worker" or "chat".
class OwnerThread final {
public:
    OwnerThread() = default;

    OwnerThread(std::shared_ptr<IThread> executor,
        std::string_view owner,
        std::string_view name)
        : executor_(requireThread(std::move(executor), owner, name))
        , owner_(owner)
        , name_(name)
    {
    }

    [[nodiscard]] const std::shared_ptr<IThread>& executor() const noexcept { return executor_; }
    [[nodiscard]] IThread* get() const noexcept { return executor_.get(); }
    [[nodiscard]] bool isCurrent() const noexcept { return executor_ && executor_->isCurrentThread(); }

    void check(std::source_location from = std::source_location::current()) const
    {
        (void)from;
        requireOnThread(executor_, owner_, name_);
    }

    template <typename F>
    void dispatch(F&& task, std::source_location from = std::source_location::current()) const
    {
        requireThread(executor_, owner_, "dispatch")
            ->dispatch(std::function<void()>(std::forward<F>(task)), from);
    }

    template <typename F>
    void dispatchAsync(F&& task, std::source_location from = std::source_location::current()) const
    {
        requireThread(executor_, owner_, "dispatchAsync")
            ->dispatchAsync(std::function<void()>(std::forward<F>(task)), from);
    }

    template <typename Rep, typename Period, typename F>
    void dispatchAfter(std::chrono::duration<Rep, Period> delay,
        F&& task,
        std::source_location from = std::source_location::current()) const
    {
        requireThread(executor_, owner_, "dispatchAfter")
            ->dispatchAfter(std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay),
                std::function<void()>(std::forward<F>(task)),
                from);
    }

private:
    std::shared_ptr<IThread> executor_;
    std::string owner_;
    std::string name_;
};

} // namespace lgc
