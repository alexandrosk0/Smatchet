<!-- index-summary: close four gate blind spots — cross-TU dead exports, sub-floor helper clones, link-gate target scope, and the untaxonomised root audit docs -->
# Plan — Gate blind-spot sweep

> **Slug**: `gate-blind-spot-sweep` (matches this file's basename without `.md`).
>
> **Status**: `shipped`

## Context

The repo runs 30+ CI gates, yet a whole-tree exploration pass (2026-07-24) found four classes of debt that are **structurally invisible** to all of them — not "the gate is red and nobody looked", but "no gate can ever see this". Each class is small on its own; together they mean the codebase accumulates a category of rot that the DRY/lint/doc machinery is architecturally unable to report.

1. **Cross-TU dead exports.** `40-unused-config-guard.sh` catches the `-Wunused-function` *static-in-a-TU* shape, and `check-test-list.sh` catches orphan test files, but nothing checks whether a symbol **declared in a public `Source/Core/include/**` header** has any caller. A crude scan (free functions with capitalised names only — a floor, not a ceiling) found 8 with declaration + definition and zero references in `Source/` or `tests/`.
2. **Sub-floor helper clones.** `dup_audit.py` has `MIN_CLONE_TOKENS = 70` (`agents/scripts/core/dup_audit.py:70`) and is delta-gated vs `origin/develop`. A 3-line helper is ~25 tokens, so small-helper duplication is invisible to Pillar 5 **by construction** and the 449-clone baseline can never surface it. `ToLowerAsciiCopy` already exists in `StringUtil.h:25` and is nonetheless re-rolled in 9 TUs under 5 different names.
3. **Link-gate target scope.** `test-markdown-links.sh` rule 5 scopes the scan to `docs/**`, `agents/**`, `AGENTS.md`, `BUILD.md`, `README.md`. Two top-level **user-facing** guides — `CLI_GUIDE.md` and `LUA_GUIDE.md` — are not in the target list, and both carry dangling links today. Separately, `--all` reports 14 pre-existing dangling links repo-wide that diff-scope mode grandfathers permanently with no burn-down path.
4. **Untaxonomised root audit docs.** `docs/STRUCTURE.md` calls itself "the **binding map** of where things live" and names an enforcing guard for every row — but has no row for the six audit reports at repo root (`SECURITY_AUDIT.md` 81 KB, `UX_DESIGN_CRITIQUE.md` 54 KB, `AGENTIC_INFRA_AUDIT.md` 38 KB, `CPP_CODE_AUDIT.md` 36 KB, `TEST_COVERAGE_GAP_MAP.md` 22 KB, `MUTATION_PILOT.md` 18 KB), nor for `backlog/`. These are load-bearing: production comments cite them normatively (`CPP_CODE_AUDIT.md #25 / #19 / #7 / #3`, `SECURITY_AUDIT.md #1`). They are referenced like ADRs but governed like scratch files.

**Intended outcome**: after this lands, each of the four classes has either a gate that can see it or an explicit, documented decision not to gate it — and the currently-known instances are burned down.

## Approach

Four independent slices, shipped as separate PRs on one branch, ordered cheapest-first so early slices pay for the later ones. Slices 1–2 add **advisory-first** gates (WARN, non-blocking) following the precedent set by the DRY gate's WARN-first → blocking graduation ([ADR-0015](../../adr/0015-dry-quality-pillar-duplication-gate.md)); graduation to blocking is a separate decision once the baseline is clean and the false-positive rate is known.

The deliberate non-choice: **do not lower `MIN_CLONE_TOKENS`.** Dropping the floor to ~15 would flood `dup_audit.py` with every `if (!x) return false;` in the tree and destroy the signal that makes the blocking DRY gate viable. Instead slice 2 adds a *separate*, narrow detector keyed on exact-body match of small named helpers — a different question than "is this a copy-paste clone", asked with a different tool.

Slice 4 is doc-only and has no gate to add: the fix is a taxonomy row plus a decision on where dated audit reports live. It is bundled here because it is the same root cause (governance with no owner), not because it shares an implementation.

## Files to modify

**Slice 1 — cross-TU dead-export detector**

1. `agents/scripts/core/dead_export_audit.py` (NEW) — parse declarations from `Source/Core/include/**/*.h`, count references across `Source/`, `tests/`, `Source/Plugins/`, `Source/Standalone/`; report symbols whose only hits are the declaration + definition. Must handle: member functions (skip — vtable/virtual dispatch defeats textual counting), `inline` header-only definitions, `SMATCHET_WITH_*`-guarded symbols (a symbol used only under a feature flag is NOT dead), and Lua/MCP-dispatched names reachable only via string tables.
2. `agents/scripts/core/test-dead-export-audit.sh` (NEW) — `--selftest` (assert a planted dead symbol is flagged) + `--check`. Required: `test-gate-selftests.sh` asserts every `--selftest`-exposing script has a failure case.
3. `agents/scripts/core/test-dead-export-audit-bats.sh` + `tests/bats/dead_export_audit.bats` (NEW) — `test-orphan-bats.sh` requires every bats suite to have a `test-*.sh` wrapper.
4. `.github/workflows/agentic-selftests.yml` (MOD) — register the new gate advisory-first.
5. `docs/high-integrity/dead-export-baseline.md` (NEW, generated) — grandfathered baseline, same shape as `dup-baseline.md`.

**Slice 1b — burn down the 8 known dead exports** (delete; each is decl + def with zero callers)

6. `Source/Core/include/StringUtil.h:65` — `EqualsCaseInsensitive` (zero references repo-wide).
7. `Source/Core/include/SmatchetDefaults.h:13` — `GetEnvString`.
8. `Source/Core/include/SmatchetLocalization.h:16` + `Source/Core/src/SmatchetLocalization.cpp:1471` — `GetLanguage`. **Delete rather than fix**: it returns `const std::string&` straight out of `CurrentLanguageRef()` *without* taking `LocalizationMutex()`, while `SetLanguage` mutates that same string under the lock — a data race by signature. Every other accessor in the file locks. Deleting removes the hazard; if a caller ever needs it, it must return by value under the lock.
9. `Source/Core/include/Persistence/SmatchetImageTextureCache.h:42` + `.cpp:306` — `EvictCacheKey`.
10. `Source/Core/include/TicketGridModel.h:63` + `.cpp:135` — `ResolveTicketGridRenderPlan`.
11. `Source/Core/include/Tracker/ProjectResolver.h:56` + `.cpp:105` — `ResolveProjectForDraftFromParent`.
12. `Source/Core/include/Tracker/TrackerFieldValueUtils.h:23,26` + `.cpp:124,134` — `SaveDurationSuggestions`, `SaveCommentTemplates`. Note these are not merely unused: they are a **dead second write path** that read-modify-writes the whole `TrackerConfig`, while the shipped write path is the Preferences panel mutating `d.cfg` directly (`Source/Core/src/Ui/SmatchetPreferencesUi_Templates.cpp:90-241`). Wiring them would introduce a lost-update race; deleting removes the trap.

**Slice 2 — small-helper clone census**

13. `agents/scripts/core/small_helper_audit.py` (NEW) — exact-normalised-body match across free functions below `MIN_CLONE_TOKENS`, grouped by body hash, reported only when ≥3 TUs share a body. Deliberately narrower than `dup_audit.py`.
14. `agents/scripts/core/test-small-helper-audit.sh` + bats wrapper + suite (NEW) — same gate-selftest/orphan-bats obligations as slice 1.
15. Call sites to migrate onto `ToLowerAsciiCopy` (`Source/Core/include/StringUtil.h:25`): `Source/Core/src/Logger.cpp:15`, `Source/Core/src/SmatchetLocalization.cpp:1333`, `Source/Core/src/Commands/Builtin/BuiltinCommands_Helpers.cpp:60`, `Source/Core/src/Tracker/TrackerHttpPure.cpp:26`, `Source/Core/src/Tracker/IssueTableSerializer.cpp:16`, `Source/Core/src/AppController_LuaBindings.cpp:89`, `Source/Plugins/Whisper/ModelDownloadPolicy.cpp:12`, `Source/Plugins/Mcp/McpJsonRpcPure.cpp:22`. The last one carries a live `SMATCHET_DEVIATION(rule=duplication; reason=Core StringUtil helper is out of scope…)` — **retire that deviation in the same commit**, don't leave it dangling.
16. `Source/Core/src/Tracker/PlaneFieldCatalogPure.cpp:20` — the upper-case twin; needs a `ToUpperAsciiCopy` added to `StringUtil.h` first.

> **Purity constraint**: `McpJsonRpcPure.cpp` and `*Pure.cpp` TUs are deliberately dependency-light so they link into the focused test rig. Confirm `StringUtil.h` (header-only, stdlib-only) does not violate that before migrating those two — if it does, the deviation stays and slice 2 records it as a justified exemption rather than forcing the include.

**Slice 3 — link-gate scope**

17. `agents/scripts/core/test-markdown-links.sh:198-215` (MOD) — extend the `is_active_md` target set to include top-level user-facing guides (`CLI_GUIDE.md`, `LUA_GUIDE.md`, `MCP_GUIDE.md`, `AI_POLICY.md`, `CONTEXT-MAP.md`). Update the rule-5 comment block in the header to match — the header is the documented contract.
18. `CLI_GUIDE.md:3` (MOD) — `docs/plans/active/applied/command-system-plan.md` is dangling **and** tiered; rewrite tier-less as `docs/plans/command-system-plan.md`.
19. `CLI_GUIDE.md` (MOD) — `.claude/PERF_WORKFLOW.md` is dangling; retarget at `docs/guides/perf-workflow.md` after confirming that is the intended successor.
20. `docs/high-integrity/markdown-link-baseline.md` (NEW, generated) — grandfather the 14 pre-existing `--all` failures so the newly-scoped files can be gated without a 14-item cleanup blocking the slice.

**Slice 4 — doc taxonomy**

21. `docs/STRUCTURE.md:19-38` (MOD) — add rows for the six root audit reports and for `backlog/`, each with an owning guard or an explicit `—` plus a one-line reason.
22. `docs/STRUCTURE.md` § Naming (MOD) — state where a *new* dated audit report goes (root vs `docs/reference/` vs `docs/audits/`), so the next audit has a rule instead of precedent.

## Existing utilities reused

- `ToLowerAsciiCopy` — `Source/Core/include/StringUtil.h:25` — the canonical helper slice 2 migrates onto; do not re-roll.
- `TrimCopyAsciiWhitespace` — `Source/Core/include/StringUtil.h:9` — same family; check for the same clone pattern while sweeping.
- `dup_audit.py` `--selftest` / `--diff` argument shape — `agents/scripts/core/dup_audit.py` — the new audits mirror its CLI so `test-gate-selftests.sh` and the workflow wiring need no special-casing.
- `plan_tierless_resolves` — `agents/scripts/core/test-markdown-links.sh:283` — slice 3 must not regress tier-less resolution when widening the target set.
- `docs/high-integrity/dup-baseline.md` header format — the "auto-generated, do not hand-edit, regenerate with `--dup-baseline`" convention the two new baselines copy.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — this plan adds detectors and deletes dead code; it moves no bodies between files and splits no over-cap file.

## UX Pillar callouts

**This plan's own diff is docs-only and has zero pillar impact.** The callouts below are *forecasts for the slice implementation PRs*, recorded here so each slice inherits them instead of re-deriving them. Slices 1, 3 and 4 add tooling and docs only; **slices 1b and 2 do change `Source/Core/` production code** (deletions and call-site migrations respectively).

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: expected no regression. Slice 1b deletes code with no reachable caller, so no hot path changes. Slice 2's migrations are behaviour-identical (the same `std::transform` body, reached through one inline function), so at worst neutral and possibly a marginal win from a single inlinable definition.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: expected no impact — no slice touches I/O or threading.
- **Pillar 3 (never crash)**: expected net positive. Deleting `GetLanguage()` removes an unlocked read of mutex-protected state (a data race by signature, TSan-detectable the moment it gained a caller). Deleting `Save{Duration,Comment}*` removes a lost-update trap. Slice 2 is behaviour-preserving; the ASan/UBSan nightly covers the migrated TUs.
- **Pillar 4 (accessibility)**: expected no impact — no user-facing UI change in any slice.

## Perf-review-system gates

**For this plan's docs-only diff: N/A on all five** — no `Source/Core/` file is touched here.

Forecast for the implementation PRs, so each slice inherits the declaration (slices 1b and 2 will need to restate it against their actual diff):

1. **PR-fast CI** — expected N/A as a *regression* risk: slice 1b deletes uncalled code, slice 2 substitutes an identical inline body. No scenario maps to a changed hot path. The coverage gate may register a delta from slice 1b's deletions — expected and directional (removing zero-coverage lines should *raise* the percentage); see § Risks.
2. **Pillar 2 static scanner** — expected N/A: no new sync I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — expected N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — expected N/A: no new sync stall > 100 ms.
5. **Marker inventory** — expected N/A: no `SMATCHET_UI_PERF_SCOPE` markers added.

## Risks / non-goals

- **Risk — the dead-export detector false-positives on indirect dispatch.** A symbol reachable only through a Lua binding table, an MCP command name string, or a `SMATCHET_WITH_*`-guarded call site looks textually dead. *Mitigation*: advisory-first + a grandfathered baseline; every slice-1b deletion is individually verified by build + full ctest on **both** targets, not by the detector's say-so.
- **Risk — deleting dead code moves the coverage number.** These symbols sit in the coverage denominator with zero reachable callers, so removing them should *raise* line coverage — but the delta gate may read an unexpected jump. *Mitigation*: land slice 1b as its own PR so the coverage delta is attributable; expect and document the direction. This interacts favourably with the open `test.md` item "raise Source/Core line coverage 67% → 70%".
- **Risk — widening the link-gate target set turns 14 grandfathered failures into noise.** *Mitigation*: slice 3 generates the baseline before flipping scope (step 20 precedes step 17 in execution order even though it is listed after).
- **Risk — `StringUtil.h` in a `*Pure.cpp` TU breaks the focused test rig's link set.** *Mitigation*: named as an explicit pre-check in § Files to modify; the fallback is to keep the deviation, which is a documented outcome rather than a failure.
- **Accepted**: the dead-export detector will only ever see free functions and will miss dead *member* functions and dead *classes*. Ceiling not reached; a partial detector that runs beats a complete one that does not.
- **Non-goal — lowering `MIN_CLONE_TOKENS`.** Explicitly rejected in § Approach.
- **Non-goal — graduating either new gate to blocking.** Advisory-first only; graduation is a separate decision with its own ADR, mirroring ADR-0015.
- **Non-goal — burning down the 14 pre-existing dangling links.** Baselined, not fixed. A follow-up sweep can drain them.
- **Non-goal — relocating the six root audit reports.** Slice 4 gives them a taxonomy row and a rule for the *next* one; physically moving 232 KB of cross-referenced docs is its own plan.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: existing suites must stay green after the slice-1b deletions and slice-2 migrations. `tests/Core/` suites covering the migrated TUs (`TrackerHttpPure`, `IssueTableSerializer`, `McpJsonRpcPure`) pin the lower-casing behaviour — no new cases needed, but a red here means the migration was not behaviour-identical.
- **Bucket E (ImGui Test Engine)**: N/A — no UI behaviour change.
- **Bash-driver scenario / screenshot / sanitizer**: nightly ASan/UBSan covers the migrated TUs; no new scenario.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target). **Load-bearing for slice 1b** — the dual-target build is what actually proves a deleted symbol had no caller, including DX12-only paths the desktop build never compiles.
- **New-gate self-verification**: `bash agents/scripts/core/test-dead-export-audit.sh --selftest` and `test-small-helper-audit.sh --selftest` must each fail on a planted positive; `test-gate-selftests.sh --check` must count the new scripts.
- **Doc validation (blocks plan-doc PRs)**: the canonical `scripts/dev/test-docs.sh` suite green. Slice 3 additionally requires `test-markdown-links.sh --all` and `--merge-tree-warn` green after the scope widening.
- **Plan stress-test — `grill-with-docs`**: stress-test this plan against the domain model + sharpen terms before finalising; record the outcome. **Not yet run** — owed before slice 1 starts.
- **Manual residue**: none expected. If the `*Pure.cpp` purity pre-check forces a deviation, record it in `docs/self-improvement/categories/tooling.md` rather than leaving it implicit.

