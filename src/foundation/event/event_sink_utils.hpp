#pragma once

#include "foundation/event/i_event_sink.hpp"
#include "foundation/redaction/redactor.hpp"
#include "foundation/status/result.hpp"

#include <utility>

namespace lc::detail {

[[nodiscard]] inline const Redactor& defaultEventRedactor()
{
    static const Redactor redactor;
    return redactor;
}

[[nodiscard]] inline Result<RuntimeEvent> prepareRuntimeEvent(
    RuntimeEvent event,
    const EventSinkOptions& options)
{
    if (auto status = ensureRuntimeEventIdentity(event); !status.isOk())
        return status;

    if (options.redact_) {
        if (options.redactor_)
            event = options.redactor_->redact(std::move(event));
        else
            event = defaultEventRedactor().redact(std::move(event));
    }

    auto status = validateRuntimeEvent(event, options.limits_);
    if (!status.isOk())
        return status;

    return event;
}

} // namespace lc::detail
