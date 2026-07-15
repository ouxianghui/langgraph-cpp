# Skill: Subgraphs And Checkpoint Namespaces

Use this when changing `addSubgraph`, subgraph execution, checkpoint namespaces, parent/child event
forwarding, or namespace string construction.

Pair with [`persistence.md`](persistence.md), [`stream-projection.md`](stream-projection.md),
[`docs/PERSISTENCE_MODEL.md`](../../docs/PERSISTENCE_MODEL.md), and API_CONTRACT namespace rules.

## Role

Compiled subgraphs run as parent nodes. Subgraph state diffs merge into the parent; checkpoints may
use independent namespaces and persistence modes (per-invocation, per-thread, or stateless).

## Namespace Rules (must)

- Hierarchical subgraph levels: `|`
- Per-invocation task/run segments: `:`
- User node names must **not** contain `|` or `:`
- Stream metadata may expose both `checkpoint_ns` string and path form
- Namespaces must not cross-contaminate history queries

## Rules

- Parent forwards subgraph events with namespace / parent run id / trace path metadata.
- `Command::gotoParentNode(s)` only targets declared parent destinations.
- Persistence mode choice is explicit in `SubgraphOptions`; do not silently inherit unsafe defaults.
- Replay/resume on parent and child namespaces keep their own latest pointers.

## Coding Checklist

- [ ] Namespace builders use official separators only.
- [ ] Tests cover isolation between sibling namespaces.
- [ ] Stream projection filters for subgraphs stay consistent with envelope contract.
- [ ] LIMITATIONS / compat docs updated if parent/child semantics change.

## Verification

```sh
ctest --test-dir build/unix-debug -R "subgraph|compat|checkpoint" --output-on-failure
build/unix-debug/examples/subgraph_module
```
