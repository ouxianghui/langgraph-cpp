# langgraph-cpp MVP 任务拆分

## 1. 任务约定

任务状态：
- `P0`：MVP 必须完成。
- `P1`：MVP 强相关，允许在核心跑通后补齐。
- `P2`：后续版本增强。

开发原则：
- 每个模块先提供接口和 mock / memory 实现，再接真实依赖。
- 默认构建必须能在无 llama.cpp、无真实硬件环境下通过。
- 每个 P0 任务必须有单元测试或可运行示例。

## 2. 阶段总览

| 阶段 | 目标 | 核心产出 |
|---|---|---|
| M0 | 项目骨架 | CMake、目录结构、测试框架、基础类型 |
| M1 | 图运行时 | StateGraph、CompiledGraph、节点执行、条件路由 |
| M2 | 状态与恢复 | State、Reducer、Checkpoint、SQLite resume |
| M3 | Agent loop | Message、MockModel、Tool、ToolNode |
| M4 | 可观测与中断 | RuntimeEvent、stream、interrupt、Command |
| M5 | 本地模型与边缘适配 | llama.cpp 可选集成、硬件工具 adapter 示例 |

## 3. M0：项目骨架

### T-001 初始化 CMake 项目

优先级：P0  
依赖：无

任务：
- 创建 `CMakeLists.txt`。
- 设置 C++23。
- 添加 `LANGGRAPH_CPP_BUILD_TESTS`、`LANGGRAPH_CPP_BUILD_EXAMPLES`、`LANGGRAPH_CPP_WITH_SQLITE`、`LANGGRAPH_CPP_WITH_LLAMA_CPP` 选项。
- 生成核心库 target：`langgraph_cpp`。

验收：
- 默认配置可生成构建目录。
- 未开启可选依赖时，核心库可编译。

### T-002 建立目录结构

优先级：P0  
依赖：T-001

任务：
- 创建 `include/langgraph_cpp/`。
- 创建 `src/`、`tests/`、`examples/`。
- 添加顶层聚合头 `include/langgraph_cpp/langgraph.hpp`。

验收：
- 示例代码可通过 `#include <langgraph_cpp/langgraph.hpp>` 引入核心 API。

### T-003 引入测试框架

优先级：P0  
依赖：T-001

任务：
- 选择轻量测试框架，例如 Catch2 或 doctest。
- 添加最小 smoke test。

验收：
- `ctest` 可运行并通过。

## 4. M1：Graph Runtime Core

### T-101 定义核心类型

优先级：P0  
依赖：T-002

任务：
- 定义 `NodeId`、`ThreadId`、`CheckpointId`、`StepId`。
- 定义 `START`、`END` 常量。
- 定义基础错误类型或 `Result<T>` 机制。

验收：
- 类型不直接暴露底层存储细节。
- 错误可携带 code 和 message。

### T-102 实现 StateGraph 声明 API

优先级：P0  
依赖：T-101

任务：
- 实现 `add_node`。
- 实现 `add_edge`。
- 实现 `add_conditional_edges`。
- 实现重复节点、未知节点、非法边检查。

验收：
- 可声明线性图和条件图。
- 非法图定义返回明确错误。

### T-103 实现 CompiledGraph

优先级：P0  
依赖：T-102

任务：
- 实现 `compile()`。
- 生成内部节点表、边表、路由表。
- 校验 `START` 可达、`END` 可达或显式允许长循环。

验收：
- 编译后的图不可再被修改。
- 编译错误包含可定位信息。

### T-104 实现 invoke 执行

优先级：P0  
依赖：T-103、T-201

任务：
- 实现 `invoke(input_state, config)`。
- 执行 START 到 END 的节点流程。
- 支持普通边和条件边。
- 支持最大步数保护。

验收：
- 最小图、条件图、有环图均可执行。
- 超过最大步数时返回明确错误。

### T-105 增加基础示例

优先级：P0  
依赖：T-104

任务：
- `examples/minimal_graph.cpp`。
- `examples/conditional_graph.cpp`。
- `examples/loop_graph.cpp`。

验收：
- 示例可构建并输出最终 state。

## 5. M2：State, Reducer & Checkpoint

### T-201 实现 State 与 StateUpdate

优先级：P0  
依赖：T-001

任务：
- 选择 JSON 库并封装 `State`。
- 定义 `StateUpdate`。
- 提供 get/set/contains/merge 基础操作。

