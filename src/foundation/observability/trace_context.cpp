#include "foundation/observability/tracing.hpp"

#include "foundation/crypto/crypto.hpp"
#include "foundation/observability/tracing_common.hh"

#include <span>
#include <string>
#include <utility>

namespace lc::tracing_detail {

[[nodiscard]] Result<std::string> nextTraceHex(
    const TraceOptions& options,
    std::size_t bytes)
{
    if (bytes == 0)
        return std::string();

    Bytes data(bytes);
    auto writable = std::as_writable_bytes(std::span(data.data(), data.size()));
    auto filled = options.randomSource_->fill(writable);
    if (!filled.isOk())
        return filled.status();

    auto out = toHex(data);
    if (allZero(out))
        out.back() = '1';
    return out;
}

[[nodiscard]] Result<TraceContext> makeRootContext(const TraceOptions& options)
{
    auto traceId = nextTraceHex(options, 16);
    if (!traceId.isOk())
        return traceId.status();
    auto spanId = nextTraceHex(options, 8);
    if (!spanId.isOk())
        return spanId.status();

    TraceContext context {
        .traceId_ = std::move(*traceId),
        .spanId_ = std::move(*spanId),
        .traceFlags_ = std::string(kDefaultTraceFlags),
    };
    if (auto status = validateTraceContext(context); !status.isOk())
        return status;
    return context;
}

[[nodiscard]] Result<TraceContext> makeChildContext(
    const TraceContext& parent,
    const TraceOptions& options)
{
    if (auto status = validateTraceContext(parent); !status.isOk())
        return status;

    auto spanId = nextTraceHex(options, 8);
    if (!spanId.isOk())
        return spanId.status();

    TraceContext context {
        .traceId_ = parent.traceId_,
        .spanId_ = std::move(*spanId),
        .parentSpanId_ = parent.spanId_,
        .traceFlags_ = parent.traceFlags_.empty() ? std::string(kDefaultTraceFlags) : parent.traceFlags_,
        .traceState_ = parent.traceState_,
        .baggage_ = parent.baggage_,
    };
    if (auto status = validateTraceContext(context); !status.isOk())
        return status;
    return context;
}

} // namespace lc::tracing_detail

namespace lc {
namespace {
using tracing_detail::allZero;
using tracing_detail::isLowerHex;
using tracing_detail::kDefaultTraceFlags;
using tracing_detail::makeChildContext;
using tracing_detail::makeRootContext;
using tracing_detail::normalizeOptions;
using tracing_detail::validateBaggage;
using tracing_detail::validateTraceState;
}

bool TraceContext::isValid() const noexcept
{
    const auto flags = traceFlags_.empty() ? std::string_view(kDefaultTraceFlags) : std::string_view(traceFlags_);
    return traceId_.size() == 32
        && isLowerHex(traceId_)
        && !allZero(traceId_)
        && spanId_.size() == 16
        && isLowerHex(spanId_)
        && !allZero(spanId_)
        && (parentSpanId_.empty()
            || (parentSpanId_.size() == 16 && isLowerHex(parentSpanId_) && !allZero(parentSpanId_)))
        && flags.size() == 2
        && isLowerHex(flags);
}

bool TraceContext::sampled() const noexcept
{
    const auto flags = traceFlags_.empty() ? std::string_view(kDefaultTraceFlags) : std::string_view(traceFlags_);
    if (flags.size() != 2 || !isLowerHex(flags))
        return false;
    const auto last = flags[1];
    const auto value = last <= '9' ? last - '0' : last - 'a' + 10;
    return (value & 0x01) != 0;
}

Result<std::string> formatTraceParent(const TraceContext& context)
{
    if (auto status = validateTraceContext(context); !status.isOk())
        return status;

    std::string out;
    out.reserve(55);
    out.append("00-");
    out.append(context.traceId_);
    out.push_back('-');
    out.append(context.spanId_);
    out.push_back('-');
    out.append(context.traceFlags_.empty() ? std::string(kDefaultTraceFlags) : context.traceFlags_);
    return out;
}

Result<std::string> formatBaggage(const TraceContext& context)
{
    if (auto status = validateBaggage(context.baggage_, TraceLimits {}); !status.isOk())
        return status;
    return context.baggage_;
}

Result<TraceContext> parseTraceParent(
    std::string_view traceparent,
    std::string_view tracestate,
    std::string_view baggage)
{
    if (traceparent.size() != 55)
        return Status::invalidArgument("traceparent must be 55 characters");
    if (traceparent.substr(0, 3) != "00-"
        || traceparent[35] != '-'
        || traceparent[52] != '-') {
        return Status::invalidArgument("traceparent has invalid format");
    }

    TraceContext context {
        .traceId_ = std::string(traceparent.substr(3, 32)),
        .spanId_ = std::string(traceparent.substr(36, 16)),
        .traceFlags_ = std::string(traceparent.substr(53, 2)),
        .traceState_ = std::string(tracestate),
        .baggage_ = std::string(baggage),
        .remoteParent_ = true,
    };
    if (auto status = validateTraceContext(context); !status.isOk())
        return status;
    if (auto status = validateTraceState(context.traceState_, TraceLimits {}); !status.isOk())
        return status;
    if (auto status = validateBaggage(context.baggage_, TraceLimits {}); !status.isOk())
        return status;
    return context;
}

Result<TraceContext> makeRootContext()
{
    auto options = normalizeOptions(TraceOptions {});
    return makeRootContext(options);
}

Result<TraceContext> makeChildContext(const TraceContext& parent)
{
    auto options = normalizeOptions(TraceOptions {});
    if (!parent.isValid())
        return Status::invalidArgument("parent trace context is invalid");
    return makeChildContext(parent, options);
}

} // namespace lc
