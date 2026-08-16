- 2026-08-03 · orchestrator · [test] · P2 — the tracker **Test connection** probe has no fixture seam a bucket-E test can inject through, so the five most valuable first-run assertions (probe→green result string, in-flight disabled-button cue, Save-after-green clears `ReadOnlyMode`, edit-after-green invalidates the fingerprint, offline escape leaves read-only set) are **unwritable at the UI layer** and fall back to pure-logic + a human smoke
  Details: Surfaced shipping slice 2 of `dev-onboarding-first-run-quickstart`. The plan's
    § Verification listed 7 bucket-E items; 2 shipped (`tracker_first_run_setup.test.cpp`), 5 did not.
    Root cause is a seam, not effort: #1833 shipped the probe as an **`AppController` member**, so a
    bucket-E test holding only `SmatchetActiveUiTestAppController()` has no point to substitute a
    fake backend at — it can drive the button but the probe then attempts a real network round-trip
    the CI runner cannot satisfy. The plan had anticipated this and chosen a public
    `AppController::BackendFactory()` accessor as the injection point; that decision was voided when
    the re-audit found the probe already inside `AppController`, and no replacement seam was designed.
    Net effect: an ImGui-Test-Engine lane that *looks* like it covers first-run setup covers two
    render branches of it.
  Concrete next action: give the probe a test-only backend seam. Cheapest shape that does not widen
    `AppController.h`'s fan-in: an `ITrackerBackendFactory` override settable on `AppController`
    behind `#if defined(SMATCHET_WITH_UI_TESTS)`, defaulting to the production factory — the bucket-E
    TU installs a fixture that returns a canned `AuthenticatedReachable` / failure result without I/O.
    Then port § Verification items 2-6 into `tracker_first_run_setup.test.cpp`. Prefer this over a
    network-level fake (an httplib stub would pin transport, not the setup state machine, which is
    what regressed-risk lives in). Est ~0.5-1d.
  Cross-ref: `docs/plans/shipped/dev-onboarding-first-run-quickstart.md` § Verification bucket-E items
    2-6 + § Deviations (the entry recording the descope); `tests/ui/tracker_first_run_setup.test.cpp`
    (the 2 shipped tests); `Source/Core/src/Ui/SmatchetPreferencesUi.cpp` (probe dispatch +
    `onPreferencesSaveAndSync`); `Source/Core/include/ITrackerBackendFactory.h`;
    PR #1833 (shipped the probe as an `AppController` member, closing the planned accessor seam).
  Status: open
  Last-reviewed: 2026-08-03
