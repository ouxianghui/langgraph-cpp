#pragma once

#include "foundation/status/status.hpp"
#include "foundation/threading/owner_thread.hpp"

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace lgc {

/// Owner-thread guard with executor-style naming.
class OwnerExecutor final {
public:
    OwnerExecutor() = default;

    OwnerExecutor(
        std::shared_ptr<IThread> thread,
        std::string_view owner,
        std::string_view name)
        : owner_(std::move(thread), owner, name)
    {
    }

    [[nodiscard]] const std::shared_ptr<IThread>& thread() const noexcept { return owner_.executor(); }
    [[nodiscard]] IThread* get() const noexcept { return owner_.get(); }
    [[nodiscard]] bool isCurrent() const noexcept { return owner_.isCurrent(); }

    void check(std::source_location from = std::source_location::current()) const
    {
        owner_.check(from);
    }

    template <typename F>
    [[nodiscard]] Status post(F&& task, std::source_location from = std::source_location::current()) const
    {
        try {
            owner_.dispatchAsync(std::forward<F>(task), from);
            return Status::ok();
        } catch (const std::exception& error) {
            return Status::failedPrecondition(error.what());
        } catch (...) {
            return Status::failedPrecondition("owner executor post failed");
        }
    }

    template <typename Rep, typename Period, typename F>
    [[nodiscard]] Status postDelayed(
        std::chrono::duration<Rep, Period> delay,
        F&& task,
        std::source_location from = std::source_location::current()) const
    {
        try {
            owner_.dispatchAfter(delay, std::forward<F>(task), from);
            return Status::ok();
        } catch (const std::exception& error) {
            return Status::failedPrecondition(error.what());
        } catch (...) {
            return Status::failedPrecondition("owner executor postDelayed failed");
        }
    }

    template <typename F>
    [[nodiscard]] Status execute(F&& task, std::source_location from = std::source_location::current()) const
    {
        try {
            owner_.dispatch(std::forward<F>(task), from);
            return Status::ok();
        } catch (const std::exception& error) {
            return Status::failedPrecondition(error.what());
        } catch (...) {
            return Status::failedPrecondition("owner executor execute failed");
        }
    }

    template <typename F>
    [[nodiscard]] Status executeAndWait(F&& task, std::source_location from = std::source_location::current()) const
    {
        try {
            requireThread("executeAndWait")->dispatchSync(std::function<void()>(std::forward<F>(task)), from);
            return Status::ok();
        } catch (const std::exception& error) {
            return Status::failedPrecondition(error.what());
        } catch (...) {
            return Status::failedPrecondition("owner executor executeAndWait failed");
        }
    }

private:
    [[nodiscard]] IThread* requireThread(std::string_view operation) const
    {
        auto* thread = owner_.get();
        if (!thread)
            throw std::invalid_argument("OwnerExecutor: required executor is not configured: " + std::string(operation));
        return thread;
    }

    OwnerThread owner_;
};

} // namespace lgc
