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

- **Triage + archival** — `27bd2ab` · `d21d40d` · `484d8a5` · `5b6ae67` · `7560495` · `b7f346f` · `e7fab5d`: archived the entries the tree or a slice below resolves, folded duplicates, re-filed three process entries as `debt`, recorded residuals on partial ones; `fe83fea` adds the cache-sourced-UI review check, the #2148 eval case and the WARN-gate measurement rule.
- **A — merge-gate poller** — `b1b0e77` filter via standalone `jq -f`; `4b5d368` / `922fc7d` rate-limited and silent-CR code heads block (grace expiry terminal); `c9beda9` pure-docs rate-limit discount; `3e4f100` / `bf1fc28` `plan-lock-out-of-band` needs a disposition and refuses a stale Plan-lock red; `4769ab8` / `277e20a` / `493fac9` / `cb05ec1` tests + comments; `4b65d7f` TSan wording; `d85b6cc` poller skips the aggregate; `3d0b330` payload-input checks are never "fixed" by a rerun.
- **B — CR finding gate** — `2bf381c` "No findings" clean pass + full-review rule; `f4d8fbb` gate-code provenance; `5464d76` PENDING text names the human step; `e4c05d4` a failed requested review is terminal; `5a23f04` mechanism claims need a positive observation.
- **C — plan-lock release** — `b50b173` release by `claim.json` branch; `7570c28` commented-marker warning; `7493fa5` dispatch release workflow; `3450960` `record-review-verdict.sh --sync-pr` through a marker-preserving PR-body helper.
- **D — merge-snapshot ledger** — `0e37bfd` `safe-merge.sh` writes its row; `58007a4` ledger-hole detector + nudge; `b929a18` / `d8d9688` `postmortem-owed.sh` fails loud and flags never-green CR contexts; `5cc6125` optional `crState`; `605fdb4` verifier trace labels.
- **E — CI robustness** — `214553e` Font Awesome verified from the committed TTF; `7c8e983` expired vs absent `PLAN_INDEX_PAT`; `d7418fe` coverage infra-crash retry + exit 3; `b14d25e` / `a26da91` / `48e1609` / `db614f6` the `All checks green (block-on-any-red)` aggregate; `67808d5` / `e852197` / `7d9600e` auto-merge arm guard hook.
- **F — bucket-E drivers** — `c9e93ea` shared driver preamble; `31a059e` orphaned drivers + default-filter gate; `9f5e763` testing docs; `fe6923d` unseeded tracker-first-run profile; `35fce15` ASCII bats titles; `9318ea8` / `6d621c9` wrappers + doc follow-ups.
- **G — git hygiene + backlog tooling** — `e9e3d3c` applied.md rotation/sort by each entry's own date; `882e048` / `6b79203` worktree-prune guards + `--branches`; `4318bc3` leftover-audit `--remote`; `c8c8b1a` relative `core.hooksPath`; `889ee61` `no-new-ps1`; `93058e0` `--flag=N` forms.
- **H — lint/audit gates** — `decd53e` coverage-gate off-target arms; `2a6b61c` comment_audit moved-line grandfathering; `53a62b9` portable-purity renames; `081334e` p99 gate bats; `6d911c2` / `659a8ec` gate-script self-edit WARN; `93f669c` `run-gate.sh`; `86f7c1b` localization fallback-drift WARN.
- **I1 — product C++** — `a6e014b` dispatcher completion posts survive overflow + loss accounting; `da9d102` RefreshFieldCatalog pane latch; `bcfbd81` `FindLocked` takes the registry lock; `e059778` `debug.lua_eval` / `debug.lua_log_test` marshal to the UI thread. (`6e47f82` p4 describe retry, `a42a5d8` project picker, `9b4b2c5` pending counts were superseded by develop's #2313 at the `4d12153` merge — see Deviations.)
- **I2 — product C++** — `ce995f1` `AiWirePure.h` (JoinUrl / ResolveBaseUrlOr / message builders); `7ffb1e6` StubAiClientCancel advisory budgets; `c2ce873` Jira mapper + field-catalog fuzzers and the out-of-range JSON int fix they found; `da8cb8f` `UpdateField` default body; `3fd89de` dock-slot gate; `1f496cf` `GatherAboutInfo(TrackerConfig)`.
- **Review round (4 adversarial reviews of the branch)** — C++: `b53e0e3` catalog writes re-check the generation under the mutex, `ed3fc9c` allocation-safe drain deferral, `c871e5b` FindLocked race under TSan + lua_eval test join; Issue #2308 filed for the background-pool shutdown race (AppController.cpp locked). Workflows/hooks: `a792528`…`ddb3bb2` (aggregate disposition, moot verdict, budget, ETag polling; prune moved-head guard; tokenizing auto-merge guard; rotation write order; coverage exit 4; PAT wording; self-edit advisory paths; leftover-audit gh failure). Test tooling: `b15f9a4`…`5310678` (coverage-gate classifier reset + lexer + Android-only arms; driver trap exit status, linked-source staleness, pinned seed; comment_audit per-file keys; verifier-labels PR segments + overrides). Merge-gate scripts: `fd14f4c` … `db484c0` (one shared disposition reader for the poller, safe-admin-merge and the aggregate; pure-docs rate-limit discount tied to the current head; manual-review-only CodeRabbit status is not a review; jq `IN()` probe with a `gh --jq` fallback; stale Plan-lock re-check via the base-repo remote; safe-merge binds the arm to the gated head and refuses a leaked stub gate; locks kept while a branch's work is live; PR-body race guard; negated bats asserts that now assert). Round-2 review of those fixes: `ee59a8f` catalog clear epoch + latched config + one locked write, `df08f05` CL tooltip launch failure, `2f52aad` worklog sum saturation + 64-bit progress, `71ecd06` race-test pacing + deterministic cancel case, `9b37098` coverage gate marks spliced comments / directives after a comment untrusted; the layer-side review items ride the layer PR.
- **Round 3 (review of the round-2 fixes and the post-split host commits)** — `360de7a` a catalog fetch is judged by its pane's tracker kind and an explicit clear empties memory only; `1d9cee1` the progress fast path keeps 10+ digit values whole; `b7c571e` / `872dbc5` the coverage gate's lexer stops trusting splices, directive-name splits and directives after a comment; `e5166f8` / `460d5b1` bats negatives demand grep's no-match status, and the layer-paths suite fails on CI without the mount; `26b8d44` every agent-layer path a host workflow calls must exist in the pinned mount; `acf5b73` only a newer refresh supersedes a project refresh (per-catalog `RefreshSeq`; the project is pinned with its data), a superseded failure writes no banner, and an unscoped refresh fetches for the tracker the pane runs. The grid's own catalog apply path still reads the global config: Issue #2328 (needs an AppController.h overload; locked).
- **Round 4 (review of the round-3 fixes: 4 medium, 9 low)** — `e9e0f7a`:
  - the grid's two-call apply pins its own project with its catalog (a pending pin, consumed by the next unguarded apply), so a draft refresh landing in between no longer lends it the draft's project;
  - a failed refresh restores its snapshot only into a still-empty catalog and decides its banner on the catalog under the lock, and the success path clears the banner under the lock;
  - a pane keyed by a site takes a refresh only for that site.

  `c67f7bf`: the coverage gate follows the compilers on `R""(` delimiters, `$` or non-ASCII before an R prefix, a carriage return inside a line, form feed or vertical tab, backslash then whitespace, and a C/C++ file git shows as binary. New fixtures pin each round-3 splice rule separately.

  `af2d2e3`: workflow_layer_paths reads `agent-layer/` spellings; the last bare bats negatives demand status 1.

  Left as is: the precise splice rule (L3; the conservative rule only costs three headers their drops), and two test gaps (the posix catalog tests are not in the TSan rig; the failure handler's in-lock re-checks have no injection seam), filed as `categories/test/2026-10-06-field-catalog-race-tests-miss-tsan-and-handler-window.md`.
- **Merges** — `4d12153` merges origin/develop (#2300–#2320); `70f488b` merges #2321 (the agent-layer move): every layer-side path left this repo, `4b905ad` / `4c6162a` reach layer scripts through `agent-layer/`, `1266a1d` repairs the applied ledgers after the union merge.
- **Agent-layer half** — ported to alexandrosk0/the-unwilling-agentic-bunch as its own PR; this PR carries the `agent-layer` pointer bump once that PR merges (after the Historical-review Batch 26 layer PR, by agreement between the two sessions).

## Deviations from plan

- **I1 overlap with #2313** — develop's #2313 fixed the p4 describe failure cache (#2270), the project picker read and the pending-count reads with its own design (published snapshots; an explicit refresh after the cache is recreated). The merge takes develop's versions and drops this branch's three commits and their tests, including the same-address recount fix `5bd04ee` that develop's design does not need. #2270 is therefore closed by #2313, not by this PR.
- **applied.md re-partition** — planned as its own change; it happened in this PR instead, because the develop merge (applied.md is `merge=union`) concatenated the bounded head with this branch's unrotated copy, and rebuilding it ran the fixed rotation, which also re-homed 8 misfiled per-entry blocks. Verified line-for-line: nothing lost or duplicated.
- **Not done (locked or out of reach)** — AppController.h / .cpp / LocalCacheDb.cpp stayed untouched (`shutdown-cancel-all-pane-syncs` lock): the `PostCompletionToMainThread` override, the shutdown race (#2308) and the guarded grid catalog apply (#2328) wait for that work. Visual-validation files were not edited. Repo settings (required contexts, secrets) are admin steps.
- **Agent-layer split** — develop's #2321 moved the agent tooling into the `agent-layer/` submodule mid-flight, so packages A–H (and their review fixes) ship as a layer PR plus a pointer bump here, not in this PR's diff. Host workflows that call new layer scripts (all-checks-green, lock-release-on-close) fail until the bump.
- **Plan archive** — the `git mv` to `shipped/` waits: `docs/plans/INDEX.md` is in the `hook-tree-resolution` lock's write set.
- **Catalog banner readers** — the banner strings and `fieldCatalogEverLoaded_` are now written under the catalog mutex on every path, but `AppController.h` / `GridContextDepsAdapter.cpp` still read them unlocked: the reader change waits for the AppController.h lock (with #2328).
- **Left open** — two pre-existing coverage-gate fail-opens the review found (a `'('` character literal inside a wrapped LOG_ call; a column-0 `++x;` read as a `+++` header) and the pinned-profile seed edge in `ui-test-driver.sh`: low, recorded for a follow-up.

## Verification (actual)

- **Linux builds** — `ninja-test-linux` (`SmatchetTsanTests`, 869 cases after the #2321 merge) green; `ninja-tsan-linux` full TSan run green (FindLocked race test confirmed to fire with the lock removed); `posix-core-check` (`SmatchetCore_PosixCheck` + `SmatchetCommandsTests`) green; fuzzers built under `ninja-fuzzer-linux`, both new targets clean for 240 s each. The full Linux `Smatchet` app target fails to link OpenGL in this container; the same on develop (no GL-related diff), so not this PR's.
- **Mutation checks** — the recount, field-catalog guard, FindLocked TSan and lua_eval tests each fail with their fix reverted; in round 3, each of the refresh-sequence, pin-with-data, pane-kind and pane-site checks fails its own test when reverted, and the eight new coverage-gate fixtures fail against the old `post_line`. The stricter gate marks no develop product file newly untrusted except three headers whose spliced macros carry string literals (`stb_image.h`, `SmatchetDragCheckbox.h`, `QuarantineTestCase.h`), which only lose the off-target drops.
- **Script gates** — every touched bats suite and `--selftest` passes; `scripts/dev/test-all.sh --ci` after the #2321 merge: 2261 passed, 3 failed — 2 are the pinned layer's old `worktree_prune` suite against this branch's new `worktree-prune.sh` (fixed by the layer PR + bump), 1 `test-lint-hook-split` collided with a concurrent dup_audit run and passes 20/20 on re-run; `test-lint-rules.sh --diff origin/develop` EXIT 0 (advisory WARNs only); `test-markdown-links.sh --all` 0 dangling; `test-docs.sh` green.
- **Not run here** — MSVC dual-target build, bucket-C/E, live behaviour of the new workflows: covered by this PR's CI.
- **grill-with-docs** — not run as a separate session; the plan's terms (merge gates, plan-locks, ledger, Quality Pillars) are the existing vocabulary of `docs/agent-rules/merge-gates.md` / ADR-0017 / ADR-0026, and no new domain term or ADR-level decision was introduced.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*In the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*Blocked while `docs/plans/INDEX.md` sits in another session's plan-lock write set (`hook-tree-resolution`); the move rides the first follow-up after that lock releases.*
