# Plan — backlog sweep 2026-10 (fix every entry fixable from a Linux container)

> **Slug**: `backlog-sweep-2026-10` (matches this file's basename without `.md`).
>
> **Status**: `active` — one PR, many independent slices (work packages A–I below), developed in parallel worktrees and integrated on one branch.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

The user asked to "fix all backlog issues possible". The open backlog (2026-10-04, `develop` @ `705b0a0`) holds ~190 self-improvement entries (`docs/self-improvement/categories/{debt,process,tooling,infra,test,security}` — monolith files plus per-entry files) and 6 open GitHub Issues. This session runs in a Linux cloud container: it can build the `ninja-test-linux` doctest subset, the whole-core `posix-core-check` archive (+ `SmatchetCommandsTests`), and the libFuzzer targets, and it can run bats / shellcheck / python gates. It cannot build MSVC, launch the Windows app, run bucket-C/E, read live CI logs, or change repo settings.

Intended outcome: every entry that is fixable *and validatable* here is fixed with test coverage; every entry already resolved in the tree is archived; every entry that cannot be done here keeps an accurate residual (what remains + why).

## Approach

1. **Triage** (done): seven read-only agents classified every entry FIX / PARTIAL / STALE / BLOCKED against the tree, with file:line evidence and overlap checks against open PRs and held plan-locks.
2. **Implement** in nine work packages with disjoint file ownership, each in its own worktree, each validated by the gates it touches (bats, `--selftest`, shellcheck, Linux builds/doctests):
   - **A** merge-gate poller (`merge-gates.sh`, `merge-gates.d/10-gate-filter.sh`): `jq -f` instead of a 25 KB argv filter; rate-limited / silent-CR heads block instead of failing open; pure-docs rate-limit discount of a pending CR-findings context; `plan-lock-out-of-band` needs a disposition; TSan lane advisory-drift docs.
   - **B** CR finding gate action: clean-pass wording, default-branch-code provenance step, next-step pending text, terminal state on a failed CR review, frozen-payload re-run guidance + workflow property test, observation-vs-mechanism rule.
   - **C** plan-lock release keyed on `claim.json` branch at PR close; commented-marker warning; dispatch release workflow; `--sync-pr` verdict line through a marker-preserving PR-body helper.
   - **D** merge-snapshot ledger: `safe-merge.sh` writes its row; ledger-hole detector; `postmortem-owed.sh` fails loud on a fetch error and flags a never-green CR context.
   - **E** CI robustness: Font Awesome verified from the committed TTF (no network); PAT warning split; `All checks green` aggregate workflow + auto-merge arming guard hook; coverage infra-crash retry/exit code.
   - **F** bucket-E driver hygiene: shared driver helper (profile isolation, update check off, exe-staleness guard, wedge-proof capture), orphaned/renamed drivers + filter-resolves gate, testing docs.
   - **G** git hygiene + backlog tooling: applied.md rotation handles both entry formats; worktree-prune idle/untracked/protected guards + `--branches`; leftover-audit `--remote`; relative `core.hooksPath`; `no-new-ps1` gate.
   - **H** lint/audit gates: coverage-gate off-target platform arms; comment_audit moved-line grandfathering; portable-purity rename following; p99 gate tests; enforcement-surface review WARN; `run-gate.sh`; localization fallback-drift check.
   - **I** product C++ (posix-core-check + doctests): Issue #2270 (failed `p4 describe` cached for the session), render-path I/O (project picker, pending-action counts), dispatcher completion-loss accounting, AI client URL helpers, fuzz targets for the Jira mappers, and smaller debt items.
3. **Archive** STALE entries with `archive-backlog-entry.sh` (rotation skipped: the one-time applied.md re-partition is its own change) and rewrite residuals of PARTIAL / re-scoped entries in place.

Held plan-locks of other sessions (`hook-tree-resolution`, `sanitizer-nightly-run-tests`, `shutdown-cancel-all-pane-syncs`) and open PRs #2294 / #2291 / #2288 / #2279 fence off their write sets; entries whose fix lives in those files stay open with that reason.

## Files to modify

Grouped by work package; each package owns its files exclusively (shared docs are edited section-disjointly).

1. A — `agents/scripts/core/merge-gates.sh`, `agents/scripts/core/merge-gates.d/10-gate-filter.sh`, `agents/scripts/core/plan-lock-gate.sh`, `tests/bats/merge_gates.bats`, `.github/workflows/tsan-linux-nightly.yml` (comment), TSan-related plan docs, `docs/agent-rules/merge-gates.md` § Per-PR overrides.
2. B — `.github/actions/cr-finding-gate/action.yml`, `.github/workflows/cr-finding-gate.yml`, `tests/bats/cr_finding_gate.bats`, new workflow-payload property bats, `docs/agent-rules/merge-gates.md` § CodeRabbit, `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` § Workflow.
3. C — `.github/workflows/lock-cleanup.yml`, new `lock-release-on-close.sh` + dispatch workflow, `record-review-verdict.sh`, new PR-body helper, lock bats.
4. D — `safe-merge.sh`, `merge-snapshot-append.sh`, `postmortem-owed.sh`, new `merge-snapshot-holes.sh`, their bats, SessionStart wiring.
5. E — `.github/actions/fetch-fontawesome/`, `doc-validation.yml` autosync note step, new `all-checks-green` workflow + script, new auto-merge guard hook, `scripts/dev/coverage.sh`.
6. F — `scripts/dev/test-ui-*.sh`, `scripts/dev/lib/` helper, `scripts/dev/is-exe-fresh.sh`, `scripts/dev/test-lua-error-log.sh`, testing docs + test-authoring skill.
7. G — `worktree-prune.sh`, `git-leftover-audit.sh`, `setup-harness.sh`, `doctor.sh`, `rotate-applied-md.sh`, `sort-applied-md.sh`, new `test-no-new-ps1.sh`.
8. H — `coverage-delta-gate.sh`, `comment_audit.py`, `test-portable-purity.sh`, `lib/review-ack.sh`, new `run-gate.sh`, perf-compare tests, localization drift check, `docs/agent-rules/cpp-rules.md`.
9. I — `Source/Core/src/P4Annotate.cpp`, `Source/Core/src/Ui/P4ClPreview.cpp`, `Source/Core/src/Ui/SmatchetProjectPicker.cpp`, `Source/Core/src/Sync/OfflineQueueService.cpp`, `MainThreadDispatcher`, AI clients + a pure URL header, `tests/fuzz/`, matching doctests.
10. Backlog entries — archive STALE files; rewrite PARTIAL residuals.

## Existing utilities reused

- `agents/scripts/core/archive-backlog-entry.sh` — archival with link re-depthing (rotation skipped via `ARCHIVE_SKIP_ROTATE=1`).
- `merge-gates.d/10-gate-filter.sh` `$pureDocs` / rate-limit bindings and the `cr-out-of-band` + `cr-disposition` downgrade shape — reused for the new terminal arms and the plan-lock disposition.
- `OfflineFirstPure.h` `kLookupRetryAfterSeconds` — retry window for a failed `p4 describe`.
- `scripts/dev/is-exe-fresh.sh` — exe-staleness check behind the shared bucket-E driver helper.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — no whale prompt / `AGENTS.md` extraction; AGENTS.md is left untouched (149/150 lines).

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: improves — removes a per-frame SQLite SELECT (pending counts) and a per-frame cache-file parse (project picker) from the render path.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: improves — same two render-path reads move off the per-frame path.
- **Pillar 3 (never crash)**: improves — dispatcher completion tasks are no longer silently dropped at the queue cap; no new throwing paths.
- **Pillar 4 (accessibility)**: no impact — no visual change; files on the visual-validation trigger list are deliberately not edited.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI** — fires; the render-path changes (project picker combo, status-bar pending counts) are exercised by the default grid scenarios in `scripts/dev/perf-pr-fast-set.json`; expected effect is neutral-to-better.
2. **Pillar 2 static scanner** — fires; the diff removes sync I/O reachable from ImGui frames and adds none.
3. **Dispatcher drain** — fires (MainThreadDispatcher overflow policy): drain order is unchanged; only the eviction choice at the cap and drop accounting change.
4. **Visible-cue bucket-E harness** — N/A, no new sync-stall path.
5. **Marker inventory** — N/A, no `SMATCHET_UI_PERF_SCOPE` markers added.

**Pre-push local check**: not runnable here (Windows perf scenarios); PR-fast CI is the gate.

## Risks / non-goals

- Policy tightening in the merge poller (silent / rate-limited CodeRabbit heads now block until `cr-out-of-band` + `cr-disposition` or a real review) — intended by the entries; documented in `merge-gates.md`.
- Bucket-E drivers cannot run here — every driver edit is behaviour-preserving and covered by stub-exe bats; CI runs the drivers it owns.
- Non-goals: anything behind another session's plan-lock or an open PR; visual changes (no `Smatchet*Ui*.cpp` / theme / localization-table edits); repo-settings changes (required contexts, secrets).

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `ninja-test-linux` (`SmatchetTsanTests`) + `posix-core-check` (`SmatchetCommandsTests`) green; fuzz targets build and run a bounded smoke under `ninja-fuzzer-linux`.
- **Bucket E (ImGui Test Engine)**: not runnable here; driver-script changes covered by stub-exe bats.
- **Bash-driver scenario / screenshot / sanitizer**: every touched bats suite + gate `--selftest`; `shellcheck`; `test-orphan-bats.sh`; `test-gate-selftests.sh`.
- **Build gate**: `posix-core-check` compiles every touched Core TU on Linux clang; the MSVC dual-target build is CI's.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run against `docs/CONTEXT.md` / `docs/adr/` before the PR; outcome recorded in § Verification (actual).
- **Manual residue**: MSVC build, bucket-C/E, and live-CI behaviour of new workflows — covered by the PR's own CI run.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Entries blocked by other sessions' locks / open PRs — stay open with that reason (e.g. missing-parent fetch cap + cancel after #2291; Markdown alt-text Issue #2284 after #2294).
- Visual / GUI-only work (bucket-E authoring, About modal coverage, dock repair helpers, mobile accessibility) — stays open.
- The one-time applied.md re-partition — its own change once the rotation fix has landed.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*In the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*Blocked while `docs/plans/INDEX.md` sits in another session's plan-lock write set (`hook-tree-resolution`); the move rides the first follow-up after that lock releases.*
