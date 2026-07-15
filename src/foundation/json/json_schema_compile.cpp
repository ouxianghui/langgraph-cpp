#include "foundation/json/json_schema_common.hh"

#include <algorithm>
#include <cmath>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace lgc::json_schema_detail {

struct SchemaCompileContext {
    const JsonSchemaOptions& options_;
    std::size_t nodes_ { 0 };
};

[[nodiscard]] Status compileSchema(
    const json& schema,
    std::string_view schemaPath,
    std::size_t depth,
    SchemaCompileContext& context);

[[nodiscard]] Status checkSchemaBudget(
    const json& value,
    std::string_view schemaPath,
    std::size_t depth,
    SchemaCompileContext& context)
{
    if (depth > context.options_.maxDepth_)
        return schemaError(schemaPath, "schema exceeds max depth");

    ++context.nodes_;
    if (context.nodes_ > context.options_.maxNodes_)
        return schemaError(schemaPath, "schema exceeds max nodes");

    if (value.is_string()
        && value.get_ref<const std::string&>().size() > context.options_.maxStringLength_) {
        return schemaError(schemaPath, "schema string exceeds max length");
    }

    return Status::ok();
}

[[nodiscard]] Status compileTypeKeyword(const json& type, std::string_view schemaPath)
{
    if (type.is_string()) {
        if (!schemaTypeFromString(type.get_ref<const std::string&>()))
            return schemaError(schemaPath, "unknown type");
        return Status::ok();
    }

    if (!type.is_array())
        return schemaError(schemaPath, "type must be a string or array of strings");
    if (type.empty())
        return schemaError(schemaPath, "type array cannot be empty");

    std::vector<std::string> seen;
    for (std::size_t i = 0; i < type.size(); ++i) {
        const auto itemPath = appendPointer(schemaPath, i);
        const auto& item = type.at(i);
        if (!item.is_string())
            return schemaError(itemPath, "type array entries must be strings");
        const auto& name = item.get_ref<const std::string&>();
        if (!schemaTypeFromString(name))
            return schemaError(itemPath, "unknown type");
        if (std::ranges::find(seen, name) != seen.end())
            return schemaError(itemPath, "duplicate type entry");
        seen.push_back(name);
    }

    return Status::ok();
}

[[nodiscard]] Status compileRequiredKeyword(const json& required, std::string_view schemaPath)
{
    if (!required.is_array())
        return schemaError(schemaPath, "required must be an array");

    std::vector<std::string> seen;
    for (std::size_t i = 0; i < required.size(); ++i) {
        const auto itemPath = appendPointer(schemaPath, i);
        const auto& item = required.at(i);
        if (!item.is_string())
            return schemaError(itemPath, "required entries must be strings");
        const auto& name = item.get_ref<const std::string&>();
        if (std::ranges::find(seen, name) != seen.end())
            return schemaError(itemPath, "duplicate required entry");
        seen.push_back(name);
    }

    return Status::ok();
}

[[nodiscard]] Status compileEnumKeyword(const json& values, std::string_view schemaPath)
{
    if (!values.is_array())
        return schemaError(schemaPath, "enum must be an array");
    if (values.empty())
        return schemaError(schemaPath, "enum cannot be empty");

    for (std::size_t i = 0; i < values.size(); ++i) {
        for (std::size_t j = i + 1; j < values.size(); ++j) {
            if (values.at(i) == values.at(j))
                return schemaError(appendPointer(schemaPath, j), "duplicate enum value");
        }
    }
    return Status::ok();
}

[[nodiscard]] Status compileNumericRange(const json& schema, std::string_view schemaPath)
{
    if (schema.contains(kKeywordMinimum) && !schema.at(kKeywordMinimum).is_number())
        return schemaError(appendPointer(schemaPath, kKeywordMinimum), "minimum must be a number");
    if (schema.contains(kKeywordMaximum) && !schema.at(kKeywordMaximum).is_number())
        return schemaError(appendPointer(schemaPath, kKeywordMaximum), "maximum must be a number");

    if (schema.contains(kKeywordMinimum)) {
        const auto minimum = schema.at(kKeywordMinimum);
        if (minimum.is_number_float() && !std::isfinite(minimum.get<double>()))
            return schemaError(appendPointer(schemaPath, kKeywordMinimum), "minimum must be finite");
    }
    if (schema.contains(kKeywordMaximum)) {
        const auto maximum = schema.at(kKeywordMaximum);
        if (maximum.is_number_float() && !std::isfinite(maximum.get<double>()))
            return schemaError(appendPointer(schemaPath, kKeywordMaximum), "maximum must be finite");
    }

    if (schema.contains(kKeywordMinimum) && schema.contains(kKeywordMaximum)) {
        auto cmp = compareJsonNumbers(schema.at(kKeywordMinimum), schema.at(kKeywordMaximum));
        if (!cmp.isOk())
            return schemaError(schemaPath, cmp.status().message());
        if (*cmp > 0)
            return schemaError(schemaPath, "minimum cannot be greater than maximum");
    }

    return Status::ok();
}

