# Agent self-improvement — applied (archive)

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../../../agent-layer/docs/self-improvement/AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Closed entries. Archive moves immediately on Status → applied. Sorted by original surface date, latest first.
> **Bounded head**: this file holds the current + previous month only; older months are
> rotated into flat `applied-YYYY-MM.md` siblings by
> [`rotate-applied-md.sh`](../../../agent-layer/agents/scripts/core/rotate-applied-md.sh)
> (run automatically by `archive-backlog-entry.sh`).
>
> **Deleted-runtime banner (2026-05-21)** — applied entries below that reference the agentic-flow C++ runtime (`AgenticHandoffController`, `AgenticTriageController`, `AgentProposalStore`, `ClaudeCodeLocalRunner`, `PrCommentWatcher`, `PrCheckRunWatcher`, `HarnessRunState`, `CoderabbitCommentClassifier`, `CiFailureClassifier`, `dispatch_source` enum, sentinel-file protocol, `agent/<proposalId>` worktrees, `coderabbit-react-loop` design, `agents/handoff-implementer.md`, `agents/pr-iterator.md`) refer to code that **no longer exists** in the tree. The runtime was removed by v1 PR1 of [`../../plans/shipped/github-tracker-backend.md`](../../plans/shipped/github-tracker-backend.md) (merge sha `b1d241bc`, 2026-05-21). The future [`../../plans/shipped/smatchet-merge-watcher.md`](../../plans/shipped/smatchet-merge-watcher.md) revives a subset of the underlying needs in a different (host-daemon) shape; concepts there are NOT identity-mapped to the deleted runtime. Entries here are preserved as historical record of what was tried.

<!-- Latest first. Append on archival. -->

# `postmortem-owed.sh`'s batched GraphQL fetch times out at the default window, and the failure reads as "last 20 merges clean"

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-10-04
- **Observed on**: PR #2286 (merged 2026-10-04T01:59:46Z past a red `Bucket-E UI tests (Mesa headless GL)`). PR #2280 (merged with `Plan-lock gate` red under `plan-lock-out-of-band`) was hidden by the same blind spot. Reproduced from worktree `gracious-kilby-73a454` at `c9479274d`, which is the `origin/develop` tip on 2026-10-04.
- **Status**: open

## What happened

At SessionStart, `postmortem-owed.sh --list` printed `postmortem-owed: no gate escapes owed a postmortem (last 20 merges clean).` and the `--nudge` hook stayed silent. Both were wrong: #2286 had merged past a red check earlier the same day.

Trigger 1+2 reads every merged PR's checks in a single batched call (`agents/scripts/core/postmortem-owed.sh:707-709`):

```bash
done < <(gh pr list --repo "$REPO" --base develop --state merged --limit "$FETCH_N" \
            --json number,labels,mergedAt,mergeCommit,statusCheckRollup --jq "$JQ_ROWS" 2>/dev/null \
            | tr -d '\r' || true)
```

`FETCH_N` defaults to `SCAN_N * 3` = 60 (`:612`). Each develop PR now carries about 52 check runs, and at `--limit 60` the GraphQL query returns `HTTP 504: 504 Gateway Timeout (https://api.github.com/graphql)`. That reproduced 3 times out of 3 on 2026-10-04. `--limit 20`, `30` and `40` all succeed.

`2>/dev/null` drops the error and `|| true` drops the exit code, so `ROWS` stays empty. With no rows, every trigger-1/2 check is skipped. The script then reaches the success branch at `:1092` and reports the 20-merge window as clean, even though it read 0 merges.

With the fetch inside the timeout, the same script on the same tree reports both owed escapes:

```text
$ POSTMORTEM_FETCH_N=30 bash agents/scripts/core/postmortem-owed.sh --list
postmortem owed: PR #2280 — red-check: Plan-lock gate; override: plan-lock-out-of-band
postmortem owed: PR #2286 — red-check: Bucket-E UI tests (Mesa headless GL)
```

## Why it matters

This script is the post-merge safety net for every merge path that skips the poller (see the companion entry, [`2026-10-04-native-auto-merge-merges-past-a-red-non-required-check.md`](infra/2026-10-04-native-auto-merge-merges-past-a-red-non-required-check.md)). When it fails, it says "clean", which is the one output that tells the reader nothing needs doing. Its header calls the default modes advisory and fail-open when `gh` is missing. But a missing `gh` prints a "skipped (advisory)" notice (`:105`, `:148`). A failed fetch prints a clean bill of health instead. The silence grows as rollups grow, so every new check context added to develop moves the detector further past the timeout. Nothing in the window was ever going to be caught.