验收：
- 节点返回局部 update。
- 测试覆盖基础读写和缺失字段行为。

### T-202 实现 Reducer

优先级：P0  
依赖：T-201

任务：
- 定义 reducer 接口。
- 实现 overwrite、append、merge object。
- 支持字段级 reducer 注册。

验收：
- reducer 行为确定且可测试。
- 未配置字段默认 overwrite。

### T-203 定义 Checkpoint 数据结构

优先级：P0  
依赖：T-201

任务：
- 定义 `Checkpoint`。
- 包含 state、next nodes、writes、step、thread_id、checkpoint_id、parent_id、created_at。

验收：
- Checkpoint 可序列化为 JSON。
- 可从 JSON 反序列化。

### T-204 实现 Checkpointer 接口与 MemoryCheckpointer

优先级：P0  
依赖：T-203

任务：
- 定义 `Checkpointer` 抽象接口。
- 实现 `MemoryCheckpointer`。
- 支持 put/get/get_latest/list/delete_thread。

验收：
- 可保存并读取同一 thread 的多条 checkpoint。
- list 按 step 或创建时间稳定排序。

### T-205 将 checkpoint 接入 invoke

优先级：P0  
依赖：T-104、T-204

任务：
- 执行开始前写入初始 checkpoint。
- 每个 super-step 后写入 checkpoint。
- checkpoint 写失败时中止执行并返回错误。

验收：
- invoke 后可查询完整执行历史。
- checkpoint 数量与执行 step 对齐。

### T-206 实现 resume(thread_id)

优先级：P0  
依赖：T-205

任务：
- 从 latest checkpoint 读取 state 和 next nodes。
- 继续执行未完成图。
- 已完成 thread 再 resume 时返回最终 state。

验收：
- 中断在中途的图可以继续执行。
- 已完成图 resume 不重复执行节点。

### T-207 实现 SQLiteCheckpointer

优先级：P0  
依赖：T-204

任务：
- 设计 SQLite 表结构。
- 实现 checkpoint JSON 存取。
- 支持 thread 历史查询。
- 加入 CMake 可选开关。

验收：
- 进程重启后可恢复 checkpoint。
- SQLite 未启用时核心库仍可编译。

## 6. M3：Model, Messages & Tools

### T-301 定义 Message 与 ToolCall

优先级：P0  
依赖：T-201

任务：
- 定义 message role：system、user、assistant、tool。
- 定义 `ToolCall`：id、name、arguments。
- 支持 message 与 JSON 相互转换。

验收：
- messages 可存入 state。
- ToolCall 可被 ToolNode 读取。

### T-302 定义 BaseChatModel 与 MockChatModel

优先级：P0  
依赖：T-301

任务：
- 定义 `BaseChatModel::invoke`。
- 定义 `BaseChatModel::stream` 的最小接口或预留。
- 实现可脚本化响应的 `MockChatModel`。

验收：
- 测试可控制 mock model 返回普通消息或 tool call。

### T-303 实现 ModelNode

优先级：P0  
依赖：T-302、T-104

任务：
- 封装模型调用为 graph node。
- 从 state 读取 messages。
- 将模型响应 append 到 messages。

验收：
- ModelNode 可参与图执行。
- 模型错误作为结构化错误写入 state 或返回执行错误。

### T-304 定义 Tool 与 ToolRegistry

优先级：P0  
依赖：T-301

任务：
- 定义 Tool metadata。
- 定义 input_schema、output_schema。
- 实现注册、查找、启用、禁用。

验收：
- 重复注册返回错误。
- 禁用工具不可被调用。

### T-305 实现 ToolNode

优先级：P0  
依赖：T-304、T-303

任务：
- 从最新 assistant message 读取 tool calls。
- 校验工具参数。
- 执行工具 callable。
- 将 tool result 写回 messages。

验收：
- 可跑通 `model -> tool -> model -> END`。
- 参数非法时不执行 callable。

### T-306 结构化工具错误

优先级：P1  
依赖：T-305

任务：
- 统一工具错误格式。
- 区分 validation_error、not_found、disabled、timeout、runtime_error。

验收：
- 模型节点可读取工具错误并继续决策。

## 7. M4：Streaming & Interrupts

### T-401 定义 RuntimeEvent

优先级：P0  
依赖：T-104

任务：
- 定义 event type：task_start、task_end、state_update、checkpoint、message、custom、debug、interrupt。
- 定义 event payload。

