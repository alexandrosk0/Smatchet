# Plan — spawn MCP auth handshake unblock + product fix

**Slug:** `spawn-mcp-auth`
**Owner:** orchestrator (Config + MCP + command-system)
**Status:** active
**Created:** 2026-06-27

## Problem

PR #1566 (`32392e32`, "fix(security): C++ security hardening", merged
2026-06-27 09:47Z — Slice 5 of [`cpp-security-hardening.md`](cpp-security-hardening.md))
flipped `McpRequireTokenOnLoopback` default `false → true`.

This breaks the `--spawn` MCP handshake. The spawn **parent**'s
`WaitForMcpReady` probe (`CliCommandRunner.cpp`) POSTs a **tokenless** loopback
`/mcp/tools/call`; the **child**'s MCP server (`McpPlugin.cpp`) now returns
**HTTP 401** before reading the token header; the probe accepts only 2xx → polls
to its 30 s timeout → "ephemeral instance did not become reachable in time" → the
CI job idles to its `timeout-minutes` and is cancelled.

CI builds the `pull_request` **merge ref** (head + develop), so every PR's
`--spawn` gates inherited the broken default the moment #1566 landed on develop.

### Second root cause — PathConfinement rejects the perf harness's absolute `--outPath`
PR #1566 also added `Source/Core/include/Commands/PathConfinement.h` and wired it
into `ScenarioRunner.cpp`: a caller-supplied `scenario.run --outPath` is now
confined under `<userDataDir>/perf/` via `ConfinePathUnderSubdir`, which
**rejects absolute paths and `..` traversal** (candidate MUST be relative).
`scripts/dev/perf-run.sh` (the Perf PR-fast / perf-full driver) passes an
**absolute** `build/perf-runs/<scen>-<ts>.json` as `--outPath`, *and* the spawn
parent's `NormalizeOutPath` absolutizes even a relative path before sending it
to the child — so the child rejects it with `ValidationError` →
**kExitValidation(3)**. This is a *separate* break from the token-401: the PR A
env override fixed the handshake (the failure mode changed from
CANCELLED/timeout to **exit=3**, proving the probe now reaches the child), but
the perf gate still red until the path contract is repaired.

The spawn parent already re-emits the confined result file's full content in the
stdout envelope's `data` field (`CliCommandRunner::SpawnAndRunHandleAsync` reads
the **child-reported** confined `data.outPath`, waits on it, then re-emits the
content). So the security-preserving harness fix is: perf-run.sh drops
`--outPath`, captures the spawn envelope, and writes `data` to its own
`$ABS_OUT` — **no confinement bypass**, the secure default stays intact.

### Affected gates (verified `gh pr checks 1571`)
All `--spawn` gates fail: **Perf PR-fast** (blocks — meant-to-block allow-list),
Bucket-C screenshot diff, Bucket-E UI tests, Bucket-E Jira fixture, Bucket
launch-smoke Mesa GL. Non-spawn checks (Windows+MSVC, Coverage, Sanitizer,
Android security gate, lint) PASS. Bucket-C/E are masked (dropped from
meant-to-block 2026-06-15); Perf PR-fast and launch-smoke are not.

### A/B proof (debug-detective, develop tip)
Flag `false` → handshake OK, idle healthy (`SmatchetUI::Draw` avg 0.4753 ms).
Flag `true` (new default) → exact CI timeout. The earlier #1564 (color-only
theme) bisection was a confirmed false positive.

## Approach — two PRs

### PR A — quick-unblock (this branch, `fix/spawn-mcp-auth`)
Add a `SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK` env override to `ConfigManager`
(mirrors the existing `SMATCHET_MCP_ALLOW_REMOTE` security escape-hatch), and set
it `"false"` at workflow-level env in the three `--spawn` workflows
(`perf-pr-fast.yml`, `perf-full.yml`, `build-and-test.yml`). The `--spawn` child
inherits step env via `CreateProcessA` (proven by `GALLIUM_DRIVER` /
`SMATCHET_RENDERER`). Single-tenant throwaway CI runners → the co-resident-process
risk the default guards against does not apply. **Keeps the secure default ON for
products and dev machines** — only CI opts out, explicitly.

