# langgraph-cpp 路线图

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-10 |
| 当前发布线 | `v0.1.0-alpha` / 开发者预览 |
| 范围来源 | [PRD.md](PRD.md) |
| 工作包来源 | [internal/WBS.md](internal/WBS.md) |

## 1. 文档目的

本文说明 `langgraph-cpp` 如何从当前 alpha runtime 继续走向稳定的 C++ edge runtime for stateful agents。它不是历史任务日志；实现级工作放在 [internal/WBS.md](internal/WBS.md)，产品范围与验收标准放在 [PRD.md](PRD.md)。

## 2. 当前状态

项目已经越过最初 MVP 实现阶段。当前重点是 `v0.1.0-alpha` 发布加固：保持 runtime 可用，明确真实支持边界，并让 CI 足够强，确保开源发布时不夸大生产成熟度。

当前状态摘要：

- core graph execution、state update、reducer、fan-out/fan-in、`Send`、`Command`、subgraph、interrupt、replay、checkpoint history、projected stream 已实现。
- in-memory 和 SQLite-backed checkpoint/store 路径已存在，SQLite 路径包含 crash recovery 和 corruption 测试。
- message、model、provider、tool、optional llama.cpp、edge-adapter surface 已存在；测试和示例默认使用 fake/mock。
- 公共 API 已向 LangGraph-style 命名收敛，并移除了旧兼容别名。
- release gate 已包含 sanitizer、TSAN repeat stress、libFuzzer smoke、conformance、clang-tidy、docs snippet compile 和 macOS smoke。

## 3. 已完成里程碑

| 里程碑 | 状态 | 结果 |
| --- | --- | --- |
| M0 项目基础 | Done | CMake、C++23 配置、tests、examples、public aggregate header。 |
| M1 Graph runtime | Done | `StateGraph`、`CompiledStateGraph`、validation、invoke、loop、conditional routing、fan-out/fan-in。 |
| M2 State and checkpoint | Done | JSON `State`、`StateUpdate`、reducers、checkpoint saver contract、in-memory 和 SQLite-backed recovery。 |
| M3 Model/tool loop | Done | messages、`FakeChatModel`、model node、tools、registry、executor、tool node、schema validation。 |
| M4 Streaming and HITL | Done | runtime events、stream projections、`StreamWriter`、interrupts、`Command::resume`。 |
| M5 Edge differentiation | mock path Done | mock edge adapters 和 mock edge repair workflow；真实硬件 binding 仍是扩展工作。 |
| Compatibility hardening | 当前切片 Done | config bridge、stream projection fields、namespaces、subgraph history、checkpoint saver naming、store shape、message/tool deltas。 |
| Reliability hardening | 当前切片 Done | crash recovery harness、fuzz/stress tests、sanitizer/TSAN gates、failure injection tests。 |

## 4. 当前发布：`v0.1.0-alpha`

目标：发布一个可信的 developer preview，适合实验、示例和早期集成，同时明确 pre-1.0 API 和 backend 限制。

发布要求：

- 公共 API contract 在 [API_CONTRACT.md](API_CONTRACT.md) 中版本化；
- 已知差距在 [LIMITATIONS.md](LIMITATIONS.md) 中明确记录；
- 默认测试不依赖 Python、真实 cloud provider、真实硬件和外部 llama.cpp；
- optional dependency 通过 CMake option 控制；
- 文档和示例与实际构建保持一致；
- 满足 [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) 中的 release gate。

## 5. 近期路线

| 方向 | 目标 | 价值 |
| --- | --- | --- |
| 文档 readiness | PRD、roadmap、WBS、architecture、example matrix、API contract、traceability、test catalog、security、compatibility、performance、risk 与当前 release 对齐。 | 公开仓库前需要可信入口。 |
| HTTP/SSE hardening | async failure logs redaction、SSE parser buffer bound、OAuth refresh failure 语义。 | provider 集成依赖可靠 transport 和安全日志。 |
| 示例验证 | 用当前 build 刷新 example matrix 和非 optional examples。 | 示例是新用户最快的验收信号。 |
| CI 信号质量 | 保持 Linux GCC/Clang、ASAN/UBSAN、TSAN repeat、clang-tidy、fuzz smoke、conformance、docs snippets、macOS smoke 绿色。 | 开源贡献需要清晰质量门禁。 |
| 发布包装 | 确认 license、notices、changelog、tag、生成物和 build artifact hygiene。 | 减少发布摩擦。 |

