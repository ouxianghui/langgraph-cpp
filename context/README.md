# Project Context Pack

`context/` holds **project-specific** facts and skills for `langgraph-cpp`.

Generic C++ process skills live in [`.agent/`](../.agent/README.md). Authoritative design
evidence lives in [`docs/`](../docs/AI_INDEX.md). This directory distills those docs into
agent-facing conventions and library skills.

## Layout

| Path | Role |
| --- | --- |
| [`AGENTS.md`](AGENTS.md) | Project identity, skill routing, non-negotiable rules. |
| [`AUTHORITY.md`](AUTHORITY.md) | Authority order + pinned API/schema versions. |
| [`CONVENTIONS.md`](CONVENTIONS.md) | Architecture, ownership, API, concurrency, error, security conventions. |
| [`STACK.md`](STACK.md) | C++23 / CMake stack, options, targets, test commands. |
| [`SKILL_INVENTORY.md`](SKILL_INVENTORY.md) | Required files inventory (CI-checked). |
| [`REVIEW_CHECKLIST.md`](REVIEW_CHECKLIST.md) | PR checklist for context ↔ docs sync. |
| [`skills/`](skills/) | Library-specific usage and coding skills. |

## How Agents Should Load This Pack

**Minimal load policy:** constrain reads; do not delete skills to save tokens.

| Rule | Practice |
| --- | --- |
| Smallest set | One routing row from [`AGENTS.md`](AGENTS.md). Do not preload all of `skills/`. |
| Authority first | Skim [`AUTHORITY.md`](AUTHORITY.md) + relevant [`CONVENTIONS.md`](CONVENTIONS.md) sections before specialty skills. |
| Simple-change budget | Focused edits: **≤2** `skills/*.md` (and usually ≤2 `.agent` skills). Do not open 5+ context skills by default. |
| Progressive disclosure | Open stream/subgraph/HITL/provider-http/performance only when the change hits that topic. |
| Inventory is lookup | The table below is an index, not a reading list. |

At the start of a library-editing task:

1. Use root [`AGENTS.md`](../AGENTS.md) / [`CLAUDE.md`](../CLAUDE.md) for the dual-layer policy.
2. Open [`AGENTS.md`](AGENTS.md) for routing (prefer over reading this README end-to-end every time).
3. Skim [`AUTHORITY.md`](AUTHORITY.md) when contracts/conflicts apply.
4. Skim only the needed sections of [`CONVENTIONS.md`](CONVENTIONS.md) / [`STACK.md`](STACK.md).
5. Load the matching minimal [`skills/*.md`](skills/) set for the touched layer.
6. Prefer authority order in `AUTHORITY.md` when summaries disagree with docs/source.

## Skill Inventory

| Skill | Use when |
| --- | --- |
| [`skills/foundation.md`](skills/foundation.md) | Editing or using `src/foundation` infrastructure. |
| [`skills/core.md`](skills/core.md) | Editing or using `src/core` / `lgc::core` service assembly and lifecycle wiring. |
| [`skills/langgraph.md`](skills/langgraph.md) | Editing or using `src/langgraph` runtime surface. |
| [`skills/coding-standards.md`](skills/coding-standards.md) | C++ naming, headers, public API, Status/Result, optional deps. |
| [`skills/concurrency.md`](skills/concurrency.md) | Executors, owner threads, parallel super-steps, shutdown. |
| [`skills/persistence.md`](skills/persistence.md) | Checkpoint, store, storage, resume/replay, schemas. |
| [`skills/security.md`](skills/security.md) | Tools, HTTP auth, redaction, edge/hardware boundaries. |
| [`skills/stream-projection.md`](skills/stream-projection.md) | Stream envelopes, projection modes, stream fixtures. |
| [`skills/subgraph.md`](skills/subgraph.md) | Subgraphs and checkpoint namespaces. |
| [`skills/hitl-interrupt.md`](skills/hitl-interrupt.md) | Interrupts, HITL, `Command::resume`. |
| [`skills/provider-http.md`](skills/provider-http.md) | Provider chat models and HTTP/auth ports. |
| [`skills/testing-examples.md`](skills/testing-examples.md) | Writing tests and acceptance examples. |
| [`skills/performance.md`](skills/performance.md) | Hot paths, limits, and performance claims. |

## Regression Gates

```sh
scripts/check-dependency-policy.sh
scripts/check-context-skills.sh
```

See [`REVIEW_CHECKLIST.md`](REVIEW_CHECKLIST.md) for PR review steps.

## Source Of Truth

| Topic | Authoritative doc |
| --- | --- |
| Authority / pins | [`AUTHORITY.md`](AUTHORITY.md), [`PROJECT_MANIFEST.json`](../PROJECT_MANIFEST.json) |
| Design evidence index | [`docs/AI_INDEX.md`](../docs/AI_INDEX.md) |
| Architecture | [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md) |
| API contract | [`docs/API_CONTRACT.md`](../docs/API_CONTRACT.md) |
| Dependency rules | [`docs/DEPENDENCY_POLICY.md`](../docs/DEPENDENCY_POLICY.md) |
| ADRs | [`docs/ADR/`](../docs/ADR/README.md) |
| Current limitations | [`docs/LIMITATIONS.md`](../docs/LIMITATIONS.md) |

Do not duplicate large doc prose here. Keep skills concise; link back to `docs/` for detail.
