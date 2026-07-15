# Skill: Provider Models And HTTP Client

Use this when changing `ProviderChatModel`, `IHttpClient`, `HttpRequestOptions`, authorization
providers, SSE/streaming HTTP, or fake transports used in tests.

Pair with [`foundation.md`](foundation.md), [`security.md`](security.md),
[`docs/API_CONTRACT.md`](../../docs/API_CONTRACT.md) (v22–v25 themes), and
[`docs/SECURITY_MODEL.md`](../../docs/SECURITY_MODEL.md).

## Role

Cloud/provider access is **application-injected**. The graph runtime stays provider-neutral; HTTP and
auth are foundation ports used by optional model adapters.

## Contracts To Preserve

| Piece | Rule |
| --- | --- |
| `IHttpClient` entrypoints | Always take explicit `HttpRequestOptions` (timeout/deadline/retry override) |
| `IAuthorizationProvider` | Only `authorize(HttpRequest&)`; no leak of refresh into this interface |
| `IRefreshableAuthorization` | Readiness / refresh gate for expiring credentials |
| `OAuthAuthorizationProvider` | Owns OAuth credential lifecycle |
| Token usage | Normalize to `TokenUsage` / `UsageMetadata`; keep provider raw under `raw` |
| Defaults | Tests use fake HTTP; no real network in default gates |

## Rules

- Inject `IHttpClient` and auth providers; do not discover credentials from the environment inside
  graph runtime core paths.
- Redact auth headers, tokens, and raw bodies from logs/metrics/events.
- Retry/backoff must respect request options and avoid retry storms (`PERFORMANCE_MODEL`).
- Optional llama.cpp is **not** this skill — see CMake `LANGGRAPH_CPP_WITH_LLAMA_CPP` and model adapter docs.
- Changing auth/request option signatures is an API contract bump ([`../AUTHORITY.md`](../AUTHORITY.md)).

## Coding Checklist

- [ ] No real network in unit/component tests (fake transport).
- [ ] Closed clients fail with clear `Status`.
- [ ] Stream/SSE callback failures convert to `Status` with context.
- [ ] Provider profiles stay behind injected client + options.

## Verification

```sh
ctest --test-dir build/unix-debug -R "http|provider|redaction|secret" --output-on-failure
```
