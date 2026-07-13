# langgraph-cpp 工作分解结构

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-10 |
| 产品范围 | [../PRD.md](../PRD.md) |
| 路线图 | [../ROADMAP.md](../ROADMAP.md) |
| 发布门禁 | [../RELEASE_CHECKLIST.md](../RELEASE_CHECKLIST.md) |

## 1. 文档目的

本文把 `langgraph-cpp` 的工作拆成可 review、可验证的 work package。它是 [../PRD.md](../PRD.md) 的实现侧配套文档，也是 [../ROADMAP.md](../ROADMAP.md) 的任务层展开。

历史 MVP 任务不再作为冗长 backlog 重复展开，而是汇总成当前工作包。新增工作应包含稳定 WBS ID、状态、依赖、验收标准和验证命令。

## 2. 状态说明

| 状态 | 含义 |
| --- | --- |
| Done | 已实现，并有测试、示例或文档门禁覆盖。 |
| Partial | 当前切片已实现，但仍有明确延后工作。 |
| Planned | 未来增量范围内，尚未实现。 |
| Deferred | 有价值，但不属于当前 release track。 |
| Blocked | 需要产品、依赖或环境决策才能继续。 |

## 3. 工作包模板

| 字段 | 要求 |
| --- | --- |
| WBS ID | 稳定编号，例如 `WBS-3.2`。 |
| Work package | 简短名词短语，说明交付物。 |
| Status | 使用上面的状态枚举。 |
| Dependencies | 前置 work package、build option 或外部服务。 |
| Acceptance | 可观察结果，而不是内部意图。 |
| Verification | 聚焦命令、测试、示例或 CI gate。 |

## 4. 当前工作分解

