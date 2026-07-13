#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lc {

/// Small helper for converting between `json` and model types that expose
/// nlohmann ADL hooks via `DECLARE_JSON_ADL`.
class JsonCodec final {
public:
    template <typename Model>
    static Model modelFromJson(const json& payload)
    {
        return payload.get<Model>();
    }

    template <typename Model>
    static Model modelFromJsonString(const std::string_view payload)
    {
        return json::parse(payload).get<Model>();
    }

    template <typename Model>
    static json jsonFromModel(const Model& model)
    {
        return json(model);
    }

    template <typename Model>
    static std::string jsonStringFromModel(const Model& model,
        const int indent = -1)
    {
        return jsonFromModel(model).dump(indent);
    }
};

} // namespace lc
