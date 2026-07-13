# 已知限制

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-10 |
| 当前发布线 | `v0.1.0-alpha` / 开发者预览 |
| 关联文档 | [PRD.md](PRD.md)、[ROADMAP.md](ROADMAP.md)、[API_CONTRACT.md](API_CONTRACT.md)、[LANGGRAPH_COMPATIBILITY.md](LANGGRAPH_COMPATIBILITY.md)、[RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) |

本文档记录当前开发者预览版本的边界。这里刻意写得比较明确，这样示例可以被视为发布就绪，而不会暗示所有规划中的 adapter、backend 或 provider 都已经实现。

## API 稳定性

- 公共 API 仍处于 pre-1.0 阶段，后续仍可能变化。
- `StateGraph` 只保留 LangGraph-style camelCase builder API：`addNode()`、
  `addEdge()`、`addConditionalEdges()`、`setEntryPoint()`、
  `setConditionalEntryPoint()`、`setFinishPoint()`、`addSequence()`、
  `validate()`、`addSubgraph()`、`addCommandRoute()` 以及 `set*`
  配置方法；旧的短名 builder 接口已从公共接口移除。
- 字段名目前采用尾随下划线约定，例如 `threadId_` 和 `checkpointer_`。
- `Runtime` 使用 `Runtime::Options` 构造，避免长参数列表在运行时扩展时
  变得易错。
- HTTP request authentication 使用 provider-neutral `IAuthorizationProvider`：
  内置 `NoAuthorizationProvider`、`BearerTokenAuthorizationProvider`、
  `ApiKeyAuthorizationProvider`、`BasicAuthorizationProvider`、
  `OAuthAuthorizationProvider` 和 `FunctionAuthorizationProvider`。
  `IAuthorizationProvider` 只负责 `authorize(HttpRequest&)`；OAuth 等会过期的认证
  通过可选 `IRefreshableAuthorization` 暴露 readiness / refresh gate。OAuth token
  生命周期已合并进 `OAuthAuthorizationProvider`。
- 当前不承诺 ABI 稳定性。
- 聚合头文件 `include/langgraph_cpp/langgraph.hpp` 暴露当前 runtime surface，但
  `SQLiteStorage` 等底层可选组件可能仍需要直接包含 foundation 头文件。

## 执行模型

- 图运行时支持 multi-active-path fan-out：同一 super-step 的 ready nodes 会读取同一个
  state 快照并并行执行，随后按确定性顺序通过 reducer 合并。
- 普通边可以 fan-out 到多个目标；conditional router 可以返回单个目标或多个目标。
- `Send` 风格的动态 fan-out 已支持；Send 分支可以携带各自的 branch-local state，
  pending branch state 会写入 checkpoint 并可 resume。
- 并行 fan-out 可通过 `RunOptions::executor_` 注入执行器，并可通过
  `RunOptions::maxConcurrency_` 限制单个 super-step 的并发 node task 数。
- `Command` 支持节点返回时 update state 并 goto 一个或多个声明目标；subgraph 节点可通过
  `Command::gotoParentNode(s)` 把控制权交回父图的声明目标。
- `StateGraph::addSubgraph()` 支持将已编译子图作为父图节点执行；子图 state diff 会作为
  父图 update 合并。子图 checkpoint 可使用独立 namespace，并可选择 per-invocation、
  per-thread 或 stateless persistence mode。checkpoint namespace 使用官方 LangGraph
  分隔符：`|` 分隔子图层级，per-invocation task/run segment 使用 `:`。子图事件会带
  namespace、parent run id 和 trace path 转发到父图流。
- `StateSnapshot`、`getState()`、`getStateHistory()`、`replay()` 和 `updateState()`
  已支持 latest / historical checkpoint 查询、从历史 checkpoint 继续执行，以及
  基于外部 state update 创建 time-travel fork。从历史 checkpoint replay 后产生的新
  checkpoint 会排在当前 latest step 之后，因此 replay 再次 interrupt 时可继续用同一
  thread resume。
- 并行 super-step 中如果部分节点失败，运行时会把同一步已成功节点的 writes 作为
  pending writes 存入 checkpoint；resume 时只重跑失败节点，并在该 super-step 完成后
  一次性合并 pending writes 与新 writes。
- `RunOptions::durability_` 支持 `Async`、`Sync` 和 `Exit`。默认 `Async` 保持
  super-step 级提交；`Sync` 会在 super-step 内写入 task-level pending writes
  checkpoint；`Exit` 跳过普通中间 step，只保留初始、暂停、失败和完成 checkpoint。
