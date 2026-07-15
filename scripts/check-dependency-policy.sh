#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

failures=0

report_failure() {
    printf 'ERROR: %s\n' "$1" >&2
    failures=$((failures + 1))
}

require_file_contains() {
    local file="$1"
    local pattern="$2"
    local message="$3"

    if ! rg -q "$pattern" "$file"; then
        report_failure "${message}"
    fi
}

require_no_matches() {
    local path="$1"
    local pattern="$2"
    local message="$3"
    local tmp

    tmp="$(mktemp)"
    if rg -n "$pattern" "$path" >"${tmp}"; then
        report_failure "${message}"
        sed 's/^/  /' "${tmp}" >&2
    fi
    rm -f "${tmp}"
}

require_no_matches \
    "src/foundation" \
    '^\s*#\s*include\s*[<"][^>"]*langgraph/' \
    "src/foundation must not include src/langgraph headers."

require_no_matches \
    "src/foundation" \
    '^\s*#\s*include\s*[<"]core/' \
    "src/foundation must not include src/core (lgc::core) headers."

require_no_matches \
    "src/core" \
    '^\s*#\s*include\s*[<"][^>"]*langgraph/' \
    "src/core must not include src/langgraph headers."

# Assembly includes look like #include "core/...". langgraph/core/ids.hpp uses
# #include "langgraph/core/..." and is intentionally allowed.
require_no_matches \
    "src/langgraph" \
    '^\s*#\s*include\s*[<"]core/' \
    "src/langgraph must not include src/core (lgc::core) assembly headers."

require_file_contains \
    "CMakeLists.txt" \
    'target_link_libraries\(core PUBLIC lgc::foundation\)' \
    "CMake target 'core' must link PUBLIC lgc::foundation."

require_no_matches \
    "CMakeLists.txt" \
    'target_link_libraries\(langgraph[^)]*lgc::core' \
    "CMake target 'langgraph' must not link lgc::core."

require_no_matches \
    "include/langgraph_cpp/langgraph.hpp" \
    '^\s*#\s*include\s*[<"][^>"]*\.hh[>"]' \
    "The public aggregate header must not include internal .hh headers."

require_no_matches \
    "include/langgraph_cpp/langgraph.hpp" \
    '^\s*#\s*include\s*[<"][^>"]*third_party/' \
    "The public aggregate header must not include third_party paths directly."

require_no_matches \
    "include/langgraph_cpp/langgraph.hpp" \
    '^\s*#\s*include\s*[<"]core/' \
    "The public aggregate header must not include lgc::core assembly headers."

require_file_contains \
    "CMakeLists.txt" \
    '^option\(LANGGRAPH_CPP_WITH_NETWORK\b' \
    "LANGGRAPH_CPP_WITH_NETWORK option is required for optional network support."

require_file_contains \
    "CMakeLists.txt" \
    '^option\(LANGGRAPH_CPP_WITH_SQLITE\b' \
    "LANGGRAPH_CPP_WITH_SQLITE option is required for optional SQLite support."

require_file_contains \
    "CMakeLists.txt" \
    '^option\(LANGGRAPH_CPP_WITH_CRYPTO\b' \
    "LANGGRAPH_CPP_WITH_CRYPTO option is required for optional crypto support."

require_file_contains \
    "CMakeLists.txt" \
    '^option\(LANGGRAPH_CPP_WITH_GZIP\b' \
    "LANGGRAPH_CPP_WITH_GZIP option is required for optional gzip support."

require_file_contains \
    "CMakeLists.txt" \
    '^option\(LANGGRAPH_CPP_WITH_SPDLOG\b' \
    "LANGGRAPH_CPP_WITH_SPDLOG option is required for optional spdlog support."

require_file_contains \
    "CMakeLists.txt" \
    '^option\(LANGGRAPH_CPP_WITH_LLAMA_CPP\b' \
    "LANGGRAPH_CPP_WITH_LLAMA_CPP option is required for optional llama.cpp support."

require_file_contains \
    "CMakeLists.txt" \
    '^if\(LANGGRAPH_CPP_WITH_LLAMA_CPP\)' \
    "llama.cpp integration must remain behind LANGGRAPH_CPP_WITH_LLAMA_CPP."

if (( failures > 0 )); then
    printf '\nDependency policy check failed with %d issue(s).\n' "${failures}" >&2
    exit 1
fi

printf 'Dependency policy check passed.\n'
