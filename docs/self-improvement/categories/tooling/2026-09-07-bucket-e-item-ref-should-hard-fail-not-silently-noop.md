- 2026-09-07 · code-review · [tooling] · P2 — a bucket-E item ref that resolves to nothing degrades a test to a silent no-op instead of a hard failure

  Details: the ImGui Test Engine helper `ClickSortByCheckbox(ctx, ref)` used across bucket-E grid tests
  returns `false` when the item ref does not resolve, and every call site does
  `IM_CHECK_NO_RET(clicked); if (!clicked) return;` — which reports one failed assertion and then exits
  cleanly, reading in CI logs almost identically to a legitimately-skipped test. In PR #2198 a UI-label
  rename left the item ref (`"**/Story group"` -> `"**/Parent group"` typo — see the paired
  `localization-t-fallback-wins-...` entry) pointing at nothing; the test still "passed" in the sense of
  not crashing the runner, and the feature it exists to cover shipped uncovered until an adversarial review
  caught it by tracing the localization call path, not by reading test output.
  Found during adversarial review of docs/plans/shipped/parent-issue-hierarchy.md (PR #2198).

  Concrete next action: add a shared `IM_CHECK_ITEM_EXISTS`-style helper to the ui-test harness
  (`tests/ui/` support code) that hard-fails with the literal ref text in the failure message when a ref
  does not resolve, and use it ahead of every `ClickSortByCheckbox`-style call instead of the
  check-then-early-return pattern. Also note in `agents/_shared/skills/test-authoring` (or wherever bucket-E
  authoring guidance lives) that a checkbox/button label used as an item ref belongs in a single shared
  constant alongside the `T(key, fallback)` fallback string it mirrors, so a rename cannot drift the two independently.
  Status: open
  Last-reviewed: 2026-09-07
