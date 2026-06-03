# Plan — Self-improvement one-entry-per-file (kill concurrent-PR conflicts)

> **Slug**: `self-improvement-one-entry-per-file` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.
>
> **STATUS: DEFERRED (pending recurrence) — 2026-06-03.** Judged over-scoped for the
> current pain. The **frequent** conflict (the hand-maintained count line, touched
> by every entry-adding PR) was killed cheaply instead — the stored count column
> was removed from `AGENT_SELF_IMPROVEMENT.md` § Index; counts are on-demand via
> `test-backlog-counts.sh --list`; the gate now guards against re-introducing it.
> The conflict this plan targets — two concurrent adds to the *same category file*
> — is **rarer** (P3, observed-only, ~5 min manual resolve) and does not justify a
> 135-entry migration + a 121-file producer sweep yet. **Implement only if that
> category-content conflict starts recurring often.** The § Approach below is the
> ready-to-go design for that day.

## Context

Concurrent agentic sessions merging into `develop` conflict on the shared
self-improvement files. Two distinct sources, only one of which is solved:

1. **Category-file content conflicts (UNSOLVED, the dominant source).** The live
   category files (`docs/self-improvement/categories/{bug,process,tooling,infra,
   test,security,external-blockers}.md`) are single files with many entries,
   newest-first. Two concurrent PRs that each **prepend** an entry edit the same
   top-of-file lines → git conflict; two that **delete** different entries also
   conflict. `merge=union` is **deliberately not** applied here (`.gitattributes:49`)
   because union wrongly preserves both deleted blocks on a parallel-delete. So
   every concurrent add/remove needs manual resolution. ~135 live entries across
   the 7 files today.
2. **The `AGENT_SELF_IMPROVEMENT.md` count-line (a derived symptom).** Its § Index
   table carries a hand-updated "Live count" per category. Parallel branches
   modify the same count line → conflict; union would garble it (`.gitattributes:53`).
   A gate already exists — `agents/scripts/core/test-backlog-counts.sh --fix`
   regenerates the table from `grep -c '^- 20' <file>` — but the count is still
   *stored + hand-edited*, so it conflicts (and a branch-time `--fix` reproduces
   the `INDEX.md` concurrent-archive drift seen 2026-06-03).

Already solved + **out of scope**: `applied.md` (the archive) uses `merge=union`
+ `sort-applied-md.sh` and converges fine — its prepend-only, never-delete shape
is exactly what union handles. Leave it as-is.

The fix the existing backlog (`process.md` 2026-06-03 P3; `process-rules.md`
§ Backlog-archive union merge) already names but never scoped: **one entry per
file**. Two concurrent PRs that each add a *different* file share no edited line —
zero conflict, structurally. The count then derives from a directory listing.

**Intended outcome — one sentence:** after this lands, each live self-improvement
entry is its own file under `docs/self-improvement/categories/<cat>/`, the index
count is generated (not hand-stored), and two concurrent PRs adding backlog
entries **cannot** conflict because they touch disjoint files.

## Approach

Per-file is the structural fix; the generated index sits on top. Phased to bound
the 121-file reference blast radius and de-risk the migration.

**Phase 1 — Layout + migration (the mechanical core).**
- New layout: `docs/self-improvement/categories/<cat>/<YYYY-MM-DD>-<slug>.md`, one
  entry per file. The `<date>-` prefix preserves the newest-first convention as a
  reverse filename sort — no in-file ordering to conflict on. Each file holds
  exactly the existing entry body (date · agent · [cat] · Pn — title + Details +
  Concrete next action + Status + Last-reviewed).
- A one-time migration script (`agents/scripts/core/migrate-self-improvement.sh`,
  delete-after-run) splits each live `categories/<cat>.md` into per-entry files,
  deriving each slug from the title. Deterministic; idempotent; prints a
  before/after entry count for parity.
- `applied.md` stays a single union-merged file (out of scope; already solved).
- `external-blockers.md` (1 entry) migrates too for uniformity.

