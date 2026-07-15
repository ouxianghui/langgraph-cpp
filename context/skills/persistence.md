# Skill: Persistence (Checkpoint, Store, Storage)

Use this when changing checkpoint savers, pending writes, namespaces, resume/replay, stores,
`IStorage`, or checkpoint/content codecs.

Pair with [`../../docs/PERSISTENCE_MODEL.md`](../../docs/PERSISTENCE_MODEL.md),
[`foundation.md`](foundation.md), and [`langgraph.md`](langgraph.md).

## Do Not Confuse

| | Checkpoint | Store |
| --- | --- | --- |
| Purpose | Execution history for resume/history/replay | Long-term application memory |
| Scope | logical `threadId` + checkpoint namespace | namespace + key |
| Writer | graph runtime / saver | node, tool, or application |
| APIs | `put`, `putWrites`, `get`/`getTuple`, `list`, maintenance | `batch` / `put` / `get` / `search` / `deleteItem` / `listNamespaces` |

## Checkpoint Write Boundaries

Runtime writes checkpoints at:

- initial boundary;
- normal super-step completion;
- interrupt pause;
- failure with recoverable pending writes;
- completion;
- `updateState` forks;
- task-level writes under `Durability::Sync`.

Not guaranteed at arbitrary user side-effect points inside handlers.

## Resume / Replay Semantics

| API | Intent |
| --- | --- |
| `resume` | Continue from latest checkpoint for thread (+ namespace) |
| `replay` | Continue from historical checkpoint; new records append after current latest |
| `updateState` | External update → new checkpoint (time-travel fork) |
| `getState` / `getStateHistory` | Inspect snapshots / history |

## Pending Writes

On partial failure in a parallel super-step:

1. Successful task writes become pending writes (not yet final merged state).
2. Failure checkpoint records tasks to redo.
3. Resume reruns only failed/incomplete tasks.
4. On super-step completion, pending + new writes merge deterministically.

Do not “fix” resume by pretending partial success already committed.

## Namespace Rules

- Subgraph levels: `|`
- Per-invocation task/run segment: `:`
- Different namespaces must not poison each other’s history queries.
- User node names must not contain `|` or `:`.

## Storage / Codec Rules

- Storage and codec failures return explicit `Status`.
- Future schema versions without migration → `StatusCode::Unimplemented`.
- Schema bumps require contract docs, compat tests, and LIMITATIONS updates.
- Encryption/serializer plugs go through saver/storage options (e.g. codec on storage saver).

## Coding Checklist

- [ ] Checkpoint vs store APIs not mixed conceptually in comments or new helpers.
- [ ] Namespace string construction matches `|` / `:` rules.
- [ ] Pending-writes path covered by test if behavior changes.
- [ ] Crash/reopen / corruption / busy paths considered for storage-backed changes.
- [ ] Public schema or durability semantics documented in API_CONTRACT / PERSISTENCE_MODEL.

## Verification Focus

```sh
ctest --test-dir build/unix-debug -R "checkpoint|store|crash|storage" --output-on-failure
build/unix-debug/examples/sqlite_checkpoint_resume
```

## ADRs

- 0002 checkpoint boundary
- 0007 store/checkpoint separation
