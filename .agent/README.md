# Generic C++ Coding-Agent Context

This `.agent` directory is intentionally project-neutral. It should be reusable in another C++ project
without editing the files inside `.agent`.

Project-specific facts live outside `.agent`, in the host project's context directory.

## How to Use

- Start from the host project entrypoint, usually root `AGENTS.md`.
- Load only the skills that match the current task. Do not load every skill by default.
- Usually choose one primary skill, then load only the small set of supporting skills for risks the
  task actually touches. Do not load unrelated skills just to complete a checklist.
- Use `module-plan.md` before coding when requirements are broad, cross-module, or design-heavy.
- Use `implementation.md` for feature work, bug fixes, and behavior-changing edits.
- Use `definition-of-done.md` before declaring a task complete or handing work off.
- Combine skills when the task crosses concerns. For example, a public async API change may need
  `implementation.md`, `api-design.md`, `concurrency.md`, `modern-cpp.md`, `memory-safety.md`, and
  `testing.md`.
- When a skill conflicts with the host project's explicit instructions, the host project wins.
- Skills describe process and guardrails; exact commands, target names, owners, helpers, and schemas
  come from the host project's context files.

## Non-Negotiable Safety Rules

- Do not present skipped, failed, unavailable, or unrun verification as passing.
- Do not summarize, review, or claim confidence in diffs that were not read.
- Do not commit, push, publish, deploy, or open reviews unless the user explicitly asks.

## Required External Context

When a skill says "read the project context", start with the host project's context entrypoint.
Recommended layout:

1. `context/AGENTS.md` - project routing, identity, local skill list, and non-negotiable rules.
2. `context/CONVENTIONS.md` - architecture, ownership, threading, API, logging, security, and
   review rules.
3. `context/STACK.md` - build system, dependencies, targets, test commands, runtime config, and
   debug commands.

A thin root `AGENTS.md` may point agents to `context/AGENTS.md` for tool compatibility.

Shared shorthand used by skills:

> Read project context first.

This means: start with the host project context entrypoint, typically `context/AGENTS.md`,
then follow its referenced conventions, stack docs, and project-specific skills.

Those external files are the portability boundary. To reuse this `.agent` skill pack in a new C++
project, copy `.agent/` and redefine the host project's `context/` files.

## Skill Inventory

| Skill | Use when |
| --- | --- |
| `api-design.md` | Changing public headers, facades, service APIs, callback interfaces, or contracts. |
| `build-system.md` | Changing build configuration, package manager files, generated code, links, or test targets. |
| `code-review.md` | Reviewing a C++ diff. |
| `concurrency.md` | Changing owner contexts, callbacks, async work, timers, locks, shutdown, or race-prone code. |
| `debugging.md` | Investigating compile, link, runtime, crash, hang, or flaky-test failures. |
| `definition-of-done.md` | Checking completion before handoff, review, or final task summary. |
| `design-patterns.md` | Changing abstractions, extension points, collaboration, state, or lifetime models. |
| `implementation.md` | Implementing a feature, bug fix, or behavior-changing C++ edit end to end. |
| `memory-safety.md` | Changing ownership, lifetimes, references, views, callbacks, containers, or cleanup. |
| `module-plan.md` | Turning requirements, design notes, tickets, or module ideas into an implementation plan. |
| `modern-cpp.md` | Changing C++ source, headers, language idioms, types, templates, or standard-library usage. |
| `performance.md` | Changing hot paths, queues, retries, batching, timers, logging/metric-heavy paths, or owner work. |
| `refactoring.md` | Improving C++ structure without intended behavior changes. |
| `security-logging.md` | Changing logs, metrics, requests, identifiers, debug settings, or sensitive data. |
| `testing.md` | Adding, changing, or reviewing C++ tests. |

## What Belongs Here

- General C++ coding procedures.
- General testing, debugging, review, build-system, concurrency, API, performance, and logging
  checklists.
- Instructions that refer to concepts such as "the owner executor", "the build target", or "the
  approved redaction helper", with the concrete names supplied by external project docs.

## What Does Not Belong Here

- Product or domain workflows.
- Concrete target names, service names, component names, endpoint names, environment variables, or
  internal dependency names.
- Organization-specific package remotes, CI jobs, dashboards, or deployment commands.
- Project-specific schema files, metric catalogs, proto paths, or business-layer ownership rules.

Put those in `context/`, external project docs, or project-specific skills outside `.agent`.

## Maintaining This Pack

- Keep each skill task-triggered: start with `Use this when...`.
- Keep each skill project-neutral: no concrete product names, target names, endpoint names, env vars,
  metric names, package remotes, or organization-specific services.
- Refer to host facts through "project context", `CONVENTIONS.md`, and `STACK.md`.
- Prefer checklists and decision rules over tutorials.
- Include stop/ask conditions when a change would alter contracts, ownership, lifecycle, security, or
  production behavior.
- Keep examples shape-only unless they are standard C++ syntax. Mark pseudocode clearly.
- After edits, scan `.agent/` for project-specific names and long duplicated boilerplate.
