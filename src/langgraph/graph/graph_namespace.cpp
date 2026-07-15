#include "langgraph/graph/graph_namespace.hh"

#include <string>

namespace lgc::detail {

nlohmann::json namespacePathFromString(std::string_view nameSpace)
{
    nlohmann::json path = nlohmann::json::array();
    std::size_t begin = 0;
    while (begin < nameSpace.size()) {
        const auto end = nameSpace.find(kCheckpointNamespaceSeparator, begin);
        const auto segment = end == std::string_view::npos
            ? nameSpace.substr(begin)
            : nameSpace.substr(begin, end - begin);
        if (!segment.empty())
            path.push_back(std::string(segment));
        if (end == std::string_view::npos)
            break;
        begin = end + 1U;
    }
    return path;
}

} // namespace lgc::detail
