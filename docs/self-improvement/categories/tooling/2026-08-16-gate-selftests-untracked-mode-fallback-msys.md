- 2026-08-16 · orchestrator · [tooling] · P2 — `test-gate-selftests.sh --selftest` fails all 11 negative fixtures on Windows/msys because its untracked-file mode fallback is `[ -x "$f" ]`, which msys answers TRUE for every temp file — so `scripts/dev/pre-ship.sh` cannot go green on a Windows dev box and its red becomes background noise
  Details: the raw-self-exec rule in
    [`test-gate-selftests.sh`](../../../../agents/scripts/core/test-gate-selftests.sh)
    only applies to mode-100644 scripts (the "126 Permission denied" premise does
    not hold on a `+x` file). It reads the mode from the git index and falls back
    to the filesystem bit for untracked files — and the synthetic selftest
    fixtures are exactly that: untracked temp files. On NTFS under msys there is
    no meaningful exec bit; a freshly `printf`-written temp file reports
    `-rwxr-xr-x` and `[ -x ]` is TRUE. Every fixture therefore resolves to
    `_nonexec=0`, the detection chain short-circuits before the awk lexer runs,
    and each negative reports `raw self-exec … was NOT flagged`, `rc=1`.
    Verified platform, not regression: the script is byte-identical to
    `origin/develop` (`git show origin/develop:… | diff -q` → identical),
    `origin/develop` is green in CI (Linux, where the fs bit is real), and the
    detection pieces work standalone here — GNU Awk 5.3.2 matches
    `SELF_EXEC_RE` against a fixture by hand. Only the mode gate misfires.
    Damage is scoped to the SELFTEST, not the gate: real first-party scripts are
    tracked, so they take the `git ls-files --stage` path and classify correctly
    on every platform. The cost is that `pre-ship.sh` — the documented "run all
    gates locally before every push" entry point — is permanently red on Windows,
    so a genuine finding sitting next to it gets skipped. Hit while shipping
    `branch-protection-config-completeness`, where it sat alongside a real
    (separate, local-only) `test-agent-contract` drift that was easy to miss
    behind the standing red.
  Concrete next action: make the fixture's mode explicit instead of inferring it
    from the filesystem. Cheapest fix is at the fixture site — `chmod a-x` each
    synthetic fixture right after it is written, so the fallback is never
    consulted; one line per fixture, production path untouched. Belt-and-braces
    alternative: have the selftest export a mode override the classifier honours
    (e.g. `_GATE_SELFTEST_FORCE_NONEXEC=1`), which also documents that the
    fixtures are deliberately mode-100644. Either way add a bats case asserting an
    untracked, non-executable fixture IS flagged, so the platform divergence
    cannot regress silently. Est ~1h.
  Status: open
  Last-reviewed: 2026-08-16
