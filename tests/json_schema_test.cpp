#include "foundation/json/json_schema.hpp"
#include "foundation/status/status.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

void verifyBasicValidation()
{
    using nlohmann::json;

    auto location = lgc::JsonSchema::string().minLength(1);
    auto unit = lgc::JsonSchema::string().enumStrings({ "celsius", "fahrenheit" });
    auto days = lgc::JsonSchema::integer().minimum(1).maximum(10);

    auto toolSchema = lgc::JsonSchema::object()
                          .property("location", location, true)
                          .property("unit", unit)
                          .property("days", days)
                          .additionalProperties(false);

    const lgc::SchemaValidator validator;

    const auto valid = validator.validate(json {
        { "location", "Shanghai" },
        { "unit", "celsius" },
        { "days", 3 },
    },
        toolSchema);
    assert(valid.isValid());
    assert(valid.status().isOk());

    const auto invalid = validator.validate(json {
        { "unit", "kelvin" },
        { "days", 11 },
        { "extra", true },
    },
        toolSchema);
    assert(!invalid.isValid());
    assert(invalid.errors().size() == 4);
    assert(invalid.status().code() == lgc::StatusCode::InvalidArgument);
    assert(invalid.errors().front().path_ == "/location");
    assert(invalid.errors().front().schemaPath_ == "/required");

    auto stateSchema = lgc::JsonSchema::object()
                           .property("messages", lgc::JsonSchema::array().items(lgc::JsonSchema::string()), true)
                           .property("step", lgc::JsonSchema::integer().minimum(0), true)
                           .additionalProperties(true);

    auto parsedValid = validator.validateText(
        R"({"messages":["hello","world"],"step":2,"scratchpad":{"ok":true}})",
        stateSchema);
    assert(parsedValid.isOk());
    assert(parsedValid->isValid());

    auto parsedInvalid = validator.checkText(
        R"({"messages":["hello",1],"step":-1})",
        stateSchema);
    assert(!parsedInvalid.isOk());
    assert(parsedInvalid.status().code() == lgc::StatusCode::InvalidArgument);

    auto badJson = validator.validateText("{", stateSchema);
    assert(!badJson.isOk());
    assert(badJson.status().code() == lgc::StatusCode::InvalidArgument);

    auto schemaFromText = lgc::JsonSchema::fromJsonString(R"({
        "type": "object",
        "required": ["answer"],
        "properties": {
            "answer": {"type": "string", "minLength": 1},
            "confidence": {"type": "number", "minimum": 0, "maximum": 1}
        },
        "additionalProperties": false
    })");
    assert(schemaFromText.isOk());

    auto structuredStatus = validator.checkText(
        R"({"answer":"done","confidence":0.8})",
        *schemaFromText);
    assert(structuredStatus.isOk());

    auto structuredInvalid = validator.checkText(
        R"({"answer":"","confidence":2})",
        *schemaFromText);
    assert(!structuredInvalid.isOk());

    auto invalidSchema = lgc::JsonSchema::fromJsonString(R"([])");
    assert(!invalidSchema.isOk());

    assert(lgc::jsonSchemaTypeName(lgc::JsonSchemaType::String) == "string");
    assert(toolSchema.toJsonString().find("location") != std::string::npos);
}

