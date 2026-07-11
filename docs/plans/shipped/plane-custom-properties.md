# Plan — Plane custom (UUID) properties: serialize instead of silently dropping (C4)

> **Slug**: `plane-custom-properties`
>
> **Status**: `shipped`

## Context

[`backlog/BACKLOG_CODE_REVIEW.md`](../../../backlog/BACKLOG_CODE_REVIEW.md) §C4 (audit item 40) is one of the three items still genuinely open after the 2026-07-05 reconciliation pass: `PlaneClient::BuildCreatePayload` serializes only the built-in ids (`summary`/`description`/`priority`/`status`/`type`/`parent`/`assignee`) and ignores its `catalog` parameter entirely, so every custom (UUID work-item-property) `TrackerField` the user fills in a new-issue draft or bulk import is **silently dropped** — the create succeeds and the data is gone. `BuildUpdatePayload` delegates to the same builder, and `PlaneClient::FetchIssueEditMeta` already documents the agreed target shape: "Custom-property (UUID) editability is deferred until C4 lands `properties.<uuid>` serialization".

## Approach

Same pure-seam shape as `PlaneIssueMappingPure` / `PlaneFieldCatalogPure`: a new cpr-free TU `PlaneCustomPropertyPure` owns the draft-string → typed-JSON serialization (`BuildPlaneCustomProperties(fieldValues, catalog)`), and `BuildCreatePayload` emits its non-empty result under `properties`. Values are typed per catalog family (NUMBER → JSON number, boolean Type → true/false, multi families → CSV-split array, select/user → display-label→option-id resolution with raw pass-through for unknown values); read-only customs and empty values are skipped. A value the family cannot represent (non-numeric NUMBER, malformed boolean) aborts the build with `TrackerErrorInvalidRequest` — a visible validation error is strictly better than the current silent data loss, and matches the Jira builder's behaviour for the same class. The server keeps the final say on acceptance; a rejected `properties` key surfaces through the existing mutation error path like any other field rejection.

## Files to modify

1. `Source/Core/include/Tracker/PlaneCustomPropertyPure.h` — NEW pure header (namespace `smatchet::plane`; `rg -l PlaneCustomProperty Source/` confirmed no prior unit).
2. `Source/Core/src/Tracker/PlaneCustomPropertyPure.cpp` — NEW TU; per-family serializer reusing `TrackerFieldPayloadPure` primitives (`FindOptionByIdOrValue`, `SplitCommaSeparatedTrimmed`, `ParseNumberValue`).
3. `Source/Core/src/Tracker/PlaneIssueMutation.cpp` — `BuildCreatePayload` un-ignores `catalog`, appends the non-empty customs object under `properties`, and propagates the serializer's validation error as `TrackerErrorInvalidRequest`.
4. `Source/Core/src/Tracker/PlaneFieldCatalog.cpp` — `FetchIssueEditMeta` comment refreshed: serialization landed; custom editability still unreported because that seam has no catalog parameter (flagged in § Out of scope).
5. `tests/Core/PlaneCustomPropertyPure.test.cpp` — NEW bucket-A suite: per-family typing, built-in/unknown/read-only/empty skips, label→id resolution + raw pass-through, CSV multi split, number/boolean validation errors naming the field.
6. `tests/Core/PlaneIssueMutationHttp.test.cpp` — wiring cases on the real `PlaneClient`: customs ride under `properties.<uuid>`, core-only drafts keep the pre-C4 wire shape (no `properties` key), invalid custom value → `InvalidRequest`.
7. `tests/CMakeLists.txt` — wire the new suite + production TU into `SmatchetTests` (its only dep, `TrackerFieldPayloadPure.cpp`, is already linked).
8. `backlog/BACKLOG_CODE_REVIEW.md` — flip §C4 + the carry-over table row + the "still genuinely open" line.

## Existing utilities reused

- `TrackerFieldPayloadPure` option/CSV/number primitives — no duplicated per-family parsing.
- `SmatchetTests` direct-TU-link pattern (`tests/CMakeLists.txt` contract) — no new test infra.
- The existing `PlaneIssueMutationHttp.test.cpp` fixture suite hosts the shell wiring cases.

## Extraction sizing

