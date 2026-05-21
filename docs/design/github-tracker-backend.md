# Plan — Rip out agentic surface + add GitHub as third tracker

> **Slug**: `github-tracker-backend` (kept for PR continuity — the ripout + tracker land together).
>
> **Mandatory rules cross-link**: see [`AGENTS.md`](../../AGENTS.md) § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.
>
> **Scope evolution**: 2026-05-20 initial draft (GitHub tracker only). 2026-05-21 a.m. expanded to triage tracker-agnostic refactor; locked 8 decisions via grill-with-docs + 3 architect passes. 2026-05-21 mid-day scope reduced (refactor deferred). **2026-05-21 p.m. major scope expansion**: full agentic surface ripped out + new GitHub tracker built standalone (zero coupling to deleted code).

## Context

Smatchet today ships an extensive **agentic flow** layered on top of `GitHubClient` — automatic triage of GitHub issues into LLM-emitted proposals, automatic spawning of `claude` / `codex` subprocesses to implement approved proposals, scheduled-poll workers, CodeRabbit react loop, CI failure react loop, PR comment watcher, check-run watcher, merge-gates poller, sentinel-file handoff envelope, audit-trail wiring. The surface spans **25 source TUs + 22 headers + 4 builtin command files + 24+ tests + 6 design docs + 3 ADRs + 7 scripts + several `.github/workflows`** plus large sections of AGENTS.md (Merge gates, Handoff envelope, Ship-loop sequencing, Debug-mode pause-loop, Spawned-orchestrator first-move contract, Spawned-child PR draft requirement, FSM integrity, etc.).

User has decided to **delete the entire agentic surface** and add GitHub as a third grid-backing tracker with **zero coupling** to anything in the deleted surface. The tracker stands on its own — same shape as the existing `JiraClient` / `PlaneClient` (factory-owned `unique_ptr`, `ITrackerClient` virtuals only, no PR/check-run/CodeRabbit/handoff surface).

This is a **destructive ripout** of multiple weeks of shipped agentic work. Code is reachable in git history (`git log --before=2026-05-21 -- Source_Core/src/Agentic*` etc.) for future re-introduction; no archive branch needed beyond standard git reflog + tags.

Cross-link: prior over-scoped plan revisions captured in this file's git history at `1c8135fd / 76c57d6c / e9eb0478 / 491f8425 / 7ae7e584 / 133674b3 / 3b37c7ee / 58c1980b` for context on the deferred (now deleted) triage refactor.

## Decisions locked

1. **Full agentic ripout** — delete the entire `SMATCHET_WITH_AGENTIC`-gated surface, not just gate-strip it. The flag itself is retired with no consumers.
2. **Clean GitHub tracker, factory-owned** — `GitHubClient.cpp` + `GitHubClient.h` are **rewritten from scratch** as a tracker-only `ITrackerClient` implementation. Factory creates + owns a `unique_ptr<GitHubClient>` per `Create("github")` call — same ownership shape as `JiraClient` / `PlaneClient`. No shared instance with anything (because there is nothing left to share with).
3. **`UpdateField` semantics = set-replace at the virtual** — `values` is the intended full set after edit. `GitHubClient::UpdateField` for labels / assignees pre-fetches the current set and diffs internally via `ComputeLabelEditDiff` pure helper.
4. **No `SMATCHET_WITH_AGENTIC` flag, no build-time gating** — `GitHubClient.cpp` + `GitHubClientHelpers.cpp` always compile (in both `SmatchetStandalone` + `SmatchetCore_DX12`). GitHubPat config field always present.
5. **Split into TWO PRs** — PR1 ships the ripout (deletions + AGENTS.md edits + ADR withdrawals); PR2 ships the new tracker. Each PR reviewable independently; PR1 net negative LOC, PR2 net positive.
6. **Static field catalog, no API** — 6 native fields (state, labels, assignees, milestone, title, body). Projects v2 deferred.
7. **Verification = Bucket A pure-logic only** — no bucket-B/E in either PR. Backlog entry for tracker-switch UI coverage.

## Approach

Two PRs, sequenced.

### PR1 — Agentic ripout (deletions only)

Delete everything in the `SMATCHET_WITH_AGENTIC` source-list block + every consumer / test / doc / script / workflow that references the deleted symbols. Net: ~5000–10000 LOC removed, ~80 files deleted, ~20 files modified to drop now-dead references.

