#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lc {

using NodeId = std::string;
using ThreadId = std::string;
using CheckpointId = std::string;
using StepId = std::uint64_t;

inline constexpr std::string_view START = "__start__";
inline constexpr std::string_view END = "__end__";

} // namespace lc
