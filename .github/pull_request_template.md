# Pull Request

## Summary

- 

## Scope

- [ ] Feature or behavior change
- [ ] Bug fix
- [ ] Refactor without intended behavior change
- [ ] Tests only
- [ ] Docs only
- [ ] Build/CI/tooling

## Contract Impact

- [ ] Public C++ API unchanged
- [ ] Public C++ API changed and `docs/API_CONTRACT.md` updated
- [ ] Persisted schema unchanged
- [ ] Persisted schema changed and migration/compatibility notes added
- [ ] LangGraph-style behavior unchanged
- [ ] LangGraph-style behavior changed and conformance/limitations updated

## Quality Evidence

- [ ] Focused tests run
- [ ] Broader CTest or CI gate run
- [ ] `scripts/check-dependency-policy.sh` run when layering/build shape changed
- [ ] `scripts/check-context-skills.sh` run when `context/` skills, authority pins, or agent entrypoints changed
- [ ] `scripts/run-examples.sh` run when public API, examples, stream/checkpoint/runtime behavior changed
- [ ] `scripts/generate-quality-report.sh` run for release-readiness or quality evidence changes
- [ ] Docs snippet compile run when README/API docs changed
- [ ] `git diff --check` run

## Safety And Operations

- [ ] No new default privileged shell/file/network/hardware capability
- [ ] Secrets, authorization headers, request/response bodies, and private user content are not logged
- [ ] Metrics labels remain bounded and low-cardinality
- [ ] Optional dependencies remain behind CMake options or injected interfaces
- [ ] Lifecycle, cancellation, shutdown, and concurrency effects considered

## Documentation

- [ ] `docs/LIMITATIONS.md` updated when support boundaries changed
- [ ] `docs/TRACEABILITY_MATRIX.md` / `docs/TEST_CATALOG.md` updated when coverage changed
- [ ] `docs/EXAMPLE_MATRIX.md` or examples updated when public usage changed
- [ ] No docs claim unsupported production maturity or full upstream parity

## Verification

Commands run:

```text

```

Commands not run and why:

```text

```
