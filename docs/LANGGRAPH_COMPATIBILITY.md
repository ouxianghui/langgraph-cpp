# LangGraph 兼容性边界

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-10 |
| 范围 | LangGraph-style 命名、行为兼容切片、未支持 parity、验证方式 |
| 关联文档 | [API_CONTRACT.md](API_CONTRACT.md)、[LIMITATIONS.md](LIMITATIONS.md)、[ADR/0004-langgraph-compatibility-boundary.md](ADR/0004-langgraph-compatibility-boundary.md) |

`langgraph-cpp` 是独立 C++ runtime，不是官方 LangGraph 或 LangChain C++ port。项目目标是实现一个清晰的 LangGraph-style 兼容切片，而不是声称完整 Python runtime 同构。

## 1. 已对齐或接近对齐

| 区域 | 当前状态 |
| --- | --- |
| Graph builder naming | `addNode`、`addEdge`、`addConditionalEdges`、`setEntryPoint`、`setFinishPoint`、`addSequence` 等 LangGraph-style 命名。 |
| `START` / `END` | 暴露官方图常量。 |
| `Send` | 支持动态 fan-out 和 branch-local state。 |
| `Command` | 支持 update + goto、parent graph goto、resume。 |
| Reducer | 支持 overwrite、append、`add_messages`、object merge、custom reducer。 |
| Checkpoint saver | `BaseCheckpointSaver`、`InMemorySaver`、`StorageSaver`、`getTuple`、`putWrites`、`list`、`deleteThread`。 |
| Store | `BaseStore::batch()`、`put/get/search/deleteItem/listNamespaces`，保持 C++ keyword 避让。 |
| Interrupt | node interrupt、function-style sequential interrupt、multi-interrupt resume。 |
| Subgraph namespace | 使用 `|` 和 `:` 分隔规则。 |
| RunnableConfig | 支持 tags、metadata、callbacks JSON、configurable、merge/patch/apply 常用语义。 |
| Stream projection | 支持 updates、values、messages、tasks、checkpoints、interrupts、errors、output、events。 |
| Messages | `BaseMessage`、tool calls、LangChain-style content blocks、多模态 block JSON。 |

## 2. 部分对齐

| 区域 | 当前边界 |
| --- | --- |
| Stream v2/v3 | 主字段和 typed part envelope 接近 Python，但不承诺所有 event 细节完全一致。 |
| Provider/tool streaming delta | 支持 OpenAI-compatible / Anthropic 关键 tool-call delta，但不是完整 LangChain provider 生态。 |
| Serializer | 有 JSON/envelope/secure codec，但没有完整 Python `JsonPlusSerializer` / `_DeltaSnapshot` 二进制同构。 |
| Observability | 有 event/trace/metrics surface，但没有 LangSmith-compatible backend 或 trace UI。 |
| Checkpoint backends | 有 memory/storage/SQLite path，没有 Postgres/RocksDB/remote/cloud saver。 |
| Store search | 有 key-value search/filter，没有 semantic/vector backend。 |

## 3. 明确不声称支持

- 官方 LangGraph 完整 upstream parity；
- Python callback manager 执行；
- 完整 LangChain provider ecosystem；
- LangSmith backend 或 trace UI；
- 所有 provider-specific content blocks、reasoning/citation delta；
- Python serializer 二进制格式完全同构；
- 默认 shell/file/network/hardware tools。

## 4. 验证方式

| 验证 | 说明 |
| --- | --- |
| `langgraph_compat_test.cpp` | C++ 内部兼容 golden tests。 |
| `langgraph_conformance_probe.cpp` | 输出可与 Python LangGraph 比较的 C++ 行为。 |
| `python_langgraph_conformance.py` | 可选 Python runner，对比 upstream package。 |
| [API_CONTRACT.md](API_CONTRACT.md) | 记录 public surface 和 contract version。 |
| [LIMITATIONS.md](LIMITATIONS.md) | 记录未支持或 partial parity。 |

## 5. 新增兼容能力规则

1. 先在 [API_CONTRACT.md](API_CONTRACT.md) 记录 public contract。
2. 在 [LIMITATIONS.md](LIMITATIONS.md) 更新 partial/unsupported 边界。
3. 补 C++ compat test。
4. 如果可与 Python 对比，补 conformance probe。
5. 更新 [TRACEABILITY_MATRIX.md](TRACEABILITY_MATRIX.md) 和 [TEST_CATALOG.md](TEST_CATALOG.md)。
