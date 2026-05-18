# Agentic Flow — Unified Implementation Plan

> **Slug:** `agentic-flow-implementation`.
> **Canonical home:** `docs/design/agentic-flow-implementation.md`. Committed immediately per AGENTS.md § "Plan-doc safety" with `wip(plan): agentic-flow-implementation` before any code lands.
> **Reads as:** an overnight runbook. Every open question is resolved here; deferral conditions are explicit; build / test / commit / push / PR commands are copy-paste-ready.
> **Companion plans (read these for design rationale, not duplicated below):**
> - [`docs/design/agentic-triage-flow.md`](agentic-triage-flow.md) — triage half (phases T0–T9 below).
> - [`docs/design/agentic-coding-handoff.md`](agentic-coding-handoff.md) — handoff half (phases H0–H10 below).
> This plan does **not** restate the architecture; it sequences the work, locks every open decision, and gives the implementing agent everything needed to ship slices unattended.

## Mission

Ship the agentic flow end-to-end in **two halves, in order**:

1. **Triage half first** — `agentic-triage-flow` phases T0 → T9. Produces approved `AgentProposal` rows, with `ImplementIssue` as the only handoff-eligible action.
2. **Handoff half second** — `agentic-coding-handoff` phases H0 → H10. Consumes `ImplementIssue` proposals, spawns Claude Code, drives PR to merge.

Triage half lands first because handoff phase H5 (clarification GitHub-issue-comment path) and H6 (PR open) have hard dependencies on triage phase T2 (`GitHubClient` write methods) and T1 (`ITrackerClient::FetchIssueComments` virtual). Handoff cannot start until at least T2 has merged into `develop`.

## Pre-flight (run once, before any phase)

```bash
# 1. Verify no overlap with active locks.
bash scripts/dev/locks-show.sh
# Expected at time-of-writing: only `ai-client-test-override` holds a lock,
# targeting `Source_Core/include/AiClientFactory.h` + 9 siblings. Confirm your
# planned write set does not include `AiClient*` files; this plan does not.

# 2. Claim the plan-lock for this implementation.
cat > /tmp/agentic-flow-write-set.txt <<'EOF'
Source_Core/include/ITrackerClient.h
Source_Core/include/AgentProposal.h
Source_Core/include/AgentProposalStore.h
Source_Core/src/AgentProposalStore.cpp
Source_Core/include/AgenticInferenceClient.h
Source_Core/src/AgenticInferenceClient.cpp
Source_Core/include/AgenticInferenceClientPure.h
Source_Core/src/AgenticInferenceClientPure.cpp
Source_Core/include/AgenticTriageController.h
Source_Core/src/AgenticTriageController.cpp
Source_Core/include/GitHubClient.h
Source_Core/src/GitHubClient.cpp
Source_Core/include/GitHubClientHelpers.h
Source_Core/src/GitHubClientHelpers.cpp
Source_Core/src/SmatchetAgentProposalsUi.cpp
Source_Core/src/SmatchetAgentProposalsUi.h
Source_Core/src/Commands/Builtin/BuiltinCommands_Agentic.cpp
Source_Core/src/Commands/Scenarios/AgentTriageScenarioStep.cpp
Source_Core/include/SubprocessCapture.h
Source_Core/src/SubprocessCapture.cpp
Source_Core/include/SubprocessCapturePure.h
Source_Core/src/SubprocessCapturePure.cpp
Source_Core/include/CodingHarnessTypes.h
Source_Core/include/ICodingHarnessRunner.h
Source_Core/include/CodingHarnessSeedBuilder.h
Source_Core/src/CodingHarnessSeedBuilder.cpp
Source_Core/include/ClaudeCodeLocalRunner.h
Source_Core/src/ClaudeCodeLocalRunner.cpp
Source_Core/include/HarnessRunState.h
Source_Core/include/AgenticHandoffController.h
Source_Core/src/AgenticHandoffController.cpp
Source_Core/include/PrCommentWatcher.h
Source_Core/src/PrCommentWatcher.cpp
Source_Core/src/SmatchetAgentHandoffUi.h
Source_Core/src/SmatchetAgentHandoffUi.cpp
Source_Core/src/Commands/Builtin/BuiltinCommands_Handoff.cpp
Source_Core/src/Commands/Scenarios/AgentHandoffScenarioStep.cpp
Source_Core/src/P4Blame.cpp
Source_Core/src/AppController.cpp
Source_Core/include/AppController.h
Source_Core/include/ConfigManager.h
Source_Core/src/ConfigManager.cpp
Source_Core/src/Commands/BuiltinCommands.cpp
Source_Core/src/SmatchetUI.cpp
Source_Core/src/SmatchetUI_MainMenu.cpp
Source_Core/src/SmatchetPreferencesUi.cpp
Source_Core/src/DefaultTrackerBackendFactory.cpp
AGENTS.md
agents/handoff-implementer.md
agents/pr-iterator.md
docs/CONTEXT.md
docs/adr/0003-github-as-itrackerclient.md
docs/adr/0004-pluggable-coding-harness-runner.md
tests/CMakeLists.txt
tests/Source_Core/GitHubClientHelpers.test.cpp
tests/Source_Core/AgenticInferenceClientPure.test.cpp
tests/Source_Core/AgentProposalStore.test.cpp
tests/Source_Core/SubprocessCapturePure.test.cpp
tests/Source_Core/SubprocessCapture.test.cpp
tests/Source_Core/CodingHarnessSeedBuilder.test.cpp
tests/Source_Core/HarnessRunState.test.cpp
tests/Source_Core/AgenticHandoffController.test.cpp
tests/Source_Core/ClaudeCodeLocalRunner.test.cpp
tests/Source_Core/PrCommentWatcher.test.cpp
tests/Source_Core/HandoffAgentFrontmatter.test.cpp
tests/Source_Core/PrIteratorAgentFrontmatter.test.cpp
tests/_helpers/agent_frontmatter_parse.h
tests/_helpers/agent_frontmatter_parse.cpp
tests/_helpers/subproc_helper_exit.cpp
tests/_helpers/subproc_helper_sleep.cpp
tests/_helpers/subproc_helper_flood.cpp
tests/fixtures/stub-claude/stub_claude.cpp
tests/fixtures/stub-claude/CMakeLists.txt
tests/fixtures/github_issues_sample.json
tests/fixtures/ollama_chat_sample.json
tests/fixtures/handoff_seed_sample.json
tests/fixtures/pr_comments_sample.json
tests/ui/agent_proposals_panel.test.cpp
tests/ui/agent_proposals_handoff_button.test.cpp
tests/ui/agent_handoff_panel.test.cpp
tests/ui/ui_tests_registry.cpp
scripts/dev/test-agentic-triage-cli.sh
scripts/dev/test-agentic-approve-reject.sh
scripts/dev/test-agentic-handoff-cli.sh
scripts/dev/test-agentic-handoff-clarification.sh
scripts/dev/test-agentic-handoff-iterate.sh
scripts/dev/test-ui-agent-proposals.sh
scripts/dev/test-ui-agent-handoff.sh
EOF
AGENT_ID=orchestrator \
LOCK_BRANCH=feat/agentic-flow \
LOCK_PLAN=docs/design/agentic-flow-implementation.md \
LOCK_NOTES="unified triage + handoff implementation" \
bash scripts/dev/lock-claim.sh agentic-flow /tmp/agentic-flow-write-set.txt

# 3. Create the work branch off latest develop.
git fetch origin develop
git switch -c feat/agentic-flow origin/develop

# 4. Sanity-check the build is green before any edits.
cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12

# 5. Sanity-check the doctest rig is green.
cmake --build --preset ninja-test-msys2 && ctest --preset ninja-test-msys2 --output-on-failure
```