验收：
- invoke 内部可生成事件对象。

### T-402 实现 stream()

优先级：P0  
依赖：T-401、T-205

任务：
- 提供同步 iterator 或 callback 形式的 stream API。
- 支持筛选事件类型。
- 输出节点开始、结束、state update、checkpoint 事件。

验收：
- 调用方可逐步消费事件。
- stream 最终返回 final state 或错误。

### T-403 实现 StreamWriter

优先级：P1  
依赖：T-402

任务：
- 允许节点写入 custom event。
- 将 custom event 注入当前 stream。

验收：
- 示例节点可输出进度事件。

### T-404 实现 interrupt()

优先级：P0  
依赖：T-206、T-402

任务：
- 节点可返回 interrupt command。
- runtime 保存 checkpoint 并暂停。
- stream 输出 interrupt event。

验收：
- interrupt 前状态已持久化。
- invoke 返回 paused 状态而非错误。

### T-405 实现 Command 恢复

优先级：P0  
依赖：T-404

任务：
- 定义 `Command::resume(value)`。
- resume 时将外部输入送回暂停点。
- 支持 approve/reject 类工具审批。

验收：
- 人工审批示例可暂停、批准、继续。

## 8. M5：Local Model & Edge Adapters

### T-501 集成 llama.cpp 可选组件

优先级：P1  
依赖：T-302

任务：
- 添加 `LANGGRAPH_CPP_WITH_LLAMA_CPP`。
- 实现 `LlamaCppChatModel`。
- 支持 GGUF 路径、上下文长度、temperature、max_tokens。

验收：
- 开启选项后可构建本地模型示例。
- 未开启时不影响核心构建。

### T-502 实现 JSON Schema 参数校验

优先级：P0  
依赖：T-304

任务：
- 选择轻量 schema 校验方案或先实现 MVP 子集。
- 在 ToolNode 执行前校验 arguments。

验收：
- 缺失必填字段、类型错误会阻止工具调用。

### T-503 增加 grammar / GBNF 输出约束

优先级：P1  
依赖：T-501、T-502

任务：
- 将 tool schema 转换为 llama.cpp grammar。
- 支持工具调用 JSON 输出约束。

验收：
- 本地模型示例能稳定输出可解析 tool call。

### T-504 定义硬件工具 Adapter 接口

优先级：P1  
依赖：T-304

任务：
- 定义 GPIO、UART、I2C、CAN、ROS2 adapter 的接口草案。
- 不直接依赖真实硬件库。

验收：
- mock hardware tool 可通过 ToolRegistry 注册并执行。

### T-505 增加边缘设备自治示例

优先级：P1  
依赖：T-305、T-405、T-504

任务：
- 构建 mock hardware repair workflow。
- 流程包含诊断、工具调用、失败重试、人工介入。

验收：
- 示例可无真实硬件运行。
- 输出完整事件流和最终状态。

## 9. 文档与发布任务

### T-601 编写 README

优先级：P0  
依赖：T-105

任务：
- 明确产品定位。
- 提供最小示例。
- 说明非目标和安全边界。

验收：
- 新用户能在 5 分钟内运行 minimal_graph。

### T-602 编写 API 草案文档

优先级：P1  
依赖：T-104、T-205、T-305

任务：
- 文档化 StateGraph、CompiledGraph、Checkpointer、Tool、Message。
- 标注 unstable API。

验收：
- 每个核心类至少有一个代码片段。

### T-603 建立示例矩阵

优先级：P1  
依赖：T-105、T-305、T-405、T-505

任务：
- minimal_graph。
- conditional_graph。
- loop_graph。
- checkpoint_resume。
- tool_calling。
- human_interrupt。
- mock_edge_repair。

验收：
- 示例可通过 CMake 统一构建。

## 10. MVP 完成定义

MVP 完成需要满足：
- M0 P0 项目基础任务全部完成，即 T-001 至 T-003。
- M1 P0 任务全部完成。
- M2 P0 任务全部完成。
- M3 P0 任务全部完成。
- M4 中 T-401、T-402、T-404、T-405 完成。
- M5 中 T-502 完成；llama.cpp 集成可作为 P1 延后。
- README 和至少 5 个示例可运行。

建议第一批 GitHub issues 只覆盖 T-001 到 T-207，先把可恢复图运行时做扎实，再进入模型和工具层。
