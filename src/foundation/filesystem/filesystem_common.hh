#pragma once

#include "foundation/filesystem/filesystem.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace lgc::filesystem_detail {

namespace fs = std::filesystem;

[[nodiscard]] Result<fs::path> absoluteNormalized(const fs::path& input);
[[nodiscard]] bool pathStartsWith(const fs::path& base, const fs::path& candidate);
[[nodiscard]] bool containsPathSeparator(std::string_view value);
[[nodiscard]] Status validatePathComponent(
    std::string_view component,
    std::string_view field,
    const PathPolicy& policy);
[[nodiscard]] Status validateRelativePathPolicy(const fs::path& relativePath, const PathPolicy& policy);

} // namespace lgc::filesystem_detail