- `RunnableConfig` JSON 桥接已支持 Python LangGraph 常用配置输入：`tags`、`metadata`、
  `callbacks`、`recursion_limit`、`max_concurrency`、`run_name`、`run_id` 和任意
  `configurable` 字段；merge 会拼接 tags、浅合并 metadata/configurable、合并 JSON
  callbacks，patch 会在替换 callbacks 时清除旧的 `run_name` / `run_id`。`applyRunnableConfig()`
  会把 `thread_id`、`checkpoint_ns`、`run_id`、recursion limit 和 max concurrency 映射到
  `RunOptions`，并把 tags/metadata 带入 LangGraph-style event envelope。C++ 仍使用
  `eventCallback_` / `eventSink_` 作为真实回调机制；Python `CallbackManager` 对象不会在
  C++ 运行时内执行。
- `stream()` / `resumeStream()` 是收集式便捷 API，会在 `RunResult::events_` 中返回
  已收集事件；`streamEvents()` / `resumeEvents()` 提供基于 bounded channel 的运行中
  事件读取 API；`streamProjected()` / `resumeProjected()` 提供 updates、values、
  messages、custom、tasks、checkpoints、interrupts、errors、output 等投影流，并支持
  messages chunk envelope、官方 `checkpoint_ns` / namespace path、`outputKeys_`、subgraph projection 过滤，
  以及 LangGraph-style event envelope。updates、tasks、debug 和 checkpoints 投影已压实到
  LangGraph v2/v3 风格主字段；checkpoint stream payload 使用官方 `CheckpointPayload`
  主字段，`config` 和 `parent_config` 使用 LangGraph `RunnableConfig` 风格的
  `configurable` 嵌套对象。调用方可通过 `RunProjectionOptions::version_ =
  StreamProtocolVersion::V2` 请求 Python `stream(..., version="v2")` 风格 typed part envelope：
  每个 `StreamPart::data_` 包含 `type`、`ns` 和 `data`，且 values payload 中的
  `__interrupt__` 会提升为顶层 `interrupts` 字段。
- 同一 super-step 内多个并行节点可同时请求 interrupt；resume 时 `Command::resume`
  可按 interrupt id 或 node id 提供多份 resume payload。
- 节点也可使用 `Runtime::interrupt(id, payload)` 发起函数式 interrupt；
  sequential multi-interrupt 的已恢复值会随 checkpoint metadata 保存并在下一次
  resume 时重放。
- checkpoint 会在运行时定义的 super-step 边界写入；用户节点 handler 或工具内部
  的副作用仍由应用拥有者负责。

## State 与 Schema

- `State` 以 JSON 为底层表示。
- `StateGraph` 支持运行时 input/state/output JSON Schema 校验。
- Reducer 支持 overwrite、append、add_messages、merge object，以及通过
  `ReducerRegistry::set(field, reducer)` 注册的字段级 custom reducer function。
- 尚未实现 typed state wrapper、基于反射的 state schema，以及编译期 state 校验。
- JSON Schema 支持是面向当前 runtime 的实用子集，不是完整覆盖某个 draft 的 validator。

## 模型 Provider

- `FakeChatModel` 已实现，用于测试和示例。
- `BaseChatModel::stream()` 提供 provider-neutral callback contract，
  `BaseChatModel::batch()` 提供顺序批处理，`BaseChatModel::bindTools()` 是 provider
  tool binding 入口；`makeModelNode()` 可在 stream 模式下把 token chunk 转成 runtime
  `Token` event。
- 当 `LANGGRAPH_CPP_WITH_LLAMA_CPP=ON` 时，可以使用 `LlamaCppChatModel`。
- llama.cpp adapter 需要外部 llama.cpp 源码树、安装前缀或已有 CMake target；
  llama.cpp 默认不随仓库 vendored。
- llama.cpp adapter 支持 token-piece streaming callback；真实 token usage 统计仍依赖
  后续 provider-specific metadata。
- Tool-call GBNF 生成支持当前工具使用的实用 JSON Schema 子集：object、array、
  string、number、boolean、null、enum/const、required properties，以及有界的
  optional properties。
- GBNF 约束的是语法，不是意图。Prompt 仍然需要说明要调用哪个工具以及原因；
  真实稳定性取决于所选 GGUF 模型和 llama.cpp 构建。
- `ProviderChatModel` 提供 OpenAI-compatible、Anthropic、Qwen、DeepSeek profile，可通过
  注入的 `IHttpClient` 接入真实 HTTP/SSE transport；默认测试使用 fake transport，不访问
  真实云服务。
- Provider adapter 已覆盖 SSE token streaming、provider usage metadata 透传、prompt
  message/byte trimming、`batch()` 顺序批处理、OpenAI-compatible / Anthropic-style
  tool schema binding、provider tool-call response parsing、LangChain standard
  `content_blocks`、多模态 text/image/audio/file/video block JSON 表达、OpenAI-compatible
  tool-call chunk delta，以及 Anthropic `tool_use` / `input_json_delta` streaming
  归一化为 `tool_call_chunk` content block。
