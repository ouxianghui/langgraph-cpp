#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build/unix-coverage}"
REPORT_DIR="${ROOT_DIR}/docs/reports"
JOBS="${JOBS:-2}"
COVERAGE_FLAGS="${COVERAGE_FLAGS:---coverage -O0 -g}"

mkdir -p "${REPORT_DIR}"

printf 'Configuring coverage build in %s\n' "${BUILD_DIR#${ROOT_DIR}/}"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="${COVERAGE_FLAGS}" \
    -DCMAKE_CXX_FLAGS="${COVERAGE_FLAGS}" \
    -DCMAKE_EXE_LINKER_FLAGS="--coverage" \
    -DCMAKE_SHARED_LINKER_FLAGS="--coverage" \
    -DLANGGRAPH_CPP_BUILD_TESTS=ON \
    -DLANGGRAPH_CPP_BUILD_EXAMPLES=ON \
    -DLANGGRAPH_CPP_WITH_SQLITE=ON \
    -DLANGGRAPH_CPP_WITH_NETWORK=ON

cmake --build "${BUILD_DIR}" --parallel "${JOBS}"
ctest --test-dir "${BUILD_DIR}" --output-on-failure

SUMMARY_FILE="${REPORT_DIR}/coverage-summary.md"
GENERATED_AT="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

if command -v gcovr >/dev/null 2>&1; then
    gcovr \
        --root "${ROOT_DIR}" \
        --exclude "${ROOT_DIR}/third_party" \
        --exclude "${ROOT_DIR}/build" \
        --print-summary \
        --html-details "${REPORT_DIR}/coverage.html" \
        >"${REPORT_DIR}/coverage.txt"
    {
        printf '# Coverage Summary\n\n'
        printf '| Field | Value |\n'
        printf '| --- | --- |\n'
        printf '| Generated at | `%s` |\n' "${GENERATED_AT}"
        printf '| Tool | `gcovr` |\n'
        printf '| Build directory | `%s` |\n' "${BUILD_DIR#${ROOT_DIR}/}"
        printf '| HTML report | [coverage.html](coverage.html) |\n\n'
        printf '```text\n'
        cat "${REPORT_DIR}/coverage.txt"
        printf '```\n'
    } >"${SUMMARY_FILE}"
elif command -v lcov >/dev/null 2>&1 && command -v genhtml >/dev/null 2>&1; then
    lcov --directory "${BUILD_DIR}" --capture --output-file "${REPORT_DIR}/coverage.info"
    lcov --remove "${REPORT_DIR}/coverage.info" \
        "${ROOT_DIR}/third_party/*" \
        "${ROOT_DIR}/build/*" \
        --output-file "${REPORT_DIR}/coverage.filtered.info"
    genhtml "${REPORT_DIR}/coverage.filtered.info" --output-directory "${REPORT_DIR}/coverage-html"
    {
        printf '# Coverage Summary\n\n'
        printf '| Field | Value |\n'
        printf '| --- | --- |\n'
        printf '| Generated at | `%s` |\n' "${GENERATED_AT}"
        printf '| Tool | `lcov` / `genhtml` |\n'
        printf '| Build directory | `%s` |\n' "${BUILD_DIR#${ROOT_DIR}/}"
        printf '| HTML report | [coverage-html/index.html](coverage-html/index.html) |\n'
    } >"${SUMMARY_FILE}"
else
    {
        printf '# Coverage Summary\n\n'
        printf '| Field | Value |\n'
        printf '| --- | --- |\n'
        printf '| Generated at | `%s` |\n' "${GENERATED_AT}"
        printf '| Tool | not available |\n'
        printf '| Build directory | `%s` |\n\n' "${BUILD_DIR#${ROOT_DIR}/}"
        printf 'Coverage instrumentation build and tests completed, but neither `gcovr` nor `lcov` + `genhtml` was found. Install one of those tools and re-run `scripts/coverage.sh` to render a report.\n'
    } >"${SUMMARY_FILE}"
fi

printf 'Wrote %s\n' "${SUMMARY_FILE#${ROOT_DIR}/}"
