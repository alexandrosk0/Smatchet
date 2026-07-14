# Plan — Backlog PR roadmap (open-entry → PR grouping)

> **Slug**: `backlog-pr-roadmap` (matches this file's basename without `.md`).
>
> **Status**: `active` — a living tracker, not a single-feature plan. **Substantially shipped** as of the 2026-06-20 campaign: 17 of 23 PR groups landed (#1502-1518); 6 were then `deferred` (visual / hardware / decision-gated). A **2026-07-14 validity re-sweep** (this branch) re-verified every deferred row against `develop`: 3 of the 6 have since shipped in follow-up PRs (PR-10 all-but-one-scenario, PR-11, PR-18), PR-13b's bug fixes landed and PR-13c was **closed as unsound** (see § Deviations), leaving **3 genuinely-open groups** — PR-14 (coverage ramp, measurement-gated), PR-19 (DX12 readback, hardware-blocked), PR-21 (per-pane catalog, grid behavior) — plus one bucket-E residue (PR-10 `user-info-window`). Each PR group's row carries its `shipped #N` / `deferred — <reason>` status. This file flips to `shipped` and moves to `docs/plans/shipped/` only once the roadmap is exhausted or explicitly retired.

## Context

A full **multi-agent trap-sweep** (18 verifier agents, every verdict git-cited) over all 205 open/partial self-improvement backlog entries reconciled 37 stale-status rows (PRs #1493/#1494/#1496) and left **151 genuinely-open entries** verified accurate. This doc groups those 151 into coherent, owner-aligned PRs so the backlog becomes an actionable roadmap instead of a flat pile. Intended outcome: *after this lands, every open backlog item has a named home PR + owner + concrete files-to-touch, ordered so infrastructure ships before its dependents.*

Source data: the trap-sweep run (`wux8jz521`, run `wf_ca0057a0-57d`). Reconciliation PRs: #1493, #1494, #1496. First roadmap revision: #1497.

### Roadmap triple-check (2026-06-20)

2026-06-20 campaign: 17 of 23 groups shipped (#1502-1518); 6 deferred (visual/hardware/decision). See per-row status.

### Validity re-sweep (2026-07-14)

Re-verified the 6 deferred rows against `develop` @ HEAD (branch `claude/backlog-pr-roadmap-validity-*`). Findings, each git-cited:

- **PR-10** (bucket-E grid/views/UI): 6 of 7 members shipped — `views-editor-field-selection` #1793, `help-marker-hover-fallback` #1790, `keybindings-editor-rebind` #1810, `multigrid-slice3` #1794, `data-dependent-windows` #1795, `grid-description-tooltip` (`tests/ui/description_tooltip_markdown_render.test.cpp`). **Residue: `user-info-window-bucket-e-coverage`** — no `tests/ui/*user_info*` bucket-E scenario exists (`tests/Core/UserInfoActivityCancelUaf.test.cpp` is a Core UAF regression, not the window smoke).
- **PR-11** (bucket-E AI chat): **fully shipped** — clear-confirm #1796, copy-to-clipboard #1799, pin + keyboard-nav + history-persist #1821 (all 5 scenarios).
- **PR-13b** (prefs-UI bugs): shipped — assistant Anthropic base-URL field + `SeedDefaultWhisperModel` default present in `SmatchetPreferencesUi_{Assistant,Whisper}.cpp`; prefs bucket-E coverage under `tests/ui/ai_assistant_preferences_*`.
- **PR-13c** (`imgui-define-macro`): **closed — UNSOUND**, not deferrable. See § Deviations. Logged in `docs/self-improvement/categories/applied.md` (2026-06-20), halted during PR-13a (#1515).
- **PR-18** (pink-clear dock-gap): shipped — `DockGapSentinelScenario.cpp` arms the magenta `(255,0,255)` clear; `tests/bats/bucket_lane_launch_smoke.bats` asserts the zero-pink-pixel scan.
- **PR-9 follow-up** (case-8 self-activation): shipped — `SMATCHET_UITEST_WITH_LOCAL_CACHE` opt-in wired in `UiTestScenario.cpp` + `AppController_LocalCacheDb.cpp`.

Still genuinely open: **PR-14** (threshold still `65` in `project.config.json` + `coverage.yml`), **PR-19** (only a Slate-backbuffer log line exists; no `CopyResource`→readback→PPM path — needs an Unreal/DX12 build env), **PR-21** (`resolvePaneCatalog`/`ChoosePaneCatalogSource` absent from the tree).

This revision was verified before publish:

1. **Slug validity** — every member slug cross-checked against the trap-sweep `OPEN` verdict set; 0 stale/shipped/typo slugs (the only non-matches were the doc's own slug + status tokens).
2. **Completeness** — every P2 `OPEN` entry traced to a PR group; the first pass had **3 gaps**, now slotted: `out-of-band-on-trust-boundary-owes-tracked-test` → PR-1, `lint-hook-deferred-drain-4-tests` → PR-6, `subagent-eval-calibration` → new PR-23.
3. **Implementation seed** — each PR group's detail is drawn from the member entries' own git-cited "Concrete next action" fields (file paths verified to exist on develop @ the trap-sweep tree).

## Approach

Cluster the open entries by **subsystem + owning specialist** (per the delegation tables in `docs/agent-rules/delegation.md`), not by category file — entries that touch the same code/scripts ship and review together. Sequence so shared infrastructure precedes its consumers: the bucket-E harness fixes (PR-9) before the bucket-E scenario PRs (PR-10/11); the sanctioned merge wrapper (PR-1) before the rest of the merge-gate hardening. P3 entries are siblings of P2 clusters and fold into the matching PR rather than spawning their own.

This is a **batching map with per-group implementation seeds** — each PR group still gets its own plan doc (or a lightweight ship-loop) when picked up, but the § Implementation detail block below gives the owning agent the files-to-touch, approach, and verification hook so it starts from a concrete spec, not a slug. Rows carry the backlog slug so the owner can pull full git-cited evidence from `docs/self-improvement/categories/`.

## Files to modify

This roadmap doc only (`docs/plans/active/backlog-pr-roadmap.md`). Each PR group's own target files are listed per-group in § Implementation detail.

## Existing utilities reused

- `docs/agent-rules/delegation.md` — the subsystem-specialist → owner mapping each PR row cites.
- `docs/self-improvement/categories/*.md` — the open-entry source rows (slug-referenced per PR group).
- `agents/scripts/project/test-lint-rules.sh`, `agents/scripts/core/comment_audit.py`, `agents/scripts/core/test-plan-index.sh`, `agents/scripts/core/merge-gates.sh`, `agents/scripts/core/merge-watcher.py` — the most-touched scripts across the merge/lint/plan clusters.

## PR groups — overview

### Merge-gate / ship-loop tooling (owner: agentic-infra)

| PR | Members (backlog slug) | Status |
|---|---|---|
| PR-1 Sanctioned merge wrapper + gate visibility | `intent-gate-safe-merge-wrapper`, `green-pr-blocked-no-merge-signal`, `auto-merge-poller-default`, `auto-merge-armed-before-final-push-orphans-commit`, `verify-unresolved-review-threads-vs-head`, `out-of-band-on-trust-boundary-owes-tracked-test` | shipped #1503 |
| PR-2 CodeRabbit handling | `cr-review-skipped-pure-docs-auto-downgrade`, `cr-rate-limit-code-pr-auto-pause`, `coderabbit-current-head-helper`, `coderabbit-plan-ref-convention-path-instruction` | shipped #1504 |
| PR-3 Merge-watcher robustness | `merge-snapshot-ledger-uncommitted-loss-risk`, `merge-watcher-triage-attempts-unbounded`, `merge-watcher-agent-notify`, `merge-gate-absence-blind-nonrequired-allowlist`, `bucket-lane-status-broken-sentinel-auditable` | shipped #1505 |

### Lint / gate authoring (owner: build-doctor)

| PR | Members | Status |
|---|---|---|
| PR-4 Comment-noise + new-file lint reflex | `comment-blank-run-flags-single-doc-paragraph-separator`, `agent-headers-trip-comment-noise-gate`, `comment-noise-gate-reds-required-build`, `new-file-delta-lint-reflex`, `build-verify-shortcut-bypasses-lint-gate` | shipped #1509 |
| PR-5 New strict-zone lint rules | `ban-bare-json-parse-on-untrusted-ingress`, `concurrency-correctness-no-headless-test-home`, `function-size-audit-grandfather-blind`, `fail-open-meta-gate-authoring-check` | shipped #1510 (TSan leg deferred) |
| PR-6 Gate FP fixes + lint-hook tests | `lint-syntax-both-pch-version-drift-fp`, `fuzz-target-include-closure-unresolved-invisible`, `lint-hook-deferred-drain-4-tests` | shipped #1506 |

### Plan-doc / index tooling (owner: mechanic)

| PR | Members | Status |
|---|---|---|
| PR-7 plan-index robustness | `shallow-clone-corrupts-git-log-date-generators`, `test-plan-index-shallow-clone-corrupts-date-sort`, `plan-index-fix-wrong-cwd-silent-noop`, `test-plan-index-fix-shipped-date-placeholder`, `markdown-links-local-passes-ci-fails-after-plan-archive` | shipped #1502 |
| PR-8 Plan-staleness gates | `archive-staleness-check`, `plan-doc-postship-closeout-stale-active-gate`, `extraction-sizing-step`, `historical-review-ledger-staleness` | shipped #1511 |

### Bucket-E test coverage (owner: test-rig / subsystem)

| PR | Members | Status |
|---|---|---|
| PR-9 Bucket-E harness fixes *(unblocks PR-10/11)* | `bucket-e-uitestscenario-no-live-local-cache`, `bucket-e-failures-blind-stdout`, `faketrackerclient-fetch-queue-auto-sticky`, `bucket-e-ci-fixture-env-export`, `fakep4runner-spawn-fail-vs-timeout` | shipped #1518 (case-8 self-activation = follow-up) |
| PR-10 Bucket-E: grid / views / UI | `multigrid-slice3-lifecycle-bucket-e`, `data-dependent-windows-bucket-e-render`, `grid-description-tooltip-bucket-e`, `views-editor-field-selection-bucket-e`, `user-info-window-bucket-e-coverage`, `keybindings-editor-rebind-bucketE-residue`, `help-marker-hover-fallback-bucket-e` | shipped — last member `user-info-window-bucket-e-coverage` in #1850 (bucket-E 6/6 + 4 approved goldens; behaviours 3/4 fixture-gated residue backlogged) |
| PR-11 Bucket-E: AI chat | `ai-chat-bucket-e-coverage` | deferred — screenshot baselines need golden-image approval (user) |
| PR-9 Bucket-E harness fixes *(unblocks PR-10/11)* | `bucket-e-uitestscenario-no-live-local-cache`, `bucket-e-failures-blind-stdout`, `faketrackerclient-fetch-queue-auto-sticky`, `bucket-e-ci-fixture-env-export`, `fakep4runner-spawn-fail-vs-timeout` | shipped #1518 (case-8 self-activation follow-up shipped — `SMATCHET_UITEST_WITH_LOCAL_CACHE`) |
| PR-10 Bucket-E: grid / views / UI | `multigrid-slice3-lifecycle-bucket-e`, `data-dependent-windows-bucket-e-render`, `grid-description-tooltip-bucket-e`, `views-editor-field-selection-bucket-e`, `user-info-window-bucket-e-coverage`, `keybindings-editor-rebind-bucketE-residue`, `help-marker-hover-fallback-bucket-e` | shipped 6/7 (#1793/#1790/#1810/#1794/#1795 + description-tooltip) — **residue: `user-info-window-bucket-e-coverage` (open)** |
| PR-11 Bucket-E: AI chat | `ai-chat-bucket-e-coverage` | shipped #1796/#1799/#1821 (all 5 scenarios) |

### AI / assistant subsystem (owner: tracker-backend / test-rig)

| PR | Members | Status |
|---|---|---|
| PR-12 AI client tests | `aiclientcancel-per-client-regression`, `per-client-error-body-redaction-gate`, `aiassistant-streaming-scenarios-s2-s4-s5` | shipped #1513 |
| PR-13 AI prefs/controller bug fixes | `assistant-prefs-3-bugs`, `aiassistantcontroller-3-loads`, `whisper-prefs-4-bugs`, `imgui-define-macro`, `whisper-local-backend-default-flip-decision` | PR-13a shipped #1515; PR-13b (prefs-UI) shipped (base-URL + whisper-default; #1819); PR-13c (imgui-macro) **closed — UNSOUND** (applied.md 2026-06-20, see § Deviations) |

### Coverage / build / hygiene / infra

| PR | Members | Owner | Status |
|---|---|---|---|
| PR-14 Raise core coverage 65→70 | `raise-core-coverage-67-to-70`, `backend-impl-coverage-recovery` | test-rig | **open** (re-verified 2026-07-14: threshold still `65`) — flip needs measured headroom; add tests first |
| PR-15 CMake / CI robustness | `cmake4-fresh-configure-drops-ehsc`, `fetchcontent-cache-path-drift`, `advisory-ci-step-level-template`, `ubsan-merged-without-executing-validation` | build-doctor | shipped #1516 |
| PR-16 Worktree / session-registry + branch-edit guards | `session-registry-liveness-followups`, `edit-on-merged-pr-branch-reverts-develop`, `campaign-sibling-prs-edit-shared-plan-doc-thrash`, `decomposition-prs-serial-conflict-shared-files` | git-janitor | shipped #1512 |
| PR-17 Ship-loop discipline rules (docs) | `not-started-status-verified-against-merged-code`, `review-before-commit-hardening`, `adversarial-rca-before-coding`, `security-review-plan-time-trust-boundary`, `ship-time-issue-elevation-check`, `exe-auto-launch-diff-trigger`, `ci-config-slice-dup-preflight` | docs | shipped #1508 |

### Standalone subsystem items

| PR | Member | Owner | Status |
|---|---|---|---|
| PR-18 Pink-clear dock-gap scan | `pink-clear-dock-gap-scan` | ui-host | shipped — `DockGapSentinelScenario` magenta clear + `bucket_lane_launch_smoke.bats` zero-pink assertion |
| PR-19 DX12 backbuffer readback screenshot | `dx12-backbuffer-readback-screenshot-diff` | unreal-bridge | **open** (re-verified 2026-07-14: no readback path present) — needs Unreal/DX12 build environment (visual) |
| PR-20 Tracker redirect no-follow regression | `tracker-redirect-no-follow-regression-test` | tracker-backend / security | shipped #1517 |
| PR-21 Per-pane catalog value-read routing | `per-pane-catalog-value-read-routing` | grid-engine | **open** (re-verified 2026-07-14: `resolvePaneCatalog`/`ChoosePaneCatalogSource` absent) — grid behavior change (visual validation) |
| PR-22 Portable-layer + daemon hardening | `de-smatchetify-portable-layer`, `daemon-loop-per-iteration-backstop-audit` | build-doctor / agentic-infra | shipped #1514 (bounded de-smatchetify) |
| PR-23 Subagent-eval calibration | `subagent-eval-calibration` | agentic-infra | shipped #1507 |

## Implementation detail (per PR group)

Each block: **Files** (the scripts/TUs to touch) · **Approach** · **Verify** · **Depends**. Detail is the member entries' own git-cited next-actions; the owning agent should still grep-confirm line numbers (the tree moves).

### PR-1 — Sanctioned merge wrapper + gate visibility
- **Files**: new `agents/scripts/core/safe-merge.sh` + `tests/bats/safe_merge.bats`; new `scripts/dev/pr-blocked-why.sh`; edits to `docs/agent-rules/ship-loops.md` + `docs/agent-rules/merge-gates.md`; `agents/scripts/core/merge-gates.sh` (override-time obligation hook).
- **Approach**: `safe-merge.sh` is a non-admin sibling of `safe-admin-merge.sh` — runs `merge-gates.sh`, arms `gh pr merge --squash --auto` only on PASS, refuses any red block-allowlist gate lacking its `*-out-of-band` label. `pr-blocked-why.sh <pr>` reports the precise blocker (unresolved reviewThreads / skipped-required / reviews shortfall) for MERGEABLE+BLOCKED PRs. `verify-unresolved-review-threads-vs-head` + `auto-merge-poller-default` + `auto-merge-armed-before-final-push` are doc rules in those two `.md`s. `out-of-band-on-trust-boundary-owes-tracked-test`: at override-merge time, when a `tests/perf-out-of-band` label rides a strict-zone diff, auto-file a tracked `test.md` obligation (or Issue).
- **Verify**: bats — refuse-when-red-without-label, arm-when-green, obligation-filed-on-override. **Depends**: none (anchor PR).

### PR-2 — CodeRabbit handling
- **Files**: `agents/scripts/core/merge-gates.sh` + `tests/bats/merge_gates.bats`; new `scripts/dev/coderabbit-current-head.sh`; `.coderabbit.yaml`.
- **Approach**: in `merge-gates.sh` detect CR review-skipped caused by rate-limit AND `is-pure-docs-diff.sh` true → auto-downgrade to WARN. CR-rate-limit handler: pause/retry on cooldown or require a `cr-disposition:` marker before honoring `cr-out-of-band`. `coderabbit-current-head.sh <pr>` prints current head SHA + latest CR check/review for that head + "historical comments ignored". `.coderabbit.yaml` gains a `docs/plans/**` `path_instructions` teaching the tier-less ref convention.
- **Verify**: bats — rate-limit+pure-docs→pass, rate-limit+code→block; CR-state-parser fixture. **Depends**: none.

### PR-3 — Merge-watcher robustness
- **Files**: `agents/scripts/core/merge-watcher.py` + `tests/bats/` (or its pytest); `agents/scripts/core/postmortem-owed.sh`.
- **Approach**: durable ledger — commit/push each appended `merge-snapshots.jsonl` row to a ledger branch/PR, OR a pre-reset guard refusing `reset`/`checkout`/`clean` on a tree with uncommitted ledger rows. Triage cap — once `attempts_after > budget` on the SAME `head_sha`, early-return without persisting a further increment (clamp at budget+1). Agent-notify sink `.agent-events.jsonl` + a `merge-watcher-cli.py await` subcommand. Present-assertion for allow-listed non-required checks (fail-closed if configured-to-run but absent). Surface lane `status=broken` as a machine-readable artifact; teach `postmortem-owed.sh` to treat block-scope RED-because-BROKEN as auditable WARN, not an owed escape.
- **Verify**: bats — same-head re-poll while exhausted doesn't increment; broken-lane→WARN fixture. **Depends**: none.

### PR-4 — Comment-noise + new-file lint reflex
- **Files**: `agents/scripts/core/comment_audit.py`; `build_and_run.ps1` (or new `verify.ps1`); `docs/agent-rules/process-rules.md`; C++-writing subagent prompts (`agents/core/offline-sync.md`, `test-rig.md`, `build-doctor.md`, `mechanic.md`, `debug-detective.md`); optional `docs/harness/claude-code/hooks/` per-edit hook.
- **Approach**: relax `comment_audit.py` cut-blank so a single bare `//` between two non-blank comment lines of the same block is an allowed intra-block separator (+ `--selftest` fixture). `build_and_run.ps1 -BuildOnly` (or a `verify.ps1` wrapper) runs `comment_audit.py --diff` + `test-lint-rules.sh --diff` after a successful build. Add "run `test-lint-rules.sh --diff origin/develop` before push" to the subagent prompts + a new-file delta-lint reflex in `process-rules.md` § Cadence (or a pre-ship new-file detector). Residual `agent-headers` (a)+(c): comment-noise gotchas in the prompts + a per-edit hook running `comment_audit.py` on just-written files.
- **Verify**: `comment_audit.py --selftest` green on the new intra-block fixture; bats on the wrapper. **Depends**: none.

### PR-5 — New strict-zone lint rules
- **Files**: `agents/scripts/project/test-lint-rules.sh` + matching bats fixtures; (TSan leg) a CI workflow + native test leg.
- **Approach**: WARN-first delta-gated `bare-json-parse-untrusted` grep rule flagging bare `nlohmann::json::parse` on untrusted ingress not via `ParseBounded`. `concurrency-correctness`: a strict-zone rule forbidding off-UI-thread writes to g_ui request-flag fields from `Source/Core/src/Commands/**` outside a `RunOnUiThread*` closure, plus a TSan/native leg exercising AI-client request paths + a non-atomic shared-flag lint. `function-size-audit-grandfather-blind`: mandate `function_size_audit.py --scan-file` in decompose prompts + an end-of-program repo-wide `--list-empty` assertion + a CI scan-file regression mode. `fail-open-meta-gate-authoring-check`: extend `test-gate-selftests.sh` with greps for fail-open shapes B/C/D/E + a synthetic-driver fixture.
- **Verify**: each rule green on HEAD, red on a planted regression fixture (delta-gated). **Depends**: none (but coordinate with PR-4 on `test-lint-rules.sh` to avoid serial conflict — see PR-16).

### PR-6 — Gate FP fixes + lint-hook tests
- **Files**: `docs/harness/claude-code/hooks/lint-syntax-both.py`; a fuzz-closure gate or code-review checklist; `agents/scripts/core/test-lint-hook-split.sh` (or wherever the deferred tests live).
- **Approach**: add `Microsoft Visual C/C++ Version differs in precompiled file` + `was compiled for the target` to `_FP_PATTERNS` in `lint-syntax-both.py` (PCH version-drift FP). Static gate resolving each fuzz target's closure `.cpp` first-hop quote-includes against its `INCLUDES` (WARN), or a checklist item. Implement the 4 deferred lint-hook tests: 4 (fault-injection cppcheck), 5 (chunked drain), 6 (per-PID isolation), 10 (lockfile serialisation).
- **Verify**: PCH-drift line no longer flagged; the 4 hook tests pass. **Depends**: none.

### PR-7 — plan-index robustness
- **Files**: `agents/scripts/core/test-plan-index.sh` + sibling history-dependent generators; bats with a shallow-clone fixture.
- **Approach**: shared `is_shallow_or_refuse()` helper — `git rev-parse --is-shallow-repository` → auto-`fetch --unshallow` OR refuse `--fix` (exit 2) + WARN under `--check`. `cd "$(git rev-parse --show-toplevel)"` (or resolve `ARCHIVE_DIR`/`INDEX_FILE` against git root) so `--fix` can't index the wrong tree. Derive shipped-date deterministically (same as CI auto-sync) so local `--fix` is byte-identical, or refuse with a remedy instead of a placeholder. `markdown-links`: tier-ful plan-link convention in the template + `docs/STRUCTURE.md`, plus a synthetic merge-tree / behind-develop mode in `test-markdown-links.sh`.
- **Verify**: bats shallow-fixture (refuse + WARN); `--fix` byte-identical local vs CI. **Depends**: none.

### PR-8 — Plan-staleness gates
- **Files**: new `agents/scripts/project/test-plan-staleness.sh` (auto-enrolled by `test-all.sh`); `agents/scripts/core/test-plan-index.sh` (or a new check); `docs/plans/active/_plan-template.md`; new historical-review reconcile pass + SessionStart nudge.
- **Approach**: advisory WARN classifying active plans by Implementation-log stub-vs-populated + `gh pr state` (+ a git-janitor hook). A gate FLAGGING any active plan whose PRs are all-merged but post-ship sections are still the stub. An extraction-sizing step (classify EXTRACT vs STAYS) in the plan template / `grill-with-docs`. `historical-review-ledger-staleness`: a reconcile pass running `historical-review-survivors.sh`/`finding-already-fixed.sh` over each still-open priority finding + a freshness nudge when the ledger ages past N days.
- **Verify**: staleness WARN fires on a stub-but-merged fixture plan. **Depends**: PR-7 (shares `test-plan-index.sh`).

### PR-9 — Bucket-E harness fixes *(unblocks PR-10/11)*
- **Files**: `tests/support/UiTestScenario.*`, `tests/support/FakeTrackerClient.h`, `tests/support/FakeP4Runner.h`; the `ui_test.run` command; bucket-E bash drivers; a CI step.
- **Approach**: `UiTestScenario::OnStart` opt-in (`SMATCHET_UITEST_WITH_LOCAL_CACHE=1`) initialising a throwaway SQLite LCM so `offlineQueue_` constructs and the populated path self-activates. `ui_test.run --outLog=<path>` dumping `ctx->Test->Output.Log` per test; bash drivers pass + `cat` on failure. `FakeTrackerClient::EnqueueFetchResult` auto-sticky on last entry (or default `fetchFullSyncCompleted_=false` on drain) so unscripted re-fetch can't trigger stale-pruning. Reshape `FakeP4Runner.h` so spawn-fail vs non-zero-exit are distinguishable. CI step exporting `SMATCHET_TEST_JIRA_BACKEND_FIXTURE` + running `test-ui-jira-deterministic-backend.sh` as a hard check.
- **Verify**: existing bucket-E suites still green; new fixtures exercise the seams. **Depends**: none — **blocks PR-10, PR-11**.

### PR-10 — Bucket-E: grid / views / UI
- **Files**: new `tests/ui/*.test.cpp` per window (grid header, views dashboard, offline queues, new-issue draft, attachment preview, annotate, JQL, ticket-field editor, calendar, user-info, keybindings editor); driven via `UiDrawSession` latches.
- **Approach**: boot-open-assert smoke per data-dependent window; `multigrid-slice3` two cases (non-focused-visible pane live sync + hidden-pane retirement/regeneration, PR #975 fixture); `grid-description-tooltip` width regression; `views-editor-field-selection` (near-full catalog, toggle late-sorting field, assert persist + view-switch non-leak); `user-info-window` via `userInfoRequestPending` latch (7 behaviours + 4 baselines); `keybindings-editor-rebind` via test-only dirty-flag shim or replica window; `help-marker-hover` ItemHover inside `BeginDisabled`.
- **Verify**: bucket-E (`ninja-ui-test-msvc`) + screenshot baselines. **Depends**: **PR-9** (harness fixes).

### PR-11 — Bucket-E: AI chat
- **Files**: 5 new `tests/ui/ai_chat_*.test.cpp` scenarios.
- **Approach**: `ai_chat_pin_bookmark`, `ai_chat_copy_clipboard`, `ai_chat_history_persist`, `ai_chat_clear_confirm`, `ai_chat_keyboard_nav` as ImGui-Test-Engine bucket-E cases.
- **Verify**: bucket-E green. **Depends**: **PR-9**.

### PR-12 — AI client tests
- **Files**: new `tests/Core/AiClientCancel.test.cpp` + `AiClientErrorRedact.test.cpp` (or extend); new `tests/support/AiHttpFixture.h`; `Source/Core/src/Commands/Scenarios/AiAssistantSendScenario.cpp`; `scripts/dev/test-ai-assistant.sh`.
- **Approach**: parameterise cancel test across OpenAi/Anthropic/Ollama (fake httplib server, cancel mid-stream, assert `WasCancelled` within K chunks). Per-client error-body redaction gate: drive each `IAiClient` against a fake 401 echoing the key, assert `AiStreamError::Message` lacks the literal key. Headless streaming S2/S4/S5 scenarios via the new fixture + bash driver.
- **Verify**: Bucket-A ctest + `test-ai-assistant.sh`. **Depends**: none (pure-logic + fixture).

### PR-13 — AI prefs/controller bug fixes
- **Files**: `Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp`, `SmatchetPreferencesUi_Assistant.cpp`, the AiAssistantController TU, the AI Assistant TU (`#define ImGui` site).
- **Approach**: whisper 4 fixes (auto-mode prefer-local/reword, seed `WhisperModel` default, hotkey fallback text, conditional E2E hint). assistant 3 fixes (Anthropic base-URL field, probe-generation-gated commit, persist `cfgField` to `catalog[0]`). controller: ONE `TrackerConfig` snapshot at turn start threaded through `RefreshProviderForTurn`/`ResolveModelAndEffort`/`BuildChatPayload`. Replace `#define ImGui SmatchetLocalizedImGui` with `using namespace SmatchetLocalizedImGui;` + sweep all macro-trick TUs.
- **Verify**: dual-target build + any bucket-E prefs coverage; manual prefs pass if no harness. **Depends**: none.

### PR-14 — Raise core coverage 65→70
- **Files**: new `tests/Core/*.test.cpp` on lowest-covered strict-zone units; `tests/support/JiraCatalogHttpFixture.h`; `project.config.json` (`coverage.threshold`), `.github/workflows/coverage.yml` (`--threshold`).
- **Approach**: add unit tests until measured rate clears 70 with headroom, then bump threshold 65→70 in both places. `backend-impl-coverage-recovery`: extend the `JiraCatalogHttpFixture` pattern to search/mutation/user-meta paths of `JiraClient` (+ Plane/GitHub later); remove `coverage-out-of-band` once clear.
- **Verify**: Coverage CI green at the raised threshold. **Depends**: lands after the bucket-E/test PRs add coverage (sequence late).

### PR-15 — CMake / CI robustness
- **Files**: `CMakePresets.json` (`_smatchet-msvc-base`), `CMakeLists.txt`, `.github/workflows/build-and-test.yml`, `docs/agent-rules/build.md`, a lint/bats assertion.
- **Approach**: set `/EHsc` + `/DWIN32 /D_WINDOWS` in the MSVC base preset (or `CMAKE_CXX_FLAGS_INIT`) + detect/wipe poisoned empty-flags cache + a build.md note. CI assertion grepping `build-and-test.yml` cache `path:` vs the `FETCHCONTENT_BASE_DIR` default, failing on mismatch. A shared advisory-job snippet (step id + step-level `continue-on-error` + artifact upload keyed on `steps.<id>.outcome`); audit existing advisory jobs. Validate the UBSan job runs green on the next Source/Core PR.
- **Verify**: fresh-configure smoke + the cache-path assertion red on a planted drift. **Depends**: none.

### PR-16 — Worktree / session-registry + branch-edit guards
- **Files**: `agents/core/git-janitor.md` + its registry-sweep script; `docs/agent-rules/process-rules.md`; `.gitattributes`; tests/CMakeLists glob registration.
- **Approach**: wire the git-janitor registry sweep to call `sr_prune_dead_stale` over EVERY worktree's `.active-sessions/` + port-forward authoritative-pid liveness into the planned `guard.mjs`. Pre-Edit check (process-rules) confirming the checked-out branch isn't a merged-PR / behind-develop on the target file. `.gitattributes` `tests/CMakeLists.txt merge=union` + GLOB auto-registration (new test needs no CMakeLists edit) + prefer free fns over `SmatchetUI` members + one-PR-per-worktree discipline.
- **Verify**: bats on the registry sweep; `.gitattributes` union verified on a synthetic conflict. **Depends**: none.

### PR-17 — Ship-loop discipline rules (docs)
- **Files**: `docs/agent-rules/ship-loops.md`, `merge-gates.md`, `process-rules.md`, `issue-triage.md`, `agents/core/debug-detective.md`; `issue-sweep.sh`.
- **Approach**: a batch of doc rules — verify status against merged code before claiming not-started/shipped/stale; explicit code-review-pass step between fix and commit (+ Stop-hook reminder); adversarial RCA pass before committing P0/crash fixes (debug-detective); security-review at PLAN time on trust-boundary designs; ship-time Issue-elevation marker grep in `issue-sweep.sh`; exe-auto-launch diff trigger on `Source/Core/src/Ui` touch; CI-config-slice dup-preflight checklist.
- **Verify**: doc-validation; bats on the `issue-sweep.sh` elevation grep. **Depends**: none (pure docs; ship in slices if the diff is large).

### PR-18..23 — Standalone
- **PR-18 pink-clear**: `requestClearColor{R,G,B,A}` on `UiDrawSession` + restore-after-frame consumer in `main.cpp`; extend `DockGapSentinelScenario` + a bash `CountPixels(img,255,0,255,8)==0` hard assertion. *(ui-host)*
- **PR-19 DX12 readback**: `CopyResource` backbuffer→readback heap + memcpy to a PPM writer in `Source/UnrealPlugins/SmatchetImGuiPlugin/`. *(unreal-bridge)*
- **PR-20 redirect no-follow**: doctest/integration — tracker request vs an in-process server issuing cross-host 30x; assert `MakeTrackerRedirectPolicy` no-follow AND Authorization not re-sent cross-host. *(tracker-backend / security)*
- **PR-21 per-pane catalog**: populate each pane context's `fieldCatalog` independently (seed cross-backend from `FieldCatalogCache` at `EnsurePaneContextLive` + per-pane first-sync into the CONTEXT), then re-land `resolvePaneCatalog` + `ChoosePaneCatalogSource` read routing. *(grid-engine)*
- **PR-22 portable + daemon**: rewrite `agents/core/*` + `docs/agent-rules/*` prose to reference `project.config.json` keys (shrink `portable-purity-baseline.txt` toward zero); audit other daemons (issue-janitor/p4-janitor/while-True pollers) for unguarded per-iteration bodies + `subprocess.run(timeout=)` sites with no guard. *(build-doctor / agentic-infra)*
- **PR-23 subagent-eval calibration**: live code-review smoke result JSON + a judge-vs-human calibration loop with BLOCK thresholds. *(agentic-infra)*

## P3 entries (73) — fold into matching P2 PR

The 73 P3 opens are smaller siblings of the clusters above (more bucket-E scenarios, more gate helpers/FP-fixes, doc-convention tweaks, P4-layer polish, minor UI/test gaps). They ride the matching P2 PR rather than spawning their own — e.g. a P3 plan-index tweak rides PR-7, a P3 bucket-E scenario rides PR-10. Full list: query the trap-sweep verdicts or `grep 'P3' docs/self-improvement/categories/*.md`.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — this is a docs-only roadmap; perf belongs to each member PR when picked up.
- **Pillar 2 (UI-thread)**: no impact (docs only).
- **Pillar 3 (never crash)**: no impact (docs only).
- **Pillar 4 (accessibility)**: no impact (docs only).

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A`)

`N/A` — this PR touches only `docs/plans/active/`; no `Source/Core/` diff. Each member PR declares its own perf gates when authored.

## Risks / non-goals

- **Risk: roadmap rots as PRs land.** Mitigation: each PR's ship-loop flips its row to `shipped (#N)`; a periodic re-sweep (the same trap-sweep harness) re-verifies open rows.
- **Risk: per-group file paths drift.** Mitigation: § Implementation detail says "grep-confirm line numbers"; paths are seeds, not pinned line refs.
- **Non-goal: implementing any member PR.** This doc only groups + sequences + seeds; design lives in each member's own plan/ship-loop.
- **Non-goal: re-litigating priorities.** P2/P3 tiers are inherited from the backlog entries as-verified, not re-scored here.

## Verification

Per `AGENTS.md` § Project rules — this is a pure-docs roadmap; no build/test buckets apply.

- **Bucket A / E / scenario**: N/A — docs only.
- **Build gate**: N/A — no C++ diff.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint).
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: roadmap triple-checked against the live backlog — every PR row traces to a verified-open entry; 3 completeness gaps found + closed; no row references a reconciled/shipped slug; per-group file paths drawn from the entries' own git-cited next-actions.
- **Manual residue**: none.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — no symbols are deferred by this doc (it defers nothing; it schedules).

- **Mechanical archival of now-`applied` monolith entries** into `applied.md` — the trap-sweep flipped 13 entries to `applied` in place; relocating them is a separate hygiene PR, not part of this roadmap.

## Implementation log
*(populated as PR groups land — bullet per shipped member: `#N · PR-<k> <slug> shipped`)*
- `#1850` · PR-10 `user-info-window-bucket-e-coverage` shipped (2026-07-14) — the last unshipped PR-10 member. bucket-E TU (`tests/ui/user_info_window.test.cpp`, 6/6): open/Escape/Close lifecycle, close-edge `ClearPaneUserActivity` once, `VcsFeedLayout` toggle + persistence, ~400px narrow layout, Identity+Vcs sections. bucket-C: 4 `user-info-*` screenshot goldens (desktop/narrow × unified/separate), human-approved, L∞=0 self-diff. Residue (backlogged, not faked): behaviours 3 (load-button-disabled) + 4 (one-shot group fetch) need an activity-capable `FakeTrackerClient` seam — filed `2026-07-14-user-info-window-bucket-e-activity-fixture-gap`; the 3 legacy goldens' fresh-run non-determinism filed `2026-07-14-bucket-c-goldens-need-config-isolation`.

- 2026-06-20 campaign · PR-1..9, 12, 13a, 15, 16, 17, 20, 22, 23 shipped (#1502-1518).
- Post-campaign follow-ups (verified 2026-07-14 re-sweep):
  - `#1518-followup` · PR-9 `case-8 self-activation` shipped — `SMATCHET_UITEST_WITH_LOCAL_CACHE`.
  - `#1794` · PR-10 `multigrid-slice3-lifecycle-bucket-e` shipped.
  - `#1793` · PR-10 `views-editor-field-selection-bucket-e` shipped.
  - `#1790` · PR-10 `help-marker-hover-fallback-bucket-e` shipped.
  - `#1810` · PR-10 `keybindings-editor-rebind-bucketE-residue` shipped.
  - `#1795` · PR-10 `data-dependent-windows-bucket-e-render` shipped (all listed windows covered).
  - `—` · PR-10 `grid-description-tooltip-bucket-e` shipped (`tests/ui/description_tooltip_markdown_render.test.cpp`).
  - `#1796 / #1799 / #1821` · PR-11 `ai-chat-bucket-e-coverage` shipped (all 5 scenarios).
  - `#1819` · PR-13b `assistant-prefs` / `whisper-prefs` bug fixes shipped (base-URL field + `SeedDefaultWhisperModel`).
  - `—` · PR-18 `pink-clear-dock-gap-scan` shipped (`DockGapSentinelScenario` + `bucket_lane_launch_smoke.bats`).

## Deviations from plan
*(populated as the roadmap is revised — regrouping, reprioritisation, new entries from future sweeps)*

- **2026-07-14 · PR-13c `imgui-define-macro` reclassified `deferred-respec` → `closed — UNSOUND`.** The original deliverable (replace `#define ImGui SmatchetLocalizedImGui` with `using namespace SmatchetLocalizedImGui;`) is wrong by construction: every call site is the **qualified** name `ImGui::Foo`, which the `#define` textually rewrites to `SmatchetLocalizedImGui::Foo`, but a `using namespace` directive does **not** redirect qualified-name lookup — so `ImGui::Foo` would bind to the real `::ImGui::Foo`, compiling clean while silently skipping the localization overrides (`LabelFromSource`/`TranslateSource`/`WindowTitleFromSource`) across ~40 TUs. `SmatchetUI_Internal.h` documents the qualified-rewrite as deliberate. A clean-compiling silent i18n regression is worse than a build break; halted during PR-13a (#1515), logged in `docs/self-improvement/categories/applied.md`. No further work owed — the macro is load-bearing and stays.
- **2026-07-14 · 3 of 6 deferred groups had already shipped** (PR-10 all-but-`user-info`, PR-11, PR-18) — the doc's status rows were stale by ~3 weeks. Reconciled above; remaining open set is PR-14, PR-19, PR-21, plus the PR-10 `user-info-window` bucket-E residue.

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped. In the SAME PR that retires this roadmap —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*
