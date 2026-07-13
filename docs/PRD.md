# langgraph-cpp 产品需求文档

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中，pre-1.0 开发者预览 |
| 产品版本 | `v0.1.0-alpha` |
| 最后更新 | 2026-07-10 |
| 产品领域 | 面向有状态 Agent 的 C++23 边缘运行时 |
| 关联文档 | [AI_INDEX.md](AI_INDEX.md)、[ROADMAP.md](ROADMAP.md)、[internal/WBS.md](internal/WBS.md)、[API_CONTRACT.md](API_CONTRACT.md)、[QUALITY_MODEL.md](QUALITY_MODEL.md)、[TRACEABILITY_MATRIX.md](TRACEABILITY_MATRIX.md)、[LIMITATIONS.md](LIMITATIONS.md) |

## 1. 产品概述

`langgraph-cpp` 是一个 C++23 边缘运行时，用于构建有状态 Agent 和可恢复工作流图。它提供 LangGraph-style 的执行内核，围绕显式 state、graph node、conditional routing、checkpoint、interrupt、streaming event、message、model adapter、tool 和 edge adapter 组织运行过程。

本项目不是官方 LangGraph 或 LangChain C++ 移植版，而是一个独立的、社区导向的 C++ runtime。核心运行时必须在没有 Python、真实模型 provider、真实硬件和云服务的情况下完成构建与测试。

## 2. 问题陈述

靠近设备、本地模型或低延迟控制环的 C++ 系统，通常不只需要一次性请求处理器，而是需要可以长期运行、可暂停、可恢复、可观测的状态机式工作流。

这类系统需要：

- 在多步骤执行过程中保留显式状态；
- 在进程崩溃、设备重启或人工介入后恢复；
- 在运行中持续输出事件和进度；
- 在危险操作或不确定决策前安全暂停；
- 通过统一接口接入模型、工具、存储和硬件能力；
- 在不依赖真实硬件、真实云服务或真实模型的情况下进行测试。

Python LangGraph 在 Python 应用中解决了许多类似的编排问题。`langgraph-cpp` 的目标是把这类 runtime 思路带到原生 C++ 和 edge-first 场景，同时保持核心实现小而清晰。

## 3. 产品目标

| 目标 | 说明 | 成功信号 |
| --- | --- | --- |
| G1 可恢复图执行 | 执行有状态图，并在明确边界写入 checkpoint。 | linear、conditional、loop、fan-out、`Send`、`Command`、subgraph、resume 测试通过。 |
| G2 边缘优先 | 核心不绑定 Python、真实云 provider 或真实硬件。 | 默认构建和测试不需要外部 provider、硬件或 llama.cpp。 |
| G3 LangGraph-style 兼容 | 在命名、checkpoint、stream、interrupt、subgraph、config 语义上尽量贴近 LangGraph 生态。 | compatibility golden tests 和 Python conformance probe 在 CI 中通过。 |
| G4 可观测运行时 | 暴露 runtime event、projected stream、checkpoint、interrupt、error 和 message chunk。 | stream 测试覆盖 updates、values、tasks、checkpoints、interrupts、errors、messages。 |
| G5 安全扩展面 | 提供 model、tool、store、checkpoint、HTTP、edge adapter 接口，但不内置危险工具。 | 默认没有 shell/file/network/hardware 特权工具；工具必须显式注册并校验 schema。 |
| G6 高质量发布门禁 | 使用 build、sanitizer、fuzz、conformance、clang-tidy 和文档测试保护质量。 | CI 覆盖 Linux GCC/Clang、ASAN/UBSAN、TSAN、fuzz smoke、docs snippets、macOS smoke。 |

## 4. 非目标

- 不做托管 Agent 平台、云控制面、LangSmith 替代品或 trace UI。
- 不复刻完整 LangChain provider 生态。
- 不承诺完整 Python LangGraph 二进制格式和所有事件细节同构。
- 不默认内置 shell、文件、网络或硬件工具。
- `1.0` 前不承诺 ABI 稳定。
- 初始版本不承诺 RTOS 支持。

## 5. 目标用户

| 用户 | 需要完成的事情 |
| --- | --- |
| 边缘和机器人开发者 | 把感知、规划、行动、验证、重试和人工交接组织成可恢复工作流。 |
| 本地模型开发者 | 在 C++ 中运行 llama.cpp/GGUF 或 mock model tool-call loop。 |
| 系统工程师 | 构建能 checkpoint、resume、replay、查询历史并恢复进程重启的工作流运行时。 |
| 运行时集成者 | 在稳定接口后接入模型 provider、checkpoint store、vector store、硬件 adapter 或 observability sink。 |
| 库评估者 | 通过示例、API 合同、已知限制和 release gate 判断是否采用。 |

