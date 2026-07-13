# 示例矩阵

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-13 |
| 默认 preset | `unix-debug` |
| 可选示例 | `llama_cpp_chat`、`llama_cpp_tool_calling` |

当 `LANGGRAPH_CPP_BUILD_EXAMPLES=ON` 时，示例由 `examples/CMakeLists.txt` 统一构建。

本文档跟踪示例覆盖面和最近一次本地验证信号。默认示例与需要可选依赖或运行时资产的示例分开记录。

构建全部默认目标：

```sh
cmake --preset unix-debug
cmake --build --preset unix-debug
```

运行单个示例：

```sh
build/unix-debug/examples/minimal_graph
```

## 1. 矩阵

| 示例 | 源文件 | 默认门禁 | 主要 API | 展示内容 |
| --- | --- | --- | --- | --- |
| `minimal_graph` | `examples/minimal_graph.cpp` | 是 | `StateGraph`、`CompiledStateGraph`、`StateUpdate` | 最小 `START -> node -> END` workflow。 |
| `parallel_fanout` | `examples/parallel_fanout.cpp` | 是 | multi-edge fan-out、reducers | parallel super-step、确定性 merge 和 fan-in。 |
| `conditional_fanout` | `examples/conditional_fanout.cpp` | 是 | multi-target `addConditionalEdges`、reducers | router 选择的 parallel fan-out 和确定性 fan-in。 |
| `send_map_reduce` | `examples/send_map_reduce.cpp` | 是 | `Send`、reducers | 带 branch-local state 的动态 fan-out 和 map-reduce style fan-in。 |
| `command_goto` | `examples/command_goto.cpp` | 是 | `Command::gotoNode`、`addCommandRoute` | 节点一次返回 state update 和动态路由。 |
| `conditional_graph` | `examples/conditional_graph.cpp` | 是 | `addConditionalEdges`、`RouterHandler` | 基于 state 字段路由。 |
| `loop_graph` | `examples/loop_graph.cpp` | 是 | cyclic edges、max-step guard | 重复进入节点直到 state 达到停止条件。 |
| `checkpoint_resume` | `examples/checkpoint_resume.cpp` | 是 | `StorageSaver`、`MemoryStorage`、`resume` | max steps 停止后，从 latest checkpoint 恢复。 |
| `sqlite_checkpoint_resume` | `examples/sqlite_checkpoint_resume.cpp` | SQLite 启用时 | `SQLiteStorage`、`StorageSaver`、`resume` | 重新打开 SQLite storage 模拟进程重启并恢复。 |
| `time_travel_history` | `examples/time_travel_history.cpp` | 是 | `getStateHistory`、`replay`、`updateState` | 查询 checkpoint history，从早期 checkpoint replay，并创建 forked state update。 |
| `long_term_memory_store` | `examples/long_term_memory_store.cpp` | 是 | `InMemoryStore`、`Runtime::store`、`RunOptions::store_` | 在独立 graph runs 之间保存长期用户/操作员记忆。 |
| `stream_projection` | `examples/stream_projection.cpp` | 是 | `streamProjected`、`RunProjectionOptions`、`StreamMode` | 消费 events、updates、values、messages、tasks、checkpoints、custom events 和 output projection。 |
| `subgraph_module` | `examples/subgraph_module.cpp` | 是 | `StateGraph::addSubgraph`、`SubgraphOptions` | 父 workflow 组合一个带独立 checkpoint namespace 的 diagnostic subgraph。 |
| `model_tool_model_loop` | `examples/model_tool_model_loop.cpp` | 是 | `FakeChatModel`、`ToolRegistry`、`ToolNode` | model 发出 tool call，tool result 追加后 model 完成。 |
| `agent_pattern_react` | `examples/agent_patterns/react.cpp` | 是 | `FakeChatModel`、`ToolRegistry`、`ToolNode` | ReAct 风格的 reason/action/observation/model loop。 |
| `agent_pattern_plan_and_solve` | `examples/agent_patterns/plan_and_solve.cpp` | 是 | `StateGraph`、conditional loop、state updates | 先生成完整计划，再逐步执行并汇总答案。 |
| `agent_pattern_reflection` | `examples/agent_patterns/reflection.cpp` | 是 | `StateGraph`、draft/critic/reviser nodes | draft、critique、revise 的自我修正 workflow。 |
| `llama_cpp_chat` | `examples/llama_cpp_chat.cpp` | 可选 | `LlamaCppChatModel`、`makeModelNode` | 通过 llama.cpp 调用本地 GGUF 模型。 |
| `llama_cpp_tool_calling` | `examples/llama_cpp_tool_calling.cpp` | 可选 | `LlamaCppChatModel`、GBNF、`ToolRegistry` | 本地 GGUF 模型输出受约束 JSON tool calls。 |
| `human_interrupt` | `examples/human_interrupt.cpp` | 是 | `NodeOutput::interrupt`、`Command::resume` | action 前暂停，并用外部审批恢复。 |
| `tool_approval_loop` | `examples/tool_approval_loop.cpp` | 是 | `Interrupt`、`ToolResult`、reducers | tool execution 前的人类审批 gate。 |
| `edge_mock_tool_adapter` | `examples/edge_mock_tool_adapter.cpp` | 是 | `ToolRegistry`、JSON Schema、adapter sketch | 把 mock edge hardware operation 注册为结构化工具。 |
| `mock_edge_repair` | `examples/mock_edge_repair.cpp` | 是 | tools、checkpoints、interrupt、resume | diagnose、auto-repair、fail、operator reset 暂停、resume 和 verify repaired state。 |

