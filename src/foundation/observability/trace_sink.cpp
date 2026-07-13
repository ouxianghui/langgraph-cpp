#include "foundation/observability/tracing.hpp"

#include "foundation/observability/tracing_common.hh"

#include <utility>

namespace lc {
namespace {
using tracing_detail::normalizeOptions;
using tracing_detail::redactSpan;
}

InMemoryTraceSink::InMemoryTraceSink(TraceOptions options)
    : options_(normalizeOptions(std::move(options)))
{
}

Status InMemoryTraceSink::recordSpan(SpanRecord span)
{
    span = redactSpan(std::move(span), options_);
    if (auto status = validateSpanRecord(span, options_.limits_); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    if (closed_)
        return Status::unavailable("trace sink is closed");

    if (options_.limits_.maxSpans_ != 0U && spans_.size() >= options_.limits_.maxSpans_) {
        switch (options_.overflowPolicy_) {
        case TraceOverflowPolicy::Reject:
            return Status::resourceExhausted("in-memory trace sink is full");
        case TraceOverflowPolicy::DropNewest:
            return Status::ok();
        case TraceOverflowPolicy::DropOldest:
            spans_.erase(spans_.begin());
            break;
        }
    }

    spans_.push_back(std::move(span));
    return Status::ok();
}

Status InMemoryTraceSink::flush()
{
    std::lock_guard lock(mutex_);
    if (closed_)
        return Status::unavailable("trace sink is closed");
    return Status::ok();
}

Status InMemoryTraceSink::close()
{
    std::lock_guard lock(mutex_);
    closed_ = true;
    return Status::ok();
}

bool InMemoryTraceSink::isClosed() const noexcept
{
    std::lock_guard lock(mutex_);
    return closed_;
}

std::vector<SpanRecord> InMemoryTraceSink::spans() const
{
    std::lock_guard lock(mutex_);
    return spans_;
}

std::size_t InMemoryTraceSink::size() const noexcept
{
    std::lock_guard lock(mutex_);
    return spans_.size();
}

void InMemoryTraceSink::clear()
{
    std::lock_guard lock(mutex_);
    spans_.clear();
}

} // namespace lc
