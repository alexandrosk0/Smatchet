# Small-helper clones — grandfathered baseline

_Auto-generated. Do not hand-edit; run `bash agents/scripts/core/test-small-helper-audit.sh --baseline` and commit._
_The gate (`small_helper_audit.py --check`) is ADVISORY: it WARNs on groups absent from this file and never blocks. Graduation to blocking is a separate decision (mirrors ADR-0015)._

_Band: 25 <= body tokens < 70 (dup_audit.py's MIN_CLONE_TOKENS, imported). A group is a body shared by >= 3 distinct TUs._

## Totals
- groups grandfathered: 7
- call sites across all groups: 23

## group `9d2ab61b908a46ad` — 5 TUs, 42 tokens, name(s): `NowUnixMs`, `NowEpochMs`, `NowUnixSeconds`, `PlaneNowUnixSeconds`, `NowMonotonicMs`
- `Source/Core/src/AiChatTimestamp.cpp:11` — `NowUnixMs`
- `Source/Core/src/Persistence/BackendAuditTrail.cpp:40` — `NowEpochMs`
- `Source/Core/src/Tracker/JiraClient.cpp:54` — `NowUnixSeconds`
- `Source/Core/src/Tracker/PlaneIssueSearch.cpp:120` — `PlaneNowUnixSeconds`
- `Source/Plugins/Mcp/McpPlugin.cpp:70` — `NowMonotonicMs`

## group `e1e02f199b2d8b07` — 3 TUs, 65 tokens, name(s): `OnFrame`
- `Source/Core/src/Commands/Scenarios/AttachmentPreviewOpenScenario.cpp:37` — `OnFrame`
- `Source/Core/src/Commands/Scenarios/SideBySide2GridScenario.cpp:95` — `OnFrame`
- `Source/Core/src/Commands/Scenarios/SideBySideNGridScenario.cpp:106` — `OnFrame`

## group `01309970925065a9` — 3 TUs, 53 tokens, name(s): `JoinUrl`
- `Source/Core/src/AnthropicClient.cpp:30` — `JoinUrl`
- `Source/Core/src/OllamaClient.cpp:31` — `JoinUrl`
- `Source/Core/src/OpenAiClient.cpp:36` — `JoinUrl`

## group `98a26c787ee073fd` — 3 TUs, 42 tokens, name(s): `BugReportGitHubHeaders`, `BuildGitHubHeaders`, `GitHubCommitFeedHeaders`
- `Source/Core/src/Diagnostics/BugReportService.cpp:58` — `BugReportGitHubHeaders`
- `Source/Core/src/Tracker/GitHubClient.cpp:82` — `BuildGitHubHeaders`
- `Source/Core/src/Vcs/GitHubCommits.cpp:17` — `GitHubCommitFeedHeaders`

## group `bc368ac0bbec64c9` — 3 TUs, 38 tokens, name(s): `IsJiraDurationSecondsFieldKey`, `IsTimeDurationField`, `IsBasicFieldId`
- `Source/Core/src/Tracker/JiraIssueMappingPure.cpp:171` — `IsJiraDurationSecondsFieldKey`
- `Source/Core/src/Tracker/TrackerFieldValueUtils.cpp:128` — `IsTimeDurationField`
- `Source/Core/src/Ui/SmatchetViewsDashboardUi_detail.h:22` — `IsBasicFieldId`

## group `affbaee4094b0322` — 3 TUs, 32 tokens, name(s): `IsSprintField`
- `Source/Core/src/EditMetaCacheService.cpp:31` — `IsSprintField`
- `Source/Core/src/FieldEditPipelineService.cpp:33` — `IsSprintField`
- `Source/Core/src/Tracker/TrackerFieldPayloadPure.cpp:366` — `IsSprintField`

## group `3c49cb47d700cdb6` — 3 TUs, 26 tokens, name(s): `ThCol`, `Rgba`, `ColFromRgba`
- `Source/Core/src/Ui/AnnotateAnalysisUi_Modals.cpp:139` — `ThCol`
- `Source/Core/src/Ui/CppSyntaxHighlight.cpp:13` — `Rgba`
- `Source/Core/src/Ui/P4ClPreview.cpp:35` — `ColFromRgba`
