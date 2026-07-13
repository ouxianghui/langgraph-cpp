#include "foundation/redaction/redactor.hpp"

#include <stdexcept>
#include <utility>

namespace lc {

RedactionEventSink::RedactionEventSink(std::shared_ptr<IEventSink> inner, Redactor redactor)
    : inner_(std::move(inner))
    , redactor_(std::move(redactor))
{
    if (!inner_)
        throw std::invalid_argument("RedactionEventSink requires an inner sink");
}

Status RedactionEventSink::publish(RuntimeEvent event)
{
    return inner_->publish(redactor_.redact(std::move(event)));
}

Status RedactionEventSink::flush()
{
    return inner_->flush();
}

Status RedactionEventSink::waitIdle(Duration timeout)
{
    return inner_->waitIdle(timeout);
}

Status RedactionEventSink::close(Duration waitIdleTimeout)
{
    return inner_->close(waitIdleTimeout);
}

bool RedactionEventSink::isClosed() const noexcept
{
    return inner_->isClosed();
}

} // namespace lc
