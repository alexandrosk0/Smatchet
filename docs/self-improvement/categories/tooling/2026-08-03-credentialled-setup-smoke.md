- 2026-08-03 · orchestrator · [tooling] · P2 — the first-run tracker-setup path has **no automated coverage against a real backend**: bucket E exercises the UI branches with no credentials at all, bucket A exercises the pure predicates, and nothing in between ever performs a real Jira/Plane/GitHub/Linear round-trip, so "delete `smatchet_config.json`, launch, complete setup against a live account" stays a human smoke on every change to the setup flow
  Details: Surfaced shipping slice 2 of `dev-onboarding-first-run-quickstart` (the Preferences Tracker
    first-run surface). The gap is structural, not an oversight: no CI runner holds live tracker
    credentials and none should. The consequence is that the highest-value assertion in the whole
    feature — *a real user with real credentials ends up out of read-only mode with a working backend* —
    is the one assertion no gate makes. `TrackerSetupPure::CredentialFieldsComplete` /
    `NeedsSetup` / `CredentialFingerprint` are covered at the pure layer (5 cases / 26 assertions),
    and `tracker_first_run_setup.test.cpp` covers the explainer render + the close-clears-fingerprint
    path, but the probe→save→`ReadOnlyMode`-clear chain is only ever walked by a human.
  Concrete next action: add a **credentialled opt-in lane** modelled on the existing local-data opt-in
    pattern in `tests/Core/data_dependent_windows_smoke.test.cpp` — a doctest/bucket-E case that
    `SKIP`s unless a developer-local env var (`SMATCHET_LIVE_TRACKER_SMOKE=1` plus the usual
    `SMATCHET_JIRA_*` config env) is set, and when set drives config-wipe → probe → save → assert
    `BackendHasBeenReachable == true` and `ReadOnlyMode == false`. Never enabled in CI; the value is
    that the smoke becomes **reproducible on demand** (one command, deterministic assertions) instead
    of a prose checklist a human re-derives each time. Est ~0.5d.
  Cross-ref: `docs/plans/shipped/dev-onboarding-first-run-quickstart.md` § Verification § Manual residue
    (the bullet that requires this entry); `tests/ui/tracker_first_run_setup.test.cpp`;
    `tests/Core/TrackerSetupPure.test.cpp`; `tests/Core/data_dependent_windows_smoke.test.cpp`
    (the opt-in pattern to copy); `Source/Core/src/Ui/SmatchetPreferencesUi.cpp`
    (`onPreferencesSaveAndSync` — the fingerprint-gated `ReadOnlyMode` clear this lane would pin).
  Status: open
  Last-reviewed: 2026-08-03
