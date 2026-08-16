# `SMATCHET_DEVIATION` re-evaluation — 2026-08-16

Full re-evaluation of every live deviation in the tree. Every number here comes from running the
project's own scanners, or from a parser that reproduces their exact regex.

**Headline: the audit loop is not running.** Nothing is overdue today, but that is because the gate
cannot *see* 57% of the markers, not because the exemptions are current. Fix the plumbing before
reading anything into a green `deviation-overdue`.

## Inventory

462 markers repo-wide; **201 are live first-party `Source/` deviations**. The rest are gate
self-tests (`agents/scripts/**`), fixtures (`tests/**`), and grammar examples in docs — none of
which any gate scans. There are no deviations in `CMakeLists.txt` or `cmake/`.

| rule | live markers |
|---|---|
| `duplication` | 177 |
| `app-controller-fan-in` | 15 |
| `bare-json-parse-untrusted` | 6 |
| `unbounded-file-slurp` | 2 |
| `function-too-long,duplication` | 1 |

## Can the gate ever expire these?

`deviation-overdue` is the rule that forces the audit loop. Running the gate's exact regex over
every live marker and asking whether `revisit_overdue()` could ever return true:

| | count | |
|---|---|---|
| **E** gate can eventually expire it | **88** | 43% |
| **A** wrapped across lines — gate sees no marker at all | 47 | |
| **D** slug / conditional-future prose | 56 | |
| **B** author wrote a real date, paren-truncation discards it | 8 | |
| **C** no `revisit=` written at all | 2 | |
| | **113 never expire** | **57%** |

A, B and C are three independent defects. Each is confirmed below.

---

# Systemic findings

## S1 · `deviation-overdue` drops any `revisit=` that follows a parenthetical — FIXED HERE

`lint-rules.d/00-common.sh:73` — `DEV_RE='SMATCHET_DEVIATION\(([^)]*)\)'`. The `[^)]*` stops at the
**first** `)`, so a `reason=` containing `(...)` hides everything after it, `owner=` and `revisit=`
included. `rule=` precedes any paren, so **suppression keeps working while expiry silently
disappears** — the fail-open direction.

Proof, driving the real `scan_file_rules`:

| marker | revisit | `deviation-overdue` fired? |
|---|---|---|
| `reason=plain past date, no parens` | 2020-01-01 | yes |
| `reason=past date but reason has a (parenthetical) in it` | 2020-01-01 | **no** |

40 live markers are truncated this way; for **8** the author wrote a real calendar date the gate
discards: `Tracker/LinearClient.cpp:523` (2026-10-01), `Ui/MarkdownToHtml.cpp:231,:273,:301`,
`OllamaClient.cpp:27`, `Commands/Scenarios/AiChatHistoryRenderScenario.cpp:23`,
`Commands/Scenarios/InteractiveGridStressScenario.cpp:42`, `Ui/SmatchetQuickCreateIssueUi.cpp:9`
(all 2026-12-31).

The Python auditors are immune for *suppression* — they only
`re.search(r"rule=([A-Za-z0-9_,-]+)")` — but none of them implements expiry. Expiry lives **only**
in the broken bash path.

**Fixed in this change**: `DEV_REVISIT_RE` reads `revisit=` off the whole marker line, bounded by
the next `;` or `)`. Regression fixtures `deviation-{overdue,current}-paren-reason.cpp` + two bats
cases. Whole-tree overdue count today is **0 before and after** — no new CI failure — while the
2027-01-01 projection correctly rises 87 → 94 as the 8 hidden dates rejoin the loop.

## S2 · 47 markers are already wrapped across lines; the gate cannot see them at all

The bash gate needs the closing paren on the **same line**. 47 live markers open on one line and
close on a later one, so `DEV_RE` never matches and **both suppression and expiry are lost**.

Concentrated in `Source/Core/include/Tracker/{GitHub,Jira,Linear,Plane}Client.h` (21),
`Source/Core/src/Tracker/*` (8), the three AI provider clients (5), `Source/Standalone/Cli*` (4),
and 9 others.

