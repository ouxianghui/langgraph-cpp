# Skill: Project Coding Standards

Use this when writing or reviewing C++ in `src/foundation`, `src/core`, `src/langgraph`, public
headers, tests, or examples for this repository.

Pair with [`.agent/skills/modern-cpp.md`](../../.agent/skills/modern-cpp.md) and
[`.agent/skills/memory-safety.md`](../../.agent/skills/memory-safety.md). Project facts live in
[`../AUTHORITY.md`](../AUTHORITY.md), [`../CONVENTIONS.md`](../CONVENTIONS.md), and
[`../STACK.md`](../STACK.md).

## Language Baseline

- C++23, CMake; extensions off.
- Public namespace `lgc`.
- Prefer standard library + established project types over new framework layers.
- Match local file style; no unrelated formatting churn.

## Headers

- Public consumers should prefer `#include <langgraph_cpp/langgraph.hpp>`.
- Aggregate header must not include internal `.hh` or raw `third_party` paths.
- Headers are self-contained; include what you use; do not rely on lucky transitive includes.
- Keep non-trivial logic in `.cpp` unless template / necessary inline.
- Do not expand public headers only to make tests easier.

## Error Handling

- Recoverable paths return `Status` or `Result<T>`.
- Check `.isOk()` / propagate `status()`; never discard Status to “make it compile”.
- Convert exceptions to Status only at approved boundaries (node/tool/provider/async/HTTP/codec).
- Do not introduce bare-bool APIs for storage, schema, checkpoint, or transport failure modes.

## Ownership And Types

- Prefer values for simple data; `unique_ptr` for exclusive ownership; `shared_ptr` only for true
  shared lifetime (compiled graphs, injected services).
- Non-owning views (`string_view`, `span`, references) must not outlive their owner, especially
  across executor / callback / channel hops.
- Constructed objects should meet invariants without a mandatory separate `init()` when avoidable.
- Trailing-underscore member fields are the common project style (`threadId_`).

## Public API Changes

1. For the frozen graph-runtime contract, confirm the type is reachable from
   `include/langgraph_cpp/langgraph.hpp` ([`docs/API_CONTRACT.md`](../../docs/API_CONTRACT.md)).
   `lgc::core` types (`RuntimeContainer`, …) are installed from `src/core` and are **not** included by
   that aggregate header today — do not treat them as part of the numbered API contract surface unless
   docs/API_CONTRACT.md is updated to say so.
2. Prefer additive changes inside the current API contract version for the langgraph.hpp surface.
3. Breaking changes to that surface: bump version in `docs/API_CONTRACT.md` + `PROJECT_MANIFEST.json`,
   **delete** old names (no long dual-track aliases), update tests, examples, LIMITATIONS, traceability.
4. Persisted schema changes are separate high-risk work: bump schema versions and add migration /
   compatibility tests; unread future versions → `Unimplemented`.

## Layer Discipline

| Editing | Allowed direction |
| --- | --- |
| foundation | may use third_party / options; never langgraph or core |
| core (`lgc::core`) | may use foundation; never langgraph |
| langgraph | may use foundation + ports; do not require `lgc::core` |
| examples | may wire concrete adapters and optional `lgc::core` |
| tests | prefer fakes/mocks; no real cloud/hardware/llama.cpp in default gates |

Run `scripts/check-dependency-policy.sh` after layering / public-header / optional-dep changes.
Run `scripts/check-context-skills.sh` after `context/` or authority-pin changes.

## Naming

- Graph builder: LangGraph-style camelCase public methods only.
- Avoid resurrecting removed aliases (`CompiledGraph`, old checkpointer names, …).
- Prefer domain vocabulary already in headers (`BaseCheckpointSaver`, `BaseStore`, `BaseChatModel`).
- Avoid vague `Manager` / `Helper` / `Util` names unless the local module already defines them.

## Tests And Examples As Spec

- Behavior changes need tests near the change; high-value paths also need example or golden updates.
- Docs snippets / EXAMPLE_MATRIX must stay honest about what runs by default.
- Do not edit `third_party/` or treat `build/` as source.

## Stop And Escalate

Ask before proceeding if the change would:

- reverse foundation → langgraph dependency;
- change checkpoint resume / pending-writes / namespace semantics without tests;
- add a default-on heavy optional dependency;
- weaken redaction or introduce built-in privileged tools;
- claim LangGraph feature parity not listed in LIMITATIONS / compatibility docs.
