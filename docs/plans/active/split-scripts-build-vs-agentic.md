# Plan — split scripts into build/dev vs agentic/docs trees

> **Slug**: `split-scripts-build-vs-agentic`.

## Context

The CI `changes` job in `.github/workflows/build-and-test.yml` decides whether a PR runs the expensive Windows MSVC builds. It classes a file as build-affecting ("code") unless it matches a docs allow-list (`*.md`, `docs/*`, `agents/*`, a few dotfiles). **Every** file under `scripts/` is therefore code-class, because most `scripts/dev/*` genuinely affect the build (`with-msvc-env.sh`, `doctor.sh`, `build_*.ps1`). Result: a PR that only touches an *agentic* script (e.g. `scripts/dev/memory-drain-nudge.sh`, a SessionStart nudge that never compiles anything) triggers two 45-min MSVC builds for nothing (observed on PR #557).

`scripts/dev/` currently mixes two unrelated concerns in one flat dir of ~134 files: (a) C++ build / run / test-the-exe / dev-setup tooling, and (b) agentic ship-line / lock / p4 / plan / docs-meta tooling. Splitting them by directory gives the CI filter (and the sibling `scripts/dev/is-pure-docs-diff.sh` classifier) a clean structural signal.

**Intended outcome**: after this lands, a PR touching only agentic/docs scripts skips the MSVC build jobs (the required check contexts still report `skipped == success`), while any build-affecting script change still triggers them.

## Approach

Keep `scripts/dev/` as the **build / dev-setup** home (it's referenced heavily by every CI build job — minimise churn there). Create **`scripts/agent/`** and `git mv` the agentic/ship-line/docs-meta scripts into it. Add `scripts/agent/` to the CI `changes` docs-class allow-list and to `is-pure-docs-diff.sh`'s allow regex so both classifiers treat it as non-build.

**Boundary rule (deterministic, resolves every fuzzy case):** a script is **code-class (STAYS in `scripts/dev/`)** iff running it builds, launches, or tests the C++ product/exe, OR a CI build job invokes it. Otherwise it is **agentic-class (MOVES to `scripts/agent/`)** — ship-line, merge-gate, lock, p4, plan-doc, backlog, harness-wiring, and agent-meta tooling that never compiles or runs the product. Test scripts split by this same rule: `test-ui-*`, `test-*-roundtrip`, `test-grid-edit-perf-*`, and anything that launches `Smatchet.exe` STAY; `test-doc-anchors`, `test-plan-*`, `test-portable-purity`, `test-agent-*`, `test-skill-*`, `test-shell-lint`, `test-merge-*`, `test-lock-*`, `test-markdown-*`, `test-workflow-yaml`, `test-backlog-counts` MOVE.

**Trade-off named:** a flat→two-dir move touches ~90 files and every reference to them (workflows, `.github/actions/`, `.claude/hooks/`, `settings.json(.tmpl)`, `AGENTS.md`, `docs/**`, `agents/*.md`, cross-script calls, `test-all.sh`, `setup-harness.sh`, `project.config.json`). The risk is a missed reference breaking a hook or the merge-watcher daemon. Mitigated by phasing (below) + an exhaustive grep-sweep gate.

## Files to modify

Grouped. Final per-file move-set is produced mechanically (`git mv` driven by the boundary rule) and reviewed against this classification.

**1. New dir + classifiers**
1. `scripts/agent/` — new directory (target for moves).
2. `.github/workflows/build-and-test.yml:68` — add `scripts/agent/*` to the `changes` job docs-class `case` allow-list.
3. `scripts/dev/is-pure-docs-diff.sh:47` — extend `allow` regex with `scripts/agent/`.
4. `scripts/dev/test-portable-purity.sh` — if it scans script dirs, add `scripts/agent/` to its zone list.

**2. STAY in `scripts/dev/` (build/dev/run-exe — ~40):** `with-msvc-env.sh`, `doctor.sh`, `check-required-tools.sh`, `project-config.sh`, `build_*.ps1`, `build-msvc-asan.ps1`, `build_and_*.ps1`, `package_unreal_plugin_msvc.ps1`, `rebuild_testproject_plugin.ps1`, `attach_unreal_vsjit.ps1`, `run_standalone.ps1`, `run_clang_tidy.ps1`, `run_cppcheck.py`, `relaunch-smatchet.sh`, `smatchet-notify*`, `coverage*.sh`, `coverage-delta-gate.sh`, `perf-*.{sh,py,json}`, `perf-marker-inventory.sh`, `test-build-*`, `test-doctor.sh`, `test-cppcheck-path-detection.sh`, `test-ui-*.sh`, `test-*-roundtrip.sh`, `test-grid-edit-perf-*.sh`, `test-callstack-tooltip-hover.sh`, `test-tooltip-wrapwidth.sh`, `test-markdown-lang-tag.sh` (renders in exe), `test-theme-*.sh`, `test-whisper-*.sh`, `test-lua-error-log.sh`, `test-config-migration.sh`, `manual-*`.

**3. MOVE to `scripts/agent/` (ship-line / lock / p4 / plan / docs-meta / agent — ~90):** `merge-watcher.py`, `merge-watcher-cli.py`, `merge-watcher-*.ps1`, `merge-gates.{sh,graphql}`, `merge-gates-prompt.sh`, `watch-register-if-enabled.sh`, `agent-progress.sh`, `coderabbit-triage.py`, `git-janitor.sh`, `verify-cr-reply.sh`, `check-main-repo-clean.sh`, `is-pure-docs-diff.sh`*, `lock-*.sh`, `locks-*.sh`, `setup-locks-ruleset.sh`, `_lock-json.py`, `p4-*.sh`, `plan-*.sh`, `rewrite-plan-paths.sh`, `sort-applied-md.sh`, `tail-agent.sh`, `test-agent-*.sh`, `test-skill-*.sh`, `test-plan-*.sh`, `test-portable-purity.sh`, `test-doc-anchors.sh`, `test_doc_anchors.py`, `test-markdown-links.sh`, `test-workflow-yaml.sh`, `test-backlog-counts.sh`, `test-merge-*.sh`, `test-lock-*.sh`, `test-lint-bash.sh`, `test-lint-hook-split.sh`, `test-lint-rules*.sh`, `test-setup-harness.sh`, `test-pre-push-merged-pr-guard.sh`, `test-shell-lint.sh`, `plan-doc-table-probe.sh`. (*`is-pure-docs-diff.sh` moves too, but step 1.3 must reference its new path everywhere first.)

**4. Top-level `scripts/` agentic (MOVE):** `clear-session-context.sh`, `agent-tokens-report.py`, `setup-harness.sh` → `scripts/agent/` (hook/harness-critical — Phase 2).

**5. Reference sweep (every moved path):** `.github/workflows/*.yml`, `.github/actions/*/action.yml`, `.claude/hooks/*` + `.claude/settings.json` (live, gitignored — needs explicit user authz per self-mod guard) + `docs/harness/claude-code/settings.json.tmpl` + `docs/harness/claude-code/hooks/*`, `AGENTS.md`, `docs/agent-rules/*.md`, `docs/**`, `agents/*.md`, `scripts/dev/test-all.sh`, cross-script `source`/invocation lines, `project.config.json`. Out of scope: `.claude/streams/watch-button/` (sibling agent's separate checkout — regenerated, not edited here).

## Existing utilities reused

- `git mv` — preserves history per file; the move-set is scripted from the boundary rule.
- `scripts/dev/is-pure-docs-diff.sh:47` `allow` regex — the sibling classifier kept in sync with the CI `changes` job (single source-of-truth comment cross-link added).
- CI Pattern C (`docs/agent-rules/ci-required-check-pattern.md`) — the skipped-required-check==success mechanism this relies on; no change, just a new allow-list member.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — repo-tooling reorg, no `Source/Core/` touch.
- **Pillar 2 (UI-thread)**: no impact.
- **Pillar 3 (never crash)**: no impact on the product; risk is to CI/hook tooling, covered by Verification.
- **Pillar 4 (accessibility)**: N/A.

## Perf-review-system gates

N/A — diff touches no `Source/Core/` code (pure tooling/docs move).

## Risks / non-goals

- **Missed reference breaks a hook or the merge-watcher daemon.** The daemon (scheduled task `SmatchetMergeWatcher`, runs `merge-watcher.py` + `merge-gates.sh` on a loop) and the SessionStart/PostToolUse hooks invoke moved scripts by hardcoded path. Mitigation: **Phase 2** stops the daemon, moves the daemon/hook-critical set, updates `.github/actions/` + `.claude/hooks/` + `settings.json(.tmpl)`, restarts the daemon + session. Phase 1 (the ~75 non-daemon/non-hook scripts) carries no live-process risk.
- **Live `.claude/settings.json` edit hits the self-mod guard.** Accepted — surfaced for explicit user authz at Phase 2; the committed `.tmpl` covers fresh clones.
- **Sibling worktree `.claude/streams/watch-button/` holds stale paths.** Non-goal — it's a regenerated separate checkout; not swept here.
- **Boundary fuzziness.** Mitigated by the deterministic rule (runs-the-exe ⇒ stays). Any later reclassification is a one-line `git mv` + allow-list no-op.
- **Non-goal:** changing what any script *does*; renaming scripts; touching `scripts/dev/archived/`.

## Verification

- **Bucket A (pure-logic ctest)**: N/A — no C++ logic changes.
- **Bucket E**: N/A.
- **Bash-driver**: (1) `bash scripts/dev/test-all.sh` green after the sweep (exercises the moved test-* scripts at their new paths). (2) `bash scripts/dev/test-setup-harness.sh` green (validates harness wiring repaths). (3) `git grep -n "scripts/dev/<moved-name>"` returns **zero** stale hits for every moved script (the sweep gate). (4) Open a probe PR touching only a `scripts/agent/` file → confirm `Detect code changes` outputs `code=false` and the MSVC jobs show `skipped`. (5) Open a probe PR touching a `scripts/dev/` build script → confirm MSVC jobs **run**.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — sanity only (no source change; confirms no workflow/path breakage in build invocation).
- **Daemon smoke**: after Phase 2, confirm `SmatchetMergeWatcher` restarts and a registered PR still polls (`merge-watcher-cli.py list` + one poll cycle).
- **Manual residue**: the live `.claude/settings.json` edit (Phase 2) requires user authz — not silent; tracked in the phase checklist.

## Out of scope (flagged, not designed)

- Renaming `scripts/dev/` itself (e.g. to `scripts/build/`) — larger churn, no extra CI benefit; follow-up only if desired.
- Unifying `is-pure-docs-diff.sh` and the CI `changes` classifier into one shared script — attractive (single allow-list) but a separate refactor; this plan only keeps their allow-lists in sync via cross-link comment.
- Splitting `tests/` or `.github/` similarly — not needed for the build-trigger goal.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