## 6. 产品范围

### `v0.1.0-alpha` 范围内

| 模块 | 需求 |
| --- | --- |
| Graph runtime | `StateGraph`、`CompiledStateGraph`、普通边、条件边、loop、fan-out/fan-in、`Send`、`Command`、subgraph、max-step guard。 |
| State | JSON-backed `State`、`StateUpdate`、field reducer、`add_messages`、object merge、custom reducer、runtime schema validation。 |
| Checkpoint | `BaseCheckpointSaver`、`InMemorySaver`、`StorageSaver`、checkpoint namespace、pending writes、history、replay、update-state fork、delete/copy/prune 维护能力和 async facade。 |
| Durability | `Async`、`Sync`、`Exit` 三种 durability mode，以及必要的 task-level writes。 |
| Store | `BaseStore`、`InMemoryStore`、`StorageStore`、batch op、namespace prefix search、filter、delete、namespace listing。 |
| Streaming | collected/live stream API、projected stream parts、LangGraph-style event envelope、message chunk、tasks、checkpoints、interrupts、errors、output keys、subgraph projection controls。 |
| Interrupt | node interrupt、function-style sequential interrupt、multi-interrupt super-step、resume payload matching、replay/resume 行为、tool-level HITL hook。 |
| Message/model/tool | `BaseMessage`、LangChain-style content blocks、多模态 content block JSON、`ToolCall`、`BaseChatModel`、`FakeChatModel`、`ProviderChatModel`、可选 `LlamaCppChatModel`、`BaseTool`、`FunctionTool`、`ToolExecutor`、`ToolNode`、tool schema validation、structured tool error。 |
| Edge adapter | mock edge workflow、edge adapter interfaces、`EdgeAdapterRegistry`、sysfs GPIO adapter surface。 |
| Foundation | storage、serialization、content envelope、HTTP/SSE client、scheduler、executor、cancellation、logging、metrics、tracing、resource limits、fuzz/stress harness、failure injection tests。 |
| Documentation | README、PRD、roadmap、WBS、architecture、API contract、API examples、example matrix、limitations、release checklist。 |

### 延后或扩展范围

| 模块 | 延后内容 |
| --- | --- |
| Checkpoint backend | RocksDB、Postgres、remote/cloud checkpoint store、托管 retention policy。 |
| Store backend | semantic/vector search backend、remote store adapter。 |
| Observability | LangSmith-compatible backend、trace UI、OpenTelemetry exporter、托管 telemetry service。 |
| Provider ecosystem | 完整 provider 参数面、生产级 token rotation、非标准 content block、citation/reasoning delta、真实 provider 集成测试。 |
| Hardware ecosystem | UART、I2C、CAN、ROS2 action/service binding、deployment-specific hardware policy middleware。 |
| Python parity | 完整 `JsonPlusSerializer`、`_DeltaSnapshot` 以及所有 upstream stream/event 边界行为。 |

## 7. 功能需求

| ID | 需求 | 验收标准 |
| --- | --- | --- |
| FR-001 | 用户可以用 C++ callback 声明 graph node 和 edge。 | 非法图定义返回 `Status`；合法示例可以编译运行。 |
| FR-002 | node 只能通过 `StateUpdate` 或 `Command` update 修改图状态。 | reducer 行为确定；不要求可变全局状态。 |
| FR-003 | runtime 在 super-step 边界 checkpoint。 | resume、replay、history、pending-write recovery、crash recovery 测试通过。 |
| FR-004 | runtime 支持 human-in-the-loop interrupt。 | interrupt 前写入 checkpoint；resume payload 被校验；replay 后可再次 interrupt。 |
| FR-005 | streaming 暴露运行时投影。 | stream projections 覆盖 updates、values、tasks、checkpoints、interrupts、errors、messages、custom、output。 |
| FR-006 | Store 和 checkpoint API 尽量贴近 LangGraph-style contract。 | 公共接口只保留当前 contract；旧兼容别名移除。 |
| FR-007 | model/tool loop 不依赖真实 provider 即可运行。 | `FakeChatModel` 和 `ToolNode` 示例能驱动 model-tool-model loop。 |
| FR-008 | provider 和 llama.cpp 集成保持可选。 | 默认构建不需要外部 llama.cpp 或云 credentials。 |
| FR-009 | 默认不内置危险工具。 | 应用必须显式注册工具，并可提供授权策略。 |
| FR-010 | 可恢复错误通过 `Status` / `Result<T>` 显式返回。 | checkpoint、storage、schema、cancellation、tool、transport 错误不被静默吞掉。 |