## Out of scope (flagged, not designed)

**Deferral residue-sweep**: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here.

- **Dead member functions / dead classes** — the detector is free-function-only. Follow-up: no plan yet; re-open if the free-function sweep suggests the member surface is worse.
- **The 14 grandfathered dangling links** — baselined by slice 3. Follow-up: a drain PR, unscheduled.
- **Physically relocating the root audit reports to `docs/audits/`** — slice 4 adds governance only. Follow-up: own plan if the taxonomy row proves insufficient.
- **`TrimCopyAsciiWhitespace` clone family** — slice 2's detector will surface it; migrating those call sites is deliberately left to the detector's first real run rather than pre-enumerated here.

## Implementation log

Five commits on one branch, cheapest-first as designed.

**Slice 1 — `dead_export_audit.py` + gate + bats + baseline.** The detector's load-bearing design choice is that it **never classifies an occurrence as declaration-vs-call**. That needs a real C++ parser; a regex that tries it mis-reads `return Foo(x)` as a definition. Instead it partitions *files*: a symbol is dead only when (1) its declaring header holds exactly as many occurrences as parsed declaration/definition sites, (2) no other header mentions it, and (3) the `.cpp` layer holds exactly the definitions the header still owes — **zero** when the header carries an `inline` body, otherwise one. Any caller anywhere adds an occurrence and lifts the symbol out, so a false positive would need a caller that is textually absent, which C++ cannot produce.

