#include "foundation/redaction/redactor.hpp"

#include <stdexcept>
#include <utility>

namespace lc {

RedactionTraceSink::RedactionTraceSink(std::shared_ptr<ITraceSink> inner, Redactor redactor)
    : inner_(std::move(inner))
    , redactor_(std::move(redactor))
{
    if (!inner_)
        throw std::invalid_argument("RedactionTraceSink requires an inner sink");
}

Status RedactionTraceSink::recordSpan(SpanRecord span)
{
    return inner_->recordSpan(redactor_.redact(std::move(span)));
}

Status RedactionTraceSink::flush()
{
    return inner_->flush();
}

Status RedactionTraceSink::close()
{
    return inner_->close();
}

bool RedactionTraceSink::isClosed() const noexcept
{
    return inner_->isClosed();
}

} // namespace lc