If step 4 or 5 fails, **stop**. Surface the failing target/test to the user; do not attempt to fix unrelated build breaks inside this plan's worktree.

## Decisions locked (all open questions resolved — no further prompts)

Every open question from both companion plans is resolved below. Implementer must not re-litigate or re-prompt — these are the answers.

### From `agentic-triage-flow.md` § "Open questions"

1. **Issue-key encoding** — `owner/repo#N`. Parser in `GitHubClientHelpers::ParseGitHubIssueKey(s) → {owner, repo, number}`. Round-trip via `FormatGitHubIssueKey(owner, repo, number)`. Both pure, both doctest-covered in phase T1.
2. **Bearer-auth header construction** — inline inside `GitHubClient` for phase T2. Do **not** touch `BuildTrackerHeaders`. If a second bearer-auth backend lands later, promote to a shared helper then — not now.
3. **Inference response schema** — phase T3 locks this exact shape (the parser rejects extras with `LOG_WARN` once-latched per field, accepts unknown enum values as `ProposedAction::Unknown` and drops the proposal):
   ```json
   {
     "proposals": [
       {
         "action": "CommentAdd | LabelAdd | LabelRemove | AssigneeSet | StateTransition | DerivedTicketCreate | ImplementIssue",
         "rationale": "free-form one-paragraph explanation",
         "payload": { /* action-specific object, see below */ }
       }
     ]
   }
   ```
   Per-action payload shapes:
   - `CommentAdd`: `{ "body": "string" }`
   - `LabelAdd` / `LabelRemove`: `{ "label": "string" }`
   - `AssigneeSet`: `{ "user": "github-username" }`
   - `StateTransition`: `{ "state": "open | closed" }`
   - `DerivedTicketCreate`: `{ "targetTracker": "jira | plane", "summary": "string", "description": "string" }`
   - `ImplementIssue`: `{ "complexityHint": "low | medium | high", "approachOutline": "free-form" }`
4. **Poll cursor durability** — SQLite. New `agent_poll_cursor` table created in phase T4 alongside `agent_proposals`. Schema: `(source_tracker TEXT, repo_key TEXT, last_seen_updated_at INTEGER, PRIMARY KEY(source_tracker, repo_key))`. Bumped on every successful poll round.

### From `agentic-coding-handoff.md` § "Open questions"

