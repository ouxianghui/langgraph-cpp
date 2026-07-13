# Skill: Security and Logging

Use this when changing logs, metrics, external request handling, error text, identifiers, debug
settings, telemetry, or sensitive data handling in a C++ project.

Read project context first, as defined in `../README.md`.

## Never Log Unless Explicitly Allowed

- credentials, passwords, secrets, API keys, cookies, or bearer-token shapes;
- authorization headers;
- raw request or response bodies;
- user-generated private content;
- display names or other personal data;
- raw account, user, participant, session, tenant, token, payload, or private external-resource
  identifiers.

Do not add request/response dumps, even behind debug level, unless the project explicitly defines a
safe sanitized dump format.

## Data Classification and Allowlist

- Classify each new field as secret, private, internal, or public before adding it to logs, metrics, or
  traces.
- Default to deny: only fields explicitly allowed by project policy should be emitted.
- Hash, redact, or sanitize allowed sensitive fields before emission, and keep the safe form explicit
  in the field name or contract when the project requires it.

## Identifier Fields

- Hash or redact sensitive identifiers with the project-approved helper.
- Use the field naming convention defined by `CONVENTIONS.md`.
- Keep readable sanitized fields such as `reason` or `errorText` when needed for debugging.
- Do not invent new sensitive-field naming schemes in one call site.

## Sanitization Helpers

- Sanitizers are a backstop for free text and string fields.
- They are not permission to log raw secrets first.
- Prefer structured fields where sensitive values are hashed/redacted before logging.
- If new secret shapes appear, update sanitizer tests and project log-safety gates.

## External Payload and Error Text Boundaries

- Treat external SDK/API errors, exception text, status details, headers, and payload snippets as
  untrusted until classified.
- Sanitize or map external error text before it enters logs, metrics, traces, or user-visible
  diagnostics.
- Never place raw external text into metric labels or stable schema fields.

## Structured Events

- Use project-approved structured logging helpers.
- Do not add free-text logs where structured events are required.
- Context belongs in fields. Set correlation once at seam boundaries when the project has request/job
  context propagation.
- Emit seam events from wrappers such as transport endpoints, external clients, background workers,
  and state transition boundaries.
- Do not scatter duplicate logs through inner logic when one seam event describes the operation.

## Schema Compatibility and Cardinality

- Treat log event names, field names, metric names, and metric labels as operational schema.
- Prefer additive changes. Deletions, renames, or semantic changes need a migration or compatibility
  note.
- Give every metric label a clear cardinality bound before adding it.

## Metrics

- Metrics are operational schema.
- Add or change metrics through the project catalog/registry when one exists.
- Keep labels bounded and low-cardinality.
- Never use IDs, URLs, tokens, bodies, payloads, display names, or raw error strings as labels.
- Update metric code, rendering/export, tests, and docs together.

## Debug Settings

- Local debug may weaken redaction only when the project explicitly allows it.
- Do not change production defaults to make local debugging easier.
- Do not add a new debug bypass for sensitive logging without explicit approval.
- CLI flags and env vars can be visible to host tools; avoid putting secrets in examples unless the
  runtime contract requires it.

## Volume, Sampling, and Abuse Resistance

- High-frequency errors, retries, and attacker-controlled inputs must not create unbounded logs,
  metrics, or trace fields.
- Use rate limits, sampling, deduplication, or bounded summaries when volume can spike.
- Preserve the key failure signal even when reducing volume.

## Verification

- Add or update logging tests by parsing structured output.
- Add sanitizer tests for new redaction behavior.
- Run project-defined log-safety and metric-catalog gates from `STACK.md`.
- Confirm final logs still help debugging without exposing private data.
