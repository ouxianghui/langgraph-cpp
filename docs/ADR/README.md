# 架构决策记录索引

本目录记录 `langgraph-cpp` 中影响长期维护的架构决策。每篇 ADR 都应说明背景、决策、被拒绝的方案、后果和验证方式。

| ADR | 决策 |
| --- | --- |
| [0001-super-step-execution.md](0001-super-step-execution.md) | 使用 super-step 作为图执行核心。 |
| [0002-checkpoint-boundary.md](0002-checkpoint-boundary.md) | checkpoint 写入 runtime 边界而不是任意副作用边界。 |
| [0003-status-result-error-model.md](0003-status-result-error-model.md) | 使用 `Status` / `Result<T>` 表达可恢复错误。 |
| [0004-langgraph-compatibility-boundary.md](0004-langgraph-compatibility-boundary.md) | 明确 LangGraph-style 兼容边界。 |
| [0005-state-update-reducer-boundary.md](0005-state-update-reducer-boundary.md) | 使用 `StateUpdate` 和 reducer 作为唯一状态写入边界。 |
| [0006-stategraph-compile-boundary.md](0006-stategraph-compile-boundary.md) | 将 `StateGraph` builder 编译成不可变 `CompiledStateGraph` runtime。 |
| [0007-store-checkpoint-separation.md](0007-store-checkpoint-separation.md) | 明确 Store 与 Checkpoint 的职责分离。 |
| [0008-optional-integrations-behind-ports.md](0008-optional-integrations-behind-ports.md) | 将 provider、storage、network 和 hardware 集成放在可选端口后。 |
| [0009-stream-envelope-and-projection.md](0009-stream-envelope-and-projection.md) | 使用 LangGraph-style stream envelope 和 projection。 |
| [0010-explicit-tool-registration.md](0010-explicit-tool-registration.md) | 工具必须显式注册并通过 schema / policy 边界执行。 |

## 新增 ADR 规则

1. 使用递增编号：`0011-topic.md`。
2. `状态` 使用 `Proposed`、`Accepted`、`Superseded` 或 `Rejected`。
3. 写清被拒绝方案，避免只记录最终答案。
4. 写清验证方式，让设计决策能被测试、示例或文档门禁支撑。
