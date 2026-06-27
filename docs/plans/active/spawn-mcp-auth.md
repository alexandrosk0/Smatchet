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
#1566 also added `Source/Core/include/Commands/PathConfinement.h` and wired it
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

## Files to modify

| File | Change | PR |
|---|---|---|
| `Source/Core/src/Config/ConfigManager.cpp` | env override parse for `SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK` | A |
| `tests/Core/ConfigManager.test.cpp` | TEST_CASE for the new override | A |
| `.github/workflows/perf-pr-fast.yml` | workflow env `…=false` | A |
| `.github/workflows/perf-full.yml` | workflow env `…=false` | A |
| `.github/workflows/build-and-test.yml` | workflow env `…=false` | A |
| `scripts/dev/perf-run.sh` | drop `--outPath`; capture spawn envelope `data` → `$ABS_OUT` (PathConfinement-safe) | A |
| `Source/Standalone/CliCommandRunner.cpp` | parent provisions + sends spawn token; fast-fail 401; thread confined `--outPath` contract for all `--spawn` callers | B |
| `Source/Plugins/Mcp/McpPlugin.cpp` | accept sanctioned ephemeral token | B |
| `tests/…` (CLI/integration) | `--spawn` reaches MCP-ready under default config | B |

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
  is deferred to **PR B**. `build-and-test.yml`'s `mobile-texture-guard` run
  passes no `--outPath` and is unaffected.

## Verification

- [x] PR A: ctest `ConfigManager SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK env override` green — 4/4 assertions (default ON→true, env "false"→false, env "true"→true, env "garbage"→true fail-closed); full ConfigManager suite 28/28 cases
- [x] PR A: all 3 CodeRabbit findings on #1574 addressed (breadcrumb removed, fail-closed parse, test comment corrected)
- [x] PR A: security-review of the MCP-auth trust-boundary diff clean — no blocking findings; default unchanged outside CI, parse fail-secure, workflow env CI-scoped (no artifact/dev leakage)
- [ ] PR A: own Perf PR-fast run green (self-validating)
- [ ] PR #1571 / #1572 gates green after PR A merges to develop
- [ ] PR B: `--spawn` reaches MCP-ready under default config (regression test)
- [ ] Gate-escape postmortem for #1566 filed with a `### Preventing gate`