| WBS ID | 工作包 | 状态 | 依赖 | 验收 | 验证 |
| --- | --- | --- | --- | --- | --- |
| WBS-0.1 | 项目构建基础 | Done | 无 | CMake 可构建 core、tests、examples，optional component 由 option 控制。 | `cmake --preset unix-debug`; `cmake --build --preset unix-debug` |
| WBS-0.2 | Public aggregate header | Done | WBS-0.1 | 用户可通过 `<langgraph_cpp/langgraph.hpp>` 使用主 runtime surface。 | examples build |
| WBS-0.3 | Release CI gate | Partial | WBS-0.1 | CI 覆盖 Linux GCC/Clang、ASAN/UBSAN、TSAN repeat、fuzz smoke、conformance、clang-tidy、docs snippets、macOS smoke。 | GitHub Actions |
| WBS-1.1 | Graph declaration API | Done | WBS-0.2 | `StateGraph` 支持 nodes、edges、conditional edges、entry/finish、sequence、subgraph、command routes。 | `graph_runtime_test`; examples |
| WBS-1.2 | Compiled runtime | Done | WBS-1.1 | `CompiledStateGraph` 支持 invoke、stream、resume、replay、update state、history。 | `graph_runtime_test` |
| WBS-1.3 | Parallel super-step | Done | WBS-1.2 | fan-out nodes 基于同一 state snapshot 执行，并确定性 merge。 | `parallel_fanout`; fan-out tests |
| WBS-1.4 | `Send` dynamic fan-out | Done | WBS-1.2 | branch-local state 和 fan-in reducer 可 checkpoint/resume。 | `send_map_reduce`; compat tests |
| WBS-1.5 | `Command` routing | Done | WBS-1.2 | nodes/tools 可返回 state update 和声明过的 goto targets。 | `command_goto`; tool command tests |
| WBS-1.6 | Subgraph execution | Done | WBS-1.2 | parent graph 可调用 compiled subgraph，并携带 namespace、projection、parent routing。 | `subgraph_module`; compat tests |
| WBS-2.1 | JSON state and updates | Done | WBS-0.2 | nodes 通过 `StateUpdate` 更新 graph state；JSON conversion 报错清晰。 | state/update tests |
| WBS-2.2 | Reducer registry | Done | WBS-2.1 | overwrite、append、add_messages、object merge、custom reducer 确定性执行。 | reducer tests; fan-out tests |
| WBS-2.3 | Runtime schema validation | Done | WBS-2.1 | input/state/output/tool schemas 校验失败时返回显式错误。 | JSON schema tests |
| WBS-3.1 | Checkpoint saver contract | Done | WBS-2.1 | `BaseCheckpointSaver` 暴露当前 `put`、`putWrites`、`get`、`getTuple`、`list`、`deleteThread`，不保留旧别名。 | checkpoint tests |
| WBS-3.2 | In-memory checkpoint saver | Done | WBS-3.1 | tests/examples 可在无持久化存储时 checkpoint/resume。 | checkpoint tests |
| WBS-3.3 | Storage-backed checkpoint saver | Done | WBS-3.1 | SQLite-backed storage 支持 persist、reopen、list、resume、prune、copy、delete runs。 | storage/checkpoint tests |
| WBS-3.4 | Pending writes recovery | Done | WBS-3.3 | failed parallel super-step 保存成功 task writes，resume 只重跑失败 task。 | graph runtime tests |
| WBS-3.5 | Crash recovery hardening | Done | WBS-3.3 | hard-exit、timeout-kill、corruption、stale latest pointer、contention 行为正确。 | `crash_recovery_test` |
| WBS-3.6 | Additional checkpoint backends | Planned | WBS-3.1 | RocksDB/Postgres/remote adapter 可不改 core runtime 增量接入。 | future adapter tests |
| WBS-4.1 | Store contract | Done | WBS-2.1 | Store 使用 batch/op 形状，并提供 put/get/search/delete/listNamespaces helpers。 | store tests |
| WBS-4.2 | In-memory and storage stores | Done | WBS-4.1 | long-term memory 可用 memory 和 `IStorage` 实现。 | `long_term_memory_store`; store tests |
| WBS-4.3 | Semantic/vector store backend | Planned | WBS-4.1 | 配置 backend 后，`query_` search 可走 vector/semantic backend。 | future store tests |
| WBS-5.1 | Runtime events | Done | WBS-1.2 | 执行过程发出 node、checkpoint、interrupt、error、custom events。 | event tests |
| WBS-5.2 | Projected streams | Done | WBS-5.1 | updates、values、tasks、checkpoints、interrupts、errors、messages、custom、output、namespace、output keys、subgraph projection 可用。 | `stream_projection`; compat tests |
| WBS-5.3 | Interrupt and resume | Done | WBS-3.1 | node/function-style/multi/replay/tool interrupts 均可 checkpoint/resume。 | interrupt tests; examples |
| WBS-5.4 | Observability exporters | Planned | WBS-5.1 | OpenTelemetry/LangSmith-compatible export 可不改 runtime event 接入。 | future exporter tests |
| WBS-6.1 | Message model | Done | WBS-2.1 | base messages、content blocks、多模态 blocks、tool calls、chunks、JSON conversion 可用。 | message tests |
| WBS-6.2 | Chat model interface | Done | WBS-6.1 | `BaseChatModel` 支持 invoke、stream、batch、bindTools。 | model tests |
| WBS-6.3 | Provider model adapter | Done | WBS-6.2 | OpenAI-compatible、Anthropic、Qwen、DeepSeek profiles 可通过 injected HTTP transport 工作。 | provider model tests |
| WBS-6.4 | Optional llama.cpp adapter | Partial | external llama.cpp | `LANGGRAPH_CPP_WITH_LLAMA_CPP=ON` 时编译运行；真实模型质量取决于环境。 | optional examples |
| WBS-6.5 | Tool system | Done | WBS-2.3 | tools 显式注册、schema-validated、policy-aware，并可返回 structured result 或 command。 | tool tests; examples |
| WBS-7.1 | Edge adapter interfaces | Done | WBS-6.5 | mock 和 sysfs GPIO adapter surface 可通过 `EdgeAdapterRegistry` 注册。 | edge examples |
| WBS-7.2 | Real hardware adapters | Deferred | hardware/library decisions | UART/I2C/CAN/ROS2 adapters behind options。 | future integration tests |
| WBS-8.1 | HTTP/SSE client | Partial | foundation network | request auth、retry、rate limit、circuit breaker、streaming、SSE、proxy、TLS、redaction、tests 已有；仍有 hardening 项。 | HTTP client tests |
| WBS-8.2 | Fuzz and pressure tests | Done | core pure-logic modules | JSON schema、state merge、checkpoint codec、envelope、SSE、message/tool JSON、namespace、config、stress paths 已覆盖。 | `langgraph_cpp_fuzz_pressure_test`; CI fuzz smoke |
| WBS-8.3 | Docs and examples | Partial | all runtime packages | 文档匹配当前文件职责，examples 反映当前支持范围，并包含 traceability、test catalog、security、compatibility、performance、risk 证据链。 | Markdown link check; docs snippet test |
| WBS-8.4 | Release packaging | Planned | WBS-0.3, WBS-8.3 | public tag 包含 clean docs、changelog、license、notices，且没有 generated artifacts。 | [../RELEASE_CHECKLIST.md](../RELEASE_CHECKLIST.md) |

