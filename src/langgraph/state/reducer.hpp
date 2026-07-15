#pragma once

#include "foundation/serialization/state_codec.hpp"
#include "foundation/status/result.hpp"
#include "langgraph/state/state_update.hpp"

#include <functional>
#include <map>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lgc {

/// Field-level state merge behavior.
enum class ReducerKind {
    Overwrite,
    Append,
    AddMessages,
    MergeObject,
};

[[nodiscard]] std::string_view reducerKindName(ReducerKind kind) noexcept;

using ReducerFunction = std::function<Result<nlohmann::json>(
    const nlohmann::json& left,
    const nlohmann::json& right)>;

class ReducerRegistry final {
public:
    ReducerRegistry() = default;

    /// Configure merge behavior for a state field.
    ReducerRegistry& set(std::string field, ReducerKind kind);
    /// Configure custom merge behavior for a state field. The reducer receives
    /// the current field value as left, or null when absent, plus update as right.
    ReducerRegistry& set(std::string field, ReducerFunction reducer);
    [[nodiscard]] ReducerKind reducerFor(std::string_view field) const;
    [[nodiscard]] const ReducerFunction* customReducerFor(std::string_view field) const;
    [[nodiscard]] const std::map<std::string, ReducerKind>& reducers() const noexcept;

private:
    std::map<std::string, ReducerKind> reducers_;
    std::map<std::string, ReducerFunction> customReducers_;
};

[[nodiscard]] Result<State> applyStateUpdate(
    const State& state,
    const StateUpdate& update,
    const ReducerRegistry& reducers = {});

} // namespace lgc
