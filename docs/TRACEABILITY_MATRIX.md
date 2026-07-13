# 追踪矩阵

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-10 |
| 范围 | PRD 需求、架构模块、测试、示例、CI/release gate 的闭环关系 |
| 关联文档 | [PRD.md](PRD.md)、[QUALITY_MODEL.md](QUALITY_MODEL.md)、[TEST_CATALOG.md](TEST_CATALOG.md)、[EXAMPLE_MATRIX.md](EXAMPLE_MATRIX.md)、[RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) |

本文把产品需求、架构设计、测试、示例和发布门禁串成一张可审计矩阵。它的目标是让维护者和 AI 工具能快速判断：某个能力是否只是文档声明，还是已经有代码、测试和示例支撑。

## 1. 产品需求追踪

| PRD ID | 需求摘要 | 主要设计文档 | 主要源码区域 | 测试证据 | 示例证据 | 发布/CI 证据 |
| --- | --- | --- | --- | --- | --- | --- |
| FR-001 | 用 C++ callback 声明 graph node 和 edge。 | [ARCHITECTURE.md](ARCHITECTURE.md)、[ADR/0006-stategraph-compile-boundary.md](ADR/0006-stategraph-compile-boundary.md) | `src/langgraph/graph` | `graph_runtime_test.cpp` | `minimal_graph`、`conditional_graph` | 默认 CTest |
| FR-002 | node 通过 `StateUpdate` 或 `Command` 修改状态。 | [ADR/0001-super-step-execution.md](ADR/0001-super-step-execution.md)、[ADR/0005-state-update-reducer-boundary.md](ADR/0005-state-update-reducer-boundary.md) | `src/langgraph/state`、`src/langgraph/graph` | `graph_runtime_test.cpp`、`serialization_test.cpp` | `command_goto`、`parallel_fanout` | API contract tests |
| FR-003 | runtime 在 super-step 边界 checkpoint。 | [PERSISTENCE_MODEL.md](PERSISTENCE_MODEL.md)、[ADR/0002-checkpoint-boundary.md](ADR/0002-checkpoint-boundary.md) | `src/langgraph/checkpoint`、`src/langgraph/graph` | `checkpointer_test.cpp`、`crash_recovery_test.cpp` | `checkpoint_resume`、`sqlite_checkpoint_resume` | storage/checkpoint CI |
| FR-004 | 支持 human-in-the-loop interrupt。 | [PERSISTENCE_MODEL.md](PERSISTENCE_MODEL.md) | `src/langgraph/runtime`、`src/langgraph/graph`、`src/langgraph/tool` | `langgraph_compat_test.cpp`、`graph_runtime_test.cpp` | `human_interrupt`、`tool_approval_loop` | compat/conformance |
| FR-005 | streaming 暴露运行时投影。 | [ARCHITECTURE.md](ARCHITECTURE.md)、[ADR/0009-stream-envelope-and-projection.md](ADR/0009-stream-envelope-and-projection.md) | `src/langgraph/graph/stream*` | `graph_runtime_test.cpp`、`langgraph_compat_test.cpp` | `stream_projection` | docs snippets、compat |
| FR-006 | Store/checkpoint API 贴近 LangGraph-style contract。 | [API_CONTRACT.md](API_CONTRACT.md)、[LANGGRAPH_COMPATIBILITY.md](LANGGRAPH_COMPATIBILITY.md)、[ADR/0007-store-checkpoint-separation.md](ADR/0007-store-checkpoint-separation.md) | `src/langgraph/checkpoint`、`src/langgraph/store` | `checkpointer_test.cpp`、store tests、`langgraph_compat_test.cpp` | `long_term_memory_store`、checkpoint examples | conformance |
| FR-007 | model/tool loop 不依赖真实 provider。 | [ARCHITECTURE.md](ARCHITECTURE.md) | `src/langgraph/message`、`src/langgraph/model`、`src/langgraph/tool` | `agent_loop_test.cpp`、`provider_chat_model_test.cpp` | `model_tool_model_loop` | 默认 examples |
| FR-008 | provider 和 llama.cpp 集成保持可选。 | [LIMITATIONS.md](LIMITATIONS.md)、[ADR/0008-optional-integrations-behind-ports.md](ADR/0008-optional-integrations-behind-ports.md) | `src/langgraph/model`、`src/foundation/network` | `provider_chat_model_test.cpp`、`http_client_test.cpp` | `llama_cpp_chat`、`llama_cpp_tool_calling` optional | CMake options |
| FR-009 | 默认不内置危险工具。 | [SECURITY_MODEL.md](SECURITY_MODEL.md)、[ADR/0010-explicit-tool-registration.md](ADR/0010-explicit-tool-registration.md) | `src/langgraph/tool`、`src/langgraph/edge` | `edge_adapter_test.cpp`、tool tests | `edge_mock_tool_adapter`、`mock_edge_repair` | release checklist |
| FR-010 | 可恢复错误通过 `Status` / `Result<T>` 显式返回。 | [ERROR_MODEL.md](ERROR_MODEL.md)、[ADR/0003-status-result-error-model.md](ADR/0003-status-result-error-model.md) | `src/foundation/status`、所有 runtime ports | `status_result_test.cpp`、`failure_injection_test.cpp` | 多数 examples 检查 `Result` | default CTest |

