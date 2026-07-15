#include "foundation/serialization/state_codec.hpp"

#include "foundation/serialization/state_codec_common.hh"

#include <utility>

namespace lgc {
namespace {
using state_codec_detail::parseJsonObject;
}

State::State() = default;

State::State(nlohmann::json value)
    : value_(std::move(value))
{
}

Result<State> State::fromJson(std::string jsonText)
{
    return fromJson(std::move(jsonText), JsonDecodeLimits {});
}

Result<State> State::fromJson(std::string jsonText, const JsonDecodeLimits& limits)
{
    auto parsed = parseJsonObject(jsonText, "state", limits);
    if (!parsed.isOk())
        return parsed.status();

    return State(std::move(*parsed));
}

Result<State> State::fromJsonValue(const nlohmann::json& value)
{
    return fromJsonValue(value, JsonDecodeLimits {});
}

Result<State> State::fromJsonValue(const nlohmann::json& value, const JsonDecodeLimits& limits)
{
    if (!value.is_object())
        return Status::invalidArgument("state must be a JSON object");
    if (auto status = detail::validateJsonValueLimits(value, "state", limits); !status.isOk())
        return status;
    return State(value);
}

const std::string& State::json() const
{
    if (!serializedValid_) {
        serialized_ = value_.dump();
        serializedValid_ = true;
    }
    return serialized_;
}

Result<nlohmann::json> State::toJson() const
{
    return value_;
}

Result<nlohmann::json> State::toJson(const JsonDecodeLimits& limits) const
{
    // The value was validated against limits at construction; trust the invariant
    // here instead of walking the tree on every read.
    (void)limits;
    return value_;
}

} // namespace lgc