`sqlite_checkpoint_resume` 仅在 `LANGGRAPH_CPP_WITH_SQLITE=ON` 时注册。

`llama_cpp_chat` 和 `llama_cpp_tool_calling` 仅在 `LANGGRAPH_CPP_WITH_LLAMA_CPP=ON` 时注册，并需要外部 llama.cpp 源码树、安装前缀或 CMake target。

## 2. 验证状态

已在 2026-07-13 使用 `unix-debug` preset 验证。

构建验证：

```sh
cmake --build --preset unix-debug --target minimal_graph parallel_fanout conditional_fanout send_map_reduce command_goto conditional_graph loop_graph checkpoint_resume sqlite_checkpoint_resume time_travel_history long_term_memory_store stream_projection subgraph_module model_tool_model_loop agent_pattern_react agent_pattern_plan_and_solve agent_pattern_reflection human_interrupt tool_approval_loop edge_mock_tool_adapter mock_edge_repair
```

运行验证：

```sh
scripts/run-examples.sh
```

| 示例 | 状态 | 预期信号 |
| --- | --- | --- |
| `minimal_graph` | 通过 | `message` 是 `hello from langgraph-cpp`。 |
| `parallel_fanout` | 通过 | `checks` 包含两个分支，`decision` 是 `continue`。 |
| `conditional_fanout` | 通过 | `checks` 包含 router 选择的两个分支，`healthy` 为 true。 |
| `send_map_reduce` | 通过 | `drafts` 每个 Send 分支一项，`draft_count` 是 `3`。 |
| `command_goto` | 通过 | final state 包含 `decision: repair` 和 `repaired: true`。 |
| `conditional_graph` | 通过 | `route` 是 `fast`。 |
| `loop_graph` | 通过 | `count` 是 `3`。 |
| `checkpoint_resume` | 通过 | resumed `count` 是 `3`。 |
| `sqlite_checkpoint_resume` | 通过 | final state 包含 `count: 4` 和 `checkpoint_count: 6`。 |
| `time_travel_history` | 通过 | output 包含 `history_steps`、`replayed_count: 3` 和 `forked_count: 10`。 |
| `long_term_memory_store` | 通过 | 第二次 run 返回 `memory_hit: true` 和 `recommended_mode: quiet`。 |
| `stream_projection` | 通过 | output 汇总 projected `events`、`updates`、`values`、`messages`、`tasks`、`checkpoints`、`custom` 和 `output` parts。 |
| `subgraph_module` | 通过 | final state 包含 `diagnosis`、`repair_plan` 和 queued `ticket`。 |
| `model_tool_model_loop` | 通过 | tool result 包含 `value: 5`。 |
| `agent_pattern_react` | 通过 | final state 包含 `pattern: react`、sensor tool result 和 final assistant answer。 |
| `agent_pattern_plan_and_solve` | 通过 | final state 包含 `pattern: plan_and_solve`、`completed_count: 3` 和 `solved: true`。 |
| `agent_pattern_reflection` | 通过 | final state 包含 `pattern: reflection`、`critique`、`final_answer` 和 `revision_count: 1`。 |
| `llama_cpp_chat` | 可选，默认门禁未运行 | 仅在提供 llama.cpp 和 GGUF 模型后构建。 |
| `llama_cpp_tool_calling` | 可选，默认门禁未运行 | 仅在提供 llama.cpp 和 GGUF 模型后构建；使用 GBNF 约束输出。 |
| `human_interrupt` | 通过 | final state 包含 `approved: true`。 |
| `tool_approval_loop` | 通过 | final state 包含 `tool_approved: true` 和 tool result `21`。 |
| `edge_mock_tool_adapter` | 通过 | 两个 mock edge tools 均返回 structured tool results。 |
| `mock_edge_repair` | 通过 | output 包含完整 `events`、`first_run_status: paused`，以及 final `repair_status: repaired`。 |

## 3. 建议阅读顺序

