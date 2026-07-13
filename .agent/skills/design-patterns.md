# Skill: Design Patterns

Use this when introducing or changing an abstraction, extension point, collaboration pattern, state
transition model, object lifetime model, or cross-module dependency structure in a C++ project.

Read project context first, as defined in `../README.md`.

## Start With the Problem Shape

Before naming a pattern, describe the force that needs design:

- multiple interchangeable behaviors;
- an external API or legacy interface that does not match project types;
- a public facade hiding volatile internals;
- many observers of a state/event stream;
- explicit state transitions with valid/invalid moves;
- commands that must be queued, retried, logged, undone, or routed;
- object construction that depends on runtime configuration;
- compile-time variation that should not add runtime polymorphism;
- ownership/lifetime complexity that needs RAII or a narrow handle.

If the force is temporary, local, or simpler as plain code, do not introduce a pattern.

## Selection Rules

- Prefer existing project patterns before adding a new one.
- A pattern must reduce real complexity, duplication, coupling, or volatility.
- Keep the stable abstraction small and put volatile behavior behind it.
- Make ownership, lifetime, and thread/owner context explicit in the chosen design.
- Do not widen public APIs just to make a pattern fit.
- Do not add inheritance when composition, a free function, a small value type, or a lambda is enough.
- Name the tradeoff: indirection, allocation, virtual dispatch, compile-time cost, test complexity, or
  API surface.

## Extension Point Bar

- Add an extension point only when there are real variants, a stable boundary, or a clear near-term
  caller.
- Define the default or fallback behavior, registration or discovery path, and owner of each
  implementation.
- Do not create abstractions for future possibilities that are not reflected in current requirements.

## Useful C++ Patterns

- **Strategy**: interchangeable behavior selected at runtime. Prefer when call sites should not know
  the concrete algorithm.
- **Adapter**: translate an external or legacy interface into project-owned types.
- **Facade**: provide a narrow public surface over volatile internal collaboration.
- **Observer / signal**: notify multiple consumers. Define ownership, callback thread, and unsubscribe
  behavior.
- **State**: model explicit lifecycle/state transitions. Prefer when invalid transitions are bugs worth
  making visible.
- **Command**: package work for queues, routing, retries, audit/logging, or async boundaries.
- **Factory / builder**: centralize construction when setup depends on configuration or many validated
  inputs.
- **RAII guard / scope object**: bind resource acquire/release or temporary state to lifetime.
- **Pimpl**: hide implementation details or stabilize public headers when compile-time or ABI concerns
  justify the indirection.
- **Policy-based design**: compile-time variation for low-level reusable code. Avoid if it makes normal
  call sites hard to read.

## Lifecycle, State, and Error Contract

- For State, Observer / signal, Command, Factory / builder, and async collaboration patterns, define
  valid state transitions and invalid calls.
- Specify cancellation, stop/shutdown behavior, failure propagation, retry or duplicate-call behavior,
  and partial-initialization cleanup.
- Make the contract observable through return values, callbacks, logs, metrics, or tests instead of
  hidden internal state.

## Anti-Patterns to Avoid

- Pattern-first design: naming a pattern before proving the problem shape.
- God object or "Manager" classes that own unrelated responsibilities.
- Inheritance for code reuse instead of substitutability.
- Hidden singletons or global mutable registries.
- Stringly typed dispatch when typed commands, enums, variants, or callbacks would be safer.
- Premature abstraction for a one-off change.
- Abstractions that hide thread ownership, shutdown, or error handling.
- Public interfaces that exist only to make tests inspect internals.

## Pattern Cost and Exit Criteria

- State the cost of any new indirection, allocation, virtual dispatch, template expansion, build-time
  cost, or test complexity.
- Keep old and new patterns together only through a bounded migration.
- Remove obsolete abstractions, adapters, compatibility shims, and duplicate paths when they no longer
  carry a required compatibility contract.

## Boundary and Dependency Direction

- Place interfaces in the stable owning layer, not wherever the first implementation happens to live.
- Do not create dependency cycles or make lower-level code learn higher-level business concepts just to
  make a pattern fit.
- Keep public headers, include dependencies, link interfaces, and third-party type exposure as small as
  the pattern allows.

## C++ Fit Checks

- Can the abstraction live in headers without leaking implementation dependencies?
- Does it preserve source compatibility for callers?
- Does it preserve binary compatibility if the project has ABI constraints?
- Does it make exception and error boundaries clearer?
- Does it add allocation, virtual dispatch, template bloat, or compile-time cost?
- Does it make tests simpler because behavior is observable, not because internals are exposed?
- Does it compose with the project's owner/threading model?

## Before Finishing

- Document the chosen pattern and the rejected simpler alternative in the change summary or nearby
  design docs when the tradeoff is non-obvious.
- Add or update tests around the abstraction boundary, not only concrete implementations.
- Re-run the smallest test that proves the collaboration and one broader guard if the pattern touches
  public headers, threading, construction, or cross-module dependencies.
