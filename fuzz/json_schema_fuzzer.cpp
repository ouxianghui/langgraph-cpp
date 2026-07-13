#include "fuzz_common.hpp"
#include "foundation/json/json_schema.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    try {
        const auto input = lc::fuzz::inputToString(data, size);
        const auto parts = lc::fuzz::splitInput(input, 2);

        auto schema = lc::JsonSchema::fromJsonString(
            parts[0],
            lc::JsonSchemaOptions {
                .maxSchemaBytes_ = 64 * 1024,
                .maxDepth_ = 32,
                .maxNodes_ = 2048,
                .maxStringLength_ = 8192,
                .allowUnknownKeywords_ = false,
            });
        if (!schema.isOk())
            return 0;

        lc::SchemaValidator validator;
        (void)validator.validateText(
            parts[1],
            *schema,
            lc::ValidationOptions {
                .maxDepth_ = 32,
                .maxNodes_ = 4096,
                .maxErrors_ = 16,
                .maxStringLength_ = 8192,
                .maxInputBytes_ = 64 * 1024,
            });
        const auto value = lc::fuzz::parseJsonOrDiscard(parts[1]);
        if (!value.is_discarded())
            (void)validator.check(value, *schema);
    } catch (...) {
    }
    return 0;
}
