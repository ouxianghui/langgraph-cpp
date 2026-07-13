#pragma once

#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lc {

enum class JsonSchemaType : std::uint8_t {
    Any,
    Null,
    Boolean,
    Integer,
    Number,
    String,
    Object,
    Array,
};

struct JsonSchemaOptions {
    std::size_t maxSchemaBytes_ { 4 * 1024 * 1024 };
    std::size_t maxDepth_ { 64 };
    std::size_t maxNodes_ { 10'000 };
    std::size_t maxStringLength_ { 1'048'576 };
    bool allowUnknownKeywords_ { false };
};

struct ValidationOptions {
    std::size_t maxDepth_ { 64 };
    std::size_t maxNodes_ { 100'000 };
    std::size_t maxErrors_ { 64 };
    std::size_t maxStringLength_ { 1'048'576 };
    /// 0 means unlimited for already trusted in-memory callers.
    std::size_t maxInputBytes_ { 64 * 1024 * 1024 };
};

struct ValidationError {
    std::string path_;
    std::string schemaPath_;
    std::string message_;

    [[nodiscard]] std::string toString() const;
};

class ValidationResult final {
public:
    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    [[nodiscard]] const std::vector<ValidationError>& errors() const noexcept;
    [[nodiscard]] Status status() const;

    void addError(std::string path, std::string message);
    void addError(std::string path, std::string schemaPath, std::string message);
    void stop() noexcept;

private:
    std::vector<ValidationError> errors_;
    bool stopped_ { false };
};

/// Lightweight JSON Schema wrapper for tool parameters, structured output, and state validation.
///
/// This intentionally implements a practical subset of JSON Schema rather than a full draft
/// validator. Supported keywords: `type`, `required`, `properties`, `additionalProperties`,
/// `items`, `enum`, `const`, `minimum`, `maximum`, `multipleOf`, `minLength`, `maxLength`,
/// `pattern`, `minItems`, `maxItems`, `uniqueItems`, `minProperties`, `maxProperties`,
/// `allOf`, `anyOf`, `oneOf`, and `not`.
class JsonSchema final {
public:
    JsonSchema();

    [[nodiscard]] static Result<JsonSchema> fromJsonString(
        std::string_view schemaText,
        const JsonSchemaOptions& options = {});
    [[nodiscard]] static Result<JsonSchema> fromJson(
        const nlohmann::json& schema,
        const JsonSchemaOptions& options = {});

    [[nodiscard]] static JsonSchema any();
    [[nodiscard]] static JsonSchema null();
    [[nodiscard]] static JsonSchema boolean();
    [[nodiscard]] static JsonSchema integer();
    [[nodiscard]] static JsonSchema number();
    [[nodiscard]] static JsonSchema string();
    [[nodiscard]] static JsonSchema object();
    [[nodiscard]] static JsonSchema array();

    JsonSchema& type(JsonSchemaType type);
    JsonSchema& title(std::string title);
    JsonSchema& description(std::string description);
    JsonSchema& property(std::string name, const JsonSchema& schema, bool required = false);
    JsonSchema& required(std::vector<std::string> names);
    JsonSchema& additionalProperties(bool allowed);
    JsonSchema& items(const JsonSchema& schema);
    JsonSchema& enumValues(std::vector<nlohmann::json> values);
    JsonSchema& enumStrings(std::vector<std::string> values);
    JsonSchema& constant(nlohmann::json value);
    JsonSchema& minimum(double value);
    JsonSchema& maximum(double value);
    JsonSchema& multipleOf(double value);
    JsonSchema& minLength(std::size_t value);
    JsonSchema& maxLength(std::size_t value);
    JsonSchema& pattern(std::string regex);
    JsonSchema& minItems(std::size_t value);
    JsonSchema& maxItems(std::size_t value);
    JsonSchema& uniqueItems(bool enabled = true);
    JsonSchema& minProperties(std::size_t value);
    JsonSchema& maxProperties(std::size_t value);
    JsonSchema& allOf(std::vector<JsonSchema> schemas);
    JsonSchema& anyOf(std::vector<JsonSchema> schemas);
    JsonSchema& oneOf(std::vector<JsonSchema> schemas);
    JsonSchema& notSchema(const JsonSchema& schema);

    [[nodiscard]] const nlohmann::json& rawJson() const noexcept;
    [[nodiscard]] const JsonSchemaOptions& options() const noexcept;
    [[nodiscard]] std::string toJsonString(int indent = -1) const;

private:
    explicit JsonSchema(nlohmann::json schema, JsonSchemaOptions options = {});

    nlohmann::json schema_;
    JsonSchemaOptions options_;
};

class SchemaValidator final {
public:
    [[nodiscard]] ValidationResult validate(
        const nlohmann::json& value,
        const JsonSchema& schema,
        const ValidationOptions& options = {}) const;

    [[nodiscard]] Result<ValidationResult> validateText(
        std::string_view valueText,
        const JsonSchema& schema,
        const ValidationOptions& options = {}) const;

    [[nodiscard]] Status check(
        const nlohmann::json& value,
        const JsonSchema& schema,
        const ValidationOptions& options = {}) const;

    [[nodiscard]] Result<void> checkText(
        std::string_view valueText,
        const JsonSchema& schema,
        const ValidationOptions& options = {}) const;
};

[[nodiscard]] std::string jsonSchemaTypeName(JsonSchemaType type);

} // namespace lc
