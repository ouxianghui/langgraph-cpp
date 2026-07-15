#include "foundation/event/callback_event_sink.hpp"

#include "foundation/event/event_sink_utils.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

namespace lgc {

CallbackEventSink::CallbackEventSink(Callback callback, CallbackEventSinkOptions options)
    : callback_(std::move(callback))
    , options_(std::move(options))
{
    if (!callback_)
        throw std::invalid_argument("CallbackEventSink requires a non-empty callback");
}

CallbackEventSink::CallbackEventSink(VoidCallback callback, CallbackEventSinkOptions options)
    : CallbackEventSink(Callback([cb = std::move(callback)](const RuntimeEvent& event) mutable {
          if (!cb)
              return Status::failedPrecondition("event callback is empty");
          cb(event);
          return Status::ok();
      }),
          std::move(options))
{
}

Status CallbackEventSink::publish(RuntimeEvent event)
{
    auto prepared = detail::prepareRuntimeEvent(std::move(event), options_.event_);
    if (!prepared.isOk())
        return prepared.status();

    Callback callback;
    {
        std::lock_guard lock(mutex_);
        if (closed_)
            return Status::unavailable("event sink is closed");
        callback = callback_;
        ++inFlight_;
    }

    auto finish = [this] {
        std::lock_guard lock(mutex_);
        if (inFlight_ > 0)
            --inFlight_;
        if (inFlight_ == 0)
            idleCv_.notify_all();
    };

    try {
        auto status = callback(*prepared);
        finish();
        return status;
    } catch (const std::exception& error) {
        finish();
        std::string message("event callback threw: ");
        message.append(error.what());
        return Status::internal(std::move(message));
    } catch (...) {
        finish();
        return Status::internal("event callback threw a non-std exception");
    }
}

Status CallbackEventSink::flush()
{
    std::unique_lock lock(mutex_);
    idleCv_.wait(lock, [this] {
        return inFlight_ == 0;
    });
    return Status::ok();
}

Status CallbackEventSink::waitIdle(Duration timeout)
{
    std::unique_lock lock(mutex_);
    return waitIdleLocked(lock, timeout);
}

Status CallbackEventSink::close(Duration waitIdleTimeout)
{
    std::unique_lock lock(mutex_);
    closed_ = true;
    return waitIdleLocked(lock, waitIdleTimeout);
}

bool CallbackEventSink::isClosed() const noexcept
{
    std::lock_guard lock(mutex_);
    return closed_;
}

Status CallbackEventSink::waitIdleLocked(std::unique_lock<std::mutex>& lock, Duration timeout)
{
    if (inFlight_ == 0)
        return Status::ok();

    if (timeout <= Duration::zero())
        return Status::deadlineExceeded("event sink did not become idle before timeout");

    if (timeout == Duration::max()) {
        idleCv_.wait(lock, [this] {
            return inFlight_ == 0;
        });
        return Status::ok();
    }

    const auto idle = idleCv_.wait_for(lock, timeout, [this] {
        return inFlight_ == 0;
    });
    if (!idle)
        return Status::deadlineExceeded("event sink did not become idle before timeout");

    return Status::ok();
}

} // namespace lgc
