#include "foundation/json/json_schema.hpp"

#include "foundation/json/json_schema_common.hh"

#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lgc {
using namespace json_schema_detail;
namespace {

[[nodiscard]] json schemaWithType(JsonSchemaType type)
{
    json schema = json::object();
    if (type != JsonSchemaType::Any)
        schema["type"] = jsonSchemaTypeName(type);
    return schema;
}


} // namespace

JsonSchema::JsonSchema()
    : schema_(json::object())
{
}

JsonSchema::JsonSchema(json schema, JsonSchemaOptions options)
    : schema_(std::move(schema))
    , options_(std::move(options))
{
}

Result<JsonSchema> JsonSchema::fromJsonString(
    std::string_view schemaText,
    const JsonSchemaOptions& options)
{
    if (options.maxSchemaBytes_ != 0 && schemaText.size() > options.maxSchemaBytes_)
        return Status::resourceExhausted("json schema text exceeds max schema bytes");

    try {
        return fromJson(json::parse(schemaText), options);
    } catch (const std::exception& error) {
        std::string message("failed to parse json schema: ");
        message.append(error.what());
        return Status::invalidArgument(std::move(message));
    }
}

Result<JsonSchema> JsonSchema::fromJson(const json& schema, const JsonSchemaOptions& options)
{
    auto status = compileSchema(schema, options);
    if (!status.isOk())
        return status;
    return JsonSchema(schema, options);
}

JsonSchema JsonSchema::any()
{
    return JsonSchema(schemaWithType(JsonSchemaType::Any));
}

JsonSchema JsonSchema::null()
{
    return JsonSchema(schemaWithType(JsonSchemaType::Null));
}

JsonSchema JsonSchema::boolean()
{
    return JsonSchema(schemaWithType(JsonSchemaType::Boolean));
}

JsonSchema JsonSchema::integer()
{
    return JsonSchema(schemaWithType(JsonSchemaType::Integer));
}

JsonSchema JsonSchema::number()
{
    return JsonSchema(schemaWithType(JsonSchemaType::Number));
}

JsonSchema JsonSchema::string()
{
    return JsonSchema(schemaWithType(JsonSchemaType::String));
}

JsonSchema JsonSchema::object()
{
    return JsonSchema(schemaWithType(JsonSchemaType::Object));
}

JsonSchema JsonSchema::array()
{
    return JsonSchema(schemaWithType(JsonSchemaType::Array));
}

JsonSchema& JsonSchema::type(JsonSchemaType type)
{
    if (type == JsonSchemaType::Any)
        schema_.erase("type");
    else
        schema_["type"] = jsonSchemaTypeName(type);
    return *this;
}

JsonSchema& JsonSchema::title(std::string title)
{
    schema_["title"] = std::move(title);
    return *this;
}

JsonSchema& JsonSchema::description(std::string description)
{
    schema_["description"] = std::move(description);
    return *this;
}

JsonSchema& JsonSchema::property(std::string name, const JsonSchema& schema, bool required)
{
    schema_["properties"][name] = schema.rawJson();
    if (required) {
        if (!schema_.contains("required") || !schema_.at("required").is_array())
            schema_["required"] = json::array();
        if (!containsString(schema_.at("required"), name))
            schema_["required"].push_back(name);
    }
    return *this;
}

JsonSchema& JsonSchema::required(std::vector<std::string> names)
{
    schema_["required"] = std::move(names);
    return *this;
}

JsonSchema& JsonSchema::additionalProperties(bool allowed)
{
    schema_["additionalProperties"] = allowed;
    return *this;
}

JsonSchema& JsonSchema::items(const JsonSchema& schema)
{
    schema_["items"] = schema.rawJson();
    return *this;
}

JsonSchema& JsonSchema::enumValues(std::vector<json> values)
{
    schema_["enum"] = std::move(values);
    return *this;
}

JsonSchema& JsonSchema::enumStrings(std::vector<std::string> values)
{
    schema_["enum"] = std::move(values);
    return *this;
}

JsonSchema& JsonSchema::constant(json value)
{
    schema_["const"] = std::move(value);
    return *this;
}

JsonSchema& JsonSchema::minimum(double value)
{
    schema_["minimum"] = value;
    return *this;
}

JsonSchema& JsonSchema::maximum(double value)
{
    schema_["maximum"] = value;
    return *this;
}

JsonSchema& JsonSchema::multipleOf(double value)
{
    schema_["multipleOf"] = value;
    return *this;
}

JsonSchema& JsonSchema::minLength(std::size_t value)
{
    schema_["minLength"] = value;
    return *this;
}

JsonSchema& JsonSchema::maxLength(std::size_t value)
{
    schema_["maxLength"] = value;
    return *this;
}

JsonSchema& JsonSchema::pattern(std::string regex)
{
    schema_["pattern"] = std::move(regex);
    return *this;
}

JsonSchema& JsonSchema::minItems(std::size_t value)
{
    schema_["minItems"] = value;
    return *this;
}

JsonSchema& JsonSchema::maxItems(std::size_t value)
{
    schema_["maxItems"] = value;
    return *this;
}

JsonSchema& JsonSchema::uniqueItems(bool enabled)
{
    schema_["uniqueItems"] = enabled;
    return *this;
}

JsonSchema& JsonSchema::minProperties(std::size_t value)
{
    schema_["minProperties"] = value;
    return *this;
}

JsonSchema& JsonSchema::maxProperties(std::size_t value)
{
    schema_["maxProperties"] = value;
    return *this;
}

JsonSchema& JsonSchema::allOf(std::vector<JsonSchema> schemas)
{
    auto values = json::array();
    for (const auto& schema : schemas)
        values.push_back(schema.rawJson());
    schema_["allOf"] = std::move(values);
    return *this;
}

JsonSchema& JsonSchema::anyOf(std::vector<JsonSchema> schemas)
{
    auto values = json::array();
    for (const auto& schema : schemas)
        values.push_back(schema.rawJson());
    schema_["anyOf"] = std::move(values);
    return *this;
}

JsonSchema& JsonSchema::oneOf(std::vector<JsonSchema> schemas)
{
    auto values = json::array();
    for (const auto& schema : schemas)
        values.push_back(schema.rawJson());
    schema_["oneOf"] = std::move(values);
    return *this;
}

JsonSchema& JsonSchema::notSchema(const JsonSchema& schema)
{
    schema_["not"] = schema.rawJson();
    return *this;
}

const json& JsonSchema::rawJson() const noexcept
{
    return schema_;
}

const JsonSchemaOptions& JsonSchema::options() const noexcept
{
    return options_;
}

std::string JsonSchema::toJsonString(int indent) const
{
    return schema_.dump(indent);
}

} // namespace lgc