- Provider retry、rate-limit、circuit breaker、TLS、proxy 和 request auth 等策略由注入的
  `IHttpClient` / `IAuthorizationProvider` 配置承接；厂商完整参数面、
  provider-specific 非标准 content block、完整 reasoning/citation delta 细节和生产密钥/
  token 轮换策略仍属于后续扩展。

## 工具与权限

- 工具必须通过 `ToolRegistry::add()` 显式注册，可通过 `tool()`、`spec()`、`has()`、
  `enable()` 和 `disable()` 查询或控制。
- 运行时不内置 shell、filesystem、network 或 hardware 工具。
- 工具输入校验已可用；输出校验可通过 `ToolNodeOptions::validateOutput_` 选择启用。
- `ToolExecutor` 支持基础的 `ToolPolicy::authorize_` hook，并会发出 tool-call 事件。
- 工具可返回 `ToolResult::command(...)`；使用 `ToolNode` 注册的 tool node
  会把工具消息 update 与 tool-returned `Command` 合并后交给图运行时路由。
- 工具 handler 可通过 `ToolRuntime::interrupt(id, payload)` 发起 HITL 暂停；resume 后
  tool node 会重跑该工具并把 `Command::resume()` payload 返回给 handler。
- Node-level retry、同步 handler 返回后的 timeout 检查，以及 error handler fallback 已支持。
- Tool-level circuit-breaker、approval 和 sandbox middleware 仍属于后续扩展工作。

## Checkpoint 与存储

- `InMemorySaver` 是进程本地实现，面向测试和示例。
- 持久化 checkpoint 目前通过基于 `SQLiteStorage` 的 `StorageSaver` 演示；
  `checkpoint_ns` 是 latest/get/list 的查询维度。
- `BaseCheckpointSaver` 已收敛为正交接口：`put(Checkpoint)`、`putWrites(CheckpointWriteSet)`、
  `get(CheckpointQuery)`、`list(CheckpointListOptions)` 和 `deleteThread()`。
  `CheckpointTuple` 会把 checkpoint 本体与独立持久化的 pending writes 一起返回；
  list 支持 namespace、checkpoint id、before、limit、metadata filter 和顺序选项。
  `BaseCheckpointSaver` 提供 `prune()`、`copyThread()`、`deleteForRuns()` 和
  `getDeltaChannelHistory()` 这组维护能力；
  基于 executor 的 `AsyncCheckpointSaver` facade 覆盖核心 saver 方法和这些维护能力。
- SQLite durability 已有 process hard-exit / timeout-kill crash-recovery harness，覆盖
  WAL/DELETE 与 NORMAL/FULL/EXTRA synchronous reopen 矩阵、checkpoint/store 持久化、
  checkpoint row 已写入但 latest pointer 未更新、latest pointer stale/dangling reconcile、
  partial pending-write 恢复、multi-process contention，以及 checkpoint/store corruption
  error reporting。
- `StorageSaver` 提供保留策略、latest pointer 自动修复和 run-id 级 checkpoint 删除；
  `copyThread()` / `getDeltaChannelHistory()` 由 `BaseCheckpointSaver` 统一暴露。
- `BaseStore` / `InMemoryStore` / `StorageStore` 提供 namespaced key-value 长期记忆接口，并
  已收敛到官方 LangGraph 的 batch/op 形状：`batch()` 是核心虚接口，便捷方法包括
  `put()`、`get()`、`search()`、`deleteItem()` 和 `listNamespaces()`；`deleteItem()` 是
  C++ 对官方 `delete` 方法名的关键字避让，并映射为 `StorePutOp` 空 value 删除语义。
  `search()` 返回 `StoreSearchItem`，支持 namespace prefix、limit/offset 和 JSON filter；
  当前后端未配置向量索引，因此传入 `query_` 会返回 `StatusCode::Unimplemented`。向量索引、
  TTL、remote store adapter，以及独立的生产维护/admin API 仍属于后续扩展。
- persisted checkpoint payload schema 已包含 `checkpoint_ns`、`task_id`、`task_path`、`next_tasks`、
  write order、write-local next tasks、metadata、`pending_writes`、`channel_versions`、
  `versions_seen` 和 `updated_channels`，用于恢复 Send 分支、本地 command routing、
  函数式 interrupt scratchpad，以及失败或 sync durability task-level writes 的未提交写入。
  这些内部恢复字段不会作为 checkpoint stream payload 顶层字段暴露。
- `StorageSaverOptions::codec_` 是 checkpoint serializer/encryption 插拔点；
  默认使用 JSON checkpoint codec，也可以换成 envelope 或 secure checkpoint codec。
  目前还没有 Python `JsonPlusSerializer` / `_DeltaSnapshot` 二进制格式同构。
