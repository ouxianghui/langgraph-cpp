# Skill: Stream Envelope And Projection

Use this when changing `stream` / `streamEvents` / `streamProjected` / resume stream variants,
runtime event envelopes, projection options, or stream golden fixtures.

Pair with [`langgraph.md`](langgraph.md), ADR-0009, [`docs/API_CONTRACT.md`](../../docs/API_CONTRACT.md),
and [`docs/API_REFERENCE.md`](../../docs/API_REFERENCE.md). Authority pins:
[`../AUTHORITY.md`](../AUTHORITY.md).

## Role

Streaming is part of the public contract: consumers rely on stable envelopes and projection modes,
not ad-hoc callback payloads.

## API Families

| API | Behavior |
| --- | --- |
| `stream` | Collect runtime events into `RunResult` |
| `streamEvents` / `resumeEvents` | Live event stream (bounded channel + close) |
| `streamProjected` / `resumeProjected` | LangGraph-style projected parts (updates/values/messages/…) |

Key options live on `RunProjectionOptions` / stream options: modes, `outputKeys_`, subgraph filters,
`StreamProtocolVersion` (e.g. V2 typed parts).

## Rules

- Prefer explicit envelopes (`type` / `ns` / `data` or documented projection fields) over bare JSON dumps.
- Live streams must be bounded; document capacity and close/error propagation.
- Interrupt / error / checkpoint visibility must remain consumer-observable on the chosen mode.
- Changing envelope fields or projection defaults is an API contract change — bump pins, update golden
  fixtures under `testdata/langgraph/`, examples (`stream_projection`), and LIMITATIONS if parity shifts.
- Do not claim full Python LangGraph stream parity; see `LANGGRAPH_COMPATIBILITY.md`.

## Coding Checklist

- [ ] Channel capacity / backpressure / close defined.
- [ ] No secret-bearing payloads in default event fields ([`security.md`](security.md)).
- [ ] Fixtures + compat tests updated for shape changes.
- [ ] Subgraph namespace path exposed consistently with [`subgraph.md`](subgraph.md).

## Verification

```sh
ctest --test-dir build/unix-debug -R "stream|compat" --output-on-failure
build/unix-debug/examples/stream_projection
```
