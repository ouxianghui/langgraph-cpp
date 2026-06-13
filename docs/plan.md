# langgraph-cpp MVP 执行计划

## 1. 计划目标

本计划基于 [requirements.md](requirements.md)，用于指导 `langgraph-cpp` 从空项目推进到可演示 MVP。

MVP 的目标是交付一个 C++23 有状态 Agent 图运行时，能够在本地或边缘端执行可恢复、可观测、可中断的任务流，并能通过模型与工具适配器跑通 `model -> tool -> model` 的最小 Agent loop。模型策略采用 local-first、cloud-capable：优先验证本地模型，同时为云端 LLM API 保留标准适配点。

## 2. 产品主线

主线优先级：

1. 先完成可靠图运行时。
2. 再完成 state、reducer、checkpoint、resume。
3. 再接入 message、model、tool loop。
4. 再补 streaming、interrupt、人类介入。
5. 最后接入本地模型和边缘工具适配。

核心判断：
- 图运行时和恢复能力是产品内核。
- 模型与工具是可插拔能力，不应绑死核心库。
- llama.cpp、SQLite、硬件工具都应通过 CMake option 控制。
- MVP 不追求 provider 大全集，优先跑通本地可控示例。

## 3. 目标架构

```text
Application / Examples
        |
StateGraph / CompiledGraph
        |
Runtime Context / Events / Interrupts
        |
State + Reducers
        |
Checkpoint Memory / Store
        |
Model Adapters     Tool Adapters     Edge Adapters
        |                |                 |
Mock / llama.cpp   JSON Schema        GPIO / UART / ROS2 mock
```

核心库边界：
- 必须包含：Graph Runtime、State、Reducer、Runtime Context、Memory Checkpointer、Events 基础类型。
- 可选包含：SQLiteCheckpointer、LlamaCppChatModel、硬件工具 adapter。
- 扩展预留：Cloud LLM API、RAG、Vector Store、ROS2 真实绑定、远程观测平台。

## 4. 阶段计划

### Phase 0：项目骨架

目标：建立可持续迭代的 C++ 项目基础。

范围：
- CMake 项目。
- C++23 编译配置。
- 目录结构。
- 测试框架。
- 顶层聚合头。

对应任务：
- T-001 初始化 CMake 项目。
- T-002 建立目录结构。
- T-003 引入测试框架。

验收：
- 默认配置能编译核心空库。
- `ctest` 可运行 smoke test。
- 示例能 include 顶层头文件。

退出标准：
- 项目可以被后续模块稳定扩展。

### Phase 1：图运行时内核

目标：完成最小 LangGraph-style 执行模型。

范围：
- `StateGraph`。
- `CompiledGraph`。
- 节点注册。
- 普通边。
- 条件边。
- `START` / `END`。
- 最大步数保护。

对应任务：
- T-101 定义核心类型。
- T-102 实现 StateGraph 声明 API。
- T-103 实现 CompiledGraph。
- T-104 实现 invoke 执行。
- T-105 增加基础示例。

验收：
- `minimal_graph` 可运行。
- `conditional_graph` 可运行。
- `loop_graph` 可运行并能退出。
- 非法图定义返回明确错误。

退出标准：
- 不依赖模型、不依赖 SQLite，即可表达和执行有环状态图。

### Phase 2：状态合并与检查点恢复

目标：让执行过程具备可靠恢复能力。

范围：
- JSON `State`。
- `StateUpdate`。
- Reducer。
- `Checkpoint`。
- `MemoryCheckpointer`。
- `SQLiteCheckpointer`。
- `resume(thread_id)`。
- 状态历史查询。

对应任务：
- T-201 实现 State 与 StateUpdate。
- T-202 实现 Reducer。
- T-203 定义 Checkpoint 数据结构。
- T-204 实现 Checkpointer 接口与 MemoryCheckpointer。
- T-205 将 checkpoint 接入 invoke。
- T-206 实现 resume(thread_id)。
- T-207 实现 SQLiteCheckpointer。

