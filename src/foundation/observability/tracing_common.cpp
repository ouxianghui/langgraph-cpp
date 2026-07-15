#include "foundation/observability/tracing_common.hh"

#include "foundation/crypto/crypto.hpp"
#include "foundation/redaction/redactor.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace lgc::tracing_detail {

[[nodiscard]] TraceOptions normalizeOptions(TraceOptions options)
{
    if (options.clock_ == nullptr)
        options.clock_ = &SteadyClock::instance();
    if (!options.randomSource_)
        options.randomSource_ = std::make_shared<OsRandomSource>();
    if (options.redact_ && !options.redactor_)
        options.redactor_ = std::make_shared<Redactor>();
    return options;
}

[[nodiscard]] bool isLowerHex(char ch) noexcept
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
}

[[nodiscard]] bool isLowerHex(std::string_view value) noexcept
{
    return std::ranges::all_of(value, [](char ch) {
        return isLowerHex(ch);
    });
}

[[nodiscard]] bool allZero(std::string_view value) noexcept
{
    return std::ranges::all_of(value, [](char ch) {
        return ch == '0';
    });
}

[[nodiscard]] nlohmann::json redactAttributeObject(
    std::string_view key,
    nlohmann::json value,
    const TraceOptions& options)
{
    if (!options.redact_ || !options.redactor_)
        return value;

    nlohmann::json wrapper = nlohmann::json::object();
    wrapper[std::string(key)] = std::move(value);
    auto redacted = options.redactor_->redact(wrapper);
    return redacted.at(std::string(key));
}

[[nodiscard]] SpanRecord redactSpan(SpanRecord span, const TraceOptions& options)
{
    if (!options.redact_ || !options.redactor_)
        return span;
    return options.redactor_->redact(std::move(span));
}

} // namespace lgc::tracing_detail
