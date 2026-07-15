# Coding And Design Conventions

Distilled from [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md),
[`docs/API_CONTRACT.md`](../docs/API_CONTRACT.md),
[`docs/DEPENDENCY_POLICY.md`](../docs/DEPENDENCY_POLICY.md),
[`docs/CONCURRENCY_MODEL.md`](../docs/CONCURRENCY_MODEL.md),
[`docs/ERROR_MODEL.md`](../docs/ERROR_MODEL.md),
[`docs/PERSISTENCE_MODEL.md`](../docs/PERSISTENCE_MODEL.md),
[`docs/SECURITY_MODEL.md`](../docs/SECURITY_MODEL.md),
[`docs/OWNERSHIP.md`](../docs/OWNERSHIP.md), and accepted ADRs under [`docs/ADR/`](../docs/ADR/README.md).

## 1. Layer Boundaries

| Layer | May depend on | Must not |
| --- | --- | --- |
| Application / examples | public facade + concrete adapters + optional `lgc::core` | treat examples as ABI surface |
| `include/langgraph_cpp` | public module headers | include internal `.hh` or raw `third_party` paths |
| `src/langgraph` | `src/foundation`, ports | depend on `src/core`; hard-wire real providers/hardware as default deps |
| `src/core` (`lgc::core`) | `src/foundation` | depend on `src/langgraph`; pull assembly wiring into foundation |
| `src/foundation` | `third_party` / system libs behind options | include or depend on `src/langgraph` or `src/core` |

Notes:

- `src/langgraph/core/ids.hpp` is part of the langgraph library (ids / `START` / `END`), not the
  `lgc::core` assembly target.
- Layering for `lgc::core` is documented in [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md) and
  enforced by [`docs/DEPENDENCY_POLICY.md`](../docs/DEPENDENCY_POLICY.md) +
  `scripts/check-dependency-policy.sh`.
- Conflict resolution and pinned contract versions: [`AUTHORITY.md`](AUTHORITY.md).

## 2. Ownership Splits

| Concern | Owner |
| --- | --- |
| Process start, credentials, provider choice, hardware permission, tool registration | Application |
| Opinionated service bag + coordinated lifecycle of foundation subsystems | Core assembly (`src/core` / `lgc::core`) |
| Graph execution, state/reducer semantics, checkpoint boundaries, stream envelope, adapter contracts | Runtime (`src/langgraph`) |
| Generic I/O, storage, HTTP, executors, logging, redaction, cancellation, resource limits | Foundation |

Hardware lifecycle ownership stays with the application. Runtime exposes ports; it does not own real
devices or cloud accounts.

## 3. Public API Conventions

- Namespace: `lgc`.
- Primary include: `#include <langgraph_cpp/langgraph.hpp>`.
- Recoverable failures: return `Status` or `Result<T>` (ADR-0003).
- Graph builder: LangGraph-style camelCase only (`addNode`, `addEdge`, `compile`, …).
- Member fields commonly use trailing underscores (`threadId_`, `checkpointer_`).
- Prefer additive public changes; destructive rename/remove requires bumping API contract version,
  deleting old names (no long dual-track aliases), and updating tests/docs (see API_CONTRACT).
- ABI is not stable before 1.0; callers rebuild from source.
- Optional adapter internals (provider, llama.cpp, hardware) are not frozen by the API contract.

Current contract versions: see [`PROJECT_MANIFEST.json`](../PROJECT_MANIFEST.json) and
[`docs/API_CONTRACT.md`](../docs/API_CONTRACT.md).

## 4. State And Execution Conventions

- `StateGraph` declares; `compile()` produces immutable `CompiledStateGraph` (ADR-0006).
- Execution is super-step based (ADR-0001): ready nodes share a snapshot; merge is deterministic.
- Nodes treat input `State` as immutable and return `StateUpdate` / `NodeOutput` / `Command` /
  `Send` (ADR-0005). Never expose mutable global graph state to handlers.
- Parallelism is optional via `RunOptions::executor_` / `maxConcurrency_`; merge and checkpoint stay
  serial at runtime-defined boundaries.
