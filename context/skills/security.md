# Skill: Security, Tools, And Observability Boundaries

Use this when changing tools, HTTP auth, logging/metrics/tracing, redaction, provider adapters,
edge/hardware adapters, or any path that may handle secrets or external side effects.

Pair with [`../../docs/SECURITY_MODEL.md`](../../docs/SECURITY_MODEL.md) and
[`.agent/skills/security-logging.md`](../../.agent/skills/security-logging.md).

## Default Bounds

- The graph runtime executes only explicitly registered node handlers and tools.
- No built-in privileged shell, filesystem, network, or hardware tools by default.
- Credentials, endpoints, and device access are application-configured — not auto-discovered by the
  graph runtime.
- Optional llama.cpp / SQLite / provider / hardware paths stay optional.

## Tool Rules (ADR-0010)

| Requirement | Practice |
| --- | --- |
| Explicit registration | `ToolRegistry` |
| Input validation | JSON Schema via `ToolExecutor` |
| Optional output validation | `ToolNodeOptions::validateOutput_` |
| Authorization | `ToolPolicy::authorize_` before invoke |
| Failure shape | Structured tool error; no secret leakage into logs |
| Dangerous ops | Prefer interrupt/HITL approval |

## HTTP Auth

- `IAuthorizationProvider::authorize(HttpRequest&)` injects request auth.
- Refreshable credentials use `IRefreshableAuthorization` readiness/refresh gate.
- Prefer built-in providers (bearer, API key, basic, OAuth, function) or inject custom ones.
- All `IHttpClient` entrypoints take explicit `HttpRequestOptions` (timeouts/deadline/retry).

## Logging / Metrics / Traces

Never log or label with:

- passwords, API keys, cookies, bearer tokens;
- authorization headers;
- raw request/response bodies;
- raw tool payloads;
- private user content;
- unprocessed provider error payloads;
- high-cardinality secret-like IDs as metric labels.

Prefer: status codes, retry counts, bounded error categories, run/thread/node/step ids, checkpoint
namespace. Use redaction wrappers on sinks when data may be sensitive. Debug mode must not silently
disable production redaction defaults.

## External Side Effects

Checkpoint restore recovers **graph** state, not the external world. Apps owning tools/hardware/
providers should define idempotency, retry, approval, compensation, audit, and device permission
boundaries.

## Coding Checklist

- [ ] No new default-on privileged tool.
- [ ] Tool inputs validated before callable runs.
- [ ] Auth/secret materials not copied into events/logs/metrics.
- [ ] Edge adapters remain interfaces + explicit registration; examples stay mock-friendly by default.
- [ ] Security-relevant behavior changes update SECURITY_MODEL / tests (`redaction`, `http_client`,
      tool/agent, edge adapter tests).

## Verification Focus

```sh
ctest --test-dir build/unix-debug -R "redaction|secret|http|tool|edge" --output-on-failure
```
