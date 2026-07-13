# Skill: Definition of Done

Use this before declaring a C++ coding task complete, handing off work, preparing a review, or
summarizing an implementation.

Read project context first, as defined in `../README.md`.

## Principle

Definition of Done proves that the requested task is complete, not merely that code was edited.

Use this with `implementation.md`, `refactoring.md`, `testing.md`, or any project-specific skill when
the work is ready to close. Host project rules, maintainer instructions, and `STACK.md` commands are
authoritative.

## Requirement Check

- The final behavior matches the original request and acceptance criteria.
- Non-goals remain untouched.
- Any assumptions, product decisions, or unresolved questions are stated clearly.
- Bug fixes reproduce or characterize the original failure when feasible.
- The diff contains no unrelated cleanup, formatting churn, generated noise, or opportunistic
  refactors.
- User-visible behavior, public API, schema, logging, metric, config, and runtime changes are
  explicitly called out.

## Scope and Diff Evidence

- Review the final diff against the requested scope before declaring done.
- Check the worktree state, including staged, unstaged, untracked, generated, renamed, and moved
  files.
- Do not summarize, claim ownership of, or review files that were not read or are outside the task
  scope; state assumptions for unrelated existing changes.

## Code Check

- The implementation follows nearby code style, naming, layering, and ownership patterns.
- Error paths are explicit and return/report meaningful status according to project conventions.
- Public APIs do not expose internal state just to make testing easier.
- Mechanical moves, renames, and formatting are separate from semantic behavior changes.
- New abstractions have a real caller or near-term extension point; they are not speculative.
- Dead compatibility paths, unused helpers, and obsolete tests are removed when the migration is
  complete.

## C++ Safety Check

- Object lifetimes are clear: owner, borrower, observer, and transfer semantics are visible.
- RAII owns resources; cleanup does not depend on callers remembering a manual sequence.
- References, pointers, iterators, `string_view`, `span`, and callback captures do not outlive their
  storage.
- Move operations leave objects in valid states and do not hide use-after-move bugs.
- Container mutation cannot invalidate active iterators, references, or views used later.
- Exception and `noexcept` behavior matches project conventions and public contract expectations.

## Concurrency and Lifecycle Check

- Every mutable state change has a clear owner: executor, mutex, or lifecycle boundary.
- Callbacks enter the owner context before touching owner-owned state.
- Async captures use safe ownership patterns and do not keep objects alive accidentally.
- Stop, shutdown, cancellation, and rejected-after-stop behavior are handled.
- Partial initialization failure rolls back safely.
- Lock scopes are minimal; lock ordering is consistent; no blocking work runs while holding locks.
- No arbitrary sleeps are used to prove asynchronous behavior.

## Test and Verification Check

- The exact new or originally failing test/command was run first.
- The smallest broader affected build or test target was run after the focused check.
- Tests cover the changed contract, including relevant success, failure, lifecycle, and shutdown paths.
- Async tests use bounded waits with useful failure messages.
- Log tests parse structured output instead of matching natural-language prose.
- Metric tests keep code, catalog/registry, rendering/export, docs, and labels consistent.
- Any skipped checks are listed with a concrete reason, not just "not run".
- If a failure was fixed, the original failing command was rerun successfully.

## Verification Integrity

- Record exact commands that were run, including the working directory and relevant environment,
  build directory, preset, config, or target selector.
- Separate verified results from recommended follow-up checks; do not present unrun checks as proof.
- For failed, skipped, unavailable, or inconclusive checks, state the likely risk surface and whether
  the failure appears related to the change.

## Build, Generated Code, and Docs Check

- Source additions, removals, generated files, and test targets are reflected in the build system.
- Link visibility, target ownership, and package/dependency changes follow project rules.
- Generated code is updated only through the project-approved workflow.
- Dependency versions are not changed unless the task explicitly requires it.
- Runtime config, resource files, shared libraries, install/package rules, rpath, and test lookup
  paths are updated when the change depends on them.
- Documentation, examples, and project context files are updated when commands, boundaries, contracts,
  or reusable rules change.
- When project context provides a drift checklist, apply it before finishing and update authoritative
  context or docs instead of copying fast-moving inventories.

## Compatibility Check

- Source compatibility is considered for public headers and APIs.
- Binary compatibility is considered when the project has ABI constraints.
- Wire formats, persisted data, config keys, log schemas, metric names, and metric labels remain
  compatible or have an explicit migration/change note.
- Rollout, rollback, and mixed-version behavior are considered when the project deploys components
  independently.

## Security and Observability Check

- Logs and metrics contain structured fields needed for debugging the changed behavior.
- Sensitive data is not logged, exported, dumped, or used as metric labels.
- Redaction, hashing, and sanitization follow project-approved helpers and field names.
- Debug switches do not weaken production security guarantees.
- Request/response bodies, tokens, display names, user content, and raw external payloads are not
  recorded unless the project explicitly allows a safe form.

## Final Response

When reporting completion, include:

- what changed;
- key files or areas touched;
- verification commands run and results;
- checks not run and why;
- compatibility, lifecycle, concurrency, security, or observability risks that remain.

Do not claim the task is done if required verification was skipped without saying so.

## Stop and Ask

Stop before declaring done if the change needs an unapproved public contract break, dependency
upgrade, security exception, data migration, ABI decision, rollout decision, or product behavior
choice.
