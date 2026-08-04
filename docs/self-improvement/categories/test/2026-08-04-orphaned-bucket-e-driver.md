- 2026-08-04 · orchestrator · [test] · P2 — `scripts/dev/test-ui-agent-proposal-store-sqlite.sh` is an **orphaned bucket-E driver**: no test TU named `AgentProposalStore` exists anywhere in `tests/`, so the driver can only ever fail (`ui_test.run matched 0 tests for filter 'AgentProposalStore'`) — and it goes unnoticed because `test-all.sh` **SKIPs** it on any tree without a built `Smatchet.exe`, which is the common local state
  Details: Surfaced running the full `scripts/dev/test-all.sh` twice for slice 2 of
    `dev-onboarding-first-run-quickstart` — once on a worktree with a built `ninja-ui-test-msvc`
    exe (the driver ran and FAILed) and once on a worktree with no build (the driver SKIPped and
    the suite looked cleaner). That skew is the real lesson: the same commit produces a different
    failure count depending on whether a build directory happens to exist, so a genuinely broken
    driver reads as "environment noise" rather than breakage. `grep -rn AgentProposalStore tests/`
    returns nothing — the only hits are inside the driver itself (its header comment at :3 crediting
    "slice 9" and its `FILTER="${UI_TEST_FILTER:-AgentProposalStore}"` default at :12). The driver's
    zero-match fail-closed behaviour is itself correct and was added deliberately by #1192
    (the 6-HIGH fail-open gate cluster fix) — it is doing its job; what is missing is the TU it was
    written to drive, which either never landed or was removed without removing its driver.
  Concrete next action: decide the disposition and act on it in one PR. Either (a) restore the
    missing `tests/ui/agent_proposal_store.test.cpp` + its registry entry in
    `tests/ui/ui_tests_registry.cpp` if the SQLite-backed proposal-store lifecycle is still meant to
    be covered, or (b) delete the driver. Then add the *class* gate so this cannot recur silently: a
    check that every `scripts/dev/test-ui-*.sh` driver's `UI_TEST_FILTER` default resolves to at
    least one registered test name — cheap as a bats case that greps each driver's default filter and
    asserts a matching `SmatchetRegister*Tests` / `IM_REGISTER_TEST` name exists in `tests/ui/`. That
    runs with no build and no exe, which is exactly the gap that let this sit. Est ~0.5d.
  Cross-ref: `scripts/dev/test-ui-agent-proposal-store-sqlite.sh` (the orphan);
    `tests/ui/ui_tests_registry.cpp` (where the registration would live);
    PR #1192 (added the zero-match fail-closed behaviour that makes the orphan visible);
    `scripts/dev/test-all.sh` (the SKIP-when-no-exe path that hides it).
  Status: open
  Last-reviewed: 2026-08-04
