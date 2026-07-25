<!-- index-summary: gate CLI_GUIDE.md against the live command registry so a new command can't ship undocumented -->
# Plan — CLI_GUIDE ↔ command-registry parity gate

> **Slug**: `cli-guide-registry-parity-gate` (matches this file's basename without `.md`).
>
> **Status**: `active`

## Context

The Unified Command System is the repo's headline automation surface: one `MakeCommand({...})` registration feeds four frontends (shell CLI, Command Palette, MCP `tools/call`, Lua `commands.invoke()`), and [`CLI_GUIDE.md`](../../../CLI_GUIDE.md) is its user-facing catalogue — explicitly pitched in `README.md` as "Suitable for shell scripts and AI agents."

That catalogue has silently drifted. A sweep on 2026-07-24 found **12 of 57** registered commands absent from `CLI_GUIDE.md`:

`debug.crash`, `debug.dock.dump`, `debug.dock.reset`, `debug.lua_log_test`, `debug.window.resize`, `debug.window.screenshot`, `grid.clear_selection`, `ui.open_view`, `ui.zoom.in`, `ui.zoom.out`, `ui.zoom.reset`, `ui_test.run`

Six are `debug.*` and arguably intentional. But `ui.zoom.*`, `grid.clear_selection`, and `ui.open_view` are ordinary user-facing surface, and an AI agent reading `CLI_GUIDE.md` as its tool catalogue simply cannot discover them.

The drift is structural, not an oversight: the repo auto-generates and CI-gates the shipped-plan index (`test-plan-index.sh` fails on drift), gates subsystem leaf docs, doc anchors, and markdown links — but `grep -rn CLI_GUIDE .github/workflows/ agents/scripts/ scripts/` returns **nothing**. Nothing has ever checked this file against the registry.

**Intended outcome**: after this lands, a **literal `MakeCommand("<name>"` registration** that never reaches `CLI_GUIDE.md` fails CI, and the 12-command backlog is either documented or explicitly classified as internal. The guarantee is deliberately scoped to *statically discoverable* registrations — commands registered from data (`view.toggle.*`, `pane.*`, the scenario family) are **not** covered and can still drift undocumented; see § Risks for why, and § Out of scope for the follow-up that would close it. Do not read this gate as "CI catches every undocumented command."

## Approach

Mirror the proven `test-plan-index.sh` shape rather than inventing a mechanism: a script that derives the truth from source, diffs it against the doc, fails on drift, and offers `--fix`. That gate is already trusted, already wired into `doc-validation.yml`, and contributors already know its idiom.

The truth source is a **static scan of `MakeCommand("<name>"` across `Source/Core/src/Commands/`**, not a live `commands.list` call. A live call would be more faithful — it would pick up dynamically-registered names (`view.toggle.*`, `pane.*`) and respect `SMATCHET_WITH_*` feature gating — but it requires a running instance with MCP enabled, which no doc gate can assume and which would make the check unrunnable in the pure-docs CI lane where it belongs. Static scanning is the pragmatic seam; § Risks records what it consequently cannot see.

Commands legitimately absent from the user guide get an **explicit allow-list with a reason per entry**, not a silent skip. `debug.*` being internal is a decision that should be written down once and enforced, rather than re-litigated every time someone notices the gap.

## Files to modify

1. `agents/scripts/core/test-cli-guide-parity.sh` (NEW) — the gate. Modes mirroring `test-plan-index.sh`.

   **`--check` exit contract (the two directions are NOT symmetric):**
   - *Registered-but-undocumented* → **exit 1** (fatal). This is the drift the gate exists to stop.
   - *Documented-but-unregistered* → **WARN, exit 0**. Never fatal, because a dynamically-registered name (`view.toggle.*`, `pane.*`) is legitimately documented yet invisible to the static scan, so failing here would punish correct docs.
   - Both are always *reported*; only the first changes the exit code. The `--selftest` must assert **both** paths explicitly — a planted fake registration exits 1, a planted doc-only row exits 0 with a WARN line — so the asymmetry can't silently invert later.

   **`--fix` insertion contract (deterministic, fails closed):**
   - Category is the command name's prefix before the first `.` (`ui.zoom.in` → `ui`; `grid.clear_selection` → `grid`).
   - That category must map to a known `### <category>` heading in `CLI_GUIDE.md` § Command catalogue via an explicit table in the script. An **unknown category is a hard error (exit 2), never an invented section** — `--fix` must not silently manufacture document structure.
   - Creating a genuinely new category (`ui`, `grid` — neither exists today) is a **deliberate one-time human edit** in step 7, which also adds the § Contents anchor. After that the mapping table knows them and `--fix` can append rows.
   - Rows append to the end of the category's existing table, preserving column count.

   `--selftest` (plant a fake registration, assert it is flagged — required by `test-gate-selftests.sh`) additionally covers the unknown-category exit-2 path.
2. `agents/scripts/core/cli_guide_parity.py` (NEW) — the extractor + differ. Scan `Source/Core/src/Commands/**/*.cpp` for `MakeCommand("<name>"`; parse `CLI_GUIDE.md`'s catalogue tables for `` | `<name>` `` cells; report **both** directions (registered-but-undocumented **and** documented-but-unregistered — the second catches a command deleted from code while its doc row rots).
3. `agents/scripts/core/cli-guide-internal-allowlist.txt` (NEW) — one `<name>  # <reason>` per line for commands intentionally absent from the user guide. Seeded with the six `debug.*` entries.
4. `agents/scripts/core/test-cli-guide-parity-bats.sh` + `tests/bats/cli_guide_parity.bats` (NEW) — `test-orphan-bats.sh` requires every bats suite to carry a `test-*.sh` wrapper.
5. `.github/workflows/doc-validation.yml` (MOD) — register the gate alongside the other doc checks. Advisory-first for one cycle, then blocking (see § Risks).
6. `scripts/dev/test-docs.sh` (MOD) — add to the canonical local mirror so `§ Verification`'s "run the doc suite" instruction actually covers it. Note the open `test.md` backlog item that this mirror is already stale vs `doc-validation.yml`; this plan must not widen that gap.
7. `CLI_GUIDE.md` (MOD) — document the six genuinely user-facing commands: `ui.zoom.in` / `ui.zoom.out` / `ui.zoom.reset` (§ ui — new subsection; the doc has no `ui` category today), `ui.open_view`, `grid.clear_selection` (§ tickets or a new § grid), `ui_test.run` (decide: user-facing or allow-listed — it drives the bucket-E harness, so likely allow-listed with a pointer to the testing docs).
8. `CLI_GUIDE.md` § Contents (MOD) — add any new category anchors so `test-doc-anchors.sh` stays green.

## Existing utilities reused

- `test-plan-index.sh` — `agents/scripts/core/test-plan-index.sh` — the `--check` / `--fix` / drift-report structure and its auto-generated-block marker convention (`<!-- BEGIN auto-… -->`); copy the shape, do not re-derive it.
- `MakeCommand` — `Source/Core/src/Commands/Builtin/BuiltinCommands_Internal.h` (via `builtin_detail::MakeCommand`) — the single registration seam the extractor keys on; confirm no second registration spelling exists before relying on it.
- `test-gate-selftests.sh` — `agents/scripts/core/test-gate-selftests.sh` — enumerates `--selftest`-exposing scripts and asserts each has a failure case; the new gate must satisfy it.
- `md_lint.py` — `agents/scripts/core/md_lint.py` — the `--fix` output must survive it (table formatting).

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — adds a gate and doc rows; extracts nothing.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — CI tooling plus markdown. No `Source/Core/` runtime change.
- **Pillar 2 (UI-thread never blocks)**: no impact.
- **Pillar 3 (never crash)**: no impact.
- **Pillar 4 (accessibility)**: mild positive — `ui.zoom.*` is a font-scaling surface (Pillar 4's own concern) that is currently undiscoverable from the CLI catalogue. Documenting it makes an accessibility affordance findable.

## Perf-review-system gates

N/A — the diff touches no `Source/Core/` production code. The only `Source/` interaction is **read-only** scanning by the gate script.

## Risks / non-goals

- **Risk — static scanning misses dynamically-registered commands.** `view.toggle.<id>`, `pane.*`, and the scenario commands are registered from data, not a literal `MakeCommand("name"`. *Mitigation*: the extractor reports only on names it can see; the reverse direction (documented-but-unregistered) is **warn-only**, never fail, precisely because a dynamically-registered name would otherwise look like a stale doc row. Document this asymmetry in the script header.
- **Risk — feature-gated commands look undocumented on a Light build.** `ai.*` is absent from `commands.list` when `SMATCHET_WITH_AI=OFF` ([ADR-0010](../../adr/0010-light-profile-feature-gated-command-registry.md)), but the source literal is always present, so the static scan sees it regardless. This is actually the **desired** direction — the user guide should document the full surface — but the script header must say so, or someone will "fix" it into per-profile scanning.
- **Risk — the allow-list becomes a dumping ground.** A `debug.*` prefix rule would be terser but would let any future `debug.`-prefixed user-facing command hide. *Mitigation*: entry-per-line with a mandatory reason column; the gate rejects an allow-list line without a `#` comment.
- **Risk — flipping to blocking strands an in-flight PR.** *Mitigation*: advisory for one merge cycle, land the step-7 doc rows first, flip to blocking in a separate one-line PR once the gate reads zero.
- **Non-goal — auto-generating the whole catalogue from the registry.** Tempting, but `CLI_GUIDE.md`'s value is its hand-written params/notes/examples per command; generating it would flatten that to a name list. The gate checks *presence*, not content.
- **Non-goal — validating params/flags against each `Command`'s schema.** A richer check (does the doc's param list match the registered schema?) is a plausible follow-up but multiplies the parsing surface; presence-only is the 80/20.
- **Non-goal — fixing `scripts/dev/test-docs.sh`'s pre-existing staleness.** Tracked separately in `docs/self-improvement/categories/test.md`; this plan only avoids making it worse.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no C++ change.
- **Bucket E (ImGui Test Engine)**: N/A — no UI change.
- **Bash-driver scenario / screenshot / sanitizer**: N/A.
- **Build gate**: N/A — no compiled source touched. (Stated explicitly rather than omitted, per the template's forcing function.)
- **Gate self-verification**: `bash agents/scripts/core/test-cli-guide-parity.sh --selftest` must fail on a planted registration; `--check` must read zero after step 7; `test-gate-selftests.sh --check` must count the new script; `test-orphan-bats.sh --check` must find the bats wrapper.
- **Round-trip**: run `--fix` on a deliberately-reverted `CLI_GUIDE.md`, confirm the regenerated rows pass `md_lint.py --all` and `test-doc-anchors.sh`. Additionally assert the **fail-closed** path: a planted registration in an unknown category (e.g. `zzz.something`) makes `--fix` exit 2 without touching the file, rather than inventing a `### zzz` section.
- **Exit-path coverage**: assert both `--check` directions explicitly — planted registered-but-undocumented → exit 1; planted documented-but-unregistered → exit 0 with a WARN line. Pinning both stops a later refactor from quietly making the reverse direction fatal.
- **Doc validation (blocks plan-doc PRs)**: the canonical `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs`**: **not yet run** — owed before implementation starts.
- **Manual residue**: none. The one judgement call (is `ui_test.run` user-facing or internal?) is resolved at step 7, not deferred.

## Out of scope (flagged, not designed)

**Deferral residue-sweep**: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here.

- **Param/schema-level doc validation** — presence-only here; follow-up plan if drift shows up in params rather than names.
- **The same parity question for `LUA_GUIDE.md` and `MCP_GUIDE.md`** — both surface the same registry through different frontends and could drift identically. Not designed here; the gate is built so a second doc target is a config addition rather than a rewrite.
- **`MCP_GUIDE.md` tool catalogue** — MCP exposes the registry via `tools/list`, already registry-driven in code (BACKLOG N9), so doc drift is lower-risk. No action.

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
