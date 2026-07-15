#pragma once

#include "langgraph/graph/stream.hpp"

#include <string_view>
#include <vector>

namespace lgc::detail {

[[nodiscard]] std::vector<StreamPart> projectEvent(
    const RuntimeEvent& event,
    const RunProjectionOptions& options,
    std::string_view rootNamespace);

} // namespace lgc::detail
