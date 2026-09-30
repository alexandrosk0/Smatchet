# A `SMATCHET_DEVIATION` marker wrapped across comment lines never expires

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-09-30
- **Observed on**: the 2026-10-01 deviation renewal (the markers renewed or resolved alongside this entry)
- **Status**: open

## What happened

`deviation-overdue` reads markers one line at a time (`DEV_RE='SMATCHET_DEVIATION\((.*)\)'` in
`agents/scripts/project/lint-rules.d/00-common.sh`). When a marker's reason wraps onto following
comment lines, the `revisit=` field sits on a continuation line the rule never parses, so the marker
never expires: the fail-open direction. Eleven wrapped markers dated 2026-09-30 / 2026-10-01 were
past due without any gate noticing; the renewal rewrote them as single-line markers. Wrapped markers
with later dates remain in `Source/` (for example the backend-client headers under
`Source/Core/include/Tracker/`, `OllamaClient.cpp`, `OpenAiClient.cpp`). `dup_audit.py` has the same
per-line reading, so a wrapped marker may also fail to suppress the clone it was written for; its
`_ineffective_dup_deviation` diagnostic already names that cause.

Most of these markers were wrapped by clang-format before `CommentPragmas: '^ *SMATCHET_DEVIATION'`
protected them.

## Concrete next action

1. Rewrite the remaining wrapped markers as single-line markers, with the explanation kept as plain
   comment prose above them.
2. Make the grammar fail closed: in `scan_file_rules`, a comment line that contains
   `SMATCHET_DEVIATION(` but no closing `)` emits `deviation-overdue` ("marker must be one line"),
   like an empty `revisit=`. Add a `--selftest` case and a bats case.
