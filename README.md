# langgraph-cpp: LangGraph-Style C++ Runtime for AI Agents, Local LLMs, and Edge Workflows

`langgraph-cpp` is an independent C++23 AI agent framework, graph workflow
engine, and LangGraph-style runtime for building stateful agents and
recoverable workflows in native C++ clients, edge AI systems, robotics
applications, and local LLM apps. It brings graph execution, checkpointing,
streaming events, human-in-the-loop interrupts, structured tool calling,
message/model abstractions, and SQLite-backed persistence to C++ applications
without requiring Python in the core runtime.

> Positioning: a C++-native client and edge intelligent workflow runtime for AI Labs.

> Status: `v0.1.0-alpha` / developer preview. This is an independent,
> community-oriented LangGraph-style runtime, not an official LangGraph or
> LangChain project. The API, ABI, and persisted schemas are still pre-1.0 and
> may change.

The current MVP is designed to help AI labs move agent workflow prototypes
toward desktop clients, local models, edge devices, and recoverable intelligent
applications without requiring real hardware or real model providers in default
builds and tests.

The core source API and persisted schema contract are versioned for edge
runtime work. ABI compatibility is not frozen; breaking source or schema
changes require an explicit contract/schema version bump.

## What It Is For

- AI Lab client-side and edge intelligent workflow experiments.
- Stateful agent workflows in native C++.
- Desktop client runtimes that need local, recoverable AI workflows.
- Edge and robotics control loops that need checkpointed recovery.
- Local or mock model loops with structured tool calls.
- Human-in-the-loop workflows that pause, persist state, and resume.
- Hardware adapter experiments without binding the core runtime to a hardware
  library.

## What It Is Not

- It is not a hosted agent platform.
- It is not an official LangGraph or LangChain C++ port.
- It is not a full LangChain provider ecosystem.
- It is not a general chat application.
- It does not enable shell, file, network, or hardware access by default.
- It does not currently promise a stable ABI or long-term source compatibility.

## MVP Capabilities

The current MVP includes:

- `StateGraph` and `CompiledStateGraph` for graph declaration and execution.
- Multi-active-path fan-out with parallel super-step execution and
  deterministic reducer merge.
- Conditional routers can select one or more next nodes.
- `Send`-style dynamic fan-out with branch-local state and checkpointed resume.
- Subgraph nodes with checkpoint namespaces, persistence modes, and
  `Command::gotoParentNode(s)` parent routing.
- JSON-backed `State` and `StateUpdate`.
- Field reducers: overwrite, append, add_messages, object merge, and custom reducer functions.
- Runtime input/state/output schema validation with the built-in JSON Schema subset.
- `InMemorySaver` and `StorageSaver`, with `checkpoint_ns` as a
  query dimension plus `Async`, `Sync`, and `Exit` durability modes.
- Namespaced `BaseStore`, `InMemoryStore`, and `StorageStore` for long-lived runtime
  memory.
- SQLite-backed storage when `LANGGRAPH_CPP_WITH_SQLITE=ON`.
- Runtime events, collect-style streaming helpers, and bounded live event
  streams via `streamEvents()` / `resumeEvents()`.
- Projected stream parts via `streamProjected()` / `resumeProjected()`,
  including namespace, output-key filtering, subgraph projection controls, and
  LangGraph-style event envelopes plus optional Python v2 typed part envelopes.
- `RunnableConfig` JSON bridge for tags, metadata, arbitrary configurable
  fields, merge/patch helpers, and application into `RunOptions`.
- `StateSnapshot`, checkpoint history, replay, and `updateState()` time-travel
  forks.
- Pending-write recovery for failed parallel super-steps and sync durability
  task-level writes.
- `Interrupt`, function-style sequential interrupts, multi-interrupt
  super-steps, `Command::resume`, `Command` update/goto, and checkpointed
  human-in-the-loop resume.
- Node retry, best-effort timeout checks for synchronous handlers, and error
  handler fallback.