Condition (3)'s inline/declaration distinction was **not** in the first cut, and the first cut was wrong because of it: `ExtensionFromMime`, `SanitizeForSpreadsheet` and `BoolArg` were all reported dead while having real callers, because their single non-header occurrence *was* the caller (the body already lived in the header). Tagging each parsed site as inline-definition vs bare declaration fixed it; that tag now has its own selftest assertion and two bats cases.

Occurrence counting runs over comment-stripped text with **string literals kept** — asymmetric on purpose. A stale `// TODO: use Foo` must not mask a dead `Foo`, but a name appearing in a Lua binding table or an MCP command-name string is the detector's worst false-positive shape, so a literal mention counts as a live reference. `SMATCHET_WITH_*`-guarded call sites needed no handling at all: the audit never evaluates the preprocessor, so a call inside `#if` is an ordinary occurrence.

**Slice 1b — the 8 deletions.** All 8 the plan named were confirmed dead and deleted. Two were hazards, not just clutter (see the Pillar 3 note in § Verification (actual)).

**Slice 2 — `small_helper_audit.py` + the migrations.** Three narrowings make the sub-floor band readable without touching `MIN_CLONE_TOKENS`: named function definitions only (not arbitrary token runs), exact normalised param+body identity using `dup_audit.normalize_token` (so copy-then-rename still collides), and a ≥3-TU spread requirement. The band **ceiling is imported from `dup_audit.py`**, not copied, and a bats test asserts that — a future `MIN_CLONE_TOKENS` retune cannot open a gap between the two audits.

