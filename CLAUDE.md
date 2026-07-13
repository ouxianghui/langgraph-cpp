# Claude Code Context

This file is the entrypoint for Claude Code working in this repository.

## Default `.agent` Skill Policy

Claude Code must use `.agent/` as the default skill registry for this repository.

At the start of a task:

1. Read `.agent/README.md` to discover the available skills.
2. Classify the task.
3. Read the matching `.agent/skills/*.md` files before planning or editing.
4. Use `.agent/skills/definition-of-done.md` before final handoff.

Do not load every skill file by default. Always load the task-specific skills that match the current
request.

## Project Context

Before editing code, read:

1. `README.md` for user-facing scope, build commands, and examples.
2. `docs/AI_INDEX.md` for the current design evidence chain.
3. `docs/PRD.md` for product and MVP requirements.
4. `docs/ROADMAP.md` and `docs/internal/WBS.md` for staged implementation intent.
5. `docs/LIMITATIONS.md` for current boundaries and deferred work.
6. `AGENTS.md` for the full repository map, build/test commands, and non-negotiable rules.

## Skill Routing

Use these `.agent` skills by task type:

| Task | Skills to read |
| --- | --- |
| Feature implementation or behavior change | `.agent/skills/implementation.md`, `.agent/skills/definition-of-done.md` |
| Broad requirement or module planning | `.agent/skills/module-plan.md`, plus relevant specialist skills |
| Public API, headers, callbacks, service contracts | `.agent/skills/api-design.md`, `.agent/skills/modern-cpp.md` |
| C++ source/header edits | `.agent/skills/modern-cpp.md`, `.agent/skills/memory-safety.md` |
| Checkpoint, resume, state, storage, lifecycle | `.agent/skills/design-patterns.md`, `.agent/skills/memory-safety.md`, `.agent/skills/testing.md` |
| Threading, executors, callbacks, timers, shutdown | `.agent/skills/concurrency.md`, `.agent/skills/memory-safety.md`, `.agent/skills/testing.md` |
| Build files, CMake, targets, optional dependencies | `.agent/skills/build-system.md` |
| Tests | `.agent/skills/testing.md` |
| Debugging failures | `.agent/skills/debugging.md` |
| Refactoring without behavior change | `.agent/skills/refactoring.md`, `.agent/skills/definition-of-done.md` |
| Code review | `.agent/skills/code-review.md` and any specialist skills matching the diff |
| Logging, metrics, redaction, sensitive data | `.agent/skills/security-logging.md` |
| Hot paths, queues, retries, batching, high-volume events | `.agent/skills/performance.md` |

## Repository Rules

The rules in `AGENTS.md` are authoritative for Claude Code:

- project identity and scope;
- repository map;
- build and test commands;
- non-negotiable C++ runtime rules;
- definition of done.

If this file conflicts with `AGENTS.md`, follow `AGENTS.md`.
