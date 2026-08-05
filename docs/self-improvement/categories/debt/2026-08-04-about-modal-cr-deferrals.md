- 2026-08-04 · claude-code · [debt] · P3 — Two CodeRabbit Majors on the About dialog (PR #1937), deferred by user direction

  Details: CodeRabbit reviewed PR #1937 (Help > About dialog) and filed 10 actionable
  findings. The user directed "ignore cr" in-session, so the CR gate was waived via
  `cr-out-of-band` + a `cr-disposition:` PR-body marker. Eight findings were Minor or
  scope-declined; the two Major ones are recorded here so the waiver does not lose them.

  (1) **`GatherAboutInfo` runs synchronously on the render thread**
      (`Source/Core/src/Ui/SmatchetAboutUi.cpp:213`). CR asked for a future + loading
      state + teardown drain. Declined on the merits, not deferred-by-oversight: the
      call is a once-per-open snapshot (guarded by `aboutOpenLatch || !aboutInfo`), not
      per-frame, and `ConfigManager` is itself cached — nowhere near the 100 ms Pillar 2
      threshold. Recorded because the *reasoning* is what would rot: if `GatherAboutInfo`
      ever grows a network probe (an update check, a license fetch) the once-per-open
      argument stops holding and the async rewrite becomes correct.
      Trigger to revisit: any new I/O added to `diagnostics::GatherAboutInfo`.

  (2) **`tests/Core/AboutInfo.test.cpp` exercises disk-backed config reads**
      (lines 172, 177, 183, 200). Accepted as a real violation of the `tests/**` path
      instruction that doctest units must not touch on-disk I/O surfaces — the doctest
      lane is meant to be pure-logic (`test-rig` refuses SQLite/HTTP/ImGui surfaces for
      the same reason). It passes today because `GatherAboutInfo` degrades gracefully
      when the config path is absent, which makes it a latent flake on a machine with an
      unexpected config rather than a hard failure.

  Concrete next action: for (2), split the assertions — keep the pure `ParseDepManifest`
  / `BuildAboutReportText` coverage in the doctest driven by synthetic `AboutInfo`
  values, and move the real-`GatherAboutInfo` smoke assertion to the bats lane
  (`tests/bats/about_buildinfo.bats` already drives the CLI end-to-end via
  `app.version --json`, which is the right home for a config-touching check). For (1),
  no action unless the revisit trigger fires.

  Status: open
  Last-reviewed: 2026-08-04
