# Plan — C++ code audit remediation (CPP_CODE_AUDIT.md)

> **Slug**: `cpp-code-audit-remediation`
>
> **Status**: `active`
>
> **Source**: [`CPP_CODE_AUDIT.md`](../../../CPP_CODE_AUDIT.md) — 33 findings (High: 1, Medium: 8, Low: 24), complementary to the already-remediated `SECURITY_AUDIT.md` sweep ([`cpp-security-hardening.md`](../cpp-security-hardening.md) — see that plan for the prior JSON-DoS/MCP-auth campaign). Audit PR: alexandrosk0/Smatchet#1586.

## Context

`CPP_CODE_AUDIT.md` landed as a docs-only PR (#1586) with no source changes — it's a 12-partition, ten-defect-angle sweep of the ~140K-LOC first-party C++ tree. Four findings were hand-verified against source (data-loss truncation, GitHub credential mis-routing, duration-sort infinite loop, AI cancel-atom rebind); the rest are auditor-reported with file:line citations. This plan tracks remediation across multiple slices/PRs since the finding set is too large for one PR under the per-PR file ceiling. **Backlog check (this pass):** grepped `backlog/BACKLOG_CODE_REVIEW.md`, `docs/self-improvement/categories/*`, and GitHub Issues for every Slice 1 finding's symbol/mechanism (`TicketFieldEditor_Modal` truncation, `RouteTrackerEnvCredentials`, `ParseDurationToSecondsForSort`) — no pre-existing entries found; these are new.

**Intended outcome**: every finding in `CPP_CODE_AUDIT.md` is either fixed (this plan's slices) or explicitly deferred with a one-line reason recorded in § Deviations.

## Approach

Remediate in slices ordered by the audit's own § Recommended remediation order (highest user-impact / hand-verified first), each shippable as its own PR:

- **Slice 1 (this PR)** — the top 3 hand-verified findings (#1 data-loss, #2 GitHub credential mis-routing, #3 duration-sort infinite loop) plus the two mechanical ParseBounded sweeps (#8 Jira field-catalog, #9 Plane) that are the same well-proven mechanical fix as the prior `cpp-security-hardening` campaign. Also folds in #19 (integer-overflow in the same `ParseDurationToSecondsForSort` function touched for #3 — same file, same function, trivial saturating-add addition, no reason to defer a second pass over identical lines).
- **Slice 2 (this PR)** — #4 (AI cancel-atom rebind), #5 (TextMerge O(n·m) OOM), #6 (offline-replay latch leak) — the AI/offline-replay reliability cluster the audit groups together. Batched onto the same branch/PR as Slice 1 per `AGENTS.md` § Autonomous ship-loop default's PR-batching rule ("one PR per logical feature, not per slice") since the PR was still open/unmerged when Slice 2 started.
- **Slice 3 (this PR)** — #7 (locale-override format-string on the `SmatchetLocalizedImGui` `Text*` sinks) — same specifier-validation mechanism the prior audit's finding #1 fix added to `SmatchetLocalization::Format`, applied to the `TranslateSource` wrapper path. Batched onto the same PR as Slices 1–2 (same rationale — PR still open).
- **Slice 4 (follow-up; Groups A+B on this PR, Groups C–H not yet started)** — remaining Low findings #10–33, batched by subsystem (Group A — ParseBounded stragglers #10–13 — done; Group B — SSRF/security #14–16, plus #22 folded in with #14 — done; Group C — integer handling #17–18 — #19 already folds into Slice 1; Group D — memory safety #20–21, #22 already folds into Group B; Group E — concurrency #23–27; Group F — resource management #28–31; Group G — error handling #32; Group H — logic cluster #33's remaining 4 sub-items, one already folded into Group A).

## Files to modify

### Slice 1 (this PR)

1. `Source/Core/src/TicketFieldEditor_Modal.cpp` — **#1**: added `SeedTruncated` + `BufferSeedShown` state, a `SeedLongTextBuffer` helper used at both the sync and async-worker seed sites, `CommitLongTextEdit` now diffs against `BufferSeedShown` (what was actually loaded) instead of the untruncated seed, plus a red "too large to edit" banner in `DrawLongTextBanners`.
2. `Source/Core/src/Config/ConfigManager.cpp` — **#2**: `RouteTrackerEnvCredentials` now compares `trackerTypeLower == "github"` (was raw `cfg.TrackerType == "github"` against the canonical PascalCase `"GitHub"`) for both the token and base-URL routing arms.
3. `Source/Core/src/TicketGridModel.cpp` — **#3**: the unrecognized-unit-char branch in `ParseDurationToSecondsForSort` now advances `pos` before continuing (was an infinite loop). **#19** (same function): added a `saturatingAccumulate` helper so the unit multiply/add can't overflow `long long` (UB) on a hostile duration string.
4. `Source/Core/src/Tracker/TrackerFieldCatalog.cpp` — **#8**: all 10 bare `nlohmann::json::parse` sites (components/priority/issuetype/status/createmeta/project/boards/sprint/field-list, per-project components) routed through `smatchet::json_safe::ParseBounded`.
5. `Source/Core/src/Tracker/PlaneIssueSearch.cpp`, `PlaneIssueMutation.cpp`, `PlaneFieldCatalog.cpp` — **#9**: the 9 cited bare-parse sites (states, error-detail extraction, work-items page, list-projects, PATCH/POST error-detail ×2, create-issue response, comments, custom-field catalog ×2) routed through `ParseBounded`. `PlaneIssueSearch.cpp:96` (`ExtractProjectFromPlaneQuery`, a locally-authored structured-query blob, not network ingress) is intentionally **not** touched — the audit doesn't cite it.

### Slice 2 (this PR)

6. `Source/Core/include/AiAssistantController.h`, `Source/Core/src/AiAssistantController.cpp` — **#4**: `Request` now carries its own `Cancel` token (created in `Submit()`); `WorkerLoop` publishes `currentCancel_ = req.Cancel` under `queueMutex_` at pop time (was set by `Submit()`, letting a later Submit steal the in-flight field from an earlier streaming turn); `Cancel()` and the destructor now flip both the in-flight token and every still-`pending_` request's own token (necessary, not just thorough — a just-`Submit()`-ted turn has no representation in `currentCancel_` until `WorkerLoop` pops it).
7. `Source/Core/src/TextMerge.cpp` — **#5**: `ExceedsLcsCellBudget`/`ShouldBailOnLcsBudget` (4M-cell budget, overflow-safe `a > budget/b` check) — `ThreeWayMerge` now returns a whole-document conflict (`WholeDocumentConflictText`) instead of running `DiffHunks`/`ComputeLcs` when either base/mine or base/theirs would exceed the budget.
8. `Source/Core/src/Tracker/IssueCreatePipeline.cpp` — **#6** (half 1): `Run`'s previously-bare `cache->SaveTicket` wrapped in try/catch, mirroring the sibling `RunUpdateExisting`'s existing pattern (LOG_WARN + swallow, not rethrow — see the inline comment on why rethrow here would reintroduce the exact "duplicate issue" bug this finding describes, via a different path).
9. `Source/Core/src/Sync/OfflineQueueService.cpp` — **#6** (half 2): new local `ScopeExit` RAII helper (anonymous namespace) so `TickOfflineCreates`'s background lambda always resets `offlineReplayInFlight_`/`nextOfflineReplayAt_` on every exit path (normal return or an exception unwinding out of `ReplayOneCreate`), not just the prior normal-completion tail.
10. `scripts/dev/test-ui-jira-deterministic-backend.sh` + `Source/Standalone/CliCommandRunner.cpp` — unrelated CI break found while validating this slice, not a `CPP_CODE_AUDIT.md` finding: `ui_test.run --outLog` requires a relative path (confined under `<userData>/ui-tests/`, from the already-shipped `SECURITY_AUDIT.md` sweep, PR #1566), but (a) this driver script passed an absolute path, and (b) `CliCommandRunner.cpp`'s `SpawnAndRun` had a pre-#1566 step that re-absolutized any relative `outPath`/`outLog` arg before forwarding it to the `--spawn`ed child — silently reintroducing the absolute-path rejection for every `--spawn` caller. Fixed both: the script now passes a bare filename and copies the confined result back to the caller-requested path; `CliCommandRunner.cpp` no longer re-absolutizes `outPath`/`outLog` (every current handler confines them under `<userData>`, so CWD-relative resolution is obsolete and was actively wrong post-#1566).

### Slice 3 (this PR)

11. `Source/Core/include/SmatchetLocalization.h`, `Source/Core/src/SmatchetLocalization.cpp` — **#7**: new `TranslateSourceAsFormat(englishSource)` — calls `TranslateSource`, then reuses the existing `FormatSpecifiersMatch`/`ConversionSpecifiers` guard (already used by `Format()` for keyed translations) to fall back to `englishSource` when the override's conversion-specifier sequence doesn't match; a pointer-equality fast path skips the specifier scan when `TranslateSource` returned its input unchanged (no override present).
12. `Source/Core/include/SmatchetLocalizedImGui.h` — **#7**: the 7 audited `Text*`/`SetTooltip`/`SetItemTooltip` wrappers plus `SliderInt`'s `format` param switched from `TranslateSource(fmt)` to `TranslateSourceAsFormat(fmt)`. `InputTextWithHint`'s `hint` and `TextUnformatted`'s `text` deliberately left on plain `TranslateSource` — neither reaches a printf-family sink (`hint` is ImGui placeholder text; `TextUnformatted` takes no format arg).

### Slice 4 (follow-up; Groups A+B landed on this PR, Groups C–H not yet started)

**Group A — ParseBounded stragglers (#10–13):**

13. `Source/Core/src/Diagnostics/BugReportService.cpp` — **#10**: all 8 cited bare `nlohmann::json::parse` sites (GitHub-API response bodies) routed through `ParseBounded`. Also closed the "Also latent" `EnsureAssetsBranch` sub-item folded into **#33**'s cluster: `["object"].value(...)` on a non-object now guarded with `contains("object") && is_object()`, mirroring the sibling guarded sites.
14. `Source/Core/src/Config/ConfigManager_Views.cpp` (`LoadPersistentViewsFromDisk`), `Source/Core/src/Config/ConfigManager_Panes.cpp` (`LoadPanesFromDisk`) — **#11**: both now read via the existing `ConfigManager::LoadJsonFile` (which already does a bounded parse with a 64 MiB cap plus its own `GetIoMutexRef()`/`ScopedFileLock` locking) instead of a manual `file >> j`, removing the now-redundant caller-side lock/lock-guard (kept would deadlock — the mutex is non-recursive).
15. `Source/Core/src/Ui/SmatchetToolbarUi.cpp`, `Source/Core/src/Ui/SmatchetUI.cpp` — **#12**: the two cited `ArgsJson`/`argsJson` bare-parse sites routed through `ParseBounded`.
16. `Source/Core/src/Persistence/BackendAuditTrail.cpp` — **#13**: `ReadRecentEvents`'s per-line parse routed through `ParseBounded`, skipping (not aborting on) a corrupt line — the inner try/catch is now redundant and removed; the function's outer try/catch (other exception sources) is unchanged.

**Group B — SSRF/security (#14–16) + #22 (folded in, same file as #14):**

17. `Source/Core/src/Ui/SmatchetFieldIconRender.cpp` — **#14**: new `ExtractUrlHost`/`IconUrlHostAllowed` helpers; `ResolveJiraIconUrlReference` now rejects an absolute/scheme-relative `iconUrl` whose host doesn't match the configured `jiraDomain`'s host (per the audit's own prescribed fix — "reject hosts ≠ `cfg.Domain`"), closing the SSRF where a malicious tracker points `iconUrl` at an internal/loopback endpoint. **#22** (same file): `DrawImagePathOrUrl` now guards `icon.Texture == nullptr` once, before branching on explicit-vs-zero size, so both branches return the same (false) result for a failed/zero-dim decode instead of only the zero-size branch (`DrawLoadedIconSized`) silently no-op'ing while the explicit-size branch null-derefed.
18. `Source/Plugins/Whisper/WhisperPlugin.cpp` — **#15**: `whisper.transcribe-once` (mic-capture path only — `--file` is unaffected) and `whisper.simulate-press` now reject `CommandSource::Mcp` with a `ValidationError`, closing the silent-mic-trigger-via-loopback-MCP vector; verified via grep that CI's own usage of both commands is CLI/scenario-only (no MCP caller depends on the removed capability).
19. `Source/Core/src/AiEndpointSanitize.cpp` — **#16**: new `IsIpv6LinkLocalHextet` (extracted to stay under the branch-count cap) range-checks the first hextet against `[0xfe80, 0xfebf]` instead of string-matching only the `"fe80:"` prefix, closing the fe80::/10 upper-half gap (`fe90::`–`febf::`).

**Groups C–H — not yet started (integer handling #17–18, memory safety #20–21, concurrency #23–27, resource management #28–31, error handling #32, logic cluster #33's remaining 4 sub-items).** See `CPP_CODE_AUDIT.md` for file:line citations — not re-listed here to avoid drift; this plan's § Deviations records the per-slice PR link once shipped.

## Existing utilities reused

- `smatchet::json_safe::ParseBounded(text, errOut)` — `Source/Core/include/Json/BoundedJsonParse.h:122`. Same shared bounded-ingress parser the prior `cpp-security-hardening` campaign wired everywhere else; Slice 1 extends its reach to the two TUs the original sweep's dedup gate skipped.
- The `(std::min)` / parenthesized-`numeric_limits` idiom already used elsewhere in this file family for the Windows `min`/`max` macro guard.

## Extraction sizing

N/A — no file crosses a split threshold in this slice.

## UX Pillar callouts

- **Pillar 1 (perf)**: negligible. `ParseBounded` drives the same SAX-based DOM builder as `nlohmann::json::parse`; the `SeedLongTextBuffer` helper is a single extra `memcpy`-sized copy already happening. `ParseDurationToSecondsForSort` gains one branch + one lambda call per unit token — sort comparator, not a steady-state per-frame path.
- **Pillar 2 (no UI-thread block > 100 ms)**: no new sync I/O — the long-text seed/commit and duration-sort fixes are pure in-memory logic; the Plane/Jira catalog parses already ran on the same thread as the unbounded parse they replace.
- **Pillar 3 (never crash)**: this slice is squarely Pillar 3 — #3 fixes an actual infinite loop (permanent UI freeze, worse than a crash), #8/#9 convert two more uncatchable depth-bomb stack-overflow sites into clean rejections, #1 fixes silent data corruption (not a crash, but the audit's highest-severity finding).
- **Pillar 4 (accessibility)**: no impact.

## Perf-review-system gates (diff touches `Source/Core/`)

1. **PR-fast CI** — no scenario directly exercises the long-text modal or the Jira/Plane catalog fetch in the curated PR-fast set; this is logic-correctness, not a perf-sensitive path. N/A.
2. **Pillar 2 static scanner** — no new sync I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — untouched.
4. **Visible-cue bucket-E harness** — no new >100 ms sync-stall path (the fixes *remove* an infinite-loop stall).
5. **Marker inventory** — no new `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: local build/CI is unavailable in this environment (`FetchContent` 403s through the proxy — same constraint recorded in `cpp-security-hardening.md` § Implementation log); CI is the build/test gate for this PR.

## Risks / non-goals

- **Risk — `TicketFieldEditor_Modal.cpp` fix changes Save semantics for over-64KB docs.** Mitigation: the fix is diff-baseline-only (compare against what was actually loaded, not the untruncated seed) plus a visible banner — it does not attempt full dynamic buffer growth (which would risk a dangling-pointer bug in the dictation-router registration lifecycle across the async reseed path); an unmodified over-limit doc is now a no-op Save instead of silent corruption, and an edited one is capped + visibly flagged rather than silently truncated.
- **Risk — Plane/Jira `ParseBounded` conversion changes error-detail extraction behavior for malformed error bodies.** Mitigation: mechanical 1:1 replacement of `is_discarded()`/`try-catch` control flow with `parseErr.empty()` checks — same branches, same fallback-to-raw-text behavior, just non-throwing instead of exception-based.
- **Risk — `Cancel()` now also cancels queued-but-not-yet-started turns, not just the in-flight one.** The only caller that can have more than one turn queued at once is the ungated `AppController::PromptAi` (Lua/automation) path; a panel Cancel() click while a Lua-submitted turn is queued behind the visible one now also cancels that queued turn. Mitigation/acceptance: there is no per-turn Cancel UI to selectively avoid this, and `PromptAi` turns are already silently dropped by the UI's stale-turn gate regardless (`CPP_CODE_AUDIT.md` #33, out of scope here) — cancelling one changes what discards it, not whether the user ever saw it. The alternative (Cancel() touching only `currentCancel_`) reintroduces a race: a turn `Submit()`-ted but not yet popped by `WorkerLoop` has no representation in `currentCancel_` yet, so it would escape a Cancel() that lands in that window.
- **Risk — `CliCommandRunner.cpp`'s `NormalizeOutPath` removal changes `--spawn` behavior for `outPath`/`outLog` beyond the one driver script this slice fixes.** Verified safe: grepped every command handler that reads `outPath`/`outLog` (`ui_test.run`, `scenario.run`, `perf.dump`) — all three already confine the value under a `<userData>` subdir and reject an absolute path outright (`SECURITY_AUDIT.md` sweep, PR #1566), so the removed CWD-absolutization step was already actively wrong for every current caller, not just this one.
- **Non-goal**: this PR does not touch #7 or the Low findings — those are Slice 3–4, tracked here and shipped as follow-up PRs.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no local build available this session (see § Perf-review-system gates); CI's existing ctest suite covers `TicketGridModel`/`ConfigManager`/`Tracker` if fixtures exist, otherwise this is a residue item (see below).
- **Build gate**: CI-only this session — `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` deferred to CI.
- **Doc validation**: `scripts/dev/test-docs.sh` — run locally where possible.
- **Plan stress-test — `grill-with-docs`**: not run interactively this session (autonomous single-turn task, no user available mid-turn); flagging as manual residue below rather than skipping silently.
- **Manual residue**: (1) no local ctest run — CI is the authoritative gate this session, consistent with the `cpp-security-hardening` precedent; (2) `grill-with-docs` stress-test not run — both logged here per `AGENTS.md` § Process rules "no silent residue."

## Out of scope (flagged, not designed)

- Slice 4 Groups C–H (findings #17–18, #20–21, #23–33) — not yet started, tracked in § Approach above.
- `agents/scripts/project/*` lint-rule changes — this PR doesn't graduate any WARN-first gate (unlike `cpp-security-hardening` Slice 6); N/A.

## Implementation log

- Slice 1 (this PR, branch `claude/pr-1586-findings-697xkc`): #1, #2, #3, #8, #9, #19 — see § Files to modify for the per-file summary.
- Post-implementation `/code-review` (4-angle, 1-vote verify) surfaced one additional real bug, folded into the same commit: `ConfigManager.cpp`'s `RouteTrackerEnvCredentials` had the identical raw-casing bug for Plane (`cfg.TrackerType == "Plane"`) that #2 fixed for GitHub — `DefaultTrackerBackendFactory::Create` case-insensitively matches all four backends (a hand-edited lowercase `"plane"` in `smatchet_config.json` still boots `PlaneClient`, per `SmatchetPreferencesUi.cpp`'s own "the load path doesn't canonicalize" comment), so the original audit's "only GitHub is affected" call was incomplete. Both Plane arms now route off `trackerTypeLower` like GitHub/Linear.
- Slice 2 (same PR/branch): #4, #5, #6 — see § Files to modify for the per-file summary.
- Post-implementation `/code-review` on Slice 2 (3 parallel finder angles: line-by-line, removed-behavior + cross-file, conventions) surfaced 4 candidates:
  - **Real, fixed inline**: the `#4` driver-script fix as first written didn't survive the `--spawn` code path — `CliCommandRunner.cpp`'s `SpawnAndRun` re-absolutized any relative `outPath`/`outLog` before forwarding to the spawned child, defeating the confinement fix and reproducing the exact CI failure the fix was meant to close. Root-caused + fixed by removing the obsolete re-absolutization (see § Files to modify item 10).
  - **Doc-drift, fixed inline**: `Cancel()`'s header doc-comment still described only the pre-fix in-flight-only behavior; updated to document the new "also flips pending tokens" behavior and why it's necessary (not just thorough).
  - **Policy-tension, addressed with a comment**: the new `IssueCreatePipeline.cpp` catch uses LOG_WARN + swallow, which reads as a literal violation of `exception-handling-policy.md`'s Cache/DB tier (LOG_ERROR + rethrow-unless-destructor) — but it deliberately mirrors the pre-existing sibling `RunUpdateExisting` pattern, and rethrowing here would unwind past the already-set `result.Ok`/`result.IssueKey`, reintroducing this same finding's duplicate-issue bug via a different path. Added an inline comment explaining the deliberate divergence rather than changing the behavior.
  - **Accepted as designed, documented**: `Cancel()`'s pending-token flip can cancel a Lua-`PromptAi`-queued turn the user didn't intend to stop — recorded in § Risks / non-goals rather than changed, since the alternative reintroduces a real race (see that entry for the full reasoning).
- Slice 3 (same PR/branch): #7 — see § Files to modify for the per-file summary.
- Post-implementation `/code-review` on Slice 3 (single targeted agent, given the small/contained diff reusing an already-proven mechanism) — zero findings; the fix was verified correct and complete, including confirming no other `TranslateSource` caller outside `SmatchetLocalizedImGui.h` feeds a printf sink and the pointer-equality fast path is sound.
- Slice 4 Group A+B (same PR/branch): #10, #11, #12, #13, #14, #15, #16, #22 (folded in with #14, same file) — see § Files to modify for the per-file summary. The `EnsureAssetsBranch` "Also latent" sub-item of **#33**'s cluster was closed opportunistically alongside #10 (same file, same pre-existing guard pattern already used at sibling call sites).
- Post-implementation `/code-review` on Slice 4 Group A+B (2 parallel finder angles: line-by-line + parse-conversion correctness, cross-file + conventions) — the line-by-line agent surfaced zero findings (all `ParseBounded` conversions verified 1:1 control-flow-preserving against the sites they replaced, including confirming `LoadJsonFile`'s internal locking makes the caller-side lock removal in #11 non-deadlocking). The cross-file agent surfaced 3 candidates:
  - **Real, fixed inline**: `DrawImagePathOrUrl`'s two size branches disagreed on the return value for the identical `icon.Texture == nullptr` condition (the explicit-size branch's new #22 guard returned `false`; the zero-size branch's pre-existing `DrawLoadedIconSized` guard just no-op'ed, leaving `DrawImagePathOrUrl` to return `true`) — a Lua caller branching on the bool would incorrectly skip its fallback only in the zero-size case. Fixed by hoisting a single `icon.Texture == nullptr` guard above the branch, so both paths agree.
  - **Test-coverage gap, fixed inline**: no regression test exercised the fe80::/10 upper-half range #16 actually fixes (`fe90::`/`fea0::`/`febf::` — only the already-covered `fe80::1` prefix case existed). Added `tests/Core/AiEndpointSanitize.test.cpp` cases for the three upper-half boundaries plus a `fec0::` (one hextet past the fixed /10) negative case guarding against the range check being accidentally widened.
  - **Accepted as designed, no change**: the #14 host-pin rejects any absolute `iconUrl` whose host differs from `cfg.Domain`, which changes behavior for a hypothetical Jira deployment serving icons from a separate CDN/assets host — this is exactly the audit's own prescribed fix ("reject hosts ≠ `cfg.Domain`"), and the fallback chain (`LoadPriorityIconWithFallbacks`) already degrades gracefully to the bundled offline icon rather than failing loudly.

## Deviations from plan

- #19 folded into Slice 1 opportunistically (same function as #3) rather than deferred to Slice 4 with the other Low integer-handling findings — avoids a second pass over the exact same lines.
- The Plane credential-routing casing bug (see § Implementation log) was not in the original `CPP_CODE_AUDIT.md` #2 write-up; fixed in this PR anyway since it's the same mechanism, same file, same function.
- `TrackerFieldCatalog.cpp`'s `#8` conversion changed `EnrichSprintFields`/`DiscoverSprintBoardIds` from all-or-nothing (a parse failure anywhere in Jira Agile board/sprint pagination threw, discarding every result collected in that call) to best-effort partial-result (a parse failure now only stops the current page loop, keeping whatever board IDs/sprint options were already collected) — because `ParseBounded` doesn't throw. This is a deliberate accepted side effect, not a regression: it makes JSON-parse failures behave the same as the pre-existing HTTP-status-failure handling in the same functions (which already `break`s/`continue`s on a bad page rather than aborting the whole enrichment), and using partial catalog data beats discarding it entirely on one bad page.
- The `#3` infinite-loop fix (advance `pos` past an unrecognized unit char, per the audit's own prescribed fix) makes a decimal-formatted duration like `"2.5h"` parse as two tokens (`2` plain seconds + `5h`) instead of looping forever — a wrong sort key instead of a hang. This is strictly better than the pre-fix behavior (infinite loop) and matches the audit's literal fix ask; teaching the parser real decimal-hour semantics is a separate, out-of-scope correctness improvement not requested by finding #3.

## Verification (actual)

- Build/ctest — not run, local build unavailable (`FetchContent` 403); CI is the gate for this PR. See § Manual residue.
- `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` — PASS (strict-zone, function-size/branchy, duplication, include-cycle, AppController fan-in, agent-size gates all clean).
- `bash scripts/dev/pre-ship.sh origin/develop` (format + delta lint + doc-validation mirror) — PASS.
- `/code-review --diff origin/develop medium` (4 parallel finder angles: line-by-line, removed-behavior, cross-file tracer, conventions) — surfaced 5 candidates; 1 real bug fixed (Plane credential casing, folded into this PR — see § Implementation log), 2 marker-placement nits fixed (moved `SMATCHET_DEVIATION` comments to sit directly above their clone span per `cpp-rules.md`'s documented grammar), 2 accepted-as-designed behavior notes recorded in § Deviations (sprint-enrichment partial results, decimal-duration sort key) — no further code changes needed.
- Slice 2: same gate sequence re-run after each fix — `test-lint-rules.sh --diff origin/develop` PASS, `pre-ship.sh origin/develop` PASS (ack recorded), `agents/scripts/core/test-shell-lint.sh --diff origin/develop` PASS (249/249) for the driver-script edit. A minimal bash repro (`reader < <(producer)` vs `producer | reader`) was used earlier in this session to confirm the unrelated `coverage-delta-gate.sh` SIGPIPE mechanism — not re-run here, different file.
- Slice 2 `/code-review` (3 parallel finder angles) — see § Implementation log for the 4 findings and how each was resolved.
- Slice 3: `test-lint-rules.sh --diff origin/develop` PASS, `pre-ship.sh origin/develop` PASS (ack recorded), single-agent `/code-review` — zero findings (see § Implementation log).
- Slice 4 Group A+B: `agents/scripts/project/test-lint-rules.sh --diff origin/develop` PASS (strict-zone, function-size/branchy, duplication, include-cycle, AppController fan-in gates clean — the two `SMATCHET_DEVIATION(rule=duplication; ...)` markers needed for the `SmatchetUI.cpp` include-block clone surfaced by adding one include line are documented inline in that file). `scripts/dev/pre-ship.sh --ack-review origin/develop` PASS (ack recorded after applying the 2 real fixes from `/code-review` — see § Implementation log).

## Out of scope — deferral residue-sweep

Grepped `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray references to findings #4–33 or this plan's slug — none found (this is a new plan with no prior deferred-symbol footprint to clear).