## Concrete next action

1. **Fail loud, not clean.** Keep the fetch's exit status and stderr. If the fetch fails, or returns zero rows when the window should contain merges, print `postmortem-owed: merged-PR fetch failed (<first stderr line>) — window NOT scanned`. Print it on stderr in `--list` and `--nudge`, and exit non-zero in `--blocking`. Never fall through to the "merges clean" line. `:1092` should only be reachable when `${#ROWS[@]} -gt 0`.
2. **Fit the fetch under the timeout.** Page the window in chunks of at most 20, either with `gh api graphql --paginate` and `first: 20`, or with a loop of 20-PR `gh pr list --search "merged:<…"` calls. Alternatively, drop `statusCheckRollup` from the batch and read check runs per PR over REST (`commits/{sha}/check-runs`) only for the `SCAN_N` merges kept. Either way the cost scales with checks per PR, not the window size.
3. **Bats regression.** Run with a stub `gh` on `PATH` that exits 1 with `HTTP 504` for `pr list`. Assert the output does **not** contain `merges clean` and does contain `NOT scanned`. Also assert `--blocking` exits non-zero.

**Enumerator + replay**: the enumerator is the `gh pr list --state merged --limit $FETCH_N` row set (`ROWS`, `:704-709`). Replayed on 2026-10-04 at the default `FETCH_N=60`, the call 504s and `ROWS` is empty, so action 1 prints `window NOT scanned` instead of `clean`. With action 2's 20-row chunks, the first chunk contains #2286 with `Bucket-E UI tests (Mesa headless GL)` as its curated red check, and it is flagged. That is the `POSTMORTEM_FETCH_N=30` output above.

Triggered-follow-up: when=pr-count:base=develop;since=2026-10-04;n=10; action=re-run postmortem-owed.sh --list at the default window and confirm it either scans or says NOT scanned, never a false clean; baseline=HTTP 504 at --limit 60 (3/3) and a false "last 20 merges clean" hiding #2280 + #2286, 2026-10-04; fired=never
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — postmortem-owed.sh keeps the fetch's rc + first stderr line, retries a 5xx/timeout at a halved --limit (60→30→15), prints 'merged-PR fetch failed (…) — window NOT scanned' and exits 3 under --blocking; the clean line is reachable only when rows were read and reports the real count. Not done (optional): ≤20-per-page chunking, so a degraded retry scans fewer merges.
  Status: applied (2026-10-04)

- 2026-10-03 · deviation renewal (markers expiring 2026-12-31) · [debt] · P3 — the HTML and ADF Markdown engines each keep their own copy of the image-span and text-skip logic

Details:
md4c reports an image's alt text as ordinary text events between the image span's enter and leave
callbacks, so both engines collect it instead of emitting it. `MarkdownToHtml.cpp` and
`MarkdownToAdf.cpp` each repeat that logic:
- the `MD_SPAN_IMG` enter (push the src, raise the depth, clear the alt buffer);
- the leave tail (pop the src, clear the alt, lower the depth);
- the text-callback preamble (skip NUL chars, skip and log once on raw HTML under `MD_FLAG_NOHTML`,
  route text inside an image span into the alt buffer).

`dup_audit.py` reports these as clones, exempted by three markers in `MarkdownToHtml.cpp`. The markers
used to say that folding would couple two independent engines. It would not: both engines already
share `MarkdownConvert_Internal.h`, which holds their builder structs and `MdAttrToString`.

Concrete next action:
In `MarkdownConvert_Internal.h`:
- Add a `MdImageSpan` struct (depth, alt, src stack) and give both builders one in place of
  `imgSpanDepth` / `imgAltBuf` / `imgAltAccum` / `imgSrcStack`.
- Add inline helpers to enter an image span, pop its src, leave it, and absorb alt text, plus one for
  the NUL/raw-HTML skip that takes the engine name for its debug log.

Then move both engines onto the helpers and run the MarkdownConvert tests. Delete the three exemptions
(`revisit=2027-08-31`).

