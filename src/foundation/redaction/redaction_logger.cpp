#include "foundation/redaction/redactor.hpp"

#include <stdexcept>
#include <utility>

namespace lc {

RedactionLogger::RedactionLogger(std::shared_ptr<ILogger> inner, Redactor redactor)
    : inner_(std::move(inner))
    , redactor_(std::move(redactor))
{
    if (!inner_)
        throw std::invalid_argument("RedactionLogger requires an inner logger");
}

void RedactionLogger::log(const LogRecord& record) noexcept
{
    try {
        auto copy = record;
        copy.tag_ = redactor_.redact(copy.tag_);
        copy.message_ = redactor_.redact(copy.message_);
        copy.file_ = redactor_.redact(copy.file_);
        copy.traceId_ = redactor_.redact(copy.traceId_);
        copy.spanId_ = redactor_.redact(copy.spanId_);
        copy.runId_ = redactor_.redact(copy.runId_);
        copy.threadId_ = redactor_.redact(copy.threadId_);
        copy.nodeId_ = redactor_.redact(copy.nodeId_);
        for (auto& [key, value] : copy.fields_) {
            if (redactor_.sensitiveKey(key))
                value = redactor_.config().replacement_;
            else
                value = redactor_.redact(value);
        }
        inner_->log(copy);
    } catch (...) {
    }
}

Status RedactionLogger::flush()
{
    try {
        return inner_->flush();
    } catch (...) {
        return Status::unknown("redaction logger flush failed");
    }
}

Status RedactionLogger::close()
{
    try {
        return inner_->close();
    } catch (...) {
        return Status::unknown("redaction logger close failed");
    }
}

bool RedactionLogger::isClosed() const noexcept
{
    return inner_->isClosed();
}

} // namespace lc
