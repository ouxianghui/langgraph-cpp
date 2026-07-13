#include "foundation/json/json_schema.hpp"

#include "foundation/json/json_schema_common.hh"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <regex>
#include <string>
#include <string_view>
#include <utility>

namespace lc::json_schema_detail {

struct ValidationContext {
    const ValidationOptions& options_;
    ValidationResult& result_;
    std::size_t nodes_ { 0 };
};

[[nodiscard]] std::optional<ValidationError> checkValueTreeBudget(
    const json& value,
    std::string_view path,
    std::size_t depth,
    const ValidationOptions& options,
    std::size_t& nodes)
{
    if (depth > options.maxDepth_) {
        return ValidationError {
            .path_ = std::string(path),
            .schemaPath_ = {},
            .message_ = "value exceeds max depth",
        };
    }

    ++nodes;
    if (nodes > options.maxNodes_) {
        return ValidationError {
            .path_ = std::string(path),
            .schemaPath_ = {},
            .message_ = "value exceeds max nodes",
        };
    }

    if (value.is_string()
        && value.get_ref<const std::string&>().size() > options.maxStringLength_) {
        return ValidationError {
            .path_ = std::string(path),
            .schemaPath_ = {},
            .message_ = "string exceeds max length",
        };
    }

    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.key().size() > options.maxStringLength_) {
                return ValidationError {
                    .path_ = appendPointer(path, it.key()),
                    .schemaPath_ = {},
                    .message_ = "object key exceeds max length",
                };
            }
            if (auto error = checkValueTreeBudget(*it, appendPointer(path, it.key()), depth + 1, options, nodes);
                error.has_value()) {
                return error;
            }
        }
    } else if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (auto error = checkValueTreeBudget(value.at(i), appendPointer(path, i), depth + 1, options, nodes);
                error.has_value()) {
                return error;
            }
        }
    }

    return std::nullopt;
}

void addValidationError(
    ValidationContext& context,
    std::string path,
    std::string schemaPath,
    std::string message)
{
    if (context.result_.stopped())
        return;

    if (context.options_.maxErrors_ == 0) {
        context.result_.addError("", "", "validation maxErrors must be greater than zero");
        context.result_.stop();
        return;
    }

    if (context.result_.errors().size() >= context.options_.maxErrors_) {
        context.result_.stop();
        return;
    }

    context.result_.addError(std::move(path), std::move(schemaPath), std::move(message));
    if (context.result_.errors().size() >= context.options_.maxErrors_)
        context.result_.stop();
}

[[nodiscard]] bool checkValidationBudget(
    ValidationContext& context,
    std::string_view path,
    std::string_view schemaPath,
    std::size_t depth)
{
    if (context.result_.stopped())
        return false;

    if (depth > context.options_.maxDepth_) {
        addValidationError(context, std::string(path), std::string(schemaPath), "value exceeds max depth");
        return false;
    }

    ++context.nodes_;
    if (context.nodes_ > context.options_.maxNodes_) {
        addValidationError(context, std::string(path), std::string(schemaPath), "value exceeds max nodes");
        return false;
    }

    return true;
}

[[nodiscard]] long double jsonNumberToLongDouble(const json& value)
{
    if (value.is_number_unsigned())
        return static_cast<long double>(value.get<std::uint64_t>());
    if (value.is_number_integer())
        return static_cast<long double>(value.get<std::int64_t>());
    return static_cast<long double>(value.get<double>());
}

