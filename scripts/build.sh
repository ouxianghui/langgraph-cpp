#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_MODE="${BUILD_MODE:-unix}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
BUILD_TARGET="${BUILD_TARGET:-}"
FRESH_CONFIGURE="${FRESH_CONFIGURE:-0}"

show_usage() {
  cat <<'EOF'
Usage:
  ./scripts/build.sh [Debug|Release]
  ./scripts/build.sh --mode unix --build-type Release
  ./scripts/build.sh --mode xcode --build-type Debug
  ./scripts/build.sh --mode xcode --fresh
  ./scripts/build.sh --target langgraph_cpp

Options:
  --mode <unix|xcode>          Build mode. Default: unix
  --build-type <type>          Debug or Release. Default: Debug
  --target <name>              Optional CMake target to build
  --fresh                      Reconfigure from a fresh CMake cache
  -h, --help                   Show this help

Environment overrides:
  BUILD_MODE, BUILD_TYPE, BUILD_TARGET, FRESH_CONFIGURE

Examples:
  ./scripts/build.sh
  ./scripts/build.sh Release
  ./scripts/build.sh --mode xcode
  ./scripts/build.sh --mode xcode --fresh --target langgraph_cpp
EOF
}

POSITIONAL_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      BUILD_MODE="$2"
      shift 2
      ;;
    --build-type)
      BUILD_TYPE="$2"
      shift 2
      ;;
    --target)
      BUILD_TARGET="$2"
      shift 2
      ;;
    --fresh)
      FRESH_CONFIGURE=1
      shift
      ;;
    -h|--help)
      show_usage
      exit 0
      ;;
    *)
      POSITIONAL_ARGS+=("$1")
      shift
      ;;
  esac
done

if [[ ${#POSITIONAL_ARGS[@]} -ge 1 ]]; then
  BUILD_TYPE="${POSITIONAL_ARGS[0]}"
fi

case "${BUILD_TYPE}" in
  Debug|Release) ;;
  *)
    echo "Unsupported BUILD_TYPE=${BUILD_TYPE}. Use Debug or Release."
    exit 1
    ;;
esac

case "${BUILD_MODE}" in
  unix|xcode) ;;
  *)
    echo "Unsupported BUILD_MODE=${BUILD_MODE}. Use unix or xcode."
    exit 1
    ;;
esac

if [[ "${BUILD_MODE}" == "xcode" && "$(uname -s)" != "Darwin" ]]; then
  echo "Xcode mode is only available on macOS."
  exit 1
fi

run_configure_preset() {
  local preset="$1"
  if [[ "${FRESH_CONFIGURE}" == "1" ]]; then
    cmake --fresh --preset "${preset}"
  else
    cmake --preset "${preset}"
  fi
}

run_build_preset() {
  local preset="$1"
  local args=(--preset "${preset}")
  if [[ -n "${BUILD_TARGET}" ]]; then
    args+=(--target "${BUILD_TARGET}")
  fi
  cmake --build "${args[@]}"
}

if [[ "${BUILD_MODE}" == "xcode" ]]; then
  run_configure_preset "xcode"

  if [[ "${BUILD_TYPE}" == "Debug" ]]; then
    run_build_preset "xcode-debug"
  else
    run_build_preset "xcode-release"
  fi

  echo "Xcode ${BUILD_TYPE} build completed: ${PROJECT_ROOT}/build/xcode"
  exit 0
fi

if [[ "${BUILD_TYPE}" == "Debug" ]]; then
  run_configure_preset "unix-debug"
  run_build_preset "unix-debug"
  echo "Unix Debug build completed: ${PROJECT_ROOT}/build/unix-debug"
else
  run_configure_preset "unix-release"
  run_build_preset "unix-release"
  echo "Unix Release build completed: ${PROJECT_ROOT}/build/unix-release"
fi
