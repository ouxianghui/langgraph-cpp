#include "foundation/observability/tracing.hpp"

#include "foundation/observability/tracing_common.hh"

#include <cctype>
#include <cstddef>
#include <limits>
#include <utility>

namespace lc::tracing_detail {

[[nodiscard]] Status validateTraceId(std::string_view value)
{
    if (value.size() != 32)
        return Status::invalidArgument("trace id must be 32 lower-hex characters");
    if (!isLowerHex(value))
        return Status::invalidArgument("trace id must be lower-hex");
    if (allZero(value))
        return Status::invalidArgument("trace id cannot be all zeros");
    return Status::ok();
}

[[nodiscard]] Status validateSpanId(std::string_view value, std::string_view label)
{
    if (value.size() != 16) {
        std::string message(label);
        message.append(" must be 16 lower-hex characters");
        return Status::invalidArgument(std::move(message));
    }
    if (!isLowerHex(value)) {
        std::string message(label);
        message.append(" must be lower-hex");
        return Status::invalidArgument(std::move(message));
    }
    if (allZero(value)) {
        std::string message(label);
        message.append(" cannot be all zeros");
        return Status::invalidArgument(std::move(message));
    }
    return Status::ok();
}

[[nodiscard]] Status validateTraceFlags(std::string_view value)
{
    if (value.size() != 2)
        return Status::invalidArgument("trace flags must be 2 lower-hex characters");
    if (!isLowerHex(value))
        return Status::invalidArgument("trace flags must be lower-hex");
    return Status::ok();
}

[[nodiscard]] Status validateTraceState(std::string_view value, const TraceLimits& limits)
{
    if (limits.maxTraceStateLength_ != 0 && value.size() > limits.maxTraceStateLength_)
        return Status::invalidArgument("trace state is too long");
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7f)
            return Status::invalidArgument("trace state contains a control character");
    }
    return Status::ok();
}

[[nodiscard]] Status validateBaggage(std::string_view value, const TraceLimits& limits)
{
    if (limits.maxBaggageLength_ != 0 && value.size() > limits.maxBaggageLength_)
        return Status::invalidArgument("baggage is too long");
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7f)
            return Status::invalidArgument("baggage contains a control character");
    }
    return Status::ok();
}

[[nodiscard]] bool validNameChar(char ch) noexcept
{
    const auto byte = static_cast<unsigned char>(ch);
    return byte >= 0x20 && byte != 0x7f;
}

[[nodiscard]] Status validateText(
    std::string_view value,
    std::string_view label,
    std::size_t maxLength,
    bool allowEmpty)
{
    if (!allowEmpty && value.empty()) {
        std::string message(label);
        message.append(" cannot be empty");
        return Status::invalidArgument(std::move(message));
    }
    if (maxLength != 0 && value.size() > maxLength) {
        std::string message(label);
        message.append(" is too long");
        return Status::invalidArgument(std::move(message));
    }
    for (const char ch : value) {
        if (!validNameChar(ch)) {
            std::string message(label);
            message.append(" contains a control character");
            return Status::invalidArgument(std::move(message));
        }
    }
    return Status::ok();
}

[[nodiscard]] Status validateAttributeValue(
    const nlohmann::json& value,
    const TraceLimits& limits,
    std::size_t depth,
    std::size_t& nodes)
{
    if (limits.maxAttributeDepth_ != 0 && depth > limits.maxAttributeDepth_)
        return Status::resourceExhausted("span attributes exceed max depth");
    if (limits.maxAttributeNodes_ != 0 && ++nodes > limits.maxAttributeNodes_)
        return Status::resourceExhausted("span attributes exceed max nodes");

    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (limits.maxAttributeStringLength_ != 0 && text.size() > limits.maxAttributeStringLength_)
            return Status::resourceExhausted("span attribute string is too long");
        return Status::ok();
    }

    if (value.is_object()) {
        if (limits.maxAttributes_ != 0 && value.size() > limits.maxAttributes_)
            return Status::resourceExhausted("span attribute object has too many entries");
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (auto status = validateText(it.key(), "span attribute key", limits.maxAttributeKeyLength_); !status.isOk())
                return status;
            if (auto status = validateAttributeValue(*it, limits, depth + 1, nodes); !status.isOk())
                return status;
        }
        return Status::ok();
    }

    if (value.is_array()) {
        for (const auto& item : value) {
            if (auto status = validateAttributeValue(item, limits, depth + 1, nodes); !status.isOk())
                return status;
        }
    }

    return Status::ok();
}

