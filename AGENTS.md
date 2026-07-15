# Codex Agent Context

This file is the entrypoint for Codex working in this repository.

## Default Skill Policy

This repository uses two skill layers. Agents **must** load both when editing library code:

| Layer | Path | Role |
| --- | --- | --- |
| Process skills | `.agent/` | Project-neutral C++ workflow (implement, review, test, …). |
| Project context | `context/` | langgraph-cpp conventions and library skills (foundation / core / langgraph). |

### Minimal Load Policy (cost control)

Prefer constraining how much you read over deleting skills from the pack.

1. **Smallest skill set only** — load the one routing row that matches the touched layer/topic. Do
   not browse or preload the full `context/skills/` directory.
2. **Authority first** — for library edits: skim `context/AUTHORITY.md` and the relevant sections of
   `context/CONVENTIONS.md` (and `STACK.md` only if build/options matter) **before** opening specialty
   skills such as stream, subgraph, HITL, provider-http, or performance.
3. **Simple-change budget** — for a focused bugfix or single-module edit, default to **≤2**
   `context/skills/*.md` and **≤2** `.agent/skills/*.md` (plus `definition-of-done` at handoff).
   Do not open five or more context skills “just in case”.
4. **Progressive disclosure** — open a specialty skill only when the diff actually touches that
   concern (e.g. stream skill only if stream/projection changes).
5. **Never full-pack reads** — do not concatenate README inventory + every skill into context.

### At the start of a task

1. Read `.agent/README.md` only as needed to pick process skills (do not load all of them).
2. Read `context/README.md` **or** `context/AGENTS.md` for routing (not both fully unless needed).
3. Read `context/AUTHORITY.md` when contracts, schemas, or contested semantics are involved;
   otherwise skip until a conflict appears.
4. Classify the task and pick the **single best** routing row.
5. Read that minimal matching `.agent/skills/*.md` and `context/skills/*.md` set before editing.
6. Use `.agent/skills/definition-of-done.md` before final handoff.
7. When changing `context/` or contract docs, run `scripts/check-context-skills.sh` (and
   `scripts/check-dependency-policy.sh` when layering changes).

## Project Identity

`langgraph-cpp` is a C++23 native client and edge intelligent workflow runtime for AI Labs. The MVP centers on:

- LangGraph-style graph execution with `StateGraph` and `CompiledStateGraph`.
- JSON-backed state and field reducers.
- Thread-scoped checkpoints and resume.
- Runtime events, streaming helpers, interrupt, and `Command::resume`.
- Message, model, tool, and mock edge workflow adapters.
- Foundation services for storage, events, threading, executors, scheduler, logging, networking,
  serialization, status/result, and resource limits.
- Optional application assembly via `src/core` (`lgc::core`: `RuntimeServices` / `RuntimeContainer`).

The core runtime must build and test without Python, real hardware, real cloud model providers, or
an external llama.cpp setup.

## Required Reading Order

Before editing code, read:

1. `README.md` for user-facing scope, build commands, and examples.
2. `docs/AI_INDEX.md` for the current design evidence chain.
3. `docs/PRD.md` for product and MVP requirements.
4. `docs/ROADMAP.md` and `docs/internal/WBS.md` for staged implementation intent.
5. `docs/LIMITATIONS.md` for current boundaries and deferred work.
6. `.agent/README.md` for the reusable process skill inventory.
7. `context/AGENTS.md`, `context/AUTHORITY.md`, `context/CONVENTIONS.md`, and `context/STACK.md`.

Then load task-specific skills from `.agent/skills/` and `context/skills/` before planning or editing.

## Skill Routing

### Process skills (`.agent/`)

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

### Project library skills (`context/`)

| Task | Skills to read |
| --- | --- |
| `src/foundation/**` | `context/skills/foundation.md`, `context/skills/coding-standards.md` |
| `src/core/**` / RuntimeServices / RuntimeContainer | `context/skills/core.md`, `context/skills/coding-standards.md` |
| `src/langgraph/**` | `context/skills/langgraph.md`, `context/skills/coding-standards.md` |
| Stream / projection / envelopes | `context/skills/stream-projection.md` |
| Subgraph / checkpoint namespace | `context/skills/subgraph.md` |
| Interrupt / HITL / Command::resume | `context/skills/hitl-interrupt.md` |
| Provider / IHttpClient / auth | `context/skills/provider-http.md` |
| Graph / stream concurrency | `context/skills/langgraph.md`, `context/skills/concurrency.md` |
| Checkpoint / store / storage / codecs | `context/skills/persistence.md` |
| Tools / redaction / edge | `context/skills/security.md` |
| Tests / examples authorship | `context/skills/testing-examples.md` |
| Hot paths / limits / perf claims | `context/skills/performance.md` |