## 8. 非功能需求

| 类别 | 需求 |
| --- | --- |
| 语言与构建 | C++23、CMake、可选依赖通过 option 控制。 |
| 可移植性 | Linux/macOS 开发路径；edge Linux 和 ARM64 是产品目标。 |
| 可靠性 | 覆盖 crash recovery、corruption reporting、SQLite multi-process contention、pending writes、stale latest-pointer reconciliation。 |
| 并发 | parallel super-step 和 async service 必须有明确 owner、必要的 bounded queue 和 shutdown 行为。 |
| 安全 | 不内置特权工具；HTTP auth provider-neutral；日志和 trace 默认 redact secret，不 dump 原始 payload。 |
| 可观测性 | runtime event、stream envelope、metrics、trace record 需要提供调试所需上下文，同时避免高基数 secret label。 |
| 兼容性 | API 和 persisted schema 有版本号；`1.0` 前 ABI 不稳定。 |
| 可测试性 | core tests 不依赖 Python、真实硬件、真实 cloud provider 或外部 llama.cpp。 |

## 9. 发布验收标准

`v0.1.0-alpha` 可发布的条件：

- Linux GCC 和 Linux Clang 的默认 configure/build/CTest 通过；
- Linux ASAN/UBSAN 和 TSAN gate 通过；
- clang-tidy release gate 通过；
- fuzz harness smoke run 在 CI 中通过；
- Python LangGraph conformance probe 在支持的兼容切片内通过；
- docs snippet compile test 通过；
- 非 optional examples 构建通过，example matrix 与实际验证一致；
- README、limitations、API contract、release checklist 和本 PRD 描述真实 release 状态；
- 已知差距被明确记录，而不是暗示已支持。

## 10. 风险与缓解

| 风险 | 缓解 |
| --- | --- |
| upstream LangGraph 演进快于 C++ runtime。 | conformance tests 显式化，文档写清兼容边界，不声称完整 parity。 |
| edge deployment 需要比本地 SQLite 更强的 durability。 | storage interface 保持可插拔，Postgres/RocksDB/remote stores 作为 optional adapter 增量接入。 |
| provider 集成细节过多。 | core 保持 provider-neutral；provider profile 放在 `ProviderChatModel` 和注入的 `IHttpClient` 后面。 |
| tool/hardware 集成可能不安全。 | 显式注册、schema validation、policy hook、无特权默认值。 |
| 文档与实现漂移。 | [internal/WBS.md](internal/WBS.md)、[EXAMPLE_MATRIX.md](EXAMPLE_MATRIX.md)、[LIMITATIONS.md](LIMITATIONS.md)、[RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) 绑定 release gate。 |

## 11. 关联文档

- [ROADMAP.md](ROADMAP.md)：里程碑与发布顺序。
- [internal/WBS.md](internal/WBS.md)：工作包与验证命令。
- [ARCHITECTURE.md](ARCHITECTURE.md)：系统边界、模块职责、图和运行流程。
- [AI_INDEX.md](AI_INDEX.md)：AI 工具和新开发者的工程入口。
- [QUALITY_MODEL.md](QUALITY_MODEL.md)：质量门禁和行为契约证据链。
- [TRACEABILITY_MATRIX.md](TRACEABILITY_MATRIX.md)：需求、源码、测试、示例和发布门禁的追踪矩阵。
- [SECURITY_MODEL.md](SECURITY_MODEL.md)：权限、tool、auth、redaction 和 observability 安全模型。
- [PERFORMANCE_MODEL.md](PERFORMANCE_MODEL.md)：性能原则、hot path 和资源增长边界。
- [CONCURRENCY_MODEL.md](CONCURRENCY_MODEL.md)：线程、owner 和 super-step 并发模型。
- [ERROR_MODEL.md](ERROR_MODEL.md)：`Status` / `Result<T>` 错误模型。
- [PERSISTENCE_MODEL.md](PERSISTENCE_MODEL.md)：checkpoint、pending writes、namespace、store 持久化模型。
- [API_CONTRACT.md](API_CONTRACT.md)：source API 和 persisted schema contract。
- [tutorials/API_EXAMPLES.md](tutorials/API_EXAMPLES.md)：公共 API 示例。
- [EXAMPLE_MATRIX.md](EXAMPLE_MATRIX.md)：可运行示例覆盖矩阵。
- [LIMITATIONS.md](LIMITATIONS.md)：当前边界和延后工作。
- [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md)：公开发布检查清单。