Two sibling fixture backends make the failure legible — same rule, same reason, same code:

```
Tracker/TrackerFixtureBackendBase.cpp:25   marker on ONE line  -> suppressed, clean
Tracker/GitHubFixtureBackend.cpp:26        marker WRAPPED      -> NOT suppressed
```

Running the project's own `scan_file_slurp_file` over the tree today emits
`unbounded-file-slurp Source/Core/src/Tracker/GitHubFixtureBackend.cpp:28` — the marker two lines
above it is invisible. That rule is WARN-first, so nothing blocks today. I swept the whole tree
with every bash scanner: `bare-json-parse-untrusted` and `catch-all-swallow` are clean, so no
*blocking* rule is currently defeated this way. That is luck, not design.

**Not fixed here** — 47 markers need re-wording to fit one line, which is 47 judgement calls about
`reason=` prose, not a mechanical edit. Filed as
`docs/self-improvement/categories/tooling/2026-08-16-wrapped-deviation-markers-invisible-to-gate.md`.

## S3 · `clang-format -i` rewrites 15 more markers on contact — FIXED HERE

`.clang-format` set `ColumnLimit: 120` and no `CommentPragmas`. 73 live markers exceed 120 columns;
58 survive only because someone hand-wrapped them in `// clang-format off` / `// clang-format on`.
**15 had no such guard**:

| rule | n | markers |
|---|---|---|
| `duplication` | 8 | `CommandRegistry.cpp:424`, `GitHubActivityFeed.cpp:67,78`, `JiraUserAndMeta.cpp:371,433,506`, `LinearClient.cpp:523`, `SmatchetOfflineQueueUi.cpp:5` |
| `bare-json-parse-untrusted` | 4 | `IssueDraft.cpp:256`, `PlaneIssueMappingPure.cpp:223`, `PlaneIssueSearch.cpp:98`, `PlaneProjectScope.cpp:13` |
| `app-controller-fan-in` | 3 | `SmatchetActiveProjectGridCells.cpp:7`, `SmatchetActiveProjectGridTable.cpp:7`, `SmatchetPreferencesUi_General.cpp:36` |

End-to-end proof on `PlaneProjectScope.cpp` with the real `scan_bare_json_parse_file`: in-tree →
clean; after `clang-format` → `bare-json-parse-untrusted ...:15`, **gate FAILS**.

`scripts/dev/pre-ship.sh:429` runs `clang-format -i` on every changed first-party TU *before* the
gate, and no CI job checks formatting (`grep clang-format .github/workflows/*.yml` → empty), so the
drift stays invisible until someone touches one of those 12 files. Per-file clang-format drift
there is 3–12 lines and is essentially *only* the marker lines.

This is the open P2 finding
[`2026-08-05-clang-format-reflows-deviation-comments.md`](../self-improvement/categories/tooling/2026-08-05-clang-format-reflows-deviation-comments.md),
still unimplemented a fortnight later.

**Fixed in this change**: its option 2 — `CommentPragmas: '^ *SMATCHET_DEVIATION'`. Verified across
all 110 marker-holding TUs: marker lines clang-format would rewrite goes **15 → 0**, with no other
formatting change attributable to the pragma. Its option 3 (a gate that fails on a wrapped marker)
is **not** shipped here: it would red-wall CI on the 47 markers in S2 on the first run. Sequence is
S2 first, then the gate.

## S4 · Nothing validates marker well-formedness

Two markers carry no `owner=` and no `revisit=` at all —
`Source/Core/include/AppController.h:989` and `Source/Core/src/Tracker/LinearIssueMutation.cpp:85`,
both the shape `SMATCHET_DEVIATION(rule=duplication): <prose>`. That parses (only `rule=` is read),
suppresses, and can never enter the audit loop or be routed to an owner.

There is no gate anywhere that checks a marker has the four documented fields, sits on one line, or
sits directly above its target. **Participation in the audit loop is opt-in** — which is what makes
S1–S4 survivable for months. The gate that closes this is the same gate S3 defers.

