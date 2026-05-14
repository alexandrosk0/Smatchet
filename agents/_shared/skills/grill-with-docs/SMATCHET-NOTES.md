# Smatchet — grill-with-docs integration notes

`SKILL.md`, `ADR-FORMAT.md`, and `CONTEXT-FORMAT.md` in this directory are pulled verbatim from
[mattpocock/skills · main · skills/engineering/grill-with-docs](https://github.com/mattpocock/skills/tree/main/skills/engineering/grill-with-docs).
Do not edit those three. To refresh from upstream:

```bash
for f in SKILL.md ADR-FORMAT.md CONTEXT-FORMAT.md; do
  curl -fsSL "https://raw.githubusercontent.com/mattpocock/skills/main/skills/engineering/grill-with-docs/$f" \
    -o "agents/_shared/skills/grill-with-docs/$f"
done
bash scripts/sync-agents.sh
```

## Smatchet-specific file mapping

The upstream skill talks about three artifact locations. Smatchet uses:

| Artifact | Upstream default | Smatchet location |
|---|---|---|
| Glossary | `CONTEXT.md` at repo root | `docs/CONTEXT.md` (single-context repo — no `CONTEXT-MAP.md` needed) |
| ADRs (architecture decision records) | `docs/adr/0001-slug.md` | `docs/adr/0001-slug.md` (matches upstream) |
| Feature plans | (not in upstream) | `docs/design/<slug>.md` — Smatchet convention, separate from ADRs |

**Plans vs ADRs — keep them separate.** A plan (`docs/design/<slug>.md`) is the full multi-step design for a feature; it gets revised post-implementation per AGENTS.md § Plan revision after implementation. An ADR (`docs/adr/000N-slug.md`) records **one architectural decision** with a real trade-off — single paragraph, no revision lifecycle. Plans may **reference** ADRs but don't replace them.

**Glossary scope.** `docs/CONTEXT.md` is the Smatchet domain glossary — terms specific to this product (Smatchet, Tracker, View, Field, Catalog, Scenario, Pipeline, Audit, Backend, …). It is **not** a place for general programming concepts (timeout, mutex, async, etc.) per the upstream rule.

## When to invoke this skill

- User hands you a feature plan and you spot terminology drift or unresolved trade-offs → grill before delegating.
- New term keeps showing up in conversation that isn't in `docs/CONTEXT.md` → grill once, write the term, move on.
- Architectural choice surfaces during a refactor (e.g. "should the new field flow through the catalog or skip it?") → grill to surface alternatives + record ADR if the three criteria fit (hard-to-reverse + surprising + real trade-off).

## When NOT to invoke

- Single-symbol rename → `mechanic`, no grilling needed.
- Mechanical bug fix → `debug-detective`, no grilling needed.
- "Add a field to the grid" with no design tension → `grid-engine` or `tracker-backend` directly.

The skill is for **plan stress-testing**, not for every change.

## Integration with the architect agent

`agents/architect.md` produces design docs at `docs/design/<slug>.md`. The grill-with-docs skill is a **complement**, not a replacement:

1. Architect drafts the plan.
2. Orchestrator (or user) invokes grill-with-docs to interrogate the plan term-by-term.
3. Grilling produces (a) refinements written back into `docs/design/<slug>.md`, (b) `docs/CONTEXT.md` updates when terms get pinned, (c) `docs/adr/000N-slug.md` when the three ADR criteria fire.
4. Plan ships; revision sections (`## Implementation log`, `## Deviations from plan`, `## Verification`) get appended per the standard rule.

ADRs are write-once. Plans are revised. Glossary entries are stable but additive.