## 6. 后续产品增量

| 增量 | 状态 | 候选范围 |
| --- | --- | --- |
| `v0.2` Durable backends | Planned | Postgres 或 RocksDB checkpoint adapter、retention policy、remote store adapter shape、migration docs。 |
| `v0.3` Semantic memory | Planned | Store vector/semantic search backend、embedding adapter interface、query limits、fake embeddings deterministic tests。 |
| `v0.4` Observability exports | Planned | OpenTelemetry exporter、LangSmith-compatible event sink exploration、trace schema docs、bounded-cardinality metrics。 |
| `v0.5` Provider maturity | Planned | 更广 provider profiles、provider-specific streaming deltas、production auth rotation guidance、更多 provider contract tests。 |
| `v0.6` Edge/hardware adapters | Planned | UART/I2C/CAN/ROS2 adapter behind options、hardware policy middleware、integration test strategy。 |
| `v1.0` Stability line | Future | Source compatibility policy、migration guide、schema migration policy、release cadence、deprecation rules。 |

## 7. 长期方向

长期目标是成为可嵌入 C++ 应用和边缘系统的稳定 workflow kernel。项目应保持：

- core 小而明确；
- state 和 checkpoint 边界显式；
- 默认 provider-neutral、hardware-neutral；
- recoverable error 必须显式返回；
- public API 变更保守；
- 对 LangGraph-compatible 与 C++-specific 的边界保持诚实。

潜在长期能力：

- production checkpoint backends 和 managed retention；
- semantic memory 与 vector store adapters；
- OpenTelemetry 和 LangSmith-compatible observability surface；
- 更广 provider 与 multimodal content 支持；
- 不进入默认 core 的硬件集成包；
- persisted state 和 checkpoint schema migration tooling。

## 8. 风险与检查点

| 风险 | 检查点 |
| --- | --- |
| 文档比实现更乐观。 | `LIMITATIONS.md` 和 `EXAMPLE_MATRIX.md` 必须作为 release blocker。 |
| compatibility claim 与 upstream LangGraph 漂移。 | parity 变更必须同步 conformance tests 和 API contract。 |
| 新 backend 给默认 runtime 引入重依赖。 | optional adapter 放在 CMake option 和 injected interface 后面。 |
| provider/hardware 工作污染 core。 | provider/hardware 逻辑留在 adapter boundary，不进入 graph execution。 |
| pre-1.0 API churn 影响用户。 | 使用 API contract version、changelog 和 removal note。 |

## 9. 阶段验证门禁

| 阶段 | 聚焦 gate | 更广 gate |
| --- | --- | --- |
| Docs readiness | Markdown link check、docs snippet compile、`git diff --check` | README/docs review against current source and examples。 |
| Runtime behavior | `graph_runtime_test`、`langgraph_compat_test` | Full `ctest --test-dir build/unix-debug --output-on-failure`。 |
| Persistence | checkpoint、storage、crash recovery tests | SQLite WAL/DELETE durability matrix、multi-process contention tests。 |
| Concurrency | TSAN repeat label、stream/resume stress tests | Linux TSAN CI job。 |
| Provider/HTTP | provider model、HTTP client tests | redaction、SSE、retry、streaming、auth、failure injection tests。 |
| Release | [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) | GitHub Actions release gate。 |

## 10. 关联文档

- [PRD.md](PRD.md)：产品目标、范围、非目标和验收标准。
- [internal/WBS.md](internal/WBS.md)：具体工作包和验证命令。
- [ARCHITECTURE.md](ARCHITECTURE.md)：模块边界、运行流程和图。
- [TRACEABILITY_MATRIX.md](TRACEABILITY_MATRIX.md)：需求到源码、测试、示例和 release gate 的映射。
- [TEST_CATALOG.md](TEST_CATALOG.md)：测试职责和行为契约覆盖。
- [LIMITATIONS.md](LIMITATIONS.md)：已知限制和 release 风险摘要。
- [PERFORMANCE_MODEL.md](PERFORMANCE_MODEL.md)：未来 benchmark 工作负载和指标。
- [LIMITATIONS.md](LIMITATIONS.md)：当前边界和延后工作。
- [EXAMPLE_MATRIX.md](EXAMPLE_MATRIX.md)：示例覆盖和验证状态。
