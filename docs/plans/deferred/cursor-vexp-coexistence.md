# Plan — Cursor harness + vexp coexistence fixes

> **Slug**: `cursor-vexp-coexistence` (matches this file's basename without `.md`).
>
> **Status**: `deferred — retired` (2026-06-23). **Not started** (no code on develop, § Implementation log empty) **and the premise is dead**: vexp was removed as the Claude Code nav tool by **#1084**, which merged *before* this plan doc was filed (#1176). The `.cursor/rules` file-vs-directory collision this plan was meant to resolve no longer arises from a vexp install Smatchet ships against. Retired rather than executed; re-file fresh if a real Cursor/vexp coexistence conflict recurs.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules (Plan location, Plan-doc safety, Plan revision after implementation, Plan stress-test, Plan template).

## Context

Two layout models collide. The vexp installer (v1.2.x) writes `.cursor/rules` as a **single file**; Smatchet's `setup-harness` cursor adapter needs `.cursor/rules/` to be a **directory** of `.mdc` rules. Today `setup_cursor()` hard-`exit 1` when `.cursor/rules` is a file ([setup-harness.sh:462](../../../agents/scripts/core/setup-harness.sh#L462)). Result: Smatchet's `agents.mdc` never installs, and the five leaf `Source/Core/src/*/AGENTS.md` files are never auto-loaded in Cursor — unlike Claude Code's gitignored `@AGENTS.md` shims from `gen_subsystem_claude_shims()` ([setup-harness.sh:199](../../../agents/scripts/core/setup-harness.sh#L199)).

After this lands: one idempotent `bash agents/scripts/core/setup-harness.sh cursor` run auto-migrates the vexp single-file into `.cursor/rules/vexp.mdc`, installs `agents.mdc`, and emits per-subsystem `subsystem-<ctx>.mdc` shims — with vexp + Smatchet search policies reconciled rather than contradictory.

> **Stale-context note (added during plan-archival sweep)**: PR #1084 (merged 2026-06-09, before this plan was filed 2026-06-13) removed vexp as a Claude Code nav tool and scrubbed vexp from the `docs/harness/cursor/` docs; vexp now survives only in `AGENTS.md` + the locally-deployed `CLAUDE.md`. The Cursor-vexp-coexistence premise should be re-validated / re-scoped before implementation begins.

```
vexp installer        --writes single file-->  .cursor/rules (file)        --blocks-->  .cursor/rules/*.mdc (dir)
setup-harness cursor  --needs directory----->  .cursor/rules/*.mdc (dir)
```

## Approach

Replace the hard `exit 1` with an idempotent `migrate_cursor_rules_file()` that moves the vexp single-file aside (temp, not destructive), creates `.cursor/rules/`, and wraps the original vexp body into `.cursor/rules/vexp.mdc` with YAML frontmatter (`alwaysApply: true`). Add `gen_subsystem_cursor_mdc()` mirroring `gen_subsystem_claude_shims()` to emit one glob-scoped `subsystem-<ctx>.mdc` per leaf `AGENTS.md`. Reconcile the two "search policy" rules (both `alwaysApply: true`) with explicit exception blocks in `agents.mdc` + the vexp wrapper, citing `AGENTS.md` § Semantic-search exceptions so grep/glob stays valid for exhaustive sweeps while `run_pipeline` is primary for navigation.

Trade-off: 2 always-on rules (`agents` + `vexp`) cost tokens on every Cursor turn vs Claude Code's lazy-load `@`-shims; accepted — Cursor has no per-file lazy-load for `alwaysApply` rules, and the 5 subsystem rules ARE glob-scoped (load only when matching files are in context). Documented vs Claude trade-off in [per-subsystem-agent-docs](../shipped/per-subsystem-agent-docs.md).

User-data safety is the gating constraint: migration only fires when vexp delimiters (`<!-- vexp --> … <!-- /vexp -->`) are detected; unrelated content → warn + bail, never destroy. User-modified `vexp.mdc` → move new file to `.reinstall.bak`, never clobber.

## Files to modify

1. [agents/scripts/core/setup-harness.sh](../../../agents/scripts/core/setup-harness.sh) — Slice 1: replace `exit 1` block (L462–467) with `migrate_cursor_rules_file()`; Slice 2: add `gen_subsystem_cursor_mdc()` (mirror `gen_subsystem_claude_shims()` @ L199); call both from `setup_cursor()` (L457).
2. `docs/harness/cursor/rules/subsystem-leaf.mdc.tmpl` — **NEW**: glob-scoped pointer template for leaf `AGENTS.md` shims.
3. [docs/harness/cursor/rules/agents.mdc](../../harness/cursor/rules/agents.mdc) — Slice 3: add 2–3-line semantic-search-exceptions block citing root `AGENTS.md`.
4. [docs/harness/cursor/setup.md](../../harness/cursor/setup.md) — Slice 4: document auto-migration + generated shims + refresh-after-pull + re-install-heals.
5. [docs/harness/SETUP.md](../../harness/SETUP.md) — Slice 4: Cursor row in per-subsystem table.
6. `tests/bats/setup_harness_cursor.bats` — **NEW** (Slice 5): headless fixtures.

Grep confirmed (2026-06-13): `migrate_cursor_rules_file`, `gen_subsystem_cursor_mdc`, `subsystem-leaf.mdc`, `rules.vexp.migrate` — **zero** existing hits; no `vexp.mdc` / `subsystem-*.mdc` tracked; no cursor/vexp branch. Nothing started.

## Existing utilities reused

- `gen_subsystem_claude_shims()` — [setup-harness.sh:199](../../../agents/scripts/core/setup-harness.sh#L199) — registry pattern (`git ls-files 'Source/Core/src/*/AGENTS.md'`) + per-leaf emit + user-modified skip; `gen_subsystem_cursor_mdc()` mirrors it exactly.
- `copy_template()` — [setup-harness.sh](../../../agents/scripts/core/setup-harness.sh) — skip-if-user-modified contract reused for `vexp.mdc` + `subsystem-*.mdc` regen guard.
- [CONTEXT-MAP.md](../../../CONTEXT-MAP.md) — same leaf-`AGENTS.md` registry the Claude shims + this generator read.

## UX Pillar callouts

Diff is shell scripts + `.mdc`/`.tmpl` templates + docs + bats — **no runtime C++ / UI code**.

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: N/A — no runtime path touched.
- **Pillar 2 (UI-thread never blocks)**: N/A — no UI thread touched.
- **Pillar 3 (never crash)**: N/A — setup-time shell; migration is non-destructive (temp-move + restore-on-failure), the only safety surface.
- **Pillar 4 (accessibility)**: N/A — no UI.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

**N/A — diff touches no `Source/Core/`** (shell scripts + templates + docs + bats only).

## Risks / non-goals

- **User data loss on migration** — mitigated: vexp-delimiter detection gates the move; unrelated content → warn + bail; temp-file (`.vexp.migrate.tmp`) restored on any failure before non-zero exit.
- **vexp re-install recreates the single-file conflict** — mitigated: next `setup-harness cursor` re-migrates; user-modified `vexp.mdc` → `.reinstall.bak` + warn, never clobber.
- **Glob path separators on Windows host** — mitigated: forward slashes (`Source/Core/src/Tracker/**`) per Cursor docs convention; verify on Windows in bats.
- **Non-goal**: widening `vexp-strip-agents-md.sh` to portable `agents/core/*.md` (separate P2 [tooling backlog](../../self-improvement/categories/tooling.md) — `vexp-strip-hook-misses-agent-core-md`).
- **Non-goal**: Cursor hook parity for lint drains / HEAD-drift guard (Claude-only; documented in [docs/harness/cursor/hooks-equivalent.md](../../harness/cursor/hooks-equivalent.md)).
- **Non-goal**: upstream vexp installer writing `.mdc` directly (external).

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps where feasible.

- **Bucket A (pure-logic ctest)**: N/A — no C++ helper added.
- **Bucket E (ImGui Test Engine)**: N/A — no UI.
- **Bash-driver scenario**: `tests/bats/setup_harness_cursor.bats` (NEW) in a temp git tree —
  1. **vexp file migration** — seed `.cursor/rules` with vexp markers → run `setup-harness.sh cursor` → assert `.cursor/rules/` is a dir, `agents.mdc` + `vexp.mdc` exist, original file gone.
  2. **idempotent re-run** — second run no-op (no dup files, exit 0).
  3. **user-modified skip** — pre-seed modified `vexp.mdc` → migration writes `.reinstall.bak`, does not overwrite.
  4. **subsystem shims** — with tracked leaf `AGENTS.md` present, assert 5 `subsystem-*.mdc` with correct `globs:` lines.
  Wire into [scripts/dev/test-all.sh](../../../scripts/dev/test-all.sh) only if the suite is < 5 s; else document as manual `bats tests/bats/setup_harness_cursor.bats`.
- **Build gate**: N/A — no C++ touched (no `cmake --build`).
- **Doc validation (blocks plan-doc PRs — keep)**: `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint).
- **Plan stress-test — `grill-with-docs` (keep)**: run before finalising; record outcome.
- **Manual residue**: two smoke checks remain manual (acceptable, named here) — (a) new Cursor Agent chat cites `AGENTS.md` delegation table when asked; (b) opening a file under `Source/Core/src/Tracker/` surfaces Tracker invariants without an explicit `@AGENTS.md` read. Cursor has no headless agent-chat driver; deferred-automation action plan → [tooling backlog](../../self-improvement/categories/tooling.md) entry `cursor-agent-chat-headless-smoke`.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray refs to anything deferred here; revise or delete.

- Widening `vexp-strip-agents-md.sh` to portable `agents/core/*.md` — follow-up P2 tooling backlog.
- Cursor hook parity (lint drains / HEAD-drift guard) — no-action; Claude-only, documented in `hooks-equivalent.md`.
- Upstream vexp installer emitting `.mdc` directly — no-action; external/user issue.

## Implementation order

1. Template + migration helper (Slice 1 + Slice 3 body text)
2. `gen_subsystem_cursor_mdc()` (Slice 2)
3. Docs (Slice 4)
4. Bats fixtures (Slice 5)

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*
