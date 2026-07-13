# ADR-0005: 使用 StateUpdate 和 reducer 作为唯一状态写入边界

| 字段 | 内容 |
| --- | --- |
| 状态 | Accepted |
| 日期 | 2026-07-10 |
| 关联 | [../API_CONTRACT.md](../API_CONTRACT.md)、[../ARCHITECTURE.md](../ARCHITECTURE.md)、[../CONCURRENCY_MODEL.md](../CONCURRENCY_MODEL.md) |

## 背景

图节点可能并发执行，也可能通过 `Send` 创建 branch-local state。若节点直接修改共享 state，runtime 无法保证 fan-out merge 顺序、checkpoint replay、stream projection 和错误恢复的一致性。

## 决策

节点不持有可变 graph state。节点观察输入 state，并通过 `StateUpdate` 或 `Command` 返回写入意图。runtime 在 super-step 边界收集全部 writes，并通过字段 reducer 合并到下一份 state。

默认语义是字段覆盖；需要累加、append、去重或 domain-specific merge 的字段必须显式注册 reducer。

## 被拒绝的方案

| 方案 | 拒绝原因 |
| --- | --- |
| 让 node handler 接收可变 state 引用 | 并发写入不确定，难以 replay，也难以定位写入来源。 |
| 所有字段都内置复杂 merge 规则 | 对业务语义做过度假设，且会隐藏冲突。 |
| 只允许完整 state 返回 | 简单但低效，且不利于 pending writes 和 stream updates。 |

## 后果

- state mutation 入口单一，便于测试和审计。
- reducer 是并发 fan-in 的核心 contract。
- `StateUpdate` 是 patch，不是完整 state snapshot。
- checkpoint 保存 runtime 合并后的 state 和 pending writes，而不是每个节点私下修改的对象。
- 用户需要为并发写入同一字段选择明确 reducer。

## 验证

- `graph_runtime_test`
- `serialization_test`
- `parallel_fanout`
- `conditional_fanout`
- `send_map_reduce`
- `command_goto`
