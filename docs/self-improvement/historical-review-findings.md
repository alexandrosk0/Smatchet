# Historical code-review findings

> Findings from sweeping **already-merged** PRs with the
> `historical-code-review` skill (`agents/_shared/skills/historical-code-review/`)
> + `agents/scripts/core/historical-review-survivors.sh`. Each PR is
> reviewed for **only the lines it introduced that are still alive and untouched
> at `origin/develop`** — code a newer PR already changed/fixed is excluded by
> construction, so nothing here is already-resolved.
>
> Reviewer model: `code-review` (opus/high). Findings are advisory backlog, not
> auto-fixed. User-visible product defects should be elevated to GitHub Issues
> (ADR-0014); the rest is tech-debt. Newest batch on top.

## Remediation log (2026-06-08)

Autonomous fix pass over the safe, deterministic findings (gate-false-pass +
doc-drift); user-visible product correctness/data-loss bugs routed to their own
PRs or GitHub Issues per ADR-0014. Each item verified still-alive at
`origin/develop` before acting; already-fixed ones marked accordingly.

- ✅ **#918** (coverage-delta-gate false-exempt) — FIXED: dropped the `'*'*` /
  `'*/'` bare comment-continuation cases (genuine continuations are consumed by
  the caller's `in_block_comment` state machine, so a `*`-leading line reaching
  the helper is a pointer-deref statement, not a comment) and replaced the broad
  `'/*'*) return 0` with a strip-and-classify-residual so `/* note */ code();`
  no longer exempts real surface. +2 `--selftest` FALLTHROUGH fixtures.
- ✅ **#834** (coverage.sh bare `Ui` token) — ALREADY FIXED on develop
  (`--excluded_sources "Source*Core*src*Ui"`, dir-anchored). No action.
- ✅ **#909 / #855** (build.md `build_and_run.ps1`/`build_standalone.ps1` refs) —
  FIXED → `scripts/dev/local/…`.
- ✅ **#630** (imgui-draw-pattern audit grep) — FIXED → `Source/Core/src/Ui/Smatchet*Ui*.cpp`.
- ✅ **#657** (CONTEXT.md AppController line-pins :574/:595) — FIXED → symbol-only
  (drift-proof).
- ✅ **#722** (CONTEXT.md header paths) — FIXED → `Source/Core/include/Tracker/{LabelEditDiffPure,GitHubClientHelpers}.h`.
- ✅ **#755** (test-rig.md `AppControllerDepsAdapter.cpp`) — FIXED → `GridContextDepsAdapter.cpp`.
- ✅ **#853** (ADR-0016 line-pin `OfflineQueueService.cpp:788`) — FIXED → symbol-only.
- ✅ **#940** (ADR-0018 plan ref) — ALREADY RESOLVED: tier-less
  `docs/plans/multi-grid-tabs.md` resolves via the ref-integrity resolver
  (117/117). No action.
- ✅ **#670** (Jira wrong-status transition, user-visible correctness) — FIXED:
  matcher extracted to the pure, Logger-free unit `smatchet::jira::FindJiraTransitionId`
  (`JiraIssueMappingPure.{h,cpp}`) with GLOBAL two-pass priority (exact status
  id / `to.name` across all transitions BEFORE the transition-name fallback);
  `JiraIssueMutation.cpp` calls it + logs the divergence warn on a name-fallback.
  4 doctest cases incl. the exact #670 shape (name-"Done" transition leading to
  "In Review" must lose to a later transition leading to "Done").
- ⏭ **#854** (offline scalar-edit data-loss), **#611 / #761 / #732 / #767 / #892**
  (sync-I/O on UI render thread → freeze), **#671** (orphaned subprocess),
  **#948** (tickets_v2 migration key) — user-visible → GitHub Issues (ADR-0014),
  not batched here.
- ⏭ **#908** (CMake dead sol2 re-run-cleanup group) — left as-is: editing the
  sol2 patch chain is higher-risk than the harmless dead comment; deferred.

## Sweep status & remaining work (as of 2026-06-13)

