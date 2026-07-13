# Skill: Module Plan

Use this when turning requirements, design notes, tickets, bug reports, or module ideas into a
C++ implementation plan.

Read project context first, as defined in `../README.md`.

## Inputs

- Identify the requirement source: user request, issue, design doc, failing test, production symptom,
  or maintainer instruction.
- Separate must-have behavior, nice-to-have behavior, non-goals, constraints, and open questions.
- Preserve explicit acceptance criteria. Do not replace them with inferred architecture preferences.
- If the requirement changes public contracts, ownership, lifecycle, security, observability, or build
  shape, make that explicit in the plan.
- If the source is missing, stale, or internally inconsistent, stop and ask before planning a large
  change.

## Requirement Evidence and Freshness

- Record the requirement source and the code, tests, docs, logs, or runtime evidence that support the
  plan.
- If a ticket, design doc, comment, or copied inventory conflicts with current source code or
  authoritative project context, plan from the current source/context and state the assumption.
- Do not base a large plan on stale or unverified inventories.

## Explore the Existing System

- Locate current entry points, similar implementations, tests, build targets, generated-code targets,
  config, and docs.
- Read nearby code before proposing new abstractions.
- Check whether the host project already defines module boundaries, owner contexts, facade rules,
  naming conventions, and test target maps.
- Prefer extending an existing pattern when it fits the requirement and keeps ownership clear.
- Record any discovered constraints that affect the plan: ABI/API compatibility, thread ownership,
  lifecycle order, schema compatibility, rollout, or dependency policy.

## Impact and Consumer Discovery

- When changing a contract, state model, config, log, metric, build entrypoint, generated input, or
  runtime asset, search for direct consumers before finalizing the plan.
- Include call sites, tests, docs, project context, generated artifacts, packaging rules, and runtime
  lookup paths in the impact scan.
- Do not plan only around the owning module when downstream consumers must change too.

## Define Scope

The plan should name:

- behavior to add, remove, or preserve;
- affected modules, APIs, headers, tests, docs, build files, generated code, logs, and metrics;
- data flow, state ownership, callback ownership, and shutdown behavior;
- dependency and build-system impact;
- compatibility expectations for source, binary, wire, log, metric, config, and persisted schemas;
- smallest useful test target and broader verification gate.

## Verification Gate Map

- Map each major slice to a focused gate that proves its changed contract and a broader guard that
  catches integration regressions.
- For build files, generated inputs, package rules, or runtime inputs, include configure, regenerate,
  or build gates before dependent tests.
- Identify verification that is unavailable, too expensive, or requires external state, and state the
  risk early.

## Choose Supporting Skills

Use more specific skills while planning when they apply:

- `api-design.md` for public headers, facades, service APIs, callbacks, or contracts.
- `design-patterns.md` for new abstractions, extension points, state models, or collaboration models.
- `concurrency.md` for owner executors, async callbacks, timers, cancellation, locks, or shutdown.
- `memory-safety.md` for ownership, lifetimes, references, views, callback captures, or containers.
- `build-system.md` for targets, generated code, package manager files, or linking.
- `testing.md` for test target selection and test strategy.
- `security-logging.md` for logs, metrics, identifiers, requests, debug output, or sensitive data.
- `performance.md` for hot paths, queues, retries, batching, or latency-sensitive work.

## Plan Slicing and Sequencing

- Split the implementation into the smallest slices that can be reviewed and verified independently.
- Establish the observable behavior boundary or test before replacing internal implementation when
  feasible.
- Avoid binding refactors, public contract changes, build changes, formatting, and behavior changes
  into one unverifiable step unless the dependency is unavoidable.

## Plan Output

Produce a short implementation plan with:

1. Goal and acceptance criteria.
2. Assumptions, open questions, and non-goals.
3. Impacted files, modules, and contracts.
4. Proposed design and tradeoffs.
5. Implementation steps in dependency order.
6. Test and verification strategy.
7. Compatibility, rollout, and rollback risks.

Keep the plan executable. Avoid vague steps such as "update logic" without naming the owning module
or contract.

## Stop and Ask

Stop before implementation if the plan requires a product decision, public contract break, dependency
upgrade, new thread/lifecycle owner, security relaxation, migration, or large architectural split that
the requirement did not explicitly authorize.
