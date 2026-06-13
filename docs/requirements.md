# langgraph-cpp MVP 需求文档

## 1. 产品定位

`langgraph-cpp` 是面向边缘端、本地模型、云端 LLM API 和硬件工具的 C++ 有状态 Agent 图运行时。

它的核心形态不是聊天助手，也不是 LangChain 的 C++ 复刻版，而是一个 LangGraph-style runtime：用显式状态、图节点、条件路由、检查点和流式事件，把长任务编排成可恢复、可观测、可审计的执行过程。

一句话定位：

> C++ edge runtime for stateful agents.

模型策略：
- Local-first：优先支持 llama.cpp / GGUF 等本地模型。
- Cloud-capable：通过模型适配器接入 OpenAI-compatible、DeepSeek、Qwen、Anthropic 等云端 LLM API。
- Runtime-first：核心运行时不绑定任何具体模型 provider。

## 2. 目标用户与场景

目标用户：
- 机器人与 ROS2 开发者。
- 边缘网关、IoT、工业控制开发者。
- llama.cpp / GGUF 本地模型开发者。
- 需要 C++ 原生工作流编排和故障恢复的系统工程师。

核心场景：
- 机器人高层任务编排：感知、规划、行动、校验、重试。
- 边缘设备自治运维：日志诊断、工具修复、断电恢复、人工审批。
- 本地模型工具调用：离线模型基于 JSON Schema / grammar 调用本地工具。
- 嵌入式可靠工作流：OTA、设备自检、故障处理、回滚流程。

## 3. 设计原则

- C++ 原生：核心运行时不依赖 Python，MVP 最低要求 C++23。
- 边缘优先：优先适配 x86_64 Linux 和 ARM64 Linux。
- 状态优先：所有执行围绕显式 state、thread、checkpoint 展开。
- 小内核：图运行时、状态合并、检查点和事件流保持轻量。
- 可扩展：模型、工具、存储、硬件能力通过接口接入。
- 可恢复：每个关键执行边界都应具备恢复和审计能力。
- 可测试：核心模块不依赖真实模型或真实硬件即可测试。

## 4. MVP 模块

### 4.1 Graph Runtime Core

职责：提供 LangGraph 风格的低层图编排内核。

功能范围：
- `StateGraph`：声明节点、普通边、条件边、`START`、`END`。
- `CompiledGraph`：将声明式图编译为可执行计划。
- `Node`：接收 state，返回 state update 或 runtime command。
- `Router`：基于 state 返回下一个节点名。
- 有环执行：支持 `model -> tool -> model` 等 Agent 循环。
- Super-step：以 step/tick 作为状态保存、事件输出和恢复边界。
- 运行保护：支持最大步数、最大运行时间、取消信号。

验收标准：
- 可运行线性图、条件图和有环图。
- 有环图可通过路由到 `END` 或达到最大步数退出。
- 节点只能通过 state update 改变图状态。

### 4.2 State & Reducers

职责：定义状态容器和状态合并语义。

功能范围：
- `State`：MVP 使用 JSON object 作为动态状态。
- `StateUpdate`：节点返回局部更新，而不是直接暴露全局可变状态。
- Reducer：字段级合并策略，至少支持 overwrite、append、merge object。
- Schema 预留：为后续 typed state、JSON Schema 校验和反射生成预留接口。
- 冲突处理：并行节点写入同一字段时必须有确定性策略。

验收标准：
- 可对不同字段配置不同 reducer。
- reducer 行为可单元测试。
- 未配置 reducer 的字段默认 overwrite。

### 4.3 Checkpoint & Memory

职责：提供 thread 级短期记忆、故障恢复和历史状态查询。

功能范围：
- `Checkpoint`：保存 state、next nodes、step、thread_id、checkpoint_id、parent_id、writes。
- `Checkpointer` 接口：`put`、`get_latest`、`get`、`list`、`delete_thread`。
- `MemoryCheckpointer`：用于测试和无持久化场景。
- `SQLiteCheckpointer`：MVP 默认本地持久化实现。
- `Store` 接口：跨 thread 的长期记忆接口，MVP 可先提供 memory 实现。
- `resume(thread_id)`：从最新 checkpoint 恢复执行。
- 历史查询：支持按 thread_id 获取 checkpoint 列表。

验收标准：
- 进程重启后可从 SQLite 恢复最新 checkpoint。
- 可查询同一 thread 的历史 state。
- checkpoint 写入失败时，图执行返回明确错误。

### 4.4 Runtime Events, Streaming & Interrupts

职责：暴露运行过程，并支持人类介入。

功能范围：
- `RuntimeEvent`：统一事件结构，包含 type、thread_id、step、node、payload、timestamp。
- `stream()`：输出 updates、values、tasks、checkpoints、messages、custom、debug。
- `StreamWriter`：节点和工具可写入自定义事件。
- `interrupt()`：节点可暂停执行并要求外部输入。
- `Command`：恢复中断、更新 state、跳转控制流的统一指令。
- 恢复中断：使用同一 thread_id 和 command 继续执行。

