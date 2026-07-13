#pragma once

#include "foundation/status/result.hpp"
#include "langgraph/core/ids.hpp"

#include <string_view>

namespace lc::graph_detail {

inline constexpr char kCheckpointNamespaceSeparator = '|';
inline constexpr char kCheckpointNamespaceTaskSeparator = ':';

[[nodiscard]] inline Result<void> validateUserNodeId(std::string_view id)
{
    if (id.empty())
        return Status::invalidArgument("node id cannot be empty");
    if (id == START || id == END)
        return Status::invalidArgument("node id is reserved");
    if (id.find(kCheckpointNamespaceSeparator) != std::string_view::npos)
        return Status::invalidArgument("node id contains reserved namespace separator");
    if (id.find(kCheckpointNamespaceTaskSeparator) != std::string_view::npos)
        return Status::invalidArgument("node id contains reserved namespace task separator");
    return okResult();
}

} // namespace lc::graph_detail