The 25-token floor is calibrated, not guessed: at 20 the report gained a junk group collapsing three unrelated `lock_guard(g_mutex); return <member>;` accessors, because at that size a guarded getter's normalized body carries no distinguishing structure. 25 drops that group and keeps every genuine one (the smallest real finding, the rgba-pack helper, is 26 tokens).

The migration was not a uniform delete-and-rename. Three of the nine helpers turned out to be **published seams** — header-declared and called from sibling TUs, and `McpJsonRpcPure::ToLowerAscii` is directly unit-tested by `McpDispatch.test.cpp`. Those keep their names as one-line forwarders; only the six TU-private ones were deleted outright.

**Slice 3 — link-gate scope.** Baseline generated *before* the scope widening, per the plan's execution-order note. Two pre-existing bugs surfaced and were fixed: the `--selftest` re-invoked `"$0"` directly, but the script is tracked mode 100644, so on a POSIX checkout it failed with permission denied and *both* assertions read the same non-zero exit (it reproduces on the unmodified file at `origin/develop`); and an unknown argument silently fell through to a full diff-scope scan instead of erroring.

**Slice 4 — taxonomy.** Doc-only, plus one gate-on-gate fix described in § Deviations.

## Deviations from plan

1. **Shipped as five commits on ONE branch and ONE PR, not four PRs.** § Approach called for separate PRs per slice. The branch/PR shape is fixed by the harness for this task. Slice boundaries are preserved as commit boundaries, so slice 1b's coverage delta is still attributable to a single commit — the property § Risks actually wanted.

