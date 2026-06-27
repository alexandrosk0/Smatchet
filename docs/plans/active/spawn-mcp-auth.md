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

## Files to modify

| File | Change | PR |
|---|---|---|
| `Source/Core/src/Config/ConfigManager.cpp` | env override parse for `SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK` | A |
| `tests/Core/ConfigManager.test.cpp` | TEST_CASE for the new override | A |
| `.github/workflows/perf-pr-fast.yml` | workflow env `…=false` | A |
| `.github/workflows/perf-full.yml` | workflow env `…=false` | A |
| `.github/workflows/build-and-test.yml` | workflow env `…=false` | A |
| `Source/Standalone/CliCommandRunner.cpp` | parent provisions + sends spawn token; fast-fail 401 | B |
| `Source/Plugins/Mcp/McpPlugin.cpp` | accept sanctioned ephemeral token | B |
| `tests/…` (CLI/integration) | `--spawn` reaches MCP-ready under default config | B |

## Implementation log

- **2026-06-27 — PR A** — added the env override + test + 3 workflow env entries.
  Lint PASS (one soft WARN: `ApplyOverridesAndClamps` 21>20 branches — soft tier,
  under the 30 hard cap; flat env-override list, refactor declined as scope creep).

## Deviations

- _(none yet)_

## Verification

- [x] PR A: ctest `ConfigManager SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK env override` green — 3/3 assertions (default ON→true, env "false"→false, env "true"→true); full ConfigManager suite 28/28 cases, 357 assertions
- [x] PR A: security-review of the MCP-auth trust-boundary diff clean — no blocking findings; default unchanged outside CI, parse fail-secure, workflow env CI-scoped (no artifact/dev leakage)
- [ ] PR A: own Perf PR-fast run green (self-validating)
- [ ] PR #1571 / #1572 gates green after PR A merges to develop
- [ ] PR B: `--spawn` reaches MCP-ready under default config (regression test)
- [ ] Gate-escape postmortem for #1566 filed with a `### Preventing gate`