**Files deleted (source — 25 TUs)**:
- `Source_Core/src/AgentProposalStore.cpp`
- `Source_Core/src/AgentTriageScenarioFixtures.cpp`
- `Source_Core/src/AgenticContextDoc.cpp`
- `Source_Core/src/AgenticHandoffController.cpp`
- `Source_Core/src/AgenticInferenceClient.cpp` + `AgenticInferenceClientPure.cpp`
- `Source_Core/src/AgenticProposalAuditPure.cpp`
- `Source_Core/src/AgenticTriageController.cpp`
- `Source_Core/src/CiFailureClassifier.cpp` + `CiFailureClassifierPure.cpp`
- `Source_Core/src/ClaudeCodeLocalRunner.cpp`
- `Source_Core/src/CoderabbitCommentClassifier.cpp` + `CoderabbitCommentClassifierPure.cpp`
- `Source_Core/src/CodingHarnessSeedBuilder.cpp`
- `Source_Core/src/Commands/Scenarios/AgentHandoffScenarioStep.cpp` + `AgentTriageScenarioStep.cpp`
- `Source_Core/src/HarnessRunState.cpp`
- `Source_Core/src/OpenPrRegistrar.cpp`
- `Source_Core/src/PrCheckRunWatcher.cpp` + `PrCommentWatcher.cpp`
- `Source_Core/src/SmatchetAgentHandoffUi.cpp`
- `Source_Core/src/SmatchetAgentProposalsUi.cpp` + `SmatchetAgentProposalsUiPure.cpp`
- `Source_Core/src/GitHubClient.cpp` + `Source_Core/src/GitHubClientHelpers.cpp` — **deleted entirely** (re-introduced fresh in PR2; the existing file is intimately woven with triage primitives + PR/check-runs/GraphQL).
- `Source_Core/src/Commands/Builtin/BuiltinCommands_Agentic.cpp` + `_CiReact.cpp` + `_Coderabbit.cpp` + `_Handoff.cpp`

**Files deleted (headers — 22)**:
- `Source_Core/include/AgentProposal.h`
- `Source_Core/include/AgentProposalStore.h`
- `Source_Core/include/AgentTriageScenarioFixtures.h`
- `Source_Core/include/AgenticContextDoc.h`
- `Source_Core/include/AgenticHandoffController.h`
- `Source_Core/include/AgenticInferenceClient.h` + `AgenticInferenceClientPure.h`
- `Source_Core/include/AgenticProposalAuditPure.h`
- `Source_Core/include/AgenticTriageController.h`
- `Source_Core/include/AgentsMdLoader.h` (used by AgenticContextDoc only — verify no non-agentic consumer)
- `Source_Core/include/CiFailureClassifier.h` + `CiFailureClassifierPure.h`
- `Source_Core/include/ClaudeCodeLocalRunner.h`
- `Source_Core/include/CoderabbitCommentClassifier.h` + `CoderabbitCommentClassifierPure.h`
- `Source_Core/include/CodingHarnessSeedBuilder.h` + `CodingHarnessTypes.h`
- `Source_Core/include/HarnessRunState.h`
- `Source_Core/include/OpenPrRegistrar.h`
- `Source_Core/include/PrCheckRunClassifier.h` + `PrCheckRunWatcher.h`
- `Source_Core/include/PrCommentClassifier.h`
- `Source_Core/include/GitHubClient.h` + `GitHubClientHelpers.h` — deleted; re-introduced fresh in PR2.
- `Source_Core/include/SmatchetAgentProposalsUiPure.h` + the inline `SmatchetAgentHandoffUi.h` / `SmatchetAgentProposalsUi.h` headers in `src/`.

