- 2026-08-03 · orchestrator · [test] · P1 — **every** bucket-E `--spawn` driver hung to its full timeout on a debug/asserts build, and the surfaced error named the wrong cause: the child process passed its tests, then tripped `"You need to call ImGui::DestroyContext() BEFORE ImGuiTestEngine_DestroyContext()"` during teardown and died before printing the result envelope, so the parent reported `{"code":"timeout","hint":"Try --timeout=<larger-ms> or --frames=<smaller-n>"}` — a hint that is actively misleading (no timeout value can fix a teardown assert, and `CliDispatch.cpp:476` ignores `pa.timeoutMs` on the `--spawn` path anyway)
  Details: Surfaced writing the bucket-E TU for slice 2 of `dev-onboarding-first-run-quickstart`.
    Isolated as pre-existing by running an **untouched** sibling driver
    (`scripts/dev/test-ui-annotate-prefs-persist.sh`), which hung identically. Mechanism: the test
    engine is created per `ui_test.run` and destroyed while the app's ImGui context is still alive
    (the app outlives any single `ui_test.run`), and ImGui's settings-save path runs against the
    engine's context during that window. Fixed in this slice by setting `io.ConfigSavedSettings = false`
    in `Source/Core/src/Commands/Scenarios/UiTestScenario.cpp` — the scenario has no use for persisted
    ini state, and disabling it removes the teardown write entirely. Two failure-visibility problems
    remain and are the real lesson: (1) a child that dies after passing is indistinguishable at the
    parent from a child that never finished; (2) the timeout hint asserts a remedy the code path does
    not implement. Note: bucket-E **CI-lane** flake remediation (llvmpipe/Mesa collapse to
    `Passed:0 Failed:73`) is separately in flight in the `bucket-e-gate-escape-pm` /
    `bucket-e-residual-fix` worktrees — this entry is the **local driver/teardown** class, not that one.
  Concrete next action: two small changes in `Source/Core/src/Commands/CliDispatch.cpp` around the
    `--spawn` wait. (a) On child exit **without** a parsed envelope, distinguish the cases: report
    `code:"child-died"` with the child's exit code and the tail of its log instead of `code:"timeout"`,
    and only report `timeout` when the child is still alive at the deadline. (b) Either honour
    `pa.timeoutMs` in the `scenarioWaitMs = (frames / 60 + 30) * 1000` computation at :476, or drop
    `--timeout` from the hint string — a hint naming a flag the path ignores costs every future
    investigator the same detour. Est ~0.5d. Optional follow-on: a bats case that plants a child
    which exits non-zero after printing a PASS line and asserts the parent reports `child-died`.
  Cross-ref: `Source/Core/src/Commands/Scenarios/UiTestScenario.cpp` (the `io.ConfigSavedSettings = false`
    fix); `Source/Core/src/Commands/CliDispatch.cpp:476` (the `scenarioWaitMs` computation that drops
    `pa.timeoutMs`); `scripts/dev/test-ui-tracker-first-run-setup.sh` + `scripts/dev/test-ui-annotate-prefs-persist.sh`
    (drivers that both hung pre-fix); `docs/plans/active/dev-onboarding-first-run-quickstart.md`
    § Deviations (the out-of-plan infra-fix entry).
  Status: open
  Last-reviewed: 2026-08-03