1. `minimal_graph`
2. `parallel_fanout`
3. `conditional_fanout`
4. `send_map_reduce`
5. `command_goto`
6. `conditional_graph`
7. `loop_graph`
8. `checkpoint_resume`
9. `time_travel_history`
10. `long_term_memory_store`
11. `stream_projection`
12. `subgraph_module`
13. `model_tool_model_loop`
14. `agent_pattern_react`
15. `agent_pattern_plan_and_solve`
16. `agent_pattern_reflection`
17. `llama_cpp_chat`，当 `LANGGRAPH_CPP_WITH_LLAMA_CPP=ON`
18. `llama_cpp_tool_calling`，当 `LANGGRAPH_CPP_WITH_LLAMA_CPP=ON`
19. `human_interrupt`
20. `tool_approval_loop`
21. `mock_edge_repair`

这个顺序对应 runtime 层次：graph、routing、loop control、checkpointing、time travel、long-term memory、streaming、subgraph composition、messages/models/tools、agent patterns、interrupt/resume，最后是 edge-style workflow orchestration。

## 4. 预期输出

JSON 字段顺序可能不同。

### `minimal_graph`

```json
{"message":"hello from langgraph-cpp"}
```

### `parallel_fanout`

```json
{"checks":["temperature","power"],"decision":"continue","facts":{"temperature_c":42.5,"voltage_v":12.1},"healthy":true}
```

### `conditional_fanout`

```json
{"checks":["temperature","power"],"decision":"inspect","facts":{"temperature_c":42.5,"voltage_v":12.1},"healthy":true}
```

### `send_map_reduce`

```json
{"draft_count":3,"drafts":["edge-draft","robot-draft","sensor-draft"],"joined":true,"subjects":["edge","robot","sensor"]}
```

### `command_goto`

```json
{"decision":"repair","repaired":true}
```

### `conditional_graph`

```json
{"priority":"high","route":"fast"}
```

### `checkpoint_resume`

```json
{"count":3}
```

### `time_travel_history`

```json
{"final_count":3,"forked_count":10,"history_steps":[3,2,1,0],"replayed_count":3}
```

### `long_term_memory_store`

第二次 run 复用同一个 `InMemoryStore`，并读取第一次 run 保存的 profile：

```json
{"memory_count":1,"second_run":{"memory_hit":true,"recommended_mode":"quiet"}}
```

### `stream_projection`

输出会汇总 projected stream modes，并包含 `final_state` 对象：

```json
{"modes_seen":["events","tasks","checkpoints","values","messages","custom","updates","output"],"final_state":{"answer":"dispatch maintenance window","status":"planned"}}
```

### `subgraph_module`

```json
{"status":"queued","ticket":{"route":"edge-maintenance"}}
```

### `model_tool_model_loop`

final state 包含一条 user message、一条 assistant tool call、一条带 `{"ok":true,"result":{"value":5}}` 的 tool message，以及最终 assistant response。

### `agent_pattern_react`

final state 包含 user message、assistant tool call、sensor observation tool message，以及最终 assistant response：

```json
{"pattern":"react"}
```

### `agent_pattern_plan_and_solve`

```json
{"completed_count":3,"pattern":"plan_and_solve","solved":true}
```

### `agent_pattern_reflection`

```json
{"pattern":"reflection","revision_count":1}
```

### `mock_edge_repair`

final state 包含：

```json
{
  "device_healthy": true,
  "repair_attempts": 1,
  "repair_status": "repaired"
}
```

它也会打印 `checkpoint_count`，用于证明 checkpointed pause/resume path 被执行。

## 5. 覆盖映射

| 能力 | 覆盖示例 |
| --- | --- |
| 最小图执行 | `minimal_graph` |
| fan-out / fan-in | `parallel_fanout`、`conditional_fanout` |
| dynamic `Send` | `send_map_reduce` |
| `Command` routing | `command_goto` |
| loop 和 max-step guard | `loop_graph` |
| checkpoint/resume | `checkpoint_resume`、`sqlite_checkpoint_resume`、`mock_edge_repair` |
| history/replay/update-state | `time_travel_history` |
| store | `long_term_memory_store` |
| stream projection | `stream_projection` |
| subgraph | `subgraph_module` |
| model/tool loop | `model_tool_model_loop`、`llama_cpp_tool_calling` |
| agent patterns | `agent_pattern_react`、`agent_pattern_plan_and_solve`、`agent_pattern_reflection` |
| HITL interrupt | `human_interrupt`、`tool_approval_loop`、`mock_edge_repair` |
| edge adapter shape | `edge_mock_tool_adapter`、`mock_edge_repair` |

## 6. 新增示例规则

新增默认示例时，请同时更新：

- 本矩阵中的示例行；
- 验证命令；
- 预期输出或预期信号；
- 覆盖映射；
- 如涉及新限制或可选依赖，同步更新 [LIMITATIONS.md](LIMITATIONS.md)。
