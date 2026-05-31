# Plan — split scripts: build/dev stay, agentic move under `agents/scripts/{core,project}/`

> **Slug**: `split-scripts-build-vs-agentic`.

## Context

Two problems, one move.

1. **CI over-builds on agentic-script-only PRs.** The `changes` job in `.github/workflows/build-and-test.yml` runs the 45-min Windows MSVC builds unless every changed file matches a docs-class allow-list (`*.md`, `docs/*`, **`agents/*`**, a few dotfiles). All of flat `scripts/` is code-class, so a PR touching only an agentic script (e.g. `scripts/dev/memory-drain-nudge.sh`, a SessionStart nudge that never compiles anything) triggers two full builds for nothing (observed on PR #557).
2. **The agentic layer is being prepared for extraction into a separate repo** (per `docs/PORTABILITY.md` § Extraction checklist). Today the agentic scripts are intermixed with C++ build/dev scripts in one flat `scripts/dev/` dir, so "what ships to the agents repo" is not structurally visible.

Both are solved by relocating the agentic scripts under the existing portable agentic root `agents/`, mirroring the agent split (`agents/core/` portable vs `agents/project/` subsystem-bound). Because the CI allow-list already contains `agents/*` **and** the bash `case` glob matches nested paths (`agents/scripts/core/x.sh` matches `agents/*`), the move **needs no workflow edit** — relocation alone makes every agentic script docs-class.

**Intended outcome**: after this lands, (a) agentic-script-only PRs skip the MSVC builds (required contexts still report `skipped == success`), and (b) `agents/` is a self-contained agentic tree — agents + their scripts — ready to lift into a standalone repo, while the C++ **build remains self-sufficient** (every script the build/CI-build-job invokes stays in `scripts/dev/`).

## Approach

Four-way classification, mirroring `docs/PORTABILITY.md` § Agent split rule:

- **`scripts/dev/` (STAYS) — build / dev / run-the-exe / CI-invoked.** Any script that compiles, launches, or tests the C++ product, OR is invoked by a CI build/test job, OR is sourced by such a script. The code repo must build with **no** dependency on the `agents/` tree, so build-invoked utilities stay here even when otherwise "portable" (notably `project-config.sh`, sourced by `with-msvc-env.sh`).
- **`scripts/dev/local/` (MOVE within `scripts/dev/`) — human-run, CI-irrelevant.** PowerShell convenience wrappers + local build/lint/package tools + `manual-*` scripts that **no** workflow or action references (verified: 0 CI refs). Changing one cannot alter any CI outcome, so it should not trigger the MSVC build. Unlike `agents/*` (already allow-listed), this needs **one** added line in the `changes` job. Still part of the code repo (not agentic) — just CI-noise.
- **`agents/scripts/core/` (MOVE) — portable agentic.** Generic ship-line / merge-gate / lock / harness-wiring / plan-doc / agent-meta tooling + the docs/agent-meta test scripts. Mirrors `agents/core/`. Reads project values from `project.config.json`; never compiles or runs the product.
- **`agents/scripts/project/` (MOVE) — project-bound agentic.** Scripts coupled to *this* project's subsystems/values: `p4-*` (this project's optional VCS layer wiring), `test-lint-rules*` (this project's strict-zone globs), `test-config-migration.sh`. Mirrors `agents/project/`.

**Boundary rule (deterministic):** runs/builds/tests the exe ⇒ `scripts/dev/`. Else agentic ⇒ `core/` if it would work unchanged in another project (values from config), `project/` if its identity is bound to a this-project subsystem/zone. Test scripts split the same way: `test-ui-*`, `test-*-roundtrip`, `test-grid-edit-perf-*`, `perf-*`, `test-build-*` STAY (they launch the exe); `test-doc-anchors`, `test-plan-*`, `test-agent-*`, `test-skill-*`, `test-shell-lint`, `test-merge-*`, `test-lock-*`, `test-markdown-links`, `test-workflow-yaml`, `test-backlog-counts`, `test-portable-purity` → `core/`; `test-lint-rules*`, `test-p4-dual-vcs` → `project/`.

**The test orchestrator (`test-all.sh`) is a special case — handle it BEFORE any test-`*` move.** `test-all.sh` discovers its suite by a single-directory glob: `find scripts/dev -maxdepth 1 -name 'test-*.sh'` (verified at `scripts/dev/test-all.sh:46`). It aggregates *both* the product tests that STAY (`test-ui-*`) and the ~40 agentic `test-*` that MOVE. If we move the agentic test scripts first, this glob silently matches **fewer files and still exits green** — the moved tests just stop running. The §5 `git grep` stale-path sweep **cannot catch this** (no stale string exists — the glob simply finds less), and a naive "`test-all.sh` is green" check would pass *because the tests vanished*. Classification + required edit:
> - **`test-all.sh` STAYS in `scripts/dev/`** (it's the pre-push aggregator and `tests the exe` via the product tests; it is referenced by `.github/pull_request_template.md` and a `build-and-test.yml` comment, not run as a CI build job). It is **edited first** to discover across all three roots — `scripts/dev/`, `agents/scripts/core/`, `agents/scripts/project/` — each root included **only if it exists** (`[ -d ]` guard), so the code repo still runs standalone when the `agents/` tree is later extracted, and the agents repo can ship its own runner without a dangling `scripts/dev` reference. The test-time reach into `agents/` is the one accepted code↔agents coupling (the **build** stays fully self-sufficient; only the optional pre-push test aggregation spans both trees).
> - **A count invariant guards the silent-skip**: capture the discovered-test count before the move and assert `test-all.sh` discovers **≥ that count** after (see § Verification). A drop = a move that escaped the multi-root glob.

**Trade-off named:** the move relocates ~99 scripts (≈75 → `agents/scripts/core/`, ~10 → `agents/scripts/project/`, ~14 → `scripts/dev/local/`) and rewrites every hardcoded reference to them. The risk is a missed reference breaking a hook or the running merge-watcher daemon. Mitigated by 2-phase execution (Phase 1 ≈ the ~70 non-daemon/non-hook relocations, including all `scripts/dev/local/` moves; Phase 2 ≈ the ~15 daemon/hook-critical `core/` scripts — see § Risks) + an exhaustive grep-sweep gate (below). The CI benefit needs *no* `agents/*` allow-list change — it already covers the agentic moves; only `scripts/dev/local/*` is added — which also shrinks the blast radius vs. a `scripts/agent/` target.

**Count reconciliation (source of truth is the boundary rule, not the tallies).** The tree actually holds **139** in-scope scripts: **136** under `scripts/dev/*.{sh,py,ps1}` + **3** top-level (`scripts/setup-harness.sh`, `scripts/clear-session-context.sh`, `scripts/agent-tokens-report.py`). Budget: ~25 STAY + ~75 `core/` + ~10 `project/` + ~14 `local/` ≈ 124. The ~15 residual is rounding plus non-`{sh,py,ps1}` companions that move **with their owner** (e.g. `merge-gates.graphql` follows `merge-gates.sh`, `*.bats` fixtures follow their `test-*-bats.sh`) and the `archived/` subdir (explicitly out of scope). **Execution gate**: the move-set is generated by applying the boundary rule to the *full* `git ls-files scripts/` list, and the script must assert every in-scope path lands in exactly one bucket (no file unclassified) — the tallies above are estimates, the partition is mandatory.

## Files to modify

**1. New dirs + the one allow-list edit**
1. `agents/scripts/core/` + `agents/scripts/project/` + `scripts/dev/local/` — new directories (move targets).
2. `.github/workflows/build-and-test.yml` `changes` job `case` allow-list — add `scripts/dev/local/*` (the only allow-list edit; `agents/*` already covers the agentic moves).

**2. STAY in `scripts/dev/` (build / CI-invoked / build-sourced — ~25):** `test-all.sh` (the pre-push test aggregator — STAYS and is edited *first* to multi-root discover; see § Approach "test orchestrator"), `with-msvc-env.sh`, `project-config.sh` (build dependency — sourced by `with-msvc-env.sh`, so it stays despite `docs/PORTABILITY.md` tagging it portable; PORTABILITY's generic-scripts row is corrected as part of §5), `doctor.sh`, `check-required-tools.sh`, `relaunch-smatchet.sh`, `coverage*.sh`, `coverage-delta-gate.sh`, `perf-*.{sh,py,json}`, `perf-marker-inventory.sh`, `test-build-*`, `test-doctor.sh`, `test-cppcheck-path-detection.sh`, `test-ui-*.sh`, `test-*-roundtrip.sh`, `test-grid-edit-perf-*.sh`, `test-callstack-tooltip-hover.sh`, `test-tooltip-wrapwidth.sh`, `test-markdown-lang-tag.sh`, `test-theme-*.sh`, `test-whisper-*.sh`, `test-ai-prefs-validator.sh`, `test-lua-error-log.sh`. **Not here** — the PowerShell build/run wrappers (`build_*.ps1`, `build_and_*.ps1`, `package_unreal_plugin_msvc.ps1`, `rebuild_testproject_plugin.ps1`, `attach_unreal_vsjit.ps1`, `run_standalone.ps1`, `build-msvc-asan.ps1`), `run_clang_tidy.ps1`, `run_cppcheck.py`, and `manual-*` are human-run / CI-irrelevant → they move to `scripts/dev/local/` (§4b), not stay.

**3. MOVE → `agents/scripts/core/` (portable agentic — ~75):** `merge-gates.{sh,graphql}`, `merge-gates-prompt.sh`, `merge-watcher.py`, `merge-watcher-cli.py`, `merge-watcher-*.ps1`, `watch-register-if-enabled.sh`, `smatchet-notify*`, `agent-progress.sh`, `git-janitor.sh`, `coderabbit-triage.py`, `verify-cr-reply.sh`, `check-main-repo-clean.sh`, `is-pure-docs-diff.sh`, `lock-*.sh`, `locks-*.sh`, `setup-locks-ruleset.sh`, `_lock-json.py`, `vexp-strip-agents-md.sh`, `memory-drain-nudge.sh`, `sort-applied-md.sh`, `tail-agent.sh`, `rewrite-plan-paths.sh`, `plan-doc-table-probe.sh`, `test-shell-lint.sh`, `test-doc-anchors.sh`, `test_doc_anchors.py`, `test-agent-contract.sh`, `test-agent-discovery-fixture.sh`, `test-plan-index.sh`, `test-plan-naming.sh`, `test-plan-ref-integrity.sh`, `test-markdown-links.sh`, `test-portable-purity.sh`, `test-skill-load-log.sh`, `test-skill-vs-agent-parity.sh`, `test-backlog-counts.sh`, `test-merge-gates.sh`, `test-merge-watcher-bats.sh`, `test-merge-watcher-integration-bats.sh`, `test-lock-primitives*.sh`, `test-workflow-yaml.sh`, `test-setup-harness.sh`, `test-lint-bash.sh`, `test-lint-hook-split.sh`, `test-pre-push-merged-pr-guard.sh`. **Top-level `scripts/`:** `clear-session-context.sh`, `agent-tokens-report.py`, `setup-harness.sh` → `agents/scripts/core/`.

**4. MOVE → `agents/scripts/project/` (project-bound agentic — ~10):** `p4-git-sync-check.sh`, `p4-reconcile-check.sh`, `p4-task-stream*.sh`, `test-p4-dual-vcs.sh`, `test-lint-rules.sh`, `test-lint-rules-bats.sh`, `test-config-migration.sh`.

**4b. MOVE → `scripts/dev/local/` (human-run, 0 CI refs — ~14):** `build_and_run.ps1`, `build_and_run_ninja_debug.ps1`, `build_and_run_vs_debug.ps1`, `build_and_run_vs_release.ps1`, `build_standalone.ps1`, `build_deploy_and_open_unreal.ps1`, `build_and_deploy_unreal_plugin.ps1`, `run_standalone.ps1`, `attach_unreal_vsjit.ps1`, `rebuild_testproject_plugin.ps1`, `build-msvc-asan.ps1`, `package_unreal_plugin_msvc.ps1`, `run_clang_tidy.ps1`, `run_cppcheck.py`, `manual-*`. (Each verified 0 references under `.github/`.)
> **STAY→MOVE dependency to repath (do not miss):** `run_cppcheck.py` moves here, but the **STAYING** `scripts/dev/test-cppcheck-path-detection.sh` copies it by hardcoded path (`cp "$REPO_ROOT/scripts/dev/run_cppcheck.py" …` at lines 53 & 83) and `scripts/dev/doctor.sh:202` names it in a warning string. These are STAY-bucket files pointing *into* the `local/` bucket — the inverse of the usual "references to moved scripts" direction, so they are called out explicitly here in addition to the §5 grep sweep. Both must be repathed to `scripts/dev/local/run_cppcheck.py`. (Same check applies to `run_clang_tidy.ps1`'s own usage-example paths and any `doctor.sh` mention.)

**5. Reference sweep (every moved path):**
- `.github/workflows/*.yml` (esp. `test-all.sh` callers, doc-validation, cr-finding-gate, pillar2-scan, locks-render, lock-*), `.github/actions/*/action.yml`.
- `.claude/hooks/*` + `.claude/settings.json` (**live, gitignored — needs explicit user authz per self-mod guard**) + `docs/harness/claude-code/settings.json.tmpl` + `docs/harness/claude-code/hooks/*`.
- `AGENTS.md`, `docs/agent-rules/*.md`, `docs/**` (esp. `docs/PORTABILITY.md` § generic/project script rows + § External path contracts table; `docs/STRUCTURE.md` if it lists script paths; **`BUILD.md`** + any guide that documents the `build_*.ps1` / `run_*.ps1` invocation paths → new `scripts/dev/local/` prefix), `agents/*.md`, `README.md`.
- Cross-script `source` / invocation lines (e.g. `test-all.sh` runs most agentic test-*; `merge-watcher.py` calls `merge-gates.sh`; `git-janitor` calls `is-pure-docs-diff.sh`; `with-msvc-env`/build sources `project-config.sh` — stays, no change).
- `project.config.json` (any `scripts/dev/...` path values).
- `scripts/dev/setup-harness.sh` self-reference + its `link_*` targets (it moves to core; the SessionStart auto-sync block in `clear-session-context.sh` repaths too).
- **Out of scope:** `.claude/streams/watch-button/` (sibling agent's separate checkout — regenerated, not edited here); `.understand-anything/` (generated knowledge-graph/fingerprint artifacts — regenerated by tooling, never hand-edited, even though they index every script path).
- **Scope of the path-string sweep — operational, not historical.** The §Verification "zero stale hits" gate targets paths that are *executed or are current instructions* (CI YAML, `.github/actions`, hooks, `BUILD.md` / current READMEs, runtime hint strings in `Source/`, cross-script `source`/invocation, `project.config.json`). It deliberately does **not** rewrite historical narrative in `docs/plans/shipped/*` (records of what a past PR did — rewriting them falsifies history) nor sibling **active** plans whose own subject is replacing these scripts (`kill-powershell-minimize-toolchain.md`, `msvc-build-onboarding-hardening.md`, `source-root-consolidation.md`) — those repath when *they* execute. Surfacing this explicitly so the gate isn't read as "every grep hit must be zero."

**5b. Internal self-anchoring depth (the subtle one — a moved script can break itself).** A script that computes the repo root by *relative parent traversal from its own location* breaks when its nesting depth changes, with **no external reference to flag it**. `scripts/dev/X` is depth-2; `scripts/dev/local/X`, `agents/scripts/core/X`, `agents/scripts/project/X` are all depth-3 — **one level deeper** — so every such anchor needs **+1** traversal step. Confirmed instances (Phase 1a, `local/` bucket): `run_cppcheck.py` `Path(__file__).resolve().parents[2]` → `parents[3]`; the PowerShell wrappers' `$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)` → one more `Split-Path -Parent` (in `build_standalone.ps1`, `run_standalone.ps1`, `run_clang_tidy.ps1`, `package_unreal_plugin_msvc.ps1`, `build_and_deploy_unreal_plugin.ps1`, `rebuild_testproject_plugin.ps1`, `test-build-wrapper.ps1`); and `build_standalone.ps1`'s dot-source `..\common\SmatchetCMakeCommon.ps1` → `..\..\common\...`. **Same-dir sibling joins are safe** when the sibling moves too (`Join-Path $PSScriptRoot "build_standalone.ps1"` still resolves because both landed in `local/`). The `core/` and `project/` moves (Phase 1b/2) must run the identical depth-audit: grep each moved script for `parents[`, `Split-Path -Parent`, and `dirname …/..` self-root patterns and adjust. The hermetic shadow-tree test (`test-cppcheck-path-detection.sh`) must mirror the new depth so the relocated script's `parents[N]` still lands on the scratch root.

**6. Classifier sync (resolved — count agentic-script diffs as pure-docs):** extend `is-pure-docs-diff.sh`'s `allow` regex (now in `agents/scripts/core/`) to include `agents/scripts/`, so agentic-script-only diffs count as "pure docs" for the git-janitor FF-batch exception. Rationale: consistent with the CI `agents/*` docs-class allow-list, and agentic scripts never build or run the product. This regex edit is part of the §5 reference-sweep edit set.

## Existing utilities reused

- `git mv` — per-file history-preserving move; the move-set is scripted from the boundary rule, not hand-listed at execution.
- `agents/*` CI allow-list (`.github/workflows/build-and-test.yml` `changes` job) + bash `case` nested-glob match — the existing mechanism the move piggybacks on; **no edit needed**.
- `docs/PORTABILITY.md` § Classification + § Agent split rule + § External path contracts — the canonical core/project boundary this plan extends from agents to agent-scripts.
- CI Pattern C (`docs/agent-rules/ci-required-check-pattern.md`) — skipped-required-check == success; unchanged.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — repo-tooling reorg, no `Source/Core/` touch.
- **Pillar 2 (UI-thread)**: no impact.
- **Pillar 3 (never crash)**: no product impact; risk is to CI/hook/daemon tooling, covered by Verification + phasing.
- **Pillar 4 (accessibility)**: N/A.

## Perf-review-system gates

N/A — diff touches no `Source/Core/` code (pure tooling/docs move).

## Risks / non-goals

- **Missed reference breaks a hook or the merge-watcher daemon.** Daemon (`SmatchetMergeWatcher` task → `merge-watcher.py` + `merge-gates.sh` loop) and SessionStart/PostToolUse hooks call moved scripts by hardcoded path. Mitigation: **Phase 2** stops the daemon, moves the daemon/hook-critical set (`merge-*`, `watch-register`, `clear-session-context`, `vexp-strip`, `memory-drain-nudge`, `agent-progress`, `setup-harness`, `agent-tokens-report`), repaths `.github/actions/` + `.claude/hooks/` + `settings.json(.tmpl)`, restarts daemon + session. **Phase 1** (the ~70 non-daemon/non-hook scripts) carries no live-process risk.
- **Live `.claude/settings.json` edit hits the self-mod guard.** Accepted — surfaced for explicit user authz at Phase 2; committed `.tmpl` covers fresh clones.
- **Build coupling to `agents/` would break extraction.** Avoided by the STAY rule: every build/CI-build-invoked script (incl. `project-config.sh`) remains in `scripts/dev/`. The code repo builds with the `agents/` tree absent.
- **Boundary fuzziness.** Mitigated by the deterministic rule; later reclassification is a one-line `git mv` (no allow-list change, since both `scripts/dev/`-stay and `agents/`-move sides are already correctly classed by CI).
- **Non-goals:** changing what any script *does*; renaming scripts; touching `scripts/dev/archived/`; actually creating the separate agents repo (this only *prepares* the tree).

## Verification

- **Bucket A / E**: N/A — no C++ change.
- **Bash-driver**: (1) **Test-count invariant (silent-skip guard):** record `find scripts/dev -maxdepth 1 -name 'test-*.sh' | wc -l` **before** any move (baseline N); after the multi-root edit + all moves, `bash scripts/dev/test-all.sh` reports a discovered-test count **≥ N** and is green — a discovered-count *drop* fails the gate (a `test-*` that escaped the multi-root glob). (2) `bash agents/scripts/core/test-setup-harness.sh` green (harness wiring repaths). (3) `git grep -nE "scripts/(dev/)?(<moved-name>)"` returns **zero** stale hits for every moved script — the sweep gate; run it for the inverse direction too (STAY scripts pointing into `local/`, e.g. `test-cppcheck-path-detection.sh` → `run_cppcheck.py`). (4) Probe PR touching only an `agents/scripts/` file → `Detect code changes` outputs `code=false`, MSVC jobs `skipped`. (4b) Probe PR touching only a `scripts/dev/local/` file → same `code=false` / `skipped`. (5) Probe PR touching a `scripts/dev/` build script (e.g. `with-msvc-env.sh`) → MSVC jobs **run**.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — confirms no build-invocation path broke; also confirms the build needs nothing from `agents/scripts/`.
- **Daemon smoke (Phase 2)**: `SmatchetMergeWatcher` restarts; `merge-watcher-cli.py list` + one poll cycle on a registered PR succeeds from the new path.
- **Manual residue**: the live `.claude/settings.json` edit (Phase 2) requires user authz — tracked in the phase checklist, not silent.

## Out of scope (flagged, not designed)

- Creating the standalone agents repo / git-subtree split — this plan only makes the tree extractable.
- Renaming `scripts/dev/` → `scripts/build/` — no extra benefit now; follow-up if desired.
- Unifying `is-pure-docs-diff.sh` with the CI `changes` classifier into one shared allow-list — separate refactor; this plan keeps them cross-linked.
- Moving `tests/` or `.github/` — not needed for either goal.

## Implementation log

**Phase 1a — `scripts/dev/local/` bucket + orchestrator + CI allow-list (branch `plan/split-scripts-impl`).**
- **Orchestrator first (silent-skip guard):** `test-all.sh` now discovers across `scripts/dev/` + `agents/scripts/{core,project}/`, each root `[ -d ]`-guarded. Baseline test count = **57**; post-move discovery = **57** (no `test-*.sh` in this bucket) — invariant holds.
- **Moved 20 scripts** (`git mv`, history preserved) → `scripts/dev/local/`: the 14 §4b human-run scripts (build/run PS wrappers, `package_unreal_plugin_msvc.ps1`, `run_clang_tidy.ps1`, `run_cppcheck.py`, `manual-*`) + `build_and_run_*` shims + **`test-build-wrapper.ps1`** (deviation, see below).
- **CI:** added `scripts/dev/local/*` to the `changes`-job `case` allow-list (`build-and-test.yml`). `agents/*` untouched (already covers the agentic moves).
- **Self-anchoring depth fixes (plan §5b, discovered during impl):** `run_cppcheck.py` `parents[2]`→`parents[3]`; 7 PS wrappers' repo-root `Split-Path` chain +1 level; `build_standalone.ps1` dot-source `..\common`→`..\..\common`.
- **Reference repaths (operational only):** `BUILD.md`, Unreal-plugin `README.md`, `SmatchetImGuiPlugin.Build.cs` (3 hint strings, concat-space preserved), `.clang-tidy`, `doctor.sh` warning, STAY-side `test-cppcheck-path-detection.sh` (shadow tree mirrored to `scripts/dev/local/`).
- **Verification run:** `test-cppcheck-path-detection.sh` green end-to-end against the relocated script (proves `parents[3]` + shadow tree); all 8 edited `.ps1` parse clean (PS AST parser, 0 errors); `bash -n` + `py_compile` clean; stale-operational-path sweep = **zero hits**.
- **Deferred to Phase 1b/2:** `core/` + `project/` agentic moves (incl. the daemon/hook-critical set + live `.claude/settings.json`, needs user authz) and the `is-pure-docs-diff.sh` §6 regex.

## Deviations from plan
- **`test-build-wrapper.ps1` → `scripts/dev/local/`** (plan §2 had `test-build-*` STAY). Rationale: it does **not** launch the exe (the STAY rationale) — it smoke-tests the build/run PS wrappers, is `.ps1` (not in `test-all.sh`'s `.sh` glob), 0 CI refs, and `Join-Path $PSScriptRoot`'s the moved `build_standalone.ps1`/`run_standalone.ps1`. Moving it *with* its targets keeps the same-dir joins working with zero path edits and co-locates the test with what it tests.

## Verification (actual)
*(populated post-ship)*
