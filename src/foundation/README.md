# src/foundation 模块索引

`src/foundation` 是可复用基础设施层。它为 `src/langgraph` 提供 status/result、storage、serialization、HTTP/SSE、logging、metrics/tracing、events、async primitives、executors、scheduler、filesystem、crypto、redaction、resource limits 等能力。

## 1. 依赖规则

- `src/foundation` 不能依赖 `src/langgraph`。
- foundation 类型应保持通用，不引入 graph/node/checkpoint 业务概念，除非位于明确的 serializer/adapter 边界。
- async 组件必须定义 owner、shutdown、queue/backpressure 和 callback 线程。
- 可能包含 secret 的日志、metric、trace 或 HTTP 字段必须遵守 redaction 规则。

## 2. 子目录职责

| 目录 | 职责 |
| --- | --- |
| `status` | `Status` 和 `Result<T>`。 |
| `async` | channel/future 等异步原语。 |
| `executor` / `threading` | executor、thread、owner guard、thread pool。 |
| `storage` | `IStorage`、memory storage、SQLite storage。 |
| `serialization` | state/checkpoint codec、content envelope。 |
| `network` | HTTP client、SSE parser、request auth providers。 |
| `observability` / `event` | runtime events、metrics、trace sink。 |
| `redaction` | logger/event/trace/codec redaction wrappers。 |
| `crypto` | encryption、secure checkpoint codec、secure storage。 |
| `filesystem` / `blob` / `cache` | 文件、blob、cache 基础设施。 |
| `scheduler` / `process` / `cancellation` | scheduler、进程执行、取消信号。 |

## 3. 质量规则

- 可恢复错误返回 `Status` / `Result<T>`。
- public async API 需要明确 close/shutdown 行为。
- callback 不应在持锁状态下被调用。
- queue 必须有容量、背压、失败或 shutdown 策略。
- parser/codec 需要覆盖 malformed input、size limits 和 future version。

## 4. 关联文档

- [../../docs/QUALITY_MODEL.md](../../docs/QUALITY_MODEL.md)
- [../../docs/CONCURRENCY_MODEL.md](../../docs/CONCURRENCY_MODEL.md)
- [../../docs/ERROR_MODEL.md](../../docs/ERROR_MODEL.md)

