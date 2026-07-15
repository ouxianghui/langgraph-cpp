#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

failures=0

report_failure() {
    printf 'ERROR: %s\n' "$1" >&2
    failures=$((failures + 1))
}

require_file() {
    local file="$1"
    if [[ ! -f "${file}" ]]; then
        report_failure "missing required file: ${file}"
    fi
}

require_file_contains() {
    local file="$1"
    local pattern="$2"
    local message="$3"

    if [[ ! -f "${file}" ]]; then
        report_failure "missing file for content check: ${file}"
        return
    fi
    if ! rg -q "${pattern}" "${file}"; then
        report_failure "${message}"
    fi
}

REQUIRED_FILES=(
    "context/README.md"
    "context/AGENTS.md"
    "context/AUTHORITY.md"
    "context/CONVENTIONS.md"
    "context/STACK.md"
    "context/REVIEW_CHECKLIST.md"
    "context/SKILL_INVENTORY.md"
    "context/skills/foundation.md"
    "context/skills/core.md"
    "context/skills/langgraph.md"
    "context/skills/coding-standards.md"
    "context/skills/concurrency.md"
    "context/skills/persistence.md"
    "context/skills/security.md"
    "context/skills/stream-projection.md"
    "context/skills/subgraph.md"
    "context/skills/hitl-interrupt.md"
    "context/skills/provider-http.md"
    "context/skills/testing-examples.md"
    "context/skills/performance.md"
)

for file in "${REQUIRED_FILES[@]}"; do
    require_file "${file}"
done

# Entrypoints must load context/ and keep the minimal-load policy
require_file_contains \
    "AGENTS.md" \
    'Minimal Load Policy' \
    "AGENTS.md must define Minimal Load Policy"
require_file_contains \
    "AGENTS.md" \
    'context/skills/' \
    "AGENTS.md must route to context/skills/"
require_file_contains \
    "CLAUDE.md" \
    'Minimal Load Policy' \
    "CLAUDE.md must define Minimal Load Policy"
require_file_contains \
    "CLAUDE.md" \
    'context/skills/' \
    "CLAUDE.md must route to context/skills/"
require_file_contains \
    "context/AGENTS.md" \
    'Minimal Load Policy' \
    "context/AGENTS.md must define Minimal Load Policy"
require_file_contains \
    "context/README.md" \
    'Simple-change budget' \
    "context/README.md must document the simple-change budget"

# Docs closed-loop for lgc::core
require_file_contains \
    "docs/ARCHITECTURE.md" \
    'lgc::core' \
    "docs/ARCHITECTURE.md must document lgc::core"
require_file_contains \
    "docs/DEPENDENCY_POLICY.md" \
    'src/core' \
    "docs/DEPENDENCY_POLICY.md must mention src/core rules"
require_file_contains \
    "docs/OWNERSHIP.md" \
    'src/core' \
    "docs/OWNERSHIP.md must own src/core"
require_file_contains \
    "docs/OWNERSHIP.md" \
    'context/' \
    "docs/OWNERSHIP.md must own context/"

# Core skill must not claim docs silence once docs document lgc::core
if [[ -f "context/skills/core.md" ]]; then
    if rg -q 'docs/ does \*\*not\*\* currently describe' "context/skills/core.md"; then
        report_failure "context/skills/core.md still claims docs omit lgc::core; update after docs closed loop"
    fi
fi

# Authority pins must match PROJECT_MANIFEST.json contracts
if [[ ! -f "context/AUTHORITY.md" || ! -f "PROJECT_MANIFEST.json" ]]; then
    report_failure "AUTHORITY.md or PROJECT_MANIFEST.json missing for pin sync"
else
    pin_block="$(sed -n '/<!-- AUTHORITY_PINS/,/-->/p' "context/AUTHORITY.md")"
    if [[ -z "${pin_block}" ]]; then
        report_failure "context/AUTHORITY.md missing AUTHORITY_PINS block"
    else
        while IFS='=' read -r key value; do
            [[ -z "${key}" ]] && continue
            manifest_value="$(python3 - "${key}" <<'PY'
import json, sys
key = sys.argv[1]
with open("PROJECT_MANIFEST.json", encoding="utf-8") as f:
    data = json.load(f)
contracts = data.get("contracts", {})
if key not in contracts:
    print("")
    sys.exit(2)
value = contracts[key]
if isinstance(value, bool):
    print("true" if value else "false")
else:
    print(value)
PY
)" || {
                report_failure "PROJECT_MANIFEST.json missing contracts.${key}"
                continue
            }
            if [[ "${manifest_value}" != "${value}" ]]; then
                report_failure "AUTHORITY pin ${key}=${value} != PROJECT_MANIFEST contracts.${key}=${manifest_value}"
            fi
        done < <(printf '%s\n' "${pin_block}" | sed -n 's/^[[:space:]]*\([a-z_]*\)=\(.*\)$/\1=\2/p')
    fi
fi

# Inventory must mention each required skill basename
if [[ -f "context/SKILL_INVENTORY.md" ]]; then
    for file in "${REQUIRED_FILES[@]}"; do
        if [[ "${file}" == context/skills/* ]]; then
            base="$(basename "${file}")"
            if ! rg -q "${base}" "context/SKILL_INVENTORY.md"; then
                report_failure "SKILL_INVENTORY.md missing entry for ${base}"
            fi
        fi
    done
fi

# README inventory should stay in sync with skill set
if [[ -f "context/README.md" ]]; then
    for file in "${REQUIRED_FILES[@]}"; do
        if [[ "${file}" == context/skills/* ]]; then
            base="$(basename "${file}")"
            if ! rg -q "${base}" "context/README.md"; then
                report_failure "context/README.md missing skill inventory link for ${base}"
            fi
        fi
    done
fi

if (( failures > 0 )); then
    printf '\nContext skills check failed with %d issue(s).\n' "${failures}" >&2
    exit 1
fi

printf 'Context skills check passed.\n'
