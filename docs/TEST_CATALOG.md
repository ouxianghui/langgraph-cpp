# 测试目录

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-10 |
| 范围 | `tests/`、`fuzz/`、示例验证与 conformance 的测试责任 |
| 关联文档 | [QUALITY_MODEL.md](QUALITY_MODEL.md)、[TRACEABILITY_MATRIX.md](TRACEABILITY_MATRIX.md)、[../tests/README.md](../tests/README.md) |

本文列出当前测试文件的职责，帮助维护者和 AI 工具判断测试是否覆盖行为契约，而不是只覆盖 happy path。

## 1. 核心 runtime 测试

| 测试文件 | 主要覆盖 |
| --- | --- |
| `graph_runtime_test.cpp` | graph execution、fan-out/fan-in、conditional、loop、`Send`、`Command`、stream、resume/replay、pending writes。 |
| `langgraph_compat_test.cpp` | LangGraph-style shape、namespace、interrupt、stream projection、config 等兼容切片。 |
| `checkpointer_test.cpp` | saver contract、put/get/list/deleteThread、pending writes、maintenance、namespace。 |
| `crash_recovery_test.cpp` | SQLite reopen、hard-exit、partial write、stale latest pointer、corruption、contention。 |
| `failure_injection_test.cpp` | storage/checkpoint/foundation 失败注入和错误传播。 |
| `docs_snippet_compile_test.cpp` | README/API 文档代码片段编译。 |
| `versioning_test.cpp` | API/schema contract version pin。 |

## 2. LangGraph-facing 测试

| 测试文件 | 主要覆盖 |
| --- | --- |
| `langgraph_conformance_probe.cpp` | C++ conformance probe，输出可与 Python LangGraph 对比的行为。 |
| `python_langgraph_conformance.py` | 可选 Python runner，对比 upstream LangGraph 关键兼容切片。 |

这些测试不属于默认 Python-free gate，需要 `unix-debug-conformance` preset 和本地 Python `langgraph` 包。

## 3. Foundation 测试

| 测试文件 | 主要覆盖 |
| --- | --- |
| `async_channel_test.cpp` | bounded/unbuffered channel、select、sender/receiver、shutdown。 |
| `executor_test.cpp` | inline/serial/concurrent/owner executor 行为。 |
| `threading_timer_test.cpp` | thread、thread pool、timer lifecycle。 |
| `storage_test.cpp` | memory/SQLite storage contract。 |
| `serialization_test.cpp` | state/checkpoint JSON codec。 |
| `content_envelope_test.cpp` | envelope、checksum、compression/encryption wrapper behavior。 |
| `json_schema_test.cpp` | JSON Schema builder/compile/validate/error path。 |
| `http_client_test.cpp` | HTTP config、transport、auth、retry、SSE、stream、redaction-adjacent behavior。 |
| `redaction_test.cpp` | logger/event/trace/codec redaction。 |
| `observability_test.cpp` | metrics、trace、event observation。 |
| `event_test.cpp` | runtime event sink behavior。 |
| `logging_test.cpp` | logger contract。 |
| `scheduler_test.cpp` | task scheduler、retry、lifecycle。 |
| `process_test.cpp` | process runner validation、I/O、timeout/cancel。 |
| `filesystem_test.cpp` | path utils、temp files、atomic writes。 |
| `blob_store_test.cpp` | memory/filesystem blob stores、metadata、checksum/listing。 |
| `cache_test.cpp` | memory/disk cache、eviction、persistence。 |
| `crypto_test.cpp`、`encryption_test.cpp` | crypto primitives、secure payload/storage/codec。 |
| `compression_test.cpp` | compressor contract。 |
| `config_test.cpp` | config value/env/loader behavior。 |
| `cancellation_test.cpp` | cancellation token/source behavior。 |
| `lifecycle_test.cpp`、`runtime_services_test.cpp` | lifecycle and runtime service container behavior。 |
| `rate_limit_test.cpp`、`retry_policy_test.cpp` | rate limiter、circuit breaker、retry policy。 |
| `resource_limits_test.cpp` | resource limit builders and enforcement helpers。 |
| `secrets_test.cpp` | secret provider contract。 |
| `id_generator_test.cpp`、`time_test.cpp` | ID/time utilities。 |
| `status_result_test.cpp` | `Status` / `Result<T>` semantics。 |

## 4. Model/tool/edge 测试

| 测试文件 | 主要覆盖 |
| --- | --- |
| `agent_loop_test.cpp` | fake model -> tool -> model loop。 |
| `provider_chat_model_test.cpp` | provider profiles、HTTP injection、stream/tool-call parsing。 |
| `edge_adapter_test.cpp` | hardware adapter registry and mock edge surfaces。 |

## 5. Fuzz 与压力

| 入口 | 主要覆盖 |
| --- | --- |
| `fuzz_pressure_test.cpp` | 纯逻辑 fuzz-like inputs、压力路径、large state/message/pending writes。 |
| `fuzz/json_schema_fuzzer.cpp` | JSON schema parser/validator。 |
| `fuzz/state_reducer_fuzzer.cpp` | StateUpdate/reducer merge。 |
| `fuzz/checkpoint_codec_fuzzer.cpp` | checkpoint codec。 |
| `fuzz/content_envelope_fuzzer.cpp` | content envelope。 |
| `fuzz/sse_parser_fuzzer.cpp` | SSE parser。 |
| `fuzz/message_tool_json_fuzzer.cpp` | message/tool-call JSON。 |
| `fuzz/graph_namespace_fuzzer.cpp` | graph namespace parser。 |
| `fuzz/run_config_fuzzer.cpp` | RunnableConfig merge/patch/apply。 |

## 6. 常用命令

默认测试：

```sh
ctest --test-dir build/unix-debug --output-on-failure
```

文档测试：

```sh
ctest --test-dir build/unix-debug -L docs --output-on-failure
```

行为契约聚焦：

```sh
ctest --test-dir build/unix-debug -R "graph|compat|checkpoint|store|crash|stream" --output-on-failure
```

可选 conformance：

```sh
ctest --preset unix-debug-conformance -L langgraph_conformance
```

