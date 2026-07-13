#pragma once

#include "foundation/event/i_event_sink.hpp"

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>

namespace lc {

struct CallbackEventSinkOptions {
    EventSinkOptions event_;
};

class CallbackEventSink final : public IEventSink {
public:
    using Callback = std::function<Status(const RuntimeEvent&)>;
    using VoidCallback = std::function<void(const RuntimeEvent&)>;

    explicit CallbackEventSink(Callback callback, CallbackEventSinkOptions options = {});
    explicit CallbackEventSink(VoidCallback callback, CallbackEventSinkOptions options = {});
    ~CallbackEventSink() override = default;

    CallbackEventSink(const CallbackEventSink&) = delete;
    CallbackEventSink& operator=(const CallbackEventSink&) = delete;
    CallbackEventSink(CallbackEventSink&&) = delete;
    CallbackEventSink& operator=(CallbackEventSink&&) = delete;

    [[nodiscard]] Status publish(RuntimeEvent event) override;
    [[nodiscard]] Status flush() override;
    [[nodiscard]] Status waitIdle(Duration timeout) override;
    [[nodiscard]] Status close(Duration waitIdleTimeout) override;
    [[nodiscard]] bool isClosed() const noexcept override;

private:
    [[nodiscard]] Status waitIdleLocked(std::unique_lock<std::mutex>& lock, Duration timeout);

    Callback callback_;
    CallbackEventSinkOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable idleCv_;
    std::size_t inFlight_ { 0 };
    bool closed_ { false };
};

} // namespace lc
