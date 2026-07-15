# API 与 Schema 合同

`langgraph-cpp` 用显式版本号跟踪 edge-runtime 的源码 API 合同和持久化 schema 合同。当前 API 合同版本是 `25`；持久化 schema 合同版本仍是 `1`。

这是源码兼容和数据兼容承诺，不是 ABI 承诺。跨版本升级时，调用方仍应预期需要重新从源码构建。`1.0` 前可以演进，但不能含糊：破坏性变更必须提升合同版本、删除旧接口、同步测试和文档。

## 1. 合同范围

冻结的源码 API 是从 `include/langgraph_cpp/langgraph.hpp` 可触达的公共 surface，包括：

- `StateGraph`、`CompiledStateGraph`、`RunOptions`、`RunResult`、`StateSnapshot`、`Command`、`Send`、interrupt、stream projection、subgraph 和 reducer；
- checkpointer、store、message、model、tool 和 edge adapter 接口；
- runtime surface 必需的 foundation 值类型和服务接口，例如 `Status`、`Result<T>`、storage、serialization、resource limits、cancellation、executor 和 event 类型。

在 API 合同版本 `25` 内，变更应尽量保持 additive 和源码兼容。删除公共名称、改变必填字段、改变默认 runtime 语义，或改变可恢复错误码，都需要显式提升 API 合同版本。

## 2. API 合同版本 `25`

版本 `25` 将模型 token usage 从 provider raw JSON 透传收敛为标准 usage contract。

- 新增 `TokenUsage`、`UsageMetadata` 和 `UsageMetadataSource`，`usage_metadata` 序列化使用
  `input_tokens`、`output_tokens`、`total_tokens`、`source`、`provider`、`model` 和 `raw` 形状。
- `BaseMessage::usageMetadata_` 从任意 JSON 改为 typed `UsageMetadata`；`AIMessageChunk`
  新增 `usageMetadata_`，同时在 `metadata["usage"]` 中保留标准 JSON 形状，供 stream event 使用。
- 新增可选 `ITokenCounter`，用于 provider 未返回 usage 或返回不完整 usage 时补齐本地 token count。
- `ProviderChatModel` 会把 OpenAI-compatible/Qwen/DeepSeek 的
  `prompt_tokens`、`completion_tokens`、`total_tokens`，以及 Anthropic 的
  `input_tokens`、`output_tokens` 归一化为 `TokenUsage`，并保留原始 provider payload 到 `raw`。
- `LlamaCppChatModel` 实现 `ITokenCounter`，并在最终 assistant message/done chunk 中写入本地
  `input_tokens`、`output_tokens` 和 `total_tokens`。

## 3. API 合同版本 `24`

版本 `24` 将 HTTP request 执行策略从隐式 client-only 配置改为显式 per-request contract。

- 新增 `HttpRequestOptions`，用于表达 request-level `timeout_`、`deadline_` 和 buffered request `retryPolicy_` override。
- `IHttpClient::send()`、`sendAsync()`、`sendStreaming()` 和 `sendSse()` 均要求调用方显式传入 `HttpRequestOptions`；旧的无 options 方法签名已移除。
- `HttpClientConfig` 仍提供默认 connect/read/write timeout 和 retry policy；单次请求的 options 会覆盖 retry policy，并用 timeout/deadline 截断连接池等待、retry delay 和底层 transport timeout。
- `ProviderChatModelOptions` 新增 `requestOptions_`，provider invoke/stream 会把 request budget 传入注入的 `IHttpClient`。

## 4. API 合同版本 `23`

版本 `23` 进一步简化 request-auth API。

- `IAuthorizationProvider` 只暴露 `authorize(HttpRequest&)`。
- 会过期或可刷新的凭证通过可选接口 `IRefreshableAuthorization` 暴露 `onReady()`、`gate()` 和 `requestRefresh()`。
- OAuth 状态管理合并进 `OAuthAuthorizationProvider`，该类直接拥有 `OAuthCredentials`、access-token expiry、refresh-token 访问能力和 renew hooks。
- 旧的公共 `OAuthTokenProvider` 类型和 `OAuthAuthorizationProvider(std::shared_ptr<OAuthTokenProvider>)` 构造函数不再公开。

版本 `23` 还规定：历史 `CompiledStateGraph::replay()` 如果写入新 checkpoint，新 checkpoint 会接在该 thread 和 checkpoint namespace 当前 latest step 之后。这保留 time-travel 历史，同时让 replay 后再次 interrupt 的分支可以继续通过普通 `resume(thread_id)` 路径恢复。

## 5. 近期 API 合同变更摘要

