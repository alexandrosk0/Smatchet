# 25 deviations expire on the same day and block every merge when they do

- **Category**: process
- **Priority**: P1
- **Date**: 2026-08-16
- **Observed on**: the full deviation re-evaluation, [`docs/audits/DEVIATION_AUDIT_2026-08-16.md`](../../../audits/DEVIATION_AUDIT_2026-08-16.md) § S5
- **Status**: open

## What happened

`deviation-overdue` is an **absolute, whole-tree** rule: `compute_wide_violations()` scans every
first-party C++ file (not the diff), and any hit sets `rc=1` at
`agents/scripts/project/test-lint-rules.sh:705`. One overdue marker anywhere fails the gate for
every open PR until it is re-dated or removed.

The live `revisit=` dates are not spread out — they were stamped in bulk by sweeps. Stubbing
`today_ymd()` and re-running the real `compute_wide_violations`:

| date | markers overdue | gate |
|---|---|---|
| 2026-08-16 (today) | 0 | green |
| **2026-10-01** | **25** | **RED — all merges blocked** |
| 2026-10-02 | 27 | RED |
| 2026-12-02 | 34 | RED |
| **2027-01-01** | **94** | **RED** |

**Update 2026-08-16** — retiring the 20 markers that suppress no live clone (audit § Retire) cuts
the near cliff from **25 to 15** and the 2027-01-01 cohort from **94 to 77**. The class is reduced,
not closed: 15 markers still land on one day, and the remaining 77 are still a single date.

The 25 that land on 2026-10-01 all carry `revisit=2026-09-30` with `owner=security-audit` or
`owner=cpp-audit` — a single sweep's default date, not 25 exemptions that genuinely come due the
same Tuesday. The 2027-01-01 spike is the same story with `revisit=2026-12-31`.

## Why it matters

The failure lands on whoever happens to push that morning, not on the owner of any of the 25
markers, and it lands on all of them at once. The gate is correct — the audit loop is *supposed* to
force a re-evaluation — but a same-day cohort converts "re-evaluate one exemption" into "re-evaluate
25 or bypass the gate", and bypassing is the outcome that actually happens under deadline. An
`--admin` merge past it is exactly what `postmortem-owed.sh` flags, so the cliff manufactures the
incident it then reports.

## Concrete next action

1. **Before 2026-09-30**, re-evaluate the 25-marker cohort (they are listed in the audit's retarget
   table) and give each an outcome: retire it, or re-date it to a *staggered* date, or convert it to
   `revisit=never` where the exemption is genuinely standing. Most are the include-prologue class in
   [`2026-08-16-dup-auditor-flags-include-prologues.md`](../tooling/2026-08-16-dup-auditor-flags-include-prologues.md)
   and should be retired by fixing the auditor, not re-dated.
2. **Stop the class regenerating**: a sweep that stamps N markers must not give them all one date.
   Add a check to the deviation well-formedness gate proposed in
   [`2026-08-16-wrapped-deviation-markers-invisible-to-gate.md`](../tooling/2026-08-16-wrapped-deviation-markers-invisible-to-gate.md)
   that WARNs when more than ~8 first-party markers share a single `revisit=` date. Enumerator: the
   `revisit=` values `compute_wide_violations` already parses, bucketed by date. Replaying the
   motivating case against it: today's tree has buckets of 54 (2026-12-31) and 25 (2026-09-30), both
   of which would WARN.
3. `AGENTS.md` § Tiered enforcement should say plainly that `deviation-overdue` is whole-tree and
   merge-blocking, not diff-scoped. Today a reader has to infer that from the script.

Triggered-follow-up: when=date:2026-09-20; action=confirm the 25-marker 2026-09-30 cohort has been re-evaluated before it fires; baseline=25 markers dated 2026-09-30 as of 2026-08-16; fired=never
