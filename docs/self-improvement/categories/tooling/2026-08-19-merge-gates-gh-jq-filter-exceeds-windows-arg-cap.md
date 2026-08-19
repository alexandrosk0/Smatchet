# `merge-gates.sh` passes its ~25 KB jq filter on the `gh` command line, which exceeds the Windows 32 KB `CreateProcess` cap — the poller reports `GH_API_DOWN` on a healthy API

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-08-19
- **Found during**: shipping [PR #2124](https://github.com/alexandrosk0/Smatchet/pull/2124) (README screenshot) from a Windows session

## Symptom

Every invocation of the gate poller died immediately, three attempts in a row:

```
gh: Argument list too long
```

`poll_merge_gates` mapped that to its API-outage classification and returned **3**
(`GH_API_DOWN`) — a verdict that says *GitHub is unreachable*. GitHub was fine; plain
`gh api`, `gh pr view`, and `gh run list` all worked in the same shell seconds later.

Nothing about the failure points at its cause. The operator's reasonable next move —
retry, then wait for the "outage" to clear — never succeeds, because there is no outage.

## Cause

[`merge-gates.sh:646`](../../../../agents/scripts/core/merge-gates.sh) invokes:

```bash
gh api graphql -f owner=… -f repo=… -F pr=… -f query="$query_body" --jq "$GATE_FILTER"
```

Both large payloads travel **as command-line arguments**:

- `$GATE_FILTER` — the gate-decision jq program from
  [`merge-gates.d/10-gate-filter.sh`](../../../../agents/scripts/core/merge-gates.d/10-gate-filter.sh),
  **25,185 bytes** today and growing with every gate refinement;
- `$query_body` — the GraphQL document from
  [`merge-gates.graphql`](../../../../agents/scripts/core/merge-gates.graphql).

Windows caps a `CreateProcess` command line at 32,767 characters. The two together clear
it, so the process never launches. On Linux/macOS the equivalent limit (`ARG_MAX`, ~2 MB)
is far away, which is why CI and the maintainer's non-Windows paths never saw this — the
gate poller is effectively **unrunnable locally on Windows**, on a repo whose primary
development host is Windows 11.

The classification is a second, separable defect: a launch failure of the `gh` binary is
attributed to the *remote* API. `Argument list too long` is an `E2BIG` from the local OS
and can never mean the API is down.

## Workaround used

A `gh` shell-function shim that intercepts `--jq`, writes the filter to a temp file, and
pipes the response through local `jq -r -f "$tmp"`. The GraphQL body alone fits under the
cap, so only the filter needs relocating. With the shim the poller ran normally and
reached `GATES_PASSED`.

## Proposed fix

1. **Stop passing the filter as an argument** (~20 min). Fetch the raw GraphQL response
   with `gh api graphql` (no `--jq`) and pipe it through `jq -r -f <file>`, reading the
   filter from a temp file written by the script. The filter is already assembled in a
   variable, so this is a call-site change, not a restructure. `jq -f` has no
   command-line-length exposure at all.
2. **Also move the GraphQL body off the command line** — `gh api graphql -F query=@file`
   accepts a file reference, removing the second contributor and leaving headroom as both
   payloads keep growing.
3. **Do not classify a local exec failure as an API outage.** Match `Argument list too
   long` / `E2BIG` on the `gh` invocation and return the usage/dependency code (2) with
   the real reason, rather than folding it into `GH_API_DOWN` (3). An operator who is told
   "GitHub API is down" has no path to the actual fix.
4. **Regression guard**: a bats case in `tests/bats/merge_gates.bats` asserting the
   assembled filter is never interpolated into an argv position — e.g. that the `gh api
   graphql` call site carries no `--jq`.

## Why it matters

The gate poller is the enforcement point for AGENTS.md § Merge gates — the thing that
stands between an agent and a merge past a red check. On Windows it does not run at all,
and it fails in the one way that discourages investigation: an outage verdict on a
healthy API, which invites either waiting or reaching for the admin-merge carve-out. A
gate that is locally unrunnable on the primary dev platform is a gate that gets routed
around. See also [`2026-08-18-cr-out-of-band-label-inert-until-gate-rerun.md`](2026-08-18-cr-out-of-band-label-inert-until-gate-rerun.md)
for the other half of the same session's un-wedging cost.
