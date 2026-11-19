- 2026-11-17 · dropdown-recents-w1 · [debt] · P3 — FilterDropdown: mouse movement does not clear `KeyNavigated`, and a Space press can toggle the wrong row for one frame

Details:
PR #2432 (shared dropdown recents + type-to-filter + tick-box Space/Enter) merged with two CodeRabbit second-round
findings deferred on purpose (owner direction: get the PR in, defer what is left). Both are in
`Source/Core/src/FilterDropdown.cpp`:

- Mouse move never clears `st.KeyNavigated`, so after an arrow-key move the keyboard highlight keeps
  winning over a hovered row until the filter text changes. Hover should hand the highlight back to the mouse.
- One-frame Space race (thread `discussion_r4586638903`): if the keyboard-navigated row disappears on the frame
  Space is pressed, `SpaceToggles` has already returned true before `st.KeyNavigated` is cleared (about
  line 583), and the toggle at about line 767 then acts on the replacement default row even when it does
  not match the filter. The toggle decision has to be evaluated against the updated highlight.

A prepared patch for both is `dropdown-2432-deferred-cr-fixes.patch` in the side-session notes folder of session
`local_05cefdbc-3277-4e93-89b8-32d4d9a66166` (outside the repo, machine-local; re-derive from the two
descriptions above if it is gone).

Concrete next action:
Apply the patch (or re-implement), add a bucket-E case to `scripts/dev/test-ui-filter-dropdown.sh`
(arrow to a row, hover another, Space toggles the hovered one; filter the navigated row away and press Space in
the same frame toggles nothing that fails the filter), run the dropdown and bulk-field-edit suites.

Status: open
Last-reviewed: 2026-11-17
