# src/langgraph/checkpoint

本目录实现 graph checkpoint contract。checkpoint 用于恢复执行历史，不是长期业务记忆。

## 1. 核心类型

| 类型 | 职责 |
| --- | --- |
| `Checkpoint` | 保存 graph state、step、next tasks、metadata、channel versions 等恢复信息。 |
| `CheckpointWriteSet` | task-level writes，用于 pending writes 和 sync durability。 |
| `CheckpointTuple` | checkpoint 本体加独立持久化的 pending writes。 |
| `BaseCheckpointSaver` | LangGraph-style saver contract。 |
| `InMemorySaver` | 测试和单进程 demo 用 memory saver。 |
| `StorageSaver` | 基于 `IStorage` 的持久化 saver。 |
| `AsyncCheckpointSaver` | future-returning facade。 |

## 2. Contract

`BaseCheckpointSaver` 暴露：

- `put`
- `putWrites`
- `get`
- `getTuple`
- `list`
- `deleteThread`
- `prune`
- `copyThread`
- `deleteForRuns`
- `getDeltaChannelHistory`

旧 checkpointer 命名和兼容别名不属于当前 public contract。

## 3. 语义规则

- `thread_id` 是 logical thread，不是 C++ OS thread。
- `checkpoint_ns` 是查询和隔离维度。
- pending writes 用于恢复失败 super-step 中已成功 task 的输出。
- maintenance API 属于 saver contract，而不是 Store contract。
- serializer/encryption 通过 `StorageSaverOptions::codec_` 注入。

## 4. 关联文档

- [../../../docs/PERSISTENCE_MODEL.md](../../../docs/PERSISTENCE_MODEL.md)
- [../../../docs/API_CONTRACT.md](../../../docs/API_CONTRACT.md)
- [../../../docs/ADR/0002-checkpoint-boundary.md](../../../docs/ADR/0002-checkpoint-boundary.md)

