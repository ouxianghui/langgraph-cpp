# ADR-0010: 工具必须显式注册并通过 schema / policy 边界执行

| 字段 | 内容 |
| --- | --- |
| 状态 | Accepted |
| 日期 | 2026-07-10 |
| 关联 | [../SECURITY_MODEL.md](../SECURITY_MODEL.md)、[../API_CONTRACT.md](../API_CONTRACT.md)、[../LIMITATIONS.md](../LIMITATIONS.md) |

## 背景

agent runtime 很容易被误解为“模型可以直接执行任意文件、网络、shell 或硬件操作”。`langgraph-cpp` 面向 edge runtime，真实应用可能接入设备、HTTP、文件或运维动作，因此默认权限边界必须保守。

## 决策

core runtime 不内置危险工具。应用必须通过 `ToolRegistry` 显式注册工具，并为工具声明名称、描述、输入 schema、执行回调和可选 policy。工具调用来自 message/tool-call JSON，但实际执行前仍由 registry 查找、schema 校验和 policy gate 控制。

edge/hardware 能力通过 adapter 暴露为普通工具，不绕过 registry。

## 被拒绝的方案

| 方案 | 拒绝原因 |
| --- | --- |
| 默认提供 shell/file/network/hardware 工具 | 安全风险过高，也不适合库级默认行为。 |
| 允许模型直接指定任意 C++ callable | 难以审计权限、schema 和错误边界。 |
| schema 只作为文档不参与校验 | tool 输入错误会延迟到副作用执行阶段。 |

## 后果

- 默认 runtime 安全边界更清楚。
- 应用必须显式承担工具权限设计。
- tool-call parser、registry、schema validator 和 policy 都属于安全关键路径。
- 新增高权限工具示例必须保持 mock 或 optional，不进入默认危险能力。

## 验证

- tool 相关测试
- `agent_loop_test`
- `edge_adapter_test`
- `model_tool_model_loop`
- `tool_approval_loop`
- `edge_mock_tool_adapter`
- `mock_edge_repair`
