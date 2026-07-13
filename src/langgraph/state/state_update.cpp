#include "langgraph/state/state_update.hpp"

#include <utility>

namespace lc {

nlohmann::json overwriteToJson(Overwrite overwrite)
{
    return nlohmann::json {
        { "type", kOverwriteType },
        { "value", std::move(overwrite.value_) },
    };
}

bool isOverwriteJson(const nlohmann::json& value) noexcept
{
    return value.is_object()
        && value.contains("type")
        && value.at("type").is_string()
        && value.at("type").get_ref<const std::string&>() == kOverwriteType;
}

Result<Overwrite> overwriteFromJson(const nlohmann::json& value)
{
    if (!isOverwriteJson(value))
        return Status::invalidArgument("overwrite update must declare type __overwrite__");
    if (!value.contains("value"))
        return Status::invalidArgument("overwrite update value is required");
    return Overwrite { .value_ = value.at("value") };
}

StateUpdate::StateUpdate() = default;

StateUpdate::StateUpdate(nlohmann::json value)
    : value_(std::move(value))
{
}

Result<StateUpdate> StateUpdate::fromJson(std::string_view jsonText)
{
    return fromJson(jsonText, JsonDecodeLimits {});
}

Result<StateUpdate> StateUpdate::fromJson(
    std::string_view jsonText,
    const JsonDecodeLimits& limits)
{
    auto parsed = parseJsonWithLimits(jsonText, "state update", limits);
    if (!parsed.isOk())
        return parsed.status();
    return fromJsonValue(*parsed, limits);
}

Result<StateUpdate> StateUpdate::fromJsonValue(const nlohmann::json& value)
{
    return fromJsonValue(value, JsonDecodeLimits {});
}

Result<StateUpdate> StateUpdate::fromJsonValue(
    const nlohmann::json& value,
    const JsonDecodeLimits& limits)
{
    if (!value.is_object())
        return Status::invalidArgument("state update must be a JSON object");
    if (auto status = detail::validateJsonValueLimits(value, "state update", limits); !status.isOk())
        return status;
    return StateUpdate(value);
}

StateUpdate StateUpdate::empty()
{
    return StateUpdate {};
}

bool StateUpdate::isEmpty() const noexcept
{
    return value_.empty();
}

const nlohmann::json& StateUpdate::values() const noexcept
{
    return value_;
}

Result<State> StateUpdate::toState() const
{
    return State::fromJsonValue(value_);
}

} // namespace lc
