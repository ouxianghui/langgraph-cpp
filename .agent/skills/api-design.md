# Skill: API Design

Use this when changing public headers, facades, service APIs, callback interfaces, generated API
types, or other contracts in a C++ project.

Read project context first, as defined in `../README.md`.

## Boundary First

- Identify which layer owns the API: public facade, internal component, transport/wire adapter,
  infrastructure utility, generated interface, or test helper.
- Keep lower-level libraries free of higher-level domain concepts, as defined by `CONVENTIONS.md`.
- Route through existing facades and owners instead of exposing internal mutable state.
- Distinguish immediate command acceptance from eventual completion when the operation is async.

## Public Naming

- Public class, interface, method, callback, and field names must be concise, professional, and
  stable.
- Use project domain vocabulary consistently; do not introduce synonyms for established concepts.
- Prefer names that describe the contract exposed to callers, not the current implementation.
- Avoid broad names such as `Manager`, `Helper`, `Util`, `Data`, or `Info` unless the project already
  uses them with a precise meaning.
- Do not rename public API for taste alone; treat public renames as compatibility-affecting changes.

## API Evolution and Compatibility

- Prefer additive changes for existing APIs: new methods, overloads, options fields, or result fields
  before replacing existing behavior.
- Do not delete, rename, reorder, or repurpose public fields, enum values, methods, callbacks, or
  status codes without an explicit compatibility decision.
- Treat default behavior changes as contract changes even when signatures stay the same.
- Keep deprecated paths small and documented. Name the replacement, removal condition, and tests that
  prove old and new callers still behave correctly while both paths exist.
- For breaking changes, document affected callers, migration steps, rollout and rollback expectations,
  and whether source, binary, wire, config, or telemetry compatibility changes.

## Header and Dependency Surface

- Public headers must be self-contained and include what they use; consumers should not depend on
  include order.
- Keep implementation dependencies out of public signatures. Prefer forward declarations, pImpl,
  project interfaces, or value request/result types when they preserve the contract.
- Do not expose third-party types, generated internals, containers, allocators, or threading
  primitives unless they are intentionally part of the public contract.
- Changing a public include, namespace, type alias, or dependency can be a source or build
  compatibility change.

## Do Not Expose Internals for Tests

- Do not make private state public just so tests can inspect it.
- Prefer observable behavior: returned status, emitted event/fact, callback, snapshot, structured log
  field, or metric.
- If a seam is needed, add a narrow dependency/fake interface that matches production ownership.
- Keep test helpers in tests unless production code genuinely needs the abstraction.

## Parameter Shape

- Avoid stacks of boolean parameters. Use named options structs, enums, or explicit methods.
- Use strong domain types or small structs when two primitive arguments can be mixed up.
- Use `std::optional` for absence, not sentinel strings or magic numbers.
- Prefer immutable request/command structs for async handoff.
- Keep defaults explicit and close to construction.

## Validation and Error Semantics

- Define which layer validates required fields, ranges, units, encodings, and cross-field
  invariants.
- Make units explicit in names or types at API boundaries, especially for time, size, count, and
  externally meaningful identifiers.
- Distinguish absent, empty, default, unknown, and intentionally cleared values when they have
  different meaning.
- Keep status and error codes stable enough for callers to branch on them; do not expose raw
  third-party error strings as the only programmatic signal.
- Preserve useful diagnostic detail through structured, sanitized fields without requiring callers to
  parse logs or prose.

## Result Shape

- Public APIs should return enough information for the caller to distinguish validation errors,
  dependency failures, business denials, internal faults, and accepted async work.
- Do not force callers to parse log text or exception strings for control flow.
- Do not throw across thread, callback, destructor, shutdown, or C ABI boundaries unless the project
  explicitly allows it.

## Lifetime and Ownership

- `std::string_view` is for borrowed input only; do not store it or send it across async boundaries.
- Use owning values such as `std::string` for request data that outlives the call.
- Make ownership obvious in names and types: `unique_ptr`, `shared_ptr`, `weak_ptr`, references, and
  raw pointers should each communicate a real lifetime relationship.
- Avoid callbacks that can be invoked while holding locks.

## Async and Callback Contracts

- Document the callback owner, executor, thread, or synchronization context before adding the
  callback to a public API.
- State callback cardinality: exactly once, at most once, zero or more times, until unsubscribe, or
  only while an operation is active.
- Define ordering guarantees between calls, callbacks, status updates, cancellation, and shutdown.
- Define whether callbacks may re-enter the API, and ensure callbacks are not invoked while holding
  internal locks.
- Define cancellation and shutdown behavior: what work is rejected, drained, completed, or silently
  ignored after stop begins.
- For async APIs, make acceptance, progress, completion, failure, and timeout signals observable
  without relying on log text.

## Contract Changes

Stop and ask or document explicitly when changing:

- public header behavior;
- source compatibility for downstream callers;
- binary compatibility when the project has ABI constraints;
- generated or wire schema;
- lifecycle order;
- thread ownership;
- log event or field schema;
- metric names, labels, buckets, or help text;
- runtime configuration behavior.

Schema changes must update code, tests, and project docs together.
