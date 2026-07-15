#pragma once

#include <filesystem>

namespace lgc {

/// Parent directory of the running executable (best-effort: macOS / Linux / `argv[0]` / cwd).
[[nodiscard]] std::filesystem::path executableDirectory(char* argv0);

/// Directory containing `app.<env>.json` next to the binary unless **`BOT_CONFIG_DIR`** is set
/// (then that path is used as-is, relative or absolute).
[[nodiscard]] std::filesystem::path resolveConfigDirectory(char* argv0);

} // namespace lgc