[[nodiscard]] bool integerMultipleOf(const json& value, const json& multiple)
{
    if (!(value.is_number_integer() || value.is_number_unsigned())
        || !(multiple.is_number_integer() || multiple.is_number_unsigned())) {
        return false;
    }

    if (multiple.is_number_unsigned()) {
        const auto divisor = multiple.get<std::uint64_t>();
        if (divisor == 0)
            return false;
        if (value.is_number_unsigned())
            return value.get<std::uint64_t>() % divisor == 0;

        const auto raw = value.get<std::int64_t>();
        if (raw == 0)
            return true;
        if (raw < 0) {
            if (divisor > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                return false;
            const auto signedDivisor = static_cast<std::int64_t>(divisor);
            return raw % signedDivisor == 0;
        }
        return static_cast<std::uint64_t>(raw) % divisor == 0;
    }

    const auto signedDivisor = multiple.get<std::int64_t>();
    if (signedDivisor <= 0)
        return false;
    if (value.is_number_integer())
        return value.get<std::int64_t>() % signedDivisor == 0;

    const auto raw = value.get<std::uint64_t>();
    return raw % static_cast<std::uint64_t>(signedDivisor) == 0;
}

[[nodiscard]] bool numberMultipleOf(const json& value, const json& multiple)
{
    if (integerMultipleOf(value, multiple))
        return true;

    const auto number = jsonNumberToLongDouble(value);
    const auto divisor = jsonNumberToLongDouble(multiple);
    if (!std::isfinite(static_cast<double>(number)) || !std::isfinite(static_cast<double>(divisor)) || divisor <= 0.0L)
        return false;

    const auto quotient = number / divisor;
    const auto nearest = std::round(quotient);
    const auto tolerance = std::numeric_limits<long double>::epsilon()
        * 64.0L
        * std::max<long double>(1.0L, std::fabs(quotient));
    return std::fabs(quotient - nearest) <= tolerance;
}

void validateValue(
    const json& value,
    const json& schema,
    std::string_view path,
    std::string_view schemaPath,
    std::size_t depth,
    ValidationContext& context);

[[nodiscard]] bool validateSubschemaQuiet(
    const json& value,
    const json& schema,
    std::string_view path,
    std::string_view schemaPath,
    std::size_t depth,
    const ValidationOptions& options)
{
    ValidationResult branchResult;
    ValidationContext branchContext {
        .options_ = options,
        .result_ = branchResult,
    };
    validateValue(value, schema, path, schemaPath, depth, branchContext);
    return branchResult.isValid();
}

void validateObject(
    const json& value,
    const json& schema,
    std::string_view path,
    std::string_view schemaPath,
    std::size_t depth,
    ValidationContext& context)
{
    if (!value.is_object() || context.result_.stopped())
        return;

    if (schema.contains(kKeywordMinProperties)) {
        const auto minProperties = static_cast<std::size_t>(schema.at(kKeywordMinProperties).get<std::uint64_t>());
        if (value.size() < minProperties)
            addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordMinProperties), "object has fewer properties than minProperties");
    }
    if (schema.contains(kKeywordMaxProperties)) {
        const auto maxProperties = static_cast<std::size_t>(schema.at(kKeywordMaxProperties).get<std::uint64_t>());
        if (value.size() > maxProperties)
            addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordMaxProperties), "object has more properties than maxProperties");
    }

    if (schema.contains(kKeywordRequired)) {
        const auto& required = schema.at(kKeywordRequired);
        for (const auto& item : required) {
            if (context.result_.stopped())
                return;
            const auto& name = item.get_ref<const std::string&>();
            if (!value.contains(name))
                addValidationError(context, appendPointer(path, name), appendPointer(schemaPath, kKeywordRequired), "required property is missing");
        }
    }

    const json* properties = nullptr;
    if (schema.contains(kKeywordProperties)) {
        properties = &schema.at(kKeywordProperties);
        for (auto it = properties->begin(); it != properties->end(); ++it) {
            if (context.result_.stopped())
                return;
            if (value.contains(it.key())) {
                validateValue(
                    value.at(it.key()),
                    *it,
                    appendPointer(path, it.key()),
                    appendPointer(appendPointer(schemaPath, kKeywordProperties), it.key()),
                    depth + 1,
                    context);
            }
        }
    }

    if (!schema.contains(kKeywordAdditionalProperties))
        return;

    const auto& additional = schema.at(kKeywordAdditionalProperties);
    if (additional.is_boolean()) {
        if (additional.get<bool>())
            return;
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (context.result_.stopped())
                return;
            if (properties == nullptr || !properties->contains(it.key()))
                addValidationError(context, appendPointer(path, it.key()), appendPointer(schemaPath, kKeywordAdditionalProperties), "additional property is not allowed");
        }
        return;
    }

    for (auto it = value.begin(); it != value.end(); ++it) {
        if (context.result_.stopped())
            return;
        if (properties != nullptr && properties->contains(it.key()))
            continue;
        validateValue(
            *it,
            additional,
            appendPointer(path, it.key()),
            appendPointer(schemaPath, kKeywordAdditionalProperties),
            depth + 1,
            context);
    }
}

