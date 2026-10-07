# `postmortem-owed.sh`'s batched GraphQL fetch times out at the default window, and the failure reads as "last 20 merges clean"

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-10-04
- **Observed on**: PR #2286 (merged 2026-10-04T01:59:46Z past a red `Bucket-E UI tests (Mesa headless GL)`). PR #2280 (merged with `Plan-lock gate` red under `plan-lock-out-of-band`) was hidden by the same blind spot. Reproduced from worktree `gracious-kilby-73a454` at `c9479274d`, which is the `origin/develop` tip on 2026-10-04.
- **Status**: open

## What happened

At SessionStart, `postmortem-owed.sh --list` printed `postmortem-owed: no gate escapes owed a postmortem (last 20 merges clean).` and the `--nudge` hook stayed silent. Both were wrong: #2286 had merged past a red check earlier the same day.

Trigger 1+2 reads every merged PR's checks in a single batched call (the `gh pr list --state merged --limit "$FETCH_N" --json …statusCheckRollup` read into `ROWS` in `agent-layer/agents/scripts/core/postmortem-owed.sh`):

```bash
done < <(gh pr list --repo "$REPO" --base develop --state merged --limit "$FETCH_N" \
            --json number,labels,mergedAt,mergeCommit,statusCheckRollup --jq "$JQ_ROWS" 2>/dev/null \
            | tr -d '\r' || true)
```

`FETCH_N` defaults to `SCAN_N * 3` = 60 (the `FETCH_N=` assignment). Each develop PR now carries about 52 check runs, and at `--limit 60` the GraphQL query returns `HTTP 504: 504 Gateway Timeout (https://api.github.com/graphql)`. That reproduced 3 times out of 3 on 2026-10-04. `--limit 20`, `30` and `40` all succeed.

`2>/dev/null` drops the error and `|| true` drops the exit code, so `ROWS` stays empty. With no rows, every trigger-1/2 check is skipped. The script then reaches the success branch (the `merges clean` echo) and reports the 20-merge window as clean, even though it read 0 merges.

With the fetch inside the timeout, the same script on the same tree reports both owed escapes:

```text
$ POSTMORTEM_FETCH_N=30 bash agent-layer/agents/scripts/core/postmortem-owed.sh --list
postmortem owed: PR #2280 — red-check: Plan-lock gate; override: plan-lock-out-of-band
postmortem owed: PR #2286 — red-check: Bucket-E UI tests (Mesa headless GL)
```

## Why it matters

This script is the post-merge safety net for every merge path that skips the poller (see the companion entry, [`2026-10-04-native-auto-merge-merges-past-a-red-non-required-check.md`](../infra/2026-10-04-native-auto-merge-merges-past-a-red-non-required-check.md)). When it fails, it says "clean", which is the one output that tells the reader nothing needs doing. Its header calls the default modes advisory and fail-open when `gh` is missing. But a missing `gh` prints a "skipped (advisory)" notice (the two `skipped (advisory)` echoes near the top of the script). A failed fetch prints a clean bill of health instead. The silence grows as rollups grow, so every new check context added to develop moves the detector further past the timeout. Nothing in the window was ever going to be caught.

## Concrete next action

1. **Fail loud, not clean.** Keep the fetch's exit status and stderr. If the fetch fails, or returns zero rows when the window should contain merges, print `postmortem-owed: merged-PR fetch failed (<first stderr line>) — window NOT scanned`. Print it on stderr in `--list` and `--nudge`, and exit non-zero in `--blocking`. Never fall through to the "merges clean" line. The `merges clean` echo should only be reachable when `${#ROWS[@]} -gt 0`.
2. **Fit the fetch under the timeout.** Page the window in chunks of at most 20, either with `gh api graphql --paginate` and `first: 20`, or with a loop of 20-PR `gh pr list --search "merged:<…"` calls. Alternatively, drop `statusCheckRollup` from the batch and read check runs per PR over REST (`commits/{sha}/check-runs`) only for the `SCAN_N` merges kept. Either way the cost scales with checks per PR, not the window size.
3. **Bats regression.** Run with a stub `gh` on `PATH` that exits 1 with `HTTP 504` for `pr list`. Assert the output does **not** contain `merges clean` and does contain `NOT scanned`. Also assert `--blocking` exits non-zero.

**Enumerator + replay**: the enumerator is the `gh pr list --state merged --limit $FETCH_N` row set (`ROWS`). Replayed on 2026-10-04 at the default `FETCH_N=60`, the call 504s and `ROWS` is empty, so action 1 prints `window NOT scanned` instead of `clean`. With action 2's 20-row chunks, the first chunk contains #2286 with `Bucket-E UI tests (Mesa headless GL)` as its curated red check, and it is flagged. That is the `POSTMORTEM_FETCH_N=30` output above.

Triggered-follow-up: when=pr-count:base=develop;since=2026-10-04;n=10; action=re-run postmortem-owed.sh --list at the default window and confirm it either scans or says NOT scanned, never a false clean; baseline=HTTP 504 at --limit 60 (3/3) and a false "last 20 merges clean" hiding #2280 + #2286, 2026-10-04; fired=never