Applied 2026-10-03:
- Both builders now derive from `MdEngineState` (code-block depth, `MdImageSpan`, raw-HTML log flag).
- The engines call `EnterImageSpan` / `PopImageSrc` / `LeaveImageSpan`, `MdLinkHref` and
  `ConsumeNonEmittedMdText`, all in `MarkdownConvert_Internal.h`. Text events also build their string
  only after the line-break early returns.
- The three dated exemptions are gone. One `revisit=never` marker remains for the md4c callback
  skeleton (the end of the span switch and the start of the text callback), which both engines
  implement in the same order.
- A differential run of 16 image cases gives byte-identical HTML and ADF before and after. New
  `MarkdownToHtml: images` goldens pin the correct cases.
- The run also exposed three pre-existing alt-text bugs, filed as alexandrosk0/Smatchet#2284: empty
  `<em></em>` before an image, double-escaped entities, and nested images.

Status: applied
Last-reviewed: 2026-10-03

- 2026-10-03 · deviation renewal (markers expiring 2026-12-31) · [debt] · P3 — the AI provider clients each keep their own copy of the URL helpers and the chat messages build

Details:
`AnthropicClient.cpp`, `OllamaClient.cpp` and `OpenAiClient.cpp` each define the same anonymous-namespace
`JoinUrl` and a `ResolveBaseUrl` that differs only in its default base URL; OpenAI's also trims a
trailing `/v1`. Ollama and OpenAI also build the chat `messages` array the same way: an optional leading
`{role: system}` entry, then one entry per `History` item. `dup_audit.py` reports these as clones,
exempted by two markers in `OllamaClient.cpp` (`revisit=2027-05-31`). The same three files carry further
grandfathered, unmarked clones in their reachability probes and streaming loops.

The markers used to say that sharing these would couple independent provider adapters. It would not:
the three clients already share `AiErrorRedact.h` (`smatchet::ai::pure`) and `AiWireIntrospect.h`, and
the helpers are pure string and JSON work with no provider policy in them.

The per-provider wire-introspection wrappers (`<Provider>BuildChatBodyJson` / `<Provider>ResolveChatUrl`)
are not part of this. Each has to live in its own TU to reach that TU's anonymous-namespace builders, so
their markers are `revisit=never`.

Concrete next action:
- Add a pure header next to `AiErrorRedact.h` with `JoinUrl(base, path)`,
  `ResolveBaseUrlOr(cfg, defaultBase)` and `AppendChatMessages(json& messages, systemPrompt, history)`.
- Move the three clients onto it, keeping OpenAI's `/v1` trim at its call site.
- Cover the helpers with a bucket-A test, and keep the existing `ai.dump-request` wire tests green.
- Delete the two `OllamaClient.cpp` exemptions.

