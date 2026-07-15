# Context Skill Inventory

Canonical list of required `context/` files. Used by humans and by
[`scripts/check-context-skills.sh`](../scripts/check-context-skills.sh).

## Required Pack Files

| Path | Role |
| --- | --- |
| `context/README.md` | Pack entry + inventory index |
| `context/AGENTS.md` | Project routing + non-negotiables |
| `context/AUTHORITY.md` | Authority order + version pins |
| `context/CONVENTIONS.md` | Distilled design conventions |
| `context/STACK.md` | Build/stack/commands |
| `context/REVIEW_CHECKLIST.md` | Review checklist for context/docs sync |
| `context/SKILL_INVENTORY.md` | This file |

## Required Skills

| Path | Role |
| --- | --- |
| `context/skills/foundation.md` | `src/foundation` |
| `context/skills/core.md` | `src/core` / `lgc::core` |
| `context/skills/langgraph.md` | `src/langgraph` |
| `context/skills/coding-standards.md` | Cross-cutting C++ / API standards |
| `context/skills/concurrency.md` | Executors / ownership / super-step concurrency |
| `context/skills/persistence.md` | Checkpoint / store / storage |
| `context/skills/security.md` | Tools / auth / redaction / edge |
| `context/skills/stream-projection.md` | Stream envelopes and projections |
| `context/skills/subgraph.md` | Subgraphs and checkpoint namespaces |
| `context/skills/hitl-interrupt.md` | Interrupt / HITL / resume |
| `context/skills/provider-http.md` | Provider models and HTTP client |
| `context/skills/testing-examples.md` | Tests and examples authorship |
| `context/skills/performance.md` | Hot paths and performance claims |

## Required Cross-Links

Skills that claim contract or layering facts must remain consistent with:

- `docs/ARCHITECTURE.md` (including `src/core` / `lgc::core`)
- `docs/DEPENDENCY_POLICY.md`
- `docs/API_CONTRACT.md` / `PROJECT_MANIFEST.json` (via `AUTHORITY.md` pins)
- `docs/OWNERSHIP.md` (`context/` ownership row)

## Change Rules

1. Adding/removing a skill: update this inventory, `context/README.md`, `context/AGENTS.md`, root
   `AGENTS.md` / `CLAUDE.md` routing tables, and the check script allow-list.
2. Never leave required files untracked in a merge intended to ship context — the check script runs
   in CI and fails on missing files.
3. After edits, run `scripts/check-context-skills.sh`.
