# Plan — Agentic ripout doc cleanup (v2 follow-up to `github-tracker-backend`)
<!-- plan-date: 2026-05-21 -->

> **Slug**: `agentic-ripout-doc-cleanup-v2`
>
> **Status**: DRAFT-WAITING. v1 PR1 + PR2 merged 2026-05-21 (`b1d241bc` + `cd66e28c`). Watcher plan-doc landed 2026-05-21 (`docs/plans/shipped/smatchet-merge-watcher.md` `8677e8b`); v2's "wait for watcher P1" sequencing step retired (see § Sequencing). Single remaining blocker: **3 in-flight PRs (#358 / #359 / #360) must merge first** to avoid AGENTS.md cascade-conflict. Once those clear, v2 is ready to execute per the locked 2026-05-21 evening re-grill (see § Context).
>
> **Mandatory rules cross-link**: see [`AGENTS.md`](../../AGENTS.md) § Project rules § Plan location.

## Context

The v1 plan ([`github-tracker-backend.md`](github-tracker-backend.md)) explicitly accepts **bit-rot in docs** as a known cost — `AGENTS.md` sections, `agents/*.md` agent files, `docs/agentic/`, `docs/agent-rules/delegation.md`, `docs/plans/active/agentic-*.md`, ADRs 0004 + 0005, `scripts/dev/test-agentic-*.sh`, `scripts/dev/test-coderabbit-react.sh`, `scripts/dev/test-ui-agent-*.sh` all stay verbatim while their underlying C++ runtime disappears.

This v2 plan exists to clean up that bit-rot once v1 has shipped + stabilised. **Do not start v2 work until v1 PR1 + PR2 merge** — running them in parallel creates rebase conflicts in the same files.

