# ADR-0006: 将 StateGraph builder 编译成不可变 CompiledStateGraph runtime

| 字段 | 内容 |
| --- | --- |
| 状态 | Accepted |
| 日期 | 2026-07-10 |
| 关联 | [../ARCHITECTURE.md](../ARCHITECTURE.md)、[../API_REFERENCE.md](../API_REFERENCE.md)、[../CONCURRENCY_MODEL.md](../CONCURRENCY_MODEL.md) |

## 背景

`StateGraph` 负责声明 node、edge、router、subgraph、entry point 和 reducers。运行时则需要高频执行 invoke、stream、resume、replay、getStateHistory 和 updateState。如果声明期和运行期混在同一个可变对象里，运行时并发调用、验证错误、缓存执行计划和 API 可读性都会变差。

## 决策

`StateGraph` 是 builder；`compile()` 完成结构验证并生成 `CompiledStateGraph`。编译后对象作为不可变 execution plan，可被多个 invocation 共享。每次运行的 state、tasks、stream writer、checkpoint cursor 和 interrupt/resume payload 保持 run-local。

## 被拒绝的方案

| 方案 | 拒绝原因 |
| --- | --- |
| `StateGraph` 同时负责声明和运行 | 声明期 mutation 与运行期并发共享混在一起，生命周期难以解释。 |
| 每次 invoke 都重新 validate graph | 简化对象模型，但浪费运行期成本，也延迟结构错误暴露。 |
| 编译后仍允许修改 graph topology | 会破坏已缓存的 routes、checkpoint namespace 和运行时假设。 |

## 后果

- graph topology 错误在 compile 阶段暴露。
- compiled plan 可以安全复用。
- runtime 实现可以围绕 immutable plan 和 run-local state 组织。
- 新增 builder API 需要考虑 compile validation。
- 新增 runtime API 不应反向修改 builder topology。

## 验证

- `graph_runtime_test`
- docs snippet compile tests
- `minimal_graph`
- `subgraph_module`
- `stream_projection`
