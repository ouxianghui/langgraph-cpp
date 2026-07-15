#pragma once

#include "foundation/status/status.hpp"

#include <chrono>
#include <functional>

namespace lgc {

/// Internal timer substrate. Runtime-facing scheduling should use `ITaskScheduler`.
///
/// Time reads and deadline calculations live in `foundation/time`; this interface only owns the
/// low-level scheduling/cancellation primitive used by tests and small adapters.
class ITimer {
public:
    using Callback = std::function<void()>;
    using Duration = std::chrono::milliseconds;

    virtual ~ITimer() = default;

    virtual void setInterval(Duration value) noexcept = 0;
    [[nodiscard]] virtual Duration interval() const noexcept = 0;

    virtual void setSingleShot(bool value) noexcept = 0;
    [[nodiscard]] virtual bool singleShot() const noexcept = 0;

    virtual void setHandler(Callback value) = 0;

    [[nodiscard]] virtual Status start() = 0;
    [[nodiscard]] virtual Status start(Duration interval) = 0;

    [[nodiscard]] virtual Status stop() = 0;
    [[nodiscard]] virtual bool active() const noexcept = 0;
    [[nodiscard]] virtual Status waitIdle(Duration timeout) = 0;
    [[nodiscard]] virtual Status close(Duration waitIdleTimeout) = 0;
    [[nodiscard]] virtual bool isClosed() const noexcept = 0;
};

} // namespace lgc
