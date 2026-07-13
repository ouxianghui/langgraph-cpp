# Skill: Concurrency

Use this when changing owner executors, event loops, callbacks, async components, timers, locks,
shutdown, or code that can race in a C++ project.

Read project context first, as defined in `../README.md`.

## Owner Decision

Before editing mutable state, answer these questions:

1. Which object owns the state?
2. Which thread, executor, event loop, actor, or mutex may mutate it?
3. Is the state protected by owner confinement, a mutex, lifecycle immutability, or a narrow atomic
   flag?
4. Which foreign-thread callbacks can observe or modify it?
5. What happens to queued work after shutdown begins?

If the owner is unclear, do not add the state yet. Clarify the owner boundary or route through an
existing facade.

## Callback Into Owner Pattern

Use this shape for foreign-thread callbacks, adapted to the project's executor API.

This is shape-only pseudocode, not copy-paste API. Names such as `weak_from_this()`,
`ownerExecutor().post()`, and `checkOwner()` are placeholders for the project's actual lifetime,
dispatch, and owner-check mechanisms.

```cpp
auto weakSelf = weak_from_this();
source->setCallback([weakSelf](Event event) {
  if (auto self = weakSelf.lock()) {
    self->ownerExecutor().post([self, event = std::move(event)] {
      self->checkOwner();
      self->handleEventOnOwner(event);
    });
  }
});
```

The important parts are:

- capture weak ownership across the foreign boundary;
- move or copy payloads with clear lifetime;
- enter the owner context before touching owner state;
- check owner context at the owner-only entry point;
- reject or ignore work after shutdown begins when appropriate.

## Ordering and Reentrancy Contract

- Define whether callbacks can run synchronously during registration or API calls.
- Define whether callbacks can run concurrently, are serialized on one owner context, or may arrive
  on multiple contexts.
- Define event ordering guarantees, including whether older queued work can run after newer state
  has replaced it.
- Define whether callbacks may re-enter the API, and if so which methods are safe to call.
- Do not depend on incidental executor FIFO behavior unless the project explicitly guarantees it.

## weak_ptr Capture Rules

- Prefer `weak_ptr` for delayed callbacks, timers, transport callbacks, and queued work that can
  outlive the initiating call.
- Use strong `shared_ptr` capture only when keeping the owner alive is intentional and bounded.
- Never capture raw `this` into work that can run after shutdown begins.
- Do not capture references to stack values unless the work is guaranteed to run before the stack
  unwinds.
- Store owning values, not borrowed views, across async boundaries.

## Stop-State, Epoch, and Cancellation

- Stop accepting new work before tearing down resources.
- Use optional-send/try-send APIs for work that may be rejected after shutdown starts.
- Use an epoch/token when old async completions must be ignored after restart or state replacement.
- Cancel timers/subscriptions before destroying their owner state.
- Drain or intentionally discard queued work; do not leave shutdown behavior accidental.
- Shutdown functions and destructors must be safe after partial initialization.

## Queue, Backpressure, and Fairness

- Give every queue, mailbox, retry loop, and batch a bounded growth story: capacity, backpressure,
  drop policy, coalescing, or shutdown drain.
- Define what happens when work cannot be accepted: fail fast, retry with limits, drop lowest-value
  work, or surface backpressure to callers.
- Avoid retry or dispatch loops that monopolize an owner context or starve unrelated work.
- Keep per-item work small enough that owner contexts remain responsive, or move expensive work to an
  appropriate worker boundary.

## Lock Scope Checklist

Before adding or changing a mutex:

- Name the invariant protected by the lock.
- Keep the critical section minimal.
- Do not hold the lock across callbacks, network I/O, blocking waits, or log/metric-heavy loops.
- Copy or move data out of the lock before invoking callbacks.
- Document lock ordering if two locks can be held together.
- Avoid one global mutex for unrelated per-instance state.

## Atomics and Memory Visibility

- Prefer owner confinement or a mutex for multi-field invariants.
- Use atomics only for simple flags, counters, state tokens, or deliberately lock-free handoff.
- Do not compose several atomics into one implicit invariant unless the memory-ordering story is
  documented and tested.
- Use explicit memory ordering only when needed; otherwise prefer the clearest project-approved
  default or a mutex.

## Condition Variables and Waits

- Wait with a predicate in a loop; do not use bare waits.
- Every wait must have a shutdown notification path, and tests should use bounded deadlines.
- Notify after state changes that can satisfy the predicate or unblock shutdown.
- Do not prove ordering or completion with sleeps; use explicit synchronization or owner-drain hooks.

## Deadlock and Hang Checklist

When a test or runtime path hangs, check:

- synchronous dispatch from the same owner context;
- owner task waiting for work queued behind itself;
- condition variable wait without a shutdown notify path;
- lock held while calling into code that can re-enter the same object;
- blocking call on an owner/event-loop thread;
- timer/test wait with no bounded timeout;
- shutdown path waiting for a thread that is waiting on shutdown.

## Tests

- Add tests that prove owner context, shutdown behavior, and callback delivery.
- Use latches, futures, condition variables, or semaphores with deadlines instead of sleeps.
- For race fixes, write the test around the synchronization contract, not incidental timing.
- For shutdown fixes, test queued work before, during, and after stop when feasible.
