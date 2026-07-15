#pragma once

#include "foundation/observability/tracing.hpp"

#include <string_view>

namespace lgc::tracing_detail {

inline constexpr std::string_view kDefaultTraceFlags = "01";

[[nodiscard]] TraceOptions normalizeOptions(TraceOptions options);
[[nodiscard]] bool isLowerHex(char ch) noexcept;
[[nodiscard]] bool isLowerHex(std::string_view value) noexcept;
[[nodiscard]] bool allZero(std::string_view value) noexcept;
[[nodiscard]] Status validateTraceId(std::string_view value);
[[nodiscard]] Status validateSpanId(std::string_view value, std::string_view label);
[[nodiscard]] Status validateTraceFlags(std::string_view value);
[[nodiscard]] Status validateTraceState(std::string_view value, const TraceLimits& limits);
[[nodiscard]] Status validateBaggage(std::string_view value, const TraceLimits& limits);
[[nodiscard]] Status validateText(
    std::string_view value,
    std::string_view label,
    std::size_t maxLength,
    bool allowEmpty = false);
[[nodiscard]] Status validateAttributes(const nlohmann::json& value, const TraceLimits& limits);
[[nodiscard]] nlohmann::json redactAttributeObject(
    std::string_view key,
    nlohmann::json value,
    const TraceOptions& options);
[[nodiscard]] SpanRecord redactSpan(SpanRecord span, const TraceOptions& options);
[[nodiscard]] Result<TraceContext> makeRootContext(const TraceOptions& options);
[[nodiscard]] Result<TraceContext> makeChildContext(const TraceContext& parent, const TraceOptions& options);

} // namespace lgc::tracing_detail
