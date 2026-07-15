# Skill: Tests And Examples Authorship

Use this when adding or revising files under `tests/` or `examples/`, docs snippets, golden fixtures,
or EXAMPLE_MATRIX / TEST_CATALOG entries.

Pair with [`../../tests/README.md`](../../tests/README.md),
[`../../examples/README.md`](../../examples/README.md),
[`docs/TEST_CATALOG.md`](../../docs/TEST_CATALOG.md),
[`docs/EXAMPLE_MATRIX.md`](../../docs/EXAMPLE_MATRIX.md), and
[`docs/QUALITY_MODEL.md`](../../docs/QUALITY_MODEL.md).

## Default Gate Principles

- No Python, real cloud provider, real hardware, or external llama.cpp in default tests/examples.
- Assert `Status` / `Result` failures; do not ignore errors in examples.
- Cover failure paths for graph, checkpoint, interrupt, stream, subgraph, store, crash recovery.
- Docs public API snippets should compile under the docs CTest label.

## Tests

| Do | Don't |
| --- | --- |
| Name tests by scenario + expectation | Sleep-based timing flakes |
| Explicit sync/timeout/shutdown in concurrency tests | Unbounded waits |
| Temp dirs for crash/corruption | Writing unknown paths outside the sandbox |
| Fake HTTP / mock edge adapters | Live network or devices |

## Examples

| Do | Don't |
| --- | --- |
| One clear capability per example | Kitchen-sink demos |
| Check every `Status` / `Result` | Swallow errors to “keep it short” |
| Mark optional deps in EXAMPLE_MATRIX | Hide llama.cpp/hardware requirements |
| Keep smoke-eligible examples offline-friendly | Require credentials for default smoke |

Optional llama examples stay behind `LANGGRAPH_CPP_WITH_LLAMA_CPP=ON` and are out of the default
release gate.

## Docs / Fixtures Sync

- Behavior changes → update tests and, when public, examples + `TRACEABILITY_MATRIX` / catalogs.
- Envelope/schema changes → update `testdata/langgraph` golden fixtures.
- After example matrix changes, prefer `scripts/run-examples.sh`.

## Verification

```sh
ctest --test-dir build/unix-debug --output-on-failure
ctest --test-dir build/unix-debug -L docs --output-on-failure
scripts/run-examples.sh
```