- Checkpoint writes at runtime boundaries, not arbitrary user side-effect boundaries (ADR-0002).
- `Store` ≠ checkpoint history (ADR-0007).

## 5. Concurrency Conventions

| Concept | Meaning |
| --- | --- |
| `threadId_` | Logical LangGraph thread for checkpoint/resume |
| OS thread / `IExecutor` | Actual execution |
| `OwnerThread` / `OwnerExecutor` | Foundation rule: one serial executor owns mutable state |

- Do not put graph invocation state on a global owner thread.
- Use owner-thread patterns for long-lived foundation services (HTTP async queue, scheduler, GUI
  bridge, hardware bindings that require affinity).
- Callbacks: define callback thread, owner, shutdown, cancellation, queue capacity/backpressure, and
  error propagation before merging.
- Do not call user callbacks while holding locks.
- Do not retain references to `Runtime`, `State`, or `StreamWriter` across async lifetime boundaries.

## 6. Error Conventions

- Parsers, codecs, storage, schema, checkpoint, HTTP, tool, model, and runtime APIs must not hide
  failure behind a bare `bool`.
- Never silently ignore checkpoint / pending-writes / storage / schema / cancellation / tool /
  transport errors.
- Node/tool/provider exceptions at designated boundaries convert to `Status` with useful context.
- Future schema versions unread without migration → `StatusCode::Unimplemented`.
- Logs/metrics must not carry secrets (tokens, raw bodies, credentials, raw tool payloads).

## 7. Security Conventions

- Explicit tool registration + schema validation (ADR-0010).
- No built-in privileged shell/file/network/hardware tools by default.
- HTTP request auth via `IAuthorizationProvider`; refreshable creds via `IRefreshableAuthorization`.
- Redact credentials, auth headers, raw request/response bodies, and high-cardinality secret-like
  metric labels.
- External side effects (tools/hardware/providers) need application-owned idempotency, approval, and
  compensation policy; checkpoint restore does not restore the external world.

## 8. Optional Integration Conventions

Behind CMake options and/or injected interfaces (ADR-0008):

| Capability | Typical gate |
| --- | --- |
| OpenSSL crypto | `LANGGRAPH_CPP_WITH_CRYPTO` |
| HTTP (cpp-httplib) | `LANGGRAPH_CPP_WITH_NETWORK` |
| SQLite storage | `LANGGRAPH_CPP_WITH_SQLITE` |
| gzip | `LANGGRAPH_CPP_WITH_GZIP` |
| spdlog | `LANGGRAPH_CPP_WITH_SPDLOG` |
| llama.cpp adapter | `LANGGRAPH_CPP_WITH_LLAMA_CPP` (default OFF) |

Default tests and examples use fake/mock paths.

## 9. Naming And Style (Project)

- Prefer LangGraph-style public names already established in headers; do not revive deleted aliases.
- Node names must not contain `|` or `:` (reserved for checkpoint namespace delimiters).
- Checkpoint namespace: `|` separates subgraph levels; `:` appends per-invocation segments.
- Store API uses `deleteItem` (C++ keyword avoidance for LangGraph `delete`).
- Keep foundation types generic: no graph/node/checkpoint business concepts unless at an explicit
  serializer/adapter boundary.
- Match surrounding file style; no drive-by format-only churn.

## 10. High-Risk Change Checklist

When changing any of the following, update contracts/tests/docs and expect stricter review
([`docs/OWNERSHIP.md`](../docs/OWNERSHIP.md), [`docs/MAINTAINER_GUIDE.md`](../docs/MAINTAINER_GUIDE.md)):

- public headers / API contract version;
- checkpoint, storage, content envelope, or other persisted schemas;
- graph execution, stream projection, interrupt, subgraph, resume/replay semantics;
- new CMake option or optional dependency;
- logging/metrics/auth/redaction;
- tool or hardware external side effects;
- any path that could let `foundation` include `langgraph` or `core`, or let `core` include `langgraph`.
