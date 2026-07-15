#pragma once

#include "foundation/event/runtime_event.hpp"
#include "foundation/status/status.hpp"

#include <chrono>
#include <memory>

namespace lgc {

class Redactor;

enum class EventOverflowPolicy {
    Reject,
    DropOldest,
    DropNewest,
};

struct EventSinkOptions {
    RuntimeEventLimits limits_;
    std::shared_ptr<const Redactor> redactor_;
    bool redact_ { true };
};

class IEventSink {
public:
    using Duration = std::chrono::steady_clock::duration;

    virtual ~IEventSink() = default;

    IEventSink(const IEventSink&) = delete;
    IEventSink& operator=(const IEventSink&) = delete;
    IEventSink(IEventSink&&) = delete;
    IEventSink& operator=(IEventSink&&) = delete;

protected:
    IEventSink() = default;

public:
    [[nodiscard]] virtual Status publish(RuntimeEvent event) = 0;

    /// Drain work accepted before this call. Purely synchronous sinks may complete immediately.
    [[nodiscard]] virtual Status flush() = 0;

    /// Wait until accepted events are no longer queued or in-flight.
    [[nodiscard]] virtual Status waitIdle(Duration timeout) = 0;

    /// Stop accepting new events and wait for already accepted events according to the timeout.
    [[nodiscard]] virtual Status close(Duration waitIdleTimeout) = 0;

    [[nodiscard]] virtual bool isClosed() const noexcept = 0;
};

} // namespace lgc
