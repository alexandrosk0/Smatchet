# Bucket-E: mobile touch cell-editor long-press / commit / discard matrix

**Category**: test · **Priority**: P2 · **Raised**: 2026-06-23 · **Slice**: P1.3 (mobile-app-fuller-integration)

## What

The P1.3 touch cell editors (long-press open + arm-then-popup + explicit-commit policy) were
emulator-verified by hand for two discard paths only (inline-text `Escape`, SingleSelect combo tap-away).
The full behavioural matrix is still manual:

- **Open gesture** — long-press past `kCellLongPressOpenSeconds` opens/arms each of the five editors
  (inline text, SingleSelect, MultiSelect, Cascading, Labels) + the DateTime modal; a quick tap does **not**
  open (it selects/scrolls).
- **Commit** — Save / Apply on each editor issues exactly one queued field-edit (and the no-op-PUT
  suppression via `valueChanged` fires when the value is unchanged).
- **Discard** — Cancel / Back / tap-away on each editor issues **zero** queued edit (the stray-PUT proof,
  extended to all four non-text editors + DateTime).
- **DateTime centering** — the modal opens phone-centered (`SetNextWindowPos(displayCenter)`) on the touch
  build vs `MousePos` on desktop.

## Why automate

Hand-verifying five editors × {open, commit, discard} per slice is slow and non-deterministic (IME timing,
screenshot scaling). The pure seams (`ShouldOpenCellEditorByLongPress`, `ShouldCommitTouchPopupEdit`,
`ShouldCloseTouchPopupEdit`) are already unit-tested; the gap is the **ImGui glue** that wires them —
which `test-rig` refuses (ImGui surface). That is exactly bucket-E (ImGui Test Engine) territory.

## Action plan

Write an ImGui Test Engine case that drives each editor via simulated long-press + click:
1. arm + open each editor, assert the popup/modal id is open;
2. Apply → assert one `QueueFieldEdit` with the canonical value; unchanged-value Apply → assert none
   (no-op suppression);
3. Cancel / Back / tap-away → assert zero `QueueFieldEdit`;
4. assert the DateTime modal pos == display center on the `kMobileTouchBuild` path.

## Blocker

Bucket-E mobile coverage is **CI-blocked today**: the Mesa-GL bucket-C/E lanes can't boot the CI exe
(AGENTS.md § Merge gates — `Bucket-` dropped from the gate 2026-06-15). Land the case so it runs locally +
is ready when the lane is restored. Shares the blocker with the P1.2 view-switcher bucket-E entry
(`2026-06-23-mobile-view-switcher-bucket-e.md`).
