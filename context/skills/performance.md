# Skill: Performance Boundaries

Use this when changing hot paths (scheduling, merge, stream channels, checkpoint I/O, HTTP retries),
resource limits, or any docs/README claims about latency/throughput.

Pair with [`docs/PERFORMANCE_MODEL.md`](../../docs/PERFORMANCE_MODEL.md),
[`concurrency.md`](concurrency.md), and [`.agent/skills/performance.md`](../../.agent/skills/performance.md).

## Principles

- Correctness, recovery, and security beat optimization.
- Define workload + risk before optimizing.
- Queues/retries/streams/batches need capacity, backpressure, or fail policy.
- Do not trade clear lifetimes for a copy avoided.
- Never mix debug/sanitizer/release numbers.

## Hot Paths And Controls

| Path | Control |
| --- | --- |
| Infinite loops / step growth | `ResourceLimits` / recursion limit |
| Fan-out explosion | `RunOptions::maxConcurrency_`, routing discipline |
| Slow stream consumers | bounded `RunStreamOptions::capacity_` |
| Checkpoint history growth | saver `prune` / retention |
| HTTP retry storms | request retry policy, rate limits |

## Forbidden “Optimizations”

- Skipping reducer/schema/checkpoint error handling for speed.
- Logging raw external payloads “for debug”.
- Running network I/O / huge serialize / long sleep on owner threads.
- Unbounded queues to hide backpressure.
- Borrowed views across async boundaries to avoid copies.

## Claim Discipline

- No published latency/throughput targets until benchmark suite exists (`PERFORMANCE_MODEL` §5).
- Do not document future benchmark commands as if targets already exist.
- Optional provider network performance is outside the default gate.

## Verification

```sh
ctest --test-dir build/unix-debug -R "graph|stream|checkpoint|http" --output-on-failure
# Prefer TSAN for new concurrency/hot-path races when presets allow.
```
