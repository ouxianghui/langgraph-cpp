# ADR-0001: 使用 super-step 作为图执行核心

| 字段 | 内容 |
| --- | --- |
| 状态 | Accepted |
| 日期 | 2026-07-10 |
| 关联 | [../ARCHITECTURE.md](../ARCHITECTURE.md)、[../CONCURRENCY_MODEL.md](../CONCURRENCY_MODEL.md) |

## 背景

`langgraph-cpp` 需要执行有状态图，支持普通 edge、conditional routing、fan-out/fan-in、`Send`、`Command`、interrupt、checkpoint 和 resume。若逐节点立即修改共享 state，parallel fan-out 会产生不确定 merge 顺序，也难以在失败时恢复部分成功 task。

## 决策

图执行采用 super-step 模型：

1. 每轮 super-step 计算 ready tasks。
2. ready nodes 观察同一个 state snapshot，或自己的 `Send` branch-local state。
3. node 并发或串行执行，返回 `NodeOutput`。
4. runtime 收集全部 outputs。
5. writes 按确定性顺序通过 reducer merge。
6. routing 基于 merged state 生成下一轮 tasks。
7. checkpoint 在 runtime 边界写入。

## 被拒绝的方案

| 方案 | 拒绝原因 |
| --- | --- |
| 每个 node 完成后立即修改共享 state | fan-out merge 顺序不稳定，失败恢复复杂。 |
| 全局串行执行所有 node | 实现简单，但放弃 parallel super-step 能力。 |
| 让用户 node 直接持有可变 graph state | 破坏 reducer contract，难以 checkpoint/replay。 |

## 后果

- fan-out/fan-in 行为可预测。
- reducer 是唯一 state merge 入口。
- pending writes 可以表达部分成功 task。
- node handler 必须把输入 state 当作不可变值。
- 外部副作用的幂等性仍由应用负责。

## 验证

- `graph_runtime_test`
- `parallel_fanout`
- `conditional_fanout`
- `send_map_reduce`
- `command_goto`
- `ctest --test-dir build/unix-debug -R "graph|compat" --output-on-failure`

