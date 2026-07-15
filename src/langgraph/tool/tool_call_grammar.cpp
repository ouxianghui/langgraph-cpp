#include "langgraph/tool/tool_call_grammar.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace lgc {
namespace {

using nlohmann::json;

[[nodiscard]] std::string gbnfLiteral(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\':
            out.append("\\\\");
            break;
        case '"':
            out.append("\\\"");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (ch < 0x20U) {
                constexpr char digits[] = "0123456789ABCDEF";
                out.append("\\x");
                out.push_back(digits[(ch >> 4U) & 0x0FU]);
                out.push_back(digits[ch & 0x0FU]);
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

[[nodiscard]] std::string jsonLiteral(const json& value)
{
    return gbnfLiteral(value.dump()) + " space";
}

[[nodiscard]] std::string keyLiteral(std::string_view key)
{
    return gbnfLiteral(json(std::string(key)).dump());
}

[[nodiscard]] std::string ruleName(std::string_view hint)
{
    std::string out;
    out.reserve(hint.size());
    bool previousDash = false;
    for (const unsigned char raw : hint) {
        const auto ch = static_cast<char>(std::tolower(raw));
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            out.push_back(ch);
            previousDash = false;
        } else if (!previousDash && !out.empty()) {
            out.push_back('-');
            previousDash = true;
        }
    }
    while (!out.empty() && out.back() == '-')
        out.pop_back();
    if (out.empty())
        out = "value";
    if (out.front() < 'a' || out.front() > 'z')
        out.insert(0, "r-");
    return out;
}

[[nodiscard]] std::string joinAlternatives(const std::vector<std::string>& expressions)
{
    if (expressions.empty())
        return {};
    if (expressions.size() == 1)
        return expressions.front();

    std::string out("(");
    for (std::size_t i = 0; i < expressions.size(); ++i) {
        if (i != 0)
            out.append(" | ");
        out.append(expressions[i]);
    }
    out.push_back(')');
    return out;
}

[[nodiscard]] std::string typeName(const json& schema)
{
    if (!schema.is_object() || !schema.contains("type") || !schema.at("type").is_string())
        return {};
    return schema.at("type").get<std::string>();
}

[[nodiscard]] std::set<std::string> requiredProperties(const json& schema)
{
    std::set<std::string> out;
    if (!schema.is_object() || !schema.contains("required") || !schema.at("required").is_array())
        return out;
    for (const auto& item : schema.at("required")) {
        if (item.is_string())
            out.insert(item.get<std::string>());
    }
    return out;
}

class GbnfBuilder final {
public:
    explicit GbnfBuilder(GbnfGrammarOptions options)
        : options_(options)
    {
        usedRuleNames_ = {
            "root",
            "tool-call",
            "value",
            "object",
            "array",
            "string",
            "char",
            "integer",
            "number",
            "boolean",
            "null",
            "space",
        };
    }

    [[nodiscard]] Result<std::string> buildFromSchema(const JsonSchema& schema)
    {
        auto root = expressionForSchema(schema.rawJson(), "schema");
        if (!root.isOk())
            return root.status();
        addRule("root", *root);
        addCommonRules();
        return render();
    }

    [[nodiscard]] Result<std::string> buildToolCalls(
        const std::vector<ToolSpec>& tools,
        std::size_t maxToolCalls)
    {
        if (maxToolCalls == 0)
            return Status::invalidArgument("max tool calls must be greater than zero");

        std::vector<std::string> callAlternatives;
        for (const auto& tool : tools) {
            if (!tool.enabled_)
                continue;
            auto call = toolCallExpression(tool);
            if (!call.isOk())
                return call.status();
            callAlternatives.push_back(*call);
        }
        if (callAlternatives.empty())
            return Status::invalidArgument("tool call grammar requires at least one enabled tool");

        const auto toolCallExpr = joinAlternatives(callAlternatives);
        addRule("tool-call", toolCallExpr);

        std::string calls = "\"[\" space tool-call";
        if (maxToolCalls > 1) {
            calls.append(" (\",\" space tool-call){0,");
            calls.append(std::to_string(maxToolCalls - 1));
            calls.append("}");
        }
        calls.append(" \"]\" space");

        const std::string root = "\"{\" space "
            + keyLiteral("tool_calls")
            + " space \":\" space "
            + calls
            + " \"}\" space";
        addRule("root", root);
        addCommonRules();
        return render();
    }

private:
    [[nodiscard]] Result<std::string> toolCallExpression(const ToolSpec& tool)
    {
        auto args = expressionForSchema(tool.inputSchema_.rawJson(), tool.name_ + "-arguments");
        if (!args.isOk())
            return args.status();

        const auto nameLiteral = jsonLiteral(tool.name_);
        return std::string("\"{\" space ")
            + keyLiteral("id") + " space \":\" space string"
            + " \",\" space " + keyLiteral("name") + " space \":\" space " + nameLiteral
            + " \",\" space " + keyLiteral("args") + " space \":\" space " + *args
            + " \"}\" space";
    }

    [[nodiscard]] Result<std::string> expressionForSchema(const json& schema, std::string_view hint)
    {
        if (!schema.is_object())
            return Status::invalidArgument("GBNF conversion requires JSON Schema objects");

        if (schema.contains("const"))
            return jsonLiteral(schema.at("const"));

        if (schema.contains("enum")) {
            const auto& values = schema.at("enum");
            if (!values.is_array() || values.empty())
                return Status::invalidArgument("GBNF conversion requires non-empty enum arrays");
            std::vector<std::string> alternatives;
            alternatives.reserve(values.size());
            for (const auto& value : values)
                alternatives.push_back(jsonLiteral(value));
            return joinAlternatives(alternatives);
        }

        auto type = typeName(schema);
        if (type.empty()) {
            if (schema.contains("properties"))
                type = "object";
            else if (schema.contains("items"))
                type = "array";
        }

        if (type.empty())
            return std::string("value");
        if (type == "null")
            return std::string("null");
        if (type == "boolean")
            return std::string("boolean");
        if (type == "integer")
            return std::string("integer");
        if (type == "number")
            return std::string("number");
        if (type == "string")
            return stringExpression(schema);
        if (type == "array")
            return arrayExpression(schema, hint);
        if (type == "object")
            return objectExpression(schema, hint);

        return Status::unimplemented("GBNF conversion does not support schema type: " + type);
    }

    [[nodiscard]] Result<std::string> stringExpression(const json& schema) const
    {
        const auto hasMin = schema.contains("minLength") && schema.at("minLength").is_number_unsigned();
        const auto hasMax = schema.contains("maxLength") && schema.at("maxLength").is_number_unsigned();
        if (!hasMin && !hasMax)
            return std::string("string");

        const auto minLength = hasMin ? schema.at("minLength").get<std::size_t>() : 0U;
        const auto maxLength = hasMax ? schema.at("maxLength").get<std::size_t>() : options_.maxStringLength_;
        if (minLength > maxLength)
            return Status::invalidArgument("string minLength cannot exceed maxLength");

        std::string out("\"\\\"\" char{");
        out.append(std::to_string(minLength));
        out.push_back(',');
        out.append(std::to_string(maxLength));
        out.append("} \"\\\"\" space");
        return out;
    }

    [[nodiscard]] Result<std::string> arrayExpression(const json& schema, std::string_view hint)
    {
        const json* itemSchema = nullptr;
        if (schema.contains("items")) {
            if (!schema.at("items").is_object())
                return Status::unimplemented("GBNF conversion supports object-valued items only");
            itemSchema = &schema.at("items");
        }

        auto item = itemSchema
            ? expressionForSchema(*itemSchema, std::string(hint) + "-item")
            : Result<std::string>(std::string("value"));
        if (!item.isOk())
            return item.status();

        std::size_t minItems = 0;
        if (schema.contains("minItems") && schema.at("minItems").is_number_unsigned())
            minItems = schema.at("minItems").get<std::size_t>();
        std::size_t maxItems = options_.maxArrayItems_;
        if (schema.contains("maxItems") && schema.at("maxItems").is_number_unsigned())
            maxItems = schema.at("maxItems").get<std::size_t>();
        if (minItems > maxItems)
            return Status::invalidArgument("array minItems cannot exceed maxItems");

        if (maxItems == 0)
            return std::string("\"[\" space \"]\" space");

        std::string sequence;
        if (minItems == 0) {
            sequence = "(" + *item + " (\",\" space " + *item + "){0,"
                + std::to_string(maxItems - 1) + "})?";
        } else {
            sequence = *item;
            if (minItems > 1) {
                sequence.append(" (\",\" space ");
                sequence.append(*item);
                sequence.append("){");
                sequence.append(std::to_string(minItems - 1));
                sequence.push_back('}');
            }
            if (maxItems > minItems) {
                sequence.append(" (\",\" space ");
                sequence.append(*item);
                sequence.append("){0,");
                sequence.append(std::to_string(maxItems - minItems));
                sequence.push_back('}');
            }
        }

        return std::string("\"[\" space ") + sequence + " \"]\" space";
    }

    [[nodiscard]] Result<std::string> objectExpression(const json& schema, std::string_view hint)
    {
        if (!schema.contains("properties") || !schema.at("properties").is_object())
            return std::string("object");

        const auto& properties = schema.at("properties");
        const auto required = requiredProperties(schema);

        std::vector<std::string> names;
        names.reserve(properties.size());
        for (auto it = properties.begin(); it != properties.end(); ++it)
            names.push_back(it.key());

        std::vector<std::string> optional;
        for (const auto& name : names) {
            if (!required.contains(name))
                optional.push_back(name);
        }
        if (optional.size() > options_.maxOptionalProperties_)
            return Status::resourceExhausted("too many optional properties for GBNF expansion");

        const auto rule = uniqueRuleName(hint);
        const auto subsetCount = static_cast<std::size_t>(1) << optional.size();
        std::vector<std::string> alternatives;
        alternatives.reserve(subsetCount);

        for (std::size_t mask = 0; mask < subsetCount; ++mask) {
            std::set<std::string> included = required;
            for (std::size_t i = 0; i < optional.size(); ++i) {
                if ((mask & (static_cast<std::size_t>(1) << i)) != 0U)
                    included.insert(optional[i]);
            }

            std::vector<std::string> kvs;
            for (const auto& name : names) {
                if (!included.contains(name))
                    continue;
                auto value = expressionForSchema(properties.at(name), std::string(rule) + "-" + name);
                if (!value.isOk())
                    return value.status();
                kvs.push_back(keyLiteral(name) + " space \":\" space " + *value);
            }

            std::string object = "\"{\" space";
            if (!kvs.empty()) {
                object.push_back(' ');
                object.append(kvs.front());
                for (std::size_t i = 1; i < kvs.size(); ++i) {
                    object.append(" \",\" space ");
                    object.append(kvs[i]);
                }
                object.push_back(' ');
            }
            object.append("\"}\" space");
            alternatives.push_back(std::move(object));
        }

        addRule(rule, joinAlternatives(alternatives));
        return rule;
    }

    [[nodiscard]] std::string uniqueRuleName(std::string_view hint)
    {
        auto base = ruleName(hint);
        auto name = base;
        std::size_t index = 2;
        while (usedRuleNames_.contains(name)) {
            name = base + "-" + std::to_string(index);
            ++index;
        }
        usedRuleNames_.insert(name);
        return name;
    }

    void addRule(std::string name, std::string expression)
    {
        if (!usedRuleNames_.contains(name))
            usedRuleNames_.insert(name);
        rules_.push_back({ std::move(name), std::move(expression) });
    }

    void addCommonRules()
    {
        addRule("value", "object | array | string | number | boolean | null");
        addRule("object", "\"{\" space (string \":\" space value (\",\" space string \":\" space value)*)? \"}\" space");
        addRule("array", "\"[\" space (value (\",\" space value)*)? \"]\" space");
        addRule("string", "\"\\\"\" char* \"\\\"\" space");
        addRule("char", "[^\"\\\\\\x00-\\x1F\\x7F] | \"\\\\\" ([\"\\\\/bfnrt] | \"u\" [0-9a-fA-F]{4})");
        addRule("integer", "\"-\"? (\"0\" | [1-9] [0-9]*) space");
        addRule("number", "\"-\"? (\"0\" | [1-9] [0-9]*) (\".\" [0-9]+)? ([eE] [-+]? [0-9]+)? space");
        addRule("boolean", "(\"true\" | \"false\") space");
        addRule("null", "\"null\" space");
        addRule("space", "[ \\t\\n\\r]*");
    }

    [[nodiscard]] std::string render() const
    {
        std::ostringstream out;
        for (const auto& [name, expression] : rules_)
            out << name << " ::= " << expression << '\n';
        return out.str();
    }

    GbnfGrammarOptions options_;
    std::vector<std::pair<std::string, std::string>> rules_;
    std::set<std::string> usedRuleNames_;
};

} // namespace

Result<std::string> jsonSchemaToGbnf(
    const JsonSchema& schema,
    const GbnfGrammarOptions& options)
{
    GbnfBuilder builder(options);
    return builder.buildFromSchema(schema);
}

Result<std::string> toolCallJsonGrammar(
    const std::vector<ToolSpec>& tools,
    const ToolCallGrammarOptions& options)
{
    GbnfBuilder builder(options.schemaOptions_);
    return builder.buildToolCalls(tools, options.maxToolCalls_);
}

Result<std::string> toolCallJsonGrammar(
    const ToolRegistry& registry,
    const ToolCallGrammarOptions& options)
{
    return toolCallJsonGrammar(registry.list(), options);
}

Result<BaseMessage> assistantMessageFromToolCallJson(std::string content)
{
    json value;
    try {
        value = json::parse(content);
    } catch (const json::exception& error) {
        return Status::invalidArgument(std::string("tool-call JSON is not parseable: ") + error.what());
    }

    if (!value.is_object())
        return Status::invalidArgument("tool-call JSON must be an object");
    if (!value.contains("tool_calls") || !value.at("tool_calls").is_array())
        return Status::invalidArgument("tool-call JSON requires tool_calls array");

    std::vector<ToolCall> calls;
    for (const auto& item : value.at("tool_calls")) {
        auto call = toolCallFromJson(item);
        if (!call.isOk())
            return call.status();
        calls.push_back(std::move(*call));
    }
    if (calls.empty())
        return Status::invalidArgument("tool-call JSON requires at least one call");

    std::string assistantContent;
    if (value.contains("content") && value.at("content").is_string())
        assistantContent = value.at("content").get<std::string>();

    return BaseMessage::ai(std::move(assistantContent), std::move(calls));
}

} // namespace lgc
