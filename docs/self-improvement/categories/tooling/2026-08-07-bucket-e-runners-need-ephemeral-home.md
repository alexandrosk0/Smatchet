- 2026-08-07 · claude-code · [tooling] · P2 — extract the ephemeral-`SMATCHET_USER_DATA` hardening into a shared helper; roughly three-quarters of the bucket-E runners still inherit the developer's real profile

  There are **30** `scripts/dev/test-ui-*.sh` runners. Only **7** point `SMATCHET_USER_DATA`
  at a `mktemp -d` (`data-dependent-windows`, `duration-inline-edit`, `funcsize-grid-render`,
  `grid-pane-windows`, `jira-deterministic-backend`, `linear-deterministic-backend`,
  `omnibar-search-apply`), and only **3** of those also seed the config JSON
  (`duration-inline-edit`, `grid-pane-windows`, `omnibar-search-apply`). So ~23 runners need
  the ephemeral home and ~27 need the seed.

  The exemplar to copy is [`scripts/dev/test-ui-grid-pane-windows.sh`](../../../../scripts/dev/test-ui-grid-pane-windows.sh)
  lines 38-47, which does both and already carries the rationale in a comment. The seed is:

  ```json
  {"read_only_mode": false, "whisper_setup_completed": true, "backend_has_been_reachable": true}
  ```

  Two independent reasons, both applying to every runner:

  - **Correctness.** A *fresh* profile shows the `##WhisperSetupBanner`, which floats over
    window headers and swallows clicks. A case that clicks anything in the top strip fails
    for a reason unrelated to the code under test — and, worse, *passes* on a developer
    machine whose real profile already dismissed the banner. That is a machine-dependent
    test, which is the failure mode bucket-E exists to avoid.
  - **Safety.** Without the override the run reads and writes the developer's real
    `%LOCALAPPDATA%/Smatchet/`, including `imgui.ini`. A docking test that ends mid-layout
    leaves the developer's actual window arrangement mangled.

  Proposed: extract the seed + `mktemp -d` + trap-cleanup into a sourced helper
  (`scripts/dev/lib/ui-test-home.sh`) and have every runner source it, so the hardening
  cannot drift back out one script at a time — which is what the 7-of-30 split shows has
  already happened.

  A `test-orphan-bats`-style gate would keep it honest: assert every `scripts/dev/test-ui-*.sh`
  either sources the helper or carries an explicit opt-out comment.

  Separately, `test-ui-window-expand.sh` on the [PR #1966](https://github.com/alexandrosk0/Smatchet/pull/1966)
  branch (not on `develop`, so not linkable from here) takes a **different** approach to the
  same problem — it exports `LOCALAPPDATA` / `APPDATA` / `XDG_CONFIG_HOME` to a `mktemp -d`
  rather than setting `SMATCHET_USER_DATA`, and seeds nothing. Worth reconciling into the one
  helper when that branch merges rather than letting two idioms coexist.