验收：
- 每个 super-step 后可查询 checkpoint。
- 中断执行后可从最新 checkpoint 继续。
- 进程重启后可通过 SQLite 恢复。
- checkpoint 写入失败会中止运行并返回错误。

退出标准：
- 项目具备与普通 C++ workflow engine 拉开差距的核心能力：可恢复 stateful runtime。

### Phase 3：Message、Model 与 Tool Loop

目标：跑通最小 Agent 循环。

范围：
- `Message`。
- `ToolCall`。
- `BaseChatModel`。
- `MockChatModel`。
- `ModelNode`。
- `Tool`。
- `ToolRegistry`。
- `ToolNode`。
- JSON Schema 参数校验。

对应任务：
- T-301 定义 Message 与 ToolCall。
- T-302 定义 BaseChatModel 与 MockChatModel。
- T-303 实现 ModelNode。
- T-304 定义 Tool 与 ToolRegistry。
- T-305 实现 ToolNode。
- T-502 实现 JSON Schema 参数校验。

验收：
- mock model 能发出 tool call。
- ToolNode 能校验参数、执行工具、返回 tool message。
- 可运行 `model -> tool -> model -> END` 示例。
- 参数非法时不会触发工具调用。

退出标准：
- 不依赖真实 LLM，即可完整验证 Agent loop 语义。

### Phase 4：Streaming 与 Interrupt

目标：让运行过程可观测、可暂停、可由人类恢复。

范围：
- `RuntimeEvent`。
- `stream()`。
- `StreamWriter`。
- `interrupt()`。
- `Command::resume(value)`。
- 人类审批示例。

对应任务：
- T-401 定义 RuntimeEvent。
- T-402 实现 stream()。
- T-403 实现 StreamWriter。
- T-404 实现 interrupt()。
- T-405 实现 Command 恢复。

验收：
- 调用方能逐步看到 task_start、task_end、state_update、checkpoint。
- 节点可以发出 custom event。
- 工具调用前可 interrupt。
- approve/reject 后可恢复执行。

退出标准：
- 图运行时具备真实产品所需的可观测和 human-in-the-loop 能力。

### Phase 5：本地模型与边缘适配

目标：验证项目的边缘端差异化。

范围：
- `LlamaCppChatModel` 可选集成。
- grammar / GBNF 输出约束。
- mock hardware adapter。
- mock edge repair workflow。
- 后续预留 ROS2 / UART / GPIO 真实绑定。

对应任务：
- T-501 集成 llama.cpp 可选组件。
- T-503 增加 grammar / GBNF 输出约束。
- T-504 定义硬件工具 Adapter 接口。
- T-505 增加边缘设备自治示例。

验收：
- 未开启 llama.cpp 时核心库仍可构建。
- 开启 llama.cpp 后可加载本地 GGUF 运行示例。
- mock hardware repair workflow 可展示失败重试、checkpoint、interrupt 和最终状态。

退出标准：
- 项目不只是图框架，而能展示本地模型和边缘工具结合的产品价值。

## 5. 里程碑版本

### v0.1：可执行图内核

包含：
- Phase 0。
- Phase 1。
- State / Reducer 的最小实现。

演示：
- `minimal_graph`。
- `conditional_graph`。
- `loop_graph`。

发布标准：
- 核心单测通过。
- README 可以指导用户运行第一个图。

### v0.2：可恢复运行时

包含：
- Phase 2。

演示：
- `checkpoint_resume`。
- SQLite 恢复。
- state history 查询。

发布标准：
- 支持 thread_id。
- 支持从 latest checkpoint 恢复。

### v0.3：最小 Agent Loop

包含：
- Phase 3。

演示：
- `tool_calling`。
- `model_tool_model_loop`。

发布标准：
- mock model + tool registry 可完整跑通。
- schema 校验能阻止非法工具参数。

### v0.4：可观测与人类介入

包含：
- Phase 4。

演示：
- `stream_events`。
- `human_interrupt`。

发布标准：
- stream 事件稳定。
- interrupt 必须先 checkpoint。
- command resume 可继续原 thread。