Resolution: applied 2026-10-06 (backlog-sweep-2026-10, PR #2296) — header-only Source/Core/include/AiWirePure.h (smatchet::ai::pure) holds JoinUrl (exactly one '/' between base and path), ResolveBaseUrlOr (trailing '/' trimmed; a configured malformed base never falls back to the default host), BuildChatMessages (system + history: Ollama, OpenAI) and BuildHistoryMessages (Anthropic); the three clients use it, OpenAI's /v1 trim stays at its call site, both OllamaClient duplication markers are gone. Request URLs are identical before/after across 55 base×endpoint cases except bases ending in '//', which now join correctly; wire bodies byte-identical. Covered by tests/Core/AiWirePure.test.cpp.
Status: applied (2026-10-06; was: open)
Last-reviewed: 2026-10-06

# A `SMATCHET_DEVIATION` marker wrapped across comment lines never expires

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-09-30
- **Observed on**: the 2026-10-01 deviation renewal (the markers renewed or resolved alongside this entry)
- **Status**: open

## What happened

`deviation-overdue` reads markers one line at a time (`DEV_RE='SMATCHET_DEVIATION\((.*)\)'` in
`agents/scripts/project/lint-rules.d/00-common.sh`). When a marker's reason wraps onto following
comment lines, the `revisit=` field sits on a continuation line the rule never parses, so the marker
never expires: the fail-open direction. Eleven wrapped markers dated 2026-09-30 / 2026-10-01 were
past due without any gate noticing; the renewal rewrote them as single-line markers. Wrapped markers
with later dates remain in `Source/` (for example the backend-client headers under
`Source/Core/include/Tracker/`, `OllamaClient.cpp`, `OpenAiClient.cpp`). `dup_audit.py` has the same
per-line reading, so a wrapped marker may also fail to suppress the clone it was written for; its
`_ineffective_dup_deviation` diagnostic already names that cause.

Most of these markers were wrapped by clang-format before `CommentPragmas: '^ *SMATCHET_DEVIATION'`
protected them.

## Concrete next action

1. Rewrite the remaining wrapped markers as single-line markers, with the explanation kept as plain
   comment prose above them.
2. Make the grammar fail closed: in `scan_file_rules`, a comment line that contains
   `SMATCHET_DEVIATION(` but no closing `)` emits `deviation-overdue` ("marker must be one line"),
   like an empty `revisit=`. Add a `--selftest` case and a bats case.
  Resolution: verified-in-tree 2026-10-04 (backlog-sweep-2026-10) — deviation-malformed (absolute-0) fails a SMATCHET_DEVIATION marker that is not one whole line with rule/reason/owner/revisit (lint-rules.d/10-line-rules.sh + 00-common.sh helpers, fixture tests/fixtures/lint_rules/deviation-malformed.cpp), so a wrapped marker can no longer dodge expiry.
  Status: applied (2026-10-04)

- 2026-09-29 · offline-first calibration (S13) · [debt] · P3 — the bulk-import hydration prefetch retries a failed fetch every frame

  Details: `Ui/SmatchetBulkTicketsUi.cpp` calls `app.PrefetchIssueTicketsForKeys(keysNeedingHydration)`
  on every frame the Bulk Import panel draws. `AppController::PrefetchIssueTicketsFrom` only
  de-duplicates keys that are still in flight. When a fetch fails, its keys are released and the next
  frame launches the same fetch again, with no backoff. While the tracker is reachable but failing
  (5xx, rate limit), that is a continuous stream of doomed requests for as long as the panel is open.
  Since S13 the prefetch is skipped while the tracker is known offline, so the offline case is covered.
  Found by the offline-first calibration review (`offline-network-read-ungated` hit on
  `AppController_TicketPrefetch.cpp`).

  Concrete next action: route the prefetch through a `KeyedLookupCache` keyed by
  (backend key, issue key), or keep a per-key retry-after next to `bulkImportPrefetchKeysInFlight_`.
  After a failure the key then waits out `kLookupRetryAfterSeconds`, and `OnConnectivityRecovered`
  clears the wait.
  Status: applied (offline-first follow-ups) — a failed prefetch holds its (backend key, issue key) entries back for `kLookupRetryAfterSeconds`; a connectivity recovery clears every backoff
  Last-reviewed: 2026-10-05

- 2026-09-24 · offline-first sweep · [debt] · P2 — the offline-queue counts run a SQLite SELECT on the UI thread every frame

  Details: `Ui/SmatchetUI.cpp` sets `d.cachedPendingFieldEditCount` from
  `app.GetPendingFieldEdits().size()` once per frame. `OfflineQueueService::GetPendingFieldEdits`
  loads every row through `LocalCacheManager::LoadPendingFieldEdits`, only to count them. The status
  bar (`Ui/SmatchetStatusBarUi.cpp`) does the same for creates: `app.GetPendingCreateCount()` loads
  every pending create through `LoadPendingCreates()` and returns `.size()`. Both reads are synchronous
  SQLite I/O on the render path (Quality Pillar 2). They are cheap while the queue is empty, but they
  grow with the queue, which is exactly the offline case the queue exists for. Found by the
  offline-first sweep (docs/plans/offline-first.md).

  Concrete next action: keep both counts in `OfflineQueueService` as atomics. Update them on the worker
  after every enqueue, replay, archive and restore (the same places that already touch the tables).
  The UI then reads the atomics and never queries SQLite in a frame.
  Status: applied (offline-first follow-ups) — OfflineQueueService and PendingActionQueueService publish an in-memory snapshot of their tables (`Sync/PublishedSnapshot.h`), rebuilt on a worker after every change; the status bar and the Offline Queue panel read it and never query SQLite in a frame
  Last-reviewed: 2026-10-05

- 2026-09-24 · offline-first sweep · [debt] · P2 — `AppController::UpdateTicket` writes the ticket to SQLite on the UI thread

  Details: `AppController::UpdateTicket` (`AppController_CatalogAndFieldEdit.cpp`) calls
  `Cache->SaveTicket(capturedKey, ticket)` inline, and its callers apply field-edit results on the
  UI thread (`ApplyFieldEditResult` through the grid pipeline's main-thread post-back). The write runs
  inside `RunWriteTxnWithBusyRetry`, whose busy-retry deadline can hold the frame, so a contended
  cache stalls the UI (Quality Pillar 2). Every queued offline edit also passes through this path when
  it is applied locally. Found by the offline-first sweep (docs/plans/offline-first.md).

  Concrete next action: post the `SaveTicket` call to a worker. Keep the key-and-generation latch
  that precedes it (issue #1081), and pass the latched key into the worker so the write still lands
  under the captured backend. The in-memory ticket update and `RefreshLocalData()` stay on the UI
  thread.
  Status: applied (offline-first follow-ups) — UpdateTicket patches the pane's row in memory on the UI thread and queues the SQLite write plus the grid re-read on a worker (`SerialTaskQueue.h`), in call order; only the latest queued save for a pane re-reads the grid
  Last-reviewed: 2026-10-05

- 2026-09-24 · offline-first sweep · [debt] · P2 — the project picker reads and parses the catalog-cache index every frame while its combo is open

  Details: `Ui/SmatchetProjectPicker.cpp` `DrawRecentSection` calls
  `FieldCatalogCache::ListCachedProjects()` on every frame the picker combo is open. That call reads
  the field-catalog cache from disk and parses its JSON, so an open picker does synchronous file I/O
  plus a parse on the render path (Quality Pillar 2). The cost grows with the number of cached
  projects. Found by the offline-first sweep (docs/plans/offline-first.md).

  Concrete next action: load the list once when the popup opens, on a worker, and keep it in the
  picker's state until the popup closes. Show the previous list, or an empty list, while that load
  runs.
  Status: applied (offline-first follow-ups) — the picker reads the recently used projects on a worker once per popup open (`SmatchetProjectPicker::StartRecentProjectsLoad`) and draws the rows it last read
  Last-reviewed: 2026-10-05

# An orphaned plan-lock has no direct write path for a session-scoped token — release it through a closing PR's `lock-slug:` line instead

- **Category**: process
- **Priority**: P3
- **Date**: 2026-09-10
- **Observed on**: Issue #2182 (`refs/locks/fa-fetch-raw-host`), alongside #2183/`pillar2-shutdown-flush` (PR #2201's session) and #2198's `parent-issue-hierarchy` (same session)
- **Status**: applied (this PR)

## What happened

`refs/locks/fa-fetch-raw-host` was claimed 2026-08-17 by branch
`fix/fa-fetch-raw-host` for a one-line fix to
`.github/actions/fetch-fontawesome/action.yml`. The fix landed the next day —
folded into unrelated PR #2119 rather than shipped from the locked branch —
so `fix/fa-fetch-raw-host` was deleted with no PR ever pointing back at the
`fa-fetch-raw-host` slug. `lock-staleness.yml` flagged it 23 days later as
Issue #2182.

## Why the obvious fixes don't apply

- `lock-cleanup.yml` only fires on `pull_request: closed` and only releases a
  ref named by a `lock-slug:` line in *that* PR's body. A lock whose branch
  never became a PR — or whose fix shipped under an unrelated PR, as here —
  is orphaned forever by that path alone.
- `lock-release.sh` pushes a ref delete straight to `refs/locks/*`, which
  needs git credentials with write access to that namespace. A
  session-scoped `GITHUB_TOKEN` / CCR credential does not have it —
  confirmed by a repeatable HTTP 403 chasing the same problem for
  `parent-issue-hierarchy` (#2198's lock), released only because the repo
  owner ran the push locally with `SMATCHET_ALLOW_MERGED_PR_PUSH=1`.

## The fix that generalizes

`lock-cleanup.yml` releases on **any** PR close, merged or abandoned — so a
PR that carries `lock-slug: <slug>` in its body releases the ref the moment
it closes, whether or not its own diff touches the locked write-set at all.
PR #2195 used exactly this to release `pillar2-shutdown-flush` for #2183;
this PR does the same for `fa-fetch-raw-host` — the diff is this note, and
the ref is released by the `lock-slug:` line in the PR body, not by the
note's content.

Any future orphaned-lock Issue where the branch is gone and no PR names the
slug can be closed the same way: open (and merge or close) a PR whose body
carries `lock-slug: <slug>`. No push access to `refs/locks/*` required.
  Resolution: verified-in-tree 2026-10-04 (backlog-sweep-2026-10) — lock-staleness-sweep.sh's stale-lock Issue body offers the lock-slug PR path for agents without ref-write access; the sweep also gains a workflow_dispatch release path in this sweep.
  Status: applied (2026-10-04)

# dup_audit delta gate false-flags a clone in UNCHANGED files (winnow boundary drift)

**Category**: tooling · **Priority**: P2 · **Status**: applied (fixed in the N12 slice-3 PR)

## What happened

N12 slice-3 (a Tracker-only refactor) tripped `dup_audit.py --diff origin/develop` with:

```
[dup] FAIL CodeColorView.cpp:97 <-> CppSyntaxLex.cpp:113 — 74-token copy-paste clone
```

Neither file was in the diff. Probe of the module confirmed both files were **byte-identical**
at the merge-base and at HEAD, yet `find_clones` surfaced an *extra* maximal-clone boundary
`(97,113)` ntok 74 at HEAD that it did not surface at base `(98,124)` ntok 71. Winnowing is
corpus-sensitive: adding tokens in an unrelated changed file shifts which shingles seed the
extension for a pre-existing clone between two unchanged files, so the drifted boundary gets a
base-absent `content_hash` and slips past the hash-only grandfathering.

## Impact

A blocking DRY gate can fail a PR over duplication in files the author never touched, keyed on the
happenstance of what other files the diff perturbs. Every "gate, don't trust" property assumes the
gate flags the author's own new duplication; this violated it.

## Fix (applied)

`new_clones_vs` now grandfathers a clone whose **every** occurrence is in a file unchanged (by
normalized token stream) vs the base — you cannot duplicate code *into* a file without changing it,
so an all-unchanged-files clone is definitionally pre-existing, whatever boundary the winnow drift
selected. Sound: it cannot mask a genuinely-new clone (a new clone has ≥1 occurrence in a changed
file). Pinned by a `--selftest` case that stubs `find_clones` to emit a drifted base-absent hash for
an unchanged pair and asserts it stays grandfathered (fails on the old code, passes on the new).

## Preventing recurrence

The selftest locks the invariant. A stronger follow-up (not done — low value): make winnowing seed
selection deterministic w.r.t. corpus so boundaries don't drift at all; deferred as the guard fully
closes the false-positive class.
  Resolution: verified-in-tree 2026-10-04 (backlog-sweep-2026-10) — dup_audit.py grandfathers clones between files whose normalized token streams are unchanged at base and head (the boundary-drift case), with selftest coverage; nothing left to do.
  Status: applied (2026-10-04)

- 2026-09-07 · code-review · [debt] · P2 — worker-side missing-parent fetch gates on the ACTIVE view; the grid projection gates per pane

  Details: `TicketSyncService::FetchMissingParentsIntoQueue` decides whether to skip the keyed parent fetch
  by reading `ConfigManager::FindActiveViewOrFirst(viewsCopy.Views, viewsCopy.ActiveViewId)`'s `HideParents`
  and the global `TrackerConfig::LoadParentIssues` — one decision for the whole sync. But the grid's row
  projection (`SmatchetActiveProjectGridTable.cpp`, `HierarchyOptionsForView(activeViewForGrid)`) reads
  the hierarchy options **per pane** (ADR-0018 multi-pane views). A background pane whose own view wants
  Story group / does not hide parents gets no parent top-up whenever the globally-active view happens to
  hide parents — the two switches read different scopes for the same feature.
  Found during adversarial review of docs/plans/shipped/parent-issue-hierarchy.md (PR #2198).

  Concrete next action: either (a) make the worker-side gate check every open pane's view (fetch parents
  if ANY pane wants them), or (b) document the active-view-only scope as an intentional simplification in
  docs/guides/parent-hierarchy.md and drop the cost only when literally no pane could use the result.
  Enumerator: `g_ui.gridPanes` (or the pane-view resolver used by `HierarchyOptionsForView` callers) is the
  per-pane view source of truth to reconcile against.
  Resolution: not a defect, closed 2026-10-04 (backlog-sweep-2026-10) — every sync is pane-scoped: pane-kicked syncs re-point views.ActiveViewId to the pane's own view before SyncPaneWithBackend, and the focused pane syncs against the active view, so FetchMissingParentsIntoQueue resolves each pane's own HideParents.
  Status: applied (2026-10-04; was: open)
  Last-reviewed: 2026-10-04

# Re-running a body-reading gate replays a frozen event payload — and the rollup dedup turns that into a fresh red

- **Category**: process
- **Priority**: P2
- **Date**: 2026-09-07
- **Observed on**: PR #2180 (`claude/agent-layer-a1-seam`) — two `gh run rerun` invocations against run `34139417600`, the second landing a terminal `FAILURE` at `17:03:05Z` where the head had previously carried only a stale `CANCELLED`
- **Status**: open

## What happened

`Intent section` was red on #2180's head. The reflex remedy — the one `merge-gates.md` documents —
is `gh run rerun <id>`. It was issued twice. Neither run could ever have passed, and the second one
made the gate strictly worse than before it ran.

The re-trigger that actually worked was a **PR body edit**, which produced run `34147012343`,
`SUCCESS` at `17:17:52Z`.

## The mechanics, source-read

1. `.github/workflows/doc-validation.yml:536` passes the body into the check as
   `PR_BODY: ${{ github.event.pull_request.body }}` — the body **as it stood in the event payload
   that started the run**, not the body as it stands now. `PR_HEAD_SHA` at line 537 comes from the
   same frozen payload.
2. `gh run rerun` re-executes a run against its **original** payload. So a rerun of a `synchronize`
   run re-reads the body from the moment of that push. If the body was wrong then — a missing
   `## Intent`, or a `head=` that no longer matches after a later push — it is still wrong on every
   rerun, forever. The condition being waited on is immutable, so the retry can never converge.
3. The workflow already knows this and says so, at `doc-validation.yml:73-79`: `edited` is in the
   trigger list precisely so "a PR that adds or fixes its `## Intent` section via a body edit
   re-runs the `Intent section` job and self-heals the stale-red — no wasted empty-commit push".
   The comment documents the cure. Nothing anywhere documents that the *reflex* is a poison.
4. The rerun is not merely futile — it is **actively regressive**. `merge-gates.sh:30-32`:
   > Rollup dedup: required CheckRuns with the same `.name` are deduped to the entry with the latest
   > `.startedAt` so stale FAILUREs from rerun jobs don't falsely block.
   Latest-`startedAt`-wins is the right rule for its stated purpose, but it cuts both ways: a rerun
   always carries the newest `startedAt`, so a rerun that fails **displaces whatever was there
   before**. On #2180 the pre-rerun state was a `CANCELLED` entry that blocked nothing; the post-rerun
   state was a terminal `FAILURE` that blocked everything. The remedy manufactured the block it was
   invoked to clear.

## The class, and how small it is

Exactly **one** PR-gating workflow in this repo reads a frozen payload field as its subject:

```
grep -rln 'github.event.pull_request.body\|github.event.pull_request.title' .github/workflows/
  .github/workflows/doc-validation.yml     # pull_request: [opened, synchronize, reopened, edited]
  .github/workflows/lock-cleanup.yml       # pull_request: [closed] — not a gate
```

That is the whole population today. The bound is what makes a gate cheap; the fact that the
population is one is also why the trap has never been written down.

## Why the existing docs point the wrong way

[`merge-gates.md`](../../../agent-layer/docs/agent-rules/merge-gates.md) is where an agent looks when a check is red,
and it prescribes `gh run rerun` twice without scoping:

- line 97, halt-code 8 (*Cancelled-while-pending*): "Rerun the named run(s) (`gh run rerun <id>` from
  the BLOCK output), then re-poll".
- line 245, in the recovery recipe: `gh run rerun <run-id>  # the run whose job is CANCELLED, not the
  newer one`, followed by "No push, no force, no PR-body re-pin — none of those touch the stale
  context."

Both are correct **for the concurrency-collapse case they were written for**, where the run never
executed and the payload is irrelevant. Neither says "unless the workflow's subject is the event
payload". The last quoted sentence is the exact inversion of the truth for `doc-validation` — there,
the PR-body re-pin is the *only* thing that touches it.

## Concrete next action

1. **Scope the rerun remedy where it is prescribed.** Add a one-line carve-out at
   `merge-gates.md:97` and `:245`: a rerun cannot fix a check whose input is the event payload
   (`github.event.pull_request.*`); for those, edit the PR body to fire the `edited` trigger.
   Enumerator: the `grep -rln` above — keep the carve-out keyed on that command, not on a hardcoded
   workflow name, so it stays true when the population grows.
2. **Make the workflow say it at the point of temptation.** The `doc-validation.yml:73-79` comment
   explains why `edited` exists; extend it with the inverse — that `gh run rerun` replays the stale
   body and can never pass. An agent reading the failing workflow should not have to find the
   remedy doc to learn the remedy is wrong here.
3. **Assert the property rather than the instance**, in `tests/bats/workflow_job_mask.bats` (the
   existing workflow-YAML-shape suite): every `pull_request`-triggered workflow that references
   `github.event.pull_request.body` or `.title` MUST list `edited` in its `types:`. Without `edited`
   such a gate has *no* re-trigger at all short of a new commit — the failure mode one config edit
   away from today's, and the one a name-based carve-out would not catch.
4. **Record the dedup's second edge.** The `merge-gates.sh:30-32` comment justifies latest-wins in
   one direction only ("stale FAILUREs … don't falsely block"). Note the other: a rerun's fresh
   FAILURE displaces an older SUCCESS or a harmless CANCELLED. The rule is still right; a reader
   deciding whether to rerun needs to know it is not free.

Triggered-follow-up: when=pr-count:base=develop;since=2026-09-07;n=20; action=re-check whether the rerun carve-out landed in merge-gates.md, whether any new workflow reads `github.event.pull_request.body` without an `edited` trigger, and whether another rerun-induced FAILURE displaced a green; baseline=1 rerun-manufactured FAILURE on PR #2180 and 1 payload-reading gate repo-wide, 2026-09-07; fired=2026-10-05 (carve-out landed in merge-gates.md halt-code-8 row + 405 recipe; tests/bats/workflow_event_payload.bats finds no payload-reading workflow without `edited`; rerun-displaced-green incidence not checked — needs run history)
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — (1) carve-out in merge-gates.md at the halt-code-8 row and the 405 recipe; (2) the inverse warning in doc-validation.yml's edited comment; (3) tests/bats/workflow_event_payload.bats asserts every pull_request workflow reading the body/title lists edited (closed-only exempt); (4) the dedup's second edge is documented in merge-gates.sh.
  Status: applied (2026-10-04)

- 2026-09-07 · code-review · [tooling] · P2 — `SmatchetLocalization::T(key, fallback)` returns the call-site fallback for en-US, not the table's English column — editing one without the other is a silent no-op

  Details: `TranslateEntryLocked` (`Source/Core/src/SmatchetLocalization.cpp`) returns
  `fallback ? fallback : entry.English` for the active en-US language — the `kEntries` table's English
  column is used only as an *overrides lookup key* (`overrides.find(entry.English)`), never rendered
  directly. A diff that edits only `kEntries`'s English string (e.g. renaming a label) changes nothing
  user-visible in English; the call-site `T("key", "...")` literal is what actually renders. This bit twice
  in one session on the same PR (#2198): the author renamed "Story group" -> "Parent group" by editing only
  the table row, and a review pass initially read the table as the source of truth too before tracing the
  function. The mismatch also silently broke the feature's only bucket-E UI-test coverage — the test's item
  ref matched the (unwritten) new label, so `ClickSortByCheckbox` never resolved it.
  Found during adversarial review of docs/plans/shipped/parent-issue-hierarchy.md.

  Concrete next action: document the fallback-wins-for-en-US contract in `docs/agent-rules/cpp-rules.md`
  § Conventions (near any existing localization guidance), and add a cheap delta-gated check: for every
  changed `kEntries` row `{"<key>", "<english>", ...}` in `SmatchetLocalization.cpp`, grep the repo for
  `T("<key>"` call sites and flag if none of their literal fallback strings match the new `<english>` value.
  Enumerator: the `kEntries` array in `Source/Core/src/SmatchetLocalization.cpp`; rule id sketch
  `localization-english-fallback-drift`.
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — docs/agent-rules/cpp-rules.md § Localization states the fallback-wins rule; agents/scripts/project/test-localization-drift.sh (WARN-first; --diff/--all/--strict/--selftest; C-escape aware) compares call-site fallbacks with the table's English column. Whole-tree: 380 call sites, 3 drifts, all real (toast.reverted_layout, cmdpalette.destructive_hint, whisper.preferences.testConnection.failure) — product text, so filed as GitHub Issue #2305 rather than changed here (visual-validation rule).
  Status: applied (2026-10-04; was: open)
  Last-reviewed: 2026-10-04
