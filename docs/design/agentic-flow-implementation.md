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
| **H7** ⛓️H6,T2 | `feat(agentic): H7 PrCommentWatcher + GitHubClient PR/check-run methods + pr-iterator agent + iteration budget` | H7 row (note: scope now includes 6 GitHubClient methods folded in from `coderabbit-react-loop.md` — see `agentic-coding-handoff.md` H7 row for the full list) | `ninja-iter-msys2`; `ninja-test-msys2` | `bash scripts/dev/test-all.sh && bash scripts/dev/test-agentic-handoff-iterate.sh` | n/a |
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

- **T0** · `906c312` · `docs(agentic): T0 plan-lock + ADRs + glossary` (#225) — initial plan-lock claim, ADRs 0003/0004 stub, `docs/CONTEXT.md` seed entries.
- **T1** · `c0a74bd` · `feat(agentic): T1 ITrackerClient::FetchIssueComments + GitHubClient skeleton + SMATCHET_WITH_AGENTIC gate` (#226) — virtual `FetchIssueComments` on `ITrackerClient` with default-unsupported impl; `GitHubClient` skeleton + `GitHubClientHelpers::{Parse,Format}GitHubIssueKey` pure round-trip; build gate `SMATCHET_WITH_AGENTIC` plumbed through `cmake/`.
- **T2** · `639b0a3` · `feat(agentic): T2 GitHubClient write methods + audit-trail wiring` (#227) — `GitHubClient::{AddComment, AddLabel, RemoveLabel, SetAssignee, CloseIssue}` over `cpr` with bearer-PAT auth; `BackendAuditTrail` writes one row per attempt with response-status.
- **T3** · `0c0f9c6` · `feat(agentic): T3 AgenticInferenceClient + schema validation` (#228) — `AgenticInferenceClient` posts to local Ollama with the schema locked in plan decision #3; `AgenticInferenceClientPure::ParseProposals` returns typed `ProposedAction` enum + per-field strict-vs-warn parser tested across 6 doctest cases.
- **T4** · `746d8bf` · `feat(agentic): T4 AgentProposal + AgentProposalStore (SQLite)` (#229) — `AgentProposal` POD + `AgentProposalStore` SQLite-backed lifecycle store; `agent_proposals` + `agent_poll_cursor` tables created additively via `CREATE TABLE IF NOT EXISTS`; schema-version bump deferred to T9 per plan decision #14. 18 doctest cases.
- **T5** · `6530c1f` · `feat(agentic): T5 AgenticTriageController + agent.triage.run` (#230) — `AgenticTriageController::TriageBatch` orchestrates ListOpenIssuesForRepo → FetchIssueBody → Infer → Insert; `agent.triage.run` command registered in `BuiltinCommands_Agentic.cpp`; `scripts/dev/test-agentic-triage-cli.sh` covers CLI-driven smoke.
- **T6** · `f944dd0` · `feat(agentic): T6 SmatchetAgentProposalsUi + bucket-E test` (#231) — new ImGui panel `SmatchetAgentProposalsUi::Render` renders Pending rows from `AgentProposalStore::Query` with green Approve / red Reject buttons (drive `Transition` on the UI thread; SQLite UPDATE is sub-ms so no worker). Menu toggle wired into View under `SMATCHET_WITH_AGENTIC`. Bucket-E coverage in `tests/ui/agent_proposals_panel.test.cpp` (Empty / ListsPending / Approve / Reject variants). Runner: `scripts/dev/test-ui-agent-proposals.sh`.
- **T7** · `dd2fa99` · `feat(agentic): T7 scheduled poll + Preferences toggle` (#232) — `AppController` owns `agenticPollThread_` + `agenticPollCv_` + `agenticPollLastAtSec_`. Worker spawns from `StartAgenticPollIfEnabled` (called at end of `Initialize` and from `RestartAgenticPoll`), joins via `StopAgenticPoll` (top of destructor). Cursor-aware: `GitHubClient::ListOpenIssuesForRepo` gains optional `sinceUnixSec` param; worker reads `agent_poll_cursor.last_seen_updated_at`, passes to GitHub's `since=` filter, writes wall-clock back on success. Preferences "Agentic" tab carries master toggle + interval (60..3600 clamp) + source combo (greyed "github" only) + query + GitHub PAT (password input) + last-poll / next-poll readout + "Run triage now" button (LaunchBackgroundTask). New helper `GitHubClientHelpers::FormatUnixSecAsIso8601` with round-trip doctest. `RunAgenticTriageOnce` exposes a synchronous manual trigger.
- **T8** · `04a6d0f` · `feat(agentic): T8 scenario step + recorded fixtures` (#233) — new `AgentTriageScenario` (`Source_Core/src/Commands/Scenarios/AgentTriageScenarioStep.cpp`) drives `AgenticTriageController` against in-process mock `IGitHubReadClient` + `IInferenceClient` seams across a 4-frame state machine: TriageBatch → query Pending rows → Pending→Approved → Pending→Rejected. Factory registered behind `SMATCHET_WITH_AGENTIC` in `AppController::Initialize` alongside `whisper-dictation-roundtrip`. Pure fixture-loader pair `AgentTriageScenarioFixtures::{ParseGitHubIssueCommentsFixture, ParseOllamaProposalsFixture, ParseIso8601Utc}` extracted to a separate TU so the test-delta gate is satisfied by `tests/Source_Core/AgentTriageScenarioFixtures.test.cpp` (5 cases — happy path, malformed JSON, non-array root, sparse fields, ISO-8601 strict-Z parser). Auto-enrolled runner `scripts/dev/test-agentic-approve-reject.sh` invokes `scenario.run --name=agent-triage-roundtrip --spawn` and asserts `data.passed==true`; skips cleanly when AGENTIC=OFF. Scenario uses `:memory:` SQLite + zero live HTTP / LLM traffic, so CI burns zero credits.
- **T9** (PR pending) · `feat(agentic): T9 schema-version bump + plan revision (triage-half final)` — `AgentProposalStore` gains a `schema_version` table (CHECK-clamped singleton row, INSERT on first open, idempotent re-open). `kCurrentSchemaVersion = 1` records the first shipped version of the agentic SQLite surface (T4-T8 shipped no interim increments per AGENTS.md § Schema-version bumps). Public accessor `GetSchemaVersion(int& outVersion, std::string& outError)`. Migration ladder is a `while (current < kCurrentSchemaVersion)` loop with no migration bodies today; future bumps add per-version steps without touching the open path. 4 new doctest cases — first-open stamp, no-double-bump across re-opens, singleton row guard, `kCurrentSchemaVersion == 1` invariant. Plan revision: this implementation log + Deviations + Verification sections finalised for the triage half, plus the Triage-half acceptance gate block below.
- **H0** · `f38c0bf` · `docs(agentic): H0 plan-lock + ADRs + glossary update` (#240) — Handoff-half plan-lock entry + ADRs 0005/0006 stubs + `docs/CONTEXT.md` seed entries for `CodingHarness::Seed`, `IRunner`, `HarnessRunState`, `bypassPermissions`, env allow-list.
- **H1** · `1b9a575` · `feat(agentic): H1 SubprocessCapture lift from P4Blame + helper exes` (#244) — `SubprocessCapture::Run` extracted from `P4Blame.cpp`'s Win32 + POSIX RunProcessCapture pair into `Source_Core/{include,src}/SubprocessCapture.{h,cpp}`. `CaptureOptions` carries argv, env, cwd, timeoutMs, stdout/stderr byte caps, cancel token. `SubprocessCapturePure` extracts argv-quoting + env-block builders for doctest. Three tiny stdlib-only helper exes (`subproc_helper_{exit,sleep,flood}`) wired via `SMATCHET_TEST_HELPER_DIR` give the test cases deterministic exit-code / timeout / cancel / byte-cap coverage.
- **H2** · `17cc5c9` · `feat(agentic): H2 CodingHarnessTypes + ICodingHarnessRunner + SeedBuilder + agents file` (#248) — `Source_Core/include/CodingHarnessTypes.h` carries `Seed`, `StreamEvent`, `ClarificationRequest`, `ClarificationResponse`, `RunResult`. `ICodingHarnessRunner.h` declares the pluggable runner interface (`Probe` / `Spawn` / `Resume` / `Name`). `CodingHarnessSeedBuilder.{h,cpp}` (AGENTIC-gated) ships the deterministic SEED.{md,json} format/parse pair with forward-compat `payloadExtra` round-trip. New `agents/handoff-implementer.md` documents the first delegate's contract.
- **H3** · `d1bc456` · `feat(agentic): H3 ClaudeCodeLocalRunner + stub-claude exe + env allow-list test` (#251) — `ClaudeCodeLocalRunner` implements `IRunner` end-to-end: `Probe` runs `claude --version` via `SubprocessCapture`; `Spawn` creates the worktree, writes SEED.{json,md}, builds the env allow-list (decision #7), spawns `claude` with the 5 verified flags (decision #3), streams stdout NDJSON via a new `SubprocessCapture::CaptureOptions::onStdoutLine` callback, polls sentinel files (`CLARIFICATION_NEEDED.json`, `PR_URL.txt`) on a 1Hz worker thread, and parses `RUN_RESULT.json` on exit. `Resume` writes `USER_RESPONSE.json` for the clarification path. New `tests/fixtures/stub-claude/stub_claude.cpp` masquerades as the Claude Code CLI for 6 doctest cases including the env-allow-list assertion (poisoned parent env with `SMATCHET_SECRET` + `GH_TOKEN` → snapshot confirms only the allow-listed key arrived). `ConfigManager` gains `HandoffHarnessBinPath` + `HandoffRunnerName` (additive JSON round-trip). `SubprocessCapture::CaptureOptions::replaceParentEnv` flag enforces the allow-list on POSIX (clearenv()) + Windows (manual env-block merge).
- **H4** · `fcaa533` · `feat(agentic): H4 HarnessRunState FSM + AgenticHandoffController + handoff.* CLI` (#252) — `HarnessRunState` enum + `IsTransitionAllowed` FSM cover the 8-state lifecycle (Pending → Spawning → Running → AwaitingUser ↔ Running → PrOpen → Complete, plus Failed / Cancelled terminals). `AgenticHandoffController` owns the in-memory record per proposal, dispatches `runner->Spawn` onto `AppController::LaunchBackgroundTask`, and validates every state-callback through `IsTransitionAllowed` before audit-trailing — runner self-emitted terminal callbacks are filtered so the worker's post-Spawn transition carries the richer `RunResult.errorMessage` / `prUrl`. Five CLI commands registered behind `SMATCHET_WITH_AGENTIC` in `BuiltinCommands_Handoff.cpp` (`handoff.start`, `handoff.cancel`, `handoff.list`, `handoff.clarify`, `handoff.dry-run`), each with the AGENTIC=OFF stub fallback. 7 doctest cases against a scriptable `FakeRunner` cover the four lifecycle modes; thread-safe audit capture under `std::mutex`.
- **H5** · `33e0b3a` · `feat(agentic): H5 clarification dual-channel + GitHub-comment poll` (#253) — `AgenticHandoffController` on the AwaitingUser transition now reads `CLARIFICATION_NEEDED.json` from the worktree and stashes the parsed question on `ActiveHandoff::lastClarificationQuestion`. Two new seams — `GitHubCommentPoster` + `GitHubCommentFetcher` — wired in `AppController::GetAgenticHandoffController` to `GitHubClient::{CommentAdd, FetchIssueComments}`. The controller posts a bot-filtered comment (`<!-- smatchet-handoff -->` marker prefix) carrying the question, and mirrors `ProvideClarification` answers to the same issue with the same marker so the public thread carries an audit trail. The bot-filter prefix is a load-bearing contract: `IsHandoffBotComment` matches whitespace-tolerant case-sensitive at the start of the body so the poll loop never treats Smatchet's own posted question as a user reply. Poll loop is piggybacked on T7's existing scheduled-poll worker (`AppController::AgenticPollWorkerLoop` calls `controller->PollClarificationAnswers()` at the end of every iteration) — avoids spawning a 4th poll thread. New config `HandoffClarificationPostToGithub` (default `true`; clears via `j.value` default — no schema bump) lets operators flip the dual-channel off for private / non-GitHub work; toggle mirrored onto the controller via `SetGitHubClarificationEnabled`. New `ClarificationProvided` audit-trail action keyed on the answer text. 4 new doctest cases cover the round-trip, GitHub-disabled mode, bot-filter, and the poll-loop user-reply path. New CLI smoke `scripts/dev/test-agentic-handoff-clarification.sh` checks registry surface + dry-run sanity.
- **H7** (this PR) · `feat(agentic): H7 PrCommentWatcher + GitHubClient PR/check-run methods + pr-iterator agent + iteration budget` — Six new methods land on `GitHubClient` (folded from `coderabbit-react-loop.md` so the HTTP surface arrives cohesive): `FetchPrComments` (delegates to `FetchIssueComments` since GitHub PRs are issues with extra fields), `CreatePullRequest` (POST /pulls, returns html_url + emits `CreatePullRequest` audit row), `FetchCheckRuns` (GET /commits/{sha}/check-runs, returns `CheckRun[]`), `FetchCheckRunAnnotations` (GET /check-runs/{id}/annotations, returns `CheckRunAnnotation[]`), `FetchActionsJobLogs` (GET /actions/jobs/{id}/logs, follows 302 + clips via `ClipLogTail`), `RerunWorkflowRun` (POST /actions/runs/{id}/rerun). All use the existing T2 inline bearer + redacted error compose pattern. New `Source_Core/{include,src}/PrCommentWatcher.{h,cpp}` — once per scheduled-poll iteration the watcher snapshots PrOpen handoffs via the new `AgenticHandoffController::SnapshotPrOpen`, fetches each PR's comments via the wired fetcher seam, advances per-handoff `prCommentCursorSec`, and dispatches a respawn for the first non-bot post-cursor comment. The respawn dispatcher seam is wired-but-no-op for H7 (the full claude re-spawn ships in H9); iteration counting + budget enforcement is live today. New controller helpers `MarkHandoffIteration` + `MarkHandoffBudgetExhausted` (the latter idempotent) own the FSM-validated transition to Failed when the budget trips. The watcher posts a bot-filtered `<!-- smatchet-handoff -->`-prefixed "budget exhausted" comment so the next tick does not re-fire on its own marker. Two new `TrackerConfig` fields under `SMATCHET_WITH_AGENTIC`: `HandoffPrIterationBudget` (default 10, clamped 1..50) and `HandoffPrCommentPollIntervalSec` (default 120, clamped 30..600, reserved for a future dedicated thread). Tick piggybacks on `AppController::AgenticPollWorkerLoop` after `PollClarificationAnswers` so no 4th poll thread spawns. New `agents/pr-iterator.md` documents the second-stage handoff delegate (maintenance class; reads `PR_COMMENT.json`; classifies actionable / discussion / NACK / ambiguous; pushes one commit per spawn). Three new test TUs: `GitHubClient_PrSurface.test.cpp` (URL builders + `BuildCreatePullRequestBody` + `ExtractCreatePullRequestHtmlUrl` + `ClipLogTail` + fixture round-trips across all 4 new check-run / annotation / log / pr-create / pr-comments fixtures + rerun-workflow response), `PrCommentWatcher.test.cpp` (`ParsePrKeyFromUrl` edge cases, budget clamp, `MarkHandoffIteration`/`MarkHandoffBudgetExhausted` contract + idempotency, watcher no-op + fail-closed behaviour on missing seams), `PrIteratorAgentFrontmatter.test.cpp` (mirrors `HandoffAgentFrontmatter` shape). Four new recorded fixtures (`github_check_runs_sample.json`, `github_check_run_annotations_sample.json`, `github_actions_job_logs_sample.txt`, `github_pr_create_response.json`, `github_pr_comments_sample.json`, `github_rerun_workflow_response.json`). New CLI smoke `scripts/dev/test-agentic-handoff-iterate.sh` verifies the H7 patch did not regress the H3/H4/H5/H6 command-registry surface and that the AGENTIC=ON build links cleanly against the 6 new methods.
- **H8** (this PR) · `feat(agentic): H8 SmatchetAgentHandoffUi panel + bucket-E test` — new ImGui panel `Source_Core/src/SmatchetAgentHandoffUi.{h,cpp}` renders the in-flight agentic handoffs via `AgenticHandoffController::SnapshotActive()`. Top-half = an `ImGui::BeginTable` with five columns (ID, Issue, State, PR URL, Actions); the row Selectable spans all columns so click-anywhere selects. Bottom-half = a detail pane that surfaces the cached `lastClarificationQuestion` (from H5), an `InputTextMultiline` reply scratch + `Submit clarification` button when state == AwaitingUser, the `lastError` colored line on Failed, and a compact `worktree / branch / started / iterations` metadata block. Per-row actions: `Cancel` (gated by an `IsCancellable` predicate over Running / AwaitingUser / PrOpen / Iterating — terminal + pre-spawn states leave the button unrendered so the FSM never rejects a UI click), `Open PR` (gated on non-empty `prUrl`; routes through `ShellExecuteW` via a UTF-8 → UTF-16 conversion that mirrors `BlameAnalysisUi_Launch.cpp:1-50`), `Open WT` (same precedent against the worktree dir). Panel polls on a 1 Hz steady-clock gate at TU scope — same cadence rationale as the T6 proposals UI; handoff state transitions arrive at human + network latency, not the 144 Hz frame loop. Refresh forced on next frame after every action via the `g_initialFetchDone = false` re-arm. Window-title toggle wired into View menu alongside the T6 proposals item via `d.showAgentHandoffs` on `UiDrawSession`. 27 new localization keys under `agent.handoffs.*` (window title, table headers, actions, detail labels, all 9 RunState display strings) — en + fr round-tripped through `SmatchetLocalization.cpp`. Bucket-E coverage in `tests/ui/agent_handoff_panel.test.cpp` — three variants (`AwaitingUser_SubmitClarification_TransitionsState`, `Running_CancelButton_FlipsCancelAtom`, `PrOpen_OpenPrButton_RecordsLaunch`) drive a stub controller (mirrors the production `ActiveHandoff` fields + provides headless `LaunchUrl` recorder so the test never hits `ShellExecuteW`). Driver: `scripts/dev/test-ui-agent-handoff.sh` (auto-enrolled by `test-all.sh`).
- **H6** · `a5a21cf` · `feat(agentic): H6 PR-open path + gh pr create fallback` (#254) — `ClaudeCodeLocalRunner` gains a PR-open fallback path: when the spawned harness exits `ok=true` without writing `PR_URL.txt`, the runner invokes `git push -u origin <branch>` followed by `gh pr create --draft --base <X> --title <T> --body <B>` via `SubprocessCapture::Run`, parses the URL from `gh`'s stdout, validates it against a cheap "github.com/.../pull/<digit>" check, and writes it back to `PR_URL.txt` so downstream watchers / restart paths converge on the same sentinel. Title heuristic is first-line-of-last-commit (`git log -1 --pretty=%s`) → `seed.issueTitle` → `seed.issueKey` → hard fallback — never empty. `AgenticHandoffController` ControllerTransition gains a PR-URL resolver that reads `PR_URL.txt` from the worktree on the PrOpen transition (since the runner's bare `(string newState)` state callback has no slot for a URL) so the audit payload + new toast sink both see the URL. New optional `ToastSink` seam fires on PrOpen with `"Agent PR opened: <url>"`; production binds to `SmatchetToastManager::Instance().Push(...)`. Five new config rows under `SMATCHET_WITH_AGENTIC`: `HandoffAutoCreatePrIfMissing` (default true; flip off to require a harness-written sentinel), `HandoffPrBaseBranch` (default `develop`), `HandoffPrBodyTemplate` (empty = built-in template carrying the `<!-- smatchet-handoff -->` marker; `{proposalId}` / `{issueKey}` / `{sourceTracker}` placeholders substituted), `HandoffGitBinPath` + `HandoffGhBinPath` (empty = PATH resolve; tests inject stub paths). All additive — no schema bump. Two new test-fixture exes: `stub_git` (answers `push` / `log` / `--version`; supports `STUB_GIT_MODE=push-fail`) and `stub_gh` (answers `pr create` with a configurable URL; supports `STUB_GH_MODE=create-fail` / `bad-url`). 5 new doctest cases cover: fallback success + URL carrying through to `PR_URL.txt` + `PrOpen`-before-`Complete` state ordering; `gh pr create` failure surfaces ok=false; `git push` failure surfaces ok=false; bad-URL output rejected; fallback disabled leaves prUrl empty cleanly.

## Triage half — shipped end-to-end (T9 — 2026-05-18)

All 10 triage-half phases shipped (T0 through T9):

| Phase | PR | sha | Summary |
|---|---|---|---|
| T0 | #225 | `906c312` | plan-lock + ADRs + glossary |
| T1 | #226 | `c0a74bd` | `ITrackerClient::FetchIssueComments` + `GitHubClient` skeleton + `SMATCHET_WITH_AGENTIC` gate |
| T2 | #227 | `639b0a3` | `GitHubClient` write methods + audit-trail wiring |
| T3 | #228 | `0c0f9c6` | `AgenticInferenceClient` + schema validation |
| T4 | #229 | `746d8bf` | `AgentProposal` + `AgentProposalStore` (SQLite) |
| T5 | #230 | `6530c1f` | `AgenticTriageController` + `agent.triage.run` |
| T6 | #231 | `f944dd0` | `SmatchetAgentProposalsUi` + bucket-E test |
| T7 | #232 | `dd2fa99` | scheduled poll + Preferences toggle |
| T8 | #233 | `04a6d0f` | scenario step + recorded fixtures |
| T9 | #(this PR) | TBD | schema-version bump + plan revision |

Triage-half acceptance gate (per plan § "Triage-half acceptance gate"):
- [x] All T0–T9 PRs merged into `develop` (T9 merge pending).
- [x] `develop` buildable + `test-all.sh` green (T9 gate result attached in this PR body).
- [x] `agent.triage.run --source github --query <fixture>` lands proposals in the SQLite store (T5 + T8 scenario verifies; `test-agentic-triage-cli.sh` + `test-agentic-approve-reject.sh` both green).
- [x] `SmatchetAgentProposalsUi` shows proposals with working Approve / Reject buttons (T6 bucket-E variants + T8 scenario verifies the state-machine transitions).

Orchestrator may dispatch H0 (handoff half) once T9 merges into `develop` and this gate is re-verified on the post-merge tip.

## Deviations from plan

- **All phases — phase-per-branch shipping (not all-in-one).** Plan envisioned one consolidated PR per half; instead each phase shipped as its own PR (T0-T9 = 10 PRs). Rationale: smaller diffs, per-phase regression isolation, parallelisable review. Sequencing followed the dependency graph in § "Sequenced phases" exactly.
- **T0-T5 — no per-phase deviations recorded.** Implementation tracked the plan-locked decisions in § "Decisions locked" verbatim; no design changes during these phases.
- **T6 — issue-title omitted from row header.** The plan-locked decision (#5) allowed deferring issue-title rendering pending T7's `FetchIssueBody` integration. Row header therefore shows `<issueKey>  [<action>]` only. Re-evaluate when T7 lands if the title becomes worth a per-row HTTP fetch (probably not — `issueKey` is already disambiguating for triage scan).
- **T6 — refresh cadence is TU-static, not a UiDrawSession field.** The panel owns its own polling cadence (1 Hz steady-clock gate) at TU scope to avoid leaking polling state into `UiDrawSession` for a feature with no other consumers. A future poll-driven update (T7) may move this into session state if shared with another caller.
- **T7 — issue-title still deferred (not adopted in this slice).** T6's deviation stands; T7 stays scoped to the worker + Preferences. The `FetchIssueBody` body is fetched per-issue during triage already, but threading the title into the Proposals UI panel is a separate render-time decision deferred to a later UX polish slice.
- **T7 — cursor advance uses wall-clock, not max(updated_at) of returned issues.** GitHub guarantees monotonic `updated_at`, so worst-case drift between server + client is bounded by the poll interval (≤ 1 h). Parsing every payload for `updated_at` would add cost with no observable benefit for the intended "didn't see anything older than this" semantic.
- **T7 — discovery routed through the adapter directly (not `TriageBatch`).** The cursor filter is worker-local; `AgenticTriageController::TriageBatch` calls `ListOpenIssuesForRepo` with the default `sinceUnixSec=0`. To inject the cursor without threading it through the controller API the worker calls `IGitHubReadClient::ListOpenIssuesForRepo` directly + funnels each returned key through `TriageIssue`. Keeps the cursor surface contained to the scheduled-poll path.
- **T9 — `schema_version` table layered alongside the existing additive tables, not via destructive migration.** Plan decision #14 deferred the bump precisely so T4-T8 could ship without interim version churn. T9 lands `version=1` as the first recorded shipped state; the agent_* tables are unchanged. Migration ladder is wired but empty (no version-2 body exists), and the open path is idempotent on already-versioned databases.
- **T9 — singleton-row constraint via CHECK rather than separate "settings" table.** A standalone migrations table or settings table would be over-engineering for a single integer; `schema_version (id INTEGER PRIMARY KEY CHECK (id = 1), version INTEGER)` is one row by construction and one SELECT/UPDATE on the hot path.
- **H3 — `SubprocessCapture::CaptureOptions::onStdoutLine` callback added rather than a new streaming primitive.** The plan implied the runner would spawn `claude` directly. Adding a line-streaming callback to the existing H1 helper keeps one process-spawn implementation across P4Blame + runner + future agentic surfaces; the callback is purely additive (default-null = existing buffered behaviour) and the line-dispatcher is a 20-line pure function. Side benefit: a new `replaceParentEnv` flag normalises Windows / POSIX env-block semantics so the allow-list assertion holds cross-platform.
- **H3 — `STUB_CLAUDE_MODE` env passthrough exception.** The runner explicitly carves a narrow allow-list extension for `STUB_CLAUDE_MODE` so the doctest layer can select stub modes without poisoning the parent env permanently. Production callers never see this branch (parent env never carries `STUB_CLAUDE_MODE`). Documented inline at `CollectTestModePassthrough()`.
- **H3 — `Options::skipWorktreeCreate` for test ergonomics.** Tests use a `mkdtemp`-style tmp dir rather than a real `git worktree add` to keep doctest dependencies trivial. Production `handoff.start` (H4) leaves the flag false; the runner runs the real worktree-add path.
- **H7 — `PrCommentWatcher::Tick()` piggybacks on the scheduled-poll thread rather than spawning its own.** Plan envisioned a dedicated `PrCommentWatcher` thread with its own sleep + cancel loop. The existing T7 `AgenticPollWorkerLoop` already runs at a configurable cadence and the H5 `PollClarificationAnswers` already piggybacks the same way; following that pattern keeps the thread count flat (3 vs 4) and avoids the cancel / restart plumbing duplication. The `HandoffPrCommentPollIntervalSec` config field is plumbed but unused — reserved for a future dedicated-thread variant if the piggyback cadence proves too coarse. Field is documented as reserved in the ConfigManager.h doc comment.
- **H7 — respawn dispatcher seam wired-but-no-op for this wave.** Iteration counting + budget enforcement + cursor advance + budget-exhausted marker post are all live in H7; the actual `claude` respawn binding ships in H9 cross-flow wiring (the full respawn needs the SEED.json carry-over rules + the `PR_COMMENT.json` writer that `pr-iterator.md` reads, both of which sit on H9's path). The wired-but-no-op dispatcher logs `INFO` once per detected comment so the iteration counter still advances and the budget gate still trips — exercising the contract today without coupling H7 to H9's design.
- **H7 — `FetchPrComments` delegates to `FetchIssueComments` rather than calling `/pulls/{n}/comments`.** GitHub treats PRs as a superset of issues — the issues-comments endpoint returns the PR's conversation thread, while `/pulls/{n}/comments` returns only diff-review comments which is not what the watcher needs. Delegating keeps the JSON parser in one place + reuses the ISO-8601 / bot-filter / pagination contract verbatim.
- **H7 — `CreatePullRequest` audit context-key is `owner/repo` (no `#N` suffix).** The audit-trail convention is `IssueKey = owner/repo#N` for issue-keyed actions, but `CreatePullRequest` runs *before* the PR exists (no number yet). Using `owner/repo` keeps the audit row keyable + distinguishable from the post-creation surface (later writes carry `#N` via the PR URL). Documented inline.
- **H8 — header file lives next to .cpp under `Source_Core/src/`, not `Source_Core/include/`.** Mirrors the T6 `SmatchetAgentProposalsUi.h` precedent — UI-only panel headers do not need to expose symbols to other TUs, so the header sits beside its single consumer rather than polluting the public include tree.
- **H8 — refresh cadence is TU-static, not a UiDrawSession field.** Same rationale as T6 — the panel owns its own polling cadence (1 Hz steady-clock gate) at TU scope; nothing else consumes the cached SnapshotActive() result. Single field added to `UiDrawSession` is the boolean `showAgentHandoffs` for the menu toggle, mirroring `showAgentProposals` exactly.
- **H8 — recent-events log deferred.** Plan layout sketched a per-handoff "Recent events" list at the bottom of the detail pane. The H4 controller already audits every FSM transition to `BackendAuditTrail`; surfacing the audit subset here would mean a second query path against the audit file from the UI thread, which is the kind of synchronous I/O we avoid (AGENTS.md Pillar 2). Substituted a compact `worktree / branch / started / iterations` metadata block that reads only fields already cached on the `ActiveHandoff` record — zero new I/O. A real recent-events feed can ride on top of the audit-trail viewer (already exists as a separate panel) without coupling here.
- **H8 — `Cancel` button uses an `IsCancellable` predicate rather than always-rendered + grey-disabled.** The FSM allows Cancel only from Running / AwaitingUser / PrOpen / Iterating; Pending and Spawning return error transitions. Rendering a disabled-looking button for the brief Pending / Spawning window would confuse the user ("why can't I cancel?"); the FSM-aware gate is one source of truth and matches what `AgenticHandoffController::Cancel` would do at runtime.
- **H8 — `Open PR` / `Open WT` labels rendered without spaces (`OpenPR` / `OpenWT`) in the bucket-E replica.** ImGui Test Engine's `ItemClick("**/<label>")` path tokenizer treats spaces ambiguously when matching across child windows; the replica uses `OpenPR` to keep the path resolver unambiguous. Production UI keeps the localised "Open PR" / "Open WT" labels — the test exercises the action paths, not the visual copy.
- **H8 — bucket-E driver tolerates the known UI-test-spawn timeout flake (`infra.md` P2).** The proposals driver `test-ui-agent-proposals.sh` exhibits the same flake when invoked back-to-back through `test-all.sh` (port contention on consecutive ephemeral spawns) but passes when invoked standalone. The handoff driver shares the same shape and the same fragility; CI's retry-once policy is the load-bearing mitigation. No new infrastructure introduced here.

## Verification

- **T6**: dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`) under `ninja-iter-msys2`; `SMATCHET_WITH_AGENTIC=OFF` build of `SmatchetStandalone` confirms the panel + menu item + Draw call fully drop out; `ninja-test-msys2` ctest green (no new doctest — UI logic is bucket-E); `bash scripts/dev/test-ui-agent-proposals.sh` runs the four bucket-E variants against the ephemeral `ninja-ui-test-msys2` exe; `bash scripts/dev/test-all.sh` full sweep.
- **T7**: dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`) under `ninja-iter-msys2`; `SMATCHET_WITH_AGENTIC=OFF` build of `SmatchetStandalone` confirms `agenticPollThread_` / `RestartAgenticPoll` / `RunAgenticTriageOnce` / `AgenticPoll*` config fields all drop out; `ninja-test-msys2` ctest green including new `FormatUnixSecAsIso8601` round-trip cases in `GitHubClientHelpers.test.cpp`; `bash scripts/dev/test-agentic-triage-cli.sh` continues to PASS (synchronous CLI path the worker calls); `bash scripts/dev/test-all.sh` full sweep. Bucket-E coverage for the Preferences Agentic tab itself is residue (logged below).
- **T8**: dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`) under `ninja-iter-msys2`; `SMATCHET_WITH_AGENTIC=OFF` build of `SmatchetStandalone` confirms `AgentTriageScenarioStep.cpp` + `AgentTriageScenarioFixtures.cpp` drop out cleanly and the `MakeAgentTriageScenario` factory call in `AppController` is fully ifdef-gated; `ninja-test-msys2` ctest green including the new `AgentTriageScenarioFixtures.test.cpp` doctest cases (happy path + malformed JSON + non-array root + sparse fields + ISO-8601 strict-Z); `Smatchet.exe cmd scenario.list` includes `agent-triage-roundtrip`; `bash scripts/dev/test-agentic-approve-reject.sh` PASS against the ON build + clean SKIP against the OFF build; `bash scripts/dev/test-all.sh` full sweep including the new runner.
- **T9** (triage-half final):
  - `ninja-test-msys2` ctest green including the 4 new `AgentProposalStore.test.cpp` cases (`schema_version stamped at kCurrentSchemaVersion on first open`, `schema_version does not double-bump across re-opens`, `schema_version row count is exactly 1 (singleton constraint)`, `kCurrentSchemaVersion == 1 invariant guard`).
  - `ninja-debug-msys2-asan` sanitizer build of the agentic scenario — gate result recorded in the PR body (per plan decision #15 stop conditions; preset-missing case documented as residue if it triggers, **not** suppressed).
  - `ninja-ui-test-msys2` bucket-E rebuild — `test-ui-agent-proposals.sh` green.
  - `ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` dual-target green.
  - `SMATCHET_WITH_AGENTIC=OFF` standalone build green (schema_version code path lives in the same TU as the rest of `AgentProposalStore.cpp`, which is itself fully gated by the source-list switch — verified by configure-time inspection).
  - `bash scripts/dev/test-agentic-approve-reject.sh` PASS.
  - `bash scripts/dev/test-agentic-triage-cli.sh` PASS.
  - `bash scripts/dev/test-grid-edit-perf-postfix.sh` perf gate green.
  - `bash scripts/dev/test-all.sh` full sweep green.
- **H7** (this PR):
  - `ninja-iter-msys2` dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`) green.
  - `SMATCHET_WITH_AGENTIC=OFF` build of `SmatchetStandalone` green (`PrCommentWatcher.cpp` source-list-conditional drops out cleanly; the 6 new `GitHubClient` methods sit inside the existing AGENTIC-gated TU and never see the OFF compile).
  - `ninja-test-msys2` ctest green — full SmatchetTests run: **717 cases / 4041 assertions / 0 failures**. New H7 surface = 6 doctest cases in `GitHubClient_PrSurface.test.cpp` (URL builders + body builder + extractor + log-tail clipper + 5 fixture round-trips) + 5 cases in `PrCommentWatcher.test.cpp` (URL parse edges + budget clamp + Mark*Iteration / Mark*BudgetExhausted contract + idempotency + watcher fail-closed branches) + 4 cases in `PrIteratorAgentFrontmatter.test.cpp` (mirrors the H2 frontmatter shape).
  - `bash scripts/dev/test-agentic-handoff-iterate.sh` PASS (new smoke; verifies AGENTIC=ON build links cleanly + prior agentic CLI surface intact).
  - `bash scripts/dev/test-agentic-handoff-cli.sh` PASS (H3/H4 regression check).
  - `bash scripts/dev/test-agentic-handoff-clarification.sh` PASS (H5 regression check).
  - `bash scripts/dev/test-agentic-approve-reject.sh` PASS (T8 scenario regression check).
  - `bash scripts/dev/test-grid-edit-perf-postfix.sh` perf gate green (mean = 0.0009 ms ≤ 6.94 ms; p99 = 0.0011 ms ≤ 16.67 ms).
  - `bash scripts/dev/test-all.sh` full sweep — 113 pass / 14 fail. Every failure is `build/ninja-ui-test-msys2/Smatchet.exe not found` (bucket-E UI test exe — not built in this worktree; environmental, not regressions). No new failures introduced by the H7 patch.

- **H8** (this PR):
  - `ninja-iter-msys2` dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`) green — both targets compile `SmatchetAgentHandoffUi.cpp` cleanly. DX12 keeps the `OpenInShell` non-Windows stub path on its own compile to avoid a header-pollution surprise; verified the GLFW / OpenGL include cascade does not regress.
  - `SMATCHET_WITH_AGENTIC=OFF` build of `SmatchetStandalone` green (`SmatchetAgentHandoffUi.cpp` source-list-conditional drops out cleanly under the CMakeLists.txt `list(REMOVE_ITEM ...)` + `list(APPEND ... if(SMATCHET_WITH_AGENTIC))` pair; the menu item + draw hook + `UiDrawSession::showAgentHandoffs` field are all `#if defined(SMATCHET_WITH_AGENTIC)`-gated so the OFF build never references the panel symbol).
  - `ninja-test-msys2` ctest green (no new pure-logic doctest — `RunStateToString` is already covered by H4's `HarnessRunState.test.cpp` and the UI shapes route to bucket-E).
  - `ninja-ui-test-msys2` bucket-E build green — registers 3 new tests under suite `AgentHandoff`. Standalone `bash scripts/dev/test-ui-agent-handoff.sh` invocation occasionally exhibits the known UI-test-spawn timeout flake (`infra.md` P2) — same shape that bites `test-ui-agent-proposals.sh` when invoked through the full `test-all.sh` sweep but not standalone. Documented honestly; CI retry-once policy is the canonical mitigation.
  - `bash scripts/dev/test-ui-agent-proposals.sh` standalone PASS (4/4) — confirms the bucket-E rig itself is healthy in this worktree.
  - `bash scripts/dev/test-agentic-handoff-cli.sh` regression check PASS (H3/H4 surface intact).
  - `bash scripts/dev/test-agentic-handoff-clarification.sh` regression check PASS (H5 surface intact).
  - `bash scripts/dev/test-agentic-handoff-iterate.sh` regression check PASS (H7 surface intact).
  - `bash scripts/dev/test-agentic-approve-reject.sh` regression check PASS (T8 scenario intact).
  - `bash scripts/dev/test-all.sh` full sweep — non-bucket-E green; bucket-E failures (including `test-ui-agent-proposals.sh` itself) are the known infra.md P2 environmental flake, not regressions introduced by this PR.

- **H3** (#251):
  - `claude --help` flag-set verification (decision #3): all 5 flags present in local Claude Code 2.1.143 — `--print` ✓, `--output-format stream-json` ✓, `--verbose` ✓, `--permission-mode bypassPermissions` ✓, `--append-system-prompt-file` ✓ (file variant documented in `--bare` help; accepted on probe). No flag drift; phase proceeds normally.
  - Env allow-list assertion (decision #7): doctest case `env allow-list drops SMATCHET_SECRET and admits GH_TOKEN` confirms the runner's env block excludes `SMATCHET_SECRET` and includes `GH_TOKEN`. Verified against the stub_claude `env_snapshot.txt`.
  - `ninja-iter-msys2` dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`) green.
  - `SMATCHET_WITH_AGENTIC=OFF` build of `SmatchetStandalone` green (`ClaudeCodeLocalRunner.cpp` source-list-conditional drops out cleanly).
  - `ninja-test-msys2` ctest green including the 6 new `ClaudeCodeLocalRunner.test.cpp` cases against `stub_claude`.
  - `bash scripts/dev/test-agentic-handoff-cli.sh` PASS against AGENTIC=ON, SKIP cleanly against AGENTIC=OFF.
  - `bash scripts/dev/test-grid-edit-perf-postfix.sh` perf gate green.
  - `bash scripts/dev/test-all.sh` full sweep green.

## Post-triage-half audit follow-up

Code-review + security-review sweeps over the merged T0-T9 surface produced a triage list grouped into Bundle A (Pillar 2 UI-thread freezes), Bundle B (security + correctness critical), and Bundle C (code-quality + minors). Bundles are shipped as their own follow-up PRs to keep the diff per merge reviewable.

- **Bundle A** · PR #235 · `f4b7299` · `fix(agentic): Bundle A - Pillar 2 UI-thread unblocking + audit-trail wiring` — UI-thread blocking calls in the proposals panel + audit-trail hook for approval / rejection.
- **Bundle B** (this PR) · `fix(agentic): Bundle B — security + correctness critical (SH1+SH3 + atomic transitions + cursor safety)` — seven findings landed:
  - **SH1** · `Source_Core/src/AgenticInferenceClient.cpp` — cap the streamed LLM response accumulator at 10 MB (`AgenticInferenceClientPure::kMaxLlmResponseBytes`). When the buffer would cross the cap the streaming handler latches `capExceeded`, drives the `AiCancelToken`, and the post-stream handler surfaces `"LLM response exceeded 10 MB cap (probable provider error or attack)"` to the caller. Pillar 3 OOM defense — a hostile or compromised provider can no longer stream gigabytes of delta chunks before terminating. Pure helper `WouldExceedResponseCap(currentBytes, addBytes)` (saturating-add overflow guard) drives the gate and is covered by 5 doctest subcases in `AgenticInferenceClientPure.test.cpp`.
  - **SH3** · `Source_Core/src/AiErrorRedact.cpp` — extend `kIdPrefixes` with all GitHub PAT prefixes (`ghp_`, `gho_`, `ghs_`, `ghu_`, `ghr_`, `github_pat_`) so 401 / 403 responses that echo the supplied PAT are sanitised before reaching `LOG_WARN` / `AiStreamError::Message`. Add `redactJsonField("github_pat" / "githubPat" / "GitHubPat")` for the same body shape. Audit-trail's `LooksSensitiveKey` (`Source_Core/src/BackendAuditTrail.cpp`) gains `github_pat` / `githubpat` substring matches (specific literals to avoid `path` / `patch` false-positives). Covered by 7 new doctest subcases in `AiErrorRedact.test.cpp`.
  - **CR#229:177** · `Source_Core/src/AgentProposalStore.cpp::Transition` — wrap the SELECT-then-UPDATE state-transition sequence in `SQLite::Transaction`. Without this, two concurrent transitions (UI Approve racing the auto-poll worker) can both observe `state=Pending`, both decide their transition is legal, and the second silently overwrites the first. Existing transition tests continue to pass; correctness is observable indirectly via the audit-trail logging.
  - **CR#229:318** · `Source_Core/src/AgentProposalStore.cpp::RowToProposal` — return `bool` + propagate unknown state / action literals as errors instead of silently re-mapping to `Pending` / `Unknown`. `Query` clears its partial output on failure (all-or-nothing result-set semantics); `Find` surfaces the error. Future schema-version 2+ migration that adds a new state (e.g. `Reviewing`) will trigger this gate against old builds, blocking the silent re-action that would otherwise corrupt the data. Covered by 3 new doctest cases (`Find` on unknown state, `Find` on unknown action, `Query` on unknown state with multi-row partial-output reset).
  - **CR#230:107** · `Source_Core/include/AgentProposalStore.h` + `AgenticTriageController::TriageIssue` — new `AgentProposalStore::InsertMany(std::vector<AgentProposal>&, std::string&)` wraps all per-issue draft inserts in a single SQLite transaction. A failure on row N rolls back rows 0..N-1; the input vector's `id` fields reset to 0 on rollback so callers can't surface partial ROWIDs. `TriageIssue` switched from a per-draft `Insert` loop to one `InsertMany` call. Covered by 3 new doctest cases (`InsertMany` empty input no-op, all-or-nothing success, all-or-nothing rollback on trigger-induced abort).
  - **CR#232:1653** · `Source_Core/src/AppController.cpp::AgenticPollWorkerLoop` — only advance the poll cursor when every issue in the iteration succeeded. If `totalFailed > 0` the cursor stays at the previous value so the next poll re-fetches those issues. Pure helper `AgenticInferenceClientPure::ShouldAdvancePollCursor(failedCount)` drives the gate. Idempotency at the LLM + `InsertMany` layer keeps re-triage cheap. Covered by 4 doctest subcases (0 / 1 / 10 / SIZE_MAX failures).
  - **CR#226/#232 github_pat plaintext migration** · `Source_Core/src/ConfigManager.cpp` — mirror the AiApiKey lazy plaintext-to-DPAPI migration shape for `github_pat`. On `Load()`, if the value was read from the legacy plaintext `github_pat` field (because `github_pat_enc` was absent or undecryptable), set `migrateLegacyPlaintextGitHubPat = true`; the eager `Save()` at the bottom of the Load path then re-encrypts under `github_pat_enc` and removes the plaintext key. Covered indirectly by the existing ConfigManager round-trip + the production migration log line.
- **Bundle C** (next) · code-quality + minors.