- **Swept:** **#331–#1174** (batches 1–9) — **~760 PRs reviewed** across 9 batches.
  Batch 9 (#331–#438, 100 PRs) + Batch 8 (#439–#541, 100 PRs) added 2026-06-13;
  Batch 7 (#1029–#1174, 122 PRs) added the same day. Tooling:
  `agents/scripts/core/historical-review-survivors.sh` + the `historical-code-review`
  skill (shipped PR #968); the persisted workflow shipped PR #1182.
- **Remaining (UNSWEPT):** merged PRs **#330 → #13** (~300). Not yet historically
  reviewed.
- **Resume instructions:**
  1. List the next batch — `gh pr list --state merged --base develop --limit 900
     --json number --jq '[.[] | select(.number < 331) | .number] | sort |
     reverse | .[0:100]'` (lower the `< 331` bound as you progress).
  2. Run the persisted workflow, passing the batch as `args`:
     `Workflow({ name: 'historical-review-sweep', args: [<the numbers>] })`.
     Pass a JSON array — but note this harness delivers `args` to the script as a
     **string** even when you pass an array (probe: `argType:'string'`,
     `parsedIsArray:true`), so the workflow `JSON.parse`s it internally. You don't
     stringify it yourself; you just don't rely on it arriving pre-parsed.
     The script is tracked at
     [`agents/project/workflows/historical-review-sweep.js`](../../agents/project/workflows/historical-review-sweep.js)
     — project-scoped (it embeds Smatchet paths, so it can't live in the
     portable, purity-gated `agents/_shared/workflows/`). `setup-harness.sh` links
     it into the gitignored `.claude/workflows/`, so it resolves by name across
     sessions (run `bash agents/scripts/core/setup-harness.sh claude-code` once
     after a fresh clone). **No per-batch script edit** — pass a different `args`
     list each batch; an empty/unparseable list throws loudly, never a silent
     no-op. Or, per PR, run `historical-review-survivors.sh --pr <N>` and review
     the survivor digest manually.
  3. Append each batch's findings here (newest on top) + commit/push.
- **Cost guide:** ~100–120 PRs/batch ≈ 5.7–7.0M output tokens, ~25–30 min wall-clock.
- **Top still-alive findings to act on first** (logged, NOT fixed per the
  no-fix directive): #854 (offline edit data-loss), #670 (wrong Jira status
  transition), #611/#761/#732/#892 (sync I/O on UI render thread → freeze),
  #671 (orphaned subprocess), #834/#918 (blocking gates measuring wrong /
  false-passing). User-visible ones → GitHub Issues per ADR-0014 when actioned.

<!-- Batches appended at the top. -->

## Batch 9 — #331–#438 (100-PR sweep, 2026-06-13)

Coverage: **100 reviewed — 7 with findings, 41 clean, 52 fully superseded.** Net: **0 CRITICAL, 1 HIGH, 0 MEDIUM, 7 LOW.** Survivor-filtered against origin/develop, so every finding is current — already-fixed code excluded by construction. (Reviewer model `code-review` opus/high, concurrency held to the Opus ≤6 guardrail — run-journal validated max overlap **exactly 6**; 100/100 agents returned, 0 died, 0 errored, all 100 model `claude-opus-4-8`; windowed-read held — max per-agent **91,423** tokens, 0 over 100k; ~20.1 min, 4.54M tokens.) **All 8 findings are `userVisible:false` (internal tooling / gates / docs / test-scaffolding) → NO GitHub Issues this pass; backlog only per ADR-0014 + the no-fix directive.** The lone HIGH (#430) is another **fail-open gate** — a non-recursive scan blind to the subdirectory sites it claims to cover — a recurrence of the Batch-8 fail-open-gate cluster (cross-filed P1 in [`categories/tooling.md`](categories/tooling.md)).

### HIGH
- **#430 (sha n/a) · `scripts/dev/test-tooltip-wrapwidth.sh:46`** — the gate scans only the **top level** of `Source/Core/src` via `os.listdir(src_dir)` (non-recursive, root `*.cpp` only), but its header contract claims "every BeginTooltip+MarkdownPreviewRender::Render block in Source/Core/src/" (full-tree). Real markdown-tooltip sites live in subdirs the scan never reaches (`Ui/SmatchetOfflineQueueUi.cpp`, `Ui/SmatchetAiAssistantUi.cpp`, `Ui/SmatchetPlanDocViewerUi.cpp`, `Ui/SmatchetFieldRender.cpp`, `Commands/Scenarios/…`). A new offending site under `Ui/` is silently skipped — `checked` never increments, the script prints "Passed: N  Failed: 0" and exits 0: a fail-open gate that cannot catch the regression it exists to prevent. Fix: walk recursively (`os.walk(src_dir)` over all `*.cpp`), keep the per-file tooltip-block parsing as-is.

### LOW (7)
- **#420 (87b78f34) · `tests/bats/merge_gates.bats:1592`** — broken doc cross-ref: comment cites `docs/evaluation/agentic-infrastructure-2026-05-23.md`, but the doc moved to `docs/reference/` (`docs/evaluation/` no longer exists). Fix: repoint to `docs/reference/agentic-infrastructure-2026-05-23.md` (lines 1630/1674 carry the same stale path outside this PR's survivor set — fix together).
- **#420 (87b78f34) · `tests/bats/merge_gates.bats:1600`** — comment-vs-code drift: the comment describes the guarded mechanism as the defensive `|| echo -1`, but `merge-gates.sh` was refactored to parameter-expansion defaults (`ci_fail="${fields[6]:--1}"`, `cr_open="${fields[12]:--1}"`); the `|| echo -1` form no longer exists. Test assertions remain correct (both verify fail-closed blocking). Fix: reword the comment to the current `${fields[N]:--1}` default form.
- **#415 (2b1119a5) · `docs/perforce/AGENT_FLOWS.md:196`** — stale line-pin: the comment pins "test-p4-dual-vcs.sh scenario 2 (line 149)" but at origin/develop line 149 is a mid-block comment; scenario 2's empty-string `SMATCHET_LOCK_BACKEND=""` contract is at line 153 (block spans 125-168). Fix: repoint to line 153, or drop the line number and reference "scenario 2" by name.
- **#403 (eb0cde08) · `docs/perforce/RUNBOOK.md:86`** — checkpoint-recovery recipe replays journals via `Get-ChildItem … | Sort-Object Name` (lexicographic), so once rotation reaches double digits the order is wrong (`journal.10.gz` sorts before `journal.2.gz`) → out-of-sequence replay during disaster recovery. Bounded (non-canonical depot, rotation rarely double-digit) but the documented recipe is subtly incorrect. Fix: sort numerically by the rotation index (`Sort-Object { [int]($_.Name -replace '\D','') }`).
- **#398 (sha n/a) · `tests/bats/merge_gates.bats:757`** — the secondary assertion `[[ … *"2/2"* || … *"1/2"* ]]` is too loose: the test exists to prove a CheckRun "build" and a StatusContext "build" are NOT deduped to one, but the OR-branch accepts `1/2` — exactly the deduped-to-one outcome it claims to reject. Primary asserts (`status -eq 1`, `1 fail`) still verify the FAILURE blocks merge, so not fully fail-open, but the count assertion can't distinguish the collision bug. Fix: drop the `|| *"1/2"*` branch, assert only `*"2/2"*`.
- **#391 (a249cf5e) · `docs/CONTEXT.md:53`** — stale forward-reference: pins the scripts at `scripts/dev/p4-task-stream*.sh`, but they landed at `agents/scripts/project/p4-task-stream*.sh` (no `scripts/dev/` copy exists); lines 55/57 of the same section already use the correct path → internally inconsistent. Fix: update line 53 to `agents/scripts/project/p4-task-stream*.sh`, or drop the now-stale forward-reference note (PRs #380/#382 merged).
- **#364 (sha n/a) · `tests/bats/merge_watcher.bats:341`** — loose disjunction: the "handle_pass on PR-already-merged → merge_failed" test asserts `merge_failed` OR `skipped`, but the stub makes `gh repo view` succeed and only `gh pr merge` fail, so only `merge_failed` can fire; the `|| skipped` weakens the guard — a regression that early-returns to `skipped` (never attempts the merge) would still pass green. Fix: drop the `|| skipped` alternative, assert only `merge_action: merge_failed`.

**Fully superseded (52, no review surface):** #438, #437, #435, #425, #423, #422, #419, #416, #414, #413, #412, #408, #406, #402, #400, #399, #396, #395, #394, #392, #389, #388, #386, #385, #383, #382, #380, #379, #378, #376, #371, #370, #369, #367, #362, #359, #358, #356, #355, #354, #353, #351, #350, #349, #346, #345, #340, #339, #338, #336, #335, #333 — every introduced line was changed/removed by a later PR; excluded by construction.

## Batch 8 — #439–#541 (100-PR sweep, 2026-06-13)

Coverage: **100 reviewed — 12 with findings, 48 clean, 40 fully superseded.** Net: **0 CRITICAL, 6 HIGH, 6 MEDIUM, 7 LOW.** Survivor-filtered against origin/develop, so every finding is current — already-fixed code excluded by construction. (Reviewer model `code-review` opus/high, concurrency held to the Opus ≤6 guardrail cap; 100/100 agents returned, 0 died, 0 errored; ~17.8 min, 4.64M tokens.) **All 19 findings are `userVisible:false` (internal tooling / gates / docs / test-scaffolding) → NO GitHub Issues this pass; backlog only per ADR-0014 + the no-fix directive.** Dominant theme: a **fail-open gate cluster** (6 HIGH) where a probe/test driver returns green on a transient error or a zero-match filter — cross-filed as P1 in [`categories/tooling.md`](categories/tooling.md).

### HIGH
- **#519 (9aaba5c7) · `.github/actions/cr-finding-gate/action.yml:69`** — the CR-installed probe fails **OPEN** on transient API errors: it collapses every non-zero `gh` exit (genuine 404 *and* auth/network/500) to `cr_installed=false`, so the required gate posts "CR not installed", exits 0, and waves a PR through with its CodeRabbit findings un-reviewed. This is exactly the pre-H12 bug `merge-gates.sh` was hardened against (its lines 194-222 separate a real 404→absent from a transient failure). Fix: set `cr_installed=false` only on a confirmed HTTP 404; treat any non-404 as installed (fail safe / closed).
- **#513 (ce58faf1) · `scripts/dev/test-ui-mcp-lua-fresh-state-race.sh:99`** — fail-open: the driver fails only when `FAILED != 0`; if the FreshState filter matches **zero** tests, `passed=0 failed=0` exits 0 green and the cross-thread `lua_State` race this guard exists to catch could re-land undetected. Fix: also fail when zero tests executed (`if [ "$PASSED" -lt 1 ]; then …; exit 1`).
- **#452 (27419f5b) · `scripts/dev/test-ui-agent-proposal-store-sqlite.sh:65`** — fail-open: no `passed=0 && failed=0` guard between the run (L60) and the final green echo, so a zero-match AgentProposalStore filter passes with zero coverage. Sibling drivers guard this. Fix: add a zero-test guard before the final echo.
- **#452 (27419f5b) · `scripts/dev/test-ui-ai-assistant-preferences.sh:65`** — same fail-open on the AiPrefsTab filter; a renamed/zero-match filter exits green. Fix: add the zero-test guard.
- **#452 (27419f5b) · `scripts/dev/test-ui-description-tooltip-markdown-render.sh:65`** — same fail-open on the DescriptionTooltip filter (defensive cover for the be2b1d9 `wrapWidth` regression); zero matches → green, regression undetected. Fix: add the zero-test guard.
- **#452 (27419f5b) · `scripts/dev/test-ui-spawn-warmup-deterministic-gate.sh:64`** — same fail-open on the SpawnWarmup filter (the infra.md P2-line-16 deterministic gate); zero matches → green. Fix: add the zero-test guard.

### MEDIUM
- **#524 (808fde79) · `.github/actions/cr-finding-gate/action.yml:167`** — the actionable-finding count is parsed only from the **first** body line (`split("\n")[0]`); a CR banner/walkthrough preamble before "Actionable comments posted: N" makes the header invisible → real findings missed, gate fails open. (`merge-gates.sh` keeps `cr_actionable=-1` on a parse-miss → fails closed — opposite direction.) Fix: scan the whole body for the header, or fail closed when the header is absent but `n_reviews > 0`.
- **#518 (a0d2b97a) · `.github/workflows/pillar2-scan.yml:57`** — `git diff … 2>/dev/null … || true` silences all errors, so a failed fetch / unresolvable base / all-zero SHAs yields zero files and exits 0 "no first-party C++ changed" **without scanning** — a Pillar-2 escape. Fix: distinguish a git error from a genuinely empty diff (capture rc, `rev-parse --verify` the base, exit 1 on error).
- **#509 (35fc99a8) · `docs/harness/claude-code/hooks/autoregister-pr.sh:28`** — the PR number is grepped as the first `pull/[0-9]+` across the **entire** payload then `head -1`, so `gh pr create --body "supersedes pull/123"` lets the body's number win → the **wrong** PR is registered with the merge-watcher → an unintended auto-merge target. Fix: parse only the created-URL (jq on `tool_response`), or take the **last** `pull/N`.
- **#502 (1b9e607e) · `docs/CONTEXT.md:35`** — stale gated literal: the Pillar-1 row says "Perf PR-fast (windows-2022) **NOT** required on develop", but that promotion already shipped — it **is** required. Fix: update the row to "Required on develop".
- **#498 (sha n/a) · `tests/bats/markdown_links.bats:49`** — the detection/exclusion bats cases re-run **handwritten Python copies** of the lint regex instead of invoking `$LINT`: the inline `LINK_RE` omits the title-suffix branch and the inline `is_active_md` hardcodes only the `docs/plans/shipped` exclusion vs the real lint's two. A real regression in the lint would still pass the test. Fix: drive every case through the real `$LINT`.
- **#471 (5701646a) · `docs/harness/claude-code/hooks/lint-catch-all.py:59`** — the body-capture loop only appends when `j > body_start`, so a single-line `} catch (...) { return false; }` yields an empty body → misclassified as an empty catch (false CRITICAL) even though it has content. Fix: seed `body_text` with the post-`{` remainder of the opening line.

### LOW (7)
- **#502 (1b9e607e) · `docs/CONTEXT.md:37`** — count drift: claims "3 MSVC variants" but branch protection lists 2. Fix: change to 2.
- **#502 (1b9e607e) · `docs/CONTEXT.md:35`** — scenario-count drift: "all 15 scenarios" but the canonical count is 14. Fix: reconcile to 14 (or "all registered scenarios").
- **#471 (5701646a) · `docs/harness/claude-code/hooks/lint-catch-all.py:48`** — the brace scan is bounded to a 100-line window; a `catch` body extending beyond it is silently skipped (fail-open). Fix: scan to EOF, or emit a diagnostic on window-exhaustion.
- **#471 (5701646a) · `docs/harness/claude-code/hooks/lint-catch-all.py:49`** — brace depth counts braces inside string literals / comments, so `LOG_ERROR("… }")` miscounts and can misclassify the catch. Fix: skip braces inside string literals and `//` comments.
- **#447 (57081811) · `tests/support/FakePlaneFixture.h:88`** — raw `new FakeTrackerClient("Plane")`; `no-raw-new` is an absolute (0-grandfathering) rule. Fix: `std::make_unique<FakeTrackerClient>("Plane")`.
- **#446 (sha n/a) · `tests/support/FakeGitHubFixture.h:84`** — raw `new FakeTrackerClient(...)`; test scaffolding, no leak, but the same `no-raw-new` style deviation. Fix: `make_unique`.
- **#445 (4407adcd) · `tests/_debug/SmatchetAgentDebug.h:358`** — `SMATCHET_AGENT_DEBUG_FSYNC=true` only increments `fsync_count`; it does **not** fsync (no-op), yet is advertised as "semantically wired" → a latent footgun for anyone relying on it for durability testing. Fix: back it with an explicit fd + real fsync, or change the comment to "no-op (count only)".

**Fully superseded (40, no review surface):** #536, #532, #529, #528, #521, #516, #515, #512, #508, #506, #504, #503, #501, #499, #496, #494, #493, #492, #491, #489, #483, #481, #480, #479, #474, #470, #469, #468, #467, #466, #465, #464, #463, #455, #453, #451, #450, #449, #448, #440 — every introduced line was changed/removed by a later PR; excluded by construction.

## Batch 7 — newer #1029–#1174 (122-PR sweep, 2026-06-13)

Coverage: **122 reviewed — 16 with findings, 99 clean, 7 fully superseded.** Net: **0 CRITICAL, 3 HIGH, 5 MEDIUM, 10 LOW.** Survivor-filtered against origin/develop, so every finding is current — already-fixed code excluded by construction. (Reviewer model `code-review` opus/high; 122/122 agents returned, 0 died, 0 errored.) **3 user-visible findings → GitHub-Issue candidates (ADR-0014): #1158, #1138, #1049** — logged here, **NOT** filed and **NOT** fixed this pass per the no-fix directive.

### HIGH
- **#1158 (928693ae) · `Source/Core/src/Commands/PaneCommands.cpp:176`** — `pane.new` arms the deferred-create latch (`d.paneAddRequest.sourceId = focusedPane().id`, :169) **before** the `BackendCredentialsPresent` check. On absent creds the handler returns `Failure` (:176) but leaves `sourceId` set with `targetBackendKey` cleared; the host's `ApplyPaneAddAndCloseRequestsCore` keys only on `!sourceId.empty()` (no credential re-check) and creates a same-backend **duplicate pane** next frame. So `pane.new {backend:"Plane"}` with no Plane creds reports "no credentials configured" yet still spawns an unwanted pane — command reports failure but mutates state. **→ Issue candidate (user-visible).** Fix: validate creds + resolve backend/view into locals first, arm `d.paneAddRequest` only on the success path; or clear the latch (`d.paneAddRequest = PaneAddRequest{};`) before the `Failure` return.
- **#1138 (49932ed8) · `Source/Core/src/AppController_CatalogAndFieldEdit.cpp:1952`** — data race on the `gridContexts_` `std::map` **container itself**. `FetchPaneGroupMembers` runs on a `std::async` worker (`SmatchetUserInfoUi::launchMembersFetch`) and, after a multi-second blocking HTTP fetch, calls `gridContexts_.find(paneId)` for the roster write-back. Concurrently the UI thread's `TickAllContexts → retireExpiredHiddenContexts_` (AppController.cpp:797-841) **erases** from the same bare `std::map` (no guarding mutex; AppController.h:999). ADR-0012's latch/graveyard keeps the retired `GridLiveContext` object alive but does **not** serialize map-node mutation — an erase rebalances/deletes tree nodes a concurrent find traverses → UB/crash. The User-Info window can outlive a retired/hidden source pane, so the wide post-HTTP window is reachable. **→ Issue candidate (user-visible crash).** Fix: guard all `gridContexts_` structural access (find here + emplace at :487 + erase at :841) with a dedicated mutex, or snapshot the `unique_ptr<GridLiveContext>*` under that mutex before the fetch and skip the post-fetch find.
- **#1116 (17db90b3) · `scripts/dev/pre-ship.sh:283`** — strict-zone detection fails **OPEN** on Windows where `python3` resolves to the App-execution-alias stub. `command -v python3` succeeds for the stub, so the WARN fallback (:289) never fires; the actual `python3 -c …` (:285) then fails (exit 49 "Python was not found"), swallowed by `2>/dev/null … || true`, leaving `review_strict_zones` empty with no warning. A sub-60-line strict-zone diff (e.g. 30-line edit to `Source/Core/src/Sync/`) is then classified non-substantive and the review gate **N/A-passes** it (:338) — exactly the high-risk code the gate exists to force a review on. Empirically confirmed: `bash scripts/dev/pre-ship.sh --selftest` exits 1 here ("review gate passed a strict-zone diff with NO ack"). Internal-tooling (gate fail-open), not user-visible. Fix: probe a real `"$PY" -c 'import json'` invocation (not file existence), or fall back to a pure-shell `project.config.json` parse; emit WARN whenever zero zones AND cpp changed; wire the selftest into CI/SessionStart.

### MEDIUM
- **#1164 (12a5444f) · `scripts/dev/worktree-prune.sh:99`** — dirty-gate checks `git diff --quiet` + `--cached --quiet` but **not untracked files**. A merged worktree holding only untracked scratch is classed clean → shown as "would-reap" in dry-run and routed to REAP on `--apply`. No data loss (non-force `git worktree remove` at :105 refuses it, rc=128 → reap reports FAILED), but the dry-run candidate list misleads and `--apply` spuriously fails; the bats dirty-skip test only covers the staged path. Fix: treat non-empty `git -C "$path" ls-files --others --exclude-standard` as dirty=1; add a bats untracked-only case.
- **#1161 (b1cf9c0f) · `docs/harness/claude-code/hooks/guard-shared-tree.sh:79`** — linked-worktree exemption extracts one command-global `-C <path>` (`grep -oE … | tail -n1`) and exempts the **whole** command if it resolves to a foreign worktree. The :69 trigger matches if ANY mutating git op appears in a compound command, so `git -C <other-wt> merge && git reset --hard origin/develop` extracts only the worktree `-C`, resolves foreign, and exempts (:84) — letting the bare `reset --hard` in the **shared** tree escape the sibling-HEAD rug-pull guard. `tail -n1` also lets the last `-C` win. Mitigated: secondary/advisory guard (`guard-head-drift.sh` is the hard net; compound form uncommon). Fix: only exempt when EVERY mutating git op carries a `-C` to a non-integration worktree — split on `;`/`&&`/`||`/`|` and evaluate each segment's git op independently.
- **#1057 (d82b2b9f) · `tests/CMakeLists.txt:808`** — surviving comment claims the glob-vs-list guard (widened to `Plugins/` here at :810-812) is "Mirrored by `agents/scripts/core/check-test-list.sh`", but the bash mirror only scans `tests/Core/*.test.cpp` (:55 + :26 glob Core-only). No `Plugins/` coverage → an unreferenced `tests/Plugins/**/*.test.cpp` passes the local check and only trips at CMake configure. Doc-vs-code drift on a gate claim. Fix: widen `check-test-list.sh` to also scan `tests/Plugins/*/*.test.cpp`, or soften the comment to "mirror covers Core/ only".
- **#1053 (ecf911df) · `agents/scripts/core/test-gate-selftests.sh:55`** — `exposers()` scans scan-dirs **non-recursively** (`for f in "$root/$d"/*` + `[ -f "$f" ]` skips dirs), so any `--selftest`-exposing script in a subdirectory is silently excluded. `scripts/dev/local/` already holds 6 scripts. Latent fail-open in the meta-gate whose stated job is "a future gate physically cannot ship a pass-only selftest" — a selftest exposer one level deeper escapes entirely (none currently expose `--selftest`, so not yet triggered). Fix: recurse via `find "$root/$d" -type f \( -name '*.sh' -o -name '*.py' -o -name '*.bash' \)` (preserve unreadable-fail-closed + no-match suppression), or assert the top-level-only scope.
- **#1049 (15ac159d) · `Source/Core/src/Ui/AnnotateAnalysisUi_Window.cpp:209`** — the day→CL resolve launch (:196-218) is gated only on date-picker confirm + valid parse, **not** on `State().beforeClResolving`. Confirming a second date while the first server-wide `p4 changes -r -m 1 -s submitted //...` scan is in flight reassigns `State().beforeClFut` (:209), dropping the last reference to the unready `std::async` shared state — whose destruction **BLOCKS the UI thread** until the in-flight p4 scan finishes (the Pillar-2 freeze this code claims to fix). The mirror `P4ClPreview.cpp:45-56` explicitly detaches the pending future into `DetachedHoverFuts` before overwrite; this re-fire path omits the safeguard. **→ Issue candidate (user-visible UI freeze).** Fix: gate the launch on `!State().beforeClResolving`, or adopt the `P4ClPreview` detach-and-reap pattern.

### LOW (10)
- **#1168 (d147a685) · `agents/scripts/project/test-lint-rules.sh:503`** — `cmake-local-gate-ci-scope` is a greedy 80-line backward-window heuristic: any `ENV{CI}`/`ENV{GITHUB_ACTIONS}` token in the window sets `ci=1` and suppresses the finding even if it belongs to an unrelated earlier `if()` block → an unrelated CI-gated stanza upstream of a genuinely-unguarded knob-keyed `FATAL_ERROR` fails open. The `*'message(FATAL_ERROR'*` match (:498) also misses `message( FATAL_ERROR` with whitespace. Accepted cheap-heuristic per the plan's Risks; if precision matters later, scope `ci=` to the enclosing `if()/endif()` block and tolerate whitespace after `message(`.
- **#1160 · `agents/scripts/core/test-oob-label-impl.sh:68`** — `_label_is_implemented` treats any non-comment line that merely **mentions** the label as "implemented" — no check it is actually READ (no `$labels`/`gh pr view --json labels`/`has_label` context). A `*-out-of-band` label on a non-comment-but-non-reading line (a step `- name:`, an `echo`) falsely PASSES, weakening the prose-promise class the gate exists to catch; the `--selftest` only exercises a genuine reading line. Fix: require the label co-occur with a label-reading construct, or add a non-reading-line selftest fixture.
- **#1154 · `Source/Core/src/TicketFieldEditor.cpp:222`** — `LoadDurationSuggestions()` runs **every frame** the suggestions popup is open, on the ImGui render path: `ConfigManager::Load()` (memory-cached, no disk I/O, but takes a mutex + returns the full `TrackerConfig` by value — many string/vector members) then copies out the `DurationSuggestions` vector — per-frame heap-churn while the dropdown shows. Latent perf, scoped to popup-open frames; an existing backlog item already tracks `ConfigManager::Load()` render-path overhead. Fix: cache the list once on popup-open (per-widget `ImGuiStorage` / static refreshed on config-invalidate).
- **#1116 (17db90b3) · `scripts/dev/pre-ship.sh:289`** (twin of the HIGH) — the WARN "strict-zone detection skipped (the line threshold still applies)" only prints when `command -v python3` fails outright; on the common Windows-stub case (command -v succeeds, invocation fails) the operator gets **no** warning, so a silently-disabled strict-zone half looks identical to a genuinely non-substantive diff in the "gate N/A" output (:338). Fix: trigger the warning on the observable condition (`review_strict_zones` empty while `review_changed_cpp` non-empty), not on the `command -v` guard.
- **#1110 (4918b5e9) · `.github/scripts/mobile-emulator-smoke.sh:3` (+8 others)** — surviving code-comments reference the plan at `docs/plans/mobile-mvp-completion.md`, but this same PR archived it to `docs/plans/shipped/mobile-mvp-completion.md`. Stale path recurs in 8 more surviving comments: `build-and-test.yml:1214`, `mobile-emulator-smoke.yml:3`, `Tracker/TrackerHttpPure.h:7`, `AndroidApp/app/build.gradle:89`, `SmatchetActivityImeTest.java:15`, `robolectric.properties:2`, `test-android-openssl-failfast-bats.sh:13`, `tests/CMakeLists.txt:876`. Free-text comments (not gated links), so no gate breaks — misdirect a reader only. Fix: repoint each to the `shipped/` location.
- **#1101 (d806f863) · `agents/scripts/core/postmortem-owed.sh:400`** — the Core-cpp de-noise gate drops a flagged PR when `! pr_touches_core_cpp`; `pr_touches_core_cpp` runs `gh pr view … 2>/dev/null | grep -qE`, so on a transient gh/API/auth failure the pipeline emits nothing, grep returns false, the negation is true, and a **genuine** Core-cpp escape carrying an allow-listed trigger is silently suppressed (false negative — the direction the file's own :375/:381 comments call worse). Latent: advisory SessionStart nudge (never blocks merge), upstream `gh pr list` would usually fail in the same outage, and the fail-silent pattern matches sibling helpers. Optional hardening: capture `files=$(gh pr view …)`, treat empty as "no Core cpp" only when `$?`==0, else keep the PR.
- **#1085 (ecdc9332) · `agents/scripts/project/test-mobile-security.sh:130`** — `check_cmake` uses `if ! grep -Pzq …` for the `FATAL_ERROR` control-flow check; grep exits 2 on error (e.g. a `-P`/PCRE-less grep build), `! 2` is false, the block is skipped, and a missing fail-fast marker is silently treated as PASS — fail-open on a non-GNU grep. Guarded in practice (the workflow runs `--selftest` first). Fix: capture `rc=$?`, treat `rc>=2` as infra error (return 2); same for `OSSL_WARN_RE`/`grep -qF`.
- **#1085 (ecdc9332) · `agents/scripts/project/test-mobile-security.sh:32`** — documented self-disable hole (:32-37): the merge-gate poller enforces absent-check presence only for **required** contexts (`$reqAbsent` over `$reqNames`, merge-gates.sh:350); an absent non-required allow-listed check passes. A PR that deletes/renames `mobile-security.yml` in its own diff makes "Android security gate" absent, so the poller no longer blocks the #1067/#1068 regressions. Acknowledged + backlogged out-of-WS1-scope, visible in diff review. Fix: poller-wide present-assertion for allow-listed meant-to-block checks (already tracked).
- **#1060 (703261fc) · `agents/scripts/core/test-markdown-links.sh:80`** — the `--selftest` re-invokes the real diff-scope scan and asserts only a non-zero overall exit: if any other changed/untracked markdown already has a dangling link the selftest passes for the wrong reason; conversely the :87 "clean untracked file must PASS" assertion false-FAILs whenever the ambient tree has a pre-existing dangling link. Not hermetic — depends on working-tree cleanliness; errs toward false-FAIL not false-PASS of the production gate (tooling robustness only). Fix: scope the re-invocation to a temp dir / synthetic fixture (drive the python scanner over fixture files, or env-gated `TARGETS` override).
- **#1037 (5eefb9c2) · `docs/plans/active/build-quality-velocity-hardening.md:83`** — broken markdown cross-ref: `[tracker-result-migration.md](docs/plans/tracker-result-migration.md)` is missing the `active/` segment; the file lives at `docs/plans/active/tracker-result-migration.md`, so the link resolves to a nonexistent file. Fix: repoint to `docs/plans/active/tracker-result-migration.md`.

**Fully superseded (7, no review surface):** #1153, #1090, #1077, #1051, #1042, #1041, #1039 — every introduced line was changed/removed by a later PR; excluded by construction.

## Batch 6 — newer #952–#1028 (64) + older #601–#542 (50) (115-PR sweep, 2026-06-08)

Coverage: **115 reviewed — 20 with findings, 82 clean, 13 fully superseded.** Net: **1 CRITICAL, 6 MEDIUM, 13 LOW.** Survivor-filtered against origin/develop, so every finding is current (already-fixed code excluded by construction — see the Remediation log above for prior-batch fixes).

### CRITICAL
- **#565 (1a9db3b0) · `Source/Core/src/Ui/AnnotateAnalysisUi_Preferences.cpp:212`** — the `colorRow` lambda in `DrawAnnotateCacheAndColors` (render path) calls `ConfigManager::SaveAnnotateAnalysis(cfg)` **synchronously on each color-edit commit** — takes the shared config RMW mutex, re-reads merged config, JSON-encodes, atomically rewrites `smatchet_config.json` on disk, all on the UI thread. The rest of the file deliberately routes this off-thread via `PersistAnnotateCfg`/`ScheduleAnnotateConfigSaveDetached`; line 212 is the survivor on the sync path. **→ Issue candidate (joins the #611/#761 sync-I/O cluster).** Fix: `PersistAnnotateCfg("edit_color")`.

### MEDIUM
- **#975 (d304eab3) · `Source/Core/src/AppController_CatalogAndFieldEdit.cpp:681`** — `EnsureProjectComponentsLoaded` inserts the `projectComponentsInFlight_` marker into the kick-time focused context but the worker re-resolves `fieldCatalog()` at completion (L697) and erases/writes into the *completion-time* focused context. A pane focus-switch mid-fetch **permanently leaks the in-flight marker** in the original pane → that project stuck "Loading components…" forever until restart (NOT self-healing, contra the debt note). Fix: capture the kick-time `GridLiveContext*` and write through it.
- **#967 (c742847d) · `docs/harness/claude-code/hooks/guard-head-drift.sh:107`** — NEW fail-open: `all_git_ops_target_safe_worktree`'s `grep -oE` trailing boundary `(\$|[;&|)[:space:]])` is *consuming*, so `git -C /safe-wt commit;git commit` (single separator, no space) — grep eats the `;`, the second bare `git commit` loses its leading boundary and isn't extracted → exemption validates only the safe op → deny skipped → **bare commit to develop slips through**. Distinct from the shapes in tooling.md:54. Fix: zero-width trailing boundary (`grep -oP` lookahead) or normalize separators to whitespace first; add a bats case.
- **#978 (d68f0f32) · `docs/plans/active/subagent-eval-agentic-coverage.md:72`** — Phase-3 schema design uses a JSON-schema `else` clause but `validate_schema.py` only resolves `if`→`then` inside `allOf` (no `else`); implemented verbatim, the single-shot branch is never evaluated → `expectedFindingCount` silently un-enforced (false-pass), contradicting the plan's own "grill-verified" guarantee. Fix: express as two `allOf` if/then branches, or extend the validator.
- **#976 (cdc7b8dd) · `docs/agent-rules/process-rules.md:172`** — the Claude Code row prescribes `autoCompactEnabled: true` + numeric `autoCompactWindow` in `~/.claude/settings.json`; these keys appear nowhere in the repo/harness docs and don't match Claude Code's known settings schema (auto-compact isn't a numeric knob) → an operator adds a no-op key (~75% confidence). Fix: verify vs the real schema; if absent, describe the built-in auto-compact as advisory (like the Codex/Cursor rows).
- **#572 (d4a31a75) · `tests/ui/annotate_prefs_persist_flow.test.cpp:240`** — host config restore not failure-safe: the test writes a seed into the REAL config file then restores via `EndConfigSnapshot` only after all asserts; `IM_CHECK` expands to `if(!res) return;`, so a failing check skips the restore → leaves the host's real config polluted (violates the file's "byte-identical" guarantee; compounds across variants). Fix: RAII scope-guard restore on every return path.
- **#548 (e9688342) · `docs/adr/0003-github-as-itrackerclient.md:9`** — #548's link-retarget points two Context links at `../plans/active/agentic-flow-implementation.md` + `agentic-triage-flow.md`, both hard-deleted by the agentic ripout 8 days prior → 404 inside an Accepted ADR. (+2 LOW same pattern in ADR-0004.) Fix: repoint to `docs/plans/shipped/github-tracker-backend.md`.

### LOW (13)
- **#968 (d01ca584) — in the historical-review tool itself:** `historical-review-survivors.sh:157` hardcodes 40-hex SHA-1 in the blame-porcelain parser → on a SHA-256 repo every file reads as FULLY SUPERSEDED (false-clean; latent); `:173` `--context N>0` range-merge vs ±N pad can overlap → context printed twice + backwards `@@` numbers (cosmetic). Fix: accept 40-or-64-hex; merge when `gap ≤ 2*ctx+1`.
- **#1017 (508277ba)** `SmatchetResult.h:142` — `Result<T,E>` not exception-safe like sibling `Optional<T>`: assignment ops set `ok_` before a throwing placement-new + the private default ctor leaves no member; a throw → `Destroy()` runs a dtor on unconstructed storage (silent UB). Narrow (current types have noexcept moves) but a foundational reusable primitive. Fix: construct-then-commit + a constructed-member guard.
- **#962 (472c2de3)** `GridPane.h:13,72` — header still states the Slice-2 model ("exactly ONE GridLiveContext is live … until Slice 3"); Slice 3 shipped (#986) making every visible pane live; sibling headers updated, this central data-structure header missed → misstates the liveness/thread-safety model. Doc-only.
- **#578 (6eab3dbc)** `SmatchetUI.cpp:459` — `ParseImGuiHotkey` re-tokenizes the config string into a heap `vector<string>` every frame on the render path (Pillar-1 per-frame alloc); negligible but trivially memoizable.
- Doc-drift / broken-ref / stale-status cluster (all LOW, doc-only): **#1010** plan "4 PRs" miscount; **#980** `merge_gates.bats:516` ordering-coverage gap (fixture has "too many files" so the arm-order invariant is never exercised); **#952** golden-image `SMATCHET_TEST_DEFAULT_IMGUI_THEME` knob documented but unread in source; **#590** DeepSeek buffer-shape comment contradicts the sizes; **#563** `p4-annotate.md:18` duplicate `- annotate` trigger from the rename; **#555** `test-doctor.sh:91` strip-dir only removes first PATH copy; **#549** `smatchet-merge-watcher.md:90` `../guides/` link + `STRUCTURE.md:68` stale "157" (now 192) purity count; **#545×3** stale `docs/plans/active/applied/` refs in coverage-gate.yml + 2 Lua test comments; **#542** `PORTABILITY.md:34,41` stale `p4-blame` (→ `p4-annotate`).

### Prior-findings re-verification (this request's "what's been fixed")
The **Remediation log (top of file)** already records the prior-batch fixes done in the 2026-06-08 pass — confirmed independently here: **#918, #834, #630, #657, #722, #755, #853, #909/#855, #940, #670 are FIXED** at origin/develop; **#854, #611, #761, #732, #767, #892, #671, #948** are deferred to GitHub Issues (user-visible), still alive. Batch 6's survivor filter excludes all of those fixed lines automatically, so no batch-6 finding re-reports a remediated one. (Note: the new **#565** CRITICAL is the same sync-I/O class as the deferred #611/#761 — fold it into that Issue.)

## Batch 5 — PRs #602–707 (100-PR workflow sweep, 2026-06-07) — FINAL BATCH

Coverage: **100 reviewed — 13 with findings, 73 clean, 14 fully superseded.** Net: **1 CRITICAL, 1 HIGH, 3 MEDIUM, 8 LOW.** Sweep stopped here per user.

### CRITICAL
- **#611 (246b5238) · `Source/Core/src/Ui/SmatchetToolbarUi.cpp:124`** — `RefreshTrackerAppendCache()` calls `ConfigManager::LoadPersistentViewsFromDisk()` (sync ifstream + JSON parse under `GetIoMutexRef()` + OS `ScopedFileLock`) from `RenderBar()` (`:142`) on the ImGui render path → Pillar-2 CRITICAL. Memoized (backend-change/startup/post-save), but those frames block + can stall under contention with a concurrent `SavePersistentViewsToDisk`. **→ Issue candidate.** Fix: source the per-tracker append from in-memory config, or hoist off-thread (`std::async` + per-frame poll) on backend-change.

### HIGH
- **#671 (2533b9ab) · `Source/Standalone/CliCommandRunner.cpp:915`** — `SpawnAndRunHandleAsync`'s invalid-JSON `catch(...)` returns `kExitHandler` **without** sending `app.quit`, breaking its own documented invariant (`:845`) that the caller relies on. Result: when the scenario result file exists but isn't valid JSON, the spawned ephemeral instance is never told to quit → **orphaned subprocess holding a TCP port**. The other two failure branches do send the best-effort quit. Fix: POST the same `app.quit` before the `return` (mirror `:887-890`).

### MEDIUM
- **#670 (333a133f) · `Source/Core/src/Tracker/JiraIssueMutation.cpp:81`** — `FindTransitionIdInArray` applies its match priority **per-transition** instead of globally (contradicting its own doc: id → status-name → transition-name). An earlier transition whose *name* matches the requested status but whose `to.name` differs is returned ahead of a later transition that actually leads to the requested status → **the issue moves to the WRONG status in the user's Jira** (WARN logged, but wrong transition still executes). **→ Issue candidate (user-visible external mutation).** Fix: two-pass — exact id/status-name match across all transitions first, transition-name fallback only if none.
- **#630 (8a824ef7) · `docs/guides/imgui-draw-pattern.md:91`** — Rule 4's audit command greps `Source/Core/src/Smatchet*Ui*.cpp` but all UI sources are under `Source/Core/src/Ui/` → matches **zero** files, silently implying "no `static` locals to extract", defeating the rule. Fix: `Source/Core/src/Ui/Smatchet*Ui*.cpp`.
- **#620 (ac7dcb58) · `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md:77`** (+85,112-114) — instructs agents to run `test-backlog-counts.sh --fix` to sync a § Index count column; the script was rewritten 2026-06-03: no `--fix` flag, the count column was deliberately removed, and the gate now **FAILS** if a count column is re-added. Following the doc trips the guard (inverse of documented effect). Fix: drop `--fix` + count-sync; state counts are on-demand via `--list` and a count column must never be re-added.

### LOW (8)
- **#664 (9765cf4a)** `SmatchetAiAssistantUi.cpp:232` — hash-collision recovery branch uses `emplace` (no-op on existing key) → returns stale plan + duplicate insertion-order key + byte-gauge drift. Unreachable (64-bit FNV collision) but the guard is a no-op. Use `[]=` / erase-then-insert.
- **#663 (57769145)** `test-ui-jira-deterministic-backend.sh:75` — `passed=0 failed=0` (filter matches nothing / all skipped) exits 0 → silent false-pass bucket-E gate. Add a positive-test floor.
- **#657 (bf921a6b)** `docs/CONTEXT.md:77` stale line refs `AppController.cpp:574/:595` (now 826/880). Use symbol-only refs.
- **#653 (9ae9c24e)** `tests/agent-eval/code-review/cr-dpapi-secret-loss.json:37` — fixture shows 3 unguarded data-loss sites but `expectedFindingCount=1`; a more-thorough prompt scoring 3 would regress to 0.0 (false gate fail). Trim to 1 site or set count=3.
- **#643 (feb9d903)** `TrackerFieldCatalogPure.cpp:195` dead `is_number_unsigned()` branch (unreachable after `is_number_integer()`); latent narrowing via `get<long long>()`. Reorder or drop.
- **#640 (bdd2644a)** `subagent-eval-flywheel.md:9` + **#623 (0772d4be)** `bug-report-font-redaction-censor.md:5,9,96` + earlier batch's #747 — same broken AGENTS.md `§ Plan *` cross-links (restructured to `§ Process rules § Plan-doc family`) + a stale in-flight-branch claim + a `log-a-bug-github.md` link now under `shipped/`.
- **#610 (f938ac5b)** `verify-cr-reply.sh:20` stale usage-example path `scripts/dev/verify-cr-reply.sh` (moved to `agents/scripts/core/`).

**Cluster (confirms batches 3–4):** Pillar-2 **sync-I/O-on-render survived decompositions** — now #611 (CRITICAL) joins #761/#732/#767/#892. Strong candidate for a single targeted audit + the accepted off-thread/`MarkPrefsDirty`/snapshot-on-open patterns. Plus the **bucket-E/gate false-pass-on-0-tests** recurrence (#663 + batch-4 #719). #670 is the one genuinely user-facing *correctness* bug (wrong Jira status).

## Batch 4 — PRs #708–808 (100-PR workflow sweep, 2026-06-07)

Coverage: **100 reviewed — 14 with findings, 75 clean, 11 fully superseded.** Net: **1 CRITICAL, 4 HIGH, 5 MEDIUM, 12 LOW.**

### CRITICAL
- **#761 (8b5a39f1) · `Source/Core/src/Ui/AnnotateAnalysisUi_Window.cpp:191`** — `DrawCallstackProcessControls` (on the render path) synchronously runs `p4 changes -r -m 1 -s submitted //...@start,end` via the blocking `P4RunCommand` subprocess on the UI thread, no cue, when the "or day" date picker is confirmed. Server-wide depot range round-trip can exceed 100 ms → freeze (Pillar-2; checklist explicitly names p4 as must-be-off-thread). Lone on-thread blocking call in a file that offloads everything else; relocated verbatim by #761's decomposition, still alive. **→ Issue candidate.** Fix: `LaunchBackgroundTask` + `PostToMainThread`, "Resolving CL…" status.

### HIGH
- **#732 (02eb69c8) · `SmatchetPreferencesUi_Templates.cpp:78`** (+90,102,127,167,179,191,216) — duration-suggestion & work-log-template sub-tabs call `SaveDurationSuggestions`/`SaveCommentTemplates` → `ConfigManager::Save` (full read-modify-write + DPAPI encrypt + disk write under 2 mutexes) **synchronously on every reorder/delete/add click** on the render thread. Sibling sub-tabs already use deferred `MarkPrefsDirty(d)`; these two are the survivors on the sync path. Fix: mutate `d.cfg.*` + `MarkPrefsDirty(d)` (or `ConfigSaveWorker`).
- **#784 (0c62b21c) · `agents/scripts/core/postmortem-owed.sh:54`** — `has_entry()` dedup regex requires the `PR ` prefix, so it matches only the FIRST PR in a multi-PR ledger entry (`## … PR #N, #M …`); comma-joined trailers (`#906/#907/#908`, `#774/#776/#778` …) are re-flagged "postmortem owed" every SessionStart despite an existing entry. **This is the source of the recurring postmortem-owed nudges.** Fix: match a bare `#N` token regardless of `PR ` prefix.
- **#789 (6987b7d5) · `scripts/dev/pre-ship.sh:126`** — markdown-lint step hardcodes `python3` (bypasses the repo's `resolve_python()`), so on Windows local the store-stub `python3` (exit 49) makes `pre-ship` print "FAIL — fix the markdown findings" even when docs are clean. Defeats the local half of the gate. Fix: resolve a working interpreter (`python3`/`python`/`py`) and fail loudly only if none runs.
- **#807 (fe06fa23) · `README.md:62,91,96`** — onboarding "one-command build" `scripts/dev/build_and_run.ps1` was relocated to `scripts/dev/local/` (file not found at origin/develop) AND the README claims it auto-bootstraps vcvars ("no Developer Prompt needed") but the chain never invokes `with-msvc.ps1`/vcvars → `cl.exe not found` from a plain shell. Fix: correct the path + the bootstrap claim (or wire the bootstrap). (2 MEDIUM, grouped.)

### MEDIUM
- **#767 (802402c3) · `SmatchetViewsDashboardUi_widgets.cpp:276`** — `ListCachedProjects()` (ifstream + JSON parse + v3 migrate + sort under global mutex) called **every frame** the project-pill popup is open. Sub-frame today (16-entry cap) + matches the accepted sibling convention (`SmatchetProjectPicker`, `SmatchetPreferencesUi` — see #892), so MEDIUM. Fix: snapshot on popup-open into `UiDrawSession`.
- **#746 (4d166612) · `scripts/dev/pre-ship.sh:93`** — comment claims "(staged, unstaged, committed)" but the bare `git diff` captures unstaged only; a staged-never-committed-no-further-edit file is clang-format/lint-skipped (silent false-pass). Fix: `git diff HEAD` or add a `--cached` pass.
- **#719 (78e19958) · `scripts/dev/test-ui-funcsize-window-render-smoke.sh:69`** — a run with `passed=0 failed=0` (filter matches nothing after a rename) exits 0 green; only `FAILED!=0` is checked. Zero-coverage green on a no-visual-validation gate. Fix: require positive count.

### LOW (12)
- **#788 (19779297)** `test-backlog-counts.sh:53` redundant `|| echo 0` double-emits for empty categories; `:61` regression-guard regex omits `debt`; `AGENT_SELF_IMPROVEMENT.md:149` § Index table omits the `debt` row.
- **#789** `md_lint.py:38` MD028 no fence tracking (latent false-positive on a fenced blockquote example); `:27` `git ls-files` returncode unchecked → silent clean from a non-repo dir.
- **#759 (0268a29b)** `with-msvc.ps1:85` when the pinned toolset isn't installed, falls back to first install but still forces `-vcvars_ver=<pin>` → vcvars fails silently, build runs in non-MSVC env (opaque `cl.exe not found`); contradicts the "exit 2" contract. (Related to the toolset-pin friction in this very session.)
- **#755 (1d88d25c)** `test-rig.md:55` names `AppControllerDepsAdapter.cpp` (renamed to `GridContextDepsAdapter.cpp` by #945).
- **#747 (ec0d1770)** `agent-kit-productization.md:5` cross-links 5 AGENTS.md subsections that no longer exist (restructured to nav-only).
- **#722 (6990b8bf)** `docs/CONTEXT.md:13,14` broken header paths — missing `Tracker/` subdir (`LabelEditDiffPure.h`, `GitHubClientHelpers.h`).
- **#709 (777dc48d)** `SubprocessCapture.cpp:475` iterator-pair `std::string(char*, const char*)` is a hard compile error on the non-glibc/BSD fallback path (unreachable on Win/Linux glibc matrix, but malformed C++); `:540` POSIX pump FD_SETs an EOF'd fd forever → 100% CPU spin if a child closes one pipe but keeps running (test-only POSIX path).

**Cluster:** Pillar-2 **sync-I/O-on-render-thread survived decompositions** — #761 (p4, CRITICAL), #732 (config save, HIGH), #767 + batch-3 #892 (cache read, MEDIUM). Worth a targeted audit + the accepted `MarkPrefsDirty`/`LaunchBackgroundTask`/snapshot-on-open patterns. Also a **gate-fails-open / false-pass** recurrence (#719/#746/#789/#788) consistent with batch 3's #834/#918.

## Batch 3 — PRs #809–925 (100-PR workflow sweep, 2026-06-07)

Ran via the `historical-review-sweep` workflow (100 code-review agents, concurrency-capped, structured output). Coverage: **100 reviewed — 20 with findings, 65 clean, 15 fully superseded.** Net: **5 HIGH, 4 MEDIUM, 14 LOW.**

### HIGH
- **#854 (7d7b2f01) · `Source/Core/src/Sync/OfflineQueueService.cpp:617`** — scalar conflict-resolve writes the chosen DISPLAY string verbatim into the payload key, but every ID/object/array-valued field (single/multi-select, status, priority, issuetype, user, component, cascading, labels) stores an object/array payload (`{"id":"3"}` …). Resolve clobbers `{"priority":{"id":"3"}}` → `{"priority":"Low"}`; `ReplayOneFieldEdit` PUTs it → Jira 400 → retries → dead-letter → **user's resolved offline edit silently lost** (despite "edit re-queued" toast). "Use Mine" equally broken. Test uses a bare-string payload so CI never sees it. **→ Issue candidate (data-loss).** Fix: route scalar "Use Mine" through the unchanged-payload path; for "Use Theirs"/"Save" rebuild via `BuildFieldPayload/BuildValue`; only overwrite with a bare string when the existing value is itself a string. Add an object-payload test.
- **#892 (1e085193) · `Source/Core/src/Ui/SmatchetPreferencesUi.cpp:324`** — `DrawTrackerRecentProjects` calls `FieldCatalogCache::ListCachedProjects()` **every frame** the Prefs→Tracker tab is open → synchronous `ifstream` read + full JSON parse + schema migration + sort on the UI render thread (Pillar-2 violation). Slow/locked/large cache file stalls the window. (Relocated by the refactor, but line 324 is the surviving call.) **→ Issue candidate (UI freeze).** Fix: read+filter once on tab-open, cache in `UiDrawSession`, refetch only on backend change / Forget; or load on a worker.
- **#834 (2fd9ef33) · `scripts/dev/coverage.sh:168`** — OCC exclude patterns use the bare token `Ui` as an unanchored case-insensitive substring → excludes far more than `Ui/`: the whole strict-zone `Source/Core/src/Commands/Builtin/` (`Builtin`), `CommandPaletteUi.cpp`, `Commands/Scenarios/UiTestScenario.cpp`, `AiContextBuilder.cpp` (`Builder`). The **blocking** coverage gate's 67% baseline is computed on a too-small surface; strict-zone code uncounted; tests there can't move the gate. Fix: anchor to a dir boundary (`Source*Core*src\Ui\`), re-verify the captured file list, re-baseline.
- **#918 (c2287c02) · `scripts/dev/coverage-delta-gate.sh:79`** — classifier `'*'*) return 0` (meant for block-comment continuations, already handled by the state machine) instead only fires on real statements starting with deref/indirection: `*out = compute();`, `*it = next();` → classified no-runtime-surface → a diff of output-pointer writes is falsely auto-EXEMPTED from the required test-delta gate (false PASS, unsafe direction; violates the file's own CONSERVATIVE contract). Fix: drop the redundant comment-continuation cases or tighten `'*'*` to a true continuation shape.
- **#919 (1d83a108) · `tests/bats/merge_watcher.bats:354`** — the two new `handle_pass` tests (enqueue-on-queue, merge-when-no-queue — the entire point of #919) FAIL on Windows: (1) the gh stub selector `case "$2 $3"` no longer matches now that #919 moved the discriminator to `$1/$2` (`gh pr merge`/`gh repo view`), falls through to `exit 0`; (2) the `PATH=$STUB_BIN gh` trick is documented non-functional on Windows (native `shutil.which` skips extensionless stubs → real `gh.exe` runs). So merge-queue-safety logic has **zero working coverage**; CI misses it (no bats on the Windows runner). Fix: drive the 3 tests through the `mw.squash_merge_pr`/`_gh_owner_repo` monkeypatch seams; if keeping a stub, name it `gh.cmd` and use `case "$1 $2"`.

### MEDIUM
- **#918 · `coverage-delta-gate.sh:77`** — `'/*'*) return 0` exempts open-and-close comment lines with trailing code (`/* note */ launchTask();`); mirror hole at `:185` (`*/` + code → `continue` drops the statement). Both silently exempt real surface. Fix: strip the comment span, classify the residual code.
- **#814 (c0e4f4e5) · `agents/scripts/project/migrate-bugs-to-issues.sh:12`** — usage header claims `--apply` "create Issues + move debt + prune bug.md" but apply only creates Issues (debt/bug.md reconciliation is manual by design). Fix: correct the header.
- **#813 (f3652350) · `project.config.json:146`** — `governance.loop_mode` is a dead knob: `_doc` (+ AI_POLICY.md, ship-loops.md) claims it's the SessionStart default, but the only consumer `clear-session-context.sh` reads only `SMATCHET_LOOP_MODE` env and hardcodes `in` else. Harmless only because both equal `in`. Fix: read `governance.loop_mode` as the unset-env fallback, or reword the docs to "advisory/unconsumed".
- **#810 (1ec969d5) · `docs/agent-rules/issue-triage.md:68`** — documents the `[issue-propose]` line as `(P<k>, area:X)` but `issue-sweep.sh:111` emits priority only `(P0)`/`(P1)`, no area. Fix: drop `area:X` from the doc or append the area label in the emitter.

### LOW (14)
- **#917** `test-lua-mirror-smoke.sh:58` `mapfile` breaks on macOS bash 3.2 (no summary line → silent gate degrade); not on CI/Win targets.
- **#915** `check-test-list.sh:29` + `tests/CMakeLists.txt:608` — substring (not path-boundary) match → a basename that's a suffix of a referenced file is falsely "referenced" → uncompiled test (the exact false-green this guard prevents).
- **#914** `infra.md:31` `test-delta-test-light-exemption` still `open` but shipped (#918) → mark applied/archive; leave sibling `pr-burst-guard` open.
- **#913** `guard-shared-tree.sh:51` doesn't exempt `-C <worktree>`-targeted git ops (false-deny) unlike the parity helper in `guard-head-drift.sh`; `SMATCHET_ALLOW_SHARED_SWITCH=1` overrides.
- **#911** `test-config-globs.sh:72` process-substitution helper crash isn't caught by `set -e` → loop reads 0 globs → PASS (fails OPEN vs documented fail-CLOSED). Capture into a var + check status.
- **#909** `build.md:31` broken ref `scripts/dev/build_and_run.ps1` → `scripts/dev/local/build_and_run.ps1`.
- **#908** `CMakeLists.txt:664` sol2 RE-RUN CLEANUP group is dead (PRIMARY patch FATALs first on a double-appended tree); harmless (SHA-pin re-fetches fresh) but comment is wrong. Delete the dead group or reorder + fix comment.
- **#888** `docs/harness/pi/README.md:57` overstates "read-only enforced by tool scoping" — read-only agents with `shell` get `bash` (can write). Soften wording.
- **#887** `agents/_shared/workflows/pre-merge-review.js:106` `filter(Boolean)` destroys positional identity → on partial reviewer failure the judge mis-attributes code-review vs security-review punch lists. Reference `reviews[0]/[1]` directly.
- **#872** `followup-due-nudge.sh:186` unguarded `"${warns[@]}"` under `set -u` (bash<4.4) in the due-path; mirror line 195's `:-` guard. Dormant on supported toolchains.
- **#855** `build.md:19` broken refs `scripts/dev/build_and_run.ps1`/`build_standalone.ps1` → under `scripts/dev/local/`.
- **#853** `docs/adr/0016-…md:30` stale line-pin `OfflineQueueService.cpp:788` (write moved to :1008). Reference the symbol instead.
- **#814** `issue-sweep.sh:75` relabel verdict fires on missing-priority but apply only adds `bug` → perpetual no-op RELABEL inflating the acted count; `:7` header lists verdicts (`mirror-then-close`/`flag-stale`) the script never emits.
- (Plus the #810/#813/#814 MEDIUMs above each had adjacent doc-vs-code drift.)

**Recurring classes:** (a) gate scripts failing OPEN / false-exempting (#834/#918/#911/#915) — highest-value, undermines blocking gates; (b) stale doc line-pins + moved-script refs (#909/#855/#853/#914/#810/#813); (c) `set -u`/portability latent shell bugs (#872/#917). The gate-false-pass cluster (#834/#918) is worth prioritising — they let untested/uncovered code merge green.

## Batch 2 — PRs #926–946 (swept 2026-06-07)

20 PRs (#946,945,944,942,941,940,939,938,937,936,935,934,933,932,931,930,929,928,927,926). Net: **0 HIGH, 2 MEDIUM, doc-drift LOW cluster.** Fully superseded (nothing alive): #941, #936, #929. Clean: #946, #945 (faithful behaviour-identical extraction — destruction order + ADR-0012 atomics verified), #944, #942, #939, #933, #927, #934.

### MEDIUM
- **#935 (e3996308)** — `docs/agent-rules/merge-gates.md:10` quotes the meant-to-block allow-list regex as `Coverage|Sanitizer|Bucket-`, but live `merge-gates.sh:365` is `Coverage|Sanitizer|Bucket-|Perf PR-fast` (the 4th pattern added later by #942). A reader concludes a red `Perf PR-fast` is non-blocking when it actually blocks — dangerous doc-vs-code drift on a merge-gate. Same omission (LOW) at `merge-gates.sh:9` header comment and `AGENTS.md:49`. **Fix:** append `|Perf PR-fast` (or soften to "e.g.") at all three sites; consider a selftest asserting the regex literal mirrors into the docs.
- **#928 (394746a8)** — `Source/Core/src/Tracker/CONTEXT.md` documents as *live data-model facts* terms that exist nowhere in `Source/` on develop: `:43-47` `TrackerActivityEntry` + `GroupMemberCache`/"group roster" (Slice 2 never landed, gated behind unshipped multi-grid), and `:26` claims `ITrackerCollaboration` "handles ... per-user activity" but the interface has no `FetchUserActivity` (the PR's own plan says no activity endpoint exists). Canonical leaf-doc describes vaporware → readers chase non-existent symbols. **Fix:** mark both as planned/forthcoming or revert the CONTEXT.md additions until the backend slice ships.

### LOW (notable)
- **#940 (af465eb8)** — `docs/adr/0018-multi-grid-pane-contexts.md:6` broken ref `docs/plans/multi-grid-tabs.md` → should be `docs/plans/shipped/multi-grid-tabs.md`; `:3` status still `proposed` though Slice 1 shipped; stale `AppController.h` line citations in the design addendum.
- **#937 (b5716262)** — `scripts/dev/perf-run.sh:152` inline JSON validator checks `data.rows` then top-level `rows`, inverted vs `perf-compare.py extract_rows()` (top-level first); on a malformed file carrying both, perf-run.sh PASSes while perf-compare reads `[]` → false PASS. Match the order.
- **#931 (27e9e428)** — `postmortems.md:269` "### Filed as … (P1, decision-pending)" stale (resolved option B, #933, per RESOLVED note above it); stale `merge-gates.sh:345` line citation.
- **#932 (5f2dd18b)** — `build-quality-velocity-hardening.md:196` status block lists #8/#13 "Parked" but impl-log (:175) records "UN-PARKED → GATE ARMED"; impl-log missing bullets for #920/#925/#926.
- **#930 (7531a53c)** — `session-guard-agnostic.md:75-78` nested-backtick markdown breaks the Perf-gates `N/A` block rendering.
- **#938 (09f4c791)** — `TicketSyncService.test.cpp:427` mislocated comment; `SpinUntil` 400 ms cap is a latent flake-watch (5 new dependents).
- **#926 (c9f0c9dc)** — `data_dependent_windows_smoke.test.cpp`: stale Views-Dashboard probe comment, `"SMAT-1"` vs `"SMAT-TEST-1"` comment mismatch, and `app==nullptr` logs SKIP but records PASS (vacuous-green if harness fails to boot) — `IM_CHECK(app!=nullptr)` would fail loudly.
- **#942 (3e381a8c)** — informational only: armed relative gate inert on 3/4 scenarios (calls<min_baseline_calls) + warmup-contaminated `emaAvgMs` baked into approved baselines (unused by current gate). Calibration-phase state, not a defect.

### Note on doc-drift recurrence
Several findings (#935, #931, #932, #934, #940) are the same class: a later PR changed code/status and left mirror docs (allow-list, postmortem status, plan status, ADR status) stale. Candidate standing gate: mirror-literal selftests + a "doc status vs shipped" check. (#934 itself flagged the #942-induced AGENTS.md staleness — same root as #935.)

## Batch 1 — PRs #947–951 (swept 2026-06-07)

Survivor coverage (alive/introduced at origin/develop): #951 610/610 · #950 1/1 ·
#949 ~305/306 · #948 690/690 · #947 281/284. Net: **1 HIGH, 0 other.**

### PR #948 (2e9a7fbf) — multi-grid Slice 1b: tickets_v2 namespacing migration
- **HIGH · still alive** — `Source/Core/src/AppController.cpp` `InitConfig` migration block (~L1214-1220): the one-time tickets_v2 copy migration stamps legacy rows with `NormalizeViewsBackendKey(backendType)` (the `Initialize` **param**), but the live read/write key is re-derived in `InitBackends` (`AppController.cpp:1344`) from `ConfigManager::Load().TrackerType` — which ignores the ephemeral `--backend-type`/`-b` CLI override (`StandaloneAppBootstrap.cpp`) and the embedder's `options.BackendType` (`SmatchetImGuiHost.cpp:636`). Launch e.g. `Smatchet -b Plane` once on a pre-1b DB whose persisted tracker is Jira → migration copies Jira rows into `tickets_v2` under `"Plane"`, **consumes the `cache_meta` flag permanently**, while the live path reads under `"Jira"` (empty): legacy cache stranded + wrong namespace polluted, never re-migrated.
  **Fix:** run the migration with the resolved live key *after* `InitBackends` re-stamps it (`focusedContext().CacheBackendKeyCopy()`), exactly as `RecreateLocalCacheDatabase` already does (`AppController.cpp:2132`). The 1218-1219 comment's "fixtures use fresh DBs" rationale covers only the env-factory case, not this CLI/embedder one.
  **Route:** user-visible (cached tickets vanish on upgrade for non-default backends) → GitHub Issue candidate (ADR-0014). Originally surfaced as CR-948-1 in the live PR review; this confirms it is **still unfixed on origin/develop**.

### PR #947 (7835dba3) — guard-head-drift worktree git -C + PowerShell
- Clean (no NEW bug in the surviving set). CR-947-1 (trailing-boundary regex) already **fixed by #956** → correctly excluded. CR-947-2 (space-in-`-C`-path false-block, fail-safe) already backlogged in `tooling.md`.

### PR #949 (f7411db7) — perf: per-scope p99Ms in snapshots
- Clean. Percentile math (`ceil(0.99n)` rank, in-bounds for all n), ring wraparound/bounds, and cold-path locking all verified correct on the surviving lines. (Some #949 perf lines were re-attributed to the #963 100 Hz change and excluded.)

### PR #950 (fef198e4) — config strict=false
- Clean. Sole survivor `project.config.json` `branch_protection.strict: false` — valid JSON, matches the documented merge-throughput decision; required-contexts lists unchanged.

### PR #951 (258116f2) — multi-grid Slice 1c: pending-queue BackendKey + replay
- Clean. All SQLite column↔bind index orderings verified after the inserted `backend_key`; migration idempotent/transactional/empty-key-guarded; replay filter never replays against a wrong backend nor drops rows. (Earlier live-review CR-951-1 was a *verify* item about the dead-letter restore UI path; the surviving cache path `RestoreDeadPendingCreate` re-queues under the original key correctly.)
