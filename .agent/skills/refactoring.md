# Skill: Refactoring

Use this when improving C++ structure without changing intended behavior.

Read project context first, as defined in `../README.md`.

## Refactor Intent and Behavior Fence

- State the refactor intent, preserved behavior boundary, and non-goals before editing.
- If tests are thin or missing, pin current behavior with the smallest existing command, use case, or
  observable log/output before moving code.
- Do not mix opportunistic bug fixes or product behavior changes into a pure refactor.

## Rules

- Preserve public behavior and tests.
- Make one architectural move at a time.
- Keep mechanical moves/renames separate from semantic behavior changes.
- Keep ownership clearer after the refactor than before.
- Do not move business logic into lower-level/shared libraries if the project layering forbids it.
- Do not widen public interfaces to avoid local plumbing.
- Delete obsolete compatibility paths when tests prove callers moved.
- Rename for precision, not novelty.

## Boundary and Dependency Check

- Keep dependency direction at least as clear as before the refactor.
- Do not introduce dependency cycles, global mutable state, cross-layer shortcuts, or lower-layer
  knowledge of higher-layer business concepts.
- Do not expand public headers, include dependencies, link interfaces, or exported types unless the
  refactor's boundary change explicitly requires it.

## Move, Rename, and Extraction Hygiene

- Keep moves, renames, and extractions as mechanical as possible.
- Update call sites and reach a compiling state before making any separate semantic adjustment.
- For broad renames or file moves, avoid unrelated formatting churn, include reordering, and cleanup
  edits.

## Safe Sequence

1. Add or identify tests that pin current behavior.
2. Make the mechanical move, extraction, or rename with minimal behavior changes.
3. Update call sites.
4. Run targeted tests if the mechanical step is non-trivial.
5. Make any intended semantic behavior change as a separate step.
6. Delete old path.
7. Run targeted tests and static gates from `STACK.md`.
8. Use `definition-of-done.md` as the final completion gate.

## Verification and Review Shape

- Run a targeted gate after each non-trivial mechanical step when feasible.
- At the end, build the owning target and at least one meaningful consumer or test target when the
  refactor touches public headers, link interfaces, or cross-module boundaries.
- In the summary, separate mechanical diff from semantic diff so reviewers can inspect risk in order.

Stop if the refactor changes lifecycle order, thread ownership, wire/public contracts, or log/metric
schema without an explicit requirement.
