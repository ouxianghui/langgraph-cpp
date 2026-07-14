# API Reference

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-10 |
| 范围 | `include/langgraph_cpp/langgraph.hpp` 可达的核心源码 API |
| 关联文档 | [API_CONTRACT.md](API_CONTRACT.md)、[tutorials/API_EXAMPLES.md](tutorials/API_EXAMPLES.md) |

本文是面向使用者和 AI 工具的核心 API 索引。稳定性边界以 [API_CONTRACT.md](API_CONTRACT.md) 为准；本文不替代头文件。

## 1. 公共入口

| 入口 | 用途 |
| --- | --- |
| `#include <langgraph_cpp/langgraph.hpp>` | 推荐公共聚合头。 |
| `lc` namespace | 所有公开 runtime 类型所在 namespace。 |
| `lc::Status` / `lc::Result<T>` | recoverable error contract。 |
| `lc::State` / `lc::StateUpdate` | JSON-backed state 和 partial update。 |

## 2. Graph Builder

`lc::StateGraph` 是可变 graph declaration builder，不是执行引擎。构建完成后调用 `compile()` 得到不可变 `CompiledStateGraph`。

| API | 说明 |
| --- | --- |
| `setNodeDefaults(NodeOptions)` | 设置 node 默认 retry/timeout/error handler 等选项。 |
| `setSchemas(StateSchemaOptions)` | 设置 input/state/output schema。 |
| `addNode(NodeId, NodeHandler)` | 添加返回 `StateUpdate` 的 node。 |
| `addNode(NodeId, NodeOutputHandler)` | 添加可返回 update、command、interrupt、send 的 node。 |
| `addSubgraph(NodeId, shared_ptr<CompiledStateGraph>, SubgraphOptions)` | 将 compiled subgraph 作为父图 node。 |
| `addEdge(source, target)` | 添加普通确定性边。 |
| `addEdge(vector<sources>, target)` | 多 source fan-in 边。 |
| `addSequence(...)` | 添加线性 node 序列。 |
| `setEntryPoint(target)` | 设置 `START -> target`。 |
| `setFinishPoint(source)` | 设置 `source -> END`。 |
| `setConditionalEntryPoint(router, destinations)` | 从 `START` 走条件路由。 |
| `addConditionalEdges(source, router, destinations)` | 添加 router-driven edges。 |
| `addCommandRoute(source, destinations)` | 声明 `Command::goto*` 可去的目标。 |
| `validate()` | 检查声明是否合法。 |
| `compile()` | 验证并返回 `CompiledStateGraph`。 |

## 3. Compiled Runtime

`lc::CompiledStateGraph` 是不可变可执行图，可被多个 run 复用。每次 run 拥有自己的 state、task queue 和 stream projection state。

| API | 说明 |
| --- | --- |
| `invoke(input, options)` | 从 input state 开始普通执行。 |
| `stream(input, options)` | 执行并收集 runtime events。 |
| `streamEvents(input, options, streamOptions)` | 返回 live event stream。 |
| `streamProjected(input, options, projectionOptions)` | 返回 LangGraph-style projected stream parts。 |
| `resume(threadId, options)` | 从 thread latest checkpoint 恢复。 |
| `resumeStream(...)` / `resumeEvents(...)` / `resumeProjected(...)` | resume 的 stream variants。 |
| `getState(threadId, options)` | 获取 latest `StateSnapshot`。 |
| `getState(threadId, checkpointId, options)` | 获取历史 checkpoint snapshot。 |
| `getStateHistory(threadId, options)` | 获取 checkpoint history。 |
| `replay(threadId, checkpointId, options)` | 从历史 checkpoint replay。 |
| `updateState(threadId, update, options, updateOptions)` | 创建 external state update checkpoint，可用于 time-travel fork。 |

## 4. RunOptions 与 Runtime

| 类型 | 用途 |
| --- | --- |
| `RunOptions` | 每次 run 的 thread id、checkpoint namespace、checkpointer、store、executor、durability、resource limits、callbacks 等配置。 |
| `RunnableConfig` bridge | 将 LangGraph-style JSON config 映射到 `RunOptions`。 |
| `Runtime` | node handler 的 per-attempt runtime context。 |
| `StreamWriter` | node 内发出 custom runtime event。 |
| `RunControl` | run-scoped cooperative drain signal。 |

`Runtime` 的关键方法：

| API | 说明 |
| --- | --- |
| `context()` / `previous()` | 读取 run context 和 previous metadata。 |
| `store()` / `checkpointer()` | 访问 injected store/checkpointer。 |
| `executionInfo()` | 读取 run id、thread id、node id、step、task id。 |
| `streamWriter()` | 发出 custom events。 |
| `hasResumeValue()` / `resumeValue()` | 读取 `Command::resume` payload。 |
| `interrupt(id, payload)` | 发起 function-style interrupt。 |

