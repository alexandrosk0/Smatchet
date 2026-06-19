- 2026-06-19 · orchestrator · [tooling] · P2 — concurrency-correctness fixes have no headless doctest home, so tests-out-of-band waves them; add seam lints + a native/TSan leg
  Details: PR #1390 (g_ui UI-thread marshalling in BuiltinCommands_Debug.cpp) and #1409
    (std::atomic audit of AnthropicClient/OllamaClient/OpenAiClient + a new Windows-on-ARM
    native test leg) both shipped behaviour-changing concurrency-correctness fixes to product
    .cpp with zero tests/Core/*.test.cpp delta. The Test-delta gate passed only because the
    tests-out-of-band label waved it — the deterministic load-bearing condition in
    postmortem-owed.sh override_is_moot (Test-delta == SUCCESS AND pr_touches_test_files) is
    FALSE for both (gate green, no .test.cpp). Root reason there is no test: the correctness
    invariant is a threading property — "the g_ui request-flag write executes on the UI thread,
    not the dispatching MCP/Lua worker thread" / "the shared cross-thread flag is std::atomic" —
    that the headless, single-threaded pure-logic doctest rig cannot assert (no UI thread, no
    g_ui, no second thread, no AppController command-queue marshalling). This is a recurring
    class, not a one-off: see postmortems.md 2026-06-19.
  Concrete next action: two static/structural gates that catch the class without relying on the
    rig that can't host it —
    (1) extend test-lint-rules.sh with a strict-zone rule forbidding direct writes to the g_ui
    request-flag fields (requestWindowResize/requestWindowWidth/requestWindowHeight/
    requestScreenshot/requestScreenshotPath) from command-dispatch TUs (Source/Core/src/Commands/**)
    outside a RunOnUiThread* closure — green on HEAD (debug.dock.*, bug.report already conform),
    fires only on a new off-thread-write regression (the #1390 pre-fix shape). Est ~0.5d.
    (2) extend the Windows-on-ARM native test leg #1409 added (or the in-flight
    feat/tsan-subset-sync-layer TSan subset) to exercise the AI-client request paths so a
    non-atomic shared-flag regression surfaces at runtime, plus a lint flagging plain
    (non-std::atomic) shared mutable cross-thread flags in the AI-client TUs. Est ~1-2d.
  Cross-ref: postmortems.md 2026-06-19 PR #1390, #1409; commits b546e125 (#1390), ad0b34a1 (#1409);
    branch feat/tsan-subset-sync-layer; prior tests-out-of-band residue #1317 / #1308 are distinct
    (behaviour-preserving relocations, owe nothing).
  Status: open
  Last-reviewed: 2026-06-19
