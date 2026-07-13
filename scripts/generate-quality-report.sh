#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPORT_DIR="${ROOT_DIR}/docs/reports"
REPORT_FILE="${REPORT_DIR}/latest-quality-report.md"
BUILD_DIR_INPUT="${BUILD_DIR:-build/unix-debug}"
RUN_FULL=0

if [[ "${BUILD_DIR_INPUT}" = /* ]]; then
    BUILD_DIR_ABS="${BUILD_DIR_INPUT}"
    BUILD_DIR_DISPLAY="${BUILD_DIR_INPUT#${ROOT_DIR}/}"
else
    BUILD_DIR_ABS="${ROOT_DIR}/${BUILD_DIR_INPUT}"
    BUILD_DIR_DISPLAY="${BUILD_DIR_INPUT}"
fi

for arg in "$@"; do
    case "${arg}" in
        --full)
            RUN_FULL=1
            ;;
        -h|--help)
            cat <<'USAGE'
Usage: scripts/generate-quality-report.sh [--full]

Generates docs/reports/latest-quality-report.md.

Default checks:
  - dependency policy
  - docs snippet compile, when BUILD_DIR exists
  - git diff whitespace check

With --full:
  - also runs the default CTest suite from BUILD_DIR, when it exists

Environment:
  BUILD_DIR defaults to build/unix-debug.
USAGE
            exit 0
            ;;
        *)
            printf 'Unknown argument: %s\n' "${arg}" >&2
            exit 2
            ;;
    esac
done

mkdir -p "${REPORT_DIR}"

SUMMARY_FILE="$(mktemp)"
DETAILS_FILE="$(mktemp)"
trap 'rm -f "${SUMMARY_FILE}" "${DETAILS_FILE}"' EXIT

append_row() {
    local name="$1"
    local status="$2"
    local command="$3"
    local safe_command="${command//|/\\|}"

    printf '| %s | %s | `%s` |\n' "${name}" "${status}" "${safe_command}" >>"${SUMMARY_FILE}"
}

append_detail() {
    local name="$1"
    local status="$2"
    local command="$3"
    local output="$4"

    {
        printf '\n### %s\n\n' "${name}"
        printf -- '- Status: `%s`\n' "${status}"
        printf -- '- Command: `%s`\n\n' "${command}"
        printf '```text\n'
        if [[ -n "${output}" ]]; then
            printf '%s\n' "${output}"
        else
            printf '<no output>\n'
        fi
        printf '```\n'
    } >>"${DETAILS_FILE}"
}

run_check() {
    local name="$1"
    local command="$2"
    local output
    local exit_code

    set +e
    output="$(cd "${ROOT_DIR}" && eval "${command}" 2>&1)"
    exit_code=$?
    set -e
    output="${output//${ROOT_DIR}/<repo>}"

    if [[ "${exit_code}" -eq 0 ]]; then
        append_row "${name}" "passed" "${command}"
        append_detail "${name}" "passed" "${command}" "${output}"
    else
        append_row "${name}" "failed (${exit_code})" "${command}"
        append_detail "${name}" "failed (${exit_code})" "${command}" "${output}"
    fi

    return "${exit_code}"
}

record_not_run() {
    local name="$1"
    local command="$2"
    local reason="$3"

    append_row "${name}" "not run" "${command}"
    append_detail "${name}" "not run" "${command}" "${reason}"
    if [[ "${OVERALL_STATUS:-passed}" == "passed" ]]; then
        OVERALL_STATUS="passed-with-skips"
    fi
}

OVERALL_STATUS="passed"

if ! run_check "Dependency policy" "scripts/check-dependency-policy.sh"; then
    OVERALL_STATUS="failed"
fi

if [[ -d "${BUILD_DIR_ABS}" ]]; then
    if ! run_check "Docs snippet compile" "ctest --test-dir ${BUILD_DIR_DISPLAY} -L docs --output-on-failure"; then
        OVERALL_STATUS="failed"
    fi
else
    record_not_run "Docs snippet compile" "ctest --test-dir ${BUILD_DIR_DISPLAY} -L docs --output-on-failure" "${BUILD_DIR_DISPLAY} does not exist. Run cmake --preset unix-debug first, or set BUILD_DIR."
fi

if ! run_check "Git diff whitespace" "git diff --check"; then
    OVERALL_STATUS="failed"
fi

if [[ "${RUN_FULL}" -eq 1 ]]; then
    if [[ -d "${BUILD_DIR_ABS}" ]]; then
        if ! run_check "Default CTest suite" "ctest --test-dir ${BUILD_DIR_DISPLAY} --output-on-failure"; then
            OVERALL_STATUS="failed"
        fi
    else
        record_not_run "Default CTest suite" "ctest --test-dir ${BUILD_DIR_DISPLAY} --output-on-failure" "${BUILD_DIR_DISPLAY} does not exist. Run cmake --preset unix-debug first, or set BUILD_DIR."
    fi
else
    record_not_run "Default CTest suite" "ctest --test-dir ${BUILD_DIR_DISPLAY} --output-on-failure" "Skipped by default. Re-run with --full to execute."
fi

GENERATED_AT="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
COMMIT="$(cd "${ROOT_DIR}" && git rev-parse --short HEAD 2>/dev/null || printf 'unknown')"
BRANCH="$(cd "${ROOT_DIR}" && git rev-parse --abbrev-ref HEAD 2>/dev/null || printf 'unknown')"

{
    printf '# 最新质量报告\n\n'
    printf '| 字段 | 内容 |\n'
    printf '| --- | --- |\n'
    printf '| 状态 | `%s` |\n' "${OVERALL_STATUS}"
    printf '| 生成时间 | `%s` |\n' "${GENERATED_AT}"
    printf '| Git branch | `%s` |\n' "${BRANCH}"
    printf '| Git commit | `%s` |\n' "${COMMIT}"
    printf '| Build directory | `%s` |\n' "${BUILD_DIR_DISPLAY}"
    printf '| 生成命令 | `scripts/generate-quality-report.sh%s` |\n' "$([[ "${RUN_FULL}" -eq 1 ]] && printf ' --full' || true)"
    printf '\n'
    printf '本文由脚本生成，用于给维护者和 AI 工具提供最近一次本地质量门禁证据。它不是 release 证明；公开发布仍以 [../RELEASE_CHECKLIST.md](../RELEASE_CHECKLIST.md) 和 CI 为准。\n\n'
    printf '## 汇总\n\n'
    printf '| Check | Status | Command |\n'
    printf '| --- | --- | --- |\n'
    cat "${SUMMARY_FILE}"
    printf '\n## 详情\n'
    cat "${DETAILS_FILE}"
} >"${REPORT_FILE}"

printf 'Wrote %s\n' "${REPORT_FILE#${ROOT_DIR}/}"

if [[ "${OVERALL_STATUS}" == "failed" ]]; then
    exit 1
fi
