# Skill: Debugging

Use this when investigating a compile failure, link failure, runtime failure, crash, hang, or flaky
test in a C++ project.

Read project context first, as defined in `../README.md`.

## First Reproduce

1. Reproduce with the smallest command or test target.
2. Capture the exact command, exit code, and first meaningful error.
3. Classify the failure before editing: compile, link, runtime, crash, hang, or flaky.
4. Check recent diffs before assuming the source behavior is old or intentional.
5. Fix the root cause, not the nearest symptom.
6. Re-run the original failing command after the fix.

Do not start with a broad refactor. Make the failing signal smaller and sharper first.

## Evidence Capture

- Record the exact command, working directory, environment/config assumptions, build directory,
  preset/toolchain when relevant, exit code, and first meaningful error.
- Preserve the original failure signal before editing: logs, stack trace, assertion, compiler/linker
  diagnostic, sanitizer output, or test output.
- Capture the relevant current diff before assuming the failure is from old source behavior.
- Prefer the first failing frame, assertion, or diagnostic over the last cascade.

## Hypothesis Discipline

- Change one variable at a time while investigating.
- Use the smallest experiment that can confirm or falsify the current hypothesis.
- Do not combine debugging with broad refactors, cleanup, formatting, or dependency churn.
- If a workaround makes the symptom disappear, continue until the root cause and risk are understood.

## Compile Failures

- For missing symbols/types, check the include that should declare the symbol and whether the header
  is self-contained.
- For template or overload errors, find the first project-owned frame or instantiation point.
- For generated-code includes, confirm the schema/input is part of the configured generated-code
  target and the generated include directory is available.
- For warnings-as-errors, fix the warning instead of weakening the preset.

## Link Failures

- `undefined symbol`: the implementation file may not be part of the target, the library may not be
  linked, or declaration/definition signatures may differ.
- `duplicate symbol`: check for non-inline definitions in headers, duplicate source inclusion, or two
  implementations of the same function.
- Missing third-party symbols: check link scope and package target availability.
- Test-only link failures often mean the test target missed the production library, generated-code
  library, test framework target, or required package.

## Environment and Configuration Drift

- Check whether the toolchain, build cache, generated files, package outputs, working directory, or
  runtime resources differ from the expected project context.
- Verify runtime config, feature flags, environment variables, shared-library paths, resource paths,
  and test fixture paths before changing source behavior.
- For build failures after build-system or package changes, reconfigure/regenerate before trusting
  stale compile or link errors.

## Runtime Failures

- Read the first failure, not the last cascade.
- Verify config and environment assumptions from `STACK.md`.
- For async commands, distinguish immediate acceptance from final completion if the project uses that
  model.
- For logs and metrics, inspect structured fields and exported telemetry, not message text.

## Crashes

Check in this order:

1. Raw `this` captured into delayed work after shutdown can begin.
2. Borrowed view or reference retained across async/lifetime boundaries.
3. Callback running on the wrong owner context.
4. Object destroyed while a transport, timer, or executor still holds work.
5. Exception escaping thread, callback, destructor, or shutdown boundary.
6. Partial initialization followed by unsafe cleanup.

Prefer a small regression test that reproduces the lifetime boundary.

## Hangs and Deadlocks

Check in this order:

1. Synchronous dispatch called from the same owner context.
2. Condition variable wait that shutdown cannot notify.
3. Lock held across callback, logging-heavy loop, network I/O, or blocking wait.
4. Two-lock path with unclear ordering.
5. Owner work waiting for another task queued behind itself.
6. Test waiting without a bounded timeout.

Do not "fix" hangs by increasing sleeps.

## Flaky Tests

Check in this order:

1. Timer or async completion racing the assertion.
2. Shared global state leaking between tests.
3. Owner context not fully drained/stopped.
4. Background callback from a previous test.
5. Real clock/network/file-system dependency where a fake should be used.
6. Unbounded wait, retry, queue, or batch behavior.

Stabilize synchronization. Do not paper over flakes with longer sleeps.

## Regression and Guard Selection

- After a fix, rerun the original failing command first.
- Then run the smallest broader guard that covers the same contract, owner boundary, build graph, or
  runtime path.
- Prefer adding a regression test around the contract or lifetime boundary that failed.
- If a regression test is not feasible, state why and name the remaining risk.

## Before Finishing

- Confirm the original failure no longer reproduces.
- Run one broader guard when the fix touches shared lifecycle, threading, logging, metrics, build
  files, generated code, public headers, or other shared behavior.
- If code changed, apply `definition-of-done.md` before the final summary.
- In the final note, include the failing symptom, root cause, fix, verification commands, skipped
  checks, and residual risk.
