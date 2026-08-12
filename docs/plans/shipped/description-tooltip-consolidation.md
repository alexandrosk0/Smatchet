# Plan — description-field tooltip consolidation
<!-- plan-date: 2026-05-23 -->

> **Slug**: `description-tooltip-consolidation`

## Context

During a tooltip bug investigation an agent spent ~1.5 h chasing path #2 (`renderPlainText` → `RenderClippedFieldText`) while the live code path was path #1 (`RenderTextEditor`'s inline `BeginTooltip`). The root divergence: `TicketFieldEditor.cpp` contains two independent tooltip generators for description-type fields, and a third `BeginTooltip`+`MarkdownPreviewRender::Render` call in `SmatchetOfflineQueueUi.cpp` omits `opts.wrapWidth`. All three are latent bugs that will confuse future debuggers and can produce invisible/near-zero-width rendered text inside fresh tooltip windows.

After this lands: there is exactly one place in the codebase that decides "does this field get a markdown tooltip?" and exactly one code path that opens the tooltip window for description fields, and every `BeginTooltip`+`MarkdownPreviewRender::Render` pair in `Source_Core/` sets `opts.wrapWidth = ImGui::GetFontSize() * 48.0f`.

Originating investigation: tooltip bug that caused ~1.5h of agent mis-navigation between the two paths.

## Approach

**Path #1 (`RenderTextEditor`) is the primary display path and owns the tooltip.** `RenderTextEditor` is called for `TicketGridColumn::RenderPlan::TextEditor` when `allowEdits == true` AND the field uses an ADF document (it falls through to the display branch unconditionally). The `Selectable` hover it renders is the widget the user actually sees for description fields. That `BeginTooltip` block is live, correct, and already sets `opts.wrapWidth`.

**Path #2 (`renderPlainText` → `RenderClippedFieldText`) should be deactivated for ADF/description fields.** When `allowEdits == false`, the field lands in `renderPlainText`, which calls `RenderClippedFieldText`. That path has a structural defect for description fields: it passes the single-line-truncated `display` string as `rawValue`, so `hasNewline == false` and the tooltip fires only on horizontal overflow. The fix is to teach `renderPlainText` not to route description fields into `RenderClippedFieldText`; instead, call `RenderTextEditor` (which already handles the `allowEdits == false` read-only case gracefully). This also eliminates the duplicate `isDescriptionField` predicate in the lambda.

**The `isDescriptionLike`/`isDescriptionField` predicate should be extracted as a named static helper `IsDescriptionLikeFieldId`.** Both current predicates diverge slightly (`isDescriptionLike` catches `body`/`Body` and any field id containing "description"/"Description"; `isDescriptionField` catches only the latter). Extracting a single helper eliminates the fourth hand-rolled check.

**The `SmatchetOfflineQueueUi.cpp` `BeginTooltip`+`Render` call missing `opts.wrapWidth` is a class-2 bug and must be fixed in this PR.** The renderer falls back to `ImGui::GetContentRegionAvail().x` which is near-zero in a fresh tooltip window.

No shared helper function spanning both files is needed. The consolidation is structural (routing) not code-deduplication: `RenderTextEditor` already owns the right logic; the fix is preventing `renderPlainText` from shadowing it for ADF fields.

## Files to modify

| File | Change |
|---|---|
| `Source_Core/src/TicketFieldEditor.cpp` | Extract `IsDescriptionLikeFieldId` helper; in `renderPlainText` route ADF/description fields to `RenderTextEditor` instead of `RenderClippedFieldText` |
| `Source_Core/src/SmatchetOfflineQueueUi.cpp` | Add `opts.wrapWidth = ImGui::GetFontSize() * 48.0f` to existing `BeginTooltip`+`Render` block |
| `Source_Core/src/SmatchetFieldRender.cpp` | Comment only — path #2's `wrapWidth` is already correctly set; add note that this path is a safety net for non-ADF text fields only |

## Existing utilities reused

- `TrackerFieldPayload::FieldUsesAdfDocument(const TrackerField&)` — `Source_Core/include/TrackerFieldPayload.h` — canonical predicate for ADF routing; informs (but does not replace) `IsDescriptionLikeFieldId` since GitHub `body` is description-like but not ADF.
- `MarkdownConvert::AdfToMarkdown(nlohmann::json)` — already called in `RenderTextEditor`; no change.
- `IsTrackerDateOrDateTimeField` — existing anonymous-namespace helper in `TicketFieldEditor.cpp`; no change.

## UX Pillar callouts

- **Pillar 1**: ADF→markdown conversion fires only inside `IsItemHovered()` + `BeginTooltip` guard — identical cost profile to current path #1. No impact on steady-state grid-scroll loop.
- **Pillar 2**: No new blocking ops. `MarkdownConvert::AdfToMarkdown` is a pure in-process parse of a cached JSON string. No I/O reachable from any changed call site.
- **Pillar 3**: `field` pointer null-check is already upstream at the `RenderFieldCell` dispatch site. `RenderTextEditor` receives `const TrackerField&` reference. The `try/catch(...)` for ADF parse failures is already present in `RenderTextEditor`.
- **Pillar 4**: No impact. Tooltip content and hover semantics unchanged; same `Selectable` widget.