1. **Branch naming** — `agent/<proposalId>/<short-slug>`. Short-slug = first 32 chars of `kebab-case(issueTitle)`. Implemented in `AgenticHandoffController::DeriveBranchName(proposalId, issueTitle)`. Pure, doctested.
2. **Worktree root** — `.claude/worktrees/agent-<proposalId>` (consistent with existing `.claude/worktrees/` convention; already gitignored).
3. **`claude` CLI flag set** — already verified + locked in the companion plan: `claude --print --output-format stream-json --verbose --permission-mode bypassPermissions --append-system-prompt-file SEED.md "<positional prompt>"`. Implementer runs `claude --help | grep -E 'append-system-prompt-file|permission-mode|output-format|verbose|print'` at phase H3 start. If **any** flag is missing in the local install, **stop the phase** and surface a flag-drift report to the user — do not improvise alternates.
4. **Bot-comment-filter** — comment-prefix marker `<!-- smatchet-handoff -->` injected by `ClaudeCodeLocalRunner` on every PR comment it posts (clarification, iteration notice, budget-exhausted). `PrCommentWatcher::ShouldSkipComment(body)` returns true on prefix match. Resilient against multi-account environments (gh-auth user identity may differ from PR author).
5. **Auto-start on approve** — default `false`. User explicitly clicks `[Start handoff]` per approval. Locked. Do not flip the default without user sign-off.
6. **PR iteration budget** — `pr_iteration_budget = 10`. Locked.
7. **Env passthrough** — resolved: env allow-list `{PATH, HOME, USER, USERPROFILE, TEMP, TMP, SYSTEMROOT, GH_TOKEN, GITHUB_TOKEN, ANTHROPIC_API_KEY}` lifted into the Permission-mode decision row. `SMATCHET_*` never inherits.

### New decisions (not in either companion plan — locked here)

8. **Frontmatter doctest gap** — the companion plan's H2/H7 frontmatter assertions named `AgentsMdLoader` as the parser, but [`Source_Core/include/AgentsMdLoader.h`](../../Source_Core/include/AgentsMdLoader.h) is a layered text loader with **no YAML parser**. Resolution: ship a tiny pure-logic frontmatter parser at `tests/_helpers/agent_frontmatter_parse.{h,cpp}` (regex-based, test-only, C++14, no deps). Header API:
   ```cpp
   namespace smatchet::test::frontmatter {
       struct ParsedFrontmatter {
           std::string name;
           std::string description;
           std::string complexity;       // low | medium | high
           bool readOnly = false;
           std::vector<std::string> triggers;
           std::vector<std::string> delegatesTo;
           int version = 0;
           // raw key -> value (single-line scalars only) for assertions on extra keys
           std::unordered_map<std::string, std::string> rawScalars;
       };
       // Returns false + sets `outError` on missing `---` fences or required keys.
       bool ParseFrontmatter(const std::string& filePath, ParsedFrontmatter& out, std::string& outError);
   }
   ```
   Doctest in phase H2: `HandoffAgentFrontmatter.test.cpp` asserts `name == "handoff-implementer"`, `complexity == "medium"`, `readOnly == false`, `version == 1`. Symmetric assertions in phase H7 for `pr-iterator.md`.
9. **Frontmatter regex tolerance** — the parser accepts both block sequences (`triggers:` followed by `  - "x"` lines) and flow sequences (`triggers: ["x", "y"]`). Real agent files use block-form; the parser does not depend on yaml-cpp.
10. **Subprocess helper exes** — built at `tests/_helpers/` per the companion plan revision. `tests/CMakeLists.txt` adds three `add_executable` targets (`subproc_helper_exit`, `subproc_helper_sleep`, `subproc_helper_flood`) plus the stub-claude target. All four exes get `target_compile_definitions(SmatchetTests PRIVATE SMATCHET_TEST_HELPER_DIR="$<TARGET_FILE_DIR:subproc_helper_exit>")` so the test rig finds them via a single dir.
11. **Per-slice PR strategy** — **one PR per phase** for review traceability; each PR squash-merged into `develop` by `git-janitor` (or manually). PR titles follow `feat(agentic): T<n> <slice-summary>` for triage half and `feat(agentic): H<n> <slice-summary>` for handoff half. Phase-table rows below carry the exact title for each phase.
12. **Slice-boundary build cadence** — per AGENTS.md § "Build / ctest cadence — slice-boundary only": exactly **one** `cmake --build` + **one** `scripts/dev/test-all.sh` per phase, at the end. Mid-phase rebuilds are forbidden unless the previous build broke and you are diagnosing the breakage.
13. **Lint cadence** — deferred drain per AGENTS.md. `.claude/hooks/lint-cpp-drain.sh` runs at Stop. Do not run `clang-tidy` / `cppcheck` mid-phase.
14. **Deferral budget** — implementer may abort + surface a phase iff: (a) the slice-boundary build fails on a non-trivial diagnosis (> 30 min triage); (b) a test fails for reasons not addressed by the test's intent (e.g. a SQLite migration that breaks an unrelated test); (c) a phase-N requires a phase-(N-1) artifact that did not land. Otherwise: keep going.
15. **Stop conditions** (hard halts requiring user input — surface and pause):
    - `claude --help` flag-drift at H3 (decision 3 above).
    - GitHub PAT not present in `cfg.GitHubPat` at T2 — first non-pure phase requiring real HTTP.
    - Active lock collision on a write-set file held by another slice.
    - Any sanitizer (ASan / UBSan) finding in `ninja-debug-msys2-asan` at H10 or T9.
    - Pillar 3 (crash) regression in any phase.