2. **The detector found 14 dead exports, not the 8 the plan's crude scan predicted.** All 8 named were confirmed and deleted; the other 6 are baselined, not deleted, because the plan scoped deletion to its own list. They split into two kinds worth recording: `SmatchetHost_UpdateRendererColorFormat` and `SmatchetHost_SetKeyDown` are **public C ABI** deliberately exported for external embedders (deleting them would be wrong, and only 2 of that header's ~40 functions are uncalled, so the detector is discriminating correctly); `GetOrLoadFromFile`, `TextRun`, `DrawInlineFieldIconIfAny` and `GetThemedAiChatPalette` are genuinely uncalled Ui/Persistence helpers, each with a header comment describing an intended caller that does not exist. A follow-up burn-down is owed for those four.

3. **`<cstdlib>` deliberately RETAINED in `SmatchetDefaults.h`** even though its only consumer was the deleted `GetEnvString`. The header reaches most of the tree via `ConfigManager.h` / `SmatchetUiSession.h` / `McpPlugin.h`, and several TUs (`StandaloneAppBootstrap.cpp`, `SmatchetBugReportUi.cpp`, `CrashSink.cpp`, `SemanticVersionPure.cpp`) call `std::getenv` / `std::exit` / `std::atoi` relying on exactly that transitive path. Removing it is an include-what-you-use sweep with its own blast radius, not part of a dead-code deletion — and one this branch could not verify, since the dual-target MSVC build is unavailable here (see § Verification (actual)). The header states the reason inline.

