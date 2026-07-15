# langgraph-cpp Project Context

Entrypoint for project-specific agent guidance. Pair with the generic process skills in
[`.agent/`](../.agent/README.md). Authority and version pins: [`AUTHORITY.md`](AUTHORITY.md).

## Project Identity

`langgraph-cpp` is a C++23 native client / edge intelligent workflow runtime for AI Labs:

- LangGraph-style graph execution (`StateGraph` → `CompiledStateGraph`);
- JSON-backed state and field reducers;
- thread-scoped checkpoint / resume / replay;
- runtime events, streaming, interrupt, `Command`;
- message / model / tool / edge adapter ports;
- foundation infrastructure (status, storage, HTTP, executors, logging, …);
- optional `src/core` (`lgc::core`) service assembly.

It is **not** an official LangGraph / LangChain C++ port and **not** a hosted agent platform.

Graph runtime must build and test without Python, real hardware, real cloud providers, or external
llama.cpp.

## Minimal Load Policy

Prefer constraining reads over shrinking the skill pack.

1. **One routing row** from the table below — usually a layer skill + `coding-standards.md`.
2. **Authority / conventions first** — skim [`AUTHORITY.md`](AUTHORITY.md) and only the relevant
   sections of [`CONVENTIONS.md`](CONVENTIONS.md) before specialty skills.
3. **Simple-change budget** — focused single-module work: **≤2** files under `skills/`. Do not open
   five or more context skills “for completeness”.
4. **Specialty on demand** — stream / subgraph / HITL / provider-http / performance only when the
   change actually touches those surfaces.
5. **No full-directory load** — never read every file in `skills/` as a preamble.

## Required Context Order

Before editing library code (progressive, not mandatory full read):

1. [`AUTHORITY.md`](AUTHORITY.md) — when contracts, schemas, or contested semantics apply.
2. Relevant sections of [`CONVENTIONS.md`](CONVENTIONS.md) — not necessarily the whole file.
3. [`STACK.md`](STACK.md) — only if CMake/options/commands matter.
4. Matching **minimal** [`skills/`](skills/) file(s) for the touched layer.
5. Module README or nearby headers under `src/foundation/`, `src/core/`, or `src/langgraph/`.
6. Authoritative `docs/` linked from the skill when details are needed (do not invent contracts).

Keep [`docs/AI_INDEX.md`](../docs/AI_INDEX.md) as a lookup index — open specific docs on demand.

## Project Skill Routing

| Task | Context skills | Also load from `.agent/` |
| --- | --- | --- |
| Work in `src/foundation/**` | `skills/foundation.md`, `skills/coding-standards.md` | `modern-cpp.md`, plus concern-specific skills |
| Work in `src/core/**` | `skills/core.md`, `skills/coding-standards.md` | `design-patterns.md`, `concurrency.md`, `testing.md` |
| RuntimeServices / RuntimeContainer / lifecycle wiring | `skills/core.md`, `skills/foundation.md`, `skills/concurrency.md` | `concurrency.md`, `memory-safety.md` |
| Work in `src/langgraph/**` | `skills/langgraph.md`, `skills/coding-standards.md` | `implementation.md` / `api-design.md` as needed |
| Stream / projection / envelopes | `skills/stream-projection.md` | `api-design.md`, `testing.md` |
| Subgraph / checkpoint namespace | `skills/subgraph.md`, `skills/persistence.md` | `design-patterns.md`, `testing.md` |
| Interrupt / HITL / resume | `skills/hitl-interrupt.md` | `testing.md` |
| Provider / HTTP / auth | `skills/provider-http.md`, `skills/security.md` | `security-logging.md`, `testing.md` |
| Graph / state / Command | `skills/langgraph.md`, `skills/concurrency.md` | `design-patterns.md`, `testing.md` |
| Checkpoint / store / storage / serializers | `skills/persistence.md`, `skills/foundation.md` | `design-patterns.md`, `memory-safety.md`, `testing.md` |
| Executors, channels, owner thread, shutdown | `skills/concurrency.md`, `skills/foundation.md` | `concurrency.md`, `memory-safety.md` |
| Tools, redaction, edge adapters | `skills/security.md` | `security-logging.md`, `testing.md` |
| Tests / examples authorship | `skills/testing-examples.md` | `testing.md` |
| Hot paths / perf claims | `skills/performance.md` | `performance.md` |
| Public headers / contract version | `skills/coding-standards.md`, `AUTHORITY.md` | `api-design.md`, `modern-cpp.md` |
| CMake / optional deps | `STACK.md`, `skills/coding-standards.md` | `build-system.md` |
| Context pack maintenance | `SKILL_INVENTORY.md`, `REVIEW_CHECKLIST.md` | `definition-of-done.md` |