void verifySchemaCompileFailures()
{
    using nlohmann::json;

    assert(lgc::JsonSchema::fromJson(json {
               { "type", "bool" },
           })
               .status()
               .code()
        == lgc::StatusCode::InvalidArgument);

    assert(lgc::JsonSchema::fromJson(json {
               { "type", json::array({ "string", "string" }) },
           })
               .status()
               .code()
        == lgc::StatusCode::InvalidArgument);

    assert(lgc::JsonSchema::fromJson(json {
               { "required", json::array({ "a", "a" }) },
           })
               .status()
               .code()
        == lgc::StatusCode::InvalidArgument);

    assert(lgc::JsonSchema::fromJson(json {
               { "properties", json::array() },
           })
               .status()
               .code()
        == lgc::StatusCode::InvalidArgument);

    assert(lgc::JsonSchema::fromJson(json {
               { "enum", json::array({ "a", "a" }) },
           })
               .status()
               .code()
        == lgc::StatusCode::InvalidArgument);

    assert(lgc::JsonSchema::fromJson(json {
               { "minimum", 2 },
               { "maximum", 1 },
           })
               .status()
               .code()
        == lgc::StatusCode::InvalidArgument);

    assert(lgc::JsonSchema::fromJson(json {
               { "minLength", 3 },
               { "maxLength", 2 },
           })
               .status()
               .code()
        == lgc::StatusCode::InvalidArgument);

    assert(lgc::JsonSchema::fromJson(json {
               { "unknownKeyword", true },
           })
               .status()
               .code()
        == lgc::StatusCode::InvalidArgument);

    auto relaxed = lgc::JsonSchema::fromJson(
        json {
            { "unknownKeyword", true },
        },
        lgc::JsonSchemaOptions {
            .allowUnknownKeywords_ = true,
        });
    assert(relaxed.isOk());

    auto tooLargeSchemaText = lgc::JsonSchema::fromJsonString(
        R"({"type":"string"})",
        lgc::JsonSchemaOptions {
            .maxSchemaBytes_ = 4,
        });
    assert(!tooLargeSchemaText.isOk());
    assert(tooLargeSchemaText.status().code() == lgc::StatusCode::ResourceExhausted);

    auto tooLongSchemaString = lgc::JsonSchema::fromJson(
        json {
            { "title", "abcdef" },
        },
        lgc::JsonSchemaOptions {
            .maxStringLength_ = 3,
        });
    assert(!tooLongSchemaString.isOk());
    assert(tooLongSchemaString.status().code() == lgc::StatusCode::InvalidArgument);

    assert(lgc::JsonSchema::fromJson(json {
               { "minLength", "bad" },
           })
               .status()
               .code()
        == lgc::StatusCode::InvalidArgument);

    assert(lgc::JsonSchema::fromJson(json {
               { "pattern", "[" },
           })
               .status()
               .code()
        == lgc::StatusCode::InvalidArgument);

    assert(lgc::JsonSchema::fromJson(json {
               { "multipleOf", 0 },
           })
               .status()
               .code()
        == lgc::StatusCode::InvalidArgument);

    assert(lgc::JsonSchema::fromJson(json {
               { "uniqueItems", "yes" },
           })
               .status()
               .code()
        == lgc::StatusCode::InvalidArgument);

    assert(lgc::JsonSchema::fromJson(json {
               { "anyOf", json::array() },
           })
               .status()
               .code()
        == lgc::StatusCode::InvalidArgument);

    assert(lgc::JsonSchema::fromJson(json {
               { "not", true },
           })
               .status()
               .code()
        == lgc::StatusCode::InvalidArgument);
}