| 版本 | 主题 | 当前合同 |
| --- | --- | --- |
| `25` | Token usage metadata | `BaseMessage` 和 `AIMessageChunk` 使用标准 `UsageMetadata`；provider usage 和 llama.cpp 本地计数归一化为 `TokenUsage`。 |
| `24` | HTTP request options | `IHttpClient` 所有 request entrypoint 显式接收 `HttpRequestOptions`；旧无 options 签名移除。 |
| `23` | OAuth request auth | `IAuthorizationProvider` 只负责 `authorize(HttpRequest&)`；可刷新凭证通过 `IRefreshableAuthorization` 暴露 readiness/refresh gate。 |
| `22` | HTTP request auth | HTTP client 使用 provider-neutral `IAuthorizationProvider`；旧 `IOAuthTokenProvider` 和 `oauthTokenProvider()` 不再公开。 |
| `21` | Store | `BaseStore::batch()` 是核心虚接口；`abatch()` 是 async facade；便捷方法包括 `put`、`get`、`search`、`deleteItem`、`listNamespaces`。 |
| `20` | Stream / RunnableConfig / content blocks | stream projection 更接近 LangGraph v2/v3；新增 `StreamProtocolVersion`；message/model stream 支持 LangChain-style content blocks；`RunnableConfig` 保留 tags、metadata、callbacks 和任意 `configurable` 字段。 |
| `19` | Checkpoint config shape | checkpoint stream payload 和 conformance snapshot 使用 `{"configurable": {...}}` 形式表达 `thread_id`、`checkpoint_id` 和 `checkpoint_ns`。 |
| `18` | Namespace | subgraph checkpoint namespace 使用官方分隔符：`|` 分隔层级，`:` 追加 per-invocation task/run segment。 |
| `17` | Graph ids | `core/ids.hpp` 只公开 C++ domain aliases 以及官方 `START` / `END` 常量；用户 node name 不能包含 `|` 或 `:`。 |
| `16` | Graph runtime naming | 可执行图类型是 `CompiledStateGraph`；保留 LangGraph-style builder/runtime 名称；旧 `CompiledGraph` 等名称不再公开。 |
| `15` | Checkpoint saver naming | checkpoint 持久化接口收敛为 `BaseCheckpointSaver`、`InMemorySaver`、`StorageSaver`、`AsyncCheckpointSaver`。 |
| `14` | Message | 消息类型收敛为 `BaseMessage`、`MessageType::{System, Human, AI, Tool}` 和 `ToolCall::args_`。 |
| `13` | Chat model | 模型接口收敛为 `BaseChatModel`、`AIMessageChunk`、`ToolCallChunk`、`batch()` 和 `bindTools()`。 |
| `12` | Runtime | node handler 接收 `Runtime&`；运行时上下文通过 `context()`、`store()`、`streamWriter()`、`executionInfo()` 等接口访问。 |
| `11` | State update | `StateUpdate::values()` 是 partial update payload；`ReducerKind::AddMessages` 实现 append-or-replace-by-message-id。 |
| `10` | Store naming | store 接口命名收敛为 `BaseStore`、`InMemoryStore`、`StoreSearchItem`；`deleteItem()` 是 C++ 对 LangGraph `delete` 的关键字避让。 |
| `9` | Tool runtime | tool surface 收敛为 `ToolNode`、`ToolRuntime`、`BaseTool`、`ToolNodeOptions::messagesKey_` 和 `toolsCondition()`。 |
| `8` | Graph builder | 只公开 `addNode`、`addEdge`、`addConditionalEdges`、`setEntryPoint`、`setConditionalEntryPoint`、`setFinishPoint`、`addSubgraph`、`addCommandRoute` 和 schema setters。 |
| `7` | Checkpointer compatibility removal | `BaseCheckpointSaver` 只保留当前 `put`、`putWrites`、`get`、`getTuple`、`list`、`deleteThread` 以及维护方法；旧兼容别名移除。 |
| `6` | Checkpointer maintenance | retention、repair、copy、run deletion 和 delta history 能力放在 `BaseCheckpointSaver` 上。 |
| `5` | Async checkpoint shim | 移除旧的 `IAsyncCheckpointer` / `AsyncCheckpointerAdapter` shim；保留 `AsyncCheckpointSaver`。 |
| `4` | Extension interface cleanup | `Runtime` 使用 `Runtime::Options`；tool、reducer、edge registry 接口继续简化。 |
| `3` | Checkpoint tuple | checkpoint reads 使用 `get(CheckpointQuery)` / `list(CheckpointListOptions)`，并返回 `CheckpointTuple`。 |
| `2` | Builder alias cleanup | 旧 builder alias 集合被移除；版本 `8` 进一步明确 LangGraph-style builder API 是唯一公共 surface。 |

## 6. 持久化 Schema 合同

持久化 schema 合同版本是 `1`。当前由测试保护的组件 schema 版本如下：

| Schema | 当前版本 |
| --- | ---: |
| API contract | `25` |
| Schema contract | `1` |
| Checkpoint JSON schema | `3` |
| Content envelope | `1` |
| Storage schema | `1` |

除非显式加入 forward-compatible migration，否则 reader 读到未来版本时必须返回 `StatusCode::Unimplemented`。历史版本只在各组件声明的最低支持版本范围内保持支持。

## 7. 稳定性范围

| 范围 | 当前承诺 |
| --- | --- |
| C++ 源码 API | 由本文的 API contract version 管理。 |
| 持久化 schema | checkpoint、content envelope、storage schema 有独立版本，默认拒绝未来版本。 |
| ABI | `1.0` 前不承诺 ABI 稳定，调用方应从源码重新构建。 |
| 私有实现 | `.cpp`、内部 `.hh`、test helpers、未导出的 helper 不冻结。 |
| Optional adapters | provider、hardware、llama.cpp、future backend adapters 的内部实现不冻结。 |

