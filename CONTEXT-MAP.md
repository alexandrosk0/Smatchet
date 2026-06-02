<!-- index-summary: registry of per-subsystem agent docs (rules / glossary / orientation) under Source/Core/src/<ctx>/ -->
# CONTEXT-MAP — per-subsystem agent-doc registry

Heavy `Source/Core/src/<ctx>/` subsystems carry up to **three** co-located agent docs, each with its own format contract:

| Artifact | Holds | Format contract | Loaded how |
|---|---|---|---|
| `AGENTS.md` | imperative scoped rules / invariants | [agents.md spec](https://agents.md/) | harness auto-loads (nearest-wins where supported; see below) |
| `CONTEXT.md` | domain glossary (one-sentence term defs, relationships, zero impl detail) | [`grill-with-docs/CONTEXT-FORMAT.md`](agents/_shared/skills/grill-with-docs/CONTEXT-FORMAT.md) | read on demand |
| `README.md` | orientation (request flow, role-of-each-file; durable-by-construction — no `file:line`, no counts) | this registry's orientation shape | read on demand |

This file is the **registry** (what exists where) and the **harness-discovery index** (for any harness that doesn't honour nearest-wins). The gate `agents/scripts/project/test-subsystem-docs.sh` keys off the table below: a listed leaf missing on disk **FAILs**; an on-disk `Source/Core/src/<ctx>/AGENTS.md` absent from this table **FAILs**; a rule string duplicated central↔leaf **FAILs**; a context whose code changed without a `README.md` touch **WARNs**.

## Registry

| Subsystem | Rules | Glossary | Orientation |
|---|---|---|---|
| Tracker | `Source/Core/src/Tracker/AGENTS.md` | `Source/Core/src/Tracker/CONTEXT.md` | `Source/Core/src/Tracker/README.md` |
| Commands | `Source/Core/src/Commands/AGENTS.md` | — | — |
| Persistence | `Source/Core/src/Persistence/AGENTS.md` | — | — |
| Sync | `Source/Core/src/Sync/AGENTS.md` | — | — |
| Ui | `Source/Core/src/Ui/AGENTS.md` | — | — |

**System-wide glossary** (cross-cutting, not subsystem-scoped): [`docs/CONTEXT.md`](docs/CONTEXT.md). Term overlap with `Tracker/CONTEXT.md` (e.g. `TrackerIssueKey`, `UpdateField` set-replace) is intentional — `docs/CONTEXT.md` is the living index hub; the leaf is the subsystem-local view.

A `—` cell means **intentionally absent** (no scoped rule / no grill yet), not missing. `Tracker/` is the full exemplar; the other four carry rules only (glossary + orientation deferred to per-subsystem follow-up grills).

## Harness discovery

Leaf `AGENTS.md` loading is **per-harness** (see [`docs/harness/SETUP.md`](docs/harness/SETUP.md) § Per-subsystem leaf discovery):

- **Claude Code** reads nested `CLAUDE.md`, not nested `AGENTS.md`. `setup-harness.sh claude-code` generates a gitignored one-line `CLAUDE.md` (`@AGENTS.md`) beside each leaf so Claude Code lazy-loads the leaf when a file in that dir is touched. The committed tree stays `AGENTS.md`-only.
- **Codex / agents.md harnesses** read the nearest `AGENTS.md` natively — no shim needed.
- **Cursor / Aider / generic** — no nearest-wins; use this registry + explicit reads.

## Growth path

Add a leaf when **content exists**, not to fill the grid:

1. A subsystem gains a real scoped invariant → add `Source/Core/src/<ctx>/AGENTS.md` + a row here. Leaf creation is driven by content, **not** by the lint strict-zone list (a strict-zone dir need not have a leaf; a leaf need not be strict-zone).
2. A subsystem earns a domain grill → add `CONTEXT.md` (+ `README.md`) and fill its cells.
3. The gate enforces parity automatically once the row is here.
