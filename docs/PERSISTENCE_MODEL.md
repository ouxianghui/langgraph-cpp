# 持久化模型

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-10 |
| 范围 | checkpoint、pending writes、namespace、resume、replay、store |
| 关联文档 | [ARCHITECTURE.md](ARCHITECTURE.md)、[API_CONTRACT.md](API_CONTRACT.md)、[QUALITY_MODEL.md](QUALITY_MODEL.md)、[ADR/0002-checkpoint-boundary.md](ADR/0002-checkpoint-boundary.md) |

本文说明 `langgraph-cpp` 的持久化语义。重点是区分 checkpoint 与 store，并说明 runtime 在哪些边界写入 checkpoint。

## 1. 核心概念

| 概念 | 作用 |
| --- | --- |
| checkpoint | 某个 logical thread 的执行历史快照，用于 resume、history、replay。 |
| pending writes | super-step 内部分成功 task 的未提交 writes，用于失败恢复。 |
| checkpoint namespace | 区分 root graph、subgraph 和 per-invocation 子运行的持久化空间。 |
| thread id | LangGraph-style logical thread，不是 C++ OS thread。 |
| checkpoint id | 单个 checkpoint record 的稳定标识。 |
| store | 长期应用记忆，可跨 run 或 thread 使用，不等同于 checkpoint。 |

## 2. Checkpoint 写入边界

runtime 在以下边界写 checkpoint：

- initial checkpoint；
- 每个正常 super-step 完成；
- interrupt pause 前；
- failure 前，带可恢复 pending writes；
- completion；
- `updateState()` 创建 fork；
- `Durability::Sync` 下的 task-level writes。

不保证在用户 handler 的任意副作用边界写 checkpoint。外部副作用的幂等性仍由应用负责。

## 3. Resume 与 Replay

| API | 语义 |
| --- | --- |
| `resume(threadId, options)` | 从该 thread + namespace 的 latest checkpoint 继续。 |
| `getState(threadId, options)` | 查询 latest checkpoint 的 state snapshot。 |
| `getStateHistory(threadId, options)` | 查询 checkpoint history。 |
| `replay(threadId, checkpointId, options)` | 从历史 checkpoint 继续执行；新 checkpoint 接在当前 latest step 之后。 |
| `updateState(threadId, update, options, updateOptions)` | 从 latest 或指定 checkpoint 应用外部 update，并创建新 checkpoint。 |

`replay()` 不删除历史记录；它创建新的继续分支。这样 time travel 不破坏已有历史，也能让 replay 后再次 interrupt 的分支通过 normal resume 继续。

## 4. Pending writes 恢复

parallel super-step 中，如果部分 task 成功、部分 task 失败：

1. runtime 不立即把成功 writes 合并成完成 state；
2. 成功 writes 作为 pending writes 写入 checkpoint；
3. failure checkpoint 记录需要重跑的 tasks；
4. resume 时只重跑失败/未完成 tasks；
5. super-step 完成后，pending writes 和新 writes 一起确定性 merge。

这是 graph runtime 支持可靠 fan-out/fan-in 的关键契约。

## 5. Namespace 规则

- root graph 可以使用空 namespace 或应用指定 namespace。
- subgraph 层级使用官方 LangGraph 分隔符 `|`。
- per-invocation task/run segment 使用 `:`。
- stream metadata 可同时暴露 `checkpoint_ns` 字符串和 namespace path。

namespace 是 checkpoint 查询维度。不同 namespace 的 checkpoint 不应互相污染。

## 6. Store 与 Checkpoint 的区别

| 维度 | Checkpoint | Store |
| --- | --- | --- |
| 目的 | 恢复图执行历史。 | 保存长期应用记忆。 |
| 作用域 | thread + checkpoint namespace。 | namespace + key。 |
| 写入者 | graph runtime / checkpointer。 | node/tool/application。 |
| 查询方式 | `get`、`getTuple`、`list`、history。 | `batch`、`put`、`get`、`search`、`deleteItem`、`listNamespaces`。 |
| 生命周期 | 随执行历史增长，可 prune/copy/delete。 | 应用拥有 retention 语义。 |

## 7. Serializer 与加密

`StorageSaverOptions::codec_` 是 checkpoint serializer/encryption 插拔点。当前可用形态包括 JSON checkpoint codec、content envelope codec 和 secure checkpoint codec。

当前尚未承诺 Python `JsonPlusSerializer` / `_DeltaSnapshot` 二进制格式同构。这个限制记录在 [LIMITATIONS.md](LIMITATIONS.md)。

## 8. 验证入口

| 行为 | 验证 |
| --- | --- |
| checkpoint put/get/list/delete | `checkpointer_test` |
| resume | `checkpoint_resume`、`sqlite_checkpoint_resume` |
| history/replay/updateState | `time_travel_history`、graph runtime tests |
| pending writes | graph runtime tests |
| crash recovery | `crash_recovery_test` |
| namespace/subgraph isolation | `subgraph_module`、compat tests |
| store long-term memory | `long_term_memory_store`、store tests |