## 8. 不冻结的内容

- C++ ABI 和二进制布局。
- 私有实现文件、内部 helper 函数和 `.hh` 内部头。
- 生成的构建产物和测试专用 helper。
- 可选 provider、hardware、llama.cpp adapter 的内部实现细节。
- 性能特征，除非测试或文档明确声明了约束。

## 9. 变更规则

- 新公共 API 优先 additive，不复活旧兼容别名。
- 如果新名称落地，旧接口应删除，而不是长期双轨兼容。
- 可恢复错误必须继续通过 `Status` / `Result<T>` 返回。
- 持久化格式变更必须更新 schema version、迁移策略和兼容测试。
- 改变 LangGraph-style 行为时，应同步更新 conformance probe、示例和 [LIMITATIONS.md](LIMITATIONS.md)。
- Public type/member rename 视为破坏性变更：提升 API contract version，删除旧接口，不长期双轨兼容。
- Error code/default semantic change 视为破坏性变更：更新 contract、tests、migration notes。
- Persisted schema change 是高风险变更：提升 schema version，补 migration/compat tests。

破坏性变更流程：

1. 在本文增加新 contract version 说明。
2. 删除旧公共接口，不做长期兼容别名。
3. 更新所有 examples、tests、docs snippet。
4. 更新 [LIMITATIONS.md](LIMITATIONS.md)，说明仍不支持或 partial 的范围。
5. 更新 [TRACEABILITY_MATRIX.md](TRACEABILITY_MATRIX.md) 和 [TEST_CATALOG.md](TEST_CATALOG.md)。
6. 如果影响 LangGraph-style 行为，更新 conformance probe 或 golden test。
7. 在 release notes 中明确迁移动作。

持久化 schema 变更规则：

- 写入新字段时应优先保持旧 reader 可忽略。
- 删除或重命名字段必须提升 schema version。
- reader 遇到未来版本应返回 `StatusCode::Unimplemented`，不能 silent best-effort。
- migration 必须是显式函数或工具，不应隐式改写用户数据。
- checkpoint、pending writes、latest pointer、namespace 和 run metadata 的变更必须补 crash/recovery 测试。

命名策略：

- 保留 LangGraph 生态中的协议词，例如 `thread_id` 和 `checkpoint_ns`。
- C++ 关键字冲突时使用 C++ 风格避让，例如 `deleteItem()` 对应 LangGraph store `delete`。
- 新 public API 使用一致的 camelCase 方法命名。
- 旧接口移除后，不保留“为了兼容”的 duplicate method。

## 9. 验证

默认验证不依赖 Python：

```sh
cmake --preset unix-debug
cmake --build --preset unix-debug
ctest --test-dir build/unix-debug --output-on-failure
```

API/schema 版本 pin 位于 `tests/versioning_test.cpp`。

API 或 schema 变更至少需要：

```sh
ctest --test-dir build/unix-debug -R "versioning|docs|compat|checkpoint|storage|graph" --output-on-failure
git diff --check
```

如果变更影响 runtime 行为，还应运行默认全量 CTest 和对应 sanitizer/conformance gate。

可选 upstream LangGraph 行为一致性验证需要显式启用：

```sh
cmake --preset unix-debug-conformance
cmake --build --preset unix-debug-conformance
ctest --preset unix-debug-conformance -L langgraph_conformance
```

该 CTest 需要本地 Python 环境安装 upstream `langgraph` 包。它会比较 C++ probe 与 Python LangGraph 在 `RunnableConfig` merge/patch/apply、`StateSnapshot` shape/history order、checkpoint namespace shape、interrupt replay/resume、parallel/sequential multi-interrupt resume、`Command(goto+update)`、`Send` fan-out/reducer 和 subgraph boundary 等场景的行为。probe 也会输出 stream envelope、stream projection、interrupt/error task events 和 tool-returned `Command` 的 C++ golden shape。

## 9. 关联文档

- [PRD.md](PRD.md)：产品目标和验收标准。
- [ARCHITECTURE.md](ARCHITECTURE.md)：模块边界和运行模型。
- [AI_INDEX.md](AI_INDEX.md)：工程入口和设计证据链。
- [QUALITY_MODEL.md](QUALITY_MODEL.md)：质量门禁和行为契约。
- [LANGGRAPH_COMPATIBILITY.md](LANGGRAPH_COMPATIBILITY.md)：LangGraph-style 兼容边界。
- [ERROR_MODEL.md](ERROR_MODEL.md)：错误传播模型。
- [PERSISTENCE_MODEL.md](PERSISTENCE_MODEL.md)：持久化语义。
- [tutorials/API_EXAMPLES.md](tutorials/API_EXAMPLES.md)：公共 API 示例。
- [LIMITATIONS.md](LIMITATIONS.md)：当前不支持或延后的能力。
- [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md)：发布前检查项。
