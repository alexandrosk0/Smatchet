# Plan — User-info window bucket-E coverage (PR-10 residue)

> **Slug**: `user-info-window-bucket-e` (matches this file's basename without `.md`).
>
> **Status**: `active` — the one open member of `docs/plans/backlog-pr-roadmap.md` PR-10 (the other six shipped). Backlog member: `user-info-window-bucket-e-coverage` (tooling.md, P2). Depends on the PR-9 bucket-E harness fixes (shipped).

## Context

Slice 5 of `docs/plans/user-info-window.md` shipped the window, but its planned bucket-E (ImGui Test Engine) + screenshot-diff coverage was deferred — recorded as not-run in that plan's § Verification (actual) under the visual-validation exception. The window's interaction contract is currently validated only by a one-time visual-validation pause, not by a repeatable gate. Until covered, any regression in `SmatchetUserInfoUi`'s lifecycle (the relaunch-flag future handling, or the close-edge `ClearPaneUserActivity`) lands silently.

Intended outcome: *after this lands, the user-info window's 7 named lifecycle/interaction behaviours are pinned by a bucket-E TU + 4 screenshot baselines, so a lifecycle regression reds a required gate instead of shipping silently.*

## Approach

Add one bucket-E test TU that drives the window through the staged-request latch (`UiDrawSession::userInfoRequestPending` + identity fields) against a fixture backend, asserting the 7 behaviours the shipped plan's § Verification enumerates. Register 4 screenshot baselines (desktop / narrow × unified / separate VCS layout) in the existing screenshot-diff harness. This mirrors the now-shipped sibling PR-10 bucket-E TUs (`tests/ui/views_field_selection.test.cpp`, `tests/ui/keybindings_editor_rebind.test.cpp`) — same `UiDrawSession` latch-driven pattern, so no new harness capability is needed (PR-9 already landed the harness fixes).

## Files to modify

1. `tests/ui/user_info_window.test.cpp` (new) — the bucket-E TU; drive via `userInfoRequestPending` latch; assert the 7 behaviours below. Register in `tests/CMakeLists.txt` alongside the other `tests/ui/*.test.cpp` bucket-E rows.
2. Screenshot-baseline assets (4) — desktop/narrow × unified/separate — added to the existing screenshot-diff baseline set the harness consumes.
3. `docs/plans/user-info-window.md` — one-line § Verification (actual) update flipping the deferred bucket-E rows from not-run to covered (PR-only edit of a shipped plan).

The 7 behaviours to assert (from `docs/plans/user-info-window.md` § Verification):
1. window open/close (Escape + Close button);
2. Close/Escape triggers `ClearUserActivity`, while a host-`isOpen` clear does **not**;
3. load-button disabled while loading;
4. one-shot group-member fetch per open;
5. `VcsFeedLayout` toggle (unified ↔ separate) + config-key persistence;
6. ~400px narrow-layout assertion (sections stack, card rows, no horizontal scroll);
7. the four-section render at both widths.

## Existing utilities reused

- `UiDrawSession::userInfoRequestPending` + identity fields — `Source/Core/include/Ui/SmatchetUiSession.h` — the staged-request latch the TU drives.
- `tests/ui/views_field_selection.test.cpp`, `tests/ui/keybindings_editor_rebind.test.cpp` — shipped PR-10 sibling bucket-E TUs; the latch-driven pattern to copy.
- The PR-9 bucket-E harness fixes (`SMATCHET_UITEST_WITH_LOCAL_CACHE`, `ui_test.run --outLog`, `FakeTrackerClient` auto-sticky) — already shipped; the dependency this residue was blocked on.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — adds a test TU + baselines; extracts/splits nothing.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — test-only.
- **Pillar 2 (UI-thread)**: no impact (test-only); the TU asserts the existing draw path, changes none of it.
- **Pillar 3 (never crash)**: positive — the TU pins the close-edge `ClearPaneUserActivity` / relaunch-flag lifecycle so a future regression is caught.
- **Pillar 4 (accessibility)**: the narrow-layout (~400px) assertion guards the responsive/no-horizontal-scroll contract.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A`)

`N/A` — the diff is test-only (`tests/ui/` + baselines + a shipped-plan doc line). No `Source/Core/src/` change. If a test-only dirty-flag shim is required in a Core header to make a behaviour observable, keep it `#ifdef`-guarded to the test build and re-declare these gates in review.

## Risks / non-goals

- **Risk: screenshot baselines need golden-image approval (the original defer reason).** Mitigation: this is a human approval step at PR time (baseline review), not a design blocker — call it out in the PR so the reviewer approves the 4 baselines.
- **Risk: a behaviour isn't observable through the public latch.** Mitigation: prefer a fixture-backend assertion; only if unavoidable, add a test-only dirty-flag shim (as the keybindings-editor sibling did) rather than widening product API.
- **Non-goal: refactoring `SmatchetUserInfoUi`.** This adds coverage of the shipped contract; behaviour stays byte-identical.

## Verification

- **Bucket A**: N/A.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: the new `user_info_window.test.cpp` green; the 7 behaviours asserted.
- **Bash-driver scenario / screenshot / sanitizer**: 4 screenshot baselines (desktop/narrow × unified/separate) added and diffed green.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: confirm each of the 7 behaviours maps to an assertion and each baseline to a width/layout combination before finalising.
- **Manual residue**: the 4 screenshot baselines require one-time reviewer approval (visual-validation exception) — named here, not silent.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — no symbols are deferred by this plan; it *closes* the deferral recorded in `user-info-window.md` § Verification (actual) and tooling.md.

- **`android-emulator-mvp-smoke-harness`** — a sibling tooling.md bucket-E-coverage gap on the mobile MVP; unrelated surface, not folded here.

## Implementation log
*(populated post-ship — bullet per shipped commit)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*In the SAME PR that fills the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*
