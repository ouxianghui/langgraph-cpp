# 错误模型

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-10 |
| 范围 | `Status`、`Result<T>`、异常边界、可恢复错误传播 |
| 关联文档 | [API_CONTRACT.md](API_CONTRACT.md)、[QUALITY_MODEL.md](QUALITY_MODEL.md)、[LIMITATIONS.md](LIMITATIONS.md) |

本文说明 `langgraph-cpp` 如何表达错误。核心规则是：可恢复错误通过 `Status` 或 `Result<T>` 显式返回；runtime 不应静默吞掉 checkpoint、storage、schema、cancellation、tool 或 transport 错误。

## 1. 基本类型

| 类型 | 用途 |
| --- | --- |
| `Status` | 表达成功或失败，包含错误码和消息。 |
| `Result<T>` | 表达要么得到 `T`，要么得到 `Status` 错误。 |
| C++ exception | 只作为边界保护；runtime 会捕获 node/tool/provider 边界异常并转换为 `Status`。 |

## 2. 使用规则

- 公共 API 的可恢复失败返回 `Status` / `Result<T>`。
- parser、codec、storage、schema、checkpoint、HTTP、tool、model、runtime 都不能用裸 bool 隐藏失败原因。
- node handler 抛出的异常会被 runtime 捕获并变成 failed node status。
- `StatusCode::Aborted` 可用于函数式 interrupt 的内部控制流，但必须带有可观察 interrupt event/checkpoint。
- `StatusCode::Unimplemented` 用于明确未支持能力，例如未配置 vector backend 的 semantic search。
- 读取未来 schema version 时，应返回 `StatusCode::Unimplemented`，除非实现了迁移。

## 3. 不能吞掉的错误

| 区域 | 错误示例 | 期望行为 |
| --- | --- | --- |
| checkpoint | put/list/get/delete 失败 | run 返回失败 status，并发出 failure event。 |
| pending writes | task writes 写入失败 | 不假装 resume-safe；返回错误。 |
| storage | SQLite busy/corruption/schema mismatch | 返回明确 status；测试覆盖可恢复边界。 |
| schema | input/state/output/tool schema 不匹配 | 返回 invalid argument 或 schema validation error。 |
| cancellation | cancellation token 被触发 | 当前 run 停止并返回 cancellation status。 |
| tool | input validation、authorization、handler、output validation 失败 | 返回 structured tool error 或 runtime status。 |
| transport | HTTP auth、retry exhausted、stream callback failure | 返回 status，不记录 secret。 |

## 4. 异常边界

允许在内部捕获异常的边界：

- node handler；
- node error handler；
- tool handler；
- provider adapter callback；
- async executor task；
- HTTP/SSE callback；
- codec/parser 包装层。

捕获后应转换为 `Status`，并保留足够上下文，例如 “node handler”、“parallel node task”、“HTTP stream callback failed”。不要让异常越过 C API、thread entry 或 public async callback 边界。

## 5. 日志和观测

- 错误事件应带 run id、thread id、node id、step、checkpoint namespace 等调试上下文。
- 不要把 token、raw request body、raw tool payload、credential、secret path 直接写入日志。
- HTTP/provider 错误需要走 redaction 规则。
- metric label 不应包含高基数或敏感数据。

## 6. 测试要求

| 变更类型 | 测试要求 |
| --- | --- |
| 新 parser/codec | success、malformed input、future version、size limit。 |
| 新 storage/checkpoint path | unavailable、corruption、partial write、reopen。 |
| 新 async path | callback throws、shutdown during work、queue full、cancellation。 |
| 新 tool/provider | validation failure、authorization failure、handler failure、stream failure。 |
| 新 public API | invalid argument 和 recoverable failure 的 `Status` contract。 |

