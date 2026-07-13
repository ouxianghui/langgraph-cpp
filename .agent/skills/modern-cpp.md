# Skill: Modern C++ Coding

Use this when changing C++ source or headers, especially when choosing language idioms, ownership
types, value/reference semantics, templates, error/result types, or standard-library abstractions.

Read project context first, as defined in `../README.md`.

## Before Editing

- Identify the owning layer/module from `CONVENTIONS.md`.
- Identify the mutable-state owner before adding a field, callback, cache, timer, or queue.
- Search for an existing utility class, helper function, common method, facade, owner context,
  log/telemetry helper, or test pattern before creating a new one.
- Prefer a narrow local change over widening public interfaces, unless the new interface is the
  actual requirement.
- Follow the C++ standard version and library availability defined by `STACK.md`.

## Headers and Includes

- Headers must be self-contained: a consumer should be able to include the header without relying on
  include order.
- Include the headers for symbols you use; do not rely on transitive includes.
- Keep related includes in the style of the surrounding file. Do not churn include order for unrelated
  files.
- Prefer forward declarations only for project-owned types when the header does not need the full
  definition. Do not forward declare `std` types.
- Keep implementation details in `.cpp` files or private/internal headers.
- Do not expose helpers in public headers only to make tests easier.
- Avoid defining non-trivial functions in headers unless they are templates, tiny accessors, or must
  be inline for ODR reasons.

## Naming

- Class, interface, method, and member-variable names must be concise, professional, and precise.
- Prefer domain terms and established project vocabulary over long descriptive phrases.
- Name by responsibility or contract, not by implementation detail, pattern name, or temporary task.
- Avoid vague container names such as `Manager`, `Helper`, `Util`, `Data`, `Info`, or `Context`
  unless the surrounding project already gives them a precise meaning.
- Avoid clever abbreviations. Use short names only when they are standard in the project or C++
  domain.
- Member-variable names should reveal ownership, role, or guarded state when relevant without
  restating the class name.
- Public names should be stable and conservative; rename public API only when the new name is clearly
  more accurate and compatibility is addressed.

## Initialization and Invariants

- Make constructed objects satisfy their invariants before they are observable by callers.
- Keep member declarations and initializer lists in the same logical order; C++ initializes members in
  declaration order.
- Avoid designs that require callers to remember an extra `init()` call before safe use.
- When construction or startup can fail partway through, keep acquired resources and cleanup ownership
  explicit.

## Values, Views, and Ownership

- Prefer value types for simple data and clear ownership.
- Use `std::optional` for absence, not sentinel strings or magic numeric values.
- Use `std::string_view` and `std::span` only as non-owning parameter views with obvious lifetime.
- Store owning values when data crosses an async, thread, callback, container, or object-lifetime
  boundary.
- Prefer `unique_ptr` for exclusive ownership and `shared_ptr` only for real shared lifetime.
- Capture `weak_ptr` in delayed callbacks unless the owner intentionally keeps itself alive.
- Do not retain references, iterators, pointers, spans, or string views across operations that can
  invalidate them.

## Const Correctness and Mutability

- Mark values, parameters, and methods `const` when mutation is not part of the contract.
- Give `mutable` caches an owner, synchronization rule, and invalidation rule.
- Do not use `const_cast` to hide an API design problem.
- Read-only APIs should not change observable state unless the contract says so.

## Move and Copy Semantics

- Move only when ownership is transferred and the moved-from object is not used except for assignment
  or destruction.
- Do not `std::move` from `const` objects.
- Do not return `std::move(local)` from a function unless the project has a measured reason; let NRVO
  work.
- Prefer explicit copies when copying simplifies lifetime or avoids shared mutable state.
- For async handoff, move payloads when the receiver owns them; copy small immutable values when that
  makes lifetime clearer.
- Define or delete copy/move operations intentionally for resource-owning types.

## Lambda and Callable Shape

- Avoid default `[=]` or `[&]` captures in non-trivial lambdas; capture only what is needed.
- Use explicit move, copy, or weak ownership in async, queued, timer, or callback lambdas.
- Do not capture references to locals, views, iterators, or raw `this` into work that can run later.
- Keep callable objects small and name them when captures, branching, or lifetime rules become hard to
  see inline.

## APIs and Types

- Use explicit domain types or small structs when argument order is easy to confuse.
- Prefer enums or options structs over boolean parameter stacks.
- Use `std::variant` when the valid alternatives are closed and compile-time known.
- Use polymorphism or type erasure when alternatives are open-ended or loaded/extended at runtime.
- Use project-approved result/expected/status types for recoverable errors. Do not force callers to
  parse exception strings or logs for control flow.
- Do not make a function public just because another layer wants one field. Add a narrow facade or
  route through the existing owner.

## Ranges, Algorithms, and Containers

- Prefer standard algorithms/ranges when they make intent clearer than a loop.
- Use loops when ranges create awkward lifetime, readability, debugger, or compile-time costs.
- Avoid lazy range pipelines over temporaries unless lifetime is obvious.
- Be explicit about iterator/reference invalidation when modifying containers.
- Prefer `std::vector` by default for compact owned sequences; choose other containers for a specific
  access, stability, or ordering need.

## Numeric and Conversion Safety

- Avoid implicit narrowing, signed/unsigned surprises, and magic numeric constants.
- Centralize conversions at external boundaries and validate range, unit, and sentinel values there.
- Prefer `explicit` constructors and conversion operators unless implicit conversion is a deliberate
  part of the type contract.
- Use named constants or domain types when numeric meaning is not obvious at the call site.

## Time and Units

- Use `std::chrono` types for durations and time points.
- Do not pass raw integers for timeouts, intervals, or timestamps unless the boundary requires it.
- Name units at external boundaries and convert once near the boundary.
- Prefer monotonic clocks for elapsed time and deadlines; use wall clocks only for calendar time or
  externally meaningful timestamps.

## noexcept, Exceptions, and Errors

- Mark destructors, move operations, and shutdown/cleanup functions `noexcept` when they must not
  fail across boundaries.
- Do not let exceptions escape thread entry points, callbacks, destructors, shutdown functions, or C
  ABI boundaries unless the project explicitly allows it.
- Catch exceptions at lifecycle/thread/callback boundaries and convert them to project-approved
  diagnostics.
- Keep retry loops bounded and observable.
- Preserve original error context with sanitized readable fields where possible.

## Templates and Compile-Time Design

- Use templates when they remove real duplication, enforce a useful contract, or avoid runtime cost in
  low-level reusable code.
- Prefer ordinary functions, values, virtual interfaces, or small policy objects when they are easier
  to read and test.
- Keep template definitions and constraints understandable at call sites.
- Use concepts or `static_assert` when the project standard supports them and diagnostics matter.
- Avoid template metaprogramming that hides ownership, threading, or error behavior.

## Logging, Metrics, and Verification

- Use the structured logging and telemetry helpers approved by `CONVENTIONS.md`.
- Do not add free-text logs where structured events are required.
- Never log credentials, authorization headers, request/response bodies, private identifiers, or user
  content unless the project explicitly allows a sanitized form.
- Add or update the smallest test that proves the changed contract.
- Build the touched target and run the smallest relevant test from `STACK.md`.
- Run project-defined static gates for log, metric, generated-code, formatting, or lint changes.

## Stop and Ask

Stop instead of guessing if the change would alter lifecycle order, thread ownership, public wire
contracts, log/metric schema, lower-layer ownership, ABI/source compatibility, or
production-sensitive logging behavior.
