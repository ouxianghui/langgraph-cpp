# Skill: Core Assembly Library (`src/core` / `lgc::core`)

Use this when editing or consuming `src/core/**`, or when wiring application-level lifecycle and
foundation service bags (`RuntimeServices`, `RuntimeContainer`, lifecycle component factories).

Read [`../CONVENTIONS.md`](../CONVENTIONS.md), [`../STACK.md`](../STACK.md), and
[`../AUTHORITY.md`](../AUTHORITY.md) first. Pair with [`foundation.md`](foundation.md) and
[`concurrency.md`](concurrency.md).

## Do Not Confuse With `langgraph/core`

| Path | Target | Role |
| --- | --- | --- |
| `src/core/**` | CMake `core` / `lgc::core` | Application-assembly: service bag + lifecycle wiring |
| `src/langgraph/core/ids.hpp` | part of `langgraph` | Graph id aliases and `START` / `END` constants only |

This skill covers **`src/core` / `lgc::core` only**. For `START` / `END` / node ids, use
[`langgraph.md`](langgraph.md).

## Role

`core` is an **opinionated composition root** kept out of foundation on purpose (see
[`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md)):

- Foundation stays a set of decoupled primitives (`IStorage`, `IExecutor`, `Lifecycle`, …).
- `core` intentionally couples many subsystems into ready-to-use runtime service bundles and
  typed `ILifecycle` adapters.
- Graph runtime (`src/langgraph`) links to `lgc::foundation` directly; it does **not** require
  `lgc::core`. Apps may use `lgc::core` when they want assembled services + coordinated start/close.

Dependency direction (enforced by `scripts/check-dependency-policy.sh`):

```text
Application
  -> lgc::core          (optional assembly)
  -> lgc::langgraph     (graph runtime; depends on foundation, not on core)
  -> lgc::foundation
```

Hard rules:

- `src/core` may depend on `src/foundation`.
- `src/core` must **not** depend on `src/langgraph`.
- `src/foundation` must **not** depend on `src/core`.
- `src/langgraph` must **not** depend on `src/core`.

`lgc::core` types are installed but are **not** part of the numbered `langgraph.hpp` API contract
unless `docs/API_CONTRACT.md` / `AUTHORITY.md` pins say otherwise.

## Modules

| Path | Types | Responsibility |
| --- | --- | --- |
| `runtime/runtime_services.hpp` | `RuntimeServiceRequirements`, `RuntimeServices` | Declare / validate injected foundation services; build a `Lifecycle` from present services |
| `runtime/runtime_container.hpp` | `RuntimeContainer`, `createRuntimeContainer` | Own services + lifecycle; `validate` / `start` / `waitIdle` / `close` |
| `lifecycle/lifecycle_components.hpp` | `makeLifecycleComponent(...)` | Typed factories adapting concrete foundation objects to `ILifecycle` |

### `RuntimeServiceRequirements`

- `core()` — default required set (logger, storage, executor, scheduler, secrets; optional flags
  off for blob/cache/HTTP/events/metrics/trace unless set).
- `all()` — require every known service slot.
- Apps may tweak bools before `validate()`.

### `RuntimeServices`

Shared pointers for logger, storage, blob, cache, executor, scheduler, HTTP, secrets, event sink,
metrics, trace sink.

- `validate(requirements)` → `FailedPrecondition` if a required service is missing or already closed.
- `createLifecycle()` → registers lifecycle components for **present** closeable services.
- `defaultRuntimeServices()` → process-local defaults suitable for tests (memory storage, concurrent
  executor, in-memory sinks, **no** HTTP client by default).

### `RuntimeContainer`

Non-copyable/movable owner:

1. `createRuntimeContainer(services, options)` validates then constructs.
2. `start()` starts the shared `Lifecycle`.
3. `waitIdle(timeout)` drains idle according to lifecycle semantics.
4. `close(options)` / destructor path shut down owned lifecycle; afterward `validate()` fails.

Prefer container + `Lifecycle` for app integration; do not scatter raw timer/thread objects beside it
when the same concern already has an executor/scheduler.

### Lifecycle component factories

`makeLifecycleComponent(name, shared_ptr<T>)` adapters exist for executor, scheduler, event sink,
storage, metrics, trace sink, and HTTP client. They live in `core` (not foundation) so foundation’s
`Lifecycle` core stays free of those concrete subsystem headers.

## Usage Patterns

1. **Tests / simple apps** — `defaultRuntimeServices()` + `createRuntimeContainer`, or custom
   `RuntimeServices` with memory/inline executors.
2. **Production apps** — build `RuntimeServices` with real storage/HTTP/logger sinks, set
   `RuntimeServiceRequirements`, validate, then own via `RuntimeContainer`.
3. **Graph apps** — inject individual foundation services into `RunOptions` / adapters as needed;
   use `lgc::core` only when you also want coordinated process-level lifecycle of those services.
4. **Do not** move opinionated multi-subsystem wiring back into foundation “for convenience”.

## Coding Checklist

- [ ] No include/link toward `src/langgraph`.
- [ ] Missing/closed required services fail with clear `Status` (`FailedPrecondition`), not assert-only
      in library code.
- [ ] New service slots update: `RuntimeServices`, requirements, `validate`, `createLifecycle` /
      factories, defaults, and tests.
- [ ] Close/shutdown order stays in `Lifecycle`; do not invent a second ad-hoc shutdown graph.
- [ ] Defaults stay offline-friendly (no real cloud HTTP client required by `core()`).
- [ ] Secrets / logger slots respect redaction conventions ([`security.md`](security.md)).

## Verification Focus

```sh
ctest --test-dir build/unix-debug -R "runtime_services|lifecycle" --output-on-failure
scripts/check-dependency-policy.sh
```

Related sources: `tests/runtime_services_test.cpp`, `tests/lifecycle_test.cpp`.

## Authoritative Docs

- [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) — `src/core` / `lgc::core` layer
- [`docs/DEPENDENCY_POLICY.md`](../../docs/DEPENDENCY_POLICY.md) — enforceable edges
- [`docs/OWNERSHIP.md`](../../docs/OWNERSHIP.md) — ownership / review gates
- [`docs/TEST_CATALOG.md`](../../docs/TEST_CATALOG.md) — lifecycle / runtime_services entries
- [`docs/CONCURRENCY_MODEL.md`](../../docs/CONCURRENCY_MODEL.md) — shutdown / owner expectations
- [`docs/ERROR_MODEL.md`](../../docs/ERROR_MODEL.md) — Status propagation
