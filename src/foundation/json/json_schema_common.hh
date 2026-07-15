#pragma once

#include "foundation/json/json_schema.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace lgc::json_schema_detail {

using nlohmann::json;

inline constexpr std::string_view kKeywordType = "type";
inline constexpr std::string_view kKeywordRequired = "required";
inline constexpr std::string_view kKeywordProperties = "properties";
inline constexpr std::string_view kKeywordAdditionalProperties = "additionalProperties";
inline constexpr std::string_view kKeywordItems = "items";
inline constexpr std::string_view kKeywordEnum = "enum";
inline constexpr std::string_view kKeywordConst = "const";
inline constexpr std::string_view kKeywordMinimum = "minimum";
inline constexpr std::string_view kKeywordMaximum = "maximum";
inline constexpr std::string_view kKeywordMultipleOf = "multipleOf";
inline constexpr std::string_view kKeywordMinLength = "minLength";
inline constexpr std::string_view kKeywordMaxLength = "maxLength";
inline constexpr std::string_view kKeywordPattern = "pattern";
inline constexpr std::string_view kKeywordMinItems = "minItems";
inline constexpr std::string_view kKeywordMaxItems = "maxItems";
inline constexpr std::string_view kKeywordUniqueItems = "uniqueItems";
inline constexpr std::string_view kKeywordMinProperties = "minProperties";
inline constexpr std::string_view kKeywordMaxProperties = "maxProperties";
inline constexpr std::string_view kKeywordAllOf = "allOf";
inline constexpr std::string_view kKeywordAnyOf = "anyOf";
inline constexpr std::string_view kKeywordOneOf = "oneOf";
inline constexpr std::string_view kKeywordNot = "not";
inline constexpr std::string_view kKeywordTitle = "title";
inline constexpr std::string_view kKeywordDescription = "description";
inline constexpr std::string_view kKeywordSchema = "$schema";
inline constexpr std::string_view kKeywordId = "$id";
inline constexpr std::string_view kKeywordComment = "$comment";

[[nodiscard]] inline bool supportedKeyword(std::string_view key) noexcept
{
    return key == kKeywordType
        || key == kKeywordRequired
        || key == kKeywordProperties
        || key == kKeywordAdditionalProperties
        || key == kKeywordItems
        || key == kKeywordEnum
        || key == kKeywordConst
        || key == kKeywordMinimum
        || key == kKeywordMaximum
        || key == kKeywordMultipleOf
        || key == kKeywordMinLength
        || key == kKeywordMaxLength
        || key == kKeywordPattern
        || key == kKeywordMinItems
        || key == kKeywordMaxItems
        || key == kKeywordUniqueItems
        || key == kKeywordMinProperties
        || key == kKeywordMaxProperties
        || key == kKeywordAllOf
        || key == kKeywordAnyOf
        || key == kKeywordOneOf
        || key == kKeywordNot
        || key == kKeywordTitle
        || key == kKeywordDescription
        || key == kKeywordSchema
        || key == kKeywordId
        || key == kKeywordComment;
}

[[nodiscard]] inline std::string escapeJsonPointerToken(std::string_view token)
{
    std::string out;
    out.reserve(token.size());
    for (const char ch : token) {
        if (ch == '~')
            out.append("~0");
        else if (ch == '/')
            out.append("~1");
        else
            out.push_back(ch);
    }
    return out;
}

[[nodiscard]] inline std::string appendPointer(std::string_view base, std::string_view token)
{
    std::string out(base);
    out.push_back('/');
    out.append(escapeJsonPointerToken(token));
    return out;
}

[[nodiscard]] inline std::string appendPointer(std::string_view base, std::size_t index)
{
    std::string out(base);
    out.push_back('/');
    out.append(std::to_string(index));
    return out;
}

[[nodiscard]] inline std::string displayPointer(std::string_view pointer)
{
    return pointer.empty() ? std::string("/") : std::string(pointer);
}

[[nodiscard]] inline Status schemaError(std::string_view schemaPath, std::string message)
{
    std::string out("schema ");
    out.append(displayPointer(schemaPath));
    out.append(": ");
    out.append(std::move(message));
    return Status::invalidArgument(std::move(out));
}

[[nodiscard]] inline Status checkSchemaTreeBudget(
    const json& value,
    std::string_view path,
    std::size_t depth,
    const JsonSchemaOptions& options,
    std::size_t& nodes)
{
    if (depth > options.maxDepth_)
        return schemaError(path, "schema exceeds max depth");

    ++nodes;
    if (nodes > options.maxNodes_)
        return schemaError(path, "schema exceeds max nodes");

    if (value.is_string()
        && value.get_ref<const std::string&>().size() > options.maxStringLength_) {
        return schemaError(path, "schema string exceeds max length");
    }

    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.key().size() > options.maxStringLength_)
                return schemaError(appendPointer(path, it.key()), "schema key exceeds max length");
            if (auto status = checkSchemaTreeBudget(*it, appendPointer(path, it.key()), depth + 1, options, nodes);
                !status.isOk()) {
                return status;
            }
        }
    } else if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (auto status = checkSchemaTreeBudget(value.at(i), appendPointer(path, i), depth + 1, options, nodes);
                !status.isOk()) {
                return status;
            }
        }
    }

    return Status::ok();
}

