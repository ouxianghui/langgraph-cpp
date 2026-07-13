# 性能模型

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-10 |
| 范围 | graph execution、checkpoint/store、stream、HTTP/provider、未来 benchmark |
| 关联文档 | [CONCURRENCY_MODEL.md](CONCURRENCY_MODEL.md)、[QUALITY_MODEL.md](QUALITY_MODEL.md)、[LIMITATIONS.md](LIMITATIONS.md) |

本文说明当前性能设计和边界。它不宣称已有完整 benchmark 数据，而是定义哪些路径是热路径、哪些成本需要被测量、哪些资源增长必须有边界。

## 1. 性能原则

- 正确性、恢复语义和安全边界优先于优化。
- hot path 需要先定义工作负载和风险，再优化。
- queue、retry、stream、batch 必须有容量、背压或失败策略。
- 不为减少一次 copy 引入不清晰生命周期。
- debug/sanitizer/release 性能数字不能混用。

## 2. Graph execution 成本

| 路径 | 成本来源 | 当前边界 |
| --- | --- | --- |
| super-step scheduling | ready task normalization、task annotation、routing。 | tasks 数量随 fan-out 增长。 |
| node execution | 用户 handler、model/tool/provider/hardware 调用。 | 可通过 `RunOptions::executor_` 并发。 |
| reducer merge | writes 排序、JSON merge、schema validation。 | merge 串行，保证确定性。 |
| stream event | event payload 构建、projection、bounded channel。 | live stream 有 capacity。 |
| checkpoint | state serialization、writes、storage fsync/transaction。 | durability mode 控制写入频率。 |

## 3. 资源增长边界

| 资源 | 风险 | 控制点 |
| --- | --- | --- |
| steps | infinite loop | `ResourceLimits::maxSteps` / recursion limit。 |
| parallel tasks | fan-out explosion | `RunOptions::maxConcurrency_`、应用 routing 限制。 |
| stream buffer | consumer 慢或停止读取 | `RunStreamOptions::capacity_`。 |
| checkpoint history | thread 长期运行 | saver `prune` / retention。 |
| pending writes | parallel failure 或 sync durability | pending writes recovery tests。 |
| message/state size | 大 JSON merge/serialization | resource limits、future benchmark。 |
| HTTP retries | retry storm | retry policy、rate limit、circuit breaker。 |

## 4. 不应做的优化

- 不跳过 reducer/schema/checkpoint error handling 来追求吞吐。
- 不把 raw external payload 放入日志或 metrics 来“方便调试”。
- 不让 owner thread 执行网络 I/O、大序列化或长时间 sleep。
- 不用无界 queue 隐藏背压问题。
- 不把 borrowed view 跨 async 边界传递来避免 copy。

## 5. 当前缺口

- 还没有系统化 benchmark suite。
- 还没有发布级 latency/throughput target。
- semantic/vector store 尚未实现，因此没有 vector search 性能基线。
- Postgres/RocksDB/remote checkpoint backend 未实现，因此没有 backend 对比。
- provider streaming delta 的真实网络性能不属于默认测试门禁。

这些缺口记录在本文和 [LIMITATIONS.md](LIMITATIONS.md) 中。在 benchmark target 真正落地前，不应在 README 中把目标命令描述为已可用命令。

## 6. 建议验证

| 修改类型 | 建议验证 |
| --- | --- |
| graph scheduling/reducer | graph runtime tests + future graph benchmark。 |
| checkpoint/storage | checkpoint/storage/crash tests + future checkpoint throughput benchmark。 |
| stream/channel | stream tests + async channel tests + TSAN。 |
| HTTP/provider | HTTP client tests + redaction tests + future streaming benchmark。 |
| cache/blob/filesystem | targeted foundation tests + future large payload benchmark。 |

## 7. 未来 Benchmark 工作负载

benchmark 必须可重复，记录 compiler、build type、CPU、storage、filesystem 和 sanitizer 状态。Debug、sanitizer、Release 数字分开记录。每个 benchmark 同时报告吞吐、延迟分位、内存峰值和错误数；I/O benchmark 必须标明 durability mode 和 fsync/WAL 配置。

| ID | 工作负载 | 指标 | 关联风险 |
| --- | --- | --- | --- |
| B-001 | Linear graph 100/1000 steps。 | step latency、allocations、events/sec。 | runtime overhead |
| B-002 | Fan-out/fan-in 10/100/1000 branches。 | super-step duration、merge cost、max memory。 | fan-out explosion |
| B-003 | Reducer merge large JSON state。 | merge latency、copy cost、payload size。 | large state |
| B-004 | InMemorySaver checkpoint 1000+ history。 | put/get/list throughput、history memory。 | checkpoint history growth |
| B-005 | SQLite checkpoint 1000+ history。 | write latency、resume latency、db size。 | durability |
| B-006 | Pending writes large payload。 | write/read latency、recovery cost。 | failed parallel step recovery |
| B-007 | Stream projected parts with slow consumer。 | backpressure behavior、drop/error/close semantics。 | stream queue growth |
| B-008 | SSE parser large event stream。 | events/sec、partial frame handling。 | provider streaming |
| B-009 | Message/tool-call JSON parse。 | parse latency、error reporting cost。 | provider/tool ecosystem |
| B-010 | Store search/list namespace. | op latency、filter cost、namespace cardinality。 | long-term memory |
| B-011 | Blob/cache large payload. | throughput、disk usage、eviction behavior。 | foundation I/O |
| B-012 | HTTP retry/rate-limit/circuit breaker. | retry overhead、queue wait、error latency。 | provider resilience |

建议 benchmark 输出 JSONL 或 CSV，至少包含 benchmark id、git commit、compiler and version、build preset、OS and architecture、input size、iteration count、p50/p95/p99 latency、throughput、peak RSS、failure count 和 notes。

未来目标命令形态：

```sh
cmake --preset unix-release -DLANGGRAPH_CPP_BUILD_BENCHMARKS=ON
cmake --build --preset unix-release --target langgraph_cpp_benchmarks
build/unix-release/benchmarks/langgraph_cpp_benchmarks --benchmark_format=json
```
