- 2026-08-18 · claude-code · [tooling] · P1 — shared shell tooling has no Windows lane, so a Windows-only exec-limit regression in the *merge gate itself* shipped green and disabled gate polling on every interactive session for ~5 days

  Observed while trying to merge PR #2127. `safe-merge.sh 2127` refused with
  `GH_API_DOWN` (rc 3) on a PR whose checks were all green; the poll log read
  `gh: Argument list too long` three times. Not an outage — Windows caps a
  `CreateProcess` command line at 32,767 characters, and
  [`poll_merge_gates`](../../../../agents/scripts/core/merge-gates.sh) was
  passing **two** independently-growing payloads on argv: the spliced `--jq`
  filter (24,865 chars after substitution) and the GraphQL document
  (7,795 chars), 32,660 before the `gh` path, the flags and the other fields.
  `d63a7009` (PR #2120) had grown
  [`merge-gates.d/10-gate-filter.sh`](../../../../agents/scripts/core/merge-gates.d/10-gate-filter.sh)
  by 2,166 bytes, which was the straw; the sum had been ~2.2 KB under the cap
  before it. Full RCA: [`postmortems.md`](../../postmortems.md) § 2026-08-18.

  The instance is fixed in that PR (document moved off argv via
  `-F query=@"$QUERY_FILE"`, a `< 30,000` budget assertion on the substituted
  filter, and an E2BIG arm that reports `GH_ARGV_TOO_LONG` instead of laundering
  an exec failure into a GitHub outage). What remains is the **class**: the
  suite that guards the merge gate runs only on `ubuntu-latest`, where `ARG_MAX`
  is ~2 MB — roughly 60× the limit that was breached — so the platform hosting
  every interactive orchestrator / `git-janitor` / `smatchet-merge-watcher`
  session is the one platform nothing tests. Any Windows-specific fault in
  `agents/scripts/**` (exec limits, `CreateProcess` quoting, `\r\n`, path
  separators, missing coreutils) ships green the same way.

  It failed closed, which is the only reason this is a P1 and not a P0:
  `safe-merge.sh` refused rather than merging blind. But it refused *with the
  wrong diagnosis*, and a permanent local fault reported as a transient remote
  one is exactly the shape that gets an operator to reach for
  `SKIP_MERGE_GATES` — a real gate bypass one bad inference away.

  Proposed gate — **one Windows smoke lane for the shared shell tooling.** A
  `windows-latest` job running the platform-sensitive subset of
  `tests/bats/merge_gates.bats` (bats runs under Git-Bash / MSYS2) would have
  reproduced this on the #2120 head instead of five days later on an unrelated
  PR. It does not need to be the full suite or a required check to pay for
  itself: the value is that *any* exec of the real `gh`-shaped command line
  happens once on the platform with the tight limit.

  Cheaper interim, already landed: the budget assertion is a **length** check,
  not an exec, so it catches the Windows fault while running on Linux. That is
  the right shape for a known limit — but it only covers the limit somebody
  thought to write down, which is precisely the property that failed here.

  Second-order, worth a separate look: `10-gate-filter.sh` is a 25 KB jq program
  living in a shell string. Even with the document off argv it has only ~7.9 KB
  before it hits the cap on its own (5.1 KB before it trips the new budget
  assertion), and it grows every time the gate learns a new rule. `gh --jq` has no file form, so the durable fix is to stop
  routing it through `gh` — pipe the raw response into a standalone `jq -f
  merge-gates.jq` — which removes the ceiling entirely and makes the filter
  diffable, lintable and testable as a file instead of as a quoted blob.
  Status: partially-applied (instance fixed 2026-08-18; the Windows lane and the
    `jq -f` extraction are both open)
  Last-reviewed: 2026-08-18
