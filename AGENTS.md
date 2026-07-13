# Codex Agent Context

This file is the entrypoint for Codex working in this repository.

## Default `.agent` Skill Policy

Codex must use `.agent/` as the default skill registry for this repository.

At the start of a task:

1. Read `.agent/README.md` to discover the available skills.
2. Classify the task.
3. Read the matching `.agent/skills/*.md` files before planning or editing.
4. Use `.agent/skills/definition-of-done.md` before final handoff.

Do not load every skill file by default. Always load the task-specific skills that match the current
request.

## Project Identity

`langgraph-cpp` is a C++23 native client and edge intelligent workflow runtime for AI Labs. The MVP centers on:

- LangGraph-style graph execution with `StateGraph` and `CompiledStateGraph`.
- JSON-backed state and field reducers.
- Thread-scoped checkpoints and resume.
- Runtime events, streaming helpers, interrupt, and `Command::resume`.
- Message, model, tool, and mock edge workflow adapters.
- Foundation services for storage, events, threading, executors, scheduler, logging, networking,
  serialization, status/result, and resource limits.

The core runtime must build and test without Python, real hardware, real cloud model providers, or
an external llama.cpp setup.

## Required Reading Order

Before editing code, read:

1. `README.md` for user-facing scope, build commands, and examples.
2. `docs/AI_INDEX.md` for the current design evidence chain.
3. `docs/PRD.md` for product and MVP requirements.
4. `docs/ROADMAP.md` and `docs/internal/WBS.md` for staged implementation intent.
5. `docs/LIMITATIONS.md` for current boundaries and deferred work.
6. `.agent/README.md` for the reusable skill inventory.

Then load task-specific skills from `.agent/skills/` before planning or editing.

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

When multiple skills apply, read the smallest useful set. If a skill conflicts with explicit user
instructions or repository-local code patterns, the user request and nearby code win.

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
- `src/core`: runtime service container and lifecycle adapters.
- `src/foundation`: reusable infrastructure.
- `tests`: unit and component tests.
- `examples`: runnable examples used as acceptance signals.
- `docs`: AI index, PRD, roadmap, work breakdown, architecture, API contract,
  quality model, traceability matrix, test catalog, compatibility, security,
  performance, risk, examples, release checklist, and known limitations.
- `third_party`: vendored dependencies. Do not edit unless explicitly requested.
- `build`: generated build output. Do not treat as source.

## Build And Test Commands

Primary validation:

```sh
cmake --preset unix-debug
cmake --build --preset unix-debug
ctest --test-dir build/unix-debug --output-on-failure
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
- Keep the core runtime independent of real model providers and real hardware libraries.
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
- State exactly what was run and what was not run.
- Use `.agent/skills/definition-of-done.md` as the final checklist.
