#include "langgraph/message/usage_metadata.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace lc {
namespace {

constexpr std::array<std::string_view, 8> kUsageMetadataKnownFields {
    "input_tokens",
    "output_tokens",
    "total_tokens",
    "details",
    "source",
    "provider",
    "model",
    "raw",
};

[[nodiscard]] bool isKnownUsageMetadataField(std::string_view field) noexcept
{
    for (const auto known : kUsageMetadataKnownFields) {
        if (field == known)
            return true;
    }
    return false;
}

[[nodiscard]] bool emptyObject(const nlohmann::json& value) noexcept
{
    return value.is_object() && value.empty();
}

[[nodiscard]] Result<std::uint64_t> unsignedIntegerFromJson(
    const nlohmann::json& value,
    std::string_view field)
{
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        return Status::invalidArgument(
            std::string(field) + " must be a non-negative integer");
    }
    if (value.is_number_integer()) {
        const auto signedValue = value.get<std::int64_t>();
        if (signedValue < 0) {
            return Status::invalidArgument(
                std::string(field) + " must be a non-negative integer");
        }
        return static_cast<std::uint64_t>(signedValue);
    }
    try {
        return value.get<std::uint64_t>();
    } catch (...) {
        return Status::resourceExhausted(
            std::string(field) + " is too large");
    }
}

[[nodiscard]] Result<std::optional<std::uint64_t>> optionalCountFromJson(
    const nlohmann::json& value,
    std::string_view field)
{
    if (!value.contains(field))
        return std::optional<std::uint64_t> {};
    auto parsed = unsignedIntegerFromJson(value.at(field), field);
    if (!parsed.isOk())
        return parsed.status();
    return std::optional<std::uint64_t> { *parsed };
}

} // namespace

std::string_view usageMetadataSourceName(UsageMetadataSource source) noexcept
{
    switch (source) {
    case UsageMetadataSource::Unknown:
        return "unknown";
    case UsageMetadataSource::Provider:
        return "provider";
    case UsageMetadataSource::Local:
        return "local";
    case UsageMetadataSource::Mixed:
        return "mixed";
    }
    return "unknown";
}

Result<UsageMetadataSource> usageMetadataSourceFromName(std::string_view name)
{
    if (name == "unknown")
        return UsageMetadataSource::Unknown;
    if (name == "provider")
        return UsageMetadataSource::Provider;
    if (name == "local")
        return UsageMetadataSource::Local;
    if (name == "mixed")
        return UsageMetadataSource::Mixed;
    return Status::invalidArgument("usage metadata source is invalid");
}

bool TokenUsage::empty() const noexcept
{
    return !inputTokens_.has_value()
        && !outputTokens_.has_value()
        && !totalTokens_.has_value()
        && emptyObject(details_);
}

Status TokenUsage::validate() const
{
    if (!details_.is_object())
        return Status::invalidArgument("token usage details must be a JSON object");
    return Status::ok();
}

bool UsageMetadata::empty() const noexcept
{
    return tokens_.empty()
        && source_ == UsageMetadataSource::Unknown
        && provider_.empty()
        && model_.empty()
        && emptyObject(raw_);
}

Status UsageMetadata::validate() const
{
    if (auto status = tokens_.validate(); !status.isOk())
        return status;
    if (!raw_.is_object())
        return Status::invalidArgument("usage metadata raw must be a JSON object");
    return Status::ok();
}

nlohmann::json tokenUsageToJson(const TokenUsage& usage)
{
    nlohmann::json value = nlohmann::json::object();
    if (usage.inputTokens_.has_value())
        value["input_tokens"] = *usage.inputTokens_;
    if (usage.outputTokens_.has_value())
        value["output_tokens"] = *usage.outputTokens_;
    if (usage.totalTokens_.has_value())
        value["total_tokens"] = *usage.totalTokens_;
    if (usage.details_.is_object() && !usage.details_.empty())
        value["details"] = usage.details_;
    return value;
}

Result<TokenUsage> tokenUsageFromJson(const nlohmann::json& value)
{
    if (value.is_null())
        return TokenUsage {};
    if (!value.is_object())
        return Status::invalidArgument("token usage must be a JSON object");

    TokenUsage usage;
    auto inputTokens = optionalCountFromJson(value, "input_tokens");
    if (!inputTokens.isOk())
        return inputTokens.status();
    usage.inputTokens_ = *inputTokens;

    auto outputTokens = optionalCountFromJson(value, "output_tokens");
    if (!outputTokens.isOk())
        return outputTokens.status();
    usage.outputTokens_ = *outputTokens;

    auto totalTokens = optionalCountFromJson(value, "total_tokens");
    if (!totalTokens.isOk())
        return totalTokens.status();
    usage.totalTokens_ = *totalTokens;

    if (value.contains("details")) {
        if (!value.at("details").is_object())
            return Status::invalidArgument("token usage details must be a JSON object");
        usage.details_ = value.at("details");
    }

    return usage;
}

nlohmann::json usageMetadataToJson(const UsageMetadata& usage)
{
    nlohmann::json value = tokenUsageToJson(usage.tokens_);
    if (usage.source_ != UsageMetadataSource::Unknown)
        value["source"] = usageMetadataSourceName(usage.source_);
    if (!usage.provider_.empty())
        value["provider"] = usage.provider_;
    if (!usage.model_.empty())
        value["model"] = usage.model_;
    if (usage.raw_.is_object() && !usage.raw_.empty())
        value["raw"] = usage.raw_;
    return value;
}

Result<UsageMetadata> usageMetadataFromJson(const nlohmann::json& value)
{
    if (value.is_null())
        return UsageMetadata {};
    if (!value.is_object())
        return Status::invalidArgument("usage metadata must be a JSON object");

    UsageMetadata usage;
    auto tokens = tokenUsageFromJson(value);
    if (!tokens.isOk())
        return tokens.status();
    usage.tokens_ = std::move(*tokens);

    if (value.contains("source")) {
        if (!value.at("source").is_string())
            return Status::invalidArgument("usage metadata source must be a string");
        auto source = usageMetadataSourceFromName(value.at("source").get<std::string>());
        if (!source.isOk())
            return source.status();
        usage.source_ = *source;
    }

    if (value.contains("provider")) {
        if (!value.at("provider").is_string())
            return Status::invalidArgument("usage metadata provider must be a string");
        usage.provider_ = value.at("provider").get<std::string>();
    }

    if (value.contains("model")) {
        if (!value.at("model").is_string())
            return Status::invalidArgument("usage metadata model must be a string");
        usage.model_ = value.at("model").get<std::string>();
    }

    if (value.contains("raw")) {
        if (!value.at("raw").is_object())
            return Status::invalidArgument("usage metadata raw must be a JSON object");
        usage.raw_ = value.at("raw");
    }

    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!isKnownUsageMetadataField(it.key()))
            usage.raw_[it.key()] = it.value();
    }

    return usage;
}

} // namespace lc
