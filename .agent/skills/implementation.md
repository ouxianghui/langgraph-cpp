# Skill: Implementation

Use this when implementing a feature, fixing a bug, or making a behavior-changing C++ change end to
end.

Read project context first, as defined in `../README.md`.

## Start

- Clarify the requested behavior, acceptance criteria, and non-goals.
- If the work spans several modules, changes a public contract, or starts from a design document, use
  `module-plan.md` before editing code.
- Inspect the current implementation, nearest tests, build configuration, docs, and comparable
  patterns.
- Choose the smallest useful verification command from `STACK.md`.
- Identify the specific supporting skills needed for the change.

## Existing Change Handling

- Check the worktree before editing, including staged, unstaged, and untracked files.
- Treat existing changes as user-owned unless they are clearly part of the current task.
- When unrelated changes already exist in a file you must edit, preserve them and patch around them;
  do not overwrite, reformat, revert, or claim them as your work.

## Verification Strategy

- Choose a focused verification gate and one broader guard before editing when feasible.
- Use the focused gate to prove the changed contract; use the broader guard to catch integration,
  build, link, or runtime regressions.
- If the change touches build files, generated inputs, package rules, or runtime inputs, run the
  required configure, regenerate, or build gate before relying on dependent tests.
- If verification is unavailable or too expensive, state the reason and likely risk surface.

## Guardrails

- Keep the change scoped to the requested behavior.
- Follow existing local patterns unless a project rule or requirement justifies changing them.
- Do not mix unrelated cleanup with behavior changes.
- Keep mechanical refactors separate from semantic edits; use `refactoring.md` when structure changes
  are needed.
- Do not widen public APIs, expose internals for tests, or add global state to avoid local plumbing.
- Update docs, tests, generated artifacts, logs, metrics, or build files in the same change when the
  behavior depends on them.
- Do not commit, push, publish, deploy, or open a review unless the user explicitly asks.

## Utility Reuse

- Before adding a utility class, helper function, common method, wrapper, adapter, or test helper,
  search the project for an existing equivalent.
- Reuse or extend the existing utility when it matches the needed contract, ownership boundary, and
  dependency direction.
- Add a new utility only when the existing options do not fit; place it in the owning layer and avoid
  creating a parallel helper just to avoid locating or updating call sites.

## Change Slicing

- Break implementation into the smallest useful slices that can be inspected and verified.
- Avoid combining a bug fix, refactor, public contract change, build rewrite, and formatting pass in
  the same slice unless the dependency is unavoidable.
- For larger changes, establish the behavior boundary first, then replace internal implementation
  behind that boundary.

## Call-Site and Integration Audit

- When changing a public or internal contract, state model, config, log, metric, build entrypoint, or
  runtime input, search for direct consumers before finishing.
- Update affected callers, tests, docs, examples, generated artifacts, and project context together
  with the behavior change.
- Do not stop after fixing the nearest compile error if the contract has more consumers.

## Work Loop

1. Reproduce or characterize the current behavior when fixing a bug.
2. Add or update the smallest meaningful test when feasible.
3. Implement the narrowest code change that satisfies the contract.
4. Run the exact new or failing test first.
5. Fix compile, link, runtime, crash, hang, or flaky failures using `debugging.md`.
6. Run the smallest broader build/test target affected by the change.
7. Self-review the diff using `code-review.md`.
8. Update project context or docs if the change alters a reusable rule, command, boundary, or contract.
9. Apply `definition-of-done.md` before summarizing the work.
10. Summarize behavior changed, files touched, verification run, skipped checks, and residual risk.

## Skill Routing

Load the more specific skill when the implementation touches that concern:

- C++ idioms, types, templates, standard library, or headers: `modern-cpp.md`.
- Ownership, references, views, callbacks, containers, cleanup, or object lifetime: `memory-safety.md`.
- Tests or test targets: `testing.md`.
- Build files, generated code, package manager files, linking, or dependencies: `build-system.md`.
- Owner contexts, callbacks, timers, locks, shutdown, cancellation, or races: `concurrency.md`.
- Public APIs, facades, callback contracts, status/result shapes, or compatibility: `api-design.md`.
- Abstractions, design patterns, extension points, or state/collaboration models: `design-patterns.md`.
- Hot paths, queues, retries, batching, metric labels, or blocking work: `performance.md`.
- Logs, metrics, request/response data, identifiers, debug switches, or sensitive data:
  `security-logging.md`.
- Project-specific domains: the matching skill listed by the host project's context entrypoint.

## Before Finishing

- Confirm the implementation still matches the original acceptance criteria.
- Check that lifecycle, shutdown, partial initialization, and error paths are covered when touched.
- Check that structured logs, metrics, docs, and tests stay consistent when touched.
- Run the relevant command from `STACK.md`; state any skipped command and why.
- Use `definition-of-done.md` as the final completion gate.

## Stop and Ask

Stop before editing further if the implementation requires changing a public contract without an
explicit requirement, upgrading dependencies, weakening security/logging rules, adding a new runtime
owner, changing persistence/wire compatibility, or choosing between product behaviors.