Load the **smallest useful set** (see Minimal Load Policy). Resolve conflicts with
[`AUTHORITY.md`](AUTHORITY.md).

## Repository Map (Libraries)

```text
Application
  -> include/langgraph_cpp/langgraph.hpp   (public facade)
  -> src/langgraph/**                     (business runtime; depends on foundation)
  -> src/core/**                          (optional service assembly / lifecycle; depends on foundation)
  -> src/foundation/**                    (reusable infrastructure)
  -> third_party/**                       (vendored; do not edit unless asked)
```

| Path | Role |
| --- | --- |
| `include/langgraph_cpp/langgraph.hpp` | Public aggregate header (`lgc` namespace). |
| `src/langgraph/graph` | Builder, compile, invoke/stream/resume/replay. |
| `src/langgraph/state` | `StateUpdate`, reducers, merge. |
| `src/langgraph/checkpoint` | Saver contract, memory/storage savers. |
| `src/langgraph/store` | Long-term namespaced memory (not checkpoint history). |
| `src/langgraph/runtime` | Per-node `Runtime`, stream writer, interrupt. |
| `src/langgraph/message` / `model` / `tool` / `edge` | Message, model ports, tools, hardware adapters. |
| `src/langgraph/core` | Id aliases and `START` / `END` only (not `lgc::core`). |
| `src/core/**` | `RuntimeServices`, `RuntimeContainer`, lifecycle component factories (`lgc::core`). |
| `src/foundation/**` | Status/result, storage, HTTP, executors, logging, … |

## Non-Negotiable Rules

- Preserve C++23 and CMake-based builds.
- `src/foundation` must never depend on `src/langgraph` or `src/core`.
- `src/core` may assemble foundation services; it must never depend on `src/langgraph`.
- `src/langgraph` must never depend on `src/core`.
- Recoverable errors use `Status` / `Result<T>`; do not swallow checkpoint, storage, schema,
  cancellation, tool, or transport failures.
- Nodes mutate graph state only via `StateUpdate` or `Command`.
- Tools are explicitly registered and input-validated; no default privileged shell/file/network/hardware tools.
- Optional integrations (SQLite, network, crypto, gzip, spdlog, llama.cpp, providers, hardware) stay
  behind CMake options or injected ports.
- Do not treat `threadId_` as an OS thread; it is a LangGraph-style logical thread.
- Do not confuse `Store` (long-term memory) with `Checkpoint` (execution history).
- Do not treat roadmap / planned items as implemented; check [`docs/LIMITATIONS.md`](../docs/LIMITATIONS.md).
- Avoid unrelated refactors, formatting churn, and edits under `third_party/` or `build/`.

## Definition Of Done

Use [`.agent/skills/definition-of-done.md`](../.agent/skills/definition-of-done.md), then:

- run the most focused test or example for the change;
- run broader gates when touching public headers, runtime semantics, storage, threading, lifecycle,
  build files, or shared foundation;
- update docs/examples when contracts, commands, or limitations change;
- when changing `context/` or pins, run `scripts/check-context-skills.sh`;
- state exactly what was run and what was not.
