#pragma once

#include <chrono>
#include <functional>

namespace lc {

/// Virtual timer surface (Qt `QTimer`-style semantics). Concrete backends may wrap Asio,
/// platform APIs, or test doubles.
///
/// **Single-shot without owning an `ITimer`**: concrete implementations typically expose a static
/// helper (e.g. `IntervalTimer::singleShot`) or a small free function tied to that backend.
class ITimer {
public:
    using Callback = std::function<void()>;
    using Milliseconds = std::chrono::milliseconds;

    virtual ~ITimer() = default;

    virtual void setInterval(Milliseconds interval) noexcept = 0;
    [[nodiscard]] virtual Milliseconds interval() const noexcept = 0;

    virtual void setSingleShot(bool singleShot) noexcept = 0;
    [[nodiscard]] virtual bool isSingleShot() const noexcept = 0;

    virtual void setTimeoutHandler(Callback handler) = 0;

    virtual void start() = 0;
    virtual void start(Milliseconds interval) = 0;

    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual bool isActive() const noexcept = 0;
};

} // namespace lc
