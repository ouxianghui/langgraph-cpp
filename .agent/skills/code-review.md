# Skill: Code Review

Use this when asked to review a diff in a C++ project.

Read project context first, as defined in `../README.md`.

Apply the more specific skill too when relevant: `modern-cpp.md`, `memory-safety.md`, `testing.md`,
`debugging.md`, `build-system.md`, `concurrency.md`, `api-design.md`, `performance.md`,
`security-logging.md`, or a project-specific skill listed in external `AGENTS.md`.

## Scope First

Before findings, identify the review scope: the base diff, changed files, generated files, and
whether untracked files are included. If the scope is ambiguous, state the assumption before
reviewing.

## Diff and Evidence Collection

- Confirm the review base and head, and whether staged, unstaged, untracked, generated, renamed, or
  moved files are in scope.
- Read the relevant changed files and nearby context before making findings. Do not review files you
  have not inspected.
- Treat test output, logs, benchmarks, and CI status as evidence only when they are in scope or the
  user provides them.
- If the diff or evidence is incomplete, state the assumption and keep findings tied to what can be
  verified from the reviewed material.

## Review Reasoning Checklist

Review for correctness before style, in this order:

1. Ownership
2. Thread safety
3. Lock contention
4. Exception safety
5. Performance

Also check API compatibility, build/link/generated-code impact, test adequacy, observability and
security behavior, shutdown behavior, and partial-initialization cleanup when the diff touches those
areas.

## Severity and Finding Bar

Report findings that can cause a bug, regression, race, lifetime issue, contract break, security or
logging leak, build/link failure, missing required test, or operational blind spot.

Only mention style, naming, formatting, or small cleanup when it affects correctness, obscures a real
risk, or meaningfully blocks future maintenance. Avoid speculative concerns without a realistic
failure scenario.

Output findings first. Each finding needs:

- file and line;
- concrete risk;
- violated project or C++ rule;
- realistic failure scenario;
- suggested fix.

Do not spend review budget on formatting until correctness, lifetime, shutdown, and contract risks
are clear.

## Verification and Residual Risk

- Separate checks personally run from checks recommended but not run.
- Do not describe unrun tests, builds, linters, benchmarks, or static gates as verified.
- If verification is unavailable or out of scope, state the unverified risk surface.
- Mention coverage gaps when the diff changes behavior, public contracts, concurrency, lifecycle,
  generated code, build graph, logs, metrics, or security-sensitive paths.

If no blocking issues are found, say so clearly and list residual risk plus tests/checks that were or
should be run.