[[nodiscard]] Status compileMultipleOfKeyword(const json& value, std::string_view schemaPath)
{
    if (!value.is_number())
        return schemaError(schemaPath, "multipleOf must be a number");

    if (value.is_number_float()) {
        const auto raw = value.get<double>();
        if (!std::isfinite(raw) || raw <= 0.0)
            return schemaError(schemaPath, "multipleOf must be finite and greater than zero");
        return Status::ok();
    }

    if (value.is_number_integer() && value.get<std::int64_t>() <= 0)
        return schemaError(schemaPath, "multipleOf must be greater than zero");
    if (value.is_number_unsigned() && value.get<std::uint64_t>() == 0)
        return schemaError(schemaPath, "multipleOf must be greater than zero");

    return Status::ok();
}

[[nodiscard]] Status compilePatternKeyword(const json& value, std::string_view schemaPath)
{
    if (!value.is_string())
        return schemaError(schemaPath, "pattern must be a string");

    try {
        (void)std::regex(value.get_ref<const std::string&>(), std::regex::ECMAScript);
    } catch (const std::regex_error& error) {
        std::string message("pattern is not a valid ECMAScript regex: ");
        message.append(error.what());
        return schemaError(schemaPath, std::move(message));
    }

    return Status::ok();
}

[[nodiscard]] Status compileBooleanKeyword(
    const json& schema,
    std::string_view keyword,
    std::string_view schemaPath)
{
    if (!schema.contains(std::string(keyword)))
        return Status::ok();
    if (!schema.at(std::string(keyword)).is_boolean()) {
        std::string message(keyword);
        message.append(" must be a boolean");
        return schemaError(appendPointer(schemaPath, keyword), std::move(message));
    }
    return Status::ok();
}

[[nodiscard]] Status compileSizeRange(
    const json& schema,
    std::string_view minKeyword,
    std::string_view maxKeyword,
    std::string_view schemaPath)
{
    std::optional<std::size_t> minValue;
    std::optional<std::size_t> maxValue;

    if (schema.contains(std::string(minKeyword))) {
        auto parsed = nonNegativeSize(schema.at(std::string(minKeyword)), appendPointer(schemaPath, minKeyword));
        if (!parsed.isOk())
            return parsed.status();
        minValue = *parsed;
    }

    if (schema.contains(std::string(maxKeyword))) {
        auto parsed = nonNegativeSize(schema.at(std::string(maxKeyword)), appendPointer(schemaPath, maxKeyword));
        if (!parsed.isOk())
            return parsed.status();
        maxValue = *parsed;
    }

    if (minValue && maxValue && *minValue > *maxValue) {
        std::string message(minKeyword);
        message.append(" cannot be greater than ");
        message.append(maxKeyword);
        return schemaError(schemaPath, std::move(message));
    }

    return Status::ok();
}

[[nodiscard]] Status compileSchemaArrayKeyword(
    const json& schema,
    std::string_view keyword,
    std::string_view schemaPath,
    std::size_t depth,
    SchemaCompileContext& context)
{
    if (!schema.contains(std::string(keyword)))
        return Status::ok();

    const auto& schemas = schema.at(std::string(keyword));
    const auto keywordPath = appendPointer(schemaPath, keyword);
    if (!schemas.is_array())
        return schemaError(keywordPath, "keyword must be an array of schemas");
    if (schemas.empty())
        return schemaError(keywordPath, "keyword array cannot be empty");

    for (std::size_t i = 0; i < schemas.size(); ++i) {
        if (auto status = compileSchema(schemas.at(i), appendPointer(keywordPath, i), depth + 1, context);
            !status.isOk()) {
            return status;
        }
    }

    return Status::ok();
}