## Perf-review-system gates

1. **PR-fast CI**: `priority-grid-scroll` scenario most directly exercises `RenderFieldCell` → `RenderTextEditor` across ~200 visible rows. Run `bash scripts/dev/perf-run.sh priority-grid-scroll` and compare vs `docs/perf/baselines/priority-grid-scroll.dev.json` before merge.
2. **Pillar 2 static scanner**: no new sync I/O. N/A.
3. All other gates: N/A — no `MainThreadDispatcher::Drain` touch, no new stall path, no perf markers added/removed.

## Risks / non-goals

- **Risk: GitHub `body` field excluded from `FieldUsesAdfDocument`.** `IsDescriptionLikeFieldId` (not `FieldUsesAdfDocument`) must gate the routing in `renderPlainText` so `body`/`Body` fields continue through `RenderClippedFieldText` when `allowEdits == false` — GitHub returns markdown HTML, not ADF, so they must not go through the ADF tooltip path.
- **Risk: `allowEdits == false` + ADF field + `RenderTextEditor` starts an edit.** Mitigated: `RenderTextEditor`'s edit-start path is guarded by `state.IsEditingField` check and the `singleClickToEdit` flag; the `disabled` / read-only column case prevents edit initiation.
- **Non-goal: codebase-wide `BeginTooltip` sweep.** The audit identified exactly four `BeginTooltip`+`Render` pairs in `Source_Core/src/`; only `SmatchetOfflineQueueUi.cpp` was missing `opts.wrapWidth`. Other tooltip sites use `TextUnformatted` only.
- **Non-goal: frame-caching ADF→markdown conversion.** Fires only under hover guard; acceptable cost. Follow-up if profiling shows lift.
- **Non-goal: GitHub `body` rich tooltip.** Out of scope here.

## `BeginTooltip`+`MarkdownPreviewRender::Render` audit

All four sites in `Source_Core/src/`:

| File | `opts.wrapWidth` set? | Action |
|---|---|---|
| `TicketFieldEditor.cpp` (~line 591, path #1 — live) | Yes | No change |
| `SmatchetFieldRender.cpp` (~line 71, path #2) | Yes | Comment only |
| `SmatchetOfflineQueueUi.cpp` (~line 1031) | **No** | Fix: add `opts.wrapWidth` |
| `SmatchetAiAssistantUi.cpp` (uses `Mode::Full`, not in tooltip) | N/A | No change |

## Verification

- **Bucket A**: `FieldUsesAdfDocument` unit tests in `TrackerFieldPayloadPure` must stay green. Add one `test-rig` case for `IsDescriptionLikeFieldId` covering `body`, `Body`, `description`, `customDescription`, `environment` (expected: true/true/true/true/false).
- **Perf**: `bash scripts/dev/perf-run.sh priority-grid-scroll` — must not regress vs baseline.
- **Build**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Manual residue**: hover grid cell for Jira ticket with multi-paragraph ADF description — confirm tooltip renders with paragraph breaks, wraps at ~48 em. Repeat with GitHub `body` field and Offline Queue payload. Deferred-automation: add Bucket-E hover scenario for description tooltip path when ImGui test harness gains stable column-hover support; track in `docs/backlog/agent-self-improvement/tooling.md`.

## Implementation log

- **PR #430** (squash-merged as `c77584f1`): Fixed `opts.wrapWidth` in `SmatchetOfflineQueueUi.cpp`; restructured `renderPlainText` with lazy ADF tooltip; added `LOG_DEBUG` to catch blocks; added `body`/`Body` to `isDescriptionField` predicate; added `test-tooltip-wrapwidth.sh` CI gate.
- **This PR**: Extracted `IsDescriptionLikeFieldId` as a header-only inline in `Source_Core/include/TicketFieldEditorDescriptionPure.h`; replaced both inline predicates (`isDescriptionLike` in `RenderTextEditor`, `isDescriptionField` in `renderPlainText`) with calls to the helper; added safety-net comment to `SmatchetFieldRender.cpp`; added unit test `tests/Source_Core/IsDescriptionLikeFieldId.test.cpp`.

## Deviations from plan

- **`renderPlainText` → `RenderTextEditor` routing not implemented.** PR #430 already fixed the structural defect (tooltip only firing on horizontal overflow) via a lazy inline `BeginTooltip` block in `renderPlainText`. Routing to `RenderTextEditor` would introduce `allowEdits == false` + double-click → edit-start risk without correcting anything new. Deviation recorded in backlog testing item #3.
- **`IsDescriptionLikeFieldId` placed in a header-only inline** (`TicketFieldEditorDescriptionPure.h`) rather than an anonymous-namespace static. This makes it directly includable by the test rig without a separate pure TU, matching the pattern established by `TicketFieldEditorLongTextPure.h`.
- **`SmatchetFieldRender.cpp` change is a comment only** (as planned) — no logic change.

## Verification (actual)

- Dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`) clean — 0 errors.
- `test-tooltip-wrapwidth.sh` gate: 4 blocks checked, 0 violations.
- `IsDescriptionLikeFieldId` unit test: 8 cases pass (body/Body/description/customDescription → true; environment/summary/status/"" → false).
- Manual: offline queue payload tooltip renders multi-line with `opts.wrapWidth`.
