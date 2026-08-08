- 2026-08-07 · claude-code · [test] · P2 — the bucket-E "no app booted → skip" guard fails **open** at 50 of its 56 call sites, so a fully-unbooted run reports all-green instead of red

  Nearly every bucket-E case that needs a live app opens with the same guard and *skips* when
  the app is not up. Verbatim from `tests/ui/ai_chat_panel.test.cpp:138-142`:

  ```cpp
  const AppController* app = SmatchetActiveUiTestAppController();
  if (app == nullptr) {
      ctx->LogInfo("SKIP: app not booted");
      return;
  }
  ```

  **56** sites match `AppController* app = SmatchetActiveUiTestAppController();` across **28**
  files under `tests/ui/` (119 references to the accessor across 31 files in total). The log
  message wording varies; the shape does not. On the
  [PR #1966](https://github.com/alexandrosk0/Smatchet/pull/1966) branch the same shape is
  wrapped in a `BootedAppOrSkip` helper local to one test file; that helper does not exist on
  `develop`, and wrapping it does not change the semantics below.

  Individually the guard is correct — a case that cannot run should not fail. Collectively it
  is a hole: if the app never boots at all, **every** such case skips, the runner prints
  `passed=N failed=0`, and the JSON envelope reports `ok:true`. A green result is therefore
  consistent with zero real coverage.

  It is not universal, and the exceptions are the precedent for the fix: **6** sites already
  fail **closed**, surfacing the unbooted app as a real failure —
  `tests/ui/jira_deterministic_backend.test.cpp:70,108,147`,
  `tests/ui/linear_deterministic_backend.test.cpp:70,113`,
  `tests/ui/preferences_tracker_switch.test.cpp:110`, all
  `IM_CHECK_NO_RET(app != nullptr);` before the return. Whatever reasoning made those six
  fail closed applies to the other **50**.

  Those six are *inside* the 56, not additional to it — 56 is the count of the assignment shape,
  which every site shares regardless of what it does next. A first draft of this entry said the
  guard fails open "at 56 sites" and, four paragraphs later, that six of them fail closed; the
  two numbers were derived from one grep whose match set contains both populations, so the entry
  contradicted itself in its own text (6 + 56 ≠ 56). Review caught it. The generalisable check —
  when a claim reads "N do X, M do not-X" off one grep, assert the two sets are disjoint before
  writing the numbers down — is filed separately at
  [`categories/process/2026-08-07-two-counts-from-one-grep-must-be-disjoint.md`](../process/2026-08-07-two-counts-from-one-grep-must-be-disjoint.md).

  The mechanism is in [`Source/Core/src/Commands/Scenarios/UiTestScenario.cpp`](../../../../Source/Core/src/Commands/Scenarios/UiTestScenario.cpp)
  lines 381 and 402-403: `passed_` comes from `ImGuiTestEngine_GetResult` and
  `out["failed"]` is `max(0, tested_ - passed_)`. A `TestFunc` that returns early without a
  failing `IM_CHECK` ends in `Success`, so it lands in `passed` and contributes zero to
  `failed`. The existing zero-run floor (`PASSED == FAILED == 0` → FAIL, e.g.
  [`scripts/dev/test-ui-grid-pane-windows.sh`](../../../../scripts/dev/test-ui-grid-pane-windows.sh)
  lines 86-92, the check itself on 88 and its `exit 1` on 91) therefore can never trip on an
  all-skipped run.

  Proposed fix, in the shared bucket-E harness rather than per-case:

  1. Count skips separately from passes and surface `skipped` in the JSON envelope. The hook
     is already in place — 59 `ctx->LogInfo("SKIP…")` lines exist under `tests/ui/`, so a
     scenario-side counter keyed on that call (or a small `SmatchetUiTestSkip(ctx, reason)`
     wrapper replacing the raw `LogInfo`) needs no per-case rewrite of the guard logic.
  2. Raise the floor from "zero cases ran" to **"zero cases ran *for real*"** — fail when
     `passed > 0 && passed == skipped`, i.e. nothing actually exercised product code.
  3. Have the per-feature runner scripts (`scripts/dev/test-ui-*.sh`) assert
     `skipped == 0` by default, with an explicit opt-out for suites that legitimately skip
     on a capability check.

  Found while verifying the `WindowExpand` and `Dock` suites for
  [PR #1966](https://github.com/alexandrosk0/Smatchet/pull/1966) /
  [PR #1984](https://github.com/alexandrosk0/Smatchet/pull/1984) — both were genuinely
  green, but the only reason I could say so is that I separately confirmed the exe was
  newer than every edited source and that the scenario log showed frames running. That
  confirmation should be the harness's job, not the reviewer's.

  Related, on the [PR #1966](https://github.com/alexandrosk0/Smatchet/pull/1966) branch (so
  not linkable from here until it merges): `categories/test/2026-08-07-window-expand-overlap-and-redock-guards.md`,
  which covers the complementary gap — those 9 cases are green but none of them exercises
  the code the PR changed.
