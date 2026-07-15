# Skill: Foundation Library

Use this when editing or consuming `src/foundation/**`, or when wiring storage, HTTP, executors,
logging, serialization, cancellation, crypto, or redaction into the runtime or an application.

Read [`../CONVENTIONS.md`](../CONVENTIONS.md) and [`../STACK.md`](../STACK.md) first. Module map:
[`../../src/foundation/README.md`](../../src/foundation/README.md).

## Role

Foundation is **reusable infrastructure**. It must stay free of graph/node/checkpoint business
concepts except at explicit serializer/adapter boundaries.

Hard rules: **never** `#include` or link toward `src/langgraph` or `src/core`. Opinionated
multi-subsystem wiring belongs in [`core.md`](core.md) (`lgc::core`), not in foundation.

## Subsystems

| Area | Typical types / dirs | Programming rules |
| --- | --- | --- |
| Errors | `status/` → `Status`, `Result<T>` | Recoverable failures return Status/Result; do not hide with bare bool |
| Async | `async/` channels/futures | Bound queues; explicit close; define backpressure |
| Execution | `executor/`, `threading/` | Owner executor for mutable state; document callback thread |
| Storage | `storage/` → `IStorage`, memory/SQLite | Schema mismatch / busy / corruption → explicit Status |
| Serialization | `serialization/` | Cover malformed input, size limits, future versions |
| Network | `network/` HTTP/SSE + auth providers | Inject auth; redact secrets; require `HttpRequestOptions` |
| Observability | `observability/`, `event/` | Structured, low-sensitivity fields; redaction wrappers |
| Redaction | `redaction/` | Wrap logger/event/trace/codec paths that may carry secrets |
| Crypto | `crypto/` | Behind `LANGGRAPH_CPP_WITH_CRYPTO` |
| Limits / cancel | `cancellation/`, resource limits | Cooperative cancel; enforce limits on hot I/O paths |
| FS / blob / cache / scheduler / process | respective dirs | Explicit shutdown; no process-exit-only cleanup |

## Usage Patterns

1. **Depend on interfaces in langgraph / apps**  
   Prefer `IStorage`, `IHttpClient`, `IExecutor`, `IAuthorizationProvider`, event sinks — inject
   concrete impls at the edge (examples, app main).

2. **Lifecycle**  
   Long-lived services expose close/shutdown. Document what happens to pending work, queued
   callbacks, and in-flight I/O.

3. **Owner thread**  
   Use for HTTP async queues, schedulers, GUI bridges, hardware SDK affinity. Do not force graph
   runs onto a single global owner thread (see [`concurrency.md`](concurrency.md)).

4. **Callbacks**  
   Never invoke user callbacks while holding locks. Capture `weak_ptr` across delayed work unless
   the owner intentionally keeps itself alive.

5. **Optional deps**  
   Gate network/SQLite/crypto/gzip/spdlog behind the CMake options in [`STACK.md`](../STACK.md).
   llama.cpp linking must not leak into foundation targets.

## Coding Checklist

- [ ] No include path into `src/langgraph` or `src/core`.
- [ ] Public-ish APIs return `Status` / `Result<T>` for recoverable errors.
- [ ] Async API documents owner, shutdown, queue capacity, cancellation, error propagation.
- [ ] Parser/codec has tests for success, malformed input, future version, size limit.
- [ ] Logging/metrics path redacts credentials, auth headers, raw bodies, raw tool-like payloads.
- [ ] New optional third-party use has a CMake option and dependency-policy coverage.
- [ ] Tests do not require real cloud credentials or real hardware.

## Verification Focus

```sh
scripts/check-dependency-policy.sh
ctest --test-dir build/unix-debug -R "storage|http|status|redaction|executor|serialization" --output-on-failure
```

Broader gate when changing shared foundation used by graph runtime: full `ctest` under
`build/unix-debug`.

## Authoritative Docs

- [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) — foundation vs runtime layering
- [`docs/ERROR_MODEL.md`](../../docs/ERROR_MODEL.md)
- [`docs/CONCURRENCY_MODEL.md`](../../docs/CONCURRENCY_MODEL.md)
- [`docs/SECURITY_MODEL.md`](../../docs/SECURITY_MODEL.md)
- [`docs/DEPENDENCY_POLICY.md`](../../docs/DEPENDENCY_POLICY.md)
- ADR-0003, ADR-0008