### v0.5：本地模型与边缘示例

包含：
- Phase 5 的 P1 能力。

演示：
- `llama_cpp_tool_calling`。
- `mock_edge_repair`。

发布标准：
- llama.cpp 作为可选组件。
- 边缘示例无需真实硬件也能跑通。

## 6. 关键设计决策

### D-001 C++ 标准

决策：MVP 最低要求 C++23，API 保持 C++26-ready。

原因：
- C++23 能兼顾现代 API 设计和边缘端工具链可用性。
- C++26 直接作为最低标准会提高交叉编译风险。

### D-002 核心状态格式

决策：MVP 使用 JSON object 作为 `State` 底层格式。

原因：
- 便于 checkpoint 序列化。
- 便于 tool schema、message、runtime event 统一表达。
- typed state 可在后续版本通过 wrapper 或 schema 增强。

### D-003 模型策略

决策：核心只定义 `BaseChatModel`；真实模型 provider 都是 adapter。

原因：
- 避免核心绑定 llama.cpp 或云 API。
- 便于后续同时支持本地模型和 cloud LLM API。
- 测试可通过 `MockChatModel` 完成。

### D-004 持久化策略

决策：提供 `MemoryCheckpointer` 和可选 `SQLiteCheckpointer`。

原因：
- Memory 实现适合测试和示例。
- SQLite 适合边缘端本地恢复，不需要外部服务。
- 后续可扩展 RocksDB、Postgres、remote checkpoint store。

### D-005 工具安全

决策：危险工具默认不内置、不启用，工具调用前必须支持 schema 校验。

原因：
- Agent 工具调用天然有安全风险。
- 边缘端可能直接接触硬件、文件系统和 shell。
- MVP 必须把安全边界放进架构，而不是后补。

## 7. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| C++23 工具链不一致 | 边缘平台编译失败 | CI 覆盖 GCC/Clang；避免过度依赖冷门特性 |
| 图执行语义变复杂 | API 难稳定 | v0.1 只支持单 active path，有并行需求后再扩展 |
| checkpoint 恢复不一致 | 任务重复执行或状态丢失 | 明确 super-step 边界；节点副作用通过工具层审计 |
| 工具调用不安全 | 文件/硬件误操作 | schema 校验、权限策略、危险工具默认禁用 |
| llama.cpp 集成拖慢 MVP | 核心交付延期 | llama.cpp 放在 v0.5，可选构建 |
| 过早做云 provider | 范围膨胀 | 先定义 `BaseChatModel`，后续用 OpenAI-compatible adapter 扩展 |

## 8. 首批开发顺序

建议第一轮只做：

1. T-001 初始化 CMake 项目。
2. T-002 建立目录结构。
3. T-003 引入测试框架。
4. T-201 实现 State 与 StateUpdate。
5. T-202 实现 Reducer。
6. T-101 定义核心类型。
7. T-102 实现 StateGraph 声明 API。
8. T-103 实现 CompiledGraph。
9. T-104 实现 invoke 执行。
10. T-105 增加基础示例。

这条路径能最快拿到一个可展示的图运行时，再进入 checkpoint。

## 9. MVP 完成定义

MVP 完成需要同时满足：

- C++23 默认构建通过。
- 核心库不依赖 Python。
- 线性图、条件图、有环图可运行。
- State reducer 可配置且有测试。
- Memory 和 SQLite checkpoint 可用。
- `resume(thread_id)` 可恢复未完成运行。
- `model -> tool -> model -> END` 示例可运行。
- stream 事件可消费。
- interrupt / command resume 可运行。
- 工具参数非法时不会执行工具。
- 至少 5 个示例可通过 CMake 构建。

## 10. MVP 后路线

MVP 后优先级：

1. OpenAI-compatible cloud model adapter。
2. 更强的 typed state / schema state。
3. ROS2 adapter。
4. UART / GPIO / CAN 真实硬件工具。
5. remote checkpoint store。
6. vector store / RAG 扩展。
7. 多 active path / 并行节点执行。
8. LangGraph Python 语义兼容性测试集。