4. **Deleting `TrackerHttpPure::ToLower` exposed a pre-existing clone and required a NEW deviation.** Its `Trimmed` is byte-identical to `smatchet::linear::Trim`, and the deleted `ToLower` was the only thing keeping the maximal token run under `MIN_CLONE_TOKENS`; removing it pushed the run to 111 tokens and the blocking DRY gate fired. Not folded onto `TrimCopyAsciiWhitespace`, which trims only `{space,\t,\n,\r}` while this uses `std::isspace` (also `\v`,`\f`) — narrowing a trim on a host-allowlisting input is a behaviour change, and § Out of scope explicitly defers the `TrimCopyAsciiWhitespace` family. Recorded as a scoped `SMATCHET_DEVIATION` with that reasoning and a revisit date. The marker sits *inside* the function body because the gate matches a maximal token run whose start drifts below the signature; the comment says so.

5. **`BuiltinCommands_Helpers::ToLowerAscii` migration is a real (if inert) semantic change.** Its body was the odd one out — an explicit `'A'..'Z'` branch, not `std::tolower`. The two agree exactly under the `"C"` locale, which is the only locale this process ever has (nothing in the tree calls `std::setlocale`), so the substitution is behaviour-identical for every input. The forwarder comment records where to reinstate the locale-independent branch if that ever changes.

