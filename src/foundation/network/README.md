# src/foundation/network

本目录提供 provider-neutral HTTP/SSE 基础设施。它不属于 `src/langgraph` 业务层，但 model/provider adapter 可以通过接口注入使用它。

## 1. 核心职责

- `HttpClient` 实现同步、异步、streaming 和 SSE request，并通过 `HttpRequestOptions`
  支持 per-request timeout/deadline/retry override。
- `IHttpClient` 定义 provider adapter 可依赖的最小接口。
- `IAuthorizationProvider` 负责 request auth 注入。
- `IRefreshableAuthorization` 负责可刷新认证的 readiness/refresh gate。
- `SseParser` 解析 Server-Sent Events。
- transport、observation、config validation 分离，避免单文件承担所有职责。

## 2. 认证边界

当前重点是 upstream request auth，而不是 proxy auth。内置 request auth providers 包括：

- `NoAuthorizationProvider`
- `BearerTokenAuthorizationProvider`
- `ApiKeyAuthorizationProvider`
- `BasicAuthorizationProvider`
- `OAuthAuthorizationProvider`
- `FunctionAuthorizationProvider`

OAuth token 生命周期由 `OAuthAuthorizationProvider` 自己管理；不再公开独立 `OAuthTokenProvider`。

## 3. 错误和安全规则

- request validation、auth failure、queue full、close/shutdown、stream callback failure 都返回 `Status`。
- HTTP 4xx/5xx 是有效 `HttpResponse`，由调用方决定是否重试或失败。
- logs/traces 不应记录 raw secret、token、credential 或未 redacted request body。
- SSE parser 必须有 buffer/size 边界，避免 malformed stream 导致无限增长。

## 4. 关联文档

- [../../../docs/ERROR_MODEL.md](../../../docs/ERROR_MODEL.md)
- [../../../docs/QUALITY_MODEL.md](../../../docs/QUALITY_MODEL.md)
- [../../../docs/LIMITATIONS.md](../../../docs/LIMITATIONS.md)