16. **Working branch on resume** — if the implementer is rebooted or session-resumed: `git switch feat/agentic-flow && git pull --ff-only` and continue from the first non-committed phase (check `git log --oneline origin/develop..feat/agentic-flow` for already-shipped phases).

## Sequenced phases

Phases are **strictly ordered**. Implementer may not parallel-ship triage vs handoff slices — the handoff half consumes triage outputs at the source level.

Legend: ⛓️ = hard dep on prior phase output. 🌀 = pure-logic only (fastest slice). 🖥️ = adds UI surface. 🧪 = test-only artifact.

### Triage half — phases T0 → T9

Detailed scope per phase is in [`agentic-triage-flow.md`](agentic-triage-flow.md) § "Phased rollout". This table adds the runbook overlay.

| Phase | PR title | Lines from triage plan | Build target | Test command | Lock-claim update |
|---|---|---|---|---|---|
| **T0** | `docs(agentic): T0 plan-lock + ADRs + glossary` | T0 row | `ninja-iter-msys2` (doc-only, just runs configure) | none | n/a (initial claim done) |
| **T1** 🌀 | `feat(agentic): T1 ITrackerClient::FetchIssueComments + GitHubClient skeleton` | T1 row | `ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`; `ninja-test-msys2` | `bash scripts/dev/test-all.sh` | n/a |
| **T2** ⛓️T1 | `feat(agentic): T2 GitHubClient write methods + audit-trail wiring` | T2 row | `ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`; `ninja-test-msys2` | `bash scripts/dev/test-all.sh` | n/a |
| **T3** 🌀 | `feat(agentic): T3 AgenticInferenceClient + schema validation` | T3 row | `ninja-iter-msys2 --target SmatchetStandalone`; `ninja-test-msys2` | `bash scripts/dev/test-all.sh` | n/a |
| **T4** | `feat(agentic): T4 AgentProposal + AgentProposalStore (SQLite)` | T4 row | `ninja-iter-msys2`; `ninja-test-msys2` | `bash scripts/dev/test-all.sh` | n/a |
| **T5** ⛓️T2,T3,T4 | `feat(agentic): T5 AgenticTriageController + agent.triage.run` | T5 row | `ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`; `ninja-test-msys2` | `bash scripts/dev/test-all.sh && bash scripts/dev/test-agentic-triage-cli.sh` | n/a |
| **T6** 🖥️ | `feat(agentic): T6 SmatchetAgentProposalsUi + bucket-E test` | T6 row | `ninja-iter-msys2`; `ninja-ui-test-msys2` | `bash scripts/dev/test-all.sh && bash scripts/dev/test-ui-agent-proposals.sh` | n/a |
| **T7** ⛓️T5 | `feat(agentic): T7 scheduled poll + preferences toggle` | T7 row | `ninja-iter-msys2`; `ninja-test-msys2` | `bash scripts/dev/test-all.sh` | n/a |
| **T8** 🧪 | `feat(agentic): T8 scenario step + recorded fixtures` | T8 row | `ninja-iter-msys2`; `ninja-test-msys2` | `bash scripts/dev/test-all.sh` | n/a |
| **T9** | `feat(agentic): T9 schema-version bump + plan revision sections` | T9 row | `ninja-test-msys2`; `ninja-debug-msys2-asan`; `ninja-ui-test-msys2`; `ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` | `bash scripts/dev/test-all.sh` | n/a |

**Triage-half acceptance gate (between T9 and H0):**
- All T0–T9 PRs merged into `develop`.
- `develop` is buildable + test-all green.
- `agent.triage.run --source github --query <fixture>` lands proposals in the SQLite store.
- `SmatchetAgentProposalsUi` shows the proposals with working Approve / Reject buttons.

If the gate is not met: **stop**. Do not start H0. Surface the unmet condition to the user.

### Handoff half — phases H0 → H10

Detailed scope per phase is in [`agentic-coding-handoff.md`](agentic-coding-handoff.md) § "Phased rollout". Runbook overlay below.

