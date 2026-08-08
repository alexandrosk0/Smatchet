- 2026-08-05 · claude-code · [tooling] · P2 — The Stop-hook dual-target syntax check has been running vacuously: without a `ninja-iter-clang` build dir it falls back to `cl.exe` with no VS Developer shell, fatals at the first system header, and never reaches first-party code

  Observed while shipping the bucket-C screenshot determinism fixes. The lint drain
  reported `[syntax-check FAIL: SmatchetCore_DX12] / ConfigManager_Load.cpp` with an
  empty diagnostic body. An untouched control file (`SmatchetStatusBarUi.cpp`) failed
  identically, so it was not the diff. The authoritative
  `cmake --build build/ninja-iter-msvc` was clean throughout.

  Cause chain in `.claude/hooks/lint-syntax-both.py`:

  1. The script prefers `build/ninja-iter-clang/compile_commands.json` precisely
     because clang-cl knows its own system headers and supports `-fsyntax-only`
     natively. That preset is not configured on this host, so it falls back to
     `build/ninja-iter-msvc` and invokes `cl.exe`.
  2. Hook processes do not run under a VS Developer Environment, so `cl.exe` cannot
     resolve standard headers. Compilation aborts at the first one —
     `AppController.h(5): fatal error C1083: Cannot open include file: 'limits'` —
     before any first-party token is parsed.
  3. `<limits>` was missing from the `_FP_PATTERNS` system-header allow-list, so that
     line was reported as a real diagnostic. Even had it been filtered, `cl.exe`
     echoes the TU's bare filename on its own line and nothing filtered that, so a
     fully-false-positive run still produced a body-less `FAIL` and exit 2.

  The important part is not the noise, it is the **silence**: appending
  `static void zzz_probe() { undeclared_symbol_probe(); }` to a real TU and running
  the hook returned **rc=0**. The compile dies on the missing header long before the
  undeclared identifier, so the gate cannot fail on first-party code at all. A gate
  that reports FAIL on every file and PASS on a genuine error is worse than absent —
  it trains operators to dismiss its output, and the dismissal is correct, which is
  how a real dual-target divergence would slip through.

  Partially addressed in the same PR (noise only, deliberately not the root cause):
  added `limits`, `ctime`, `iomanip`, `initializer_list`, `exception`, `new`,
  `typeinfo`, `condition_variable`, `future`, `forward_list`, `stack`, `queue`,
  `ratio`, `system_error` to the allow-list, and filtered `cl.exe`'s bare-filename
  banner line (comparing against `src.name`). `--selftest` extended with the
  `<limits>` case; it still classifies the three real-error lines as real. This stops
  the false alarm on every C++ edit. **It restores no detection** — there was none in
  this environment to restore.

  Proposed fix, in preference order:

  1. Configure the `ninja-iter-clang` preset as part of harness setup
     (`docs/harness/SETUP.md` / `scripts/dev/`), so the script's existing preferred
     path is actually available. This is the design the script was written for.
  2. Failing that, invoke the fallback `cl.exe` command through
     `scripts/dev/with-msvc-env.sh` so the Developer Environment is present.

  Either way the hook should **fail loudly when it cannot check anything** rather than
  degrade to a silent pass: if every compile in the run aborted on a system-header
  C1083, that is "check did not run", not "check passed". Emit a distinct one-line
  WARN naming the missing preset and the fix, instead of exit 0.

  Concrete next action: add a `ninja-iter-clang` configure step to the harness setup
  path, then add a self-check to `lint-syntax-both.py` that distinguishes
  "no first-party diagnostics" from "compilation never reached first-party code"
  (e.g. all failures matched only FP patterns + banner ⇒ WARN "syntax check
  inconclusive, configure ninja-iter-clang"). Pin the injected-error case in a test so
  the gate's ability to fail is itself gated.

  Status: open
  Last-reviewed: 2026-08-05
