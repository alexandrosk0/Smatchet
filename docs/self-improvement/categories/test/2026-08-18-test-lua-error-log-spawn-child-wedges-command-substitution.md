- 2026-08-18 · claude-code · [test] · P2 — `scripts/dev/test-lua-error-log.sh` wedges `test-all.sh` **forever**: the `--spawn` ephemeral child inherits the command-substitution pipe, so `$(...)` never returns after the parent CLI exits

  Hit twice on one machine, on two branches, in two worktrees, at the same time:

  | run | script PID / start | orphaned child | wedged for |
  |---|---|---|---|
  | `fix-four-issue-d17ba9` (`claude/fix-2110-…`) | 2183048 @ 13:58:24 | `Smatchet.exe --ephemeral --mcp-port 57444` PID 35540 @ 13:58:26, parent 41888 already gone | ~45 min, until killed by hand |
  | `optimistic-carson-c7a325` (different branch) | 1309386 @ 23:20:06 | `Smatchet.exe --ephemeral --mcp-port 50502` PID 51676 @ 23:20:08, parent 50776 gone | **~15 h**, still wedged |

  The two-second child-follows-script signature is identical in both, which rules out any one
  branch's diff as the cause.

  **Mechanism** (not a guess — killing the orphan released the wedge instantly, and the suite
  advanced to the bats phase within seconds). `run_test()` is
  `OUT=$(…"$EXE" cmd debug.lua_log_test --spawn --yes 2>&1)`. Command substitution reads the pipe
  until **every** write end closes. `--spawn` (`Source/Standalone/CliSpawn.cpp:306`) launches
  `"<exe>" --ephemeral --mcp-port <port>` with the parent's handles inherited, so the ephemeral
  child holds a write end too. When the child fails to exit — the `--spawn`-hangs-on-~half-of-runs
  flake the root `AGENTS.md` already documents for the advisory mobile lane — the parent CLI can
  exit normally and bash *still* blocks, with no timeout and no output, indefinitely. `set -euo
  pipefail` cannot see it; the script's own zero-run floor never runs.

  This is fail-open shape **H** (hang), the sibling of the zero-run floor the script already
  guards: a suite that never finishes is indistinguishable from a slow one, and on a shared dev
  box it silently holds a worktree hostage overnight.

  **Fix shape** — two independent teeth, both cheap:
  1. Wrap each `run_test` in `timeout` (e.g. `timeout 60 "$EXE" cmd …`) so a stuck child fails the
     assertion instead of the suite, and make a timeout a `FAILED` row, not a silent skip.
  2. Redirect the spawn's inherited stdout so the grandchild cannot hold the substitution pipe —
     `… --spawn --yes 2>&1 </dev/null` plus capturing via a temp file, or have `CliSpawn.cpp` not
     inherit stdout/stderr handles for the ephemeral child at all (the CLI parent already relays
     the result over MCP, so the child's console output is not what the script reads).

  Tooth 2 is the real fix; tooth 1 keeps the harness honest either way. The same inheritance
  applies to every `--spawn` caller, so the audit is worth running past
  `grep -rn -- '--spawn' scripts/ tests/`.
