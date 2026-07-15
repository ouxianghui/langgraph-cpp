# Skill: Concurrency And Ownership

Use this when changing executors, owner threads, parallel super-steps, channels/streams,
callbacks, timers, cancellation, or shutdown in foundation or langgraph.

Pair with [`.agent/skills/concurrency.md`](../../.agent/skills/concurrency.md) and
[`../../docs/CONCURRENCY_MODEL.md`](../../docs/CONCURRENCY_MODEL.md).

## Critical Distinctions

| Term | Layer | Meaning |
| --- | --- | --- |
| `threadId_` | langgraph checkpoint protocol | Logical LangGraph thread |
| OS thread | foundation | Actual worker |
| `OwnerThread` / `OwnerExecutor` | foundation | Mutable state serial affinity |
| `IExecutor` / `RunOptions::executor_` | graph runtime | Optional node task executor |
| super-step | graph runtime | Batch of ready tasks on one snapshot |

## Graph Runtime Rules

1. `CompiledStateGraph` plan is immutable and shareable.
2. Each invocation owns run-local state, task queues, and stream projection state.
3. With an executor, node handlers may run concurrently on a shared snapshot (or Send branch state).
4. After collection, writes are sorted deterministically; reducer merge updates state **serially**.
5. Checkpoint I/O happens at runtime-defined boundaries after merge (and other pause/fail/complete points).
6. Live streams use bounded channels with explicit close semantics.

Handlers must:

- treat input `State` as immutable;
- return `StateUpdate` / `NodeOutput` / `Command` / `Send`;
- not keep references to `Runtime`, `State`, or `StreamWriter` across async lifetimes;
- leave synchronization of shared external resources to store/checkpointer/tool/adapter/app.

## Foundation Owner-Thread Rules

Use owner affinity for long-lived mutable services:

- HTTP client async queues;
- scheduler;
- GUI bridges;
- hardware/provider SDKs needing thread affinity;
- actor-like background adapters.

Do **not** force the entire graph runtime onto one global owner thread — that kills parallel fan-out
and conflates logical `thread_id` with OS threads.

## New Async Path Checklist

Before merging an async change, document and implement:

- [ ] Which executor/thread runs the callback?
- [ ] Who owns the mutable state?
- [ ] Queue capacity and backpressure / drop / fail policy?
- [ ] Cancellation and shutdown behavior for in-flight work?
- [ ] Error propagation as `Status` / events (nothing swallowed)?
- [ ] No user callback under locks?
- [ ] Tests for throw-in-callback, shutdown-during-work, queue-full, cancellation?

## Verification Focus

```sh
ctest --test-dir build/unix-debug -R "graph|stream|executor|cancel" --output-on-failure
# Prefer TSAN for races on new concurrency paths when available in local CI/presets.
```