## 2. 非功能需求追踪

| 类别 | 需求 | 设计证据 | 验证证据 |
| --- | --- | --- | --- |
| 语言与构建 | C++23、CMake、可选依赖 option。 | `CMakePresets.json`、[README.md](../README.md) | Linux/macOS build gates。 |
| 可移植性 | Linux/macOS，edge Linux/ARM64 目标。 | [ROADMAP.md](ROADMAP.md)、[LIMITATIONS.md](LIMITATIONS.md) | macOS smoke、Linux CI。 |
| 可靠性 | crash recovery、corruption、contention、pending writes。 | [PERSISTENCE_MODEL.md](PERSISTENCE_MODEL.md) | `crash_recovery_test.cpp`、`failure_injection_test.cpp`。 |
| 并发 | parallel super-step、owner 边界、bounded stream。 | [CONCURRENCY_MODEL.md](CONCURRENCY_MODEL.md) | TSAN、`async_channel_test.cpp`、`executor_test.cpp`。 |
| 安全 | 无默认危险工具、redaction、auth 边界。 | [SECURITY_MODEL.md](SECURITY_MODEL.md) | `redaction_test.cpp`、`secrets_test.cpp`、`http_client_test.cpp`。 |
| 可观测性 | events、metrics、trace、stream envelope。 | [ARCHITECTURE.md](ARCHITECTURE.md) | `event_test.cpp`、`observability_test.cpp`、`stream_projection`。 |
| 兼容性 | API/schema version、pre-1.0 policy。 | [API_CONTRACT.md](API_CONTRACT.md) | `versioning_test.cpp`、conformance preset。 |
| 可测试性 | core tests 不依赖真实 provider/hardware/Python。 | [QUALITY_MODEL.md](QUALITY_MODEL.md) | default CTest、optional conformance 单独 preset。 |

## 3. 模块到证据映射

| 模块 | 主要职责 | 设计文档 | 测试 | 示例 |
| --- | --- | --- | --- | --- |
| `src/langgraph/graph` | graph builder/runtime、super-step、stream、resume/replay。 | [CONCURRENCY_MODEL.md](CONCURRENCY_MODEL.md) | `graph_runtime_test.cpp`、`langgraph_compat_test.cpp` | `minimal_graph`、`stream_projection` |
| `src/langgraph/checkpoint` | saver contract、pending writes、history、maintenance。 | [PERSISTENCE_MODEL.md](PERSISTENCE_MODEL.md) | `checkpointer_test.cpp`、`crash_recovery_test.cpp` | `checkpoint_resume` |
| `src/langgraph/store` | long-term memory。 | [PERSISTENCE_MODEL.md](PERSISTENCE_MODEL.md) | store-related tests | `long_term_memory_store` |
| `src/langgraph/message/model/tool` | messages、model adapters、tool execution。 | [SECURITY_MODEL.md](SECURITY_MODEL.md) | `agent_loop_test.cpp`、`provider_chat_model_test.cpp` | `model_tool_model_loop` |
| `src/foundation/network` | HTTP/SSE、request auth。 | [SECURITY_MODEL.md](SECURITY_MODEL.md) | `http_client_test.cpp` | provider model tests |
| `src/foundation/serialization` | state/checkpoint/envelope codecs。 | [PERSISTENCE_MODEL.md](PERSISTENCE_MODEL.md) | `serialization_test.cpp`、`content_envelope_test.cpp` | checkpoint examples |
| `src/foundation/async/executor/threading` | channels、executors、owner guards。 | [CONCURRENCY_MODEL.md](CONCURRENCY_MODEL.md) | `async_channel_test.cpp`、`executor_test.cpp` | graph executor examples |

## 4. 缺口如何记录

如果某个需求没有完整闭环，应同时更新：

1. [LIMITATIONS.md](LIMITATIONS.md)：说明当前不支持或 partial 的范围；
2. [internal/WBS.md](internal/WBS.md)：加入 work package；
3. [ROADMAP.md](ROADMAP.md)：如果影响 release 顺序，更新 milestone；
4. 本文：标明缺少测试、示例或 CI gate；
5. [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md)：如果是发布门禁，加入 checklist。
