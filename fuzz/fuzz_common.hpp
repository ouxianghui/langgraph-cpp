#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lc::fuzz {

inline constexpr std::size_t kMaxInputBytes = 64 * 1024;

[[nodiscard]] inline std::string inputToString(const std::uint8_t* data, std::size_t size)
{
    const auto boundedSize = std::min(size, kMaxInputBytes);
    return std::string(reinterpret_cast<const char*>(data), boundedSize);
}

[[nodiscard]] inline std::vector<std::string> splitInput(
    std::string_view input,
    std::size_t maxParts)
{
    std::vector<std::string> parts;
    if (maxParts == 0U)
        return parts;
    parts.reserve(maxParts);

    std::size_t begin = 0;
    while (parts.size() + 1U < maxParts) {
        const auto separator = input.find('\0', begin);
        if (separator == std::string_view::npos)
            break;
        parts.emplace_back(input.substr(begin, separator - begin));
        begin = separator + 1U;
    }
    parts.emplace_back(input.substr(begin));

    while (parts.size() < maxParts)
        parts.emplace_back();
    return parts;
}

[[nodiscard]] inline nlohmann::json parseJsonOrDiscard(std::string_view text)
{
    return nlohmann::json::parse(text, nullptr, false);
}

} // namespace lc::fuzz
