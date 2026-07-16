# Authority Order And Version Pins

Use this file whenever design facts conflict across `context/` skills, `docs/`, source, or agent
drafts.

Agents should **skim** this file for pins/order when contracts matter; do not treat it as a reason
to also load every specialty skill.

## 1. Authority Order (highest wins)

1. **Current source of truth for behavior** — headers/sources under `include/`, `src/`, plus focused
   tests that encode the contract (`tests/`, `testdata/`).
2. **Pinned contract docs** — [`docs/API_CONTRACT.md`](../docs/API_CONTRACT.md) and the version
   fields in [`PROJECT_MANIFEST.json`](../PROJECT_MANIFEST.json).
3. **Accepted ADRs** under [`docs/ADR/`](../docs/ADR/README.md).
4. **Model docs** — `ARCHITECTURE`, `CONCURRENCY_MODEL`, `PERSISTENCE_MODEL`, `ERROR_MODEL`,
   `SECURITY_MODEL`, `PERFORMANCE_MODEL`, `DEPENDENCY_POLICY`, `OWNERSHIP`, `LIMITATIONS`.
5. **`context/` distillations** — `CONVENTIONS.md`, `STACK.md`, and `skills/*.md` (assistant aids only).
6. **Roadmap / WBS / planned notes** — never treat Planned items as implemented.

Explicit user instructions for the current task override *process* choices, but agents must **not**
silently invent or widen public contracts against (1)–(4). If the user asks for a contract change,
update (2)–(4) and pins in the same change set.

## 2. Pinned Contract Versions

These pins must match `PROJECT_MANIFEST.json` → `contracts`. The check script
[`scripts/check-context-skills.sh`](../scripts/check-context-skills.sh) fails when they drift.

<!-- AUTHORITY_PINS
api_contract_version=28
schema_contract_version=1
checkpoint_json_schema_version=3
content_envelope_version=1
storage_schema_version=1
abi_stable=false
-->

| Pin | Value | Meaning |
| --- | ---: | --- |
| `api_contract_version` | 28 | Numbered public source API contract (`langgraph.hpp` surface). |
| `schema_contract_version` | 1 | Overall persisted schema contract lineage. |
| `checkpoint_json_schema_version` | 3 | Checkpoint JSON reader/writer schema. |
| `content_envelope_version` | 1 | Content envelope schema. |
| `storage_schema_version` | 1 | Storage schema. |
| `abi_stable` | false | No ABI stability before 1.0. |

When bumping any pin: update `PROJECT_MANIFEST.json`, `docs/API_CONTRACT.md` (as required), this file’s
`AUTHORITY_PINS` block **and** the table above, then run `scripts/check-context-skills.sh`.

## 3. Surface Boundaries

| Surface | In contract? | Notes |
| --- | --- | --- |
| Types reachable from `include/langgraph_cpp/langgraph.hpp` | Yes (API contract) | Primary frozen source API. |
| `src/core` / `lgc::core` (`RuntimeContainer`, …) | Not in numbered API contract today | Installed/optional assembly; still subject to layering rules. |
| Private `.hh` / `.cpp` helpers | No | May change without API bump unless behavior leaks into tests/docs contract. |
| Optional adapter internals (provider/llama/hardware) | No | Behind options/ports; see LIMITATIONS. |

## 4. How Agents Must Resolve Conflicts

| Situation | Action |
| --- | --- |
| `context/skills/*` vs `docs/*` | Prefer `docs/` + ADRs; patch the skill in the same change. |
| `docs/*` vs current source/tests | Prefer source/tests; open/fix docs immediately if docs are stale. |
| Two docs disagree | Prefer `API_CONTRACT` / ADR over narrative prose; cite both. |
| Skill omits a pin/version | Do not invent versions; read `PROJECT_MANIFEST.json` / this file. |
| Roadmap says Planned, skill implies shipped | Follow `LIMITATIONS.md` / tests; refuse over-claim. |

## 5. Sync Checklist (contract change)

- [ ] Update `PROJECT_MANIFEST.json` contracts.
- [ ] Update `docs/API_CONTRACT.md` (and schema notes if needed).
- [ ] Refresh `AUTHORITY_PINS` + table in this file.
- [ ] Update affected `context/skills/*` and `CONVENTIONS.md` only where behavior rules change.
- [ ] Run `scripts/check-context-skills.sh` and relevant focused tests.
