# src/langgraph/graph

本目录实现图声明、编译和执行。

## 1. 核心类型

| 类型 | 职责 |
| --- | --- |
| `StateGraph` | mutable builder，用于声明 node、edge、conditional edge、subgraph、command route 和 schema。 |
| `CompiledStateGraph` | immutable runtime，用于 invoke、stream、resume、replay、getState、updateState。 |
| `RunOptions` | 单次 run 的配置：thread id、checkpoint namespace、reducers、store、checkpointer、executor、limits、command。 |
| `StateSnapshot` | LangGraph-style checkpoint snapshot view。 |
| `StreamPart` | projected stream 输出单元。 |
| `RunnableConfig` | JSON config bridge，承接 tags、metadata、configurable 和 run options。 |

## 2. 执行模型

- `StateGraph::compile()` 校验 builder 并生成 immutable plan。
- `CompiledStateGraph::runFrom()` 驱动 super-step loop。
- 每个 super-step 的 ready nodes 观察同一个 state snapshot。
- node 可以并发执行；输出收集后按 reducer 串行合并。
- routing 在 merged state 后计算。
- checkpoint 在 runtime 边界写入。

## 3. 并发规则

- 不使用全局 owner thread。
- `RunOptions::executor_` 可注入外部 executor。
- `RunOptions::maxConcurrency_` 控制单个 super-step 的并发度。
- `RuntimeEvent` 发布路径有局部同步。
- node handler 不应跨异步边界持有 `Runtime&` 或 `State&`。

## 4. 错误规则

- graph definition 错误通过 `validate()` / `compile()` 返回 `Status`。
- node handler 异常会被捕获并转换为 failed node status。
- schema、checkpoint、routing、cancellation 错误必须传播到 run result。

## 5. 关联文档

- [../../../docs/CONCURRENCY_MODEL.md](../../../docs/CONCURRENCY_MODEL.md)
- [../../../docs/PERSISTENCE_MODEL.md](../../../docs/PERSISTENCE_MODEL.md)
- [../../../docs/ADR/0001-super-step-execution.md](../../../docs/ADR/0001-super-step-execution.md)