void validateArray(
    const json& value,
    const json& schema,
    std::string_view path,
    std::string_view schemaPath,
    std::size_t depth,
    ValidationContext& context)
{
    if (!value.is_array() || context.result_.stopped())
        return;

    if (schema.contains(kKeywordMinItems)) {
        const auto minItems = static_cast<std::size_t>(schema.at(kKeywordMinItems).get<std::uint64_t>());
        if (value.size() < minItems)
            addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordMinItems), "array has fewer items than minItems");
    }
    if (schema.contains(kKeywordMaxItems)) {
        const auto maxItems = static_cast<std::size_t>(schema.at(kKeywordMaxItems).get<std::uint64_t>());
        if (value.size() > maxItems)
            addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordMaxItems), "array has more items than maxItems");
    }
    if (schema.contains(kKeywordUniqueItems) && schema.at(kKeywordUniqueItems).get<bool>()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            for (std::size_t j = i + 1; j < value.size(); ++j) {
                if (value.at(i) == value.at(j)) {
                    addValidationError(context, appendPointer(path, j), appendPointer(schemaPath, kKeywordUniqueItems), "array item is not unique");
                    break;
                }
            }
            if (context.result_.stopped())
                return;
        }
    }

    if (!schema.contains(kKeywordItems))
        return;

    const auto& itemSchema = schema.at(kKeywordItems);
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (context.result_.stopped())
            return;
        validateValue(
            value.at(i),
            itemSchema,
            appendPointer(path, i),
            appendPointer(schemaPath, kKeywordItems),
            depth + 1,
            context);
    }
}

void validateString(
    const json& value,
    const json& schema,
    std::string_view path,
    std::string_view schemaPath,
    ValidationContext& context)
{
    if (!value.is_string() || context.result_.stopped())
        return;

    const auto& text = value.get_ref<const std::string&>();
    if (text.size() > context.options_.maxStringLength_)
        addValidationError(context, std::string(path), std::string(schemaPath), "string exceeds max length");

    const auto length = utf8Length(text);
    if (schema.contains(kKeywordMinLength)) {
        const auto minLength = static_cast<std::size_t>(schema.at(kKeywordMinLength).get<std::uint64_t>());
        if (length < minLength)
            addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordMinLength), "string is shorter than minLength");
    }
    if (schema.contains(kKeywordMaxLength)) {
        const auto maxLength = static_cast<std::size_t>(schema.at(kKeywordMaxLength).get<std::uint64_t>());
        if (length > maxLength)
            addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordMaxLength), "string is longer than maxLength");
    }
    if (schema.contains(kKeywordPattern)) {
        try {
            const std::regex pattern(schema.at(kKeywordPattern).get_ref<const std::string&>(), std::regex::ECMAScript);
            if (!std::regex_search(text.begin(), text.end(), pattern))
                addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordPattern), "string does not match pattern");
        } catch (const std::regex_error& error) {
            std::string message("invalid schema pattern: ");
            message.append(error.what());
            addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordPattern), std::move(message));
        }
    }
}

void validateNumber(
    const json& value,
    const json& schema,
    std::string_view path,
    std::string_view schemaPath,
    ValidationContext& context)
{
    if (!value.is_number() || context.result_.stopped())
        return;

    if (value.is_number_float() && !std::isfinite(value.get<double>())) {
        addValidationError(context, std::string(path), std::string(schemaPath), "number must be finite");
        return;
    }

    if (schema.contains(kKeywordMinimum)) {
        auto cmp = compareJsonNumbers(value, schema.at(kKeywordMinimum));
        if (!cmp.isOk()) {
            addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordMinimum), cmp.status().message());
        } else if (*cmp < 0) {
            addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordMinimum), "number is less than minimum");
        }
    }

    if (schema.contains(kKeywordMaximum)) {
        auto cmp = compareJsonNumbers(value, schema.at(kKeywordMaximum));
        if (!cmp.isOk()) {
            addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordMaximum), cmp.status().message());
        } else if (*cmp > 0) {
            addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordMaximum), "number is greater than maximum");
        }
    }

    if (schema.contains(kKeywordMultipleOf) && !numberMultipleOf(value, schema.at(kKeywordMultipleOf)))
        addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordMultipleOf), "number is not a multipleOf value");
}

