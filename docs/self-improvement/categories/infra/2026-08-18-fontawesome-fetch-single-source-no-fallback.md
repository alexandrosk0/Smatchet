# A single-source Font Awesome fetch red-walls five required CI jobs on an upstream 404, and `--retry-all-errors` burns 5 minutes retrying a permanent failure

- **Category**: infra
- **Priority**: P1
- **Date**: 2026-08-18
- **Found during**: un-wedging [PR #2070](https://github.com/alexandrosk0/Smatchet/pull/2070) — its `Windows + MSVC` attempt 1 failure

## Symptom

`Windows + MSVC` — a **required** check — went red on PR #2070 with a failure that had
nothing to do with the diff. Step 11, *Fetch Font Awesome 6 Solid TTF (pinned +
checksum-verified)*:

```
curl: (22) The requested URL returned error: 404
```

repeated **eleven times**, then `##[error]Process completed with exit code 22`. A bare
re-run at 22:37Z cleared the same step and the job went green at 23:05:21Z — the URL was
transiently serving 404. The diff was never at fault.

The red counted against the PR for hours. Under block-on-any-red it also blocks every other
open PR that inherits the check, and this fetch runs in **five** jobs
([`build-and-test.yml`](../../../../.github/workflows/build-and-test.yml) lines 338, 436,
519, 711, 831), so one upstream blip reds the board.

## Cause

[`.github/actions/fetch-fontawesome/action.yml`](../../../../.github/actions/fetch-fontawesome/action.yml)
fetches from exactly one origin with no mirror and no cache:

```bash
curl -fsSL --retry 10 --retry-delay 30 --retry-max-time 600 --retry-all-errors \
     "https://github.com/FortAwesome/Font-Awesome/raw/${FA_TAG}/webfonts/fa-solid-900.ttf" \
     -o assets/fonts/fa-solid-900.ttf
```

Two compounding problems:

1. **No fallback source.** `github.com/.../raw/` availability is a hard build dependency of
   five required jobs. The pin itself is right (immutable tag + sha256, per the action's own
   comment) — the pin is what makes a mirror *safe*, since any source that hashes to
   `af19d135…` is byte-identical by construction.
2. **`--retry-all-errors` treats 404 as retryable.** It was added for a real reason (the
   comment records a ~2h 429 rate-limit on this URL on 2026-07-09), but 429/5xx are
   transient and 404 is not — `--retry 10 --retry-delay 30` spent ~5 minutes of runner time
   re-asking a question already answered, then failed anyway. The eleven identical lines in
   the log are that loop.

## Proposed fix

1. **Add a mirror, try in order** (~1h). jsDelivr (`cdn.jsdelivr.net/gh/FortAwesome/Font-Awesome@${FA_TAG}/webfonts/fa-solid-900.ttf`)
   serves the same tag from different infrastructure. Loop over an ordered source list,
   accept the first that passes `sha256sum -c -`, fail only when all are exhausted. The
   checksum is the trust anchor, so adding sources adds availability without adding trust
   surface.
2. **Cache the font by `FA_SHA256`.** `actions/cache` keyed on the hash makes the steady
   state a cache hit and takes upstream off the hot path entirely for the other four jobs
   once one has fetched it.
3. **Stop retrying 404.** Drop `--retry-all-errors` and keep the default retryable set
   (transient + 429), or gate it behind an explicit status check. Preserves the 2026-07-09
   fix while failing fast on a permanent error — and a fast failure is what makes the
   mirror attempt cheap.

## Why it matters

A required gate that reds on third-party CDN weather is a gate whose reds stop being read.
The correct reflex here was "re-run it" — indistinguishable, at a glance, from the reflex
that lets a real red through, and the repo has already paid for that confusion once
([`postmortems.md`](../../postmortems.md), #1957 class). The fix is cheap and the pin
already did the hard part: with a checksum this strict, a second source costs nothing in
supply-chain risk and removes a single point of failure from five required jobs.