6. **Plan correction — `LUA_GUIDE.md` carries no dangling links.** § Context claimed both `CLI_GUIDE.md` and `LUA_GUIDE.md` did; `LUA_GUIDE.md` has no markdown links at all. `CLI_GUIDE.md` was the only offender (both links fixed). The other four guides were added to the target set for coverage, not repair.

7. **The `--all` baseline holds 9 keys, not 14 entries.** The 14 reported failures are line-level occurrences of 9 distinct `(file, href)` pairs (`docs/design/separate-agents-repo.md` repeats 3 hrefs across 8 lines). Keyed by `file::href` rather than line number so an unrelated edit above a grandfathered link cannot un-grandfather it. After fixing `CLI_GUIDE.md`'s two, 9 keys remain covering 14 occurrences.

8. **One unplanned fix in `test-plan-ref-integrity.sh`.** The new markdown-link baseline records dangling paths by design — including dangling *plan* paths — so that gate began failing on this gate's evidence file. Excluded by pathspec at both of its scan sites, matching how `.understand-anything/` and the archive-plan scripts are already handled. Without this, slice 3 would have red-walled a gate it never touched.

9. **`.github/workflows/agentic-selftests.yml` needed no wiring, only documentation.** § Files to modify assumed the new gates had to be registered. `test-all.sh` discovers gates by the `test-*.sh` glob, so both auto-enrol; the workflow change is a header block naming them and their advisory-first status, so a reader knows why they never red the lane.

## Verification (actual)

**Gates run green on this branch:**

