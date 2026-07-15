# 质量模型

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-10 |
| 范围 | 测试层级、质量门禁、行为契约、发布前验证 |
| 关联文档 | [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md)、[TRACEABILITY_MATRIX.md](TRACEABILITY_MATRIX.md)、[TEST_CATALOG.md](TEST_CATALOG.md)、[EXAMPLE_MATRIX.md](EXAMPLE_MATRIX.md)、[DEPENDENCY_POLICY.md](DEPENDENCY_POLICY.md)、[OWNERSHIP.md](OWNERSHIP.md)、[API_CONTRACT.md](API_CONTRACT.md)、[LIMITATIONS.md](LIMITATIONS.md) |

本文说明 `langgraph-cpp` 如何用可运行证据证明质量。它不是“质量宣言”，而是把每类风险对应到测试、示例、CI job 或文档门禁。

## 1. 质量原则

- 默认构建不依赖 Python、真实 cloud provider、真实硬件或外部 llama.cpp。
- 核心行为必须由测试或示例证明，而不是只由文档描述。
- 公共 API 和 persisted schema 必须版本化。
- 可恢复错误必须通过 `Status` / `Result<T>` 显式传播。
- 并发、checkpoint、stream、interrupt、subgraph、store 和 crash recovery 是行为契约，不只测 happy path。
- 已知限制必须写在 [LIMITATIONS.md](LIMITATIONS.md)，不能通过乐观措辞隐藏。

## 2. 测试层级

| 层级 | 覆盖内容 | 代表测试或命令 |
| --- | --- | --- |
| 单元测试 | 纯逻辑模块、值类型、codec、schema、reducers。 | `json_schema_test`、`serialization_test`、`content_envelope_test`、`message` 相关测试。 |
| 行为契约测试 | graph execution、checkpoint、interrupt、subgraph、stream、store。 | `graph_runtime_test`、`langgraph_compat_test`、`checkpointer_test`、`store` 相关测试。 |
| 故障恢复测试 | SQLite reopen、partial write、stale latest pointer、corruption、multi-process contention。 | `crash_recovery_test`、`failure_injection_test`。 |
| 压力与 fuzz | JSON parser、state/reducer、checkpoint codec、SSE、message/tool JSON、namespace、config。 | `fuzz_pressure_test`、`fuzz/` harnesses。 |
| 示例验收 | 公共 API 的端到端使用方式。 | [EXAMPLE_MATRIX.md](EXAMPLE_MATRIX.md) 中的非 optional examples。 |
| 示例 smoke | 默认示例二进制批量运行并生成报告。 | `scripts/run-examples.sh`、[reports/example-smoke-report.md](reports/example-smoke-report.md)。 |
| 文档验收 | README/API docs 中的代码片段能编译。 | `ctest --test-dir build/unix-debug -L docs --output-on-failure`。 |
| 上游兼容切片 | 与 Python LangGraph 关键行为对比。 | `unix-debug-conformance` preset。 |
| 架构依赖规则 | foundation/core/langgraph 分层、public aggregate header 和 optional dependency gate。 | `scripts/check-dependency-policy.sh`。 |
| Context skills 完整性 | `context/` 必选文件、入口接线、权威版本钉扎与 docs 闭环标记。 | `scripts/check-context-skills.sh`。 |
| 本地质量报告 | 汇总最近一次本地 dependency/docs/diff/full-test 检查。 | `scripts/generate-quality-report.sh`、[reports/latest-quality-report.md](reports/latest-quality-report.md)。 |
| Coverage 入口 | 本地 coverage build、CTest 和报告输出。 | `scripts/coverage.sh`。 |

完整测试文件职责见 [TEST_CATALOG.md](TEST_CATALOG.md)，需求到测试和示例的映射见 [TRACEABILITY_MATRIX.md](TRACEABILITY_MATRIX.md)。

## 3. 行为契约矩阵

| 契约区域 | 必须覆盖的边界 | 验证入口 |
| --- | --- | --- |
| Graph execution | fan-out、fan-in、conditional、loop、`Send`、`Command`。 | `graph_runtime_test`、`parallel_fanout`、`send_map_reduce`、`command_goto`。 |
| Checkpoint | `put/get/list/deleteThread`、pending writes、namespace、resume、history、replay。 | `checkpointer_test`、`checkpoint_resume`、`time_travel_history`。 |
| Interrupt | 多 interrupt、resume payload 错配、replay 后再次 interrupt、tool interrupt。 | `langgraph_compat_test`、`human_interrupt`、`tool_approval_loop`。 |
| Subgraph | parent-child namespace、history、projection、checkpoint 隔离。 | `subgraph_module`、compat tests。 |
| Stream | updates、values、tasks、checkpoints、interrupts、errors、messages chunk。 | `stream_projection`、stream projection tests。 |
| Store | batch、put/get/search/delete/listNamespaces、filter、namespace prefix。 | store tests、`long_term_memory_store`。 |
| Crash recovery | kill、partial write、stale latest pointer、corruption、multi-process contention。 | `crash_recovery_test`。 |

