# Skill: Memory Safety

Use this when changing ownership, lifetimes, references, views, pointers, containers, callbacks,
resource cleanup, or code suspected of use-after-free, dangling access, or invalidation bugs.

Read project context first, as defined in `../README.md`.

## First Identify Lifetime Owners

- Name the owner of every object whose address, reference, iterator, or view is stored or captured.
- Identify which operation can destroy or invalidate that object.
- Identify whether access can happen after shutdown, cancellation, container mutation, or async
  dispatch.
- Prefer owning values at boundaries where lifetime is not obvious.
- If the lifetime story is hard to explain, change the design before adding code.

## Raw Pointers, References, and Views

- Raw pointers and references are non-owning; they must not outlive the owner.
- `std::string_view`, `std::span`, iterators, and borrowed ranges are views; do not store them unless
  the referenced storage lifetime is documented and enforced.
- Do not return references, pointers, views, or iterators to locals or temporaries.
- Do not capture references to stack values in work that can run later.
- Prefer `not_null`-style project helpers or references when null is impossible; use pointers when
  null is a real state.

## Ownership Transfer and Borrowing Contract

- At function and API boundaries, state whether the caller or callee owns each object.
- Distinguish borrowed access from retained storage, async use, or transferred ownership.
- Do not make callers infer the lifetime of raw pointers, references, views, or callback payloads.

## Smart Pointers

- Prefer `unique_ptr` for exclusive ownership.
- Use `shared_ptr` only when shared lifetime is required, not to avoid thinking about ownership.
- Use `weak_ptr` for observer/callback relationships that may outlive the owner.
- Avoid reference cycles. If two objects point to each other, at least one side should usually be weak
  or non-owning with a documented owner.
- Do not call `shared_from_this()` before an object is owned by `shared_ptr`.

## Callback and Async Capture

- Never capture raw `this` into queued, delayed, timer, transport, or callback work that may outlive
  the current call.
- Capture `weak_ptr` and re-check liveness inside the callback unless the owner intentionally keeps
  itself alive.
- Move or copy payloads into async work so their lifetime is independent of the caller's stack.
- Do not capture iterators, spans, string views, or references into async work unless the owner and
  invalidation rules are explicit.
- Ensure callbacks are unregistered, cancelled, or ignored before destroying their owner state.

## Thread-Affine Destruction and Teardown

- For objects bound to an owner thread or executor, define which context constructs, mutates, stops,
  and destroys them.
- Do not let destructors race with in-flight callbacks, queued work, timers, or transport completions
  that can touch the same state.
- Close the teardown sequence explicitly: stop, unregister or cancel, drain or join, then destroy.

## Containers and Invalidation

- Know the invalidation rules of the container being modified.
- Do not keep iterators, references, pointers, spans, or views across insert/erase/reallocation unless
  the container guarantees stability for that operation.
- Prefer indexes or stable IDs over iterators when later mutation is expected.
- Recompute views after mutation.
- Be careful with range-for loops that erase or append to the same container.

## Move and Relocation Safety

- After move, reallocation, or swap, re-check whether saved references, pointers, iterators, views, and
  callbacks still point to valid storage.
- Do not keep views into object members across operations that can move or relocate the object.
- Keep moved-from objects valid for destruction and assignment; document any narrower usable state.

## RAII and Cleanup

- Bind acquire/release to RAII when possible.
- Make cleanup idempotent when lifecycle can be retried, partially initialized, or cancelled.
- Keep destructors simple and non-throwing.
- Put complex shutdown in explicit lifecycle methods where errors can be logged or returned.
- Ensure partially constructed/started objects clean up only resources they actually acquired.

## External Resource and Buffer Lifetime

- Wrap C API handles, SDK handles, buffers, and custom deleters with an explicit owner and release path.
- Track pointer, size, owner, and borrow/retain semantics together for externally owned payloads.
- If an external library only borrows a buffer, keep it alive until the synchronous call returns or the
  documented async completion fires; deep copy when that cannot be guaranteed.

## Diagnostics and Tests

- For suspected dangling access, first reproduce with the smallest command or test.
- Prefer sanitizers, debug iterators, assertions, and focused lifetime tests when available in
  `STACK.md`.
- Test shutdown with queued work, callbacks after cancellation, and partial initialization failure.
- Add regression tests around the lifetime boundary, not only the crashing symptom.

## Stop and Ask

Stop instead of guessing if a fix requires changing ownership, callback lifetime, shutdown ordering,
container stability guarantees, or public API lifetime contracts.
