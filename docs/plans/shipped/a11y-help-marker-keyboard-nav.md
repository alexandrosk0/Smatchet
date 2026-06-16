# Plan — a11y: keyboard-reachable (?) help-marker tooltips (#1128)

> **Slug**: `a11y-help-marker-keyboard-nav` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — all cited PRs merged (see Implementation log); archived 2026-06-16 via plan-archival sweep.

## Context

GitHub Issue **#1128** (P2, Pillar-4 regression). PR #1124 (merged 2026-06-11) moved ~38 long-form UI explanations from always-visible inline text into **hover-only** tooltips behind a `(?)` glyph (`Source/Core/src/Ui/SmatchetHelpMarker.cpp`). Keyboard-only users lost all access to that text — it fires on mouse-hover only, with no nav-focus path. Accepted as a known Pillar-4 deviation at ship; this Issue files the elevation that was missed.

Affected surfaces: Preferences (Assistant / Whisper / Local / Appearance / Tracker / …) and any panel carrying a `(?)` marker. Intended outcome: after this lands, a keyboard-only user can reach every `(?)` help-marker's long-form text via standard ImGui nav focus.

## Approach

The fix is **centralized in `SmatchetHelpMarker.cpp`** — the ~38 call sites need no change (they call the shared marker). Today the marker is effectively `TextDisabled("(?)")` + `if (ImGui::IsItemHovered()) BeginTooltip(...)`. A `TextDisabled` item does not participate in keyboard nav and never becomes focused.

Make the marker a **focusable item that shows its tooltip on focus as well as hover**: render the glyph as a nav-participating item (smallest change: keep the visual but make it focusable — e.g. an invisible/`Selectable`-style hit item, or set the item flags so `ImGuiNavMoveFlags`/tab-focus reaches it), then show the tooltip when **`ImGui::IsItemHovered() || ImGui::IsItemFocused()`**. On focus, render the same tooltip (ImGui tooltips can be driven from focus, not just hover). This restores parity: Tab to the marker → tooltip appears.

**Decided (grilled 2026-06-13): nav-only focusable** — markers become reachable only when keyboard-nav is active; the mouse-only tab-order is unchanged, so mouse users see no new tab stops while keyboard users get a predictable path to every help-marker. (Rejected: always-in-tab-order, which would add ~38 stops for everyone.)

## Files to modify

1. `Source/Core/src/Ui/SmatchetHelpMarker.cpp` — make the `(?)` item focusable; show tooltip on `IsItemHovered() || IsItemFocused()`. Single seam covering all ~38 call sites.
2. `Source/Core/include/Ui/SmatchetHelpMarker.h` — only if the signature needs a flag (e.g. an opt-out for markers that shouldn't be in tab-order).

## Existing utilities reused

- `SmatchetHelpMarker` shared helper (`SmatchetHelpMarker.cpp`) — the one render path all ~38 sites call; the whole fix lives here.
- ImGui `IsItemFocused()` / item-flags nav APIs — standard ImGui keyboard-nav (already enabled in the host? confirm `ImGuiConfigFlags_NavEnableKeyboard` is set in `SmatchetImGuiHost`; if not, that's a prerequisite line).

## UX Pillar callouts

- **Pillar 1 (perf)**: negligible — one extra `IsItemFocused()` check per marker per frame; no new allocation or I/O.
- **Pillar 2 (UI-thread)**: no impact — no I/O.
- **Pillar 3 (never crash)**: no lifetime/threading change.
- **Pillar 4 (accessibility)**: the point — restores keyboard reachability of help text. WCAG-aligned (focus parity with hover).

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

`Source/Core/src/Ui/` is touched but the change is a per-item focus check with no new sync I/O or per-frame cost beyond a boolean. **Gates 1–5: N/A** — no scenario hot-path change, no new sync-I/O reachable from `ImGui::*` (gate 2 clean), no `MainThreadDispatcher` touch, no >100 ms stall, no perf markers. Declare `cell-edit`/`preferences` scenarios unaffected in the PR.

## Risks / non-goals

- **Risk**: adding markers to tab-order annoys keyboard power-users (extra stops). Mitigation: nav-only focusability or low nav priority; verify the tab-order feels right (bucket-E).
- **Risk**: if `ImGuiConfigFlags_NavEnableKeyboard` isn't enabled host-side, focus never lands — confirm/enable as a prerequisite.
- **Non-goal**: a global "show all help inline" accessibility toggle (a broader a11y feature) — separate plan if wanted.

## Verification

- **Bucket A**: N/A — no pure logic.
- **Bucket E (ImGui Test Engine)**: a test that opens a panel with a `(?)` marker, drives **keyboard nav** to focus the marker, and asserts the tooltip text renders (the exact #1128 repro, automated). This is the key gate.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: grill the always-in-taborder vs nav-only decision with the user.
- **Manual residue**: none — the bucket-E keyboard-focus test covers the repro.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray refs to anything deferred here (esp. the `feat/1124` test.md entry that flagged this elevation).

- Global "show all help inline" toggle — separate a11y feature.
- A broader Pillar-4 audit (font scaling, contrast) — the backlogged Pillar-4 epic.

## Implementation log
- `4be10390` · fix(a11y): keyboard-reachable (?) help-marker tooltips (#1128) (#1179) — `InvisibleButton` + `ImGuiButtonFlags_EnableNav` in `SmatchetHelpMarker.cpp`; tooltip on `IsItemHovered() || IsItemFocused()`; bucket-E gate `tests/ui/help_marker_keyboard_focus.test.cpp`. Related postmortem #1131 (`707bf79c`).

## Deviations from plan
- None — shipped as planned; out-of-scope items unchanged (global show-all-help toggle and broader Pillar-4 audit remain deferred).

## Verification (actual)
- Bucket-E gate `tests/ui/help_marker_keyboard_focus.test.cpp` present in tree (archival audit 2026-06-16), not re-run.
- Centralized fix in `Source/Core/src/Ui/SmatchetHelpMarker.cpp` (focusable `InvisibleButton` + `ImGuiButtonFlags_EnableNav`, tooltip on `IsItemHovered() || IsItemFocused()`) verified present in tree (archival audit 2026-06-16), not re-run.
