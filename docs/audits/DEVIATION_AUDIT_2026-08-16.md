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

### The 9 `ADR-0020 / debt.md` markers: trigger NOT fired — keep them

Worth spelling out, because the obvious reading is wrong and an adversarial pass caught it.

The tempting conclusion is that this trigger has fired. `debt.md:67` records that the narrowing
ADR-0020 deferred — hoisting `DeadLetterRestoreSummary` / `*DeleteSummary` / the pack types into
`Source/Core/include/Sync/OfflineQueueTypes.h` — **has shipped** (PR #1282), and
`OfflineQueueService.h` / `TicketSyncService.h` do now include those rather than `AppController.h`.
Fan-in has also dropped 115 → 72.

Neither is the trigger. It says *"when **AppController.h** is narrowed"* — the header's own
surface. That header is **1641 lines today**, against ~1110 when the debt entry was filed and 1410
at its 2026-06-20 re-verify: it has grown ~48%, never narrowed. `debt.md:61` still reads
*"partially applied"*; `debt.md:59` warns in as many words **"do NOT confuse with the shipped
`appcontroller-fan-in` plan … a DIFFERENT axis"**; and `debt.md:60` names the exact residual that
would fire these markers — the Lua-bindings, AI-assistant and MCP-activity concerns are *"NOT yet
extracted"*.

So all 9 are **KEEP**. The trigger is live and pending, not spent, and it is causally coupled to
these files: `AppController_LocalCacheDb.cpp:16-21` documents that it must include
`OfflineQueueService.h` / `TicketSyncService.h` precisely because `AppController.h` only
forward-declares those members — an artifact of the last narrowing. Retargeting them to `never`
would delete the hook for work `debt.md` still tracks.

The datum worth surfacing on its own: **the god-object grew 48% while its P2 debt entry sat open.**

## S8 · `revisit_overdue()` compared strings, not dates — FIXED HERE

`00-common.sh` validated `revisit=` with a shell glob and then compared **lexically** against
`date +%Y-%m-%d`. Anything that missed the glob fell through to the slug branch and became a
**permanent exemption**, silently — the marker kept suppressing, so nothing surfaced. Three shapes
reached that branch, each proved against the real scanner:

| value | why it never expired |
|---|---|
| `2026-19-30` | month 19 passes the `[0-1][0-9]` glob and string-sorts after any real today |
| `2020-1-1` | a genuinely **past** date, but not zero-padded, so it misses the glob entirely |
| `2026-02-30` | a day that does not exist in that month; nothing parses the date |

The second is the dangerous one: a real deadline turns permanent because of a formatting slip.

**Fixed in this change**: `revisit_datelike_but_invalid()` rejects a value that clearly *meant* to
be a calendar date but is not one, and `revisit_overdue()` now **fails closed** on it — a typo'd
revisit gets re-written instead of quietly buying an exemption. Pure bash (no `date -d`) so
git-bash and the ubuntu runner agree. Regression fixtures `deviation-revisit-malformed.cpp` (all
three must fire) and `deviation-revisit-valid-shapes.cpp` (a real leap day, the `YYYY-Qn` form,
`never`, and free-prose triggers must stay silent — the over-reach guard), plus two bats cases.
No live marker is affected: whole-tree overdue is still 0.

Not covered, because it is a wrong *key* rather than a wrong value: `revist=2020-01-01` (typo)
still reads as "no revisit at all" and is indistinguishable from a marker that omits it. That
belongs to the well-formedness gate deferred in S3/S4.

---

# The ledger — per-marker re-evaluation

Method: seven parallel auditors, one per marker family, each required to ground a verdict in a
path/symbol it actually read; then an adversarial pass over **every** non-KEEP verdict, instructed
to refute and to default to refuted when it could not independently reproduce the evidence. Where
family scopes overlapped, 45 markers were judged twice — useful signal in itself, recorded below.

## Retire — markers that suppress nothing

The decisive test is not "is the reason still true" but "does this marker suppress any live clone".
I ran it three ways and they agree: each auditor drove `dup_audit._suppressed()` per marker; the
skeptic re-derived it independently from `find_clones(streams_head())`; and I checked it a third
way, geometrically, against the 695-clone head corpus (`inert.json`). **The skeptic upheld 23 of 24
retire verdicts.**

| marker | why it is inert |
|---|---|
| `Source/Core/src/AttachmentAppUpdateService.cpp:8, :10, :24` | earliest clone in the TU starts at 53; targets are `#include <atomic>` / `<cctype>` / `"AttachmentMimeUtils.h"` |
| `Source/Core/src/Commands/CommandRegistry.cpp:424` | **zero** clone occurrences anywhere in the file; the marker also sits ~400 lines below the include block its reason describes |
| `Source/Core/src/Tracker/GitHubProjectsV2Pure.cpp:291` | **zero** clone occurrences in the file |
| `Source/Core/src/Ui/SmatchetUI.cpp:1, :5, :7` | five occurrences in the TU, earliest starts at 13; none reaches lines 1-8 |
| `Source/Core/src/Tracker/GitHubClient.cpp:310, :735, :1156` | no occurrence within ±40 lines of any of the three |
| `Source/Core/src/Tracker/JiraIssueSearch.cpp:479, :637` | `:479` is spliced *into* a string-concatenation initializer; both coverage sets empty, neighbours already covered by `:486` / `:591` / `:662` |
| `Source/Core/src/Tracker/PlaneIssueMutation.cpp:151` | 8 occurrences in the TU, none within ±40 lines |
| `Source/Core/src/Tracker/PlaneIssueSearch.cpp:654` | coverage set empty; the nearest clone is suppressed from the Jira side |
| `Source/Core/src/Tracker/TrackerGridFieldDisplay.cpp:16` | span 18-40 is covered twice over — by `:30` (in-span) and from the `TrackerDateTimeFieldEditor.cpp` side |
| `Source/Core/include/Tracker/JiraClient.h:48` | wrapped 48-49, so `DEV_RE` never matches it *and* its coverage set is empty |
| `Source/Core/src/Tracker/LinearFixtureBackend.cpp:49` | wrapped 49-51; coverage empty; prose is near-verbatim redundant with the plain comment at 52-54 |
| `Source/Core/src/Tracker/GitHubIssueSearch.cpp:492` | its "next non-blank line" is *another marker* at 494, and `10-line-rules.sh` resets `prev_dev_rule` on re-match — it can never suppress via the nearest-above path; 494 covers the same span |
| `Source/Core/src/Tracker/JiraIssueMutation.cpp:558` | only span 552-566 contains it, and `:551` already covers that span as nearest-above |

**Deleting these cannot change the gate's verdict**, and not by luck: `dup_audit.new_clones_vs()`
computes `changed_files` by comparing *normalized token streams*, and comments are stripped before
normalization. Removing a comment line leaves the token stream identical, so the file stays
"unchanged" and its clones stay grandfathered.

### The one retire the skeptic refuted

`Source/Core/src/Tracker/JiraUserAndMeta.cpp:506` — **relocate, do not delete.** Its own target
(`ParseBounded` at 507) is genuinely clone-free, so it looked inert. But five real, *unsuppressed*
occurrences sit just above it in the same `FetchGroupMembers` loop — 465-473, 471-480, 491-501 and
493-501 (×2), cloned against `GitHubActivityFeed.cpp`, `JiraActivityFeed.cpp` and
`PlaneActivityFeed.cpp`. The marker is misplaced by ~15 lines, not obsolete; deleting it would be
gate-safe today and would silently discard the exemption's intent. Move it to line 490, the nearest
non-blank line above span 491.

This is the case for running the adversarial pass at all: "suppresses nothing" and "is not needed"
are different claims, and only the second justifies deletion.

## S9 · `BASELINE_FAN_IN` is stale by 43

`appcontroller_fan_in_audit.py --selftest` reports live `AppController.h` fan-in = **72** vs
documented `BASELINE_FAN_IN` = **115**, a figure also copied into the AGENTS.md contract-card.
Enforcement is unaffected — the ratchet is the `--diff` merge-base set-difference, not the constant
— but a 43-includer gap between the published cap and reality misleads every reader of the
contract-card. Not fixed here (out of scope of a deviation audit); noted for a follow-up.
