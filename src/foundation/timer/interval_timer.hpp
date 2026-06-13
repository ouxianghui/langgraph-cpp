#pragma once

#include "foundation/timer/i_timer.hpp"

#include <asio/any_io_executor.hpp>

#include <memory>

namespace lc {

/// Asio `steady_timer` implementation of `ITimer` (monotonic clock).
///
/// - Callbacks run on an internal **strand** built from the constructor `executor` (pass e.g.
///   `io_context::get_executor()`; handlers never overlap).
/// - **Thread safety**: `start` / `stop` / setters may be called from any thread; internal work is
///   **serialized on the strand** created from the constructor executor (see implementation).
/// - **Lifetime**: safe to destroy from any thread; pending waits are cancelled and handlers exit
///   without invoking the user callback after shutdown.
///
/// Mapping to Qt: `setInterval` / `interval`, `setSingleShot` / `isSingleShot`, `start` / `stop` /
/// `isActive`, timeout → `setTimeoutHandler`.
class IntervalTimer final : public ITimer {
public:
    /// Build timer bound to `executor` (internally wrapped in an Asio strand for ordered access).
    explicit IntervalTimer(asio::any_io_executor executor);

    IntervalTimer(const IntervalTimer&) = delete;
    IntervalTimer& operator=(const IntervalTimer&) = delete;
    IntervalTimer(IntervalTimer&&) noexcept;
    IntervalTimer& operator=(IntervalTimer&&) noexcept;
    ~IntervalTimer() override;

    /// Zero or negative intervals are rejected by `start()` (no-op); coerced to 1 ms when using
    /// `start(Milliseconds)` if you pass non-positive (see `.cpp`).
    void setInterval(Milliseconds interval) noexcept override;

    [[nodiscard]] Milliseconds interval() const noexcept override;

    void setSingleShot(bool singleShot) noexcept override;

    [[nodiscard]] bool isSingleShot() const noexcept override;

    void setTimeoutHandler(Callback handler) override;

    /// Arm timer using current `interval()` (must be > 0). Restarts if already active (Qt-like).
    void start() override;

    /// Set interval then `start()`.
    void start(Milliseconds interval) override;

    void stop() noexcept override;

    [[nodiscard]] bool isActive() const noexcept override;

    /// Qt `QTimer::singleShot`: invoke `handler` once after `delay` on `executor` (Asio backend).
    static void singleShot(asio::any_io_executor executor, Milliseconds delay, Callback handler);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace lc
