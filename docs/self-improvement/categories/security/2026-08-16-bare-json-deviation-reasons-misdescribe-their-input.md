# Three `bare-json-parse-untrusted` exemptions describe input they do not actually receive

- **Category**: security
- **Priority**: P1
- **Date**: 2026-08-16
- **Observed on**: the full deviation re-evaluation, [`docs/audits/DEVIATION_AUDIT_2026-08-16.md`](../../../audits/DEVIATION_AUDIT_2026-08-16.md)
- **Status**: open

## What happened

`bare-json-parse-untrusted` exists because a bare `nlohmann::json::parse` builds a DOM whose
**recursive `~json` teardown** overflows the stack on a deeply-nested payload — an uncatchable
crash no `try/catch` intercepts (#1271 / #1287 / #1290), and the 3-arg non-throwing form does not
help because it still builds the full DOM. The rule is repo-wide default-deny; six first-party
sites escape it with a deviation asserting their bytes are program-internal.

Re-evaluating those six against the code that actually feeds them, three assertions do not hold.

**1 · `Source/Core/src/Config/KeybindingsConfig.cpp:153` and `:175`** —
`reason=keybinding args are app-serialised local config bytes loaded via the bounded config
reader, not external ingress`. The bounded config reader bounds the *outer* keybindings document.
`ArgsJson` is a `std::string` **field inside** it (`Config/KeybindingsConfig.h:27`, "command args
as JSON text; parsed at dispatch"), so to the outer `ParseBounded` a hostile payload is a single
depth-1 string node — the outer bound gives the inner parse no protection whatsoever. The tree
already disagrees with the reason in two places: the same `ArgsJson` bytes go through
`smatchet::json_safe::ParseBounded` at `Source/Core/src/Ui/SmatchetToolbarUi.cpp:132` and
`Source/Core/src/Ui/SmatchetImGuiHost.cpp:1060`. One field, three parse sites, and only this one
exempts itself.

That is not a coincidence, and it is the sharpest part of this entry. **`docs/audits/CPP_CODE_AUDIT.md`
finding #12 is this exact class** — *"[Low][Security/DoS] Bare `json::parse` on toolbar/keybinding
`ArgsJson` (2 sites) … Config-sourced `ArgsJson` (from `smatchet_config.json`) bare-parsed; deep DOM
→ uncatchable teardown crash. Fix: `ParseBounded` with empty-object fallback."* It is marked
**✅ Fixed** in that audit's summary table (line 16). The two sites it named
(`SmatchetToolbarUi.cpp`, `SmatchetUI.cpp`) *were* fixed, with precisely that fix. The class then
recurred in `KeybindingsConfig.cpp`, which the curated two-site list did not watch — and instead of
the prescribed one-line fix, that site received a deviation asserting it was safe.

This is the documented failure mode of a curated allow-list, quoted in `cpp-rules.md` as the reason
`bare-json-parse-untrusted` went repo-wide default-deny in the first place: *"the list lagged the
code every time the class recurred (#1573 / #1592 / #1598)."* Default-deny did its job and caught
the recurrence; the deviation then waived it on a premise the audit had already rejected.

**2 · `Source/Core/src/Tracker/PlaneProjectScope.cpp:13`** (and the same reason at
`PlaneIssueMappingPure.cpp:223`, `PlaneIssueSearch.cpp:98`) —
`reason=the Plane structured-query blob is app-serialised program-internal bytes, not
tracker-response ingress`. Traced the argument: `SetProjectInQuery(currentJql, …)` is called from
`Source/Core/src/Ui/SmatchetViewsDashboardUi_widgets.cpp:293`, where `currentJql` is
`std::string(d.viewJqlEditor.buf)` (`:245`) — the **query text the user types into the view
editor**, which is then persisted with the saved view. That is user-authored input, not
app-serialised bytes.

**3 · `Source/Core/src/Tracker/TrackerFixtureBackendBase.cpp:25`** (`rule=unbounded-file-slurp`) —
`reason=developer-authored local fixture file, small by construction`. Three lines above it, the
header this file implements says the opposite in writing
(`Source/Core/include/Tracker/TrackerFixtureBackendBase.h:77-79`): *"The fixture path is
env-var-selectable per backend (`SMATCHET_TEST_*_BACKEND_FIXTURE`), so the parse caps
depth/nodes/bytes rather than trusting wherever it points."* The *parse* is indeed
`ParseBoundedOrDiscarded` — but the exemption is on the **slurp**, `buf << in.rdbuf()`, which
reads the whole file into memory before any cap applies.

## Why it matters

None of these is remotely reachable, and the realistic worst case is a local self-inflicted crash
or memory spike, so this is not an urgent exploit. What matters is the audit trail: a deviation is
the project's record of *why* a blocking safety rule was waived, and the next auditor re-evaluating
these in 2026-12 will read a provenance claim that is simply not what the code does. A wrong reason
is worse than no reason, because it terminates the inquiry.

Note also that all four Plane/IssueDraft markers in this class are among the 15 that `clang-format`
rewrites (fixed 2026-08-16 via `CommentPragmas`), and `GitHubFixtureBackend.cpp:26` — the wrapped
twin of item 3 — is failing open today.

## Concrete next action

Per site, cheapest first:

1. **KeybindingsConfig `:153` / `:175`** — delete both deviations and route through
   `smatchet::json_safe::ParseBounded`, exactly as `SmatchetToolbarUi.cpp:132` already does with
   the same field. The call sites already handle a failed parse (`is_discarded()` →
   `json::object()`), so `ParseBoundedOrDiscarded` is a drop-in. This removes the class rather than
   re-arguing it.
2. **Plane query blob (3 markers)** — same treatment; `ParseBoundedOrDiscarded` matches the
   existing `is_discarded()` handling line for line. If the exemption is kept instead, the reason
   must say what is true: *user-authored view-query text, bounded risk accepted because the blast
   radius is the author's own session*.
3. **`TrackerFixtureBackendBase.cpp:25`** — keep the exemption (a size cap on a test-fixture read
   is not worth the code), but rewrite the reason to match the header it contradicts:
   *env-var-selected local path, developer-controlled by deployment; parse is bounded, slurp is
   not*. Then fix the wrapped twin at `GitHubFixtureBackend.cpp:26`, which suppresses nothing today
   — `--scan-slurps` reports its slurp at `:28`.

Enumerator for the verification sweep: the six `rule=bare-json-parse-untrusted` and two
`rule=unbounded-file-slurp` markers are the complete first-party set —
`bash agents/scripts/project/test-lint-rules.sh --scan-bare-json` and `--scan-slurps` enumerate
exactly the sites they guard, and `--scan-bare-json` is empty tree-wide today. Replaying the
motivating case against that enumerator: drop the marker line at `KeybindingsConfig.cpp:153`
without touching the parse and `scan_bare_json_parse_file` immediately reports the parse — proving
the marker is load-bearing and the rule really does watch that line, so the deviation is the only
thing standing between this parse and the gate.
