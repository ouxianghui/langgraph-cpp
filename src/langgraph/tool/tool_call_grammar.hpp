#pragma once

#include "foundation/json/json_schema.hpp"
#include "foundation/status/result.hpp"
#include "langgraph/message/message.hpp"
#include "langgraph/tool/tool.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace lgc {

struct GbnfGrammarOptions {
    /// Upper bound used when a JSON Schema array has no maxItems.
    std::size_t maxArrayItems_ { 8 };
    /// Upper bound used for bounded string rules derived from maxLength.
    std::size_t maxStringLength_ { 256 };
    /// Maximum optional object properties expanded into grammar alternatives.
    std::size_t maxOptionalProperties_ { 8 };
};

struct ToolCallGrammarOptions {
    GbnfGrammarOptions schemaOptions_;
    /// Maximum tool calls allowed in one assistant response.
    std::size_t maxToolCalls_ { 1 };
};

/// Convert the supported JSON Schema subset into a llama.cpp-compatible GBNF grammar.
[[nodiscard]] Result<std::string> jsonSchemaToGbnf(
    const JsonSchema& schema,
    const GbnfGrammarOptions& options = {});

/// Build a constrained JSON grammar for assistant tool-call output.
///
/// The generated JSON shape is:
/// {"tool_calls":[{"id":"...","name":"tool_name","args":{...}}]}
[[nodiscard]] Result<std::string> toolCallJsonGrammar(
    const std::vector<ToolSpec>& tools,
    const ToolCallGrammarOptions& options = {});

[[nodiscard]] Result<std::string> toolCallJsonGrammar(
    const ToolRegistry& registry,
    const ToolCallGrammarOptions& options = {});

/// Parse a constrained tool-call JSON response into an assistant message.
[[nodiscard]] Result<BaseMessage> assistantMessageFromToolCallJson(std::string content);

} // namespace lgc