| Phase | PR title | Lines from handoff plan | Build target | Test command | Lock-claim update |
|---|---|---|---|---|---|
| **H0** | `docs(agentic): H0 plan-lock + ADRs + glossary update` | H0 row | doc-only configure | none | n/a |
| **H1** ⛓️ | `feat(agentic): H1 SubprocessCapture lift from P4Blame + helper exes` | H1 row | `ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`; `ninja-test-msys2` | `bash scripts/dev/test-all.sh` | n/a |
| **H2** ⛓️H1,T4 | `feat(agentic): H2 CodingHarnessTypes + ICodingHarnessRunner + SeedBuilder + agents file` | H2 row | `ninja-iter-msys2`; `ninja-test-msys2` | `bash scripts/dev/test-all.sh` | n/a |
| **H3** ⛓️H1,H2 | `feat(agentic): H3 ClaudeCodeLocalRunner + stub-claude exe + env allow-list test` | H3 row | `ninja-iter-msys2 --target SmatchetStandalone`; `ninja-test-msys2` | `bash scripts/dev/test-all.sh && bash scripts/dev/test-agentic-handoff-cli.sh` | n/a |
| **H4** ⛓️H3,T5 | `feat(agentic): H4 HarnessRunState FSM + AgenticHandoffController + handoff.start command` | H4 row | `ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`; `ninja-test-msys2` | `bash scripts/dev/test-all.sh` | n/a |
| **H5** ⛓️H4,T2 | `feat(agentic): H5 clarification dual-channel + GitHub-comment poll` | H5 row | `ninja-iter-msys2 --target SmatchetStandalone`; `ninja-test-msys2` | `bash scripts/dev/test-all.sh && bash scripts/dev/test-agentic-handoff-clarification.sh` | n/a |
| **H6** ⛓️H4 | `feat(agentic): H6 PR-open path + gh pr create fallback` | H6 row | `ninja-iter-msys2 --target SmatchetStandalone`; `ninja-test-msys2` | `bash scripts/dev/test-all.sh` | n/a |
| **H7** ⛓️H6,T2 | `feat(agentic): H7 PrCommentWatcher + pr-iterator agent file + iteration budget` | H7 row | `ninja-iter-msys2`; `ninja-test-msys2` | `bash scripts/dev/test-all.sh && bash scripts/dev/test-agentic-handoff-iterate.sh` | n/a |
| **H8** 🖥️⛓️H7 | `feat(agentic): H8 SmatchetAgentHandoffUi panel + screenshot diff` | H8 row | `ninja-iter-msys2`; `ninja-ui-test-msys2` | `bash scripts/dev/test-all.sh && bash scripts/dev/test-ui-agent-handoff.sh` | n/a |
| **H9** ⛓️H8,T6 | `feat(agentic): H9 cross-flow wiring + Start-handoff button (ImplementIssue filter)` | H9 row | `ninja-iter-msys2`; `ninja-test-msys2`; `ninja-ui-test-msys2` | `bash scripts/dev/test-all.sh && bash scripts/dev/test-ui-agent-handoff.sh` | n/a |
| **H10** ⛓️H9 | `feat(agentic): H10 scenario + SQLite schema bump + plan revision` | H10 row | `ninja-test-msys2`; `ninja-debug-msys2-asan`; `ninja-ui-test-msys2`; `ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` | `bash scripts/dev/test-all.sh` | release lock (see § "Lock release") |

**Handoff-half acceptance gate (after H10):**
- All H0–H10 PRs merged into `develop`.
- `develop` is buildable + test-all + bucket-E green.
- A real end-to-end probe (handoff plan § "Verification" § "End-to-end happy-path probe" steps 1–8) succeeds against a real `claude` install. **This probe is the only manual step left and runs after merge — not gated on the lock.**

## Per-phase runbook (exact sequence to follow)

For each phase row above, execute this loop. **No deviations** unless a stop condition fires.

```bash
# Step 1 — confirm working branch.
test "$(git branch --show-current)" = "feat/agentic-flow" || git switch feat/agentic-flow
git pull --ff-only origin develop  # rebase-pull onto latest develop

# Step 2 — emit progress marker.
bash scripts/dev/agent-progress.sh "phase:start <phase-id> — <slice summary>"

# Step 3 — implement. Read the matching row in the companion plan's phase table for scope.
#          Touch only files in the row's "Key files" column. Add tests / fixtures in the same slice.
#          No mid-phase build. No mid-phase ctest.

# Step 4 — slice-boundary build (replace <preset> with the row's "Build target").
bash scripts/dev/agent-progress.sh "phase:gate building"
cmake --build --preset <preset>  # repeat for each preset in the row

# Step 5 — slice-boundary test (replace <cmd> with the row's "Test command").
bash scripts/dev/agent-progress.sh "phase:gate testing"
<test command>

# Step 6 — drain deferred lint (per AGENTS.md § "Build / ctest cadence").
bash scripts/dev/lint-flush.sh

# Step 7 — commit.
bash scripts/dev/agent-progress.sh "phase:commit"
git add <files-touched-this-phase>
git commit -m "$(cat <<'EOF'
<row PR title>

<one-paragraph summary of the slice — what shipped, why, refs to the companion plan row>

Companion plan: docs/design/<triage|handoff>-...md § Phased rollout row <T|H><n>.
Unified plan: docs/design/agentic-flow-implementation.md.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"

# Step 8 — push + open PR.
bash scripts/dev/agent-progress.sh "phase:push"
git push -u origin feat/agentic-flow
bash scripts/dev/agent-progress.sh "phase:pr"
gh pr create --draft --base develop --title "<row PR title>" --body "$(cat <<'EOF'
## Summary

<one-paragraph slice summary — same as commit body>

## Companion plan reference

- Triage / handoff plan: `docs/design/<triage|handoff>-...md` § Phased rollout row `<T|H><n>`.
- Unified plan: `docs/design/agentic-flow-implementation.md`.

## Test plan

- [x] `cmake --build --preset <preset>` green
- [x] `<test command>` green
- [x] Deferred lint drain clean (`bash scripts/dev/lint-flush.sh`)

## Lock

`lock-slug: agentic-flow`

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"

# Step 9 — final progress marker for the phase.
bash scripts/dev/agent-progress.sh "phase:end <phase-id>"

# Step 10 — wait for green CI + merge approval. If CI red and the failure is in the slice:
#           fix-forward in the same branch with a follow-up commit. Do not amend a pushed commit.
#           After merge, `git pull --ff-only origin develop` and start the next phase.
```

