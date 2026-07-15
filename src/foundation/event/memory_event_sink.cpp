#include "foundation/event/memory_event_sink.hpp"

#include "foundation/event/event_sink_utils.hpp"

#include <utility>

namespace lgc {

MemoryEventSink::MemoryEventSink(MemoryEventSinkOptions options)
    : options_(std::move(options))
{
}

Status MemoryEventSink::publish(RuntimeEvent event)
{
    auto prepared = detail::prepareRuntimeEvent(std::move(event), options_.event_);
    if (!prepared.isOk())
        return prepared.status();

    std::lock_guard lock(mutex_);
    if (closed_)
        return Status::unavailable("event sink is closed");

    if (options_.capacity_ != 0U && events_.size() >= options_.capacity_) {
        switch (options_.overflowPolicy_) {
        case EventOverflowPolicy::Reject:
            return Status::resourceExhausted("memory event sink is full");
        case EventOverflowPolicy::DropOldest:
            events_.erase(events_.begin());
            break;
        case EventOverflowPolicy::DropNewest:
            return Status::ok();
        }
    }

    events_.push_back(std::move(*prepared));
    return Status::ok();
}

Status MemoryEventSink::flush()
{
    return Status::ok();
}

Status MemoryEventSink::waitIdle(Duration)
{
    return Status::ok();
}

Status MemoryEventSink::close(Duration)
{
    std::lock_guard lock(mutex_);
    closed_ = true;
    return Status::ok();
}

bool MemoryEventSink::isClosed() const noexcept
{
    std::lock_guard lock(mutex_);
    return closed_;
}

std::vector<RuntimeEvent> MemoryEventSink::events() const
{
    std::lock_guard lock(mutex_);
    return events_;
}

std::size_t MemoryEventSink::size() const noexcept
{
    std::lock_guard lock(mutex_);
    return events_.size();
}

void MemoryEventSink::clear()
{
    std::lock_guard lock(mutex_);
    events_.clear();
}

} // namespace lgc