## S5 · The expiry cliff

`deviation-overdue` runs `compute_wide_violations()` over the **whole tree** (not the diff),
absolute, no grandfathering; any hit sets `rc=1` at `test-lint-rules.sh:705` and the gate fails —
blocking every merge for everyone until all of them are re-dated or removed.

Stubbing `today_ymd()` and re-running the real `compute_wide_violations` (post-S1-fix figures):

| date | markers overdue | gate |
|---|---|---|
| 2026-08-16 (today) | 0 | green |
| **2026-10-01** | **25** | **RED — all merges blocked** |
| 2026-10-02 | 27 | RED |
| 2026-12-02 | 34 | RED |
| **2027-01-01** | **94** | **RED — in a single day** |

The dates cluster because they were stamped in bulk by sweeps (`owner=security-audit`,
`owner=cpp-audit`, `owner=tracker`), not because 25 unrelated exemptions genuinely expire the same
Tuesday. A calendar `revisit=` was the wrong instrument for most of them: see the retarget list.

## S6 · 55% of `duplication` exemptions grandfather an include block, not logic

Of the 131 single-line `duplication` markers in `Source/`, **73 (55%)** sit above an `#include` /
`using` / `namespace` prologue or say so in their reason. Root cause: `dup_audit.py` tokenizes
preprocessor directives like any other code (`normalize_token` maps identifiers to `ID`; there is
no include-block skip anywhere in the file), so a shared include prologue of ~70+ tokens registers
as a copy-paste clone. Every god-file split therefore costs one exemption per sibling TU.

That is exactly the condition `Source/Core/src/Ui/SmatchetUI_MainMenu.cpp:12` names —
`revisit=when the dup auditor scopes cross-file clones to logic blocks`. It has **not** fired.
Teaching `dup_audit.py` to skip contiguous preprocessor runs would retire ~73 exemptions at once
and stop the class regenerating. Filed as
`docs/self-improvement/categories/tooling/2026-08-16-dup-auditor-flags-include-prologues.md`.

## S7 · 56 markers hang on triggers nobody owns

24 distinct non-calendar triggers. The largest:

| n | trigger |
|---|---|
| 9 | `when AppController.h is narrowed per ADR-0020 / debt.md` |
| 6 | `dup-scoping` |
| 5 | `when a shared CliCommandRunner TU prologue header is introduced` |
| 4 | `when a shared AppController_LuaBindings TU prologue header is introduced` |
| 4 | `when a shared ConfigManager TU prologue header is introduced` |
| 3 | `when a shared MarkdownConvert TU prologue header is introduced` |
| 3 | `when a shared Grid TU prologue header is introduced` |

`dup-scoping` appears **nowhere in the repo** outside the six markers citing it — not in
`docs/plans/`, not in `backlog/`, not in `docs/self-improvement/`. A slug pointing at no tracked
work is a permanent exemption in slug costume; `revisit=never` at least says so honestly.

## S8 · `revisit_overdue()` compares strings, not dates

`00-common.sh:77` lexically compares `YYYY-MM-DD` against `date +%Y-%m-%d`, so it accepts
impossible dates. `revisit=2026-02-30` and `revisit=0000-Q1` both appear (in
`agents/scripts/core/test-plan-claim-anchors.sh` — fixture-only, harmless). A typo'd `2026-13-01`
in real source would sort as a valid future date and never expire. Low severity, zero current
impact; noted so the well-formedness gate covers it.

## S9 · `BASELINE_FAN_IN` is stale by 43

`appcontroller_fan_in_audit.py --selftest` reports live `AppController.h` fan-in = **72** vs
documented `BASELINE_FAN_IN` = **115**, a figure also copied into the AGENTS.md contract-card.
Enforcement is unaffected — the ratchet is the `--diff` merge-base set-difference, not the constant
— but a 43-includer gap between the published cap and reality misleads every reader of the
contract-card. Not fixed here (out of scope of a deviation audit); noted for a follow-up.