**Files deleted (tests — 24+)**:
- `tests/Source_Core/AgentProposalStore.test.cpp`
- `tests/Source_Core/AgentTriageScenarioFixtures.test.cpp`
- `tests/Source_Core/AgenticHandoffController.test.cpp`
- `tests/Source_Core/AgenticInferenceClientPure.test.cpp`
- `tests/Source_Core/AgenticProposalAuditPure.test.cpp`
- `tests/Source_Core/AgenticTriageController.test.cpp`
- `tests/Source_Core/AgentsMdLoader.test.cpp`
- `tests/Source_Core/CiFailureClassifier.test.cpp` + `Pure.test.cpp`
- `tests/Source_Core/ClaudeCodeLocalRunner.test.cpp`
- `tests/Source_Core/CoderabbitCiReactConfig.test.cpp`
- `tests/Source_Core/CoderabbitCommentClassifier.test.cpp` + `Pure.test.cpp`
- `tests/Source_Core/CoderabbitTriageAgentFrontmatter.test.cpp`
- `tests/Source_Core/CodingHarnessSeedBuilder.test.cpp`
- `tests/Source_Core/GitHubClient_GraphQL.test.cpp` + `_PrSurface.test.cpp` + the existing `GitHubClientHelpers.test.cpp` (rewritten in PR2 for the new shape).
- `tests/Source_Core/HandoffAgentFrontmatter.test.cpp`
- `tests/Source_Core/HarnessRunState.test.cpp`
- `tests/Source_Core/OpenPrRegistrar.test.cpp`
- `tests/Source_Core/PrCheckRunWatcher.test.cpp` + `_CiDispatch.test.cpp`
- `tests/Source_Core/PrCommentWatcher.test.cpp` + `_OpenPrScan.test.cpp` + `_ShortCircuitReject.test.cpp`
- `tests/Source_Core/PrIteratorAgentFrontmatter.test.cpp`
- `tests/ui/agent_handoff_panel.test.cpp` + `agent_proposals_handoff_button.test.cpp` + `agent_proposals_panel.test.cpp`

**Files deleted (design docs)**:
- `docs/design/agentic-coding-handoff.md`
- `docs/design/agentic-flow-implementation.md`
- `docs/design/agentic-triage-flow.md`
- `docs/design/coderabbit-react-loop.md`
- `docs/design/agent-contract-alignment.md` (verify: agentic-specific?)

**Files deleted (operator docs)**:
- `docs/agentic/TRIAGE_MANUAL.md`
- `docs/agentic/USAGE.md`
- `docs/agentic/` directory itself.

**Files deleted (scripts — 7)**:
- `scripts/dev/test-agentic-handoff-clarification.sh` + `-cli.sh` + `-iterate.sh` + `-scenario.sh`
- `scripts/dev/test-coderabbit-react.sh`
- `scripts/dev/test-ui-agent-handoff.sh` + `test-ui-agent-proposals-handoff-button.sh`

**Files deleted (agents)**:
- `agents/handoff-implementer.md` (no more spawned-child harness)
- `agents/pr-iterator.md` (no more PR iteration loop)
- `agents/coderabbit-triage.md` (no more CodeRabbit react)
- Verify: `agents/perf-gatekeeper.md` — keep (perf review is general-purpose; not agentic)

**Files deleted (workflows)** — audit `.github/workflows/` for agentic-specific:
- Any workflow that triggers on PR comments matching CodeRabbit bot patterns
- Any workflow that dispatches a `claude` / `codex` subprocess
- `.coderabbit.yaml` STAYS — CodeRabbit review of PRs is still useful for non-agentic work.

**ADRs withdrawn (not deleted)**:
- `docs/adr/0003-github-as-itrackerclient.md` — **kept Accepted**. The decision to implement GitHub as `ITrackerClient` still holds; PR2's clean tracker honors it. Context paragraph references "agentic triage half" — that mention needs a one-line note: "Decision predates 2026-05-21 ripout; ITrackerClient choice still stands for the tracker-only role."
- `docs/adr/0004-pluggable-coding-harness-runner.md` — **Withdrawn (2026-05-21)**. No more harness runners.
- `docs/adr/0005-force-push-carve-out-for-spawned-agent-recovery.md` — **Withdrawn (2026-05-21)**. No more spawned-agent branches.
- `docs/adr/0006-orchestrator-pr-stays-draft-by-default.md` — **kept Accepted**. The draft-by-default rule still applies to orchestrator-opened PRs regardless of source.
- `docs/adr/0007-audit-trail-actor-column.md` — already Withdrawn.

**Files modified — config + controller + CMake**:

- [`Source_Core/include/ConfigManager.h`](../../Source_Core/include/ConfigManager.h:217) — delete the entire `#if defined(SMATCHET_WITH_AGENTIC)` block (lines 217 onwards): `GitHubPat`, `AgenticPollEnabled`, `AgenticPollIntervalSec`, `AgenticPollSource`, `AgenticPollQuery`, `HandoffHarnessBinPath`, `HandoffRunnerName`, `HandoffClarificationPostToGithub`, and all related handoff fields. Drop the `#if` / `#endif` wrapper.
- [`Source_Core/src/ConfigManager.cpp`](../../Source_Core/src/ConfigManager.cpp) — drop Load / Save / DPAPI-encrypt for every removed field. Legacy-config tolerance: silently ignore the deleted JSON keys on Load.
- [`Source_Core/include/AppController.h`](../../Source_Core/include/AppController.h) — delete `EnsureAgenticGithubClient` + `agenticGithubClient_` + `InitAgentProposalStoreOnWorker` + `RestartAgenticPoll` + every triage / handoff / react lambda binding decl + every controller member that holds an `AgenticTriageController` / `AgenticHandoffController` / `PrCommentWatcher` / `PrCheckRunWatcher` / `AgentProposalStore` reference.
- [`Source_Core/src/AppController.cpp`](../../Source_Core/src/AppController.cpp) — drop everything between approximately lines 1655-2200 (the GitHubClient lazy ctor + AuditSink lambdas + triage lambdas + watcher binding + scheduled-poll worker + handoff controller wire-up + AgentProposalStore worker init). Drop AgentTriageScenarioStep / AgentHandoffScenarioStep registrations. Drop AuditSink struct usage. AppController shrinks by ~600 LOC.
- [`Source_Core/include/BackendAuditTrail.h`](../../Source_Core/include/BackendAuditTrail.h) — KEEP. Used by Jira / Plane / new GitHub tracker writes. Triage-specific call sites disappear with the deleted TUs; the remaining backend-client call sites (Jira / Plane mutation + new GitHub UpdateField) keep using it unchanged.
- [`CMakeLists.txt`](../../CMakeLists.txt) — delete the entire `REMOVE_ITEM CORE_SOURCES` block (lines ~654-678) + the `if(SMATCHET_WITH_AGENTIC)` re-add block (lines ~678-700) + the second `if(SMATCHET_WITH_AGENTIC)` test-gating block (line 751). Delete the `SMATCHET_WITH_AGENTIC` option declaration + the DX12-side `set(SMATCHET_WITH_AGENTIC OFF)` line (line 175). Drop PUBLIC `target_compile_definitions` of `SMATCHET_WITH_AGENTIC`.

**Files modified — AGENTS.md + adjacent**:

- [`AGENTS.md`](../../AGENTS.md) — major doc surgery. Strip the following sections:
  - § Autonomous ship-loop default (most of it is handoff / merge-gates focused — strip handoff references; keep general "orchestrator runs end-to-end" framing)
  - § Merge gates (entire section — the poller is agentic-tied; without spawned-agents and CR-react loops the merge-gates infra has no consumer)
  - § Handoff envelope (entire section — no more sentinel files, no more spawn)
  - § Delegation § Debug-mode pause-loop (no more agentic interaction)
  - § Delegation § API-500 mid-run recovery (no more spawned children)
  - § Project rules § Force-push carve-out (no more agent/<id> branches)
  - § Trigger auto-activation rows for `handoff-implementer` / `pr-iterator` / `coderabbit-triage`
- [`docs/agent-rules/DELEGATION.md`](../../docs/agent-rules/DELEGATION.md) — strip the same handoff / API-500 / spawned-orchestrator first-move content.
- [`agents/_shared/`](../../agents/_shared/) — verify no agentic-only scripts remain.
- [`scripts/dev/merge-gates.sh`](../../scripts/dev/merge-gates.sh) + `.graphql` + `-prompt.sh` — **deleted**. The merge-gates poller is agentic-tied (CR-grace-window, CR-installed detection, dispatch-source-coupled halt prompts). For non-agentic squash-merge, `gh pr merge --squash` + GitHub branch-protection rules cover the same surface.
- [`tests/bats/merge_gates.bats`](../../tests/bats/merge_gates.bats) + `tests/fixtures/merge_gates_*.json` — deleted with the poller.

**Files modified — backlog**:

- `docs/backlog/agent-self-improvement/*.md` — strip entries that reference deleted surfaces (CoderabbitGrace / agentic-handoff-tooltip / etc.). Keep general-purpose entries (perf scenarios, golden-image contract, etc.).

### PR2 — Add GitHub as third tracker (clean addition on top of ripout)

Lands after PR1 merges. ~700 LOC net added.

**Files added (source)**:

- **NEW** [`Source_Core/include/GitHubClient.h`](../../Source_Core/include/) — fresh header. `class GitHubClient : public ITrackerClient`. Declares the `ITrackerClient` virtual overrides needed for grid + sync + field-edit + create-issue: `GetTrackerType`, `ProbeReachability`, `FetchIssues`, `FetchIssuesForKeys`, `FetchFieldCatalog`, `BuildBrowseUrl`, `ExtractProjectFromQuery`, `ListProjects`, `ResolveDisplayValue`, `UpdateIssueFields`, `UpdateField`, `BuildFieldPayload`, `BuildCreatePayload`, `CreateIssue`. **No** PR / check-run / GraphQL / triage-write methods. Ctor takes `baseUrl` + `pat`.
- **NEW** [`Source_Core/src/GitHubClient.cpp`](../../Source_Core/src/) — implementations. Internal-only private helpers: `MakeAuthHeaders`, `ComposeHttpError`, `RedactBody`, `PatchIssue` (for title/body/milestone), `ApplyLabelDelta` (for set-replace labels routing through GitHub's per-label POST/DELETE). PAT-empty short-circuit on every method; 5s connect / 15s overall timeout; `BackendAuditTrail::AppendBegin/AppendResult` on every write with `source="github_client"`.
- **NEW** [`Source_Core/include/GitHubClientHelpers.h`](../../Source_Core/include/) — pure helpers extracted as separate TU (mirrors `JiraIssueSearch.cpp` / `PlaneFieldCatalog.cpp` split convention): `ParseGitHubIssueKey(owner/repo#N → {owner, repo, number})`, `BuildIssueListUrlSuffix(since, perPage)`, `BuildIssuePatchUrlSuffix(owner, repo, n)`, `IsValidGitHubBaseUrl`, `ExtractGitHubErrorMessage`, `ParseIso8601ToUnixSec`. Pure; bucket-A testable.
- **NEW** [`Source_Core/src/GitHubClientHelpers.cpp`](../../Source_Core/src/) — implementations.
- **NEW** [`Source_Core/src/GitHubIssueSearch.cpp`](../../Source_Core/src/) — paginated `FetchIssues` + `FetchIssuesForKeys`. Mirrors `JiraIssueSearch.cpp` split.
- **NEW** [`Source_Core/src/GitHubFieldCatalog.cpp`](../../Source_Core/src/) — static catalog builder (6 native fields). Mirrors `PlaneFieldCatalog.cpp`.
- **NEW** [`Source_Core/include/LabelEditDiffPure.h`](../../Source_Core/include/) + [`Source_Core/src/LabelEditDiffPure.cpp`](../../Source_Core/src/) — `ComputeLabelEditDiff(currentLabels, intendedLabels) → {toAdd, toRemove}`. Pure helper for set-replace → additive primitive translation.

**Files modified**:

- [`Source_Core/src/DefaultTrackerBackendFactory.cpp`](../../Source_Core/src/DefaultTrackerBackendFactory.cpp:7) — add `"github"` branch: `return std::make_unique<GitHubClient>(cfg.GitHubBaseUrl, cfg.GitHubPat);`. Same shape as the Jira / Plane branches.
- [`Source_Core/include/ConfigManager.h`](../../Source_Core/include/ConfigManager.h) — add tracker-role fields (fresh, no `#if`): `GitHubPat`, `GitHubBaseUrl`, `GitHubOwner`, `GitHubRepo`.
- [`Source_Core/src/ConfigManager.cpp`](../../Source_Core/src/ConfigManager.cpp) — Load / Save / DPAPI-encrypt `GitHubPat`; plaintext Load / Save for the three other fields.
- [`Source_Core/src/SmatchetPreferencesUi.cpp`](../../Source_Core/src/SmatchetPreferencesUi.cpp) — add GitHub profile group under Tracker section.
- [`CMakeLists.txt`](../../CMakeLists.txt) — `CORE_SOURCES` `GLOB_RECURSE` picks the new files automatically; verify no stale REMOVE_ITEM remains (PR1 deleted that block).

**Files added (tests — bucket-A only)**:

- `tests/Source_Core/LabelEditDiffPure.test.cpp` — exhaustive set-diff.
- `tests/Source_Core/GitHubClient_FieldCatalog.test.cpp` — static catalog shape.
- `tests/Source_Core/GitHubClient_UpdateField.test.cpp` — router dispatch + internal label-diff pre-fetch + GET-current-labels assertion.
- `tests/Source_Core/GitHubFieldCatalog.test.cpp` — `ResolveDisplayValue`.
- `tests/Source_Core/GitHubClientHelpers.test.cpp` — pure helper round-trips (parser + URL builders + ISO parser).

### Internal `UpdateField` for set-fields (decision 3 pseudocode)

```
UpdateField(issueId, "labels", desiredSet):
    parse issueId via ParseGitHubIssueKey → {owner, repo, number}
    GET /repos/{owner}/{repo}/issues/{number} → currentLabels
    {toAdd, toRemove} = ComputeLabelEditDiff(currentLabels, desiredSet)
    for each label in toAdd:
        POST /repos/{owner}/{repo}/issues/{number}/labels
        audit_trail("label_add", "github_client", issueId, ...)
    for each label in toRemove:
        DELETE /repos/{owner}/{repo}/issues/{number}/labels/{name}
        audit_trail("label_remove", "github_client", issueId, ...)
    audit_trail one outer "field_update_labels" row spanning the batch
```

Audit-row nesting: outer `field_update_labels` row + N inner `label_add` / `label_remove` rows per primitive call. Consumers wanting field-edit-level only filter `action="field_update_*"`. Acceptable for realistic edit rates.

## Existing utilities reused

**PR1 (ripout)**: N/A — net deletion.

**PR2 (tracker)**:
- `ITrackerClient` virtuals — same surface Jira / Plane implement.
- `BackendAuditTrail::AppendBegin/AppendResult` — kept by PR1; new GitHub tracker writes use `source="github_client"`.
- `CachedTicket` + `LocalCacheManager` — shared shape; `FetchIssues` returns the same struct Jira / Plane do.
- `TrackerFieldCatalogResult` + `TrackerField` — [`TrackerFieldSchema.h`](../../Source_Core/include/TrackerFieldSchema.h).
- `cpr` — HTTP client (already linked into both targets).
- `nlohmann/json` — JSON parsing (already linked).

Pure helpers are net-new (the prior `GitHubClientHelpers.cpp` is deleted in PR1; PR2's new file shares only the names, not the implementation, with the deleted one). No code reuse from the agentic surface.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: tracker calls stay off-UI per existing `ITrackerClient` contract. GitHub set-field updates add one HTTP pre-fetch per label edit; expected `< 200 ms` total (network-bound, off-thread).
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: standard 5s connect / 15s overall timeouts; all call sites worker-thread; no new UI-thread entry points.
- **Pillar 3 (never crash)**: standard error handling; sanitizer-clean. The ripout (PR1) removes thousands of lines — sanitizer pass after PR1 confirms no dangling pointers / use-after-free from the deletion. PR2 is small + well-tested.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: Preferences UI changes reuse existing widget conventions; agentic UI panels deleted (no a11y regression — those panels are gone, not broken).

## Perf-review-system gates

- **PR1 (ripout)**: primary impact = binary size shrink. No new perf scenario needed. Run existing `idle` + `priority-grid-scroll` baselines to confirm no regression on common surfaces.
- **PR2 (tracker)**: primary scenarios `tracker_sync` + `tracker_label_edit`. Verify in `scripts/dev/perf-pr-fast-set.json`.
- **Pillar 2 static scanner**: no new sync-I/O reachable from `ImGui::*` in either PR.
- **Dispatcher drain**: no `MainThreadDispatcher::Drain()` touch.
- **Visible-cue bucket-E harness**: N/A — existing spinner / progress widgets cover the new tracker code paths.
- **Marker inventory**: PR1 deletes any `SMATCHET_UI_PERF_SCOPE` markers in the deleted TUs; regen `docs/perf/MARKER_INVENTORY.md` in PR1.

**Pre-push local check**: `bash scripts/dev/perf-run.sh idle priority-grid-scroll tracker_sync tracker_label_edit`.

## Risks / non-goals

**Risks**:

- **Massive deletion blast radius (PR1)** — ~5000-10000 LOC removed across 80+ files. Risk: a hidden non-agentic consumer of a deleted symbol breaks the build. Mitigation: dual-target build gate after every batch of deletions (`cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`); incremental commits within PR1 keep bisection cheap; sanitizer build (`ninja-test-msys2`) confirms no UB introduced by removed-but-still-referenced symbols.
- **Lost feature** — multiple shipped features (auto-triage, coderabbit-react, ci-react, claude-spawn handoff, merge-gates poller) are deleted. **User-confirmed**; recovery path is git history. Future re-introduction is a fresh design plan, not a revert.
- **AGENTS.md churn** — large doc surface lost; risk of orphan cross-references in other docs / agent files. Mitigation: grep for `Merge gates` / `Handoff envelope` / `dispatch_source` / `coderabbit-react` / `ci-react` after PR1 doc edits land; fix every hit.
- **ADR 0003 partial-applicability** — context paragraph references "agentic triage half" but the ITrackerClient decision still holds. Mitigation: one-line note in the ADR; full re-write deferred.
- **GitHub PATs vs OAuth** — PAT only this phase; `GitHubBaseUrl` covers GitHub Enterprise. OAuth follow-up.
- **GitHub rate limits** — 5000/hr PAT-authed. Tracker label-edit pre-fetch is one extra GET per edit; negligible.
- **CR install on PR1** — CodeRabbit is configured for this repo (`.coderabbit.yaml` stays). PR1's massive deletion may trigger long CR review or rate-limit. Acceptable; CR review of ~80-file deletion is informational, not blocking.

**Non-goals**:

- **Re-introducing any agentic feature in PR2** — PR2 is tracker-only. If you want triage back later, that's a fresh plan referencing the deleted code in git history.
- **Projects v2 custom fields via GraphQL** — separate follow-up.
- **GitHub Issues rich-text comment editor on grid** — Jira/Plane-only this phase.
- **PR-as-issue tracking** — tracker is issues-only.
- **GitHub Enterprise OAuth / SSO** — PAT only.
- **Migration tooling** — no cross-backend issue migration.
- **Bucket-E coverage for Preferences tracker switch** — backlog entry (`test` category).
- **`SMATCHET_WITH_AGENTIC` resurrection** — flag retired permanently in PR1.

## Verification

**PR1 (ripout)**:
- **Build gate**: dual-target build pass: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`. Must compile with zero agentic symbols.
- **Sanitizer gate**: `cmake --build --preset ninja-test-msys2` — ASan/UBSan clean post-deletion.
- **Test gate**: `bash scripts/dev/test-all.sh` — all surviving (non-agentic) tests pass.
- **Doc cross-ref gate**: `grep -r "AgenticTriageController\|PrCommentWatcher\|coderabbit-react\|handoff-implementer\|SMATCHET_WITH_AGENTIC\|dispatch_source\|merge-gates" docs/ agents/ AGENTS.md scripts/ tests/` returns zero hits (or only intentional historical references in this plan's git history).
- **Perf gate**: `idle` + `priority-grid-scroll` scenarios — no regression.

**PR2 (tracker)**:
- **Bucket A**:
  - `LabelEditDiffPure.test.cpp` — exhaustive set-diff.
  - `GitHubClient_FieldCatalog.test.cpp` — static catalog shape.
  - `GitHubClient_UpdateField.test.cpp` — router + internal pre-fetch + diff; **no field-id path makes a raw HTTP call outside the primitive set**.
  - `GitHubFieldCatalog.test.cpp` — `ResolveDisplayValue`.
  - `GitHubClientHelpers.test.cpp` — pure helper round-trips.
- **Build gate**: dual-target.
- **Sanitizer gate**: ASan/UBSan clean.
- **Perf gate**: `bash scripts/dev/perf-run.sh tracker_sync tracker_label_edit`.
- **Manual residue**: live-PAT smoke at first end-to-end run. Same gate Jira / Plane have today.

## Out of scope (flagged, not designed)

- **Re-introducing the agentic flow** — future plan; references this plan's git history (`git log --before=2026-05-21 -- Source_Core/src/Agentic*`) for the deleted reference impl.
- **Projects v2 custom fields** — follow-up.
- **Bitbucket / GitLab tracker backends** — different `ITrackerClient` impls; not designed here.
- **GitHub Apps + OAuth** — PATs cover immediate need.
- **Repo-multi-select on a single tracker profile** — one `owner/repo` per profile.
- **Bucket-E Preferences tracker-switch coverage** — backlog entry.
- **Re-spec ADR 0003 for the tracker-only role** — one-line note suffices; full re-spec deferred.

## Implementation log
*(populated post-ship per [`AGENTS.md`](../../AGENTS.md) § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