## Stop / pause conditions (recap, with what to surface)

1. **`claude --help` flag drift at H3** → surface the diff of expected vs actual flag set. Wait for user.
2. **GitHub PAT missing at T2** → surface "set `cfg.GitHubPat` in `smatchet_config.json` and resume by re-running the phase". Wait.
3. **Active lock collision on a write-set file** → surface the conflicting slug + holder + first overlapping path. Wait.
4. **Sanitizer finding at H10 or T9** → surface the ASan / UBSan stanza verbatim. Do **not** suppress.
5. **Pillar 3 (crash) regression in any phase** → surface the repro + stack. Wait.
6. **CI red on a merged PR** → surface the failing job + log link. Do not auto-revert.

For every stop condition: emit a `phase:halted <reason>` progress line + a `## Outcome: halted` section in the report.

## Lock release

After H10 merges to `develop`:

```bash
AGENT_ID=orchestrator bash scripts/dev/lock-release.sh agentic-flow
bash scripts/dev/locks-show.sh  # confirm `agentic-flow` is gone
```

If any PR in this plan carries the line `lock-slug: agentic-flow` in its body, [`.github/workflows/lock-cleanup.yml`](../../.github/workflows/lock-cleanup.yml) auto-releases on merge — the manual `lock-release.sh` call is belt-and-braces.

## Plan revision on completion

When H10 merges, edit this file in the same or next commit per AGENTS.md § "Plan revision after implementation":

- `## Implementation log` — one bullet per merged PR: `<sha> · <PR title>`.
- `## Deviations from plan` — every decision changed during implementation vs the locked decisions above, with one-line rationale.
- `## Verification` — the end-to-end probe outcome (passed / failed / skipped with reason).

## End-of-night handoff to user

After overnight work:

- If all 21 PRs (T0–T9 + H0–H10) merged: the morning report is `## Outcome: applied` with a list of merged SHAs + the lock-release confirmation. The only residual step is the end-to-end probe against a real GitHub repo + a real `claude` install — user runs this in the morning per handoff plan § "End-to-end happy-path probe".
- If a phase halted: `## Outcome: halted` with the stop condition, the offending diff / log, and the suggested resolution. The branch + open PRs are left intact — user resumes after addressing the stop condition by re-running step 1 of the per-phase runbook on the failing phase.
- If a phase failed (build / test / sanitizer regression that the implementer could not fix-forward within the deferral budget): `## Outcome: failed` with the exact failing command + last 50 lines of output. Do not auto-revert merged work.

## Risks + mitigations (cross-cutting)

Both companion plans list their own. The cross-cutting risks unique to running both halves back-to-back overnight:

- **Develop drift** between triage half and handoff half. Mitigation: every phase runs `git pull --ff-only origin develop` before implementation (per-phase runbook step 1). Conflicts surface immediately, not at PR time.
- **Lock starvation** if another slice claims a file in this plan's write set mid-night. Mitigation: pre-claim covers every file (see `/tmp/agentic-flow-write-set.txt`). If the claim push is rejected because another slug grabbed the slug name first, **stop** — do not improvise a renamed slug.
- **CI flake**. Mitigation: re-run the failing job once via `gh run rerun <run-id>`. A second failure is treated as red.
- **Stub-claude divergence from real claude**. Mitigation: the stub exe is a thin emitter; phase H10's real-claude probe is the truth gate. Any feature added on top of the stub gets re-verified against real claude before claiming complete.
- **`bypassPermissions` blast radius** (see handoff plan § "Decisions locked" row "Permission mode"). Phase H3's env-allow-list assertion is the test that locks the boundary; do not loosen it.

## File ownership (who writes what)

This plan's write set covers both companion plans' files in their entirety. The implementer is the sole writer; **no subagent delegation is required for the per-phase work**. Subagents may be spawned within a phase only for:

- **`build-doctor`** — if the slice-boundary build fails for a CMake / preset / link / LTO reason and 15 min of triage does not resolve it.
- **`test-author`** — if a phase's test row is materially harder than the companion plan suggests (e.g. a bucket-E test that needs new test-engine wiring). Per AGENTS.md § "Verification automation".
- **`debug-detective`** — only on a Pillar-3 crash regression. Triggers the pause-loop per AGENTS.md § "Debug-mode pause-loop".

