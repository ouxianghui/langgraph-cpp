#pragma once

#include <string_view>

#include <nlohmann/json.hpp>

namespace lgc::detail {

inline constexpr char kCheckpointNamespaceSeparator = '|';

[[nodiscard]] nlohmann::json namespacePathFromString(std::string_view nameSpace);

} // namespace lgc::detail