[[nodiscard]] Status compileSchema(
    const json& schema,
    std::string_view schemaPath,
    std::size_t depth,
    SchemaCompileContext& context)
{
    if (auto status = checkSchemaBudget(schema, schemaPath, depth, context); !status.isOk())
        return status;

    if (!schema.is_object())
        return schemaError(schemaPath, "schema must be an object");

    for (auto it = schema.begin(); it != schema.end(); ++it) {
        if (!context.options_.allowUnknownKeywords_ && !supportedKeyword(it.key()))
            return schemaError(appendPointer(schemaPath, it.key()), "unsupported keyword");
    }

    if (schema.contains(kKeywordType)) {
        if (auto status = compileTypeKeyword(schema.at(kKeywordType), appendPointer(schemaPath, kKeywordType)); !status.isOk())
            return status;
    }

    if (schema.contains(kKeywordRequired)) {
        if (auto status = compileRequiredKeyword(schema.at(kKeywordRequired), appendPointer(schemaPath, kKeywordRequired)); !status.isOk())
            return status;
    }

    if (schema.contains(kKeywordEnum)) {
        if (auto status = compileEnumKeyword(schema.at(kKeywordEnum), appendPointer(schemaPath, kKeywordEnum)); !status.isOk())
            return status;
    }

    if (schema.contains(kKeywordConst)) {
        if (schema.at(kKeywordConst).is_discarded())
            return schemaError(appendPointer(schemaPath, kKeywordConst), "const cannot be discarded json");
    }

    for (const auto keyword : { kKeywordTitle, kKeywordDescription, kKeywordSchema, kKeywordId, kKeywordComment }) {
        if (schema.contains(std::string(keyword))) {
            const auto& value = schema.at(std::string(keyword));
            if (!value.is_string())
                return schemaError(appendPointer(schemaPath, keyword), "keyword must be a string");
            if (value.get_ref<const std::string&>().size() > context.options_.maxStringLength_)
                return schemaError(appendPointer(schemaPath, keyword), "keyword string exceeds max length");
        }
    }

    if (schema.contains(kKeywordPattern)) {
        if (auto status = compilePatternKeyword(schema.at(kKeywordPattern), appendPointer(schemaPath, kKeywordPattern)); !status.isOk())
            return status;
    }

    if (schema.contains(kKeywordProperties)) {
        const auto& properties = schema.at(kKeywordProperties);
        if (!properties.is_object())
            return schemaError(appendPointer(schemaPath, kKeywordProperties), "properties must be an object");
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            const auto childPath = appendPointer(appendPointer(schemaPath, kKeywordProperties), it.key());
            if (auto status = compileSchema(*it, childPath, depth + 1, context); !status.isOk())
                return status;
        }
    }

    if (schema.contains(kKeywordAdditionalProperties)) {
        const auto& additional = schema.at(kKeywordAdditionalProperties);
        if (!additional.is_boolean() && !additional.is_object())
            return schemaError(appendPointer(schemaPath, kKeywordAdditionalProperties), "additionalProperties must be boolean or object");
        if (additional.is_object()) {
            if (auto status = compileSchema(
                    additional,
                    appendPointer(schemaPath, kKeywordAdditionalProperties),
                    depth + 1,
                    context);
                !status.isOk()) {
                return status;
            }
        }
    }

    if (schema.contains(kKeywordItems)) {
        const auto& items = schema.at(kKeywordItems);
        if (!items.is_object())
            return schemaError(appendPointer(schemaPath, kKeywordItems), "items must be an object");
        if (auto status = compileSchema(items, appendPointer(schemaPath, kKeywordItems), depth + 1, context); !status.isOk())
            return status;
    }

    if (auto status = compileNumericRange(schema, schemaPath); !status.isOk())
        return status;
    if (schema.contains(kKeywordMultipleOf)) {
        if (auto status = compileMultipleOfKeyword(schema.at(kKeywordMultipleOf), appendPointer(schemaPath, kKeywordMultipleOf));
            !status.isOk()) {
            return status;
        }
    }
    if (auto status = compileSizeRange(schema, kKeywordMinLength, kKeywordMaxLength, schemaPath); !status.isOk())
        return status;
    if (auto status = compileSizeRange(schema, kKeywordMinItems, kKeywordMaxItems, schemaPath); !status.isOk())
        return status;
    if (auto status = compileSizeRange(schema, kKeywordMinProperties, kKeywordMaxProperties, schemaPath); !status.isOk())
        return status;
    if (auto status = compileBooleanKeyword(schema, kKeywordUniqueItems, schemaPath); !status.isOk())
        return status;
    if (auto status = compileSchemaArrayKeyword(schema, kKeywordAllOf, schemaPath, depth, context); !status.isOk())
        return status;
    if (auto status = compileSchemaArrayKeyword(schema, kKeywordAnyOf, schemaPath, depth, context); !status.isOk())
        return status;
    if (auto status = compileSchemaArrayKeyword(schema, kKeywordOneOf, schemaPath, depth, context); !status.isOk())
        return status;
    if (schema.contains(kKeywordNot)) {
        const auto& notSchema = schema.at(kKeywordNot);
        if (!notSchema.is_object())
            return schemaError(appendPointer(schemaPath, kKeywordNot), "not must be a schema object");
        if (auto status = compileSchema(notSchema, appendPointer(schemaPath, kKeywordNot), depth + 1, context); !status.isOk())
            return status;
    }

    return Status::ok();
}

[[nodiscard]] Status compileSchema(const json& schema, const JsonSchemaOptions& options)
{
    std::size_t treeNodes = 0;
    if (auto status = checkSchemaTreeBudget(schema, "", 0, options, treeNodes); !status.isOk())
        return status;

    SchemaCompileContext context { .options_ = options };
    return compileSchema(schema, "", 0, context);
}

} // namespace lgc::json_schema_detail