All three subagents inherit the orchestrator's lock and branch.

## Glossary additions (phase T0 + H0 write these into `docs/CONTEXT.md`)

`docs/CONTEXT.md` does not exist yet. Phase T0 creates it with the minimum set of terms used across both halves. Each subsequent phase appends only on first introduction. Initial entries (phase T0):

- **AgentProposal** — record-of-an-LLM-suggestion-awaiting-human-approval. Stored in SQLite (`agent_proposals` table). One per LLM-emitted item. Lifecycle: `Pending → Approved | Rejected → Applied | Failed`.
- **ImplementIssue** — the single `AgentProposal.proposedAction` enum value that consents to a coding-harness handoff. All other actions stay triage-only.
- **Triage half** — `agentic-triage-flow` phases T0–T9. Produces proposals.
- **Handoff half** — `agentic-coding-handoff` phases H0–H10. Consumes `ImplementIssue` proposals, spawns Claude Code, drives PR to merge.
- **Coding harness runner** — implementer of `ICodingHarnessRunner`. Phase-1 concrete is `ClaudeCodeLocalRunner`; cloud / Codex / Aider runners are deferred.
- **Sentinel files** — single-writer single-reader JSON files in the harness's worktree (`SEED.json`, `CLARIFICATION_NEEDED.json`, `USER_RESPONSE.json`, `RUN_RESULT.json`, `ERROR.json`, `PR_URL.txt`). Vocabulary defined in `AGENTS.md § Handoff envelope`.
- **Handoff envelope** — the contract between the Smatchet-side `ClaudeCodeLocalRunner` and the spawned-harness-side first delegate (`handoff-implementer`). Documented in `AGENTS.md § Handoff envelope`.

Phase H0 appends:

- **HarnessRunState** — FSM tracking a single handoff lifecycle. States: `Pending → Spawning → Running → AwaitingUser ↔ Running → PrOpen ↔ Iterating → Complete | Failed | Cancelled`.
- **PR iteration budget** — `pr_iteration_budget = 10`; cap on how many times `PrCommentWatcher` re-spawns the harness in response to PR comments before forcing user attention.

## ADRs (phase T0 writes 0003; phase H0 writes 0004)

- **`docs/adr/0003-github-as-itrackerclient.md`** — why GitHub is implemented as an `ITrackerClient` backend rather than a parallel abstraction. Trade-off: some methods (JQL, sprints, worklog) return documented "unsupported" sentinels instead of mapping naturally. Justified because the agent core only talks `ITrackerClient` and a parallel abstraction would force a second adapter layer.
- **`docs/adr/0004-pluggable-coding-harness-runner.md`** — why `ICodingHarnessRunner` is an interface with `ClaudeCodeLocalRunner` as phase-1 concrete. Allows later cloud / Codex / Aider drop-ins without controller changes. Documents that `bypassPermissions` is not an OS sandbox and the runner's env + cwd are the only boundary.

Both ADRs follow the `docs/adr/0001-…` / `0002-…` template (decision, context, consequences).

## Implementation log

Append-only record per AGENTS.md § Plan revision after implementation. One bullet per shipped phase.

- **T6** (PR pending) · `feat(agentic): T6 SmatchetAgentProposalsUi + bucket-E test` — new ImGui panel `SmatchetAgentProposalsUi::Render` renders Pending rows from `AgentProposalStore::Query` with green Approve / red Reject buttons (drive `Transition` on the UI thread; SQLite UPDATE is sub-ms so no worker). Menu toggle wired into View under `SMATCHET_WITH_AGENTIC`. Bucket-E coverage in `tests/ui/agent_proposals_panel.test.cpp` (Empty / ListsPending / Approve / Reject variants). Runner: `scripts/dev/test-ui-agent-proposals.sh`.
- **T7** (PR pending) · `feat(agentic): T7 scheduled poll + Preferences toggle` — `AppController` owns `agenticPollThread_` + `agenticPollCv_` + `agenticPollLastAtSec_`. Worker spawns from `StartAgenticPollIfEnabled` (called at end of `Initialize` and from `RestartAgenticPoll`), joins via `StopAgenticPoll` (top of destructor). Cursor-aware: `GitHubClient::ListOpenIssuesForRepo` gains optional `sinceUnixSec` param; worker reads `agent_poll_cursor.last_seen_updated_at`, passes to GitHub's `since=` filter, writes wall-clock back on success. Preferences "Agentic" tab carries master toggle + interval (60..3600 clamp) + source combo (greyed "github" only) + query + GitHub PAT (password input) + last-poll / next-poll readout + "Run triage now" button (LaunchBackgroundTask). New helper `GitHubClientHelpers::FormatUnixSecAsIso8601` with round-trip doctest. `RunAgenticTriageOnce` exposes a synchronous manual trigger.
- **T8** (PR pending) · `feat(agentic): T8 scenario step + recorded fixtures` — new `AgentTriageScenario` (`Source_Core/src/Commands/Scenarios/AgentTriageScenarioStep.cpp`) drives `AgenticTriageController` against in-process mock `IGitHubReadClient` + `IInferenceClient` seams across a 4-frame state machine: TriageBatch → query Pending rows → Pending→Approved → Pending→Rejected. Factory registered behind `SMATCHET_WITH_AGENTIC` in `AppController::Initialize` alongside `whisper-dictation-roundtrip`. Pure fixture-loader pair `AgentTriageScenarioFixtures::{ParseGitHubIssueCommentsFixture, ParseOllamaProposalsFixture, ParseIso8601Utc}` extracted to a separate TU so the test-delta gate is satisfied by `tests/Source_Core/AgentTriageScenarioFixtures.test.cpp` (5 cases — happy path, malformed JSON, non-array root, sparse fields, ISO-8601 strict-Z parser). Auto-enrolled runner `scripts/dev/test-agentic-approve-reject.sh` invokes `scenario.run --name=agent-triage-roundtrip --spawn` and asserts `data.passed==true`; skips cleanly when AGENTIC=OFF. Scenario uses `:memory:` SQLite + zero live HTTP / LLM traffic, so CI burns zero credits.

