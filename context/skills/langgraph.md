# Skill: LangGraph Runtime Library

Use this when editing or consuming `src/langgraph/**`, building graphs, nodes, models, tools,
checkpoints, stores, streams, interrupts, or edge adapters.

Read [`../CONVENTIONS.md`](../CONVENTIONS.md) first. Module map:
[`../../src/langgraph/README.md`](../../src/langgraph/README.md). API index:
[`../../docs/API_REFERENCE.md`](../../docs/API_REFERENCE.md).

## Role

`src/langgraph` is the **business runtime**: LangGraph-style execution, state/reducers, checkpoint,
store, runtime context, message/model/tool, and edge ports. It depends on foundation; foundation
must not depend back.

Default builds stay free of Python, real cloud providers, real hardware, and external llama.cpp.

## Module Map

| Dir | Use for |
| --- | --- |
| `graph/` | `StateGraph` builder, `CompiledStateGraph`, stream projection, RunnableConfig bridge |
| `state/` | `StateUpdate`, reducers, `add_messages`, merge |
| `checkpoint/` | `BaseCheckpointSaver`, memory/storage savers, pending writes |
| `store/` | `BaseStore` long-term memory |
| `runtime/` | per-node `Runtime`, `StreamWriter`, interrupt helpers |
| `message/` | `BaseMessage`, content blocks, tool calls |
| `model/` | `BaseChatModel`, fake/provider/optional llama.cpp |
| `tool/` | registry, executor, `ToolNode`, schema/grammar helpers |
| `edge/` | hardware adapter interfaces + registry |
| `core/` | `START` / `END`, id aliases only — **not** the `src/core` / `lgc::core` assembly library |

## Canonical Usage Shape

Shape matches `examples/minimal_graph.cpp` (always check `Result` / `Status` before dereference):

```cpp
#include <langgraph_cpp/langgraph.hpp>

lgc::StateGraph graph;
auto status = graph.addNode("hello", [](const lgc::State&, lgc::Runtime&) {
    return lgc::StateUpdate::fromJson(R"({"message":"hello"})");
});
// check status; addEdge(START -> "hello"), addEdge("hello" -> END)...
auto compiled = graph.compile();  // Result<shared_ptr<CompiledStateGraph>>
auto input = lgc::State::fromJson("{}");
auto result = compiled->invoke(*input);
```

Prefer copying patterns from `examples/minimal_graph.cpp` and other examples over inventing APIs.

Rules of thumb:

1. Build with `StateGraph`, execute with `CompiledStateGraph`.
2. Each `invoke` / `stream` / `resume` owns run-local state; plans are immutable and shareable.
3. Nodes return updates/commands; they do not mutate shared graph state in place.
4. Resume / interrupt / replay require a checkpointer and stable logical `threadId_`.
5. Long-term memory uses `BaseStore`; do not overload checkpoint history for app memory.
6. Register tools explicitly; validate inputs; keep dangerous tools out of defaults.
7. Inject models/checkpointers/stores/executors via `RunOptions` / ports — do not hard-code cloud or
   hardware SDKs into core paths.

## Design Rules (Must Follow)

| Rule | Why |
| --- | --- |
| State only via `StateUpdate` / `Command` | ADR-0005; deterministic reducers |
| Super-step merge is serial and deterministic | ADR-0001; parallel nodes only see snapshots |
| Checkpoint at runtime boundaries | ADR-0002; external side effects stay application-owned |
| Store ≠ Checkpoint | ADR-0007 |
| Optional integrations behind ports/options | ADR-0008 |
| Explicit tool registration + schema/policy | ADR-0010 |
| Stream envelope/projection stability | ADR-0009; bump contract if changing |

## Programming Checklist

- [ ] Handler treats input `State` as immutable.
- [ ] No borrowed `Runtime` / `StreamWriter` / `State` refs across async boundaries.
- [ ] Node names avoid `|` and `:`.
- [ ] Checkpoint namespace uses `|` (levels) and `:` (per-invocation) correctly for subgraphs.
- [ ] Parallel failures leave pending writes resume-safe; do not pretend merge already committed.
- [ ] Public behavior changes update API contract / LIMITATIONS / tests / examples as required.
- [ ] Fake/mock paths remain the default for tests and examples.
- [ ] Provider / llama.cpp / hardware stay optional or injected.

## Prefer These Entry APIs

| Goal | API family |
| --- | --- |
| Run to completion | `invoke` |
| Collect events | `stream` |
| Live events | `streamEvents` |
| LangGraph-style parts | `streamProjected` (+ resume variants) |
| Continue after interrupt/crash | `resume` |
| History / time travel | `getState`, `getStateHistory`, `replay`, `updateState` |
| Model↔tool loop | `ToolNode`, `toolsCondition`, `FakeChatModel` / injected `BaseChatModel` |

## Verification Focus

```sh
ctest --test-dir build/unix-debug -R "graph|compat|checkpoint|store|crash" --output-on-failure
# or focused examples:
build/unix-debug/examples/minimal_graph
build/unix-debug/examples/human_interrupt
build/unix-debug/examples/model_tool_model_loop
```

Public API or stream semantics changes also need docs label tests and contract updates.

## Authoritative Docs

- [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md)
- [`docs/API_CONTRACT.md`](../../docs/API_CONTRACT.md)
- [`docs/API_REFERENCE.md`](../../docs/API_REFERENCE.md)
- [`docs/CONCURRENCY_MODEL.md`](../../docs/CONCURRENCY_MODEL.md)
- [`docs/PERSISTENCE_MODEL.md`](../../docs/PERSISTENCE_MODEL.md)
- [`docs/LANGGRAPH_COMPATIBILITY.md`](../../docs/LANGGRAPH_COMPATIBILITY.md)
- [`docs/LIMITATIONS.md`](../../docs/LIMITATIONS.md)
- [`docs/tutorials/API_EXAMPLES.md`](../../docs/tutorials/API_EXAMPLES.md)
- ADRs 0001–0010