## 5. 当前发布工作

| WBS ID | 工作包 | 状态 | 验收 |
| --- | --- | --- | --- |
| WBS-8.1a | Redact async HTTP failure paths | Planned | queue/full 或 async failure logs 不记录可能包含 secret 的 raw query/path。 |
| WBS-8.1b | Bound SSE parser buffering | Planned | malformed no-newline SSE stream 不会导致无限内存增长。 |
| WBS-8.1c | Clarify OAuth refresh failure behavior | Planned | refresh hook 失败时 queued async requests 不会无可观察地无限等待。 |
| WBS-8.3a | Refresh canonical docs | Done | PRD、roadmap、WBS、architecture、example matrix、traceability、test catalog、security、compatibility、performance、risk 与文件名职责和当前项目状态一致。 |
| WBS-8.3b | Refresh example verification | Done | 非 optional examples 已构建运行，matrix 日期与实际命令一致。 |

## 6. 验证命令地图

| 区域 | 聚焦命令 |
| --- | --- |
| 默认构建和测试 | `cmake --preset unix-debug`; `cmake --build --preset unix-debug`; `ctest --test-dir build/unix-debug --output-on-failure` |
| 文档代码片段 | `ctest --test-dir build/unix-debug -L docs --output-on-failure` |
| Graph/runtime 行为 | `ctest --test-dir build/unix-debug -R "graph|compat" --output-on-failure` |
| HTTP/provider | `ctest --test-dir build/unix-debug -R "http_client|provider_chat_model" --output-on-failure` |
| Fuzz/pressure | `ctest --test-dir build/unix-debug -R langgraph_cpp_fuzz_pressure_test --output-on-failure` |
| Crash recovery | `ctest --test-dir build/unix-debug -R crash_recovery --output-on-failure` |
| Markdown hygiene | Markdown link check + `git diff --check` |

## 7. 新增工作包规则

1. 在所属区域选择下一个稳定 WBS ID。
2. 在当前工作分解或当前发布工作表中新增一行。
3. 写清具体验收信号和聚焦验证命令。
4. 如果改变 milestone 或 release sequence，同步更新 [../ROADMAP.md](../ROADMAP.md)。
5. 如果改变支持边界，同步更新 [../LIMITATIONS.md](../LIMITATIONS.md)。

## 8. 关联文档

- [../PRD.md](../PRD.md)：产品需求与验收标准。
- [../ROADMAP.md](../ROADMAP.md)：里程碑顺序。
- [../ARCHITECTURE.md](../ARCHITECTURE.md)：runtime 和模块架构。
- [../TRACEABILITY_MATRIX.md](../TRACEABILITY_MATRIX.md)：需求到证据链追踪。
- [../TEST_CATALOG.md](../TEST_CATALOG.md)：测试目录和行为契约覆盖。
- [../LIMITATIONS.md](../LIMITATIONS.md)：限制和风险摘要。
- [../EXAMPLE_MATRIX.md](../EXAMPLE_MATRIX.md)：示例覆盖。
- [../RELEASE_CHECKLIST.md](../RELEASE_CHECKLIST.md)：发布门禁清单。
