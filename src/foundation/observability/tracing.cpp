#include "foundation/observability/tracing.hpp"

namespace lgc {

std::string_view spanStatusName(SpanStatus status) noexcept
{
    switch (status) {
    case SpanStatus::Unset:
        return "unset";
    case SpanStatus::Ok:
        return "ok";
    case SpanStatus::Error:
        return "error";
    case SpanStatus::Cancelled:
        return "cancelled";
    }
    return "unset";
}

std::string_view traceOverflowPolicyName(TraceOverflowPolicy policy) noexcept
{
    switch (policy) {
    case TraceOverflowPolicy::Reject:
        return "reject";
    case TraceOverflowPolicy::DropOldest:
        return "drop_oldest";
    case TraceOverflowPolicy::DropNewest:
        return "drop_newest";
    }
    return "unknown";
}

} // namespace lgc
