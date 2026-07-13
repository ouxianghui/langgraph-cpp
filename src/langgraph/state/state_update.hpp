#pragma once

#include "foundation/serialization/json_limits.hpp"
#include "foundation/serialization/state_codec.hpp"
#include "foundation/status/result.hpp"

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lc {

inline constexpr std::string_view kOverwriteType = "__overwrite__";

struct Overwrite {
    nlohmann::json value_;

    friend bool operator==(const Overwrite&, const Overwrite&) = default;
};

[[nodiscard]] nlohmann::json overwriteToJson(Overwrite overwrite);
[[nodiscard]] Result<Overwrite> overwriteFromJson(const nlohmann::json& value);
[[nodiscard]] bool isOverwriteJson(const nlohmann::json& value) noexcept;

class StateUpdate final {
public:
    StateUpdate();

    [[nodiscard]] static Result<StateUpdate> fromJson(std::string_view jsonText);
    [[nodiscard]] static Result<StateUpdate> fromJson(
        std::string_view jsonText,
        const JsonDecodeLimits& limits);
    [[nodiscard]] static Result<StateUpdate> fromJsonValue(const nlohmann::json& value);
    [[nodiscard]] static Result<StateUpdate> fromJsonValue(
        const nlohmann::json& value,
        const JsonDecodeLimits& limits);

    [[nodiscard]] static StateUpdate empty();

    [[nodiscard]] bool isEmpty() const noexcept;
    [[nodiscard]] const nlohmann::json& values() const noexcept;
    [[nodiscard]] Result<State> toState() const;

    friend bool operator==(const StateUpdate&, const StateUpdate&) = default;

private:
    explicit StateUpdate(nlohmann::json value);

    nlohmann::json value_ { nlohmann::json::object() };
};

} // namespace lc
