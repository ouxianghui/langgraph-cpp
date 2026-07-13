# src/langgraph 模块索引

`src/langgraph` 是 `langgraph-cpp` 的业务 runtime 层。它实现 LangGraph-style graph execution、state、checkpoint、store、runtime context、message/model/tool 和 edge adapter surface。

## 1. 设计边界

- 依赖 `src/foundation`，但 `src/foundation` 不反向依赖 `src/langgraph`。
- 默认不依赖 Python、真实 cloud provider、真实硬件或外部 llama.cpp。
- 可选 provider、llama.cpp、SQLite 和 hardware path 必须通过 CMake option 或注入接口接入。
- recoverable errors 通过 `Status` / `Result<T>` 返回。

## 2. 子目录职责

| 目录 | 职责 |
| --- | --- |
| `core` | graph id aliases 和 `START` / `END` 常量。 |
| `graph` | `StateGraph` builder、`CompiledStateGraph` runtime、stream projection、RunnableConfig bridge。 |
| `state` | `StateUpdate`、reducers、`add_messages` 和 state merge。 |
| `checkpoint` | checkpoint records、saver contract、memory/storage savers、async facade。 |
| `store` | long-term namespaced key-value memory。 |
| `runtime` | per-node runtime context、stream writer、interrupt helper。 |
| `message` | BaseMessage、content blocks、tool calls 和 JSON conversion。 |
| `model` | BaseChatModel、fake/provider/optional llama.cpp adapters。 |
| `tool` | tool registry、executor、tool node、schema validation、grammar helpers。 |
| `edge` | hardware adapter interfaces 和 registry。 |

## 3. 关键设计规则

- graph state 只能通过 `StateUpdate` 或 `Command` 修改。
- compiled graph 是不可变执行计划，可被多个 runs 共享。
- 每次 invocation 拥有自己的 run-local state。
- parallel super-step 中 node 可并发，state merge 和 checkpoint 串行。
- checkpoint 是执行历史；store 是长期应用记忆。
- public naming 尽量贴近 LangGraph-style contract，旧兼容别名不长期保留。

## 4. 关联文档

- [../../docs/AI_INDEX.md](../../docs/AI_INDEX.md)
- [../../docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md)
- [../../docs/CONCURRENCY_MODEL.md](../../docs/CONCURRENCY_MODEL.md)
- [../../docs/PERSISTENCE_MODEL.md](../../docs/PERSISTENCE_MODEL.md)
- [../../docs/ERROR_MODEL.md](../../docs/ERROR_MODEL.md)
- [../../docs/API_CONTRACT.md](../../docs/API_CONTRACT.md)

