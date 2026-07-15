#include "foundation/json/json_schema.hpp"

#include "foundation/json/json_schema_common.hh"

#include <string>
#include <utility>

namespace lgc {
using namespace json_schema_detail;

std::string ValidationError::toString() const
{
    std::string out(displayPointer(path_));
    if (!schemaPath_.empty()) {
        out.append(" (schema ");
        out.append(displayPointer(schemaPath_));
        out.push_back(')');
    }
    out.append(": ");
    out.append(message_);
    return out;
}

bool ValidationResult::isValid() const noexcept
{
    return errors_.empty();
}

bool ValidationResult::stopped() const noexcept
{
    return stopped_;
}

const std::vector<ValidationError>& ValidationResult::errors() const noexcept
{
    return errors_;
}

Status ValidationResult::status() const
{
    if (isValid())
        return Status::ok();
    return Status::invalidArgument(errors_.front().toString());
}

void ValidationResult::addError(std::string path, std::string message)
{
    addError(std::move(path), {}, std::move(message));
}

void ValidationResult::addError(std::string path, std::string schemaPath, std::string message)
{
    errors_.push_back(ValidationError {
        .path_ = std::move(path),
        .schemaPath_ = std::move(schemaPath),
        .message_ = std::move(message),
    });
}

void ValidationResult::stop() noexcept
{
    stopped_ = true;
}

std::string jsonSchemaTypeName(JsonSchemaType type)
{
    switch (type) {
    case JsonSchemaType::Any:
        return "any";
    case JsonSchemaType::Null:
        return "null";
    case JsonSchemaType::Boolean:
        return "boolean";
    case JsonSchemaType::Integer:
        return "integer";
    case JsonSchemaType::Number:
        return "number";
    case JsonSchemaType::String:
        return "string";
    case JsonSchemaType::Object:
        return "object";
    case JsonSchemaType::Array:
        return "array";
    }
    return "any";
}

} // namespace lgc
