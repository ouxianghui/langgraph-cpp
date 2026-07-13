# 安全模型

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-10 |
| 范围 | 默认权限、tool/hardware 边界、HTTP auth、secret/redaction、日志与观测安全 |
| 关联文档 | [ERROR_MODEL.md](ERROR_MODEL.md)、[LIMITATIONS.md](LIMITATIONS.md)、[RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) |

`langgraph-cpp` 的安全目标是保守默认值和清晰扩展边界。runtime 提供工具、HTTP、provider、hardware adapter 的接口，但不默认授予 shell、filesystem、network 或 hardware 权限。

## 1. 默认安全边界

- core runtime 只执行应用显式注册的 node handlers 和 tools。
- 默认不内置 shell、文件、网络或硬件工具。
- hardware adapter 只提供接口和示例形态；真实设备访问由应用显式构造和注册。
- provider credentials、HTTP auth、model endpoint 由应用配置，不由 graph runtime 隐式发现。
- 可选 llama.cpp、SQLite、provider/hardware adapters 均不应成为默认 core 依赖。

## 2. Tool 安全规则

| 规则 | 说明 |
| --- | --- |
| 显式注册 | 工具必须通过 `ToolRegistry` 注册。 |
| 输入校验 | 工具应声明 JSON Schema，并由 `ToolExecutor` 校验。 |
| 输出校验 | 可通过 `ToolNodeOptions::validateOutput_` 启用。 |
| 授权 hook | `ToolPolicy::authorize_` 可在调用前拒绝危险操作。 |
| structured error | 工具失败应返回结构化 error，不应把原始 secret 或 payload 放入日志。 |
| HITL | 危险操作可通过 interrupt/resume 做 human-in-the-loop。 |

## 3. HTTP 与认证

当前重点是 upstream request auth，而不是 proxy auth。

| 组件 | 责任 |
| --- | --- |
| `IAuthorizationProvider` | 修改 `HttpRequest`，注入 request auth。 |
| `IRefreshableAuthorization` | 表达可刷新认证的 readiness/refresh gate。 |
| `OAuthAuthorizationProvider` | 管理 OAuth credentials、access token、refresh token、expiry、renew hook。 |
| `HttpClient` | 执行 request/stream/SSE，返回 `Status` / `HttpResponse`。 |

内置 request auth providers：no auth、bearer token、API key、basic、OAuth、function provider。

## 4. Secret 与日志

默认禁止记录：

- credentials、password、API key、cookie、bearer token；
- authorization headers；
- raw request/response bodies；
- raw tool payload；
- 私有用户内容；
- 未处理的外部 provider error payload；
- 高基数 secret-like IDs 作为 metric labels。

允许记录的内容应是结构化、低敏、可调试字段，例如 status code、retry count、bounded error category、run id、step、node id、checkpoint namespace。敏感字段需要 redact、hash 或 sanitize。

## 5. Observability 安全

- runtime event 应包含调试上下文，但不应泄露 secret。
- metrics label 必须低基数，不能使用 URL、token、raw error、body、user content。
- trace sink 和 event sink 需要可被 redaction wrapper 包裹。
- debug mode 不能默认弱化生产 redaction。

## 6. 外部副作用

runtime checkpoint 恢复图状态，不恢复外部世界。真实 tool/hardware/provider side effects 需要应用提供：

- idempotency key；
- retry policy；
- approval policy；
- compensation 或 rollback 策略；
- audit/logging 策略；
- device permission boundary。

## 7. 测试与门禁

| 风险 | 证据 |
| --- | --- |
| secret/redaction | `redaction_test.cpp`、`secrets_test.cpp` |
| HTTP auth/transport | `http_client_test.cpp` |
| tool validation/policy | tool and agent loop tests |
| hardware adapter boundary | `edge_adapter_test.cpp`、mock edge examples |
| release 安全清单 | [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) |

