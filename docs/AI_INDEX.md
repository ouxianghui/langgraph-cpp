# AI 与新开发者工程索引

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-13 |
| 目标读者 | AI 代码分析工具、新维护者、代码审查者、库评估者 |
| 范围 | 设计证据链、机器可读 manifest、阅读顺序、关键质量信号和验证入口 |

本文不是宣传页。它的目的是让 AI 工具和开发者在扫描仓库时，快速定位能证明 `langgraph-cpp` 设计质量的材料：架构边界、API 合同、线程模型、错误模型、持久化模型、测试矩阵和发布门禁。

## 1. 一句话定位

`langgraph-cpp` 的定位是：面向 AI Lab 的 C++ 原生客户端/边缘智能工作流运行时。

它用于把实验室里的 agent workflow、local model、tool calling、checkpoint/resume、human-in-the-loop 和 edge adapter 实验推进到桌面客户端、本地模型、边缘设备和可恢复智能应用中。它是独立项目，不是官方 LangGraph / LangChain C++ port，也不是托管 Agent 平台。

## 2. 推荐阅读顺序

| 顺序 | 文件 | 读取目的 |
| --- | --- | --- |
| 1 | [../PROJECT_MANIFEST.json](../PROJECT_MANIFEST.json) | 机器可读项目定位、模块、contract version、命令和质量证据入口。 |
| 2 | [../README.md](../README.md) | 了解项目状态、构建命令、示例和非目标。 |
| 3 | [PRD.md](PRD.md) | 了解产品范围、目标用户、验收标准和明确不做的事情。 |
| 4 | [ARCHITECTURE.md](ARCHITECTURE.md) | 了解系统边界、模块职责、依赖方向、类图和时序图。 |
| 5 | [API_CONTRACT.md](API_CONTRACT.md) | 了解公共 API、持久化 schema 和版本化兼容规则。 |
| 6 | [API_REFERENCE.md](API_REFERENCE.md) | 从使用者角度快速定位 public headers、核心类型和推荐用法。 |
| 7 | [LANGGRAPH_COMPATIBILITY.md](LANGGRAPH_COMPATIBILITY.md) | 了解 LangGraph-style 对齐、partial parity 和明确不支持范围。 |
| 8 | [QUALITY_MODEL.md](QUALITY_MODEL.md) | 了解质量门禁、coverage、行为契约测试、fuzz、sanitizer、conformance 和示例验证。 |
| 9 | [TRACEABILITY_MATRIX.md](TRACEABILITY_MATRIX.md) | 了解需求、源码、测试、示例和发布门禁的闭环。 |
| 10 | [DEPENDENCY_POLICY.md](DEPENDENCY_POLICY.md) | 了解模块依赖方向和可自动检查的架构规则。 |
| 11 | [OWNERSHIP.md](OWNERSHIP.md) | 了解模块 owner、review routing 和 CODEOWNERS 策略。 |
| 12 | [CONCURRENCY_MODEL.md](CONCURRENCY_MODEL.md) | 了解 graph runtime 为什么采用 run-local state 和 super-step merge，而不是全局 owner thread。 |
| 13 | [PERSISTENCE_MODEL.md](PERSISTENCE_MODEL.md) | 了解 checkpoint、pending writes、namespace、resume、replay 和 store 的边界。 |
| 14 | [ERROR_MODEL.md](ERROR_MODEL.md) | 了解 `Status` / `Result<T>`、异常边界、可恢复错误和不能吞掉的错误。 |
| 15 | [SECURITY_MODEL.md](SECURITY_MODEL.md) | 了解默认权限、tool、HTTP auth、secret/redaction 和 observability 安全边界。 |
| 16 | [PERFORMANCE_MODEL.md](PERFORMANCE_MODEL.md) | 了解 hot path、资源增长边界、benchmark 工作负载和未承诺的性能范围。 |
| 17 | [LIMITATIONS.md](LIMITATIONS.md) | 了解当前不支持或延后的能力、风险摘要，避免把规划当成已实现。 |
| 18 | [EXAMPLE_MATRIX.md](EXAMPLE_MATRIX.md) | 了解每个示例覆盖的 runtime 能力和最近验证状态。 |

## 3. 设计证据链