**Phase 2 — Generated index, never hand-edited (kills symptom #2).**
- Rework `test-backlog-counts.sh` to count **files in `<cat>/`** (`ls | wc -l`),
  not `grep -c` lines. The § Index table in `AGENT_SELF_IMPROVEMENT.md` becomes
  **generated-or-verified**, never hand-updated in a feature PR.
- **Decision (grill this):** stored-but-verify-only vs not-stored. Preferred —
  **drop the stored "Live count" column entirely**; replace with a one-line
  "run `bash agents/scripts/core/test-backlog-counts.sh --list` for live counts".
  Nothing shared is hand-edited → zero conflict on the index too. The triage-cadence
  trigger ("category > ~20 open") is computed on demand by the same script. (If a
  stored count is wanted for at-a-glance, make it **verify-only at PR time +
  regenerated by a post-merge develop hook** — never branch-time `--fix`, which
  re-creates the drift.)

**Phase 3 — Update the producers (the 121-file reference surface).**
- The entry-authoring flow changes from *"append to `categories/<cat>.md`"* to
  *"create `categories/<cat>/<date>-<slug>.md`"*. Update: `AGENTS.md`
  § Self-improvement loop, `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`
  § Format / § Workflow, every `agents/*.md` agent prompt's `## Self-improvement`
  guidance, `docs/agent-rules/process-rules.md`, and the `gate-escape-postmortem`
  + `drain-memory` skills that file category entries. Mechanical, `mechanic`-shaped,
  but wide — do it as a focused sweep with a grep checklist, not by memory.
- The apply/archive flow (entry → `applied.md`) is unchanged in shape: a per-file
  entry is `git mv`'d / appended into the union-merged `applied.md` then the source
  file deleted — still conflict-free (the live file is per-entry; applied stays union).

**Non-obvious trade-off, named:** per-file trades a tidy single-file scan (one
`process.md` to read) for a directory of many small files. Mitigated by the
generated index + a `--list` reader; and the read-cost is dwarfed by the
conflict-resolution cost it removes (~5 min/slice, hit on 8 of 9 slices in the
`tooling-process-backlog-sweep` retrospective).

## Files to modify

**Phase 1:**
1. `docs/self-improvement/categories/<cat>/` (new dirs ×7) + the migrated per-entry `.md` files (~135).
2. `agents/scripts/core/migrate-self-improvement.sh` (new, run-once, delete after) — split each `categories/<cat>.md` → per-entry files; parity-count check.
3. Delete the 7 monolithic live `categories/<cat>.md` after migration (NOT `applied.md`).
4. `.gitattributes` — no `merge=union` needed for the per-file dirs (disjoint files don't conflict); keep the `applied.md` union line.

**Phase 2:**
5. `agents/scripts/core/test-backlog-counts.sh` (edit) — count files per `<cat>/` dir; `--list` reader; `--fix` / verify mode for whatever index form Phase-2 decision picks.
6. `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` § Index (edit) — drop the stored count column (or make it verify-only per the grilled decision); point at `--list`.

**Phase 3:**
7. `AGENTS.md` § Self-improvement loop (edit) — create-a-file flow.
8. `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` § Format / § Workflow (edit).
9. `docs/agent-rules/process-rules.md` (edit) — § Backlog-archive union merge note: scope it to `applied.md` only + document the per-file live layout.
10. Every `agents/*.md` with a `## Self-improvement` instruction + the `gate-escape-postmortem` / `drain-memory` skills (edit, sweep) — file-per-entry guidance. Grep checklist: `git grep -l "self-improvement/categories"`.

## Existing utilities reused

- `agents/scripts/core/test-backlog-counts.sh` — the count gate, reworked from grep-lines to file-count.
- `docs/self-improvement/categories/applied.md` + `.gitattributes` `merge=union` + `sort-applied-md.sh` — the already-solved archive path; untouched.
- The plan-doc one-file-per-doc + the `~/.claude/.../memory/` one-file-per-memory layouts — the precedent this mirrors (disjoint files = no concurrent-edit conflict).
- `git grep -l "self-improvement/categories"` — the authoritative reference inventory for the Phase-3 sweep (no from-memory enumeration).

## UX Pillar callouts

- **Pillar 1–4**: no runtime impact — docs reorganisation + a shell gate + agent-prompt prose. Zero product code.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

N/A — no `Source/Core/` code, no C++. The diff is `*.md` under `docs/` + `agents/scripts/**` shell — both pure-docs-allowlisted by `is-pure-docs-diff.sh` (so it classifies pure-docs and build/ctest/perf gates skip). Verification is shell-lint + the count-gate selftest + the migration parity count.

## Risks / non-goals

**Risks:**
- **Reference sweep misses a producer** (an agent prompt still says "append to `categories/<cat>.md`"). → Phase-3 uses `git grep -l` as the exhaustive inventory, not memory; a `test-no-monolith-category-ref.sh`-style guard can assert no live ref to a deleted monolith.
- **Branch-time index `--fix` reproduces the drift** this plan exists to kill. → Phase-2 decision must be verify-only-at-PR + regenerate-post-merge, or no-stored-count. Explicitly NOT branch-time `--fix`.
- **Migration mis-splits an entry** (multi-paragraph Details with blank lines). → the migration script splits on the `^- 20YY-MM-DD` entry-header boundary, not blank lines; parity count (before vs after) gates the run; spot-check a multi-paragraph entry.
- **In-flight backlog edits during the migration PR** conflict with the migration itself. → land the migration as one atomic PR during a quiet window; the migration is the one unavoidable big-shared-edit (after it, conflicts stop).
- **Triage-cadence trigger** ("category > 20 open") loses its at-a-glance count. → the `--list` reader prints per-category counts; the cadence rule points at it.

**Non-goals:**
- Refactoring `applied.md` to per-file — already conflict-free via union; not worth 181 file-moves.
- A cross-session coordination protocol (the other half of the `process.md` P3 entry — CI-cancellation churn / reconcile livelock) — separate concern; this plan only kills the *file-conflict* half.
- Auto-applying backlog entries — unchanged; the apply-loop + threshold stay.

## Verification

- **Migration parity**: the migration script asserts `sum(per-file entries) == grep -c '^- 20' <old monolith>` per category; fails on mismatch.
- **Conflict-free proof**: two synthetic branches each adding a *different* `categories/<cat>/<slug>.md`, merged in sequence, produce **zero** conflict (the acceptance test for the whole plan).
- **Index gate**: `test-backlog-counts.sh` (reworked) verifies/regenerates correctly against the per-file dirs; `--list` prints per-category counts.
- **No-monolith-ref guard**: `git grep "self-improvement/categories/<cat>\.md"` (excluding `applied.md`) returns zero after the Phase-3 sweep.
- **Shell-lint**: the migration + count scripts pass `test-shell-lint.sh` (5 rules).
- **Build gate**: N/A — pure-docs.
- **Manual residue**: none expected; the migration is scripted + parity-gated.

## Implementation log
*(populated post-ship — bullet per phase)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
