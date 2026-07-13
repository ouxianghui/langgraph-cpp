# Skill: C++ Testing

Use this when adding, changing, or reviewing C++ tests.

Read project context first, as defined in `../README.md`.

## Choose the Smallest Target

- Start from the contract being changed, then choose the narrowest existing test binary or test suite.
- Prefer a focused unit/component test over the full suite while developing.
- Use the broader suite after changes to public headers, lifecycle, threading, generated code,
  build-system files, logging, metrics, or shared helpers.
- If a suitable target exists, add cases there instead of creating a new executable.
- Create a new test target only when existing targets would couple unrelated domains or require
  excessive fixtures.
- Use the target map and commands from `STACK.md`; do not invent target names.

## Contract Coverage Matrix

- Start each test from the contract or observable behavior it proves.
- Prefer coverage across success, failure, boundary values, invalid input, lifecycle/shutdown, and
  compatibility when those cases are relevant.
- Do not stop at a happy-path case when the changed contract has meaningful error or edge behavior.

## Unit Test Style

- Name tests by behavior, not implementation detail.
- Assert observable outcomes: returned status, emitted event/fact, state snapshot, callback count,
  structured log fields, or rendered metrics.
- Keep fixtures small. Put reusable fakes close to the tests that need them unless several targets
  already share a helper.
- Use the test framework and naming conventions defined by `STACK.md` and existing nearby tests.
- Prefer explicit fakes/stubs for project boundaries. Use mock frameworks only when interaction
  ordering or call arguments are the contract.
- Avoid asserting every field when only one field is relevant to the behavior.

If the project uses gtest/gmock:

- Use `ASSERT_*` for preconditions that make the rest of the test unsafe, and `EXPECT_*` for multiple
  independent observations.
- Use gmock only when interaction ordering or call arguments are the contract.

## Fixture and Test Data Hygiene

- Fixtures should express only the context needed by the behavior under test.
- Keep test data small, realistic, and intentionally named.
- Helpers and builders should not hide the key action or assertion.
- Avoid copying large production payloads or sensitive examples into tests unless the project provides a
  sanitized fixture.

## Async and Thread Tests

- Do not use arbitrary sleeps to "wait long enough".
- Prefer latches, condition variables with deadlines, futures, promises, semaphores, or explicit test
  hooks that signal completion.
- Every wait must have a bounded timeout and a useful failure message.
- Assert callback owner context when the behavior depends on dispatch.
- Test both accepted work and rejected-after-shutdown behavior for async components.
- For queued work, prove drain/cancel behavior instead of only checking that the process exits.

## Determinism and Isolation

- Tests should not depend on execution order, wall-clock timing, random values, external networks, or
  shared global state.
- Use fake clocks, fake transports, deterministic schedulers, and temporary resources when the contract
  allows them.
- Isolate and clean up files, ports, environment variables, singletons, caches, and background work.

## Lifecycle Tests

- Cover start, stop/shutdown, and double/partial calls when lifecycle code changes.
- Cover partial initialization failure and rollback when a constructor/setup/start step can fail.
- Shutdown and destructors must be safe after partial startup.
- Verify that work is no longer accepted after shutdown begins.
- Avoid tests that pass only because the object is destroyed immediately after the action.

## Logging Tests

- Parse structured log output and assert fields, levels, component/category, event, and redaction
  behavior.
- Do not match natural-language message prose.
- Assert that sensitive identifiers are hashed/redacted under approved field names.
- Do not add tests that depend on raw sensitive values being logged.
- If adding a log component/category or event shape, check the production call site and project log
  safety gate.

## Metric Tests

- Check catalog/registry membership, labels, and export/render shape together.
- Metric schema changes must keep code, rendering/export, tests, and docs aligned.
- Labels must be bounded and low-cardinality.
- Never test or introduce IDs, URLs, tokens, raw errors, or bodies as labels.
- Run project-defined metric gates after metric changes.

## Failure Diagnostics and Regression Evidence

- For bug fixes, preserve or reproduce the original failure signal before changing the test.
- Assertions should identify the contract that failed and include enough context to debug the failure.
- Avoid overly broad tests that pass while hiding the root cause.
- State residual risk when a relevant failure mode cannot be covered.

## Before Finishing

- Run the exact failing/new test first.
- Then run the smallest broader target affected by the change.
- For logging or metric changes, run the project-defined static gates from `STACK.md`.
- State any test you did not run and why.
- Use `definition-of-done.md` when test changes are part of a larger task or ready for handoff.
