# Plan — split scripts: build/dev stay, agentic move under `agents/scripts/{core,project}/`

> **Slug**: `split-scripts-build-vs-agentic`.

## Context

Two problems, one move.

1. **CI over-builds on agentic-script-only PRs.** The `changes` job in `.github/workflows/build-and-test.yml` runs the 45-min Windows MSVC builds unless every changed file matches a docs-class allow-list (`*.md`, `docs/*`, **`agents/*`**, a few dotfiles). All of flat `scripts/` is code-class, so a PR touching only an agentic script (e.g. `scripts/dev/memory-drain-nudge.sh`, a SessionStart nudge that never compiles anything) triggers two full builds for nothing (observed on PR #557).
2. **The agentic layer is being prepared for extraction into a separate repo** (per `docs/PORTABILITY.md` § Extraction checklist). Today the agentic scripts are intermixed with C++ build/dev scripts in one flat `scripts/dev/` dir, so "what ships to the agents repo" is not structurally visible.

Both are solved by relocating the agentic scripts under the existing portable agentic root `agents/`, mirroring the agent split (`agents/core/` portable vs `agents/project/` subsystem-bound). Because the CI allow-list already contains `agents/*` **and** the bash `case` glob matches nested paths (`agents/scripts/core/x.sh` matches `agents/*`), the move **needs no workflow edit** — relocation alone makes every agentic script docs-class.

**Intended outcome**: after this lands, (a) agentic-script-only PRs skip the MSVC builds (required contexts still report `skipped == success`), and (b) `agents/` is a self-contained agentic tree — agents + their scripts — ready to lift into a standalone repo, while the C++ **build remains self-sufficient** (every script the build/CI-build-job invokes stays in `scripts/dev/`).

## Approach

Three-way classification, mirroring `docs/PORTABILITY.md` § Agent split rule:

- **`scripts/dev/` (STAYS) — build / dev / run-the-exe.** Any script that compiles, launches, or tests the C++ product, OR is invoked by a CI build job, OR is sourced by such a script. The code repo must build with **no** dependency on the `agents/` tree, so build-invoked utilities stay here even when otherwise "portable" (notably `project-config.sh`, sourced by `with-msvc-env.sh`).
- **`agents/scripts/core/` (MOVE) — portable agentic.** Generic ship-line / merge-gate / lock / harness-wiring / plan-doc / agent-meta tooling + the docs/agent-meta test scripts. Mirrors `agents/core/`. Reads project values from `project.config.json`; never compiles or runs the product.
- **`agents/scripts/project/` (MOVE) — project-bound agentic.** Scripts coupled to *this* project's subsystems/values: `p4-*` (this project's optional VCS layer wiring), `test-lint-rules*` (this project's strict-zone globs), `test-config-migration.sh`. Mirrors `agents/project/`.

**Boundary rule (deterministic):** runs/builds/tests the exe ⇒ `scripts/dev/`. Else agentic ⇒ `core/` if it would work unchanged in another project (values from config), `project/` if its identity is bound to a this-project subsystem/zone. Test scripts split the same way: `test-ui-*`, `test-*-roundtrip`, `test-grid-edit-perf-*`, `perf-*`, `test-build-*` STAY (they launch the exe); `test-doc-anchors`, `test-plan-*`, `test-agent-*`, `test-skill-*`, `test-shell-lint`, `test-merge-*`, `test-lock-*`, `test-markdown-links`, `test-workflow-yaml`, `test-backlog-counts`, `test-portable-purity` → `core/`; `test-lint-rules*`, `test-p4-dual-vcs` → `project/`.

**Trade-off named:** the move touches ~90 scripts and every hardcoded reference to them. The risk is a missed reference breaking a hook or the running merge-watcher daemon. Mitigated by 2-phase execution + an exhaustive grep-sweep gate (below). The CI benefit needs *no* allow-list change — `agents/*` already covers it — which also shrinks the blast radius vs. a `scripts/agent/` target.

## Files to modify

**1. New dirs**
1. `agents/scripts/core/` + `agents/scripts/project/` — new directories (move targets).

**2. STAY in `scripts/dev/` (build/dev/run-exe + build-sourced — ~40):** `with-msvc-env.sh`, `project-config.sh` (build dependency — overrides PORTABILITY's "portable" tag; see Deviations), `doctor.sh`, `check-required-tools.sh`, `build_*.ps1`, `build-msvc-asan.ps1`, `build_and_*.ps1`, `package_unreal_plugin_msvc.ps1`, `rebuild_testproject_plugin.ps1`, `attach_unreal_vsjit.ps1`, `run_standalone.ps1`, `run_clang_tidy.ps1`, `run_cppcheck.py`, `relaunch-smatchet.sh`, `coverage*.sh`, `coverage-delta-gate.sh`, `perf-*.{sh,py,json}`, `perf-marker-inventory.sh`, `test-build-*`, `test-doctor.sh`, `test-cppcheck-path-detection.sh`, `test-ui-*.sh`, `test-*-roundtrip.sh`, `test-grid-edit-perf-*.sh`, `test-callstack-tooltip-hover.sh`, `test-tooltip-wrapwidth.sh`, `test-markdown-lang-tag.sh`, `test-theme-*.sh`, `test-whisper-*.sh`, `test-ai-prefs-validator.sh`, `test-lua-error-log.sh`, `manual-*`.

**3. MOVE → `agents/scripts/core/` (portable agentic — ~75):** `merge-gates.{sh,graphql}`, `merge-gates-prompt.sh`, `merge-watcher.py`, `merge-watcher-cli.py`, `merge-watcher-*.ps1`, `watch-register-if-enabled.sh`, `smatchet-notify*`, `agent-progress.sh`, `git-janitor.sh`, `coderabbit-triage.py`, `verify-cr-reply.sh`, `check-main-repo-clean.sh`, `is-pure-docs-diff.sh`, `lock-*.sh`, `locks-*.sh`, `setup-locks-ruleset.sh`, `_lock-json.py`, `vexp-strip-agents-md.sh`, `memory-drain-nudge.sh`, `sort-applied-md.sh`, `tail-agent.sh`, `rewrite-plan-paths.sh`, `plan-doc-table-probe.sh`, `test-shell-lint.sh`, `test-doc-anchors.sh`, `test_doc_anchors.py`, `test-agent-contract.sh`, `test-agent-discovery-fixture.sh`, `test-plan-index.sh`, `test-plan-naming.sh`, `test-plan-ref-integrity.sh`, `test-markdown-links.sh`, `test-portable-purity.sh`, `test-skill-load-log.sh`, `test-skill-vs-agent-parity.sh`, `test-backlog-counts.sh`, `test-merge-gates.sh`, `test-merge-watcher-bats.sh`, `test-merge-watcher-integration-bats.sh`, `test-lock-primitives*.sh`, `test-workflow-yaml.sh`, `test-setup-harness.sh`, `test-lint-bash.sh`, `test-lint-hook-split.sh`, `test-pre-push-merged-pr-guard.sh`. **Top-level `scripts/`:** `clear-session-context.sh`, `agent-tokens-report.py`, `setup-harness.sh` → `agents/scripts/core/`.

**4. MOVE → `agents/scripts/project/` (project-bound agentic — ~10):** `p4-git-sync-check.sh`, `p4-reconcile-check.sh`, `p4-task-stream*.sh`, `test-p4-dual-vcs.sh`, `test-lint-rules.sh`, `test-lint-rules-bats.sh`, `test-config-migration.sh`.

**5. Reference sweep (every moved path):**
- `.github/workflows/*.yml` (esp. `test-all.sh` callers, doc-validation, cr-finding-gate, pillar2-scan, locks-render, lock-*), `.github/actions/*/action.yml`.
- `.claude/hooks/*` + `.claude/settings.json` (**live, gitignored — needs explicit user authz per self-mod guard**) + `docs/harness/claude-code/settings.json.tmpl` + `docs/harness/claude-code/hooks/*`.
- `AGENTS.md`, `docs/agent-rules/*.md`, `docs/**` (esp. `docs/PORTABILITY.md` § generic/project script rows + § External path contracts table; `docs/STRUCTURE.md` if it lists script paths), `agents/*.md`.
- Cross-script `source` / invocation lines (e.g. `test-all.sh` runs most agentic test-*; `merge-watcher.py` calls `merge-gates.sh`; `git-janitor` calls `is-pure-docs-diff.sh`; `with-msvc-env`/build sources `project-config.sh` — stays, no change).
- `project.config.json` (any `scripts/dev/...` path values).
- `scripts/dev/setup-harness.sh` self-reference + its `link_*` targets (it moves to core; the SessionStart auto-sync block in `clear-session-context.sh` repaths too).
- **Out of scope:** `.claude/streams/watch-button/` (sibling agent's separate checkout — regenerated, not edited here).

**6. Classifier sync (optional, flagged):** `is-pure-docs-diff.sh` (now in core) — its `allow` regex currently excludes `agents/`; decide whether agentic-script-only diffs should count as "pure docs" for the git-janitor FF-batch exception. Left as an explicit decision, not silently changed.

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
- **Bash-driver**: (1) `bash scripts/dev/test-all.sh` (or its new orchestrator path) green after the sweep — exercises every moved test-* at its new path. (2) `bash agents/scripts/core/test-setup-harness.sh` green (harness wiring repaths). (3) `git grep -nE "scripts/(dev/)?(<moved-name>)"` returns **zero** stale hits for every moved script — the sweep gate. (4) Probe PR touching only an `agents/scripts/` file → `Detect code changes` outputs `code=false`, MSVC jobs `skipped`. (5) Probe PR touching a `scripts/dev/` build script → MSVC jobs **run**.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — confirms no build-invocation path broke; also confirms the build needs nothing from `agents/scripts/`.
- **Daemon smoke (Phase 2)**: `SmatchetMergeWatcher` restarts; `merge-watcher-cli.py list` + one poll cycle on a registered PR succeeds from the new path.
- **Manual residue**: the live `.claude/settings.json` edit (Phase 2) requires user authz — tracked in the phase checklist, not silent.

## Out of scope (flagged, not designed)

- Creating the standalone agents repo / git-subtree split — this plan only makes the tree extractable.
- Renaming `scripts/dev/` → `scripts/build/` — no extra benefit now; follow-up if desired.
- Unifying `is-pure-docs-diff.sh` with the CI `changes` classifier into one shared allow-list — separate refactor; this plan keeps them cross-linked.
- Moving `tests/` or `.github/` — not needed for either goal.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship — note: `project-config.sh` kept in `scripts/dev/` despite PORTABILITY listing it portable, because the C++ build sources it; PORTABILITY's generic-scripts row updated to reflect the `agents/scripts/{core,project}/` destinations.)*

## Verification (actual)
*(populated post-ship)*
