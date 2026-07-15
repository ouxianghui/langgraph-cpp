# Skill: Interrupt And Human-In-The-Loop

Use this when changing interrupts, `Command::resume`, tool-approval loops, multi-interrupt resume
payloads, or HITL examples/tests.

Pair with [`langgraph.md`](langgraph.md), [`persistence.md`](persistence.md),
[`security.md`](security.md), and [`docs/PERSISTENCE_MODEL.md`](../../docs/PERSISTENCE_MODEL.md).

## Role

Interrupts pause a logical thread before unsafe or human-gated work proceeds. Resume continues from
the latest checkpoint for that thread (+ namespace) with explicit resume payloads.

## Mechanisms

| Mechanism | Notes |
| --- | --- |
| Node-returned interrupt | Via `NodeOutput` / command path |
| `Runtime::interrupt(id, payload)` | Function-style interrupt |
| `Command::resume` | Resume payload keyed by interrupt id and/or node id |
| Multi-interrupt | Same super-step may collect multiple; resume may supply multiple payloads |
| Sequential multi-interrupt | Already-resumed values can be checkpointed and replayed next attempt |

## Rules

- Resume requires a checkpointer and a stable logical `threadId_` (not an OS thread).
- Dangerous tools should prefer interrupt/approval instead of silent execution ([`security.md`](security.md)).
- Interrupt pause writes a checkpoint; do not invent “soft pause” without persistence if resume is claimed.
- `StatusCode::Aborted` used for interrupt control flow must still produce observable interrupt
  event/checkpoint (`ERROR_MODEL`).
- External side effects stay application-owned: resume restores graph state, not the external world.

## Coding Checklist

- [ ] Interrupt ids stable enough for resume mapping.
- [ ] Pending writes / failure interaction documented when mixed with HITL.
- [ ] Examples (`human_interrupt`, `tool_approval_loop`) and tests cover happy + missing resume paths.

## Verification

```sh
ctest --test-dir build/unix-debug -R "graph|compat|interrupt" --output-on-failure
build/unix-debug/examples/human_interrupt
build/unix-debug/examples/tool_approval_loop
```
