# Develop tip can go RED on a required check and silently block every PR until an author trips over it

- **Category:** infra
- **Priority:** P2
- **Date:** 2026-07-10
- **Status:** applied (2026-07-11 — `agents/scripts/core/develop-tip-required-green.sh` SessionStart nudge; flags a required check that ran on the develop tip and is terminal-non-success. Deliberately does NOT flag absent required checks — most are PR-only and never run on a develop push, which would false-fire every session; that self-disabled-gate case stays with postmortem-owed.sh's absence-present allow-list. Injectable data layer + `--selftest`; wired into `settings.json.tmpl`.)
- **Postmortem:** [`postmortems.md`](../../postmortems.md) § 2026-07-10 · PR #1698

## What happened

PR #1698 added `tests/bats/mutation_smoke.bats` with no `test-*.sh` wrapper. Its **required** `Doc anchors + agent contract` check ran ~60 s *after* the merge (merged 08:48:56Z, check started 08:49:56Z), so the `test-orphan-bats` failure landed on `develop` un-caught. Under **block-on-any-red**, that red develop tip was then inherited onto every open PR's own head — it silently blocked the whole repo until the #1666 fix (#1704) tripped over it and I root-caused it. Fixed the instance in #1705 (the missing wrapper).

## The gap

There's no cheap, standing signal that the **develop tip itself** has a RED required check. The failure is discovered only when the *next* author opens a PR and inherits the red — attributing the block to the wrong PR and costing a root-cause dig each time. Both detecting gates (`test-orphan-bats` in local pre-ship `test-docs.sh` AND the required CI check) exist and work; the miss was purely merge-*timing*, and nothing surfaces the resulting red-develop state proactively.

## Proposed fix

A lightweight **develop-tip required-green assertion**: query the develop tip's *required* status-check conclusions (`gh api repos/…/commits/<develop-tip>/check-runs`, filter to `required_status_checks.contexts`) and raise a loud, attributable nudge the moment any is RED — naming the check + the commit/PR that turned it red. Two viable homes:
- extend `agents/scripts/core/postmortem-owed.sh`'s SessionStart sweep (it already inspects merged state), or
- a new `agents/scripts/core/develop-tip-required-green.sh` run at SessionStart.

Converts "silent red develop blocks every PR" into an immediate signal tied to the introducing PR. Durable complement to #1705 (which fixed the specific orphan): the wrapper stops *this* orphan; the tip-health assert stops the *class* — a required check going red on develop and nobody noticing until it blocks the next author (the #1237-family merge-before-terminal race is one upstream cause).

## Self-improvement

Empty.