void verifyLimits()
{
    using nlohmann::json;

    auto schema = lgc::JsonSchema::fromJson(json {
        { "type", "object" },
        { "properties", {
                            { "items", {
                                           { "type", "array" },
                                           { "items", {
                                                          { "type", "object" },
                                                          { "properties", {
                                                                            { "name", { { "type", "string" } } },
                                                                        } },
                                                      } },
                                       } },
        } },
    },
        lgc::JsonSchemaOptions {
            .maxDepth_ = 2,
        });
    assert(!schema.isOk());
    assert(schema.status().code() == lgc::StatusCode::InvalidArgument);

    auto shallowSchema = lgc::JsonSchema::object()
                             .property("a", lgc::JsonSchema::string(), true)
                             .property("b", lgc::JsonSchema::string(), true)
                             .property("c", lgc::JsonSchema::string(), true)
                             .additionalProperties(false);

    const lgc::SchemaValidator validator;
    auto limited = validator.validate(
        json {
            { "x", 1 },
            { "y", 2 },
            { "z", 3 },
        },
        shallowSchema,
        lgc::ValidationOptions {
            .maxErrors_ = 2,
        });
    assert(!limited.isValid());
    assert(limited.stopped());
    assert(limited.errors().size() == 2);

    auto tooLargeText = validator.validateText(
        R"({"a":"xxxxxxxx"})",
        lgc::JsonSchema::object().property("a", lgc::JsonSchema::string(), true),
        lgc::ValidationOptions {
            .maxInputBytes_ = 4,
        });
    assert(!tooLargeText.isOk());
    assert(tooLargeText.status().code() == lgc::StatusCode::ResourceExhausted);

    auto tooLongString = validator.validate(
        json {
            { "a", "abcdef" },
        },
        lgc::JsonSchema::object().property("a", lgc::JsonSchema::string(), true),
        lgc::ValidationOptions {
            .maxStringLength_ = 3,
        });
    assert(!tooLongString.isValid());
    assert(tooLongString.errors().front().path_ == "/a");

    auto tooManyNodes = validator.validate(
        json::array({ 1, 2, 3 }),
        lgc::JsonSchema::array().items(lgc::JsonSchema::integer()),
        lgc::ValidationOptions {
            .maxNodes_ = 2,
        });
    assert(!tooManyNodes.isValid());
    assert(tooManyNodes.errors().front().message_.find("max nodes") != std::string::npos);

    auto invalidOptions = validator.validate(
        json {
            { "a", "ok" },
        },
        lgc::JsonSchema::object().property("a", lgc::JsonSchema::string(), true),
        lgc::ValidationOptions {
            .maxErrors_ = 0,
        });
    assert(!invalidOptions.isValid());
    assert(invalidOptions.stopped());
    assert(invalidOptions.errors().front().message_.find("maxErrors") != std::string::npos);

    auto depthSchema = lgc::JsonSchema::object()
                           .property("a",
                               lgc::JsonSchema::object().property("b",
                                   lgc::JsonSchema::object().property("c", lgc::JsonSchema::boolean(), true),
                                   true),
                               true);

    auto maxDepth = validator.validate(
        json {
            { "a", { { "b", { { "c", true } } } } },
        },
        depthSchema,
        lgc::ValidationOptions {
            .maxDepth_ = 1,
        });
    assert(!maxDepth.isValid());
    assert(maxDepth.errors().front().message_.find("max depth") != std::string::npos);

    auto anySchema = lgc::JsonSchema::any();
    auto deepValue = json {
        { "ignored", json::array({ json { { "nested", "value" } } }) },
    };
    auto fullTreeDepth = validator.validate(
        deepValue,
        anySchema,
        lgc::ValidationOptions {
            .maxDepth_ = 1,
        });
    assert(!fullTreeDepth.isValid());
    assert(fullTreeDepth.errors().front().message_.find("max depth") != std::string::npos);

    auto fullTreeString = validator.validate(
        json {
            { "ignored", "abcdef" },
        },
        anySchema,
        lgc::ValidationOptions {
            .maxStringLength_ = 3,
        });
    assert(!fullTreeString.isValid());
    assert(fullTreeString.errors().front().path_ == "/ignored");
}

void verifyJsonPointerPathsAndNumbers()
{
    using nlohmann::json;

    auto schema = lgc::JsonSchema::fromJson(json {
        { "type", "object" },
        { "properties", {
                            { "a.b/c~d", {
                                             { "type", "array" },
                                             { "items", {
                                                            { "type", "integer" },
                                                            { "minimum", std::numeric_limits<std::int64_t>::max() },
                                                        } },
                                         } },
                        } },
    });
    assert(schema.isOk());

    const lgc::SchemaValidator validator;
    auto result = validator.validate(
        json {
            { "a.b/c~d", json::array({ std::numeric_limits<std::int64_t>::max(), std::int64_t { 1 } }) },
        },
        *schema);
    assert(!result.isValid());
    assert(result.errors().front().path_ == "/a.b~1c~0d/1");
    assert(result.errors().front().schemaPath_ == "/properties/a.b~1c~0d/items/minimum");

    auto precise = lgc::JsonSchema::fromJson(json {
        { "type", "integer" },
        { "minimum", std::numeric_limits<std::uint64_t>::max() - 1U },
        { "maximum", std::numeric_limits<std::uint64_t>::max() },
    });
    assert(precise.isOk());
    auto preciseResult = validator.check(json(std::numeric_limits<std::uint64_t>::max()), *precise);
    assert(preciseResult.isOk());
}

