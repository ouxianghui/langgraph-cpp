# Stack And Build Surface

## Language And Build

| Item | Value |
| --- | --- |
| Language | C++23 |
| Build system | CMake ≥ 3.24 |
| Public namespace | `lgc` |
| Public header | `include/langgraph_cpp/langgraph.hpp` |
| Primary libs | `foundation`, `core` (`lgc::core` assembly), `langgraph` (see root `CMakeLists.txt`) |
| Third-party | vendored under `third_party/`; optional externals via options |

## Default CMake Options (selected)

| Option | Default | Notes |
| --- | --- | --- |
| `LANGGRAPH_CPP_BUILD_TESTS` | ON | Unit / component tests |
| `LANGGRAPH_CPP_BUILD_EXAMPLES` | ON | Acceptance examples |
| `LANGGRAPH_CPP_WITH_CRYPTO` | ON | OpenSSL-backed crypto |
| `LANGGRAPH_CPP_WITH_NETWORK` | ON | cpp-httplib HTTP |
| `LANGGRAPH_CPP_WITH_SQLITE` | ON | SQLite storage |
| `LANGGRAPH_CPP_WITH_GZIP` | ON | zlib gzip when available |
| `LANGGRAPH_CPP_WITH_SPDLOG` | ON | spdlog when available |
| `LANGGRAPH_CPP_WITH_LLAMA_CPP` | **OFF** | Optional llama.cpp adapter; needs external tree/target |
| `LANGGRAPH_CPP_ENABLE_PYTHON_LANGGRAPH_CONFORMANCE` | OFF | Optional upstream conformance |
| `LANGGRAPH_CPP_BUILD_FUZZERS` | OFF | Clang libFuzzer harnesses |

llama.cpp helper caches: `LANGGRAPH_CPP_LLAMA_CPP_DIR`, `LANGGRAPH_CPP_LLAMA_TARGET`.

## Primary Validation

```sh
cmake --preset unix-debug
cmake --build --preset unix-debug
ctest --test-dir build/unix-debug --output-on-failure
```

Helper: `./scripts/build.sh`

Architecture / context gates:

```sh
scripts/check-dependency-policy.sh
scripts/check-context-skills.sh
```

Docs snippets:

```sh
ctest --test-dir build/unix-debug -L docs --output-on-failure
```

Focused runtime / persistence tests:

```sh
ctest --test-dir build/unix-debug -R "graph|compat|checkpoint|store|crash" --output-on-failure
```

Default example smoke:

```sh
scripts/run-examples.sh
```

Optional llama.cpp examples are **not** part of the default gate. Enable only with
`LANGGRAPH_CPP_WITH_LLAMA_CPP=ON` plus an external llama.cpp setup and GGUF model.

## Typical Example Binaries

```sh
build/unix-debug/examples/minimal_graph
build/unix-debug/examples/sqlite_checkpoint_resume
build/unix-debug/examples/model_tool_model_loop
build/unix-debug/examples/human_interrupt
build/unix-debug/examples/mock_edge_repair
```

## Layer Link Rule

- Foundation owns optional third-party deps (OpenSSL, httplib, SQLite, zlib, spdlog).
- `core` (`lgc::core`) links `lgc::foundation` only; it is the opinionated composition/lifecycle layer.
- `langgraph` links `lgc::foundation` (+ optional llama.cpp); it does not link `lgc::core`.
- llama.cpp is a **langgraph runtime** optional concern; its link flags stay out of foundation so
  foundation never sees them (see comments in root `CMakeLists.txt`).
- Enforced by `scripts/check-dependency-policy.sh` (includes + CMake link edges).

## Contracts Snapshot

Pinned assistant-facing copies live in [`AUTHORITY.md`](AUTHORITY.md). Live numbers come from
[`PROJECT_MANIFEST.json`](../PROJECT_MANIFEST.json):

- `api_contract_version`
- `schema_contract_version`
- checkpoint / content envelope / storage schema versions
- `abi_stable: false` before 1.0

## Quality Scripts

| Script | Purpose |
| --- | --- |
| `scripts/check-dependency-policy.sh` | foundation/core/langgraph layering, public-header constraints, optional CMake options |
| `scripts/check-context-skills.sh` | required context files, entrypoint wiring, authority pin sync, docs closed-loop markers |
| `scripts/run-examples.sh` | Default example smoke + report |
| `scripts/generate-quality-report.sh` | Local quality report |
| `scripts/coverage.sh` | Coverage helper |

## Do Not Treat As Source

- `build/` outputs
- generated reports under local worktrees unless the task asks to refresh checked-in report docs
- `third_party/` (unless explicitly requested)