Full routing: [`context/AGENTS.md`](context/AGENTS.md). Authority and version pins:
[`context/AUTHORITY.md`](context/AUTHORITY.md).

When skills conflict with each other or with drafts, follow `context/AUTHORITY.md`. Explicit user
instructions still override process skills for the current task, but must not silently invent
contracts that contradict pinned docs/source.

## Repository Map

- `include/langgraph_cpp/langgraph.hpp`: public aggregate header.
- `src/langgraph/graph`: graph declaration, compile, invoke, stream, resume.
- `src/langgraph/state`: state updates and reducers.
- `src/langgraph/checkpoint`: checkpointer interfaces and adapters.
- `src/langgraph/runtime`: runtime context and stream writer.
- `src/langgraph/message`: portable message and tool-call JSON format.
- `src/langgraph/model`: chat model interface, mock model, optional llama.cpp adapter.
- `src/langgraph/tool`: tool registry, executor, policies, structured tool results, GBNF helpers.
- `src/langgraph/edge`: hardware adapter interfaces.
- `src/langgraph/core`: id aliases and `START` / `END` only (not `lgc::core`).
- `src/core`: `lgc::core` assembly — `RuntimeServices`, `RuntimeContainer`, lifecycle factories.
- `src/foundation`: reusable infrastructure.
- `tests`: unit and component tests.
- `examples`: runnable examples used as acceptance signals.
- `docs`: AI index, PRD, roadmap, work breakdown, architecture, API contract,
  quality model, traceability matrix, test catalog, compatibility, security,
  performance, risk, examples, release checklist, and known limitations.
- `context`: project-specific agent conventions and library skills for foundation / core / langgraph.
- `third_party`: vendored dependencies. Do not edit unless explicitly requested.
- `build`: generated build output. Do not treat as source.

## Build And Test Commands

Primary validation:

```sh
cmake --preset unix-debug
cmake --build --preset unix-debug
ctest --test-dir build/unix-debug --output-on-failure
```

Architecture / context gates:

```sh
scripts/check-dependency-policy.sh
scripts/check-context-skills.sh
```

Helper:

```sh
./scripts/build.sh
```

Focused example commands:

```sh
build/unix-debug/examples/minimal_graph
build/unix-debug/examples/sqlite_checkpoint_resume
build/unix-debug/examples/model_tool_model_loop
build/unix-debug/examples/human_interrupt
build/unix-debug/examples/mock_edge_repair
```

Optional llama.cpp examples require `LANGGRAPH_CPP_WITH_LLAMA_CPP=ON`, an external llama.cpp setup,
and a GGUF model. Do not make them part of the default validation gate.

## Non-Negotiable Rules

- Preserve C++23 and CMake-based builds.
- Keep the graph runtime independent of real model providers and real hardware libraries.
- Keep `src/core` (`lgc::core`) as foundation-only assembly; it must not depend on `src/langgraph`.
- `src/foundation` must not depend on `src/langgraph` or `src/core`.
- `src/langgraph` must not depend on `src/core` (`lgc::core`).
- Public APIs should return `Status` or `Result<T>` for recoverable errors.
- Do not silently swallow checkpoint, storage, schema validation, cancellation, or tool execution
  errors.
- Nodes must update graph state through `StateUpdate`; do not expose mutable global state to nodes.
- Tools must be explicitly registered and validate inputs before invoking callables.
- Dangerous shell, filesystem, network, or hardware tools are not built in by default.
- Optional dependencies must remain behind CMake options.
- Avoid unrelated refactors, formatting churn, and generated/build artifact edits.

## Definition Of Done

Before final response or handoff:

- Run the most focused test or command for the change.
- Run broader validation when changing public headers, runtime semantics, storage, threading,
  lifecycle, build files, or shared foundation code.
- Update docs/examples when behavior, commands, limitations, or public contracts change.
- When changing `context/` skills or authority pins, run `scripts/check-context-skills.sh`.
- State exactly what was run and what was not run.
- Use `.agent/skills/definition-of-done.md` as the final checklist.