void verifyAdditionalKeywords()
{
    using nlohmann::json;

    const lgc::SchemaValidator validator;

    auto stringSchema = lgc::JsonSchema::string()
                            .pattern(R"(^[a-z]+-[0-9]+$)")
                            .constant("abc-123");
    assert(validator.check("abc-123", stringSchema).isOk());
    auto badString = validator.validate("ABC-123", stringSchema);
    assert(!badString.isValid());
    assert(badString.errors().front().schemaPath_ == "/const");

    auto arraySchema = lgc::JsonSchema::array()
                           .items(lgc::JsonSchema::integer().multipleOf(2))
                           .uniqueItems()
                           .minItems(2)
                           .maxItems(3);
    assert(validator.check(json::array({ 2, 4 }), arraySchema).isOk());
    auto badArray = validator.validate(json::array({ 2, 3, 2, 8 }), arraySchema);
    assert(!badArray.isValid());
    assert(!badArray.errors().empty());

    auto objectSchema = lgc::JsonSchema::object()
                            .minProperties(1)
                            .maxProperties(2)
                            .additionalProperties(true);
    assert(validator.check(json { { "a", 1 } }, objectSchema).isOk());
    auto tooManyProperties = validator.validate(json { { "a", 1 }, { "b", 2 }, { "c", 3 } }, objectSchema);
    assert(!tooManyProperties.isValid());
    assert(tooManyProperties.errors().front().schemaPath_ == "/maxProperties");

    auto allOfSchema = lgc::JsonSchema::fromJson(json {
        { "allOf", json::array({
                       json { { "type", "integer" } },
                       json { { "minimum", 10 } },
                   }) },
    });
    assert(allOfSchema.isOk());
    assert(validator.check(12, *allOfSchema).isOk());
    assert(!validator.check(8, *allOfSchema).isOk());

    auto anyOfSchema = lgc::JsonSchema::fromJson(json {
        { "anyOf", json::array({
                       json { { "type", "string" } },
                       json { { "type", "integer" } },
                   }) },
    });
    assert(anyOfSchema.isOk());
    assert(validator.check("ok", *anyOfSchema).isOk());
    assert(validator.check("ok", *anyOfSchema, lgc::ValidationOptions { .maxDepth_ = 0 }).isOk());
    assert(validator.check(42, *anyOfSchema).isOk());
    assert(!validator.check(true, *anyOfSchema).isOk());

    auto oneOfSchema = lgc::JsonSchema::fromJson(json {
        { "oneOf", json::array({
                       json { { "type", "integer" } },
                       json { { "minimum", 0 } },
                   }) },
    });
    assert(oneOfSchema.isOk());
    assert(validator.check(-1, *oneOfSchema).isOk());
    assert(!validator.check(1, *oneOfSchema).isOk());

    auto notSchema = lgc::JsonSchema::any().notSchema(lgc::JsonSchema::string());
    assert(validator.check(1, notSchema).isOk());
    assert(!validator.check("no", notSchema).isOk());
}

void verifyConcurrentValidation()
{
    using nlohmann::json;

    auto schema = lgc::JsonSchema::object()
                      .property("id", lgc::JsonSchema::string().pattern(R"(^item-[0-9]+$)"), true)
                      .property("score", lgc::JsonSchema::number().minimum(0).maximum(1), true)
                      .additionalProperties(false);
    const lgc::SchemaValidator validator;

    std::vector<std::thread> workers;
    workers.reserve(8);
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back([&validator, &schema, i] {
            for (int n = 0; n < 200; ++n) {
                auto status = validator.check(
                    json {
                        { "id", "item-" + std::to_string(i * 200 + n) },
                        { "score", 0.5 },
                    },
                    schema);
                assert(status.isOk());
            }
        });
    }

    for (auto& worker : workers)
        worker.join();
}

} // namespace

int main()
{
    verifyBasicValidation();
    verifySchemaCompileFailures();
    verifyLimits();
    verifyJsonPointerPathsAndNumbers();
    verifyAdditionalKeywords();
    verifyConcurrentValidation();
    return 0;
}