- Optional graph node executor injection and per-super-step concurrency caps.
- `BaseMessage`, `ToolCall`, `BaseChatModel::invoke()`, callback-based
  `BaseChatModel::stream()`, `BaseChatModel::batch()`,
  `BaseChatModel::bindTools()`, `AIMessageChunk`, LangChain-style
  `content_blocks`, multimodal content blocks, standardized token
  `UsageMetadata`, optional `ITokenCounter`, and `FakeChatModel`.
- Optional `LlamaCppChatModel` when `LANGGRAPH_CPP_WITH_LLAMA_CPP=ON`, including
  token-piece streaming callbacks and local token usage accounting.
- `ToolRequest`, `ToolRuntime`, `BaseTool`, `FunctionTool`, `ToolPolicy`,
  `ToolExecutor`, schema validation, structured tool results, and structured
  tool errors. Tool nodes registered through `ToolNode` can route
  with tool-returned `Command`, and tool handlers can request HITL pauses via
  `ToolRuntime::interrupt()`.
- Edge adapter interfaces, `SysfsGpioAdapter`, and a thread-safe
  `EdgeAdapterRegistry`; mock edge examples show hardware-style tools without
  binding core to a hardware library.

## API And Schema Contract

The current contract is documented in
[docs/API_CONTRACT.md](docs/API_CONTRACT.md). The guardrails are:

- C++ source API reachable from `include/langgraph_cpp/langgraph.hpp` is
  governed by API contract version `25`.
- Persisted checkpoint/content/storage schemas are versioned and reject future
  versions by default.
- ABI, private implementation details, and optional provider/hardware adapters
  are not frozen.

## Requirements

- CMake 3.24 or newer.
- A C++23 compiler.
- Git submodules initialized:

```sh
git submodule update --init --recursive
```

The repository vendors `nlohmann_json` and SQLite sources under `third_party`.

## Build

On Linux or macOS:

```sh
cmake --preset unix-debug
cmake --build --preset unix-debug
ctest --test-dir build/unix-debug --output-on-failure
```

Or use the helper script:

```sh
./scripts/build.sh
```

The GitHub Actions workflow in `.github/workflows/ci.yml` is intended to be the
release gate for public contributions. It covers Linux GCC, Linux Clang,
Linux ASAN/UBSAN, Linux TSAN with repeat stress, libFuzzer harness smoke,
Python LangGraph conformance, clang-tidy, and a macOS AppleClang smoke test.

Common CMake options:

```sh
cmake --preset unix-debug \
  -DLANGGRAPH_CPP_BUILD_TESTS=ON \
  -DLANGGRAPH_CPP_BUILD_EXAMPLES=ON \
  -DLANGGRAPH_CPP_WITH_SQLITE=ON
```

Optional Python LangGraph conformance tests are off by default because the core
runtime must build without Python:

```sh
cmake --preset unix-debug-conformance
cmake --build --preset unix-debug-conformance
ctest --preset unix-debug-conformance -L langgraph_conformance
```

The conformance preset requires a Python environment with upstream `langgraph`
installed. Missing Python dependencies, missing C++ probe scenarios, or any
field-level mismatch fail the CTest run.

Pure-logic libFuzzer harnesses are optional and require a Clang toolchain with a
linkable libFuzzer runtime:

```sh
cmake --preset unix-fuzz
cmake --build --preset unix-fuzz
build/unix-fuzz/fuzz/langgraph_cpp_json_schema_fuzzer -runs=1000
```

Some AppleClang installs do not ship `libclang_rt.fuzzer_osx.a`; in that case,
use a full LLVM toolchain and set `CMAKE_C_COMPILER`, `CMAKE_CXX_COMPILER`,
`CMAKE_AR`, and `CMAKE_RANLIB` accordingly.

Enable the optional llama.cpp adapter by pointing CMake at a llama.cpp source
tree or install prefix:

```sh
cmake --preset unix-debug \
  -DLANGGRAPH_CPP_WITH_LLAMA_CPP=ON \
  -DLANGGRAPH_CPP_LLAMA_CPP_DIR=/path/to/llama.cpp
```

