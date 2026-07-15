#pragma once

#include "foundation/logging/logger.hpp"
#include "foundation/timer/i_timer.hpp"

#include <memory>

namespace lgc {

class TimerHandle final {
public:
    using Duration = ITimer::Duration;

    TimerHandle() = default;
    ~TimerHandle();

    TimerHandle(const TimerHandle&) = delete;
    TimerHandle& operator=(const TimerHandle&) = delete;
    TimerHandle(TimerHandle&&) noexcept;
    TimerHandle& operator=(TimerHandle&&) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] Status cancel();
    [[nodiscard]] Status wait(Duration timeout);

private:
    friend class IntervalTimer;

    struct State;
    explicit TimerHandle(std::shared_ptr<State> state);

    std::shared_ptr<State> state_;
};

/// Internal standard-library `steady_clock` implementation of `ITimer`.
///
/// Runtime scheduling should prefer `TaskScheduler::scheduleAfter` / `schedulePeriodic`; this class is
/// kept as a small utility for tests, adapters, and components that need an owned timer primitive.
///
/// - Callbacks run on a private worker thread and never overlap for one timer instance.
/// - **Thread safety**: `start` / `stop` / setters may be called from any thread.
/// - **Lifetime**: safe to destroy from any thread; pending waits are cancelled and handlers exit
///   without invoking the user callback after shutdown.
///
class IntervalTimer final : public ITimer {
public:
    explicit IntervalTimer(std::shared_ptr<ILogger> logger = Logger::defaultLogger());

    IntervalTimer(const IntervalTimer&) = delete;
    IntervalTimer& operator=(const IntervalTimer&) = delete;
    IntervalTimer(IntervalTimer&&) noexcept;
    IntervalTimer& operator=(IntervalTimer&&) noexcept;
    ~IntervalTimer() override;

    /// Zero or negative intervals are rejected by `start()` (no-op); coerced to 1 ms when using
    /// `start(Duration)` if you pass non-positive (see `.cpp`).
    void setInterval(Duration value) noexcept override;

    [[nodiscard]] Duration interval() const noexcept override;

    void setSingleShot(bool value) noexcept override;

    [[nodiscard]] bool singleShot() const noexcept override;

    void setHandler(Callback value) override;

    /// Arm timer using current `interval()` (must be > 0). Restarts if already active (Qt-like).
    [[nodiscard]] Status start() override;

    /// Set interval then `start()`.
    [[nodiscard]] Status start(Duration interval) override;

    [[nodiscard]] Status stop() override;

    [[nodiscard]] bool active() const noexcept override;
    [[nodiscard]] Status waitIdle(Duration timeout) override;
    [[nodiscard]] Status close(Duration waitIdleTimeout) override;
    [[nodiscard]] bool isClosed() const noexcept override;

    /// Schedule `handler` once after `delay`. The returned handle owns the task; destroying it cancels
    /// and waits for completion.
    [[nodiscard]] static TimerHandle singleShot(
        Duration delay,
        Callback handler,
        std::shared_ptr<ILogger> logger = Logger::defaultLogger());

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace lgc
