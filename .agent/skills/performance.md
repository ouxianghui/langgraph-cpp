# Skill: Performance

Use this when changing hot paths, queues, retries, batching, timers, logging/metric-heavy paths, or
owner-context work in a C++ project.

Read project context first, as defined in `../README.md`.

Correctness and ownership come before optimization.

## Measure or Bound First

- Identify the path: request handling, lifecycle, event flow, networking, storage, logging, metrics,
  generated-code adapter, or test-only code.
- State the performance risk: latency, throughput, memory, queue growth, lock contention, retry storm,
  or owner-context starvation.
- Prefer measurement, profiling, or a focused benchmark before changing architecture for speed.
- If measurement is not practical, at least bound queue sizes, retries, payload sizes, and log volume.

## Workload, Baseline, and Budget

- Define the workload, input size, concurrency level, and relevant latency, throughput, or memory
  budget before optimizing.
- Prefer explicit p50, p95, p99, throughput, CPU, memory, or queue-depth targets over "make it faster".
- Record before/after baselines when measurements are available.
- Avoid architecture-level optimization without a concrete target or observed bottleneck.

## Owner Context Work

- Do not block owner/event-loop contexts on network I/O, long sleeps, large serialization, or heavy
  regex/hash loops.
- Move expensive work outside locks and outside owner-critical sections.
- Do not turn owner executors into general-purpose worker pools.
- Keep high-volume callbacks small and hand off only the data needed by the owner.

## Contention, Caching, and Shared State

- Account for lock contention, shared caches, atomic counters, global registries, and cross-thread
  handoffs in high-frequency paths.
- New caches need capacity, invalidation, ownership, lifetime, and synchronization rules.
- Do not trade correctness or unbounded memory growth for a faster steady-state path.

## Hot Path Logging and Metrics

- Avoid heavy regex, hashing, serialization, or large string construction in high-frequency paths
  unless the diagnostic value is worth the cost.
- Keep high-frequency logs at appropriate levels and with bounded fields.
- Do not log raw or huge payloads.
- Metric labels must be low-cardinality. Never use IDs, URLs, tokens, raw errors, or bodies as
  labels.
- Prefer counters/histograms from the existing catalog/registry over ad-hoc instrumentation.

## Queues, Retries, and Batches

- Queues must have a bounded growth story: capacity, backpressure, drop policy, or shutdown drain.
- Retries must have a limit, delay/backoff, and observable failure path.
- Batches must have max size and max age.
- Timers must be cancellable and safe during shutdown.
- Avoid retry loops that run on owner contexts without yielding or delay.

## Algorithmic Complexity and Data Shape

- Check complexity against real and worst-case data sizes, not only small examples.
- Watch for quadratic loops, full scans, sorting, regex, serialization/deserialization, hashing, and
  string concatenation in repeated paths.
- Prefer changing the data shape or ownership boundary when it removes repeated work without obscuring
  correctness.

## Allocation and Copying

- Avoid repeated allocation in loops when a small local reuse is simple and safe.
- Move payloads across async boundaries when ownership transfers.
- Copy intentionally when it simplifies lifetime or avoids shared mutable state.
- Do not introduce borrowed views to avoid a copy if the data crosses a lifetime boundary.

## Measurement Integrity and Regression Guard

- Report the command, build configuration, dataset, warmup, repeat count, and known noise when sharing
  measurements.
- Distinguish debug, release, sanitizer, and instrumented builds before comparing performance numbers.
- After optimization, run correctness gates first, then add or update a benchmark, performance-sensitive
  test, metric, or bound that can catch regressions.

## Before Finishing

- Verify correctness tests first.
- Run a targeted performance-sensitive test, benchmark, or representative command when available.
- State what was measured or what bound was added.
- Do not weaken logging, metrics, or safety checks for performance without explicit approval.
