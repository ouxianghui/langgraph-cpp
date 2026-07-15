#pragma once

#include "foundation/status/result.hpp"

#include <cstddef>
#include <exception>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace lgc {

struct JsonDecodeLimits {
    std::size_t maxBytes_ { 64 * 1024 * 1024 };
    std::size_t maxDepth_ { 64 };
    std::size_t maxStringBytes_ { 1024 * 1024 };
    std::size_t maxArrayItems_ { 100'000 };
    std::size_t maxObjectFields_ { 4096 };
    std::size_t maxNodes_ { 1'000'000 };
    bool rejectUnknownFields_ { true };
};

namespace detail {

[[nodiscard]] inline bool exceedsJsonLimit(std::size_t value, std::size_t limit) noexcept
{
    return limit != 0U && value > limit;
}

[[nodiscard]] inline Status validateJsonTextLimits(
    std::string_view text,
    std::string_view label,
    const JsonDecodeLimits& limits)
{
    if (exceedsJsonLimit(text.size(), limits.maxBytes_))
        return Status::resourceExhausted(std::string(label) + " JSON is too large");

    bool inString = false;
    bool escaped = false;
    std::size_t stringBytes = 0;
    std::size_t depth = 0;

    for (const char ch : text) {
        if (inString) {
            if (escaped) {
                escaped = false;
                ++stringBytes;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                ++stringBytes;
                continue;
            }
            if (ch == '"') {
                inString = false;
                continue;
            }
            ++stringBytes;
            if (exceedsJsonLimit(stringBytes, limits.maxStringBytes_))
                return Status::resourceExhausted(std::string(label) + " JSON string is too large");
            continue;
        }

        if (ch == '"') {
            inString = true;
            stringBytes = 0;
            continue;
        }

        if (ch == '{' || ch == '[') {
            ++depth;
            if (exceedsJsonLimit(depth, limits.maxDepth_))
                return Status::resourceExhausted(std::string(label) + " JSON nesting is too deep");
        } else if (ch == '}' || ch == ']') {
            if (depth > 0)
                --depth;
        }
    }

    return Status::ok();
}

[[nodiscard]] inline Status validateJsonValueLimits(
    const nlohmann::json& value,
    std::string_view label,
    const JsonDecodeLimits& limits)
{
    std::vector<const nlohmann::json*> stack;
    stack.push_back(&value);
    std::size_t nodes = 0;

    while (!stack.empty()) {
        const auto* current = stack.back();
        stack.pop_back();

        ++nodes;
        if (exceedsJsonLimit(nodes, limits.maxNodes_))
            return Status::resourceExhausted(std::string(label) + " JSON has too many nodes");

        if (current->is_string()) {
            const auto& text = current->get_ref<const std::string&>();
            if (exceedsJsonLimit(text.size(), limits.maxStringBytes_))
                return Status::resourceExhausted(std::string(label) + " JSON string is too large");
            continue;
        }

        if (current->is_array()) {
            if (exceedsJsonLimit(current->size(), limits.maxArrayItems_))
                return Status::resourceExhausted(std::string(label) + " JSON array is too large");
            for (const auto& item : *current)
                stack.push_back(&item);
            continue;
        }

        if (current->is_object()) {
            if (exceedsJsonLimit(current->size(), limits.maxObjectFields_))
                return Status::resourceExhausted(std::string(label) + " JSON object has too many fields");
            for (const auto& item : current->items())
                stack.push_back(&item.value());
        }
    }

    return Status::ok();
}

} // namespace detail

[[nodiscard]] inline Result<nlohmann::json> parseJsonWithLimits(
    std::string_view text,
    std::string_view label,
    const JsonDecodeLimits& limits = {})
{
    if (auto status = detail::validateJsonTextLimits(text, label, limits); !status.isOk())
        return status;

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(text);
    } catch (const std::exception& error) {
        std::string message("failed to parse ");
        message.append(label);
        message.append(": ");
        message.append(error.what());
        return Status::invalidArgument(std::move(message));
    }

    if (auto status = detail::validateJsonValueLimits(parsed, label, limits); !status.isOk())
        return status;
    return parsed;
}

[[nodiscard]] inline Status rejectUnknownJsonFields(
    const nlohmann::json& input,
    std::initializer_list<std::string_view> allowed,
    std::string_view label,
    const JsonDecodeLimits& limits)
{
    if (!limits.rejectUnknownFields_)
        return Status::ok();
    if (!input.is_object())
        return Status::invalidArgument(std::string(label) + " must be a JSON object");

    for (const auto& item : input.items()) {
        bool known = false;
        for (const auto field : allowed) {
            if (item.key() == field) {
                known = true;
                break;
            }
        }
        if (!known)
            return Status::invalidArgument(std::string(label) + " contains unknown field: " + item.key());
    }

    return Status::ok();
}

} // namespace lgc