**2026-05-21 evening re-grill (4 locked decisions)**: after v1 PR1 (#356) merged + v1 PR2 (#357) hit CI, two P1 tooling backlog entries landed on develop that change v2's preserve/strip balance:

1. **`docs/self-improvement/categories/process.md` — "Draft PRs silently bypass CodeRabbit review"** (`9312695`) — calls for the merge-gates poller to auto-invoke `agents/core/coderabbit-triage.md` on CR feedback.
2. **`docs/self-improvement/categories/tooling.md` — "Long-running CI/CR polls block the interactive session"** (`644f822`) — proposes `smatchet-merge-watcher` as a separate host process; reuses `scripts/dev/merge-gates.*` + `agents/core/coderabbit-triage.md` as ingredients.

Both REVIVE pieces of the agentic surface in a new (watcher-driven) shape. V2's "delete coderabbit-triage" + "strip § Merge gates" calls were overreach. Re-grilled decisions:

| Item | v2 v1 (original) | v2 v2 (post-2026-05-21-grill) |
|---|---|---|
| `agents/core/coderabbit-triage.md` | DELETE | **KEEP** — strip spawned-harness language only |
| `the deleted handoff-envelope section` | DELETE | **STRIP ENTIRELY** (watcher won't spawn) |
| ADRs 0004 + 0005 | Withdraw status | **WITHDRAW + KEEP as historical** (1-line note → v1 ripout commits) |
| Timing | "after v1 merges" | **Wait for watcher P1 first** — that work clarifies preserve/strip boundaries |

V2 stays in DRAFT until watcher P1 lands; once it does, the watcher's actual reuse surface determines the exact strip vs. preserve list.

## Scope (sketch — not locked)

### AGENTS.md — strip what `(agentic)`-titled PRs added

Verified via `gh pr list --search "agentic in:title"` + `git log -S ... -- AGENTS.md`:

- **§ Handoff envelope (entire section, lines ~355-426)** — added by PR#248 (`feat(agentic): H2 ...`); modified by PR#299 + #300 (sentinel files table + First-delegate selection subsection).
  - Subsections inside that came from non-(agentic) PRs but reference deleted symbols (§ Spawned-child PR draft requirement from PR#298 references "spawned `claude` child"; § Anti-deception note from PR#283 references `HarnessRunState::IsTransitionAllowed`) — delete with parent; orphans without it.

### AGENTS.md — also-stale-but-not-(agentic)-added (locked 2026-05-21 re-grill)

These describe deleted runtime but weren't added by `(agentic)`-titled PRs. Re-grill outcomes (4 sections):

#### § Merge gates (lines ~125-197) — **KEEP wholesale; surgical edits only**

The gate semantics (CI / CodeRabbit / user-comments / mergeStateStatus / pagination) describe the contract the future `smatchet-merge-watcher` reuses verbatim. The bash poller (`scripts/dev/merge-gates.sh / .graphql / -prompt.sh`) is the watcher's polling engine. Specific edits:

- **Line 127** ("Before the orchestrator (or `git-janitor` running in the user's main session) squash-merges...") → add the watcher as a third caller: "Before the orchestrator, `git-janitor`, OR `smatchet-merge-watcher` squash-merges...".
- **Line 137** (CR `NONE` grace-window) → cross-link the P1 backlog entry: "The grace-window logic exists because CR's placeholder StatusContext on draft PRs would otherwise satisfy this branch without a real review — see `docs/self-improvement/categories/process.md` 'Draft PRs silently bypass CodeRabbit review' for the gap + the proposed `non-empty review on headRefOid` requirement that strengthens this rule once the watcher implements it."
- **Line 138** (STALE check) — add explicit "applies even when the PR has a CR StatusContext SUCCESS on the rollup" — the STALE rule trumps the placeholder.
- **Line 176** (Auto-`gh pr ready` + merge authorization) → reword the trigger list: "(post-ship option 3 'Register with watcher', in-session 'merge when green', or any PR registered with the watcher daemon)".
- **Line 194 — Scope boundary** → DELETE the second sentence entirely ("Spawned-child agents (`handoff-implementer`, `pr-iterator`) keep their existing draft-only contract — see § Handoff envelope § Spawned-child PR draft requirement"). Replaced with: "applies to the orchestrator, `git-janitor`, and `smatchet-merge-watcher`. No other caller has merge authority."
- **Line 196 — Implementation** → stays identical (paths still valid; watcher will source the same .sh / .graphql).

#### § Autonomous ship-loop default (lines ~73-109) — **KEEP; 3 surgical edits**

The diagnose → fix → build → commit → push → open PR → gate-check → squash-merge → janitor → backlog sequence is general orchestrator behavior, NOT agentic-specific. Specific edits:

- **Line 81** (`[gate-check]` phrasing) → keep the rule; add "the watcher takes over from this point when the user picks post-ship option 3" so the handoff to the daemon is documented.
- **Line 89** (Exception 1 — Debug-mode pause-loop cross-link) → cross-link breaks once `docs/agent-rules/delegation.md § Debug-mode pause-loop` is stripped (see § delegation.md below). Either (a) **inline the pause-loop rule body into AGENTS.md** under this exception (5-6 lines: debug-detective owns the investigation; pause-loop overrides ship-loop until user signals "ship it"; orchestrator emits `[temp-debug]` instrumentation per `agents/core/debug-detective.md`); or (b) keep the cross-link if delegation.md § Debug-mode pause-loop is RETAINED (it's not agentic — debug-detective is general). Option (b) is simpler — see § delegation.md decision below.
- **Line 123** (Cross-link footer) → strip the broken `§ Debug-mode pause-loop` reference if delegation.md drops it; otherwise leave.

#### § Post-ship turn-end protocol § option 3 (line 116, 121) — **KEEP; REWORD, not strip**

Original v2 said "trim option 3". Post-grill: option 3 is the watcher integration point. Reword:

- **Old line 116**: `3. **Wait for gates and merge** — orchestrator runs the merge-gates poller (see § Merge gates), then auto-`gh pr ready` + REST-squash-merge on pass. On block / timeout / `gh` down / PR closed-externally / pagination overflow → `AskUserQuestion` per the code-specific halt prompts.`
- **New line 116**: `3. **Register with watcher** — orchestrator runs `smatchet-merge-watcher register <pr>` (host daemon — see `docs/self-improvement/categories/tooling.md` 'Long-running CI/CR polls block the interactive session' for the design). Watcher runs the gate-check loop + CodeRabbit-triage loop + REST-squash-merge per the watcher contract. Session can close immediately; watcher persists. Halt prompts surface as Smatchet notifications, not back to this session.`
- **Old line 121** (Skip-condition): "enter option 3 directly (`git-janitor` invokes the merge-gates poller before merging)" → "enter option 3 directly (`git-janitor` invokes the watcher register before merging)".

#### § Project rules § Force-push carve-out for spawned-agent recovery (line 238) — **REWRITE around `claude/<id>` only**

Original v2 said "trim to just the `claude/<id>` case". Post-grill: same outcome, fuller rewrite needed because the surrounding cross-link to delegation.md § API-500 mid-run recovery references both `agent/<id>` and `claude/<id>`:

- **Old (line 238)**: `the global git push --force ban ... gets one narrow carve-out — git push --force-with-lease origin agent/<id> and git push --force-with-lease origin claude/<id> are permitted only during API-500 recovery (see docs/agent-rules/delegation.md § API-500 mid-run recovery) when the orchestrator is amending an unpushed-since-API-500 commit on a spawned-agent worktree branch.`
- **New**: `the global git push --force ban ... gets one narrow carve-out — git push --force-with-lease origin claude/<id> is permitted only during API-500 recovery (see docs/agent-rules/delegation.md § API-500 mid-run recovery) when the orchestrator is amending an unpushed-since-API-500 commit on a Claude Code SDK-spawned worktree branch. The agent/<id> case is GONE — that branch shape came from the deleted ClaudeCodeLocalRunner (per v1 of github-tracker-backend.md). Smatchet's future smatchet-merge-watcher runs as a host daemon, not a spawned subprocess, so it has no worktree branch that would need this carve-out.`
- **ADR cross-link**: 0005's `agent/<id>` reference becomes obsolete once 0005 is Withdrawn (per § ADRs above). The carve-out's `claude/<id>` case stays valid + its rationale folds into the new v2-shipped status header on 0005.

### `docs/agent-rules/delegation.md` (locked 2026-05-21 re-grill)

Not modified by any `(agentic)`-titled PR but holds rules cross-linked from AGENTS.md. Edits per the re-grill:

- **§ Debug-mode pause-loop — KEEP** (re-grill correction). Originally said "strip" because the spawned-harness debug-trigger surface is gone, but the `debug-detective` agent is general-purpose + still alive. The pause-loop rule body (debug-detective owns the investigation; ship-loop overrides until "ship it" signal; orchestrator emits `[temp-debug]` instrumentation per `agents/core/debug-detective.md`) applies whether or not anything spawns a subprocess. KEEP wholesale; only strip any references to spawned-harness or `dispatch_source` if they appear inside the subsection.
- **§ API-500 mid-run recovery — TRIM** (was "strip subsection"). The 5-step recovery (inspect → run gates → `git add -A` + commit → push + open draft PR → backlog entry) applies to ANY delegated agent that errors API-500, not just spawned ones. Trim the `agent/<id>` worktree-branch references (deleted ClaudeCodeLocalRunner shape); keep the `claude/<id>` references (Claude Code SDK spawn shape, still valid). Concrete edits: at line 203 of delegation.md, change `the spawned-agent's own agent/<id> or claude/<id> worktree` → `the Claude Code SDK-spawned claude/<id> worktree`. Drop the agent/<id> half of every example.
- **§ Trigger auto-activation table rows — TRIM** (was "strip rows" for all 3):
  - `handoff-implementer` row — STRIP (agent file deleted).
  - `pr-iterator` row — STRIP (agent file deleted).
  - `coderabbit-triage` row — **KEEP** per re-grill agent-files decision (above). Update the trigger pattern to "CodeRabbit posts CHANGES_REQUESTED on a watched PR" (watcher invocation), drop any `dispatch_source` reference.

### `agents/*.md` agent files

Four files affected (3 in original v2 + `security-review.md` surfaced by 2026-05-21 evening deep-dive):

- **`agents/handoff-implementer.md`** (PR#248; v-bumped by #299) — **DELETE entire file** (175 lines). First delegate inside the deleted spawned-`claude` orchestration. Without `ClaudeCodeLocalRunner`, no spawn → no first-delegate role.
- **`agents/pr-iterator.md`** (PR#255) — **DELETE entire file** (132 lines). Second delegate in the spawn lifecycle; same rationale. The "iterate on PR comments" intent migrates to the watcher invoking `coderabbit-triage` directly.
- **`agents/core/coderabbit-triage.md`** (`ac8aeb85` `docs(agentic)`) — **KEEP** (203 lines) per 2026-05-21 re-grill. Surgical strip:
  - Frontmatter (lines 1-43) — keep as-is (`delegates-to:` lists 15 subsystem agents all still alive; `triggers:` keywords keep, optionally append a watcher-invocation trigger).
  - Body lines 185-190 — DELETE the 6-line spawned-harness contract block (`SEED.json` / `dispatch_source` / "routed delegate inside handoff-implementer's routing" / `RUN_RESULT.json` / "outside spawned-harness mode"). Body lines 1-184 (the 18-rule override table + classification + Smatchet-invariant rejection) stay untouched.
  - REPLACE the stripped block with a watcher-invocation paragraph: "When invoked by `smatchet-merge-watcher` (CR posted CHANGES_REQUESTED on a watched PR), this agent reads the watcher's prepared payload (`{pr_number, head_sha, cr_review_body, cr_review_threads}`), runs the 18-rule override + validation, applies valid fixes via subsystem delegates, commits + pushes. Watcher re-polls; CR re-reviews on push."
- **`agents/core/security-review.md` — NEW IN V2 (surfaced 2026-05-21 evening deep-dive)** — TRIM. Lines 66-72 hold a "Coding-harness handoff surface" attack-surface block that audits the deleted `ClaudeCodeLocalRunner` spawn (env allow-list, sentinel-file write contracts, branch-name discipline, PR draft requirement, GH PAT scope, worktree GC). The runtime it audits is GONE. Strip these 7 lines. Keep the rest of the file (MCP / CLI / Lua / SQLite / HTTP / AI-assistant attack surfaces all still live). The watcher (when it ships) will introduce its own attack surface — `security-review` gets a new block added in the watcher P1's plan + PR, not as part of v2 cleanup.

### Other docs added by `(agentic)`-titled PRs

- `docs/plans/active/agentic-coding-handoff.md` — **DELETE** (PR#217/#240/#259 etc.; describes deleted design).
- `docs/plans/active/agentic-flow-implementation.md` — **DELETE** (PR#217/#225 etc.).
- `docs/plans/active/agentic-triage-flow.md` — **DELETE** (PR#217).
- `docs/plans/shipped/coderabbit-react-loop.md` — **KEEP as historical** (re-grill correction; was DELETE). Add a `## Status` header at the top: `Historical — describes the C++ CodeRabbit react loop that v1 PR1 (#356, b1d241bc) deleted. The watcher revival (per docs/self-improvement/categories/tooling.md 'Long-running CI/CR polls block the interactive session') reuses the CR-triage classification concepts; this design doc is useful input for the watcher's design pass.` Keep the body verbatim.
- `docs/agentic/TRIAGE_MANUAL.md` + `USAGE.md` — **DELETE** (`docs(agentic)` PRs).
- `docs/agentic/` directory itself — **DELETE**.

### ADRs

- **ADR 0004 (pluggable-coding-harness-runner)** — `docs/adr/0004-pluggable-coding-harness-runner.md`. Decision was about a runtime that no longer exists. Action: flip status Accepted → **Withdrawn** with a one-line note pointing at v1 ripout commits.
- **ADR 0005 (force-push carve-out for spawned-agent recovery)** — same shape; flip → **Withdrawn**.
- **ADR 0006 (orchestrator-pr-stays-draft-by-default)** — STAYS Accepted (general rule, not agentic-specific).
- **ADR 0003 (github-as-itrackerclient)** — STAYS Accepted; v1 PR2 honors it for the tracker-only role. Optional one-line update noting the original "agentic triage half" rationale is partially obsolete; new tracker stands on the same interface choice.
- **ADR 0007 (audit-trail-actor-column)** — already Withdrawn from prior session.

### docs/CONTEXT.md glossary — trim deleted-runtime entries (added 2026-05-21 v1-grill deferral)

V1 decision-1 boundary keeps glossary verbatim, but the entries describe deleted runtime and will confuse onboarding readers. V2 should:

- **Strip § Agentic flow** (lines ~7-12) — `Triage half / Handoff half / Source tracker / Code host` subsections describe deleted controllers.
- **Trim `Source tracker` entry** (line ~18) — keep the term (still used by `ITrackerClient::GetTrackerType`) but drop the `AgentProposal.sourceTracker` + `AgenticPollSource` references.
- **Trim `Code host` entry** (line ~19) — keep the term (cross-link to ADR 0003 stays useful) but drop the `PrCommentWatcher` / `PrCheckRunWatcher` / `GraphQL` references.
- **Keep `UpdateField semantics — set-replace`** (line ~20) — load-bearing for v1 PR2's `GitHubClient::UpdateField` impl. Already aligned.
- **Keep `TrackerIssueKey`** (line ~21) — load-bearing for v1 PR2. Already aligned.
- **Strip `Plan-lock`** (line ~27) — references deleted `.github/workflows/locks-render.yml` + lock-cleanup.yml; v2 audits whether workflows survive (see § Workflows below).
- **Trim `SMATCHET_WITH_*` gates** (line ~61) — drop the `SMATCHET_WITH_AGENTIC` enumeration after v1 CMake-side retirement.
- **Strip § Agentic-harness wiring + Worktree** (lines ~79-82) — references `ClaudeCodeLocalRunner` (deleted) + `agent/<proposalId>` worktree base (`SDK-spawn-only path stays`).

Estimated diff: ~50-80 lines edited, ~4-6 entries stripped. Doc-anchor CI re-verifies cross-links.

### Scripts added by (agentic)-titled PRs — DELETE all 11

Verified via `gh pr view $pr --json files --jq '.files[] | select(.path | startswith("scripts/"))'` over the 42-PR set. All drive deleted C++ surface; non-functional post-v1-ripout.

| Script | Introduced by | Notes |
|---|---|---|
| `scripts/dev/test-agentic-triage-cli.sh` | PR#230 | T5 triage CLI smoke |
| `scripts/dev/test-ui-agent-proposals.sh` | PR#231 (modified #239) | T6 UI panel smoke |
| `scripts/dev/test-agentic-approve-reject.sh` | PR#233 | T8 proposal approve/reject smoke |
| `scripts/dev/test-agentic-handoff-cli.sh` | PR#251 (modified #252, #267) | H3 ClaudeCodeLocalRunner smoke |
| `scripts/dev/test-agentic-handoff-clarification.sh` | PR#253 (modified #267) | H5 clarification dual-channel smoke |
| `scripts/dev/test-agentic-handoff-iterate.sh` | PR#255 | H7 PR-iteration smoke |
| `scripts/dev/test-ui-agent-handoff.sh` | PR#256 | H8 handoff UI panel smoke |
| `scripts/dev/test-ui-agent-proposals-handoff-button.sh` | PR#257 (modified #267) | H9 Start-handoff button smoke |
| `scripts/dev/test-agentic-handoff-scenario.sh` | PR#259 | H10 handoff scenario step smoke |
| `scripts/dev/test-ci-react.sh` | PR#302 | phase-9 CI react loop smoke |
| `scripts/dev/test-coderabbit-react.sh` | PR#302 (modified #303) | phase-9 CodeRabbit react loop smoke |

### Scripts NOT touched by (agentic)-titled PRs — KEEP per strict reading

- `scripts/dev/merge-gates.sh` + `.graphql` + `-prompt.sh` — added by PR#298 (`feat(merge-gates)`) — bash poller, no Smatchet C++ deps; runs against any PR via `gh api graphql`. Still useful for manual PR-gate polling.
- `tests/bats/merge_gates.bats` + `tests/fixtures/merge_gates_*.json` — test the bash poller; stay.

### `tests/fixtures/` — orphans surfaced by 2026-05-21 repo-wide audit

Verified via `grep -rE "AgenticHandoffController|ClaudeCodeLocalRunner|coderabbit-react|ci-react|SEED\.json|CHECK_RUN\.json|handoff-implementer|pr-iterator" tests/ .github/`. The following fixtures drive deleted C++ surface — they're orphaned (no test consumes them post-v1-PR1) but still occupy the tree. Group with v2 deletions:

| Fixture | Role | v2 action |
|---|---|---|
| `tests/fixtures/stub-claude/CMakeLists.txt` + `stub_claude.cpp` | Stub-claude exe for the deleted `ClaudeCodeLocalRunner.test.cpp` | **DELETE entire directory** |
| `tests/fixtures/stub-git-gh/CMakeLists.txt` + `stub_git.cpp` + `stub_gh.cpp` | Stub-`git` + stub-`gh` for the deleted runner's PR-open fallback tests | **DELETE entire directory** |
| `tests/fixtures/stub-ci-claude.sh` | Drives the deleted ci-react `dispatch_source` path | **DELETE** |
| `tests/fixtures/check_run_annotations_sample.json` | Fixture for the deleted `CheckRunWatcher` annotation parser | **DELETE** |
| `tests/fixtures/check_runs_failed_sample.json` | Same | **DELETE** |
| `tests/fixtures/coderabbit_comments_sample.json` | Fixture for the deleted `CoderabbitCommentClassifier.test.cpp` | **DELETE** |
| `tests/fixtures/github_check_run_annotations_sample.json` | Same | **DELETE** |
| `tests/fixtures/github_check_runs_sample.json` | Same | **DELETE** |
| `tests/fixtures/github_pr_comments_sample.json` | Fixture for the deleted PR-comment classifier | **DELETE** |
| `tests/fixtures/github_pr_create_response.json` | Fixture for the deleted runner's PR-open fallback | **DELETE** |
| `tests/fixtures/github_rerun_workflow_response.json` | Fixture for the deleted CI-react workflow-rerun | **DELETE** |
| `tests/fixtures/github_issues_sample.json` | Useful for future v1-PR2 GitHubClient HTTP fixtures (per `agents/project/tracker-backend.md` testing patterns); add a `## Usage` README header noting the new ownership | **KEEP** |

12 fixture files. Audit also covered `Plugins/`, `Locales/`, `tools/`, `.github/workflows/` — all clean.

### Backlog `applied.md` sweep — orphan archived entries

`docs/self-improvement/categories/applied.md` contains historical-archived entries that reference deleted C++ surface (`AgenticHandoffController`, `ClaudeCodeLocalRunner`, `coderabbit-react-loop`, `handoff-implementer.md`, `pr-iterator.md`). Per AGENTS.md's "archive-don't-rewrite" convention for applied entries, edit each to add a `[2026-05-21 — runtime referenced has been removed by v1 PR1 of github-tracker-backend.md (b1d241bc); this entry is preserved for historical accuracy of what was tried]` annotation. Don't delete the entries — they're history.

### Workflows

- `.github/workflows/*` — audit each file. Workflows that dispatch `claude` / `codex` subprocesses or react to CodeRabbit bot patterns: **DELETE**. Generic build / test / perf workflows: **KEEP**.
- `.coderabbit.yaml` — **KEEP** (CR review of PRs still useful for non-agentic work).

### Backlog

- `docs/self-improvement/categories/*.md` — sweep entries that reference deleted C++ surface; either delete the entry or move to `applied.md` with a note. Don't touch general-purpose entries.

## Sequencing

1. **Wait for v1 PR1 + PR2 to merge.** (PR1 #356 = MERGED `b1d241bc` 2026-05-21; PR2 #357 = MERGED `cd66e28c` 2026-05-21.)
2. **Wait for the 3 in-flight CR-feedback PRs to merge** (PR #358 = `fix/cr-feedback-pr357` for PR2's 5 missed findings; PR #359 = `fix/cr-feedback-351-353` for the code-color slices' missed findings; PR #360 = `feat/merge-gates-cr-strict` for the two P1 process fixes). All three touch `AGENTS.md` regions v2 also edits — starting v2 before they merge creates cascading merge conflicts in the same sections. As of 2026-05-21 evening, all three are ready (not draft) and waiting for real CR review.
3. **Grill v2 plan** via `grill-with-docs` once concrete decisions need locking (which non-(agentic)-titled sections to strip; how aggressive on workflow deletion; ADR status policy). _Locked 2026-05-21 evening re-grill — see § Context._
4. **Architect pre-code review** before opening v2 PR.
5. **One squashed v2 PR** for all doc + agent file + ADR + script deletions. Net negative LOC; reviewable as a single mechanical sweep.

**Removed 2026-05-21 evening**: prior step 2 was "Wait for the watcher P1" — written when the watcher was unspec'd and the plan hedged that its reuse surface might force v2 to preserve more than expected. Now that [`docs/plans/shipped/smatchet-merge-watcher.md`](archive/smatchet-merge-watcher.md) (`8677e8b`) makes the watcher's reuse surface concrete — it imports exactly `scripts/dev/merge-gates.sh` (kept) + `agents/core/coderabbit-triage.md` (kept per re-grill decision 1) + nothing else from the strip list — the hedge is empty. V2's strip list is insulated against every watcher IPC choice (Python port, `claude --headless` subprocess, MCP-tool). The watcher's open design decisions (registry location, triage failure budget, etc.) don't change which v2 files get stripped.

## Risks / non-goals

**Risks**:
- **Conflict with future re-introduction** — if the agentic flow comes back later, this v2 deletion makes recovery slightly harder (more files to re-author vs un-strip). Accepted; v1 already deleted the C++ side. Git history is the authoritative archive.
- **AGENTS.md doc-anchor regressions** — AGENTS.md has anchor-audit CI; verify every cross-link survives. Section deletions break links from other files (CONTEXT.md, agents/*.md, docs/plans/active/*.md). Mitigation: anchor-audit CI gate catches it.

**Non-goals**:
- **Re-introducing agentic features** — not this plan.
- **Touching tracker code** — v1 PR2 already shipped the clean GitHub tracker.

## Implementation log
- `a05a5ff0` · #361 (2026-05-21) — agentic ripout doc cleanup: 32 deletions + 9 modifications in one squashed PR, matching this plan's Sequencing step 5. Deleted `agents/handoff-implementer.md`, `agents/pr-iterator.md`, `docs/plans/active/agentic-{coding-handoff,flow-implementation,triage-flow}.md`, the `docs/agentic/` dir, 11 `test-*.sh` scripts, and 12 fixtures. Surgically edited `agents/core/coderabbit-triage.md` + `agents/core/security-review.md`; marked `docs/plans/shipped/coderabbit-react-loop.md` HISTORICAL; withdrew ADR 0004 + 0005; trimmed AGENTS.md / `docs/CONTEXT.md` / delegation doc.

## Deviations from plan
- **Plan-doc paths drifted post-ship.** Phase B (#543) + Phase D (#545) later relocated `docs/design/` → `docs/plans/` and flattened agent paths, so the historical commit references the pre-rename flat `agents/*.md` / `docs/design/*` layout. No content impact — git history is the authoritative archive.
- **Residual C++ orphans were a separate cleanup.** Follow-up #567 (`0060fff3`, 2026-05-30) removed leftover C++ orphans; out of this plan's doc-only scope.

## Verification (actual)
- **Deletions confirmed** present-then-gone via git (all 32 targets absent from the tree).
- **Cross-link integrity:** AGENTS.md doc-anchor audit passed (the named risk) — no dangling links after the section deletions.
- **ADR state:** 0004 → "Withdrawn (2026-05-21)", 0005 → "Withdrawn-as-partial (2026-05-21)" — both verified in `docs/adr/`.
- **Build gate:** N/A — docs + scripts + fixtures only.