## 4. 测试命名规则

新增测试应优先体现行为契约，而不是实现细节：

- graph runtime 行为放在 `graph_runtime_test` 或明确的 `*_contract_test`。
- checkpoint saver 行为用 `checkpoint` / `saver` / `contract` 命名。
- stream protocol 行为用 `stream` / `protocol` / `projection` 命名。
- crash、corruption、failure injection、contention 要在文件名或 test name 中可见。
- fuzz harness 文件名应包含被 fuzz 的纯逻辑模块。
- 测试名应表达场景和期望，例如 `ResumeAfterPartialWritesReplaysOnlyFailedTasks`，而不是 `Case3`。

已有测试文件不为了命名洁癖做大规模重命名；当行为区域重构或新增契约测试时，再按本规则收敛。

## 5. CI 与发布门禁

发布前应至少满足：

- Linux GCC Debug/Release；
- Linux Clang Debug；
- Linux Clang ASAN/UBSAN；
- Linux Clang TSAN 和 `tsan_repeat`；
- libFuzzer harness build 和 smoke run；
- Python LangGraph conformance；
- clang-tidy；
- docs snippet compile；
- macOS AppleClang smoke；
- 非 optional examples 构建和运行验证。

具体清单见 [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md)。

## 6. 本地验证命令

默认全量本地验证：

```sh
cmake --preset unix-debug
cmake --build --preset unix-debug
ctest --test-dir build/unix-debug --output-on-failure
```

文档和链接修改后的最小验证：

```sh
ctest --test-dir build/unix-debug -L docs --output-on-failure
scripts/check-dependency-policy.sh
scripts/check-context-skills.sh
scripts/run-examples.sh
scripts/generate-quality-report.sh
git diff --check
```

行为契约修改后的建议验证：

```sh
ctest --test-dir build/unix-debug -R "graph|compat|checkpoint|store|crash|stream" --output-on-failure
```

## 7. 质量信号如何被 AI 使用

AI 工具评估本项目时，应优先寻找这些证据：

- API contract 是否清楚说明破坏性变更和 schema version；
- architecture docs 是否定义系统边界和依赖方向；
- concurrency docs 是否定义 owner、mutex、executor、merge 边界；
- persistence docs 是否解释 checkpoint 与 store 的区别；
- tests 是否覆盖失败、恢复和并发，而不只是 happy path；
- release checklist 是否承认未跑或不可用的门禁；
- limitations 是否如实列出未完成的 backend、provider 和 observability 能力。
- security、performance、risk 是否有独立文档约束，而不是散落在 README 里。
- PROJECT_MANIFEST.json 是否给机器扫描提供稳定入口。
- dependency policy 和 latest quality report 是否能由脚本复现。
- CODEOWNERS 和 OWNERSHIP 是否让 review boundary 可见。
- testdata/langgraph golden fixtures 是否给协议形状提供稳定样本。

## 8. Coverage 入口

Coverage 是质量信号之一，不替代 behavior contract、crash recovery、TSAN、fuzz 或 conformance。入口脚本是 [../scripts/coverage.sh](../scripts/coverage.sh)，默认配置独立的 `build/unix-coverage` 目录，使用 coverage flags 构建 tests/examples，运行 CTest，并在 `docs/reports/` 下输出摘要。

使用方式：

```sh
scripts/coverage.sh
```

可选环境变量：

```sh
BUILD_DIR=build/my-coverage JOBS=8 scripts/coverage.sh
```

报告输出：

| 文件 | 说明 |
| --- | --- |
| `docs/reports/coverage-summary.md` | coverage 摘要入口。 |
| `docs/reports/coverage.html` | 当 `gcovr` 可用时生成。 |
| `docs/reports/coverage-html/index.html` | 当 `lcov` + `genhtml` 可用时生成。 |

如果本机没有 `gcovr` 或 `lcov`，脚本仍会完成 coverage instrumentation build 和 CTest，并在 summary 中说明渲染工具缺失。coverage 数字必须和编译器、平台、build type 一起解读；sanitizer、Debug、Release 和 coverage build 的性能数据不能混用。