## Deviations from plan

- **T6 — issue-title omitted from row header.** The plan-locked decision (#5) allowed deferring issue-title rendering pending T7's `FetchIssueBody` integration. Row header therefore shows `<issueKey>  [<action>]` only. Re-evaluate when T7 lands if the title becomes worth a per-row HTTP fetch (probably not — `issueKey` is already disambiguating for triage scan).
- **T6 — refresh cadence is TU-static, not a UiDrawSession field.** The panel owns its own polling cadence (1 Hz steady-clock gate) at TU scope to avoid leaking polling state into `UiDrawSession` for a feature with no other consumers. A future poll-driven update (T7) may move this into session state if shared with another caller.
- **T7 — issue-title still deferred (not adopted in this slice).** T6's deviation stands; T7 stays scoped to the worker + Preferences. The `FetchIssueBody` body is fetched per-issue during triage already, but threading the title into the Proposals UI panel is a separate render-time decision deferred to a later UX polish slice.
- **T7 — cursor advance uses wall-clock, not max(updated_at) of returned issues.** GitHub guarantees monotonic `updated_at`, so worst-case drift between server + client is bounded by the poll interval (≤ 1 h). Parsing every payload for `updated_at` would add cost with no observable benefit for the intended "didn't see anything older than this" semantic.
- **T7 — discovery routed through the adapter directly (not `TriageBatch`).** The cursor filter is worker-local; `AgenticTriageController::TriageBatch` calls `ListOpenIssuesForRepo` with the default `sinceUnixSec=0`. To inject the cursor without threading it through the controller API the worker calls `IGitHubReadClient::ListOpenIssuesForRepo` directly + funnels each returned key through `TriageIssue`. Keeps the cursor surface contained to the scheduled-poll path.

## Verification

- **T6**: dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`) under `ninja-iter-msys2`; `SMATCHET_WITH_AGENTIC=OFF` build of `SmatchetStandalone` confirms the panel + menu item + Draw call fully drop out; `ninja-test-msys2` ctest green (no new doctest — UI logic is bucket-E); `bash scripts/dev/test-ui-agent-proposals.sh` runs the four bucket-E variants against the ephemeral `ninja-ui-test-msys2` exe; `bash scripts/dev/test-all.sh` full sweep.
- **T7**: dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`) under `ninja-iter-msys2`; `SMATCHET_WITH_AGENTIC=OFF` build of `SmatchetStandalone` confirms `agenticPollThread_` / `RestartAgenticPoll` / `RunAgenticTriageOnce` / `AgenticPoll*` config fields all drop out; `ninja-test-msys2` ctest green including new `FormatUnixSecAsIso8601` round-trip cases in `GitHubClientHelpers.test.cpp`; `bash scripts/dev/test-agentic-triage-cli.sh` continues to PASS (synchronous CLI path the worker calls); `bash scripts/dev/test-all.sh` full sweep. Bucket-E coverage for the Preferences Agentic tab itself is residue (logged below).
- **T8**: dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`) under `ninja-iter-msys2`; `SMATCHET_WITH_AGENTIC=OFF` build of `SmatchetStandalone` confirms `AgentTriageScenarioStep.cpp` + `AgentTriageScenarioFixtures.cpp` drop out cleanly and the `MakeAgentTriageScenario` factory call in `AppController` is fully ifdef-gated; `ninja-test-msys2` ctest green including the new `AgentTriageScenarioFixtures.test.cpp` doctest cases (happy path + malformed JSON + non-array root + sparse fields + ISO-8601 strict-Z); `Smatchet.exe cmd scenario.list` includes `agent-triage-roundtrip`; `bash scripts/dev/test-agentic-approve-reject.sh` PASS against the ON build + clean SKIP against the OFF build; `bash scripts/dev/test-all.sh` full sweep including the new runner.