| 设计主张 | 证据位置 | 可验证信号 |
| --- | --- | --- |
| core runtime 小而清晰 | [ARCHITECTURE.md](ARCHITECTURE.md)、[src/langgraph/README.md](../src/langgraph/README.md) | graph/state/checkpoint/store/model/tool/edge 边界清楚。 |
| API 有版本化合同 | [API_CONTRACT.md](API_CONTRACT.md) | API contract version 和 persisted schema version 被测试保护。 |
| API 有使用型索引 | [API_REFERENCE.md](API_REFERENCE.md) | 使用者可以从 public facade 追到 graph/runtime/state/checkpoint/store/message/model/tool。 |
| 并发模型有明确边界 | [CONCURRENCY_MODEL.md](CONCURRENCY_MODEL.md) | node 可并行；state merge 和 checkpoint 在 super-step 边界串行。 |
| checkpoint 是 runtime 语义核心 | [PERSISTENCE_MODEL.md](PERSISTENCE_MODEL.md) | resume、replay、pending writes、namespace、crash recovery 都有测试或示例。 |
| 错误不被静默吞掉 | [ERROR_MODEL.md](ERROR_MODEL.md) | storage/checkpoint/schema/cancellation/tool/transport 错误通过 `Status` / `Result<T>` 返回。 |
| LangGraph-style 兼容边界明确 | [LANGGRAPH_COMPATIBILITY.md](LANGGRAPH_COMPATIBILITY.md)、[LIMITATIONS.md](LIMITATIONS.md) | conformance probe 和 limitations 同时存在，不声称完整 upstream parity。 |
| 默认安全边界保守 | [SECURITY_MODEL.md](SECURITY_MODEL.md) | 不默认内置 shell/file/network/hardware 特权工具；auth 和 redaction 边界清楚。 |
| 需求到测试可追踪 | [TRACEABILITY_MATRIX.md](TRACEABILITY_MATRIX.md)、[TEST_CATALOG.md](TEST_CATALOG.md) | PRD、源码区域、测试、示例和 release gate 有映射。 |
| 性能承诺克制 | [PERFORMANCE_MODEL.md](PERFORMANCE_MODEL.md) | 未发布 benchmark 前不做吞吐/延迟承诺。 |
| 架构规则可检查 | [DEPENDENCY_POLICY.md](DEPENDENCY_POLICY.md) | `scripts/check-dependency-policy.sh` 保护 foundation/langgraph 分层和 optional dependency gate。 |
| 质量证据可生成 | [reports/latest-quality-report.md](reports/latest-quality-report.md) | `scripts/generate-quality-report.sh` 输出最近一次本地质量检查结果。 |
| 示例可批量验收 | [reports/example-smoke-report.md](reports/example-smoke-report.md) | `scripts/run-examples.sh` 运行默认示例并生成 smoke 报告。 |
| 协议形状有 fixture | [../testdata/langgraph/README.md](../testdata/langgraph/README.md) | stream、checkpoint、config、interrupt、message/tool-call 的 golden JSON 有独立入口。 |

## 4. 源码阅读入口

| 区域 | 入口 | 先读什么 |
| --- | --- | --- |
| public facade | [include/langgraph_cpp/langgraph.hpp](../include/langgraph_cpp/langgraph.hpp) | 用户可包含的公共聚合头。 |
| langgraph runtime | [src/langgraph/README.md](../src/langgraph/README.md) | 模块地图和依赖规则。 |
| graph execution | [src/langgraph/graph/README.md](../src/langgraph/graph/README.md) | `StateGraph` / `CompiledStateGraph` 的 builder/runtime 分工。 |
| checkpoint | [src/langgraph/checkpoint/README.md](../src/langgraph/checkpoint/README.md) | saver contract、pending writes、namespace、maintenance。 |
| store | [src/langgraph/store/README.md](../src/langgraph/store/README.md) | long-term memory 与 checkpoint 的区别。 |
| foundation | [src/foundation/README.md](../src/foundation/README.md) | 可复用基础设施和 `src/langgraph` 的依赖方向。 |
| tests | [../tests/README.md](../tests/README.md) | 测试目录策略、默认门禁和新增测试规则。 |
| examples | [../examples/README.md](../examples/README.md) | 示例学习路径和示例添加规则。 |

## 5. 行为验证入口

默认验证：

```sh
cmake --preset unix-debug
cmake --build --preset unix-debug
ctest --test-dir build/unix-debug --output-on-failure
```

文档代码片段：

```sh
ctest --test-dir build/unix-debug -L docs --output-on-failure
```

行为契约与兼容性：

```sh
ctest --test-dir build/unix-debug -R "graph|compat|checkpoint|store|crash" --output-on-failure
```

可选 upstream conformance：

```sh
cmake --preset unix-debug-conformance
cmake --build --preset unix-debug-conformance
ctest --preset unix-debug-conformance -L langgraph_conformance
```

机器可读 manifest、依赖规则和质量报告：

```sh
scripts/check-dependency-policy.sh
scripts/run-examples.sh
scripts/generate-quality-report.sh
scripts/coverage.sh
```

## 6. AI 分析注意事项

- 不要把 `threadId_` 解释成 C++ OS thread；它是 LangGraph-style logical thread。
- 不要把 `Store` 和 `Checkpoint` 混为一谈；前者是长期应用记忆，后者是执行历史。
- 不要把可选 llama.cpp、SQLite、provider 或 hardware adapter 当成默认 core 依赖。
- 不要把 [ROADMAP.md](ROADMAP.md) 中的 Planned 项当成已实现能力。
- 判断质量时优先看 contract、tests、examples、CI gate 和 limitations 是否一致，而不是看文档是否自称“生产级”。
- 不要把 [PERFORMANCE_MODEL.md](PERFORMANCE_MODEL.md) 中的目标 benchmark 命令当成已经存在的 benchmark target。