- 目前还没有 RocksDB、Postgres、remote checkpoint store 或云持久化 adapter；这些应作为
  可选 `IStorage` / `BaseCheckpointSaver` adapter 增量接入，而不是成为默认 edge runtime 依赖。
- SQLite 示例可能会在最终 JSON 输出前打印 logger 行，因为默认 logger 处于启用状态。

## 边缘与硬件

- Edge workflow 通过 mock adapter 和 sysfs GPIO adapter 演示；sysfs adapter 需要应用显式
  构造和注册，不会默认启用硬件访问。
- GPIO、UART、I2C、CAN 和 ROS2 action/service 的接口草案可在
  `langgraph/edge/hardware.hpp` 中使用。
- `EdgeAdapterRegistry` 可由应用通过 `set(adapter)` 注册真实 adapter，并通过
  `find<Adapter>()` / `require<Adapter>()` 向工具层暴露当前 capabilities。
- UART、I2C、CAN 和 ROS2 的真实 binding 尚未实现或 vendored。
- Mock edge repair workflow 展示了预期形态：diagnose、tool call、retry、checkpoint、
  interrupt、resume，以及最终 verification。

## 平台

- 当前开发路径是 Linux/macOS + CMake。
- Windows preset 已存在，但本次 release readiness 验证的是 `unix-debug`。
- Embedded Linux 和 ARM64 是产品目标，但交叉编译验证仍是未来工作。

## 可观测性

- Runtime event、内存事件收集、callback、custom event、hierarchical trace metadata 和
  LangGraph-style stream event envelope 已存在；compat golden tests 覆盖 stream envelope
  身份字段、subgraph namespace/parent ids 和 token chunk 结构。
- 目前还没有 LangSmith-compatible backend、trace UI、OpenTelemetry exporter 或托管式
  observability service。

## 发布就绪快照

已在 2026-07-10 使用以下命令在 macOS / AppleClang 开发环境验证：

```sh
cmake --preset unix-debug
cmake --build --preset unix-debug
ctest --test-dir build/unix-debug --output-on-failure
```

[EXAMPLE_MATRIX.md](EXAMPLE_MATRIX.md) 中的非可选示例已随 `unix-debug` 构建编译。可选的
`llama_cpp_chat` 示例需要外部 llama.cpp 构建和 GGUF 模型。

开源发布门禁已在 `.github/workflows/ci.yml` 中补齐 Linux GCC、Linux Clang、
Linux ASAN/UBSAN、Linux TSAN、clang-tidy 和 macOS smoke test。真实 Linux/GCC
和 Linux sanitizer 结果以 GitHub Actions runner 为准；本地 macOS sanitizer 不能替代
Linux release gate。

## 风险摘要

这些风险是当前限制的运营视角，用于判断是否可以公开发布或升级 release 阶段。

| 风险 | 影响 | 缓解 / 跟踪 |
| --- | --- | --- |
| Upstream LangGraph 语义继续变化。 | compat 文档和实际行为漂移。 | 明确兼容边界，维护 conformance probe；见 [LANGGRAPH_COMPATIBILITY.md](LANGGRAPH_COMPATIBILITY.md)。 |
| Python serializer 二进制格式未同构。 | 与 Python checkpoint 互操作受限。 | 明确写在限制中，未来作为独立 adapter。 |
| RocksDB/Postgres/remote checkpoint backend 未实现。 | 生产部署 durability 选择有限。 | 保持 saver/storage 可插拔，先稳定 SQLite contract。 |
| Semantic/vector store 未实现。 | 长期记忆搜索能力有限。 | `query_` 相关能力保持显式未实现，不假装支持。 |
| Provider/tool streaming delta 生态很宽。 | 部分模型输出不兼容。 | core provider-neutral，profile 增量扩展。 |
| AppleClang 通过但 Linux/GCC 暴露问题。 | 开源后跨平台失败。 | release gate 以 Linux GCC/Clang + sanitizer 为主。 |
| 并发、shutdown 或 crash recovery 覆盖不足。 | 死锁、数据竞争、断电后无法恢复。 | TSAN、repeat stress、crash recovery 和 failure injection tests。 |
| Tool/hardware 副作用不可回滚。 | 真实设备或外部系统出现重复操作。 | 应用提供 idempotency、approval、audit、compensation；见 [SECURITY_MODEL.md](SECURITY_MODEL.md)。 |
| 日志或 trace 泄露 secret。 | 安全事件。 | redaction wrapper、低敏字段、测试覆盖。 |
| 文档声明超过实现。 | 用户误用、项目可信度下降。 | [TRACEABILITY_MATRIX.md](TRACEABILITY_MATRIX.md)、本文和 [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) 共同约束。 |
| 性能数据不足。 | 无法判断高负载场景可用性。 | [PERFORMANCE_MODEL.md](PERFORMANCE_MODEL.md) 记录 benchmark 工作负载，不用未测数据做承诺。 |
