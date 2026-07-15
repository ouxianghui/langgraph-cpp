#pragma once

#include "foundation/status/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lgc {

enum class UsageMetadataSource : std::uint8_t {
    Unknown = 0,
    Provider,
    Local,
    Mixed,
};

[[nodiscard]] std::string_view usageMetadataSourceName(UsageMetadataSource source) noexcept;
[[nodiscard]] Result<UsageMetadataSource> usageMetadataSourceFromName(std::string_view name);

struct TokenUsage {
    std::optional<std::uint64_t> inputTokens_;
    std::optional<std::uint64_t> outputTokens_;
    std::optional<std::uint64_t> totalTokens_;
    nlohmann::json details_ { nlohmann::json::object() };

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] Status validate() const;

    friend bool operator==(const TokenUsage&, const TokenUsage&) = default;
};

struct UsageMetadata {
    TokenUsage tokens_;
    UsageMetadataSource source_ { UsageMetadataSource::Unknown };
    std::string provider_;
    std::string model_;
    nlohmann::json raw_ { nlohmann::json::object() };

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] Status validate() const;

    friend bool operator==(const UsageMetadata&, const UsageMetadata&) = default;
};

[[nodiscard]] nlohmann::json tokenUsageToJson(const TokenUsage& usage);
[[nodiscard]] Result<TokenUsage> tokenUsageFromJson(const nlohmann::json& value);

[[nodiscard]] nlohmann::json usageMetadataToJson(const UsageMetadata& usage);
[[nodiscard]] Result<UsageMetadata> usageMetadataFromJson(const nlohmann::json& value);

} // namespace lgc
