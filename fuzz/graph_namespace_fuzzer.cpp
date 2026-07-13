#include "fuzz_common.hpp"
#include "langgraph/graph/graph_namespace.hh"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    try {
        const auto input = lc::fuzz::inputToString(data, size);
        (void)lc::detail::namespacePathFromString(input);
    } catch (...) {
    }
    return 0;
}