PR A also repairs the **second root cause** (PathConfinement, above) with a
**harness-only** change to `scripts/dev/perf-run.sh`: it no longer passes
`--outPath` to `scenario.run --spawn`; instead it captures the spawn parent's
stdout envelope and writes the envelope's `data` block (the bare scenario
result, byte-identical to the legacy file format) to its own `$ABS_OUT`. The
product confinement is untouched — no bypass env, no relaxation of
`ConfinePathUnderSubdir`. `perf-compare.py` and the existing rows-validation
guard read the same file format as before.

### PR B — product fix (follow-up)
Make the spawn handshake work **under the secure default** with no env opt-out:
- parent auto-provisions a per-spawn token + sends `X-Smatchet-Token`
  (`CliCommandRunner.cpp`, command-system);
- child accepts the sanctioned ephemeral token (`McpPlugin.cpp`, mcp-toolsmith);
- probe **fast-fails** on 401 with a named `spawn-unreachable` error (no more
  30 s poll-to-timeout masking the real cause);
- CLI/integration regression test asserting `--spawn` reaches MCP-ready under
  **default** config — the test that would have caught #1566.
Once PR B lands, the CI env opt-out from PR A can be removed.

## Perf-gate

PR A touches `Source/Core/src/Config/ConfigManager.cpp` (strict zone).
**Perf-inert:** the change is one additional `std::getenv` + string compare
inside `ApplyOverridesAndClamps`, which runs **once** per `ConfigManager::Load()`
(config load, not a per-frame / per-command hot path). No UI-thread, render-loop,
or grid/scroll path touched. No new allocation in steady state. No perf scenario
delta expected; the PR's own Perf PR-fast run is the self-validating gate (it
must go green — that is the whole point of the fix).

The PathConfinement follow-on touches **`scripts/dev/perf-run.sh` only** (CI
harness, not `Source/Core/` product code) — perf-inert by construction: it
changes how the driver *captures* the scenario result (stdout envelope vs file),
not what the scenario measures. The measured numbers come from the same
`scenario.run` execution; only the transport of the result JSON changed.

**PR B** touches **`Source/Plugins/Mcp/McpPlugin.cpp`** + **`Source/Standalone/CliCommandRunner.cpp`**
— neither is `Source/Core/`, so the mandatory-Perf-gate trigger does not fire.
Perf-inert regardless: the child change is one `std::getenv` + string compare in
`McpPlugin::OnStart` (server boot, once per process, not a per-frame/per-command
path); the parent change is token generation + an HTTP header on the `--spawn`
CLI one-shot flow (never on the UI/render thread). No steady-state allocation,
no grid/scroll/draw path. No perf scenario delta expected.

## Files to modify

