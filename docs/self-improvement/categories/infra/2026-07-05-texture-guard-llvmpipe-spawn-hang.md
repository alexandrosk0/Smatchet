- 2026-07-05 · claude-code · [infra] · P2 — mobile-texture-guard `--spawn` scenario hangs under llvmpipe (CI lane held advisory)

  Details: the `Mobile texture-guard smoke (Mesa headless GL)` CI job runs
  `./Smatchet.exe scenario.run --name=mobile-texture-guard --frames=8 --spawn --yes`
  under `GALLIUM_DRIVER=llvmpipe` on `windows-2022`. On a large fraction of runs the
  spawned child **hangs** — `timeout 240`/`300` fires with `rc=124` and no JSON
  envelope is ever emitted (parse-miss). Pre-flip conclusion window: ~3 success /
  ~half failing-or-cancelled over 13 develop pushes; RED on the #1619 and #1620 PR
  heads and the first post-flip develop push. Retry-on-flake does NOT help: all 3
  attempts hung the full inner timeout on the reproducing run, so it is a
  deterministic-ish deadlock, not a transient CI flake.

  The all-gates-blocking flip (2026-07-05) tried to graduate this lane to blocking;
  the reliability data forced it back to **advisory** — the one PR-lane user of the
  merge-gate poller's `advisory`-name escape (`Mobile texture-guard smoke (Mesa
  headless GL, advisory)` + a step-level `continue-on-error`). Every OTHER unmasked
  Mesa lane (bucket-C, bucket-E ×2, launch-smoke) verified reliable (zero real
  failures over the same window — they carry retry-on-collapse + a lane-integrity
  sentinel that fail-closes on a missing sentinel; texture-guard has neither).

  Suspected cause (unconfirmed — needs debug-detective): the forced-fault render
  path (`SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION=ON`, the 3 #1122 fault states —
  atlas-grow orphan / EGL-context-loss re-arm / rearm-disabled) deadlocks or
  live-locks under the software-GL (llvmpipe) backend on Windows, so the scenario's
  OnFinish JSON never lands and `--spawn` never returns. The real EGL/GLES path on
  device does not exhibit this (the guard shipped + is exercised elsewhere).

  Concrete next action:
  1. Reproduce locally under Mesa software-GL (the `install-mesa-gl` composite +
     `GALLIUM_DRIVER=llvmpipe`) against a `ninja-ui-test-msvc` build; capture the
     hung child's stack (the job uploads `mobile-texture-guard-exe-*` on the masked
     step's failure for exactly this).
  2. Fix the deadlock (or add a scenario-side hard timeout that emits a `failed`
     envelope instead of hanging, so the lane can fail-fast rather than time out).
  3. Give the lane deterministic teeth (a launch-smoke pre-step or a
     lane-integrity/no-envelope sentinel like bucket-C/E) so a hang reds a
     harness-alive signal without a full-suite false-green.
  4. Re-graduate to blocking: drop the step mask + the ", advisory" name suffix once
     the lane is green N consecutive runs; update AGENTS.md § Merge gates +
     docs/agent-rules/merge-gates.md (the advisory-token user list) accordingly.

  Cross-ref: `docs/plans/shipped/all-gates-blocking.md` § Deviations; the sibling
  reliable Mesa lanes' retry-on-collapse discipline in `.github/workflows/build-and-test.yml`
  (`bucket-e-ui-tests`, `bucket-c-screenshot-diff`).