New pure TU ~100 lines + header ~35; `PlaneIssueMutation.cpp` +10 (wiring block). No file approaches a cap.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — one linear catalog walk per create/update build; the HTTP round trip dominates.
- **Pillar 2 (UI never freezes)**: no impact — pure string/JSON work on an already-async mutation path.
- **Pillar 3 (never crash)**: no new throw surface — nlohmann object construction only; validation returns `Result` errors.
- **Pillar 4 (accessibility)**: no UI change. Data loss on create is fixed, which is the user-facing win.

## Perf-review-system gates

1. **PR-fast CI**: N/A — no scenario hot path changes shape.
2. **Pillar 2 static scanner**: no new sync-I/O reachable from `ImGui::*`.
3. **Dispatcher drain**: untouched.
4. **Visible-cue bucket-E harness**: no new stall path.
5. **Marker inventory**: no new markers.

**Pre-push local check**: N/A — no perf-relevant change (see gate 1).

## Risks / non-goals

- **Risk**: Plane deployments whose create endpoint rejects an unknown `properties` key would now fail a create that previously "succeeded" (minus the customs). Accepted: the failure is visible and actionable, the silent loss was not; only drafts that actually carry custom values emit the key (core-only drafts keep the byte-identical pre-C4 wire shape).
- **Risk**: label→id resolution picks the wrong option when a display label collides with another option's id. Mitigation: `FindOptionByIdOrValue` is the same resolution the Jira builder uses; collision behaviour is pinned by tests.
- **Non-goal**: dispatching customs to Plane's per-property values endpoint (`…/issue-properties/<id>/values/`) — if a live deployment rejects the inline key, that's the v2 shape, and it needs a live server to characterize against.
- **Non-goal**: reporting customs editable from `FetchIssueEditMeta` (seam has no catalog parameter — flagged below).

## Verification

- **Bucket A (pure-logic ctest)**: `PlaneCustomPropertyPure.test.cpp` (6 cases / 44 assertions) green — run locally under the Linux mini-doctest harness (pure TU + `TrackerFieldPayloadPure` + link stubs for its unused Markdown/date symbols).
- **Shell wiring**: `PlaneIssueMutationHttp.test.cpp` new TEST_CASE (3 subcases) — compiles locally (`-fsyntax-only` with the FetchContent include set); executes in CI's full rig.
- **Build gate**: CI dual-target + POSIX lanes (local full build unavailable in this container — curl FetchContent blocked by the egress proxy).
- **Doc validation**: `scripts/dev/test-docs.sh` suite green.
- **Manual residue**: none automatable-away: a live Plane v1 server accept/reject characterization of the inline `properties` key remains a manual smoke (flagged in § Out of scope).

## Out of scope (flagged, not designed)

- `FetchIssueEditMeta` growing a catalog parameter so custom UUIDs can be reported editable in the grid edit UI (C4 follow-on; without it customs flow through create/bulk-import/update payloads but the per-cell edit affordance stays built-ins-only).
- Live-server characterization of Plane's accepted create-payload shape for customs (`properties` inline vs per-property values endpoint) — needs credentials/deployment; tracked as manual residue.
- The remaining open backlog items C6 / N12 — separate slices.

## Implementation log

- Single slice (this PR, branch `claude/fable-5-codebase-improvements-l90taa` restarted from develop): all § Files to modify items landed together — pure serializer + shell wiring + both test suites + backlog flips (C4 section, carry-over table row, "still genuinely open" line, and the C3 cross-reference that pointed at C4 as its blocker).

## Deviations from plan

- None in the code shape. The plan doc was authored and archived in the same PR (single-slice plan; no interim `active` state ever shipped).

## Verification (actual)

- `PlaneCustomPropertyPure.test.cpp`: 6 cases / 44 assertions green under the Linux mini-doctest harness (pure TU + `TrackerFieldPayloadPure.cpp` + link stubs for its uncalled Markdown/date symbols — this container cannot build the full rig because the egress proxy blocks curl's FetchContent tarball).
- `PlaneCustomPropertyPure.cpp` and both edited test files pass `clang++-18 -std=c++14 -fsyntax-only` with the repo + FetchContent include set; the `PlaneIssueMutation.cpp` wiring block could not be syntax-checked locally (its cpr include chain needs the blocked curl headers) and is verified by CI's build lanes.
- `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` + `scripts/dev/test-docs.sh` — recorded in the PR body test plan.
- Windows full rig (`SmatchetTests` incl. the new suite + the `PlaneIssueMutationHttp` wiring cases) — CI.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
Flip § Status to `shipped`, populate the three sections above, `git mv` to `docs/plans/shipped/` in the same PR.