void validateValue(
    const json& value,
    const json& schema,
    std::string_view path,
    std::string_view schemaPath,
    std::size_t depth,
    ValidationContext& context)
{
    if (!checkValidationBudget(context, path, schemaPath, depth))
        return;

    if (schema.contains(kKeywordType) && !typeMatchesTypeSpec(value, schema.at(kKeywordType))) {
        std::string message = expectedTypesMessage(schema.at(kKeywordType));
        message.append(", got ");
        message.append(valueTypeName(value));
        addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordType), std::move(message));
        return;
    }

    if (schema.contains(kKeywordEnum)) {
        const auto& allowed = schema.at(kKeywordEnum);
        if (std::ranges::none_of(allowed, [&value](const json& item) { return item == value; }))
            addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordEnum), "value is not in enum");
    }

    if (schema.contains(kKeywordConst) && value != schema.at(kKeywordConst))
        addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordConst), "value does not match const");

    if (schema.contains(kKeywordAllOf)) {
        const auto& schemas = schema.at(kKeywordAllOf);
        for (std::size_t i = 0; i < schemas.size(); ++i) {
            if (context.result_.stopped())
                return;
            validateValue(
                value,
                schemas.at(i),
                path,
                appendPointer(appendPointer(schemaPath, kKeywordAllOf), i),
                depth,
                context);
        }
    }

    if (schema.contains(kKeywordAnyOf)) {
        const auto& schemas = schema.at(kKeywordAnyOf);
        bool matched = false;
        for (std::size_t i = 0; i < schemas.size(); ++i) {
            if (validateSubschemaQuiet(
                    value,
                    schemas.at(i),
                    path,
                    appendPointer(appendPointer(schemaPath, kKeywordAnyOf), i),
                    depth,
                    context.options_)) {
                matched = true;
                break;
            }
        }
        if (!matched)
            addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordAnyOf), "value does not match anyOf schemas");
    }

    if (schema.contains(kKeywordOneOf)) {
        const auto& schemas = schema.at(kKeywordOneOf);
        std::size_t matches = 0;
        for (std::size_t i = 0; i < schemas.size(); ++i) {
            if (validateSubschemaQuiet(
                    value,
                    schemas.at(i),
                    path,
                    appendPointer(appendPointer(schemaPath, kKeywordOneOf), i),
                    depth,
                    context.options_)) {
                ++matches;
            }
        }
        if (matches != 1)
            addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordOneOf), "value must match exactly one oneOf schema");
    }

    if (schema.contains(kKeywordNot)
        && validateSubschemaQuiet(
            value,
            schema.at(kKeywordNot),
            path,
            appendPointer(schemaPath, kKeywordNot),
            depth,
            context.options_)) {
        addValidationError(context, std::string(path), appendPointer(schemaPath, kKeywordNot), "value matches forbidden schema");
    }

    validateObject(value, schema, path, schemaPath, depth, context);
    validateArray(value, schema, path, schemaPath, depth, context);
    validateString(value, schema, path, schemaPath, context);
    validateNumber(value, schema, path, schemaPath, context);
}


} // namespace lc::json_schema_detail

namespace lc {
using namespace json_schema_detail;

ValidationResult SchemaValidator::validate(
    const json& value,
    const JsonSchema& schema,
    const ValidationOptions& options) const
{
    ValidationResult result;
    if (options.maxErrors_ == 0) {
        result.addError("", "", "validation maxErrors must be greater than zero");
        result.stop();
        return result;
    }

    auto schemaStatus = compileSchema(schema.rawJson(), schema.options());
    if (!schemaStatus.isOk()) {
        result.addError("", "", schemaStatus.message());
        return result;
    }

    std::size_t valueNodes = 0;
    if (auto error = checkValueTreeBudget(value, "", 0, options, valueNodes); error.has_value()) {
        result.addError(std::move(error->path_), std::move(error->schemaPath_), std::move(error->message_));
        result.stop();
        return result;
    }

    ValidationContext context {
        .options_ = options,
        .result_ = result,
    };
    validateValue(value, schema.rawJson(), "", "", 0, context);
    return result;
}

Result<ValidationResult> SchemaValidator::validateText(
    std::string_view valueText,
    const JsonSchema& schema,
    const ValidationOptions& options) const
{
    if (options.maxInputBytes_ != 0 && valueText.size() > options.maxInputBytes_)
        return Status::resourceExhausted("json value text exceeds max input bytes");

    try {
        return validate(json::parse(valueText), schema, options);
    } catch (const std::exception& error) {
        std::string message("failed to parse json value: ");
        message.append(error.what());
        return Status::invalidArgument(std::move(message));
    }
}

Status SchemaValidator::check(
    const json& value,
    const JsonSchema& schema,
    const ValidationOptions& options) const
{
    return validate(value, schema, options).status();
}

Result<void> SchemaValidator::checkText(
    std::string_view valueText,
    const JsonSchema& schema,
    const ValidationOptions& options) const
{
    auto result = validateText(valueText, schema, options);
    if (!result.isOk())
        return result.status();
    const auto status = result->status();
    if (!status.isOk())
        return status;
    return okResult();
}

} // namespace lc
