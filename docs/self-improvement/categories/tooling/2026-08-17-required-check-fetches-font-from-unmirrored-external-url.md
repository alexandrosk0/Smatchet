# Required checks depend on un-cached GitHub content fetches, once per job

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-08-17
- **Observed on**: PR #2090 — four waves red across two heads; `develop` red on the same job; two distinct GitHub content endpoints returning 429
- **Status**: open (the outage cleared; the design gap it exposed is unfixed)

## What happened

On 2026-08-17, GitHub content endpoints began returning **429 Too Many Requests** to this repository
for roughly two and a half hours. CI fetches content from those endpoints **once per job**, with no
local copy, so the throttle reded required checks with no author-actionable cause.

Two surfaces, one cause.

**Surface 1 — asset fetch.** `.github/actions/fetch-fontawesome/action.yml` downloads
`fa-solid-900.ttf` from `github.com/FortAwesome/Font-Awesome/raw/6.7.2/webfonts/` on every job that
needs it. There are **six call sites** — five in `build-and-test.yml`, one in `release.yml`. (The
action's own comment says "five jobs"; it undercounts, and the one it misses is the release build, so
an outage during a release does not only red a PR.) The step exhausted its retry budget four times:

| head | job | step | window | elapsed |
|---|---|---|---|---|
| `15a2a39` | Windows + MSVC (ARM64 cross) | 7 | 13:51:45 → 13:56:47 | 302 s |
| `15a2a39` | Windows + MSVC | 11 | 14:06:55 → 14:11:58 | 303 s |
| `c0324f1` | Windows + MSVC (ARM64 cross) | 7 | 14:32:25 → 14:37:27 | 302 s |
| `c0324f1` | Windows + MSVC | 11 | 14:45:00 → 14:50:02 | 302 s |

302 s is not arbitrary: it is `--retry 10 --retry-delay 30` spending its ten 30-second delays and
giving up. `--retry-max-time 600` never binds, because the delays run out first.

**Surface 2 — action tarball fetch.** Two jobs on head `6f19e5a` failed at **`Set up job`**, before a
single step of their own ran, each after three 429s on `codeload.github.com`:

| job | action being downloaded | elapsed |
|---|---|---|
| 95422505777 `Windows + MSVC` | `ilammy/msvc-dev-cmd@0b201ec…` (`/zip/`) | 67 s |
| 95423352360 `Mobile — Android APK` | `actions/setup-java@b6effb05…` (`/tar.gz/`) | 50 s |

```
Failed to download action 'https://codeload.github.com/ilammy/msvc-dev-cmd/zip/0b201ec…'
Error: Response status code does not indicate success: 429 (Too Many Requests)
Back off 15.277 seconds before retry.
... 429 ... Back off 28.574 seconds before retry.
##[error]Failed to download archive ... after 3 attempts.
```

Two *different* action repositories, so the throttle was on `codeload` itself, not on one upstream
project. `Set up job` is where the runner downloads every `uses:` action, so a 429 there kills a job
before its workflow logic begins — and it looks nothing like the font failure, which is why the first
one was initially and wrongly read as a *runner provisioning* fault. It is the same throttle.

`develop` was red at the same time: its `b5d0895` `Build and test` run (32034968270) failed on
**exactly one job, `Windows + MSVC`**, with all 22 others passing. The failing *step* there is
unconfirmed — the Actions log blob host is blocked by this environment's proxy, and `get_job_logs`
returns only a fixed ~700-line tail — so treat develop's cause as *consistent with* this entry rather
than proven.

This is a **recurrence**. The action's own comment records the shape: *"a parallel PR burst
rate-limited this exact URL for ~2h on 2026-07-09, redding otherwise-green jobs post-build."* The
retry budget was the fix applied then, and it is not enough.

**Resolution.** A targeted `rerun_failed_jobs` at 16:21, after the ~2 h precedent had elapsed, came
back fully green on the first attempt: both `Set up job` and `Fetch Font Awesome` succeeded, the
`Smatchet.exe` artefact uploaded, and the entire downstream Mesa/Sanitizer/Bucket wave ran and passed.
Nothing in the tree changed between the failing and passing attempts.

## Why it matters

`Windows + MSVC` is a branch-protection **required** check, so while the throttle held, **no PR in the
repository could go green**, and no author could act on it. The failure is also expensive and badly
placed:

- The font step runs *after* `Configure` and `Build test binaries`, so a job burns 10–13 minutes of
  compile before dying on a 400 KB download.
- The `Smatchet.exe` artefact is then never uploaded, cascading every Mesa/Sanitizer/Bucket job to
  `skipped`. One external 429 became 8 non-green jobs per wave on #2090.
- A `Set up job` 429 wastes less time but is harder to read: no step of the job's own appears in the
  log at all.

The pinning is *correct* and must stay — an immutable tag plus `sha256sum -c` for the font, and
SHA-pinned `uses:` refs for the actions. This is not a supply-chain gap. It is a **single-point-of-
failure and retry-budget gap**: immutable, already-verified bytes are re-fetched from a third party
once per job, forever, with no local copy.

## Rapid pushing amplifies this, and probably contributed

Each push starts a wave of ~23 jobs across `build-and-test`, `doc-validation`, `perf`, `plan-lock`,
`test-delta`, the duplication scanner and CodeQL. Every one of those jobs downloads several pinned
actions from `codeload`. #2090 saw **five pushes in about forty minutes**, i.e. hundreds of content
requests in a short window against the endpoints that then began 429ing. Causation is not proven —
`develop` was already red before the last of those pushes — but the action's own comment blames the
2026-07-09 recurrence on exactly this shape, and the operational lesson stands: **batch changes into
one push instead of pushing each fix as it lands.**

One measurement worth recording, because the intuition is wrong: a PR **body edit** does *not*
multiply this. `build-and-test.yml`'s `pull_request:` trigger has no `types:` override, so it uses the
default `[opened, synchronize, reopened]` and `edited` is excluded; only `doc-validation.yml` opts into
`edited`, deliberately and cheaply. So the verdict-line rebind that every push forces costs three fast
doc jobs, not a second build wave. The push is the expensive half.

## Concrete next action

Ranked, cheapest first. All keep every pin and checksum.

1. **Cache the asset.** Wrap the font fetch in `actions/cache` keyed on `FA_TAG` + `FA_SHA256`,
   restoring to `assets/fonts/fa-solid-900.ttf`, and skip the `curl` on a cache hit. The key is
   content-addressed, so a tag bump invalidates it automatically and the checksum still runs. Six call
   sites per push become at most one network fetch per cache epoch.
2. **Vendor the asset.** 400 KB, immutable, already checksum-pinned. Commit it under `assets/fonts/`
   and delete the fetch entirely. Costs one binary in the tree and a manual bump when `FA_TAG` moves.
3. **Move the font step before `Configure`/`Build`** in any job that keeps a fetch, so an outage fails
   in 30 seconds instead of after a 13-minute compile. Worth doing regardless of 1 or 2.
4. **Address the action-tarball half too** — this is the part the first version of this entry missed.
   The general form of remedy 1: cache or mirror the `uses:` tarballs so `Set up job` does not depend
   on `codeload` answering per job. Because every action here is pinned by SHA, a mirror or cache
   preserves the pin exactly; the SHA remains the integrity check. Without this, remedies 1–3 harden
   one surface and leave the other able to red the same required checks.
5. Raising `--retry-delay` / `--retry-max-time` is the **weakest** option: the observed window is ~2 h,
   so any budget that survives it converts a red job into a multi-hour one.

Enumerators for the sweep: `grep -rn 'fetch-fontawesome' .github/workflows/` names every job carrying
the asset dependency (6 call sites as of this entry); `grep -rn 'uses:' .github/workflows/ | grep -v
'\./'` names every job carrying the action-tarball dependency, which is effectively all of them.

Related: [`2026-08-06-bucket-c-goldens-env-dependent-on-icon-font.md`](../test/2026-08-06-bucket-c-goldens-env-dependent-on-icon-font.md)
covers the *rendering* consequences of this same font; this entry is about *obtaining* content.

Triggered-follow-up: when=pr-count:base=develop;since=2026-08-17;n=15; action=check whether a Fetch Font Awesome step or a Set up job action download has failed a required check again, and whether caching, vendoring or an action mirror landed; baseline=4 font failures plus 2 codeload Set up job failures across 2 heads on 2026-08-17, a same-job develop failure, and one ~2h outage on 2026-07-09; fired=never