验收标准：
- 调用方可逐步收到节点开始、节点结束、state update 事件。
- interrupt 前必须完成 checkpoint。
- 恢复后节点能获取外部输入并继续执行。

### 4.5 Model & Message Adapters

职责：提供本地模型和消息格式的最小抽象。

功能范围：
- `Message`：支持 system、user、assistant、tool 角色。
- `ToolCall`：结构化描述工具名、参数、调用 ID。
- `BaseChatModel`：统一 `invoke` 和 `stream` 接口；`batch` 放入后续版本。
- `MockChatModel`：用于测试 agent loop。
- `LlamaCppChatModel`：可选组件，支持本地 GGUF 模型。
- Context 管理：支持最大 token / message 数限制，超限时裁剪或报错。

验收标准：
- mock model 可驱动 `model -> tool -> model -> END` 示例。
- 模型响应可转换为 Message 并写入 state。
- 不启用 llama.cpp 时，核心库仍可编译和测试。

### 4.6 Tools & Structured Output

职责：提供可审计、可校验、可受限的工具调用机制。

功能范围：
- `Tool`：包含 name、description、input_schema、output_schema、callable。
- `ToolRegistry`：注册、查询、启用、禁用工具。
- `ToolNode`：执行 ToolCall，将结果写回 messages 或 state。
- 参数校验：工具执行前必须校验输入 schema。
- 结构化输出：MVP 支持 JSON Schema 校验；本地模型可选接入 llama.cpp grammar / GBNF。
- 工具错误：超时、参数错误、执行失败统一返回结构化错误。
- 权限预留：为文件、shell、网络、硬件工具预留 permission policy。

验收标准：
- 不合法参数不会触发工具 callable。
- 工具执行结果可回流模型节点。
- 工具错误可被模型节点读取并参与下一步路由。

### 4.7 Edge Runtime Adapters

职责：提供边缘端部署、硬件工具和平台适配能力。

功能范围：
- CMake 构建：核心库、测试、示例、可选组件开关。
- 平台：MVP 支持 x86_64 Linux、ARM64 Linux；macOS 作为开发环境。
- 硬件工具接口：为 GPIO、UART、I2C、CAN、ROS2 action/service 预留 adapter。
- 资源控制：最大步数、工具超时、模型超时、内存预算、日志级别。
- 安全默认值：危险工具默认禁用，需要显式注册和授权。

验收标准：
- 默认构建不依赖 llama.cpp 或真实硬件库。
- SQLite 持久化通过构建选项启用，并作为 MVP 恢复能力的默认验证路径。
- 禁用模型适配后，图运行时和工具系统仍可独立工作。
- 示例可在普通 Linux 环境运行，无需真实硬件。

## 5. 非功能需求

- 语言标准：MVP 最低要求 C++23；公共 API 保持面向 C++26 的演进空间。
- 构建系统：CMake；可选依赖通过 option 控制。
- 内存目标：不含模型权重时，核心运行时常驻内存目标小于 10 MB。
- 延迟目标：单次节点调度、路由和 state merge 目标小于 1 ms。
- 错误处理：公开 API 返回明确错误类型，不使用静默失败。
- 日志：核心提供可替换 logger，不绑定具体日志库。
- 测试：核心模块必须可用单元测试覆盖，无需真实模型或硬件。

## 6. MVP 非目标

- 不做通用聊天助手产品。
- 不复刻完整 LangChain provider、RAG、middleware 生态。
- 不实现 LangSmith、Studio、托管部署平台。
- 不内置复杂多 Agent 管理平台。
- 不承诺 RTOS 支持，第一阶段聚焦嵌入式 Linux。
- 不默认开放 shell、文件写入、网络访问等危险工具。

## 7. MVP 成功标准

MVP 完成时，应能运行以下端到端示例：

1. 最小图：`START -> node -> END`，输出 state update。
2. 条件图：根据 state 路由到不同节点。
3. 循环图：`model -> tool -> model -> END`。
4. 持久化恢复：运行中断后，使用 thread_id 从 SQLite 恢复。
5. 流式事件：调用方可看到节点事件、state update 和 checkpoint 事件。
6. 人类介入：工具调用前 interrupt，外部 approve 后继续执行。
7. 边缘模拟：本地 mock hardware tool 失败后重试，最终进入人工处理分支。

## 8. 推荐版本节奏

- `v0.1`：图运行时、state、reducer、memory checkpoint。
- `v0.2`：SQLite checkpoint、resume、历史查询。
- `v0.3`：messages、mock model、tools、agent loop 示例。
- `v0.4`：stream events、interrupt、Command。
- `v0.5`：llama.cpp 可选集成、JSON Schema / grammar 约束。
- `v0.6`：边缘工具 adapter 示例、ROS2 / UART / GPIO 方向验证。

## 9. 参考依据

- LangGraph：低层有状态编排运行时，核心能力包括 persistence、streaming、human-in-the-loop、memory、fault tolerance。
- LangGraph Checkpointers：以 thread、checkpoint、super-step 组织状态快照与恢复。
- LangChain：上层 Agent harness 由 model、tools、prompt、middleware 组合；`langgraph-cpp` 只吸收其中与本地 Agent loop 必需的最小抽象。