When enabled, the local model examples are built:

```sh
build/unix-debug/examples/llama_cpp_chat /path/to/model.gguf "Say hello"
build/unix-debug/examples/llama_cpp_tool_calling /path/to/model.gguf
```

## Five-Minute Example

Build and run the smallest graph:

```sh
cmake --preset unix-debug
cmake --build --preset unix-debug --target minimal_graph
build/unix-debug/examples/minimal_graph
```

Expected output:

```json
{"message":"hello from langgraph-cpp"}
```

The example is intentionally small:

```cpp
#include <langgraph_cpp/langgraph.hpp>

lc::StateGraph graph;
graph.addNode("hello", [](const lc::State&, lc::Runtime&) {
    return lc::StateUpdate::fromJson(R"({"message":"hello from langgraph-cpp"})");
});
graph.setEntryPoint("hello");
graph.setFinishPoint("hello");

auto compiled = graph.compile();
auto input = lc::State::fromJson("{}");
auto result = compiled->invoke(*input);
```

## Example Matrix

See [docs/EXAMPLE_MATRIX.md](docs/EXAMPLE_MATRIX.md) for the full runnable
example matrix. The headline examples are:

| Example | Shows |
| --- | --- |
| `minimal_graph` | `START -> node -> END` |
| `parallel_fanout` | Parallel fan-out/fan-in with reducers |
| `conditional_fanout` | Router-selected parallel fan-out/fan-in |
| `send_map_reduce` | Dynamic Send fan-out with branch-local state |
| `command_goto` | Command update/goto routing |
| `conditional_graph` | Conditional routing |
| `loop_graph` | Cyclic state graph |
| `checkpoint_resume` | Checkpoint and resume with storage |
| `sqlite_checkpoint_resume` | Reopen SQLite storage and resume |
| `time_travel_history` | Checkpoint history, replay, and forked state update |
| `long_term_memory_store` | Cross-run Store memory shared through `RunOptions` |
| `stream_projection` | Stream v3 projected parts and LangGraph-style envelopes |
| `subgraph_module` | Parent workflow composed from a compiled subgraph |
| `model_tool_model_loop` | Mock model, tool call, tool result |
| `agent_pattern_react` | ReAct reasoning/action loop with model and tool nodes |
| `agent_pattern_plan_and_solve` | Plan-first workflow with step execution and final solve |
| `agent_pattern_reflection` | Draft, critique, and revision workflow |
| `llama_cpp_chat` | Optional local GGUF model invocation |
| `human_interrupt` | Checkpointed interrupt and resume |
| `tool_approval_loop` | Human approval before tool execution |
| `edge_mock_tool_adapter` | Mock hardware adapter as tools |
| `mock_edge_repair` | Edge repair workflow with retry and human resume |

## Documentation

Repository guidance:

- [PROJECT_MANIFEST.json](PROJECT_MANIFEST.json): machine-readable project
  manifest for AI tools, repository scanners, and maintainers.
- [AGENTS.md](AGENTS.md) and [CLAUDE.md](CLAUDE.md): coding-agent entrypoints
  for Codex, Claude Code, and other agentic tools.
- [.agent/README.md](.agent/README.md): reusable skill inventory and routing
  guidance for repository-local coding agents.

Project reference docs:

- [docs/AI_INDEX.md](docs/AI_INDEX.md): AI and new-developer index for the
  design evidence chain.
- [docs/PRD.md](docs/PRD.md): MVP requirements and product
  scope.
- [docs/ROADMAP.md](docs/ROADMAP.md): staged technical roadmap.
- [docs/internal/WBS.md](docs/internal/WBS.md): internal implementation work
  breakdown.
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): architecture, class, and
  sequence diagrams for the core runtime.
- [docs/API_REFERENCE.md](docs/API_REFERENCE.md): public API reading map for
  graph, runtime, state, checkpoint, store, message, model, tool, and
  foundation entrypoints.
- [docs/QUALITY_MODEL.md](docs/QUALITY_MODEL.md): quality gates, behavior
  contracts, fuzz/stress, conformance, and release evidence.
