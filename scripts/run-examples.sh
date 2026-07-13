#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR_INPUT="${BUILD_DIR:-build/unix-debug}"
REPORT_DIR="${ROOT_DIR}/docs/reports"
REPORT_FILE="${REPORT_DIR}/example-smoke-report.md"

if [[ "${BUILD_DIR_INPUT}" = /* ]]; then
    BUILD_DIR_ABS="${BUILD_DIR_INPUT}"
    BUILD_DIR_DISPLAY="${BUILD_DIR_INPUT#${ROOT_DIR}/}"
else
    BUILD_DIR_ABS="${ROOT_DIR}/${BUILD_DIR_INPUT}"
    BUILD_DIR_DISPLAY="${BUILD_DIR_INPUT}"
fi

EXAMPLES=(
    minimal_graph
    parallel_fanout
    conditional_fanout
    send_map_reduce
    command_goto
    conditional_graph
    loop_graph
    checkpoint_resume
    sqlite_checkpoint_resume
    time_travel_history
    long_term_memory_store
    stream_projection
    subgraph_module
    model_tool_model_loop
    agent_pattern_react
    agent_pattern_plan_and_solve
    agent_pattern_reflection
    human_interrupt
    tool_approval_loop
    edge_mock_tool_adapter
    mock_edge_repair
)

mkdir -p "${REPORT_DIR}"

SUMMARY_FILE="$(mktemp)"
DETAILS_FILE="$(mktemp)"
trap 'rm -f "${SUMMARY_FILE}" "${DETAILS_FILE}"' EXIT

failures=0

append_summary() {
    local example="$1"
    local status="$2"
    local binary="$3"

    printf '| `%s` | %s | `%s` |\n' "${example}" "${status}" "${binary}" >>"${SUMMARY_FILE}"
}

append_detail() {
    local example="$1"
    local status="$2"
    local binary="$3"
    local output="$4"

    {
        printf '\n### `%s`\n\n' "${example}"
        printf -- '- Status: `%s`\n' "${status}"
        printf -- '- Binary: `%s`\n\n' "${binary}"
        printf '```text\n'
        if [[ -n "${output}" ]]; then
            printf '%s\n' "${output}"
        else
            printf '<no output>\n'
        fi
        printf '```\n'
    } >>"${DETAILS_FILE}"
}

for example in "${EXAMPLES[@]}"; do
    binary="${BUILD_DIR_ABS}/examples/${example}"
    display_binary="${BUILD_DIR_DISPLAY}/examples/${example}"

    if [[ ! -x "${binary}" ]]; then
        append_summary "${example}" "missing" "${display_binary}"
        append_detail "${example}" "missing" "${display_binary}" "Executable not found or not executable. Build examples first."
        failures=$((failures + 1))
        continue
    fi

    set +e
    output="$("${binary}" 2>&1)"
    exit_code=$?
    set -e
    output="${output//${ROOT_DIR}/<repo>}"

    if [[ "${exit_code}" -eq 0 ]]; then
        append_summary "${example}" "passed" "${display_binary}"
        append_detail "${example}" "passed" "${display_binary}" "${output}"
    else
        append_summary "${example}" "failed (${exit_code})" "${display_binary}"
        append_detail "${example}" "failed (${exit_code})" "${display_binary}" "${output}"
        failures=$((failures + 1))
    fi
done

GENERATED_AT="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
COMMIT="$(cd "${ROOT_DIR}" && git rev-parse --short HEAD 2>/dev/null || printf 'unknown')"
BRANCH="$(cd "${ROOT_DIR}" && git rev-parse --abbrev-ref HEAD 2>/dev/null || printf 'unknown')"

if [[ "${failures}" -eq 0 ]]; then
    overall="passed"
else
    overall="failed"
fi

{
    printf '# 示例 Smoke 报告\n\n'
    printf '| 字段 | 内容 |\n'
    printf '| --- | --- |\n'
    printf '| 状态 | `%s` |\n' "${overall}"
    printf '| 生成时间 | `%s` |\n' "${GENERATED_AT}"
    printf '| Git branch | `%s` |\n' "${BRANCH}"
    printf '| Git commit | `%s` |\n' "${COMMIT}"
    printf '| Build directory | `%s` |\n' "${BUILD_DIR_DISPLAY}"
    printf '| 示例数量 | `%s` |\n' "${#EXAMPLES[@]}"
    printf '\n'
    printf '本文由 [../../scripts/run-examples.sh](../../scripts/run-examples.sh) 生成，用于证明默认示例不是文档摆设，而是可运行验收信号。可选 llama.cpp 示例不属于默认 smoke。\n\n'
    printf '## 汇总\n\n'
    printf '| Example | Status | Binary |\n'
    printf '| --- | --- | --- |\n'
    cat "${SUMMARY_FILE}"
    printf '\n## 输出\n'
    cat "${DETAILS_FILE}"
} >"${REPORT_FILE}"

printf 'Wrote %s\n' "${REPORT_FILE#${ROOT_DIR}/}"

if [[ "${failures}" -ne 0 ]]; then
    exit 1
fi
