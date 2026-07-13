# ADR-0007: 明确 Store 与 Checkpoint 的职责分离

| 字段 | 内容 |
| --- | --- |
| 状态 | Accepted |
| 日期 | 2026-07-10 |
| 关联 | [../PERSISTENCE_MODEL.md](../PERSISTENCE_MODEL.md)、[../API_CONTRACT.md](../API_CONTRACT.md)、[../LANGGRAPH_COMPATIBILITY.md](../LANGGRAPH_COMPATIBILITY.md) |

## 背景

LangGraph-style 应用同时需要两类持久化：一类是 graph execution 的 checkpoint/resume/history，另一类是跨 runs 的长期记忆或应用数据。如果两者共用同一语义，用户会把执行恢复和业务记忆混在一起，导致 retention、namespace、schema、权限和恢复策略都变得含混。

## 决策

Checkpoint 记录一次 logical thread 的执行历史：state snapshot、pending writes、tasks、metadata、namespace、checkpoint id 和 parent checkpoint。Store 保存长期 key-value memory：namespace、key、value、filter/search/listNamespaces。

runtime 可以同时接收 checkpointer 和 store，但二者 contract 独立。checkpoint 用于 resume/replay/time travel；store 用于应用查询和长期记忆。

## 被拒绝的方案

| 方案 | 拒绝原因 |
| --- | --- |
| 用 Store 保存 checkpoint | 缺少 pending writes、parent/history、latest pointer 和 saver 语义。 |
| 用 Checkpointer 保存长期记忆 | retention/compaction/replay 会误删或污染业务记忆。 |
| 把二者合成一个大接口 | API 表面含混，backend 实现和测试边界变差。 |

## 后果

- checkpoint backend 可以围绕 crash recovery 和 history 优化。
- store backend 可以围绕 namespace/search/filter 优化。
- 文档必须持续说明二者不是同一个概念。
- 示例分别覆盖 checkpoint resume 和 long-term memory。

## 验证

- `checkpointer_test`
- store 相关测试
- `checkpoint_resume`
- `sqlite_checkpoint_resume`
- `time_travel_history`
- `long_term_memory_store`