[[nodiscard]] inline std::optional<JsonSchemaType> schemaTypeFromString(std::string_view type)
{
    if (type == "null")
        return JsonSchemaType::Null;
    if (type == "boolean")
        return JsonSchemaType::Boolean;
    if (type == "integer")
        return JsonSchemaType::Integer;
    if (type == "number")
        return JsonSchemaType::Number;
    if (type == "string")
        return JsonSchemaType::String;
    if (type == "object")
        return JsonSchemaType::Object;
    if (type == "array")
        return JsonSchemaType::Array;
    return std::nullopt;
}

[[nodiscard]] inline bool matchesType(const json& value, JsonSchemaType type)
{
    switch (type) {
    case JsonSchemaType::Any:
        return true;
    case JsonSchemaType::Null:
        return value.is_null();
    case JsonSchemaType::Boolean:
        return value.is_boolean();
    case JsonSchemaType::Integer:
        return value.is_number_integer() || value.is_number_unsigned();
    case JsonSchemaType::Number:
        return value.is_number();
    case JsonSchemaType::String:
        return value.is_string();
    case JsonSchemaType::Object:
        return value.is_object();
    case JsonSchemaType::Array:
        return value.is_array();
    }
    return false;
}

[[nodiscard]] inline std::string valueTypeName(const json& value)
{
    if (value.is_null())
        return "null";
    if (value.is_boolean())
        return "boolean";
    if (value.is_number_integer() || value.is_number_unsigned())
        return "integer";
    if (value.is_number_float())
        return "number";
    if (value.is_string())
        return "string";
    if (value.is_object())
        return "object";
    if (value.is_array())
        return "array";
    return "unknown";
}

[[nodiscard]] inline std::string expectedTypesMessage(const json& typeSpec)
{
    if (typeSpec.is_string()) {
        std::string out("expected ");
        out.append(typeSpec.get_ref<const std::string&>());
        return out;
    }

    std::string out("expected one of ");
    bool first = true;
    for (const auto& item : typeSpec) {
        if (!item.is_string())
            continue;
        if (!first)
            out.append(", ");
        out.append(item.get_ref<const std::string&>());
        first = false;
    }
    return out;
}

[[nodiscard]] inline bool typeMatchesTypeSpec(const json& value, const json& typeSpec)
{
    if (typeSpec.is_string()) {
        const auto parsed = schemaTypeFromString(typeSpec.get_ref<const std::string&>());
        return parsed.has_value() && matchesType(value, *parsed);
    }

    for (const auto& item : typeSpec) {
        const auto parsed = schemaTypeFromString(item.get_ref<const std::string&>());
        if (parsed.has_value() && matchesType(value, *parsed))
            return true;
    }
    return false;
}

[[nodiscard]] inline bool containsString(const json& array, std::string_view value)
{
    if (!array.is_array())
        return false;
    return std::ranges::any_of(array, [value](const json& item) {
        return item.is_string() && item.get_ref<const std::string&>() == value;
    });
}

[[nodiscard]] inline Result<std::size_t> nonNegativeSize(const json& value, std::string_view schemaPath)
{
    if (value.is_number_unsigned()) {
        const auto raw = value.get<std::uint64_t>();
        if (raw > std::numeric_limits<std::size_t>::max())
            return schemaError(schemaPath, "value is too large");
        return static_cast<std::size_t>(raw);
    }

    if (value.is_number_integer()) {
        const auto raw = value.get<std::int64_t>();
        if (raw < 0)
            return schemaError(schemaPath, "value must be non-negative");
        return static_cast<std::size_t>(raw);
    }

    return schemaError(schemaPath, "value must be a non-negative integer");
}

[[nodiscard]] inline Result<int> compareJsonNumbers(const json& lhs, const json& rhs)
{
    if ((lhs.is_number_integer() || lhs.is_number_unsigned())
        && (rhs.is_number_integer() || rhs.is_number_unsigned())) {
        if (lhs.is_number_unsigned() && rhs.is_number_unsigned()) {
            const auto a = lhs.get<std::uint64_t>();
            const auto b = rhs.get<std::uint64_t>();
            return (a < b) ? -1 : ((a > b) ? 1 : 0);
        }

        if (lhs.is_number_integer() && rhs.is_number_integer()) {
            const auto a = lhs.get<std::int64_t>();
            const auto b = rhs.get<std::int64_t>();
            return (a < b) ? -1 : ((a > b) ? 1 : 0);
        }

        if (lhs.is_number_integer() && rhs.is_number_unsigned()) {
            const auto a = lhs.get<std::int64_t>();
            const auto b = rhs.get<std::uint64_t>();
            if (a < 0)
                return -1;
            const auto ua = static_cast<std::uint64_t>(a);
            return (ua < b) ? -1 : ((ua > b) ? 1 : 0);
        }

        const auto a = lhs.get<std::uint64_t>();
        const auto b = rhs.get<std::int64_t>();
        if (b < 0)
            return 1;
        const auto ub = static_cast<std::uint64_t>(b);
        return (a < ub) ? -1 : ((a > ub) ? 1 : 0);
    }

    const auto a = lhs.get<double>();
    const auto b = rhs.get<double>();
    if (!std::isfinite(a) || !std::isfinite(b))
        return Status::invalidArgument("number must be finite");
    return (a < b) ? -1 : ((a > b) ? 1 : 0);
}

[[nodiscard]] inline std::size_t utf8Length(std::string_view text)
{
    std::size_t count = 0;
    for (const unsigned char ch : text) {
        if ((ch & 0xC0U) != 0x80U)
            ++count;
    }
    return count;
}

[[nodiscard]] Status compileSchema(const json& schema, const JsonSchemaOptions& options);

} // namespace lgc::json_schema_detail
