# ADR-0008: 将 provider、storage、network 和 hardware 集成放在可选端口后

| 字段 | 内容 |
| --- | --- |
| 状态 | Accepted |
| 日期 | 2026-07-10 |
| 关联 | [../DEPENDENCY_POLICY.md](../DEPENDENCY_POLICY.md)、[../SECURITY_MODEL.md](../SECURITY_MODEL.md)、[../LIMITATIONS.md](../LIMITATIONS.md) |

## 背景

`langgraph-cpp` 的 core runtime 必须能在没有真实 cloud provider、真实硬件、外部 llama.cpp、Python 或远端服务的环境中构建和测试。与此同时，项目也需要给 SQLite、HTTP、provider model、llama.cpp、edge hardware adapter 等能力留出扩展路径。

## 决策

默认 core 通过接口和 CMake options 管理集成边界：

- provider model 通过 `IChatModel` / provider adapter 注入；
- tool 和 edge 能力通过显式 registry/adapter 注入；
- SQLite、network、llama.cpp、crypto、compression 等能力保持可选；
- CI 和 dependency policy 检查 optional dependency gate；
- 默认测试不依赖真实 provider、真实硬件或外部 llama.cpp。

## 被拒绝的方案

| 方案 | 拒绝原因 |
| --- | --- |
| 默认链接所有 provider/hardware SDK | 增加构建摩擦，破坏 edge/minimal runtime 目标。 |
| 把真实云服务作为测试前提 | 默认测试不可复现，也不适合离线开发。 |
| 在 core runtime 内直接调用硬件或网络副作用 | 安全边界不清，mock 和权限控制困难。 |

## 后果

- 默认构建更轻，核心测试可离线运行。
- 可选示例必须明确依赖和运行方式。
- 新增集成必须先定义端口或 adapter 边界。
- dependency policy 需要持续防止 optional dependency 泄漏到 core。

## 验证

- `scripts/check-dependency-policy.sh`
- default CTest
- `provider_chat_model_test`
- `http_client_test`
- `edge_mock_tool_adapter`
- optional llama.cpp examples 不进入默认 release gate