- [docs/TRACEABILITY_MATRIX.md](docs/TRACEABILITY_MATRIX.md): requirement to
  design, source, test, example, and release-gate traceability.
- [docs/TEST_CATALOG.md](docs/TEST_CATALOG.md): test ownership and behavior
  contract coverage map.
- [docs/CONCURRENCY_MODEL.md](docs/CONCURRENCY_MODEL.md): graph runtime
  threading and owner-boundary model.
- [docs/ERROR_MODEL.md](docs/ERROR_MODEL.md): Status / Result error contract.
- [docs/PERSISTENCE_MODEL.md](docs/PERSISTENCE_MODEL.md): checkpoint, pending
  writes, namespace, resume/replay, and Store boundaries.
- [docs/SECURITY_MODEL.md](docs/SECURITY_MODEL.md): default permission,
  tool, request-auth, redaction, and observability safety model.
- [docs/LANGGRAPH_COMPATIBILITY.md](docs/LANGGRAPH_COMPATIBILITY.md): explicit
  LangGraph-style compatibility and non-parity boundary.
- [docs/PERFORMANCE_MODEL.md](docs/PERFORMANCE_MODEL.md): performance
  principles, hot paths, and resource-growth boundaries.
- [docs/DEPENDENCY_POLICY.md](docs/DEPENDENCY_POLICY.md): module dependency
  direction rules and static check entrypoint.
- [docs/OWNERSHIP.md](docs/OWNERSHIP.md): module ownership, review routing,
  and CODEOWNERS policy.
- [docs/ADR/README.md](docs/ADR/README.md): architecture decision records.
- [docs/API_CONTRACT.md](docs/API_CONTRACT.md): source API and persisted schema
  contract, including source API and schema stability policy.
- [docs/tutorials/API_EXAMPLES.md](docs/tutorials/API_EXAMPLES.md): compact
  public API examples.
- [docs/EXAMPLE_MATRIX.md](docs/EXAMPLE_MATRIX.md): runnable example matrix.
- [examples/README.md](examples/README.md): example learning path and adding
  examples rules.
- [tests/README.md](tests/README.md): test directory strategy and local test
  commands.
- [docs/LIMITATIONS.md](docs/LIMITATIONS.md): current MVP
  boundaries and deferred integration work.
- [docs/MAINTAINER_GUIDE.md](docs/MAINTAINER_GUIDE.md): maintainer workflow,
  review checklist, and validation guidance.
- [docs/reports/latest-quality-report.md](docs/reports/latest-quality-report.md):
  latest generated local quality report.
- [docs/reports/example-smoke-report.md](docs/reports/example-smoke-report.md):
  latest generated default example smoke report.
- [docs/RELEASE_CHECKLIST.md](docs/RELEASE_CHECKLIST.md): public release gate
  checklist.

## Safety Model

The core runtime only executes user-registered node handlers and tools. It does
not ship privileged tools by default. Applications are expected to register only
the tools they want to expose, validate inputs with JSON Schema, and use
`ToolPolicy` or their own wrapper around hardware, file, network, or shell
capabilities. See [docs/SECURITY_MODEL.md](docs/SECURITY_MODEL.md) for the
detailed permission, auth, redaction, and observability boundary.

## Current Status

This repository is an MVP implementation. The runtime, state, checkpoint,
message, mock model, optional llama.cpp model adapter, tool, interrupt, and mock
edge workflow paths are exercised by examples and tests. Cloud providers, ROS2,
GPIO, UART, CAN, and remote checkpoint stores are intended extension points, not
part of the current stable surface.

The current release-readiness snapshot is documented in
[docs/LIMITATIONS.md](docs/LIMITATIONS.md).

## Contributing, Security, and License

- Contributions: see [CONTRIBUTING.md](CONTRIBUTING.md).
- Security reporting and runtime safety boundaries: see [SECURITY.md](SECURITY.md).
- License: MIT, see [LICENSE](LICENSE).
- Vendored third-party notices: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
