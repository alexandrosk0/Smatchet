# Plan — Rip out C++ agentic code + add GitHub as third tracker

> **Slug**: `github-tracker-backend` (kept for PR continuity).
>
> **Mandatory rules cross-link**: see [`AGENTS.md`](../../AGENTS.md) § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.
>
> **Scope evolution**: 2026-05-20 initial draft (GitHub tracker only). 2026-05-21 a.m. expanded to triage tracker-agnostic refactor (8 locked decisions via grill-with-docs + 3 architect passes). 2026-05-21 mid-day scope reduced (refactor deferred). 2026-05-21 p.m. expanded to full agentic ripout incl. docs + AGENTS.md. 2026-05-21 evening narrowed: rip out only the **C++** agentic surface added by PRs whose title starts with `(agentic)` — keep all AGENTS.md sections, all docs/, all scripts, all agents/*.md, all ADRs, all `.github/workflows/` untouched. Code-only ripout, narrow + reversible. **2026-05-21 late evening grill-with-docs pass (4 plan revisions)**: (1) `SmatchetUI.cpp` added to PR1's modified-files list (was missing — mounts agentic UI panels at lines 31-32 includes + 710 + 714 Render calls); (2) AppController.cpp deletion now driven by a pre-walked symbol-name list, not "approximately lines 1655-2200" (file has 240 agentic-ref lines scattered through 3408 total — non-contiguous); (3) PR1 verification gains a mandatory boot-smoke gate (launch `Smatchet.exe --ephemeral` + reach first ImGui frame + exit cleanly) on top of bucket-A; (4) `docs/CONTEXT.md` glossary trim deferred to `agentic-ripout-doc-cleanup-v2.md` per user-confirmed decision-1 boundary (glossary references to `AgentProposal.sourceTracker`, `PrCommentWatcher`, `PrCheckRunWatcher`, `AgenticPollSource` stay in v1 — known stale, accepted).

## Context

Smatchet's agentic flow was added across **42 PRs** with `(agentic)` in the title between 2026-05-18 and 2026-05-21 (PR#217 to PR#345 — verified via `gh pr list --search "agentic in:title" --state merged`). The PRs added C++ surface for: triage controller, handoff controller, scheduled-poll worker, PR comment watcher, check-run watcher, CodeRabbit react loop, CI react loop, claude-spawn runner, sentinel-file machinery, audit-trail wiring + extensions, agent proposals UI, agent handoff UI, agent-triage scenarios, multiple builtin command surfaces, SQLite proposal store, GitHub client (read/write/PR/check-run/GraphQL surface), config fields, AppController plumbing.

User wants the **C++ code** from those PRs removed. Doc work, AGENTS.md sections, agent files, scripts, workflows, ADRs all **stay untouched** — they describe shipped behaviour at a point in time, and the user values keeping that history readable even if the runtime is gone. Bit-rot in docs is accepted.

GitHub is added as a third tracker on top of the C++-shrunk baseline with the same shape as `JiraClient` / `PlaneClient`: factory-owned `unique_ptr<GitHubClient>`, `ITrackerClient` virtuals only, zero coupling to the deleted surface.

Cross-link: prior plan revisions in this file's git history (`1c8135fd / 76c57d6c / e9eb0478 / 491f8425 / 7ae7e584 / 133674b3 / 3b37c7ee / 58c1980b / 48643f93`).

## Decisions locked

1. **C++ ripout only** — delete the C++ surface added by `(agentic)`-titled PRs. AGENTS.md untouched. All docs untouched (incl. `docs/plans/active/agentic-*.md`, `docs/agentic/`, `docs/agent-rules/`). ADRs untouched (incl. 0004 + 0005). Agent files (`agents/handoff-implementer.md`, `pr-iterator.md`, `coderabbit-triage.md`) untouched. Scripts untouched (incl. `scripts/dev/merge-gates.*`, `test-agentic-*`, `test-coderabbit-react`). `.github/workflows/` untouched. Bit-rot in docs accepted.
2. **`SMATCHET_WITH_AGENTIC` flag retired** — option removed from `CMakeLists.txt` because no remaining C++ consumer needs it. The flag's *intent* (gating an optional feature) becomes a no-op with the feature deleted. Doc references in AGENTS.md / agents/ stay as historical artifacts.
3. **Clean GitHub tracker, factory-owned** — `GitHubClient.cpp` + `GitHubClient.h` + `GitHubClientHelpers.{cpp,h}` are **rewritten from scratch** as tracker-only `ITrackerClient` implementation. Factory creates + owns a `unique_ptr<GitHubClient>` per `Create("github")` call — same shape as `JiraClient` / `PlaneClient`.
4. **`UpdateField` semantics = set-replace at the virtual** — `values` is the intended full set after edit. `GitHubClient::UpdateField` for labels / assignees pre-fetches the current set and diffs internally via `ComputeLabelEditDiff` pure helper.
5. **Static field catalog, no API** — 6 native fields (state, labels, assignees, milestone, title, body). Projects v2 deferred.
6. **Split into TWO PRs** — PR1 ships the C++ ripout (deletions + the 3 affected modify-files: ConfigManager, AppController, CMakeLists); PR2 ships the new tracker (clean additions on top).
7. **Verification = Bucket A pure-logic + boot-smoke gate** (boot-smoke added 2026-05-21 grill) — no bucket-B/E in either PR, but PR1 must additionally pass `Smatchet.exe --help` + `--ephemeral` first-frame smoke. Backlog entry for tracker-switch UI coverage.
8. **AppController.cpp deletion = symbol-list pre-walk** (added 2026-05-21 grill) — before any deletion, produce a verified list of every symbol/method/lambda/member to remove, surfaced for user sign-off. Line ranges from the original plan ("~lines 1655-2200") are inaccurate (240 agentic refs are scattered through 3408 LOC, not contiguous).
9. **`SmatchetUI.cpp` in PR1 modified-files list** (added 2026-05-21 grill) — the file mounts agentic UI panels at lines 31-32 (includes) + 710 + 714 (Render calls). PR1 must delete these or compile breaks.
10. **`docs/CONTEXT.md` glossary trim → v2 cleanup plan** (added 2026-05-21 grill) — glossary lines 18-20 reference deleted runtime (`AgentProposal.sourceTracker`, `PrCommentWatcher`, `PrCheckRunWatcher`, `AgenticPollSource`). Per decision 1's "all docs untouched" boundary, glossary trim stays out of v1; tracked as a known-stale anchor in [`agentic-ripout-doc-cleanup-v2.md`](agentic-ripout-doc-cleanup-v2.md).

## Approach

Two PRs, sequenced.

### PR1 — C++ agentic ripout (narrow, code-only)

Delete every `.h` / `.cpp` / `.test.cpp` added by `(agentic)`-titled PRs. Modify three files that hold consumer references (ConfigManager / AppController / CMakeLists). **Touch nothing else.**

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
- `Source_Core/src/GitHubClient.cpp` + `Source_Core/src/GitHubClientHelpers.cpp` — **deleted entirely** (added by PR#226 / #227 / #255; intimately woven with triage primitives + PR/check-runs/GraphQL; re-introduced fresh in PR2).
- `Source_Core/src/Commands/Builtin/BuiltinCommands_Agentic.cpp` + `_CiReact.cpp` + `_Coderabbit.cpp` + `_Handoff.cpp`

**NOT deleted from `Source_Core/src/`** (added in agentic PRs but reused by non-agentic surface):
- `SubprocessCapture.cpp` — lifted from P4Blame by PR#244; still used by P4Blame. Keep.

**Files deleted (headers — 22)**:
- `Source_Core/include/AgentProposal.h`
- `Source_Core/include/AgentProposalStore.h`
- `Source_Core/include/AgentTriageScenarioFixtures.h`
- `Source_Core/include/AgenticContextDoc.h`
- `Source_Core/include/AgenticHandoffController.h`
- `Source_Core/include/AgenticInferenceClient.h` + `AgenticInferenceClientPure.h`
- `Source_Core/include/AgenticProposalAuditPure.h`
- `Source_Core/include/AgenticTriageController.h`
- `Source_Core/include/AgentsMdLoader.h` (used by `AgenticContextDoc` only — verify no non-agentic consumer; the `agents/*.md` discovery layer if any non-agentic command parses agent frontmatter elsewhere stays)
- `Source_Core/include/CiFailureClassifier.h` + `CiFailureClassifierPure.h`
- `Source_Core/include/ClaudeCodeLocalRunner.h`
- `Source_Core/include/CoderabbitCommentClassifier.h` + `CoderabbitCommentClassifierPure.h`
- `Source_Core/include/CodingHarnessSeedBuilder.h` + `CodingHarnessTypes.h`
- `Source_Core/include/HarnessRunState.h`
- `Source_Core/include/OpenPrRegistrar.h`
- `Source_Core/include/PrCheckRunClassifier.h` + `PrCheckRunWatcher.h`
- `Source_Core/include/PrCommentClassifier.h`
- `Source_Core/include/GitHubClient.h` + `GitHubClientHelpers.h` — deleted; re-introduced fresh in PR2.
- `Source_Core/include/SmatchetAgentProposalsUiPure.h` + the inline `SmatchetAgentHandoffUi.h` / `SmatchetAgentProposalsUi.h` headers under `src/`.

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

**Files NOT touched (per "AGENTS.md untouched + docs untouched" decision 1)**:
- `AGENTS.md` (Merge gates / Handoff envelope / Debug-mode pause-loop / API-500 mid-run recovery / Force-push carve-out / etc. all stay verbatim).
- `docs/plans/active/agentic-coding-handoff.md` + `agentic-flow-implementation.md` + `agentic-triage-flow.md` + `coderabbit-react-loop.md` + `agent-contract-alignment.md` — stay.
- `docs/agentic/TRIAGE_MANUAL.md` + `USAGE.md` — stay.
- `docs/agent-rules/delegation.md` — stay.
- `agents/handoff-implementer.md` + `pr-iterator.md` + `coderabbit-triage.md` — stay.
- All ADRs unchanged: `0003-github-as-itrackerclient.md` (Accepted), `0004-pluggable-coding-harness-runner.md` (Accepted), `0005-force-push-carve-out-for-spawned-agent-recovery.md` (Accepted), `0006-orchestrator-pr-stays-draft-by-default.md` (Accepted), `0007-audit-trail-actor-column.md` (Withdrawn — left at current Withdrawn state from prior session).
- `scripts/dev/merge-gates.sh` + `.graphql` + `-prompt.sh` — stay.
- `scripts/dev/test-agentic-handoff-*.sh` + `test-coderabbit-react.sh` + `test-ui-agent-*.sh` — stay (non-functional after C++ deletion; bit-rot accepted).
- `tests/bats/merge_gates.bats` + `tests/fixtures/merge_gates_*.json` — stay (bats not C++; tests the deleted poller's behaviour but the bats runner against the bash poller still works).
- `.github/workflows/` — every workflow stays.
- `.coderabbit.yaml` — stays.
- `docs/backlog/agent-self-improvement/*.md` — stay (entries reference deleted surfaces but the backlog is descriptive, not enforcing).

**Files modified (6 — narrow surgery only)**:

1. [`Source_Core/include/ConfigManager.h`](../../Source_Core/include/ConfigManager.h:217) — delete the entire `#if defined(SMATCHET_WITH_AGENTIC)` block (lines 217 onwards through the matching `#endif`): `GitHubPat`, `AgenticPollEnabled`, `AgenticPollIntervalSec`, `AgenticPollSource`, `AgenticPollQuery`, `HandoffHarnessBinPath`, `HandoffRunnerName`, `HandoffClarificationPostToGithub`, and all related handoff fields. Drop the `#if` / `#endif` wrapper.
2. [`Source_Core/src/ConfigManager.cpp`](../../Source_Core/src/ConfigManager.cpp) — drop Load / Save / DPAPI-encrypt for every removed field. Legacy-config tolerance: silently ignore the deleted JSON keys on Load.
3. [`Source_Core/include/AppController.h`](../../Source_Core/include/AppController.h) — delete `EnsureAgenticGithubClient` + `agenticGithubClient_` + `InitAgentProposalStoreOnWorker` + `RestartAgenticPoll` + every triage / handoff / react lambda binding declaration + every controller member that holds an `AgenticTriageController` / `AgenticHandoffController` / `PrCommentWatcher` / `PrCheckRunWatcher` / `AgentProposalStore` reference.
4. [`Source_Core/src/AppController.cpp`](../../Source_Core/src/AppController.cpp) — **driven by a verified symbol-name list, not line ranges** (per 2026-05-21 grill — file is 3408 LOC with 240 agentic-ref lines scattered, not contiguous). Pre-walk step: `grep -nE "Agentic|AgentProposal|AgentHandoff|AgentTriage|PrCommentWatcher|PrCheckRunWatcher|OpenPrRegistrar|GitHubClient|CodingHarness|ClaudeCodeLocalRunner|HarnessRunState" Source_Core/src/AppController.cpp` → emit a per-symbol delete list (function bodies, lambda captures, ctor/dtor blocks, member initializers, scenario registrations). Surface the list to user for sign-off before any deletion. Drop AuditSink struct usage. AppController shrinks by ~600 LOC at the implementation level + 240 scattered references.
5. [`Source_Core/src/SmatchetUI.cpp`](../../Source_Core/src/SmatchetUI.cpp) — **NEW (added by 2026-05-21 grill)**. Delete `#include "SmatchetAgentHandoffUi.h"` + `#include "SmatchetAgentProposalsUi.h"` (lines 31-32 on develop tip) + the two `SmatchetAgentProposalsUi::Render` + `SmatchetAgentHandoffUi::Render` mount calls (lines 710 + 714 on develop tip) including their surrounding `SMATCHET_UI_PERF_SCOPE` markers. Without this PR1 fails to compile.
6. [`CMakeLists.txt`](../../CMakeLists.txt) — delete the entire `REMOVE_ITEM CORE_SOURCES` block (lines ~654-678) + the `if(SMATCHET_WITH_AGENTIC)` re-add block (lines ~678-700) + the second `if(SMATCHET_WITH_AGENTIC)` test-gating block (line 751). Delete the `SMATCHET_WITH_AGENTIC` option declaration + the DX12-side `set(SMATCHET_WITH_AGENTIC OFF)` line (line 175). Drop the PUBLIC `target_compile_definitions` of `SMATCHET_WITH_AGENTIC`.

Net PR1 surface: ~80 files deleted, 6 files modified, **0 doc / script / workflow / ADR / agent-md / AGENTS.md files touched**. ~5000-10000 LOC removed.

### PR2 — Add GitHub as third tracker (clean addition)

Lands after PR1 merges. ~700 LOC net added.

**Files added (source)**:

- **NEW** [`Source_Core/include/GitHubClient.h`](../../Source_Core/include/) — `class GitHubClient : public ITrackerClient`. Declares `ITrackerClient` virtual overrides for grid + sync + field-edit + create-issue: `GetTrackerType`, `ProbeReachability`, `FetchIssues`, `FetchIssuesForKeys`, `FetchFieldCatalog`, `BuildBrowseUrl`, `ExtractProjectFromQuery`, `ListProjects`, `ResolveDisplayValue`, `UpdateIssueFields`, `UpdateField`, `BuildFieldPayload`, `BuildCreatePayload`, `CreateIssue`. **No** PR / check-run / GraphQL / triage-write methods. Ctor takes `baseUrl` + `pat`.
- **NEW** [`Source_Core/src/GitHubClient.cpp`](../../Source_Core/src/) — implementations. Internal-only private helpers: `MakeAuthHeaders`, `ComposeHttpError`, `RedactBody`, `PatchIssue` (for title/body/milestone), `ApplyLabelDelta` (for set-replace labels routing through GitHub's per-label POST/DELETE). PAT-empty short-circuit on every method; 5s connect / 15s overall timeout; `BackendAuditTrail::AppendBegin/AppendResult` on every write with `source="github_client"`.
- **NEW** [`Source_Core/include/GitHubClientHelpers.h`](../../Source_Core/include/) — pure helpers as separate TU (mirrors `JiraIssueSearch.cpp` / `PlaneFieldCatalog.cpp` split convention): `ParseGitHubIssueKey(owner/repo#N → {owner, repo, number})`, `BuildIssueListUrlSuffix(since, perPage)`, `BuildIssuePatchUrlSuffix(owner, repo, n)`, `IsValidGitHubBaseUrl`, `ExtractGitHubErrorMessage`, `ParseIso8601ToUnixSec`. Pure; bucket-A testable.
- **NEW** [`Source_Core/src/GitHubClientHelpers.cpp`](../../Source_Core/src/) — implementations.
- **NEW** [`Source_Core/src/GitHubIssueSearch.cpp`](../../Source_Core/src/) — paginated `FetchIssues` + `FetchIssuesForKeys`. Mirrors `JiraIssueSearch.cpp` split.
- **NEW** [`Source_Core/src/GitHubFieldCatalog.cpp`](../../Source_Core/src/) — static catalog builder (6 native fields). Mirrors `PlaneFieldCatalog.cpp`.
- **NEW** [`Source_Core/include/LabelEditDiffPure.h`](../../Source_Core/include/) + [`Source_Core/src/LabelEditDiffPure.cpp`](../../Source_Core/src/) — `ComputeLabelEditDiff(currentLabels, intendedLabels) → {toAdd, toRemove}`. Pure helper.

**Files modified**:

- [`Source_Core/src/DefaultTrackerBackendFactory.cpp`](../../Source_Core/src/DefaultTrackerBackendFactory.cpp:7) — add `"github"` branch: `return std::make_unique<GitHubClient>(cfg.GitHubBaseUrl, cfg.GitHubPat);`. Same shape as Jira / Plane branches.
- [`Source_Core/include/ConfigManager.h`](../../Source_Core/include/ConfigManager.h) — add tracker-role fields (no `#if`): `GitHubPat`, `GitHubBaseUrl`, `GitHubOwner`, `GitHubRepo`.
- [`Source_Core/src/ConfigManager.cpp`](../../Source_Core/src/ConfigManager.cpp) — Load / Save / DPAPI-encrypt `GitHubPat`; plaintext Load / Save for the three other fields.
- [`Source_Core/src/SmatchetPreferencesUi.cpp`](../../Source_Core/src/SmatchetPreferencesUi.cpp) — add GitHub profile group under Tracker section.
- [`CMakeLists.txt`](../../CMakeLists.txt) — `CORE_SOURCES` `GLOB_RECURSE` picks the new files automatically; verify no stale REMOVE_ITEM remains (PR1 deleted that block).

**Files added (tests — bucket-A only)**:

- `tests/Source_Core/LabelEditDiffPure.test.cpp` — exhaustive set-diff.
- `tests/Source_Core/GitHubClient_FieldCatalog.test.cpp` — static catalog shape.
- `tests/Source_Core/GitHubClient_UpdateField.test.cpp` — router dispatch + internal label-diff pre-fetch + GET-current-labels assertion.
- `tests/Source_Core/GitHubFieldCatalog.test.cpp` — `ResolveDisplayValue`.
- `tests/Source_Core/GitHubClientHelpers.test.cpp` — pure helper round-trips (parser + URL builders + ISO parser).

### Internal `UpdateField` for set-fields (decision 4 pseudocode)

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
- `CachedTicket` + `LocalCacheManager` — shared shape; `FetchIssues` returns the same struct.
- `TrackerFieldCatalogResult` + `TrackerField` — [`TrackerFieldSchema.h`](../../Source_Core/include/TrackerFieldSchema.h).
- `cpr` — already linked into both targets.
- `nlohmann/json` — already linked.

Pure helpers are net-new; no code reuse from the agentic surface.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: tracker calls off-UI per `ITrackerClient` contract. GitHub set-field updates add one HTTP pre-fetch per label edit; expected `< 200 ms` total (network-bound, off-thread).
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: 5s connect / 15s overall timeouts; worker-thread; no new UI-thread entry points.
- **Pillar 3 (never crash)**: standard error handling; sanitizer-clean. PR1 deletion is mechanical (delete header → consumer file fails to compile → drop the consumer); sanitizer pass after PR1 confirms no UB introduced by removed-but-still-referenced symbols.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: Preferences UI changes reuse existing widget conventions; agentic UI panels deleted (no a11y regression).

## Perf-review-system gates

- **PR1 (ripout)**: primary impact = binary size shrink. No new perf scenario. Run existing `idle` + `priority-grid-scroll` baselines to confirm no regression.
- **PR2 (tracker)**: primary scenarios `tracker_sync` + `tracker_label_edit`. Verify in `scripts/dev/perf-pr-fast-set.json`.
- **Pillar 2 static scanner**: no new sync-I/O reachable from `ImGui::*` in either PR.
- **Dispatcher drain**: no `MainThreadDispatcher::Drain()` touch.
- **Visible-cue bucket-E harness**: N/A.
- **Marker inventory**: PR1 deletes any `SMATCHET_UI_PERF_SCOPE` markers in the deleted TUs; regen `docs/perf/MARKER_INVENTORY.md` in PR1.

**Pre-push local check**: `bash scripts/dev/perf-run.sh idle priority-grid-scroll tracker_sync tracker_label_edit`.

## Risks / non-goals

**Risks**:

- **Doc / script / AGENTS.md bit-rot** — per decision 1, all docs + AGENTS.md sections + scripts + ADRs + agent files stay verbatim. They describe deleted behaviour. Future readers must understand "this section describes shipped code that was ripped out 2026-05-21; see git log on `Source_Core/src/Agentic*` for the reference impl." Accepted; user-confirmed bit-rot.
- **Deletion blast radius (PR1)** — ~5000-10000 LOC removed across ~80 files. Risk: a hidden non-agentic consumer of a deleted symbol breaks the build. Mitigation: dual-target build gate after every batch (`cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`); incremental commits within PR1 keep bisection cheap; sanitizer build (`ninja-test-msvc`) confirms no UB.
- **Lost feature** — auto-triage, coderabbit-react, ci-react, claude-spawn handoff, merge-gates poller (the C++ side of it) are deleted at runtime. Bash side of merge-gates stays (no C++ deps). User-confirmed; recovery path is git history.
- **ADR 0004 (pluggable coding harness runner) + 0005 (force-push carve-out) document removed C++ surface** — left Accepted per decision 1. Future ADR-readers must triangulate "Accepted but C++ impl is gone — historical decision, not current."
- **`SMATCHET_WITH_AGENTIC` retired in CMake but referenced in `AGENTS.md` / `docs/` / `agents/*.md`** — stale references; bit-rot accepted.
- **`scripts/dev/test-agentic-*.sh` + `test-coderabbit-react.sh` + `test-ui-agent-*.sh` stay but fail** — their C++ targets are gone; running them errors out. Stay per decision 1.
- **`scripts/dev/merge-gates.*` (bash poller) stays** — calls `gh api graphql` against PR state; doesn't depend on Smatchet C++. Continues to work for non-agentic PRs.
- **GitHub PATs vs OAuth** — PAT only this phase; `GitHubBaseUrl` covers Enterprise. OAuth follow-up.
- **GitHub rate limits** — 5000/hr PAT-authed. Tracker label-edit pre-fetch is one extra GET per edit; negligible.
- **CR review on PR1** — `.coderabbit.yaml` stays; PR1's massive deletion may trigger long CR review. Acceptable.

**Non-goals**:

- **Re-introducing any agentic feature in PR2** — PR2 is tracker-only.
- **Touching any doc / AGENTS.md section / script / agent file / ADR / workflow** — decision 1 explicit boundary.
- **Projects v2 custom fields via GraphQL** — separate follow-up.
- **GitHub Issues rich-text comment editor on grid** — Jira/Plane-only this phase.
- **PR-as-issue tracking** — issues only.
- **GitHub Enterprise OAuth / SSO** — PAT only.
- **Migration tooling** — none.
- **Bucket-E coverage for Preferences tracker switch** — backlog entry (`test` category).
- **`SMATCHET_WITH_AGENTIC` resurrection** — flag retired permanently in PR1 (CMake-side only).
- **Doc cross-reference cleanup** — bit-rot in docs accepted; no grep-and-strip pass.

## Verification

**PR1 (ripout)**:
- **Build gate**: dual-target build pass: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`. Must compile with zero agentic C++ symbols.
- **Sanitizer gate**: `cmake --build --preset ninja-test-msvc` — ASan/UBSan clean post-deletion.
- **Test gate**: `bash scripts/dev/test-all.sh` — all surviving (non-agentic) tests pass.
- **Boot-smoke gate (mandatory; added 2026-05-21 grill)**: `Smatchet.exe --help` exits 0 within 5s + `Smatchet.exe --ephemeral` reaches the first ImGui frame + cleanly exits on `app.quit`. Catches missed UI-mount deletions (e.g. SmatchetUI.cpp ghost `Render(app, d)` calls referencing deleted panels) that compile-pass but crash on first draw. Script: extend `scripts/dev/test-boot-smoke.sh` (if absent, ship as part of PR1) — wraps both spawn checks + a 5s watchdog. Exit-0 required to merge.
- **NOT a gate**: doc cross-ref grep over `docs/` / `AGENTS.md` / `agents/`. Stale references are intentional (decision 1).
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
- **Manual residue**: live-PAT smoke at first end-to-end run.

## Out of scope (flagged, not designed)

- **Re-introducing the agentic flow** — future plan.
- **Doc / AGENTS.md / agent-md / script / workflow / ADR cleanup of stale references** — bit-rot accepted per decision 1. **v2 follow-up plan**: [`agentic-ripout-doc-cleanup-v2.md`](agentic-ripout-doc-cleanup-v2.md) (stub; lands after v1 PR1 + PR2 merge). Covers: strip the deleted handoff-envelope section (added by PR#248/#299/#300); delete `agents/handoff-implementer.md` + `pr-iterator.md` + `coderabbit-triage.md`; delete `docs/agentic/` + `docs/plans/active/agentic-*.md` + `coderabbit-react-loop.md`; withdraw ADRs 0004 + 0005; delete the 11 (agentic)-PR-added scripts (`test-agentic-triage-cli.sh`, `test-ui-agent-proposals.sh`, `test-agentic-approve-reject.sh`, `test-agentic-handoff-cli.sh` + `-clarification.sh` + `-iterate.sh` + `-scenario.sh`, `test-ui-agent-handoff.sh`, `test-ui-agent-proposals-handoff-button.sh`, `test-ci-react.sh`, `test-coderabbit-react.sh`); audit `.github/workflows/`; trim AGENTS.md § Merge gates / § Autonomous ship-loop / § Post-ship § option 3 / § Force-push carve-out (grill-with-docs at v2 time decides aggressive vs surgical).
- **Projects v2 custom fields** — follow-up.
- **Bitbucket / GitLab tracker backends** — different `ITrackerClient` impls.
- **GitHub Apps + OAuth** — PATs cover immediate need.
- **Repo-multi-select on a single tracker profile** — one `owner/repo` per profile.
- **Bucket-E Preferences tracker-switch coverage** — backlog entry.

## Implementation log

- `b1d241bc` · PR1 — C++ agentic ripout (85 deletions, 12 mods); merged via #356.
- `cd66e28c` · PR2 — GitHub as third ITrackerClient backend (substrate: factory + client shell + helpers + static field catalog; HTTP impl stubbed); merged via #357.
- `c76256de` · PR2 fixup — applied 5 CodeRabbit findings missed on #357.
- `67e3abb4` · PR3 — Preferences UI dropdown + TicketSyncService GitHub backend swap + in-memory cache-clear on backend-kind change. Bundled three originally-separate slices because the dropdown is non-functional without the swap, and the swap surfaced the pre-existing Jira↔Plane cache-stale bug.
- `25ef6a50` · Adjacent fix shipped via #379 — `fix(dx12): guard WhisperAi scenario refs with SMATCHET_WITH_AI`. Not strictly part of this plan, but pre-existing dual-target breakage on develop tip was blocking PR3 verification.
- `cc0e1a00` · PR5 — JQL → GitHub-search translator (`Source_Core/include/GitHubQueryFromJql.h` + `Source_Core/src/GitHubQueryFromJql.cpp`) + 19 bucket-A doctest cases. Pure-helper TU; no I/O, no globals. Maps the JQL subset used by Smatchet view editors (assignee/status/labels/text/reporter + currentUser() + AND/OR + ORDER BY) to GitHub `/search/issues` qualifier syntax. Unsupported terms drop with a non-fatal `Warning`.
- `be82becd` · PR4 — `FetchIssues` + `FetchIssuesForKeys` HTTP impl. New `Source_Core/src/GitHubIssueSearch.{cpp,h}` paginated REST fetcher (cross-repo `/search/issues` or repo-scoped `/repos/{o}/{r}/issues`). `BuildGitHubHeaders` hoisted from `GitHubClient.cpp`'s anon namespace into `smatchet::github` so the helper TU shares the same header builder. 10-page soft cap, 30s overall timeout, pull-requests filtered out of the repo-scoped path. PR9's `BuildGitHubHeaders` was the dependency anchor — PR4's branch was based off `feat/github-tracker-pr9-probereachability` to inherit it without cherry-pick.
- 2026-07-13 · PR7 — `UpdateIssueFields` + `UpdateField` HTTP impl (the last two GitHubClient stubs). `PATCH /repos/{o}/{r}/issues/{n}` for title/body/state/assignees/milestone; labels reconcile per decision 4 (pre-fetch `GET /issues/{n}` → `ComputeLabelEditDiff` → batch `POST /labels` + per-name `DELETE /labels/{name}`). New pure TU `Source/Core/src/Tracker/GitHubMutationPure.{h,cpp}` (fields→PATCH-plan translation, milestone title→number lookup, label-name extraction) mirroring the `LinearMutationPure` split. `BuildFieldPayload` rewritten to key on the catalog ids the grid actually edits with (`summary`/`description`/`status`/`assignee`/`labels`/`milestone`) — the PR2 substrate keyed on GitHub-native names (`title`/`state`/`assignees`) that never matched the catalog, so every field edit would have failed at payload build. New shared `TrackerDeleteLogged` verb helper in `TrackerHttpUtils` (idempotent retry policy, same shape as PUT/PATCH). Audit: `issue_update_fields` begin/result pair with `source="github_client"` bracketing inner `label_add`/`label_remove` rows. Tests: `tests/Core/GitHubMutationPure.test.cpp` (bucket-A pure) + `tests/Core/GitHubIssueMutationHttp.test.cpp` (httplib-loopback fixture under `TestEnvGuard`, JiraIssueMutationHttp precedent; the fixture gained DELETE routing).

## Deviations from plan

- **PR2 HTTP impl deferred**: plan §PR2 implied complete `GitHubClient` HTTP methods (`FetchIssues`, `FetchIssuesForKeys`, `UpdateField`, `UpdateIssueFields`, `CreateIssue`, `ProbeReachability`); shipped substrate emits `"… HTTP impl deferred to a follow-up slice of docs/plans/shipped/github-tracker-backend.md PR2"` via a shared `StubError` helper. Tracked as follow-up PRs (see § Remaining for GitHub issues to work below).
- **PR3 not in original plan**: original doc had two PRs; PR3 was carved out post-merge of PR2 once it became clear the Preferences UI line item §152 was load-bearing on its own. PR3 also expanded scope to include `TicketSyncService` swap + cache-clear because `SmatchetPreferencesUi.cpp:2666 app.SyncWithBackend(...)` would otherwise no-op on the GitHub case.
- **PR4/PR5 bundled in one PR**: plan §300 sequenced them as separate PRs; shipped as a single bundle because PR4's fetcher directly consumes PR5's translator — splitting them would have left PR4 referencing a not-yet-merged symbol. Commits are split for reviewability (PR5 commit first, then PR4).
- **Repo-scoped path drops JQL**: GitHub's `/repos/{o}/{r}/issues` REST endpoint does NOT accept a `q=` search expression. Plan §145 implied the same URL shape would handle both flows; reality forced the split. When a user is in repo-scoped mode AND has a non-empty JQL, we surface a `Warning` explaining the JQL was dropped and the result is the full repo issue list. Cross-repo (search) mode honours JQL.
- **10-page soft cap**: plan §300 didn't specify a pagination cap. Cap at 1000 issues to match `PlaneIssueSearch`'s `kMaxPlanePages * 100` safety limit; when reached, surfaces a `Warning` and returns the partial result rather than failing. Future PR can raise the cap or switch to cursor-based pagination if needed.
- **Field mapping unified**: cross-repo + repo-scoped paths share one `MapIssueToCachedTicket` helper. The cross-repo `/search/issues` response embeds `repository_url`; the repo-scoped + single-issue paths fall back to the `ownerHint`/`repoHint` parameters from the URL itself.
- **Pull-request filter**: `/repos/{o}/{r}/issues` returns issues AND PRs in one stream (they share the issues table server-side). PR4 filters out any object with a `pull_request` field so the tracker grid is issue-only. Plan didn't call this out — discovered while reviewing the GitHub REST docs.
- **PR7 — label reconcile lives in `UpdateIssueFields`, not `UpdateField`**: the plan's §163 pseudocode hangs the diff flow off `UpdateField`, but the live grid-edit pipeline (`FieldEditPipelineService::SubmitFieldEditRegular`) calls `BuildFieldPayload → UpdateIssueFields`; `UpdateField` is a thin router through that same path (JiraClient/LinearClient routing). The reconcile therefore triggers on the `labels` key of the fields payload wherever it enters.
- **PR7 — batch label add**: §163 sketched one POST per added label; GitHub's `POST /issues/{n}/labels` accepts `{"labels": [...]}` in one call, so additions land as a single batch POST (one `label_add` audit row carrying the array). Removals stay per-name DELETE (one `label_remove` row each) because the DELETE endpoint is per-label. Audit action names follow the shipped taxonomy (`issue_update_fields` outer pair) rather than the plan's `field_update_labels` sketch.
- **PR7 — milestone title→number resolve**: `PATCH` takes the milestone NUMBER but the grid stores/edits the display title (`GitHubIssueSearchMapping`), which the plan didn't call out. Non-empty non-numeric values resolve via paginated `GET /repos/{o}/{r}/milestones?state=all` (exact title match; miss → `InvalidRequest` naming the title); all-digit values pass through as the number; empty clears via JSON null.
- **PR7 — DELETE 404 = converged**: removing a label that is already gone reports success (set-replace only cares about the end state), so a concurrent removal never fails the edit.
- **PR7 — `[PR] ` summary-prefix strip**: the grid displays PR titles with a `[PR] ` prefix; a title edit round-trips it, so `BuildGitHubIssueUpdatePlan` strips exactly one leading occurrence before PATCHing. Known limitation: a genuine issue title deliberately starting with `[PR] ` cannot be set from Smatchet.

## Verification (actual)

- PR3 dual-target build: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — passed. `libSmatchetCore_DX12.a` linked (step 194/198), `Smatchet.exe` linked (step 195/198).
- PR3 ctest: `ctest --test-dir build/ninja-test-msvc` — 2/2 pass (`smatchet_tests` 1.89 s, `smatchet_lua_tests` 0.01 s).
- PR3 visual verify: user-driven against `build/ninja-iter-msvc/Smatchet.exe` from worktree `.claude/worktrees/agent-a42ad27499adf9969/`. Log evidence:
  ```text
  [INFO] Updated tracker config (GitHub). BaseUrl='https://api.github.com' (PAT length=93)
  [INFO] GitHubClient: ctor baseUrl='https://api.github.com' pat_bytes=93
  [INFO] TicketSyncService: Switched backend to GitHub.
  [INFO] TicketSyncService: Cleared in-memory ActiveTickets on backend-kind change.
  [INFO] TickStreamingApply finished sync session. saved_or_kept=0 total_stale=0 fullSync=0 err=FetchIssues: GitHubClient HTTP impl deferred to a follow-up slice of docs/plans/shipped/github-tracker-backend.md PR2
  ```
- Dropdown shows three options; GitHub config inputs render with correct tooltips; switching tracker kind clears the grid (in-memory `ActiveTickets`). Stub error surfaces as designed.
- PR4/PR5 dual-target build: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — passed clean. Both `Smatchet.exe` and `libSmatchetCore_DX12.a` linked.
- PR4/PR5 ctest: `ctest --test-dir build/ninja-test-msvc --output-on-failure` — 2/2 pass (`smatchet_tests` 1.99 s, `smatchet_lua_tests` 0.04 s). `SmatchetTests --test-case="*Translate*"` reports 19 / 19 PR5 translator cases pass; `--test-case="*GitHub*"` reports 26 / 26 GitHub-related cases pass overall (PR2 helpers + PR5 translator).

## Remaining for GitHub issues to work

Ordered ship list (each ≈ a single PR; estimates rough). **Status reconciled 2026-07-13** — the backend has since reached functional read/write parity with Jira / Plane / Linear:

1. ✅ **PR4 — `FetchIssues` + `FetchIssuesForKeys` HTTP impl** — shipped (`be82becd`; later extended with streamed per-page emission, a GraphQL search path, PR rows via `github-tracker-pr12-prs-in-grid.md`, and commit rows via `github-commit-tracker-rows.md`).
2. ✅ **PR5 — view-query translation** — shipped (`cc0e1a00`; `GitHubQueryFromJql.{h,cpp}`, option (a)).
3. ✅ **PR6 — `GitHubOwner` + `GitHubRepo` config + Preferences inputs** — shipped (fields live in `TrackerConfig` + Preferences; they select cross-repo vs repo-scoped fetch).
4. ✅ **PR7 — `UpdateField` + `UpdateIssueFields` HTTP impl** — shipped 2026-07-13 (see § Implementation log + § Deviations). `PATCH /repos/{o}/{r}/issues/{n}` for title/body/state/milestone/assignees; label diff via batch `POST` + per-name `DELETE` over `LabelEditDiffPure`; audit-trail with `source="github_client"`.
5. ✅ **PR8 — `CreateIssue` + `BuildCreatePayload`** — shipped (`log-a-bug-github.md` Slice 1 landed `BuildGitHubCreatePayload` + the POST path).
6. ✅ **PR9 — `ProbeReachability` real probe** — shipped (`GET /rate_limit`, status→`TrackerReachabilityProbeKind` matrix, fixture-tested in `GitHubClientHttp.test.cpp`).
7. ⬜ **PR10 — bucket-E coverage for Preferences tracker switch** (~150 LOC). Still open (backlog). ImGui Test Engine scenario asserting the dropdown switches the live backend instance + clears tickets.
8. ◐ **PR11 — rate-limit + retry policy** — largely superseded: every GitHub verb now routes through the `Tracker*Logged` helpers, whose shared `TrackerHttpRequestWithRetry` wrapper retries Transport / 429 / 5xx with exponential backoff (idempotent verbs) and transport-only for POST. Remaining delta (open, low priority): honouring a `Retry-After` header on GitHub's secondary-rate-limit `403` responses — those currently classify as auth-shaped and don't retry.

Remaining: PR10 + the PR11 `Retry-After` delta. `ListProjects` (`GET /user/repos`) also still returns empty — deferred since PR2, not on this list originally.