| File | Change | PR |
|---|---|---|
| `Source/Core/src/Config/ConfigManager.cpp` | env override parse for `SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK` | A |
| `tests/Core/ConfigManager.test.cpp` | TEST_CASE for the new override | A |
| `.github/workflows/perf-pr-fast.yml` | workflow env `…=false` | A |
| `.github/workflows/perf-full.yml` | workflow env `…=false` | A |
| `.github/workflows/build-and-test.yml` | workflow env `…=false` | A |
| `scripts/dev/perf-run.sh` | drop `--outPath`; capture spawn envelope `data` → `$ABS_OUT` (PathConfinement-safe) | A |
| `Source/Standalone/CliCommandRunner.cpp` | parent mints+sends ephemeral token when no operator token configured, else sends the configured token (no 401 for configured-token users); fast-fail 401 | B |
| `Source/Plugins/Mcp/McpPlugin.cpp` | accept sanctioned ephemeral token; scrub it from the child env after adoption | B |
| `tests/…` (CLI/integration) | `--spawn` reaches MCP-ready under default config | B |
| `.github/workflows/{build-and-test,perf-pr-fast,perf-full}.yml` | **remove** the `SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK=false` opt-out (replace with a NOTE) — every `--spawn` CI gate now runs under the secure default on PR B's handshake | C |
| `scripts/dev/test-spawn-mcp-default-auth.sh`, `tests/Core/ConfigManager.test.cpp` | refresh comments that claimed CI exports the opt-out (now removed); defensive env-unset kept | C |
| `Source/Standalone/CliCommandRunner.cpp` | thread confined `--outPath` contract for all `--spawn` callers (or stop `NormalizeOutPath` absolutizing now the parent reads `data.outPath`); + optional hardening (`lpEnvironment` block, CSPRNG, scrub-at-entry) | D |
| `Source/Core/include/Commands/Scenarios/SpawnOutLogBasename.h`, `Source/Core/src/Commands/Scenarios/SpawnOutLogBasename.cpp` | new Core helper `MakeConfineSafeSpawnOutLogBasename` — confine-safe `--spawn` outLog basename (leaf strip + entropy prefix), the tested trust boundary | E |
| `Source/Standalone/CliCommandRunner.cpp` | `SwapOutLogForConfineSafeBasename` calls the Core helper; delete the inline ms-stamp + leaf logic (CR #2 ms-collision fix via entropy) | E |
| `Source/Core/src/Commands/Scenarios/UiTestScenario.cpp` | drop the temporal `#1566` ref from the durable outLog comment (CR #1) | E |
| `tests/Core/SpawnOutLogBasename.test.cpp`, `tests/CMakeLists.txt` | doctest for the helper (traversal collapse, absolute strip, fallback, prefix, entropy-uniqueness) | E |

## Implementation log

- **2026-06-27 — PR A** — added the env override + test + 3 workflow env entries.
  Lint PASS (one soft WARN: `ApplyOverridesAndClamps` 21>20 branches — soft tier,
  under the 30 hard cap; flat env-override list, refactor declined as scope creep).
- **2026-06-27 — PR A (PathConfinement follow-on)** — first Perf PR-fast run on
  the branch surfaced the **second** #1566 break: `scenario.run --spawn` exited
  **3 (kExitValidation)** because PathConfinement rejects perf-run.sh's absolute
  `--outPath`. Fixed harness-only in `scripts/dev/perf-run.sh` — drop `--outPath`,
  capture the spawn parent's stdout envelope, write `data` → `$ABS_OUT`. Verified
  the parent surfaces the confined path: `SpawnAndRunHandleAsync` reads
  `envData["outPath"]` (child-reported) and re-emits its content as `data`
  (`resultEnv["data"] = fileData`); child stdout/stderr are redirected to the
  spawn log file (POSIX `dup2`, Win `STARTF_USESTDHANDLES`), so the parent's
  stdout is exactly the one envelope line. Also fixed the **Windows+MSVC**
  required check: the new TEST_CASE asserted the compiled default ON, but the CI
  job exports `SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK=false` job-wide — added an
  explicit env-clear before the default-ON assertion (the case self-cleans via
  its final Cleanup block; TestEnvGuard does not touch this env var).
- **2026-06-27 — PR A (CodeRabbit round 1)** — addressed all 3 CR findings on
  #1574. (#1) dropped the `#1566` PR-number breadcrumb from the ConfigManager
  comment (strict-zone: durable present-tense intent, no temporal PR refs).
  (#2) made the env parse **fail-closed**: only explicit `false`/`0` disables;
  `true`/`1` enables; an unset/empty/malformed value preserves the secure
  default + `LOG_WARN`s the ignored value. (#3) corrected the test comment that
  falsely claimed `TestEnvGuard` restores the var. Added a `garbage`→default-true
  assertion locking fail-closed.
- **2026-06-27 — PR B (product fix, branch `feat/spawn-mcp-token`)** — spawn
  parent (`CliCommandRunner.cpp`) now provisions a per-spawn 128-bit token via
  `std::random_device` (`SpawnAuthToken()`), injects it into the child env as
  `SMATCHET_MCP_SPAWN_TOKEN` immediately before `CreateProcessA`/`execv` (Windows
  branch clears it from the parent env unconditionally right after spawn; POSIX
  sets it only in the forked child so the parent env is never touched), and sends
  it as `X-Smatchet-Token` on every request (`set_default_headers` on the probe,
  dispatch, async/sync result handlers, and `app.quit` teardown). Child
  (`McpPlugin.cpp::OnStart`) adopts the env token as `auth_token` **only when
  none is configured** — strengthens the secure-default child (empty→requires
  this exact token), never overrides an operator token, never weakens the gate.
  `WaitForMcpReady` now returns `enum McpReadyStatus {Ready,Timeout,AuthRejected}`
  and **fast-fails on HTTP 401** instead of polling to the 30 s timeout
  (a 401 on `/mcp/tools/call` unambiguously means token-rejected — Host/Origin
  failures return 403). New hermetic regression test
  `scripts/dev/test-spawn-mcp-default-auth.sh` (the test #1566 lacked): fresh
  `SMATCHET_USER_DATA`, unsets the PR-A env opt-out, asserts `--spawn app.version`
  returns `ok:true` under the compiled secure default. Build clean (596/596),
  lint PASS, security-review verdict **SHIP** (no CRITICAL/HIGH; two optional LOW
  hardening notes — see Deviations).
- **2026-06-27 — PR B (CodeRabbit round 1, #1576)** — addressed all 5 CR findings.
  (#4, Major correctness) **configured-token users no longer 401**: the parent now
  `ConfigManager::Load()`s and, when `McpAuthToken` is set, sends THAT token and
  injects nothing — `SpawnAndRunSetup` split into `provisionToken` (child env) vs
  `requestToken` (wire). Empty-config still mints + injects + sends the ephemeral
  token. (#2, Major security) child now scrubs `SMATCHET_MCP_SPAWN_TOKEN` from its
  env right after adoption (`_putenv_s(…,"")` / `::unsetenv`) so no subprocess can
  inherit it — actions LOW note #2 below. (#3, DRY) extracted `RandomHexToken(int
  draws)`; `SpawnLogRandomToken`=2 draws, `SpawnAuthToken`=4 — no second copy of
  the random-device/hex pattern. (#5, style) the new `env["error"]` builds
  member-by-member, no nlohmann brace-list reassign. (#1, doc) this plan's
  Files-to-modify table + PR-A note now agree the `--outPath` threading is PR C
  *(later re-split to PR D as the H1 sub-fix — see the split note in Deviations)*.
- **2026-06-28 — PR C (branch `feat/spawn-mcp-prc`)** — removed the workflow-wide
  `SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK=false` opt-out from all three `--spawn`
  workflows (`build-and-test.yml`, `perf-pr-fast.yml`, `perf-full.yml`), replacing
  each with a NOTE that records the removal + names PR B (#1576) as the handshake
  the gates now rely on. This is the **first** time the real `--spawn` CI gates
  (bucket-C/E, Perf PR-fast `scenario.run`, launch-smoke, dx12-smoke,
  texture-guard) run under the **secure default** — PR B's CI exercised the secure
  default only via the dedicated `test-spawn-mcp-default-auth.sh`; the gates
  themselves still inherited the opt-out. The PR's own green CI is the
  self-validating proof + the standing #1566 regression guard. Refreshed two stale
  comments (`test-spawn-mcp-default-auth.sh`, `tests/Core/ConfigManager.test.cpp`)
  that claimed CI exports the opt-out; kept the defensive env-unset/-clear against a
  developer-local override. No product code changed (CI-config + comments only).
- **2026-06-28 — PR E (#1581, branch `fix-spawn-outlog-confinement`, Fixes #1579)** —
  the `--spawn` outLog confinement-relocation fix (parent forwards a confine-safe
  basename, child writes under `<userData>/ui-tests/`, parent copies back to the
  caller's absolute target). This slice **extracts the confine-safe basename
  derivation into a tested Core helper** and clears the two CodeRabbit findings:
  * New pure Core helper `smatchet::cmd::MakeConfineSafeSpawnOutLogBasename`
    (`Source/Core/{include,src}/Commands/Scenarios/SpawnOutLogBasename.{h,cpp}`) —
    strips every directory component + `..` to the leaf (filename()), falls back to
    `outlog.txt` for an empty/`.`/`..` leaf, and prefixes `spawn-<entropy>-` where
    `<entropy>` is 16 hex chars (64 bits) from `std::random_device`. Result is
    guaranteed to contain no path separators and no `..` (confine-safe). No I/O, no
    logging; one random draw per spawn (not steady-state). This puts the
    security-critical path-sanitization in the tested Core strict zone.
  * `CliCommandRunner.cpp::SwapOutLogForConfineSafeBasename` now calls the Core
    helper and the inline `std::chrono ... milliseconds` stamp + leaf logic is
    **deleted** (removes the duplication, not adds it) — **CR Finding 2 (Minor)**:
    the ms-timestamp basename collided when two spawns started in the same tick;
    entropy fixes it.
  * **CR Finding 1 (Major)**: dropped the temporal `#1566` PR-ref from the durable
    `UiTestScenario.cpp` outLog comment (durable code comments stay present-tense,
    no PR-history breadcrumbs in strict-zone source); the confinement invariant
    wording is kept.
  * New doctest `tests/Core/SpawnOutLogBasename.test.cpp` (registered in
    `tests/CMakeLists.txt`, links the production `.cpp`): traversal collapse
    (`../../etc/passwd` → `-passwd`, confine-safe), absolute strip
    (`C:/Windows/system32/x.txt` → `-x.txt`), fallback (`.`/`..`/`""` →
    `-outlog.txt`), normal-leaf preserved, `spawn-` prefix, and
    entropy-uniqueness (two calls with the same input differ). This satisfies the
    test-delta gate with a real test of the exact trust boundary (no
    `tests-out-of-band` dodge). Security posture **unchanged**: child-side
    `ConfinePathUnderSubdir` confinement untouched, absolute/`..` from MCP/Lua
    still rejected, no PathConfinement bypass env/flag — the helper only generates
    a confine-safe basename, never widens what the child accepts.

## Deviations

- **PR A scope grew to include the PathConfinement perf-harness break.** The
  original PR A plan scoped only the token-401 env override; the branch's own
  Perf PR-fast run exposed a second, independent #1566 regression (absolute
  `--outPath` rejection). Fixing it is required for PR A's self-validating gate to
  go green, so the harness-only `perf-run.sh` fix lands in PR A rather than
  waiting for PR B. No product code changed; confinement is preserved.
- **Other `--spawn` callers that pass an absolute `--outPath` are still broken by
  PathConfinement** — `scripts/dev/test-whisper-roundtrip.sh`,
  `test-whisper-ai-assistant-autosend.sh`, `test-screenshot-diff.sh` (all
  bucket-C/E, currently **masked** / dropped from the meant-to-block allow-list,
  so they don't block merge). The general product-side contract fix (parent
  threads the confined path back to all `--spawn` outPath callers, or
  `NormalizeOutPath` stops absolutizing now that the parent reads `data.outPath`)
  is deferred to **PR D** (it is the **H1** sub-fix; see the split note below).
  `build-and-test.yml`'s
  `mobile-texture-guard` run passes no `--outPath` and is unaffected.
- **PR B scope reduced — the `--outPath` PathConfinement contract threading is
  deferred to a follow-up (PR D), NOT done in PR B.** The Files-to-modify row for
  `CliCommandRunner.cpp` originally tagged PR B with "thread confined `--outPath`
  contract for all `--spawn` callers." That was split off to keep PR B's
  security-sensitive MCP-auth change focused and reviewable (a trust-boundary diff
  the #1566 regression already burned us on once). The affected whisper/screenshot
  `--outPath` callers remain bucket-C/E **masked** (don't block merge), so the
  split costs no gate coverage. PR D will either thread `data.outPath` back to all
  `--spawn` outPath callers or stop `NormalizeOutPath` absolutizing now the parent
  reads the child-reported confined path. Removing PR A's CI env opt-out (now that
  PR B proves the product fix) is what rides **PR C**.
- **Two LOW security-review hardening notes — #2 actioned in the PR B CR round, #1
  still accepted** (both optional defense-in-depth, neither a ship-blocker):
  (1, deferred) `std::random_device` is not *guaranteed* cryptographic by the
  standard — but is CSPRNG-backed on the MSVC + Clang/libc++ + libstdc++-Linux
  matrix Smatchet ships (the deterministic MinGW-libstdc++ footgun does not apply);
  the attacker who could brute a 128-bit ephemeral loopback token in its sub-second
  lifetime already has same-user local access (out of the threat model). Tracked for
  PR D if revisited. (2, **DONE** in PR B — CodeRabbit Major) the child now scrubs
  `SMATCHET_MCP_SPAWN_TOKEN` from its env right after adoption
  (`_putenv_s(…,"")` on Windows / `::unsetenv` on POSIX) so no later subprocess
  inherits it — belt-and-suspenders over the existing `IsSensitiveEnvName` `TOKEN`
  substring scrub + `ObservedSmatchetEnv` allow-list exclusion.
- **CR round-1 security-review (provision/request split) — verdict GO, two new
  hardening items deferred to PR D** (neither merge-blocking): (a, **MEDIUM →
  informational**) on Windows the spawn token is transiently written into the
  *parent's* own env block (`SetEnvironmentVariableA` set→`CreateProcess`→clear)
  so the child inherits it, opening a microsecond same-user read window; the POSIX
  path is strictly better (sets it only inside the forked child). The threat needs
  a same-user attacker who already holds the operator's full privilege (can read
  the config token / DPAPI secrets directly), so it crosses no boundary. PR-D fix:
  build an explicit merged `lpEnvironment` block so the parent env is never
  mutated. (b, **LOW**) the child env scrub runs at `McpPlugin::OnStart`, so a
  hypothetical subprocess forked *before* plugin start would inherit the unscrubbed
  var — no current boot path does this (latent only); PR-D fix: scrub at process
  entry in `main.cpp` if a pre-plugin subprocess is ever added.
- **PR C narrowed to the opt-out removal; `--outPath` threading + the three
  optional hardening items split to PR D.** The plan originally bundled both on PR
  C. Split because (a) the opt-out removal is a pure CI-config change whose green CI
  is an unambiguous signal — bundling product code would muddy "did removing the
  opt-out break a `--spawn` gate?"; (b) the `--outPath` threading touches
  `NormalizeOutPath` absolutization + **PathConfinement**, the exact #1566
  arbitrary-file-write boundary, so it earns an isolated security-review + its own
  clean CI signal (and the standing hard constraint: **no PathConfinement bypass
  env**). PR D carries the `--outPath` contract fix + the deferred hardening
  (Windows `lpEnvironment` block, CSPRNG over `random_device`,
  scrub-at-process-entry).
- **PR E — the confine-safe basename logic now lives in a Core helper, not inline in
  `CliCommandRunner.cpp`.** Moved to satisfy the test-delta gate with a real unit
  test of the trust boundary (a strict-zone Core file is the right home for
  security-critical path sanitization), and to fix CR Finding 2 by swapping the
  collision-prone ms-stamp for `random_device` entropy. Net duplication is **lower**
  (the inline derivation was deleted, not duplicated). The helper is pure and
  one-shot per spawn — it never widens what the confined child accepts.
  * **Perf-gate (Source/Core/ touched — PR E):** the new helper runs **once per
    `--spawn`** invocation (one `std::random_device` draw + two `snprintf` + a string
    concat in `SwapOutLogForConfineSafeBasename`, on the CLI one-shot spawn path).
    It is **not** on the UI/render thread, not a per-frame / per-command / grid /
    scroll / draw path, and adds no steady-state allocation. **Nil impact on the
    6.94 ms / 144 Hz budget** — no perf scenario delta expected or measured.
- **PR E — reworked to COEXIST with develop's forward-raw outLog (2026-07-05).** After PR E was
  authored, develop's `8c425435` (remediate CPP_CODE_AUDIT #4/#5/#6) independently landed a
  *mutually-exclusive* fix in the same `CliCommandRunner.cpp` region: it **removed**
  `NormalizeOutPath`/`NormalizePathArgInPlace` entirely and forwards `outPath`/`outLog` **raw**
  (child-side confinement resolves a relative value under `<userData>/ui-tests/`, which both
  processes agree on without CWD translation). PR E's original design swapped **all** outLog for a
  confine-safe basename + copy-back. Per the user's "rework to coexist" decision, the merge
  resolution **keeps develop's forward-raw contract for `outPath` + a relative `outLog`** (the case
  CI exercises) and makes `SwapOutLogForConfineSafeBasename` **ABSOLUTE-only**: an absolute `outLog`
  (which the child rejects outright) is swapped for the tested `MakeConfineSafeSpawnOutLogBasename`
  leaf + relocated back via `RelocateChildOutLog`; a relative outLog is a no-op (`requestedOutLog`
  empty). Net: this **adds** absolute-outLog `--spawn` support without reverting develop's
  forward-raw decision. The re-added `NormalizeOutPath`/`NormalizePathArgInPlace` helpers were
  dropped (develop deleted them; `outPath` stays forward-raw); the Core helper +
  `SpawnOutLogBasename.test.cpp` are retained (the absolute branch still uses them). So the
  "second root cause" prose above (`NormalizeOutPath absolutizes even a relative path`) describes
  the pre-`8c425435` world and no longer holds — the coexist code no longer absolutizes anything.

## Verification

- [x] PR A: ctest `ConfigManager SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK env override` green — 4/4 assertions (default ON→true, env "false"→false, env "true"→true, env "garbage"→true fail-closed); full ConfigManager suite 28/28 cases
- [x] PR A: all 3 CodeRabbit findings on #1574 addressed (breadcrumb removed, fail-closed parse, test comment corrected)
- [x] PR A: security-review of the MCP-auth trust-boundary diff clean — no blocking findings; default unchanged outside CI, parse fail-secure, workflow env CI-scoped (no artifact/dev leakage)
- [x] PR A: own Perf PR-fast run green (self-validating) — PR #1574 merged to develop @ `ea9134e7`
- [x] PR #1571 / #1572 gates green after PR A merges to develop
- [x] PR B: build clean (596/596, SmatchetStandalone, `ninja-iter-msvc`) — both edited TUs compile (`set_default_headers`, `McpReadyStatus` enum, MSVC 4996 getenv pragmas)
- [x] PR B: lint gate PASS (`test-lint-rules.sh --diff origin/develop`, exit 0)
- [x] PR B: `--spawn app.version` reaches MCP-ready and returns `ok:true` under the **compiled secure default** — `scripts/dev/test-spawn-mcp-default-auth.sh` PASS (the regression test #1566 lacked; hermetic fresh `SMATCHET_USER_DATA`, env opt-out unset)
- [x] PR B: security-review of the MCP-auth trust-boundary diff — verdict **SHIP**, no CRITICAL/HIGH; every threat-model claim traced clean (CSPRNG token, env-only never argv, 127.0.0.1-only, constant-time compare, name-scrubbed, parent-env cleared unconditionally, child adoption strictly strengthens); two optional LOW hardening notes recorded in Deviations
- [x] PR B: **CR round-1** (provision/request split) re-build clean + two-phase `test-spawn-mcp-default-auth.sh` PASS (Phase 1 mint+inject, **Phase 2 persisted operator token — locks the CR #4 configured-token path**, both `ok:true` under the secure default) + lint gate exit 0
- [x] PR B: **CR round-1 security-review** of the provision/request split — verdict **GO** (no CRITICAL/HIGH): configured-token branch traced to inject nothing, no token logging, AuthRejected envelope leak-free, `--outPath` confinement byte-unchanged; one MEDIUM (Windows parent-env transient same-user window) + two LOW (`random_device`, scrub-timing) deferred to PR D, none merge-blocking
- [x] Gate-escape postmortem for #1566 filed with a `### Preventing gate` — PR #1575
- [x] PR B: CI gates green on the PR — #1576 merged to develop @ `870702de` (Perf PR-fast green with the PR-A env opt-out still in place; the gates themselves still ran tokenless — PR C is what runs them under the secure default)
- [ ] PR C: removed PR A's CI env opt-out from all 3 `--spawn` workflows — the PR's own CI (every `--spawn` gate green under the SECURE default, the first time) is the self-validating proof + the standing #1566 regression guard
- [ ] PR D (follow-up): thread confined `--outPath` to all `--spawn` callers (un-mask whisper/screenshot) + optional hardening (`lpEnvironment` block, CSPRNG, scrub-at-entry)
- [x] PR E (#1581, Fixes #1579): doctest rig green — full `SmatchetTests` 2227/2227 cases (`ctest -R ^smatchet_tests$` PASS, 15.5 s); the new `SpawnOutLogBasename` cases run isolated 6/6, 12 assertions PASS (traversal collapse, absolute strip, `.`/`..`/`""` fallback, normal-leaf preserved, `spawn-` prefix, entropy-uniqueness)
- [x] PR E: DX12 dual-target compile verify — `cmake --build --preset ninja-iter-msvc --target SmatchetCore_DX12 SmatchetStandalone` clean; `SpawnOutLogBasename.cpp` + `UiTestScenario.cpp` compiled in BOTH worlds, `SmatchetCore_DX12.lib` + `Smatchet.exe` linked (new Core header has no GLFW/GL pollution)
- [x] PR E: lint gate PASS — `test-lint-rules.sh --diff origin/develop` exit 0 (strict-zone rules on the new Commands/ files clean, no new duplication, no oversized function); only pre-existing advisory `unused-symbol-under-config-guard` WARNs (unrelated MCP-gated helpers, non-blocking)
- [x] PR E: spawn smoke both ways via `scripts/dev/test-ui-jira-deterministic-backend.sh` (ui-test exe rebuilt with the change) — **env-set** (`SMATCHET_UI_TEST_OUTLOG=<abs>`): passed=3 failed=0 `ok:true`, child wrote `...\ui-tests\spawn-36b5b5545e350945-smoke-outlog-*.txt` (entropy token from the new helper), `[spawn] outLog: relocated child log -> <abs>`, the absolute outLog populated (7115 bytes); **bare** (no env): passed=3 failed=0 `ok:true`, `outLog:""`, exit 0
- [x] PR E: both CodeRabbit findings cleared — #1 (`UiTestScenario.cpp` `#1566` temporal ref dropped, invariant kept), #2 (`CliCommandRunner.cpp` ms-stamp → entropy via the Core helper; no `std::chrono ... milliseconds` remains in the basename path — only legit poll/timeout/sleep uses)
