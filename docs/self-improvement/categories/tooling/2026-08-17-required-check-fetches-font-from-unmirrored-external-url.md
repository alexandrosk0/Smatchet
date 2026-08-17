# A required check fetches a font from an unmirrored external URL, once per job

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-08-17
- **Observed on**: PR #2090 — two consecutive waves red at the same step; `develop` red on the same job
- **Status**: open

## What happened

`.github/actions/fetch-fontawesome/action.yml` downloads `fa-solid-900.ttf` from
`github.com/FortAwesome/Font-Awesome/raw/6.7.2/webfonts/` on **every job that needs it**. There are
**six such call sites** — five in `build-and-test.yml` and one in `release.yml`. (The action's own
comment says "five jobs"; it undercounts, and the one it misses is the release build, so an outage
during a release does not just red a PR.) On 2026-08-17 that URL started refusing requests, and the
step exhausted its retry budget four times in a row on one PR:

| head | job | step | window | elapsed |
|---|---|---|---|---|
| `15a2a39` | Windows + MSVC (ARM64 cross) | 7 | 13:51:45 → 13:56:47 | 302 s |
| `15a2a39` | Windows + MSVC | 11 | 14:06:55 → 14:11:58 | 303 s |
| `c0324f1` | Windows + MSVC (ARM64 cross) | 7 | 14:32:25 → 14:37:27 | 302 s |
| `c0324f1` | Windows + MSVC | 11 | 14:45:00 → 14:50:02 | 302 s |

302 s is not arbitrary: it is `--retry 10 --retry-delay 30` spending its ten 30-second delays and
giving up. `--retry-max-time 600` never binds, because the delays run out first.

`develop` is red too. Its `b5d0895` `Build and test` run (32034968270) failed on **exactly one job,
`Windows + MSVC`**, with all 22 others passing — the same job, the same singleton signature. I could
not confirm the failing *step* on that run: the Actions log blob host is blocked by this
environment's proxy, and `get_job_logs` returns only a fixed ~700-line tail that stops after the
failing step's output. So treat develop's cause as *consistent with* this entry rather than proven.

This is a **recurrence**. The action's own comment records it: *"a parallel PR burst rate-limited this
exact URL for ~2h on 2026-07-09, redding otherwise-green jobs post-build."* The retry budget was the
fix applied then, and it is not enough.

## Why it matters

`Windows + MSVC` is a branch-protection **required** check, so while the upstream limit holds, **no PR
in the repository can go green** — the failure is not attributable to any diff and no author can act
on it. It also lands late and expensively: on both heads the step runs *after* `Configure` and
`Build test binaries`, so a job burns 10–13 minutes of compile before dying on a 400 KB download, and
the `Smatchet.exe` artefact is never uploaded, which cascades every Mesa/Sanitizer/Bucket job to
`skipped`. On #2090 that turned one external outage into 8 non-green jobs per wave.

The pinning is *correct* and should stay — an immutable tag plus a `sha256sum -c` check. This is not a
supply-chain gap. It is a single-point-of-failure and a retry-budget gap: the same immutable,
checksum-verified 400 KB blob is re-fetched from a third party once per job, forever, with no local
copy.

## Concrete next action

Ranked, cheapest first. All keep the tag pin and the checksum assertion.

1. **Cache it.** Wrap the fetch in `actions/cache` keyed on `FA_TAG` + `FA_SHA256`, restoring to
   `assets/fonts/fa-solid-900.ttf`, and skip the `curl` on a cache hit. The key is content-addressed,
   so a bump invalidates it automatically and the checksum still runs. Network is touched once per
   cache epoch instead of once per job — six call sites per push become at most one.
2. **Vendor it.** 400 KB, immutable, already checksum-pinned. Commit it under `assets/fonts/` and
   delete the fetch. Removes the dependency outright; costs one binary in the tree and a manual bump
   when `FA_TAG` moves.
3. **Move the step earlier** in the jobs that keep a fetch, before `Configure`/`Build`, so an outage
   fails in 30 seconds rather than after a 13-minute compile. Worth doing regardless of 1 or 2.
4. Raising `--retry-delay`/`--retry-max-time` is the **weakest** option: the observed outage window is
   ~2h, so any budget that survives it converts a red job into a multi-hour one.

Enumerator for the sweep: `grep -rn 'fetch-fontawesome' .github/workflows/` names every job carrying
this dependency; each is a job that cannot start its build without a third party answering.

Related: [`2026-08-06-bucket-c-goldens-env-dependent-on-icon-font.md`](../test/2026-08-06-bucket-c-goldens-env-dependent-on-icon-font.md)
covers the *rendering* consequences of this same font; this entry is about *obtaining* it.

Triggered-follow-up: when=pr-count:base=develop;since=2026-08-17;n=15; action=check whether a Fetch Font Awesome step has failed a required check again, and whether caching or vendoring landed; baseline=4 failures across 2 heads on 2026-08-17 plus a same-job develop failure, and one ~2h outage on 2026-07-09; fired=never
