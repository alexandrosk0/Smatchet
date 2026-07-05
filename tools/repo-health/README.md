# Repo-health dashboard

The human-on-the-loop **visibility layer** from [`AI_POLICY.md`](../../AI_POLICY.md):
one page that replaces reconstructing project state from five markdown files plus
the Actions tab. Ten seconds of reading instead of a scavenger hunt.

**Live artifact:** <https://claude.ai/code/artifact/e1a28cab-afd3-4f31-8258-4efb2387806e>

**What it shows:** fable-5 campaign scorecard · audit burn-down · CI advisory
lanes · coverage · open agent PRs · self-improvement backlog · plan lifecycle ·
governance grants.

## Why it's "living"

Two data sources, so the numbers don't silently rot:

| Source | Who fills it | Examples |
|---|---|---|
| **Computed** — `generate.py`, offline, deterministic | recomputed on **every run** from the tree | coverage %, audit finding totals, fuzz-target count, test-TU count, plan lifecycle counts, self-improvement backlog counts, governance grant flags, `develop` SHA |
| **Facts** — [`facts.json`](facts.json), session-maintained | refreshed by a session/routine with GitHub MCP access | campaign scorecard verdicts, CI advisory-lane run statuses, open-PR gate states |

A script alone cannot fetch CI/PR state (that needs the GitHub MCP tools) or make
the campaign judgment calls — hence the split. Everything derivable from the tree
updates for free; the rest is a small JSON a session keeps current.

## Files

| File | Role |
|---|---|
| `generate.py` | collector + renderer. Reads the tree + `facts.json` → `dashboard.html`. |
| `template.html` | the design. `{{PLACEHOLDER}}` tokens; theme-aware, self-contained. |
| `facts.json` | session-maintained verdicts + live GitHub state. |
| `dashboard.html` | **build output — git-ignored.** Never commit or hand-edit. |

## Regenerate

```sh
python3 tools/repo-health/generate.py            # -> tools/repo-health/dashboard.html
python3 tools/repo-health/generate.py --print    # also dump computed metrics to stderr
```

Degrades gracefully: any metric it can't parse falls back to `facts.json` (or an
em dash) and the run still succeeds — a partial dashboard beats a crash in an
unattended routine. A leftover-token warning on stderr means the template gained a
placeholder the renderer doesn't fill yet.

## Republish (to the fixed URL)

`generate.py` only writes the HTML — publishing is an agent step (the Artifact
tool), because the URL must stay stable across redeploys:

1. `python3 tools/repo-health/generate.py`
2. Call the **Artifact** tool with `file_path` = `tools/repo-health/dashboard.html`,
   `favicon` = 🗡️, and `url` = the `artifact_url` in `facts.json`. Passing that URL
   redeploys in place instead of minting a new one.

## Keeping facts.json fresh (session / routine)

Before republishing, refresh the live half from GitHub MCP:

- **CI lanes** — `actions_list(list_workflow_runs, event=schedule)` for each nightly
  workflow (`sanitizer-nightly`, `fuzz-smoke`, `tsan-linux-nightly`,
  `fresh-clone-configure-nightly`, weekly `dep-cve-sbom`); set each lane's `status`
  (`pass` / `fail` / `weekly`) and `when` (run number).
- **Open PRs** — `list_pull_requests(state=open)`; set each `state`
  (`ready` / `green` / `draft`) from its gate poll.
- **Campaign** — update an item's `status` (`landed` / `partial` / `not-started`)
  when its state actually changes.

This is a natural add-on to the **smatchet-nightly-advisory-lane-triage** routine,
which already gathers the lane statuses: after triage, refresh `facts.json`,
regenerate, and republish so the board reflects each night.
