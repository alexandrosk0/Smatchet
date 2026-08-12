# Reduce build / cppcheck / test invocations during multi-edit changes
<!-- plan-date: 2026-05-16 -->

## Context

Today every `Edit` / `Write` to a first-party `.cpp` / `.h` fires `.claude/hooks/lint-cpp.sh` which runs **four tools serially** with no batching, debounce, or dedup:

1. `clang-format -i` (~50 ms — must stay inline so the next edit reads the formatted file)
2. `cppcheck` (~1–3 s/file, no `--cppcheck-build-dir` cache)
3. `clang-tidy -p build/<preset>` (~2–4 s/file)
4. `python lint-syntax-both.py` (`clang-format -fsyntax-only` against Standalone + DX12, ~3–8 s)

A 5-edit logical change → **5 full pipeline runs** (≈25–75 s of mostly redundant work). The audit also confirmed there is **no `Stop` hook today** and the queue / dedup affordance simply doesn't exist.

Separately, agents currently invoke `cmake --build` and `scripts/dev/test-all.sh` at whatever cadence their prompt suggests — often per-file or per-fix during a multi-edit slice. The trivial-visual-only envelope (shipped today, 2026-05-16) is the only documented carve-out and it covers only palette / locale literals.

Goal: collapse N edits to ≤ M unique-file heavy passes (where M = distinct files edited in the turn, typically 1–3), AND give agents a clear rule that builds + tests only fire at slice boundaries.

User choices (confirmed via AskUserQuestion):

- **Aggressive deferral**: `clang-format` stays inline; `cppcheck` + `clang-tidy` + dual-target syntax all defer to a `Stop` hook firing on agent-turn end.
- **Both fixes**: ship the lint deferral AND add an AGENTS.md slice-boundary rule for builds + tests, with a `.claude/.tree-dirty` sentinel agents can consult.

Six follow-up adjustments locked after critical review:

1. **Stop-hook exit-2 validation spike** before Part 1 lands (de-risks the entire design).
2. **Per-PID queue files** to eliminate the parallel-subagent race.
3. **Drain runs on `Stop` only**, not `SubagentStop` (removes latency injection between subagent return and orchestrator response).
4. **`PreToolUse:Bash` clears `.tree-dirty`** when the command matches `cmake --build` — no agent-side adoption needed.
5. **Chunked drain** with a bounded per-invocation file count and re-queue of the remainder.
6. **Test-all.sh memo cache deferred** out of this plan; revisit when there's evidence of pain.

## Approach

### Part 0 — Validation spike (≈30 min, must pass before Part 1 lands)

Before refactoring anything, prove that Claude Code's `Stop` hook surfaces `exit 2` back to the agent the same way `PostToolUse` does. Without this guarantee, deferred lint findings would be visible only to the user — agents would shrug and reply anyway.

**Procedure:**

1. Drop a throwaway `.claude/hooks/_stop-spike.sh` that writes `_stop-spike: forced re-prompt` to stderr and `exit 2`.
2. Wire it to `Stop` in `.claude/settings.json` (no matcher, 3 s timeout).
3. Trigger an end-of-turn (have Claude finish a trivial reply).
4. Observe whether Claude Code reprompts the agent with the stderr text (success) or silently displays it to the user (failure).

**Branch points:**

- **Success** — proceed to Part 1 exactly as planned.
- **Failure** — abandon the `Stop`-hook approach. Fall back to `SubagentStop` only + a documented manual `lint-flush` step at orchestrator-turn end. Re-spec Part 1 against `SubagentStop`-only semantics; reopen the latency-vs-coverage tradeoff with the user before continuing.

Plan halts here on Failure. No production hook edits land until Success is confirmed.

### Part 1 — Split the lint hook into inline + drain phases

**Inline phase** (`PostToolUse` on `Edit|Write`, kept fast — target ≤ 200 ms p99):

