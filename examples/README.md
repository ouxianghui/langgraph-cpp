# 示例目录说明

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-10 |
| 范围 | `examples/` 中的可运行示例、学习路径和验收信号 |
| 关联文档 | [../docs/EXAMPLE_MATRIX.md](../docs/EXAMPLE_MATRIX.md)、[../docs/tutorials/API_EXAMPLES.md](../docs/tutorials/API_EXAMPLES.md)、[../docs/TRACEABILITY_MATRIX.md](../docs/TRACEABILITY_MATRIX.md) |

示例是 public API 的验收信号。每个默认示例都应能在没有真实 provider、真实硬件或外部 llama.cpp 的情况下构建运行。

## 1. 建议学习顺序

| 顺序 | 示例 | 学习目标 |
| --- | --- | --- |
| 1 | `minimal_graph` | 最小 `START -> node -> END`。 |
| 2 | `conditional_graph`、`loop_graph` | 条件路由和循环。 |
| 3 | `parallel_fanout`、`conditional_fanout` | fan-out/fan-in 和 reducer。 |
| 4 | `send_map_reduce` | `Send` 动态分支和 branch-local state。 |
| 5 | `command_goto` | `Command` update/goto。 |
| 6 | `checkpoint_resume`、`sqlite_checkpoint_resume` | checkpoint、resume 和持久化。 |
| 7 | `human_interrupt`、`tool_approval_loop` | interrupt 和 human-in-the-loop。 |
| 8 | `stream_projection` | stream projected parts 和 envelope。 |
| 9 | `subgraph_module` | parent graph 调用 compiled subgraph。 |
| 10 | `model_tool_model_loop` | mock model/tool loop。 |
| 11 | `edge_mock_tool_adapter`、`mock_edge_repair` | mock hardware/edge workflow。 |

## 2. 运行示例

```sh
cmake --preset unix-debug
cmake --build --preset unix-debug
build/unix-debug/examples/minimal_graph
```

批量运行默认示例并生成 smoke 报告：

```sh
scripts/run-examples.sh
```

完整示例覆盖矩阵见 [../docs/EXAMPLE_MATRIX.md](../docs/EXAMPLE_MATRIX.md)。

## 3. 可选示例

`llama_cpp_chat` 和 `llama_cpp_tool_calling` 需要：

- `LANGGRAPH_CPP_WITH_LLAMA_CPP=ON`；
- 外部 llama.cpp 源码树、安装前缀或 CMake target；
- 本地 GGUF 模型。

它们不是默认 release gate 的必要条件。

## 4. 新增示例规则

- 示例应展示一个清晰能力，不要把多个无关能力塞进同一文件。
- 示例应检查 `Result<T>` 或 `Status`，不要忽略错误。
- 示例默认不访问真实云服务或真实硬件。
- 如果示例是 optional，必须在 [../docs/EXAMPLE_MATRIX.md](../docs/EXAMPLE_MATRIX.md) 标明依赖和运行方式。
