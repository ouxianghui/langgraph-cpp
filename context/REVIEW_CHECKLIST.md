# Context Pack Review Checklist

Use when reviewing PRs that touch `context/`, `docs/API_CONTRACT.md`, `docs/ARCHITECTURE.md`,
`docs/DEPENDENCY_POLICY.md`, `docs/OWNERSHIP.md`, `PROJECT_MANIFEST.json`, or agent entrypoints.

## Completeness

- [ ] All files in [`SKILL_INVENTORY.md`](SKILL_INVENTORY.md) exist.
- [ ] Root `AGENTS.md` and `CLAUDE.md` still load `.agent/` **and** `context/`.
- [ ] New topics have a skill *or* an explicit “covered by X.md” note in inventory.

## Authority And Versions

- [ ] `context/AUTHORITY.md` `AUTHORITY_PINS` match `PROJECT_MANIFEST.json` → `contracts`.
- [ ] API / schema bumps update contract docs + pins in the same PR.
- [ ] Skills do not invent version numbers not present in pins/manifest.

## Docs Closed Loop

- [ ] Layer claims about `lgc::core` match `docs/ARCHITECTURE.md` and `docs/DEPENDENCY_POLICY.md`.
- [ ] Ownership of `src/core` / `context/` matches `docs/OWNERSHIP.md`.
- [ ] `scripts/check-dependency-policy.sh` and `scripts/check-context-skills.sh` both pass.

## Skill Quality

- [ ] Skill points to authoritative `docs/` / ADR / examples; no orphan advice.
- [ ] Distillation does not claim Planned roadmap items as shipped (`LIMITATIONS.md` check).
- [ ] Cross-links among skills are one level deep and resolve.

## Gates To Run

```sh
scripts/check-dependency-policy.sh
scripts/check-context-skills.sh
```

Optional when narrative docs changed:

```sh
ctest --test-dir build/unix-debug -L docs --output-on-failure
```
