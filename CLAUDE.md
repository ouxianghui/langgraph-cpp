# Claude Code Context

This file is the entrypoint for Claude Code working in this repository.

## Default Skill Policy

Claude Code must use:

- `.agent/` for project-neutral process skills;
- `context/` for langgraph-cpp conventions and foundation / core / langgraph library skills.

### Minimal Load Policy (cost control)

Prefer constraining how much you read over deleting skills from the pack.

1. **Smallest skill set only** — one routing row; do not preload all of `context/skills/`.
2. **Authority first** — skim `context/AUTHORITY.md` and relevant `context/CONVENTIONS.md` sections
   before specialty skills (stream / subgraph / HITL / provider-http / performance).
3. **Simple-change budget** — focused edits: **≤2** context skills and **≤2** `.agent` skills
   (plus `definition-of-done` at handoff). Do not open five-plus context skills by default.
4. **Progressive disclosure** — open a specialty skill only when the change actually hits that topic.
5. **Never full-pack reads** — inventory tables are for lookup, not mandatory reading.

### At the start of a task

1. Read `.agent/README.md` only as needed to pick process skills.
2. Read `context/AGENTS.md` (or `context/README.md`) for routing — not both end-to-end.
3. Read `context/AUTHORITY.md` when contracts/schemas/conflicts apply; otherwise defer.
4. Classify the task; pick the single best routing row.
5. Read that minimal `.agent/skills/*.md` + `context/skills/*.md` set before editing.
6. Use `.agent/skills/definition-of-done.md` before final handoff.
7. When changing `context/` or layering, run `scripts/check-context-skills.sh` and/or
   `scripts/check-dependency-policy.sh`.

## Project Context

Before editing code, read:

1. `README.md` for user-facing scope, build commands, and examples.
2. `docs/AI_INDEX.md` for the current design evidence chain.
3. `docs/PRD.md` for product and MVP requirements.
4. `docs/ROADMAP.md` and `docs/internal/WBS.md` for staged implementation intent.
5. `docs/LIMITATIONS.md` for current boundaries and deferred work.
6. `AGENTS.md` for the full repository map, build/test commands, and non-negotiable rules.
7. For foundation/core/langgraph edits: relevant sections of `context/CONVENTIONS.md`, plus
   `context/AUTHORITY.md` / `context/STACK.md` only when contracts or build options are in scope.

## Skill Routing

Use `.agent` process skills by task type (see `AGENTS.md` for the full table). Additionally load
these `context` library skills when the task touches the corresponding layer:

| Task | Context skills |
| --- | --- |
| `src/foundation/**` | `context/skills/foundation.md`, `context/skills/coding-standards.md` |
| `src/core/**` / RuntimeServices / RuntimeContainer | `context/skills/core.md`, `context/skills/coding-standards.md` |
| `src/langgraph/**` | `context/skills/langgraph.md`, `context/skills/coding-standards.md` |
| Stream / projection | `context/skills/stream-projection.md` |
| Subgraph / namespace | `context/skills/subgraph.md` |
| Interrupt / HITL | `context/skills/hitl-interrupt.md` |
| Provider / HTTP | `context/skills/provider-http.md` |
| Graph concurrency / executors | `context/skills/concurrency.md` |
| Checkpoint / store / storage | `context/skills/persistence.md` |
| Tools / auth / redaction / edge | `context/skills/security.md` |
| Tests / examples | `context/skills/testing-examples.md` |
| Performance / hot paths | `context/skills/performance.md` |

## Repository Rules

The rules in `AGENTS.md` are authoritative for Claude Code:

- project identity and scope;
- repository map;
- build and test commands;
- non-negotiable C++ runtime rules;
- definition of done;
- dual-layer skill loading with `context/`.

Conflict resolution for design facts follows [`context/AUTHORITY.md`](context/AUTHORITY.md).
If this file conflicts with `AGENTS.md` on process/routing, follow `AGENTS.md`.