## 5. State、Reducer、Command、Send

| 类型 | 用途 |
| --- | --- |
| `State` | JSON-backed immutable-ish state value passed to nodes。 |
| `StateUpdate` | node 返回的 partial update；不是完整 state snapshot。 |
| `ReducerRegistry` | 控制字段 merge：overwrite、append、add_messages、object merge、custom。 |
| `NodeOutput` | node 可以返回 update、command、interrupt、sends。 |
| `Command` | update + goto / parent goto / resume。 |
| `Send` | dynamic fan-out with branch-local state。 |

## 6. Checkpoint

| 类型 | 用途 |
| --- | --- |
| `BaseCheckpointSaver` | LangGraph-style checkpoint saver contract。 |
| `InMemorySaver` | process-local saver，适合 tests/examples。 |
| `StorageSaver` | `IStorage` backed saver，当前 SQLite path 通过 storage layer 支撑。 |
| `AsyncCheckpointSaver` | executor-backed async facade。 |

`BaseCheckpointSaver` 核心方法：

| API | 说明 |
| --- | --- |
| `put(Checkpoint)` | 写 checkpoint。 |
| `putWrites(CheckpointWriteSet)` | 写 task-level pending writes。 |
| `get(CheckpointQuery)` | 获取 checkpoint。 |
| `getTuple(CheckpointQuery)` | 获取 checkpoint + pending writes tuple。 |
| `list(CheckpointListOptions)` | 列 history。 |
| `deleteThread(threadId)` | 删除 logical thread。 |
| `prune(...)` / `copyThread(...)` / `deleteForRuns(...)` | maintenance operations。 |
| `getDeltaChannelHistory(...)` | delta-channel history surface。 |

## 7. Store

| 类型 | 用途 |
| --- | --- |
| `BaseStore` | LangGraph-style long-term memory interface。 |
| `InMemoryStore` | process-local store。 |
| `StorageStore` | `IStorage` backed store。 |

`BaseStore` 方法：

| API | 说明 |
| --- | --- |
| `batch(vector<StoreOp>)` | 核心虚接口。 |
| `abatch(...)` | async facade。 |
| `put(namespace, key, value)` | 写长期 memory。 |
| `get(namespace, key)` | 读 item。 |
| `search(StoreSearchOptions)` | namespace prefix + filter search；semantic query 当前未实现。 |
| `deleteItem(namespace, key)` | C++ 对 LangGraph `delete` 的关键字避让。 |
| `listNamespaces(options)` | 列 namespace。 |

## 8. Message / Model / Tool

| 类型 | 用途 |
| --- | --- |
| `BaseMessage` | system/human/ai/tool message。 |
| `ToolCall` | assistant tool-call payload。 |
| `AIMessageChunk` | streaming chunk，包含 text/content blocks/tool-call chunks。 |
| `BaseChatModel` | provider-neutral chat model interface。 |
| `FakeChatModel` | deterministic tests/examples model。 |
| `ProviderChatModel` | HTTP provider profiles behind injected `IHttpClient`。 |
| `LlamaCppChatModel` | optional local llama.cpp adapter。 |
| `ToolRegistry` | 显式注册 tools。 |
| `ToolExecutor` | validation、policy、events、invoke。 |
| `ToolNode` | LangGraph-style prebuilt node，执行 latest assistant message 的 tool calls。 |
| `toolsCondition()` | model -> tools -> model loop router predicate。 |

## 9. Foundation 入口

常用 foundation 类型通过公共聚合头可达：

- cancellation：`CancellationToken`
- HTTP：`IHttpClient`、`HttpClientConfig`、`HttpRequest`、`HttpRequestOptions`
- executor：`InlineExecutor`、`ConcurrentExecutor`
- event：`RuntimeEvent`
- JSON schema：`JsonSchema`
- resource limits：`ResourceLimits`
- serialization：state/checkpoint codec
- storage：`IStorage`、`MemoryStorage`
- versioning：contract/schema version helpers

## 10. 使用建议

- 新 graph 先从 `StateGraph` + `compile()` 开始。
- node 不要修改共享全局状态，返回 `StateUpdate` 或 `NodeOutput`。
- 需要恢复、interrupt、replay 时必须配置 checkpointer 和 stable `threadId`。
- 长期记忆使用 `BaseStore`，不要把 store 当 checkpoint history。
- 真 provider、真实硬件和危险工具必须由应用显式注入和授权。