[[nodiscard]] Status validateAttributes(const nlohmann::json& value, const TraceLimits& limits)
{
    if (!value.is_object())
        return Status::invalidArgument("span attributes must be a JSON object");
    if (limits.maxAttributes_ != 0 && value.size() > limits.maxAttributes_)
        return Status::resourceExhausted("span has too many attributes");

    std::size_t nodes = 0;
    return validateAttributeValue(value, limits, 0, nodes);
}

[[nodiscard]] std::size_t jsonBytes(const nlohmann::json& value)
{
    try {
        return value.dump().size();
    } catch (...) {
        return std::numeric_limits<std::size_t>::max();
    }
}

[[nodiscard]] std::size_t approxSpanBytes(const SpanRecord& span)
{
    std::size_t bytes = 256U
        + span.context_.traceId_.size()
        + span.context_.spanId_.size()
        + span.context_.parentSpanId_.size()
        + span.context_.traceFlags_.size()
        + span.context_.traceState_.size()
        + span.context_.baggage_.size()
        + span.name_.size()
        + span.statusMessage_.size()
        + jsonBytes(span.attributes_);

    for (const auto& event : span.events_)
        bytes += 96U + event.name_.size() + jsonBytes(event.attributes_);

    return bytes;
}

} // namespace lc::tracing_detail

namespace lc {
namespace {
using tracing_detail::approxSpanBytes;
using tracing_detail::kDefaultTraceFlags;
using tracing_detail::validateAttributes;
using tracing_detail::validateBaggage;
using tracing_detail::validateSpanId;
using tracing_detail::validateText;
using tracing_detail::validateTraceFlags;
using tracing_detail::validateTraceId;
using tracing_detail::validateTraceState;
}

Status validateTraceContext(const TraceContext& context)
{
    if (auto status = validateTraceId(context.traceId_); !status.isOk())
        return status;
    if (auto status = validateSpanId(context.spanId_, "span id"); !status.isOk())
        return status;
    if (!context.parentSpanId_.empty()) {
        if (auto status = validateSpanId(context.parentSpanId_, "parent span id"); !status.isOk())
            return status;
    }
    const auto flags = context.traceFlags_.empty() ? std::string(kDefaultTraceFlags) : context.traceFlags_;
    if (auto status = validateTraceFlags(flags); !status.isOk())
        return status;
    if (auto status = validateTraceState(context.traceState_, TraceLimits {}); !status.isOk())
        return status;
    return validateBaggage(context.baggage_, TraceLimits {});
}

Status validateSpanRecord(const SpanRecord& span)
{
    return validateSpanRecord(span, TraceLimits {});
}

Status validateSpanRecord(const SpanRecord& span, const TraceLimits& limits)
{
    if (auto status = validateTraceContext(span.context_); !status.isOk())
        return status;
    if (auto status = validateTraceState(span.context_.traceState_, limits); !status.isOk())
        return status;
    if (auto status = validateBaggage(span.context_.baggage_, limits); !status.isOk())
        return status;
    if (auto status = validateText(span.name_, "span name", limits.maxNameLength_); !status.isOk())
        return status;
    if (auto status = validateAttributes(span.attributes_, limits); !status.isOk())
        return status;
    if (limits.maxEvents_ != 0 && span.events_.size() > limits.maxEvents_)
        return Status::resourceExhausted("span has too many events");
    if (auto status = validateText(span.statusMessage_, "span status message", limits.maxStatusMessageLength_, true); !status.isOk())
        return status;
    if (span.endedAt_.has_value() && *span.endedAt_ < span.startedAt_)
        return Status::invalidArgument("span ended before it started");
    if (limits.maxSpanBytes_ != 0 && approxSpanBytes(span) > limits.maxSpanBytes_)
        return Status::resourceExhausted("span exceeds max bytes");

    for (const auto& event : span.events_) {
        if (auto status = validateText(event.name_, "span event name", limits.maxNameLength_); !status.isOk())
            return status;
        if (auto status = validateAttributes(event.attributes_, limits); !status.isOk())
            return status;
    }
    return Status::ok();
}

} // namespace lc