- Same file filtering as today (Source_Core / Plugins / Target_Standalone / tests).
- Run `clang-format -i` only.
- Append the resolved path to **a per-PID queue file** `.claude/.lint-queue.$PPID` (parent PID = Claude Code's PID, stable for the session; one file per Claude Code instance / subagent). One absolute path per line; line-append is atomic under POSIX `PIPE_BUF` (4096 bytes) so concurrent appends from a single process serialise safely.
- Write `.claude/.tree-dirty` (empty marker file) for the build-cadence rule (Part 2).
- Exit `0` — silent in the common case. No `cppcheck` / `clang-tidy` / dual-target on this path.

**Drain phase** (new `lint-cpp-drain.sh`, runs on `Stop` only):

- Take a lockfile (`flock -n` on `.claude/.lint-queue.lock`) so concurrent Stop events don't tread on each other. If already locked, exit 0 — the other drain is already processing.
- **Collect**: glob `.claude/.lint-queue.*`, read all lines, dedup, keep only paths still on disk that match the first-party filter.
- **Chunk**: process up to **10 files per invocation** (configurable via `SMATCHET_LINT_DRAIN_CHUNK`, default 10). Any remainder is rewritten to a single `.claude/.lint-queue.$PPID` for the next Stop event to pick up. With ~5 s/file worst case, 10 files comfortably fit in the 120 s hook timeout.
- For each unique path in the chunk: run `cppcheck` + `clang-tidy` (same flags as current `lint-cpp.sh`) and, for `.cpp` outside `tests/`, run `lint-syntax-both.py`.
- Reuse the existing `format_issues` awk dedup + `SMATCHET_LINT_MAX_LINES` envelope so the surfaced report stays bounded.
- **Issue found**: write the consolidated report to stderr with header `lint-cpp: deferred-drain issues across N file(s); M file(s) remain queued`, exit `2` (surfaces to Claude, prompts the agent to fix before its next response).
- **Clean drain**: delete the consumed per-PID queue files, release the lock, exit `0`.
- Timeout: 120 s in `settings.json`.

**Why not `SubagentStop`?** Adding the drain to `SubagentStop` would inject up to 120 s of latency between subagent return and orchestrator response. Instead, subagent edits ride the orchestrator's `Stop` event — the queue is per-PID, but the drain globs across all queue files, so it picks up subagent entries too. The trade: subagent lint findings appear to the orchestrator at orchestrator-turn end, not when the subagent itself returns. This is acceptable because lint findings are advisory; the subagent's report is the source of truth for what the subagent did.

**Escape hatch**: env var `SMATCHET_LINT_INLINE=1` skips the queue and runs all four tools per-edit (the old behaviour). Useful when debugging the hook itself or when the user wants per-edit feedback.

**Manual flush**: `scripts/dev/lint-flush.sh` invokes the drain script against the current queue without needing a hook event. Agents can call this explicitly before reporting "done" if they want to surface lint state mid-turn.

**Session start**: extend `scripts/clear-session-context.sh` to `rm -f .claude/.lint-queue.* .claude/.lint-queue.lock` (orphaned entries from a crashed prior session are discarded — they would have been built or shipped or are stale anyway).

### Part 2 — AGENTS.md slice-boundary rule + auto-cleared `.tree-dirty` sentinel

Add a new section to `AGENTS.md` § Project rules:

> **Build / ctest cadence — slice-boundary only.** Within a single agent turn (= one logical slice), invoke `cmake --build` and `scripts/dev/test-all.sh` **at most once each**, and only after the implementation is complete. The hook-maintained `.claude/.tree-dirty` sentinel records "edits have happened since the last build" — agents reading it know the tree is mid-slice and should defer the build. Cleared automatically by **any** `cmake --build …` invocation via a `PreToolUse:Bash` hook that watches for that command prefix. The trivial-visual-only envelope already shipped (2026-05-16) is a special case of this rule.

**Adoption is automatic, not voluntary.** The `PreToolUse:Bash` hook means there is no opt-in — every existing `cmake --build` call site clears the sentinel without prompt edits. `scripts/dev/build.sh` is **dropped** from this plan; it was needed only as an adoption vehicle.

**Hook detail**:

- New `.claude/hooks/clear-tree-dirty.sh` (~5 lines): reads the JSON input, extracts `tool_input.command`, matches `^[[:space:]]*cmake[[:space:]]+--build` or `^[[:space:]]+.*[[:space:]]+cmake[[:space:]]+--build` (handles `MSYS2_PATH_TYPE=inherit cmake --build …` and friends), and on match `rm -f .claude/.tree-dirty`. Always exit 0 — never block the bash invocation.
- Wire it to `PreToolUse:Bash` in `.claude/settings.json` with a 2 s timeout.

**Per-agent doc bullets**: one one-liner each under § Common causes / § Workflow in `agents/build-doctor.md`, `agents/test-rig.md`, `agents/debug-detective.md`, `agents/perf-detective.md`, `agents/code-review.md` pointing at `.tree-dirty` + the slice-boundary rule. Bump each agent's `version:` integer per AGENTS.md § Agent versioning (workflow contract change).

### Part 3 — DROPPED

`scripts/dev/test-all.sh` memo cache is deferred. It was the largest new code surface for the smallest immediate win. Revisit when there's measured pain on `test-all.sh` wall-clock or when `git-janitor` sessions become slow enough to matter.

## Implementation log

- branch `feat/lint-hook-deferred-drain` — Stop-hook validation spike + full inline / drain split + slice-boundary rule + 5 agent docs + verification driver + plan migration. Single squash-commit on this branch covers Parts 0–2 and the dropped Part 3.

## Deviations from plan

- **Pre-existing PATH bug discovered and fixed** — `lint-syntax-both.py` was silently emitting empty-diagnostic `[syntax-check FAIL: …]` for every C++ edit since the cygpath fix landed (2026-05-15). Root cause: MSYS2 UCRT64 gcc 16.x's `cc1plus.exe` requires `C:\msys64\ucrt64\bin` on `PATH` to load its DLL deps; the hook environment inherits the user shell's PATH which on this host doesn't include the toolchain bin dir. Same root cause masked `cppcheck` and `clang-tidy` (both also live in the toolchain bin dir) via the hook's `command -v` skip. Fix: prepend the toolchain bin to `PATH` in `lint-cpp-common.sh` (covers the bash hooks) and inside `lint-syntax-both.py`'s subprocess env (covers the python invocation). Without this fix, deferred lint would have made the false-positive worse per-turn (once-per-Stop instead of once-per-edit).
- **`lint-cpp-common.sh` factored out** as a sourced shared library — not in the original plan as a separate file. Both `lint-cpp.sh` (inline) and `lint-cpp-drain.sh` (drain) source it for path normalisation, first-party filter, the three heavy tool wrappers (`lint_run_cppcheck`, `lint_run_clang_tidy`, `lint_run_dual_target`), and `lint_format_issues` (the awk dedup + line cap). Keeps the two hooks from drifting.
- **`.gitignore` not modified** — the plan called for adding `.claude/.lint-queue.*`, `.claude/.lint-queue.lock`, `.claude/.tree-dirty`. Existing `.gitignore` line 63 already covers the entire `.claude/` directory; these are redundant.
- **Canonical hook sources** ship under `docs/harness/claude-code/hooks/` alongside `lint-cpp.sh` / `vexp-guard.sh` / `lint-syntax-both.py`. `scripts/setup-harness.sh` extended to copy the new hooks (`lint-cpp-common.sh`, `lint-cpp-drain.sh`, `clear-tree-dirty.sh`) on `bash scripts/setup-harness.sh claude-code`. `docs/harness/claude-code/settings.json.tmpl` updated with the new `Stop` and `PreToolUse:Bash` matchers. Without this, a fresh clone wouldn't pick up the new pipeline.

## Verification

`bash scripts/dev/test-lint-hook-split.sh` ships 14 assertions across 7 of the 11 planned checks. All green at branch tip:

```
[lint-hook-split] Test 1 — inline hook produces queue + tree-dirty
  PASS  inline exit=0
  PASS  queue file appeared: .lint-queue.<pid>
  PASS  queue contains probe path
  PASS  .tree-dirty written
[lint-hook-split] Test 2 — multi-edit dedup
  PASS  queue has 3 lines pre-drain (one per inline call)
  PASS  drain consumed dedup'd queue
[lint-hook-split] Test 3 — multi-file drain
  PASS  queue has N lines pre-drain (one per file)
  PASS  drain consumed multi-file queue
[lint-hook-split] Test 7 — SMATCHET_LINT_INLINE=1 escape hatch skips the queue
  PASS  inline mode skipped the queue
[lint-hook-split] Test 8 — manual flush via scripts/dev/lint-flush.sh
  PASS  lint-flush drained the queue
[lint-hook-split] Test 9 — clear-tree-dirty.sh removes .tree-dirty on cmake --build
  PASS  .tree-dirty cleared on cmake --build invocation
  PASS  .tree-dirty preserved on unrelated Bash command
  PASS  .tree-dirty cleared with env-var prefix
[lint-hook-split] Test 11 — SessionStart clears orphaned queue / lock / tree-dirty
  PASS  SessionStart removed all orphan markers

Passed: 14  Failed: 0
```

Auto-enrolled via `scripts/dev/test-all.sh` (driver follows the `Passed: N  Failed: M` summary contract). Stop-hook exit-2 reprompt semantics validated live during Part 0 (spike emitted `Stop hook feedback: _stop-spike: forced re-prompt …` into a fresh agent turn; design proceeded).

**Deferred verification** (filed in `docs/backlog/AGENT_SELF_IMPROVEMENT.md`):

- Test 4 — issue surfacing with a real cppcheck violation. Requires fault-injection into a production .cpp; entangles test driver with real source state.
- Test 5 — chunked drain across > `SMATCHET_LINT_DRAIN_CHUNK` files. Requires synthesising 11+ distinct C++ files in the project; out of scope for the unit driver.
- Test 6 — parallel-subagent per-PID isolation. Requires staging live parallel subagent fixtures; out of scope for headless shell harness.
- Test 10 — lockfile serialises concurrent drains. Requires multi-process orchestration in shell.

**Live end-to-end check** (Part 0 validated mid-implementation):

- Inline hook call on `Source_Core/src/SmatchetTheme.cpp` — exits 0 in < 100 ms, queue + tree-dirty appear, no heavy tool invocation in process tree.
- Drain after one-file queue — ~3 s wall clock, exits 0, queue + tree-dirty consumed (toolchain bin on PATH after the pre-existing-bug fix; previously cc1plus crashed silently and dual-target tripped exit 2 on every drain).

## Residual risks (recorded for future revisits)

- **Lint feedback arrives once at end-of-turn.** Mitigation in place: drain exits 2, Claude Code reprompts via Stop-hook semantics (validated Part 0). Manual flush available via `scripts/dev/lint-flush.sh`.
- **Stop hook doesn't fire (Claude killed mid-reply).** Queue persists across sessions; SessionStart hook truncates on next start.
- **Drain finds an issue in a file edited by a since-completed subagent** — orchestrator gets the reprompt, must fix or re-delegate. Documented in AGENTS.md slice-boundary rule.
- **`.tree-dirty` sentinel is advisory** — agents that ignore it still build. Auto-clear via PreToolUse:Bash means it stays accurate regardless.
- **Chunk size 10 is a guess.** `SMATCHET_LINT_DRAIN_CHUNK` env var allows local tuning; default re-evaluated from telemetry after first month.
- **PowerShell users have no native parity.** `scripts/dev/lint-flush.sh` is bash-only; PS users invoke via `bash scripts/dev/lint-flush.sh`. Add `.ps1` sibling if demand surfaces.
- **5-agent version-bump cluster** generates telemetry noise. Noted in the backlog entry.