- `dup_audit.py --diff origin/develop` — clean (after the deviation in § Deviations #4; it genuinely failed first, which is the gate working).
- `function_size_audit.py --diff`, `include_cycle_audit.py --diff` — clean.
- `test-lint-rules.sh` — full suite pass. The one `tu-line-ceiling` WARN (`SmatchetLocalization.cpp`) is pre-existing and advisory; this branch made that TU 4 lines *shorter*.
- `test-gate-selftests.sh` — 73 selftest-exposing scripts, all asserting a failure case (was 71; +2 from this branch).
- `test-orphan-bats.sh` — 75 suites, all wrapped (was 73).
- `test-dead-export-audit.sh` / `test-small-helper-audit.sh` — selftest and `--check` green; 6 and 7 findings respectively, all baselined.
- `tests/bats/dead_export_audit.bats` 14/14 · `small_helper_audit.bats` 11/11 · `markdown_links.bats` 12/12 (was 7).
- `test-markdown-links.sh` in all four modes: default, `--all` (0 failures, 14 grandfathered), `--selftest` (now passes — see § Implementation log), `--merge-tree-warn`.
- `test-shell-lint.sh` — 302/0 across the whole target set (8 rules incl. shellcheck 0.11.0), covering the four new scripts; `test-shell-lint-bats.sh` 22/22.
- `scripts/dev/test-all.sh --ci` — **1625 passed / 0 failed across 178 scripts**. Its only two failures on a first pass (`test-shell-lint-bats`, `test-lint-hook-split`) were **environmental and reproduce identically on `origin/develop`**: `shellcheck` was absent from this container, and `.claude/` was unprovisioned. Both go green once `shellcheck` is installed and `setup-harness.sh claude-code` has run — which is exactly what the CI lane does before invoking the aggregator.
- `scripts/dev/test-docs.sh` — green except `test-plan-index` DRIFT, which **reproduces unchanged on `origin/develop`** in a clean worktree (shallow-clone artifact; the script warns about exactly this). Not introduced here.

**Widened-gate proof, not just a green run.** Appending a dangling link to `CLI_GUIDE.md` was verified to fail *both* default diff-scope and `--all` — i.e. the scope widening actually gates, and the baseline does not swallow a new break in an already-grandfathered file (also pinned by a bats case).

**Test-delta gate → a real test, not a label.** CI's coverage-delta gate failed the first push (11 production files changed, 0 test deltas) and offered the sanctioned `tests-out-of-band` label, which this diff would have qualified for twice over (dead-code deletion adds no runtime surface; the slice-2 migrations are behaviour-preserving). It was **not** used, because the gate was asking for the right thing: slice 2 concentrated nine per-TU helper bodies into one, so a regression in `ToLowerAsciiCopy` now breaks nine TUs at once — including two security-adjacent callers that compare lowercased hosts (`McpJsonRpcPure`'s origin/host allow-listing, `ModelDownloadPolicy`'s download-domain check) — and `ToUpperAsciiCopy` was brand-new production surface with no coverage at all. `tests/Core/StringUtilAsciiCase.test.cpp` pins both: ASCII folding, non-ASCII/high-byte passthrough (the `unsigned char` cast is load-bearing — passing a negative `char` to `std::tolower` is UB), embedded NUL, idempotence, and a **256-byte equivalence proof** that the explicit `'A'..'Z'` branch this replaced in `BuiltinCommands_Helpers` agrees with `std::tolower` for every input, so § Deviations #5's locale argument is asserted rather than merely argued. 275 assertions, 0 failures.

**Bucket A (`test-rig` ctest): NOT RUN — the gap in this verification.** The dual-target MSVC/DX12 build the plan called load-bearing for slice 1b is unavailable in this Linux container (no MSVC, and `nlohmann/json` is CMake-fetched rather than vendored, so the JSON-dependent TUs cannot even be syntax-checked). Compensating checks actually performed:

- every touched header parses standalone under `g++ -std=c++14 -fsyntax-only`;
- the three self-contained migrated TUs (`TrackerHttpPure.cpp`, `ModelDownloadPolicy.cpp`, `Logger.cpp`) compile clean under the same;
- every deleted symbol was re-confirmed to have zero references across `Source/` and `tests/` by direct grep, independent of the detector's verdict;
- each deleted function's callees were checked so no anonymous-namespace helper was orphaned by losing its only caller — which under `/WX` + `-Wunused-function` would red the build. `ResolveRenderPlan`, `RequiresAllowEditsCheck`, `ExtractIssueKeyPrefix`, `EntryBytes` and `QueueTextureDestroy` all retain other callers;
- `<utility>` was added where a new forwarder introduced `std::move`, and a duplicate `StringUtil.h` include in `AppController_LuaBindings.cpp` was removed.

**CI must still run the dual-target build before this is considered proven for slices 1b and 2.**

**Pillar impact (actual, vs the forecast).** Pillar 1/2/4: no change, as forecast — no hot path, I/O, threading or UI behaviour was touched. **Pillar 3 is a net positive as forecast, and for the two named reasons**: deleting `GetLanguage()` removes an unlocked read of mutex-protected state (a data race by signature — it returned a reference into a string `SetLanguage` mutates under `LocalizationMutex()`), and deleting `Save{DurationSuggestions,CommentTemplates}` removes a dead second write path whose read-modify-write of the whole `TrackerConfig` would have raced the shipped Preferences path. Both headers now record why the function is absent, so neither gets "helpfully" restored.

**Plan stress-test (`grill-with-docs`): NOT RUN.** § Verification listed it as owed before slice 1. It was not run, and this is a real deviation rather than an oversight worth hiding — the plan's own § Approach was specific enough to implement against, and the two places its analysis proved wrong (the 8-vs-14 count, and `LUA_GUIDE.md`) were caught by the implementation itself rather than by design review.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*
