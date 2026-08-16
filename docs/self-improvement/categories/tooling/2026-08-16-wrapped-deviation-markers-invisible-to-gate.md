# 47 `SMATCHET_DEVIATION` markers are wrapped across lines and invisible to every gate

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-08-16
- **Observed on**: the full deviation re-evaluation, [`docs/audits/DEVIATION_AUDIT_2026-08-16.md`](../../../audits/DEVIATION_AUDIT_2026-08-16.md) § S2
- **Status**: open

## What happened

The bash gate matches `DEV_RE='SMATCHET_DEVIATION\(([^)]*)\)'`, which requires the closing paren on
the **same line**. 47 live first-party markers open on one line and close on a later one, so
`DEV_RE` never matches them: the line is treated as ordinary prose, and **both the suppression and
the expiry are lost**. The Python auditors (`dup_audit`, `function_size_audit`,
`appcontroller_fan_in_audit`, `include_cycle_audit`) are per-line too — their "nearest non-blank
line above the target" is the marker's trailing prose, which carries no token, so a wrapped marker
survives only via `dup_audit._suppressed`'s "anywhere within the clone span" fallback, which
[`cpp-rules.md`](../../../agent-rules/cpp-rules.md) itself warns is accidental and intermittent.

Where they are: `Source/Core/include/Tracker/{GitHub,Jira,Linear,Plane}Client.h` (21),
`Source/Core/src/Tracker/*` (8), the three AI provider clients (5), `Source/Standalone/Cli*` (4),
9 others.

Two sibling fixture backends make it legible — same rule, same reason, same code:

```
Source/Core/src/Tracker/TrackerFixtureBackendBase.cpp:25   marker on ONE line  -> suppressed, clean
Source/Core/src/Tracker/GitHubFixtureBackend.cpp:26        marker WRAPPED      -> NOT suppressed
```

Running the project's own `scan_file_slurp_file` over the tree today emits
`unbounded-file-slurp  Source/Core/src/Tracker/GitHubFixtureBackend.cpp:28`.

## Why it matters

`unbounded-file-slurp` is WARN-first, so nothing blocks today. A whole-tree sweep with every bash
scanner confirms `bare-json-parse-untrusted` and `catch-all-swallow` are currently clean — i.e. no
*blocking* rule is defeated right now. That is luck. The same wrap on a `bare-json-parse-untrusted`
or `no-detach` escape fails the merge gate for reasons unrelated to the author's change, and a wrap
on any marker removes it from `deviation-overdue` permanently and silently.

This is the tail of [`2026-08-05-clang-format-reflows-deviation-comments.md`](2026-08-05-clang-format-reflows-deviation-comments.md):
that entry's option 2 (`CommentPragmas`) shipped 2026-08-16 and stops *new* wrapping, but it does
not un-wrap the 47 already in the tree.

## Concrete next action

Two steps, in order — the second is unsafe before the first.

1. **Sweep**: re-word each of the 47 markers so the whole `SMATCHET_DEVIATION(...)` fits one line
   directly above its target, moving overflow prose to lines *above* the marker (the shape
   `cpp-rules.md` § "One line, directly above" prescribes). 47 judgement calls about `reason=`
   prose, not a mechanical edit — do it per-subsystem, `Tracker/*Client.h` first (21 of the 47, all
   the same "interface-mandated override-signature symmetry" text).
2. **Gate it**: add the well-formedness rule that option 3 of the 2026-08-05 entry proposed —
   fail on a `SMATCHET_DEVIATION(` with no balanced `)` on the same line, and on a marker missing
   `reason=` / `owner=` / `revisit=`. Enumerator: every line matching `SMATCHET_DEVIATION(` in
   `git ls-files 'Source/Core/**' 'Source/Plugins/**' 'Source/Standalone/**'` filtered to
   `.cpp/.h/.hpp` — the same file set `compute_wide_violations` already walks. Replaying the
   motivating case against that enumerator: `Source/Core/src/Tracker/GitHubFixtureBackend.cpp:26`
   appears in it, has no balanced `)` on the line, and would trip the gate — as would
   `Source/Core/include/AppController.h:989`, which has no `owner=` or `revisit=`. Run step 2 only
   after step 1, or CI red-walls on all 47 at once.

Triggered-follow-up: when=date:2026-09-15; action=re-run the wrapped-marker count and confirm the sweep landed before the 2026-10-01 overdue cliff; baseline=47 wrapped markers on 2026-08-16; fired=never
