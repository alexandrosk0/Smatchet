# Plan — Reduce source-comment bloat across first-party C++
<!-- plan-date: 2026-05-30 -->

> **Slug**: `reduce-source-comment-bloat` (matches this file's basename without `.md`).

## Context

First-party C++ source files have grown noticeably, and the growth is disproportionately **comments**, not code. A baseline scan (see § Baseline metrics) measured **11,409 comment lines** across **94,337** first-party lines — **12.1%** of the corpus, **13.4%** of non-blank lines.

The bloat is concentrated in three places: (1) **API-doc volume** — `Source/Core/include/` headers are **34% comments** (4,252 lines), the single biggest category; (2) **organizational/decorative banners** (e.g. `// 3. THE REST OF YOUR INCLUDES`); (3) **verbose multi-line rationale**. A meaningful slice of inline comments, however, is dense *why*-knowledge not recoverable from the code (e.g. [AppController.h:51](Source/Core/include/AppController.h:51) — `unique_ptr<incomplete-type>` sizeof reasoning; [:4](Source/Core/include/AppController.h:4) — GCC-13 include-order constraint). Those must survive.

**Intended outcome**: first-party C++ comment line-count drops materially (noise + self-evident docs removed, verbose blocks tightened) **with zero behavior change and zero loss of load-bearing or genuine-rationale comments**, proven mechanically (code-token stream identical before/after), not by eyeball. The before→after delta is reported as a hard number (see § Baseline metrics § Post-implementation diff).

Originating request: source files growing due to comments; clarified via `AskUserQuestion` (scope = all first-party C++; depth = moderate; API docs = remove-when-self-evident; execution = hybrid script + LLM). Refined via a `grill-with-docs` stress-test: two global waves (mechanical then judgment), pilot-gated on Wave-2 marginal yield; commented-out code deleted by default; a Phase-4 regrowth guard (noise-bucket delta hard-fail + advisory ratio warning); explicit before/after metrics.

## Baseline metrics (measured 2026-05-30)

Counted by the prospective `comment_audit.py` definition: a **comment line** is a full-line comment (stripped line starts with `//`, `/*`, or `*`); trailing comments on code lines count as code. Scope = tracked first-party C++ (`Source/Core/`, `Source/Plugins/`, `Source/Standalone/`), excluding `**/ThirdParty/`, `tests/`, `build/`.

| Scope | Files | Total lines | Code | Comment | Blank | Comment % of total | Comment % of non-blank |
|---|---|---|---|---|---|---|---|
| **First-party C++ (sweep scope)** | 409 | **94,337** | 73,526 | **11,409** | 9,402 | **12.1%** | 13.4% |
| Third-party C++ (context only, not swept) | 10 | 20,082 | 15,136 | 2,847 | 2,099 | 14.2% | 15.8% |

**Per-subsystem comment density** (drives batch order + pilot choice):

| Subsystem | Files | Total lines | Comment lines | Comment % |
|---|---|---|---|---|
| `Source/Core/include/` (headers/API docs) | 161 | 12,343 | 4,252 | **34%** |
| `Source/Plugins/` | 30 | 6,554 | 1,180 | 18% |
| `Source/Core/src/` (non-Ui) | 111 | 31,592 | 2,835 | 9% |
| `Source/Standalone/` | 7 | 2,936 | 269 | 9% |
| `Source/Core/src/Ui/` | 57 | 26,014 | 2,135 | 8% |
| `Source/Core/src/Tracker/` | 43 | 14,898 | 738 | **5%** |

**Reading**: the reduction *universe* is the 11,409 comment lines. Bloat concentrates in headers (`Source/Core/include/`, 34%); `.cpp` trees are already lean (5–9%). This shapes the pilot — see § Phases.

### Post-implementation diff *(populated post-ship)*

**Phase-0 re-confirmation (2026-05-30, `comment_audit.py` on current develop):** 409 files · 94,380 total ·
73,635 code · **11,343 comment** · 9,402 blank · **12.0%** — reproduces the original hand-measured baseline
within rounding (per-subsystem also matches: `include/` 34.4%, `Plugins/` 18.0%, `Ui/` 8.2%, `Tracker/` 4.6%).
The original § Baseline metrics numbers stand as the reference baseline; the analyzer is validated against them.

**Final (`comment_audit.py` on merged `develop` after all Wave-1 batches + Wave-2 pilot + Phase-4 guard):**

| Metric | Baseline (Phase-0 reconfirm) | Final (merged develop) | Δ net |
|---|---|---|---|
| First-party files | 409 | 423 | **+14** (unrelated merges) |
| First-party total lines | 94,380 | 96,020 | +1,640 |
| Code lines | 73,635 | 75,154 | +1,519 |
| Comment lines | 11,343 | 11,228 | **−115** |
| Comment % of total | 12.0% | 11.7% | **−0.3 pp** |

**Sweep reduction (the real number, deterministic + per-PR `assert-code-unchanged`-proven):** ~**381** comment
lines removed by the Wave-1 mechanical batches (pilot + strict-zone + Ui + Plugins/Standalone) **+ 9** by the
Wave-2 Tracker pilot = **~390 comment lines removed** with zero behavior change. The **audited net is only −115**
because the corpus *grew* by **+14 files / +1,519 code lines** from unrelated feature PRs merged during the
multi-day sweep window — those new files brought ~275 of their own comment lines, diluting the net. The sweep's
reduction is isolated and real (each merged PR carried a green code-token-identical residue proof); the net metric
is confounded by concurrent work, not by the sweep. **Code-line growth (+1,519) is entirely new files, never edits
to swept files** — every Wave-1/Wave-2 PR proved its code-token residue byte-identical, so no non-comment edit
slipped through.

**Per-subsystem (final merged develop):**

| Subsystem | Files | Comment % | vs baseline |
|---|---|---|---|
| `Source/Core/include/` | 167 | 33.7% | 34% → 33.7% |
| `Source/Plugins/` | 30 | 17.4% | 18% → 17.4% |
| `Source/Standalone/` | 7 | 8.9% | 9% → 8.9% |
| `Source/Core/src/` (non-Ui) | 117 | 8.7% | 9% → 8.7% |
| `Source/Core/src/Ui/` | 59 | 8.0% | 8% → 8.0% |
| `Source/Core/src/Tracker/` | 43 | 4.6% | 5% → 4.6% |

Every subsystem's comment % held flat or edged down despite corpus growth — the regrowth guard (Phase 4, this PR)
now keeps it there: new commented-out code / decorative banners / blank-comment runs hard-fail any PR repo-wide.

## Approach

A **taxonomy-driven, tooling-gated, batched** sweep. Four pieces:

1. **A comment taxonomy** (below) splits every comment into *cut* (mechanical, script-safe), *compress/judge* (LLM), and *protect* (never touch) buckets. The protect-list is the safety crux: legacy lint markers, `SMATCHET_DEVIATION`, perf annotations, build-divergence macro comments, and genuine *why*-rationale.

2. **Two scripts** under `scripts/dev/`: a read-only **analyzer** that classifies + counts every comment (establishes the before/after metric and the candidate-removal report), and a deterministic **mechanical stripper** that removes only the unambiguous *cut* buckets behind a hardcoded protect-list with dry-run + per-file diff. The stripper never touches doc bodies or multi-line rationale — those are LLM-only. A third **`assert-code-unchanged`** check strips all comments+whitespace from the pre- and post-edit versions of each file and requires the residue to be byte-identical: this proves only comments changed and is the gate that makes the whole sweep safe.

3. **Per-subsystem batches**, one PR each, each running build (dual-target) + lint-delta + tests + the assert-code-unchanged check. Batching by lint **zone** matters: strict-zone trees (`Tracker`, `Sync`, `Persistence`, `Config`, `Commands`, `Mcp`) fail CI on any *new* `(rule, file, snippet)` violation — so removing a `// custom-deleter` marker next to a raw `new` is caught automatically, validating the protect-list against the strongest available net.

4. **A regrowth guard** (Phase 4, lands *after* the sweep) so the trimmed corpus stays trimmed: new delta-lint rules that hard-fail a PR introducing fresh *noise-bucket* comments (commented-out code, decorative banners, blank-comment runs — restate-the-code was deferred as too false-positive-prone for a hard gate, see § Deviations), plus a non-blocking soft warning when a PR worsens a file's comment ratio past a threshold. Genuine doc / *why* comments are never penalized.

The sweep runs as **two global waves**, not two passes welded into each batch: **Wave 1** — the deterministic mechanical stripper (`comment_strip.py`, no judgment, only the unambiguous *cut* buckets) across the whole corpus; **Wave 2** — the LLM judgment pass (compress verbose rationale, remove self-evident docs) across the whole corpus. Each wave is split into reviewable per-subsystem PRs (strict lint-zones first). Wave 1 PRs need near-zero judgment review (deterministic diff + green code-unchanged gate) and merge fast; Wave 2 PRs concentrate human + CodeRabbit attention. The sweep is **pilot-gated**: one slice runs both waves end-to-end first, and Wave 2 rolls out repo-wide only if its *marginal* yield clears a measured go/no-go threshold (see § Phases) — otherwise Wave 1 ships alone and the judgment wave is shelved as low-ROI.

The non-obvious trade-off: "self-evident" (API docs) and "redundant" (inline) are judgment calls a script cannot make safely, so the LLM pass is load-bearing — mitigated by erring toward keep, small reviewable diffs, CodeRabbit, and the mechanical code-unchanged proof. Lowering the API-doc risk: **no first-party doc-generation pipeline exists** (every `Doxyfile` in the tree belongs to a vendored FetchContent dep; zero `doxygen` references in first-party CMake/scripts), so `///` / `/** */` blocks are consumed only by IDE hover + human reading — removing a self-evident one breaks no published-docs build, it only drops that symbol's editor tooltip.

### Comment taxonomy

**CUT — script-detectable, mechanical:**
- Blank/empty comment lines (`//` alone, empty `*` inside a block being trimmed).
- Decorative banners / separators (`// =====`, `// -----`, `// ####`, ASCII dividers, shouting section headers carrying no info like `// 3. THE REST OF YOUR INCLUDES`).
- Restate-the-code one-liners (`// constructor`, `i++; // increment i`, `return x; // return x`) — comment paraphrases the adjacent line, adds nothing.
- Commented-out code (`// foo();`, `// int x = ...;`) — **Wave 1 flags only** (regex can't tell dead code from an example snippet); **Wave 2 deletes by default** (git history is the recovery path), keeping three carve-outs: (1) blocks with an explicit keep-note (`// kept: …`, `// reference impl …`, `// example:`), (2) code snippets *inside* doc comments used as usage examples, (3) `#if 0 … #endif` blocks — preprocessor, not comments, out of scope.

**COMPRESS / JUDGE — LLM:**
- Verbose multi-line `//` rationale → tighten to the essential *why* (keep the fact, cut the prose).
- API doc comments (`///`, `/** */`) where symbol name + signature are self-evident → **remove** (per chosen policy); keep where they encode non-obvious contract: units, lifetime, threading, nullability, side effects, invariants.
- `@param`/`@return`-style prose that only echoes the signature → drop.

**PROTECT — never touch (hardcoded script whitelist + LLM instruction):**
- Legacy lint markers + their explanatory tail: `// CLI stdout — product output, not logging`, `// pre-logger-init — LOG_* unavailable`, `// C-ABI handle`, `// custom-deleter — make_unique inapplicable`, `// pimpl`. These are **inline trailing** comments on the code line (e.g. `auto* p = new Foo(); // custom-deleter`); `test-lint-rules.sh:105` (`has_inline_exempt`) matches them on the same line, so stripping the trailing marker reintroduces the suppressed violation. The mechanical pass must never strip a trailing comment off a line it shares with code.
- `// SMATCHET_DEVIATION(...)` suppressor lines (govern next-line lint).
- Perf annotations: `/* PILLAR2_WORKER_ONLY */`, `// est-latency: <N>ms`; and `SMATCHET_UI_PERF_SCOPE(...)` (code, not comment — but adjacent, do not disturb).
- Build-divergence macro comments documenting `SMATCHET_WITH_*` / `SMATCHET_EMBEDDED_IN_UNREAL` / include-order constraints — load-bearing knowledge.
- Genuine *why*-rationale (the AppController unique_ptr / GCC-13 cases and their kin).
- Cross-refs to design docs / ADRs (`see docs/plans/...`, `docs/adr/...`) — navigation aids.
- `// clang-format off|on`, `// NOLINT`, IWYU pragmas (none found today; protect if introduced).
- Closing-brace namespace labels on long blocks (`} // namespace Foo`) — navigation aid, keep.

### Phases

**Phase 0 — Tooling + baseline (1 PR).** Build `comment_audit.py`, `comment_strip.py`, `assert-code-unchanged.sh`, and their fixture tests. Lock the protect-list. Emit the baseline comment-count report across the whole corpus (the § Baseline metrics numbers; also ranks files by noise to prioritise batches). Ships no product-source comment edits — only `scripts/dev/` + `tests/`.

**Phase 1 — Pilot (1–2 PRs), `Source/Core/{src,include}/Tracker/` (smallest strict zone).** Run **both waves** end-to-end on this one slice: mechanical strip, then LLM judgment. Record two numbers — Wave-1 mechanical reduction, and Wave-2 *marginal* reduction on top — each as **comment-lines-removed ÷ slice comment baseline** (Tracker baseline = 738 comment lines). Inspect Wave-2 diff quality. **Go/no-go gate: roll Wave 2 out repo-wide only if its marginal reduction is ≥ 20% of the slice comment baseline with clean, behavior-preserving diffs** (threshold provisional). Below threshold → ship Wave 1 alone repo-wide and shelve Wave 2 as low-ROI. Wave 1 proceeds repo-wide regardless (deterministic + low-risk).
- **Pilot-density caveat**: Tracker is comment-*sparse* (5% in `src`), chosen for the strongest **safety** net (smallest strict zone). It under-represents Wave-2 *doc-compression* yield, which concentrates in `Source/Core/include/` (34%). So treat the Tracker gate as a **floor**, and schedule `Source/Core/include/` as the **first** post-pilot Wave-2 batch to re-confirm yield where the bloat actually lives before committing the long tail.

**Phase 2 — Wave 1, mechanical, repo-wide (batched).** Apply `comment_strip.py` to every remaining subsystem, one reviewable PR per batch, lint-zone order (strict first). Deterministic diffs; each PR carries the green `assert-code-unchanged` proof, so review is fast.

**Phase 3 — Wave 2, LLM judgment, repo-wide (batched), pilot-gated.** Only if Phase 1 cleared the gate. Apply the judgment pass to every remaining subsystem, headers first (highest density), then the rest. Where review attention concentrates.

**Batch order within each wave** (strict lint-zones first, where the delta-lint net is strongest; Tracker already done by the pilot):
- `Source/Core/{src,include}/{Sync,Persistence,Config}/` (strict)
- `Source/Core/{src,include}/Commands/` + `Source/Plugins/Mcp/` (strict)
- `Source/Core/include/` remaining (highest comment density — prioritise for Wave 2)
- `Source/Core/src/Ui/` (light; largest by lines — **split into sub-PRs** per the batch-size cap in § Risks)
- `Source/Core/src/` root (`AppController*`, `Ai*`, `Logger`, …)
- `Source/Plugins/{LuaConsole,Whisper}` + `Source/Standalone/`

Per-batch pipeline (both waves): apply pass (Wave 1 = `comment_strip.py` dry-run → apply; Wave 2 = LLM judgment) → `clang-format -i` → dual-target build → lint-delta gate → `test-all.sh` → `assert-code-unchanged` → PR → CodeRabbit → merge gates.

**Phase 4 — Regrowth guard (1 PR), lands after Wave 1 (and Wave 2 if it ran) merge.** Baseline is taken against the *cleaned* `develop`, so the guard never grandfathers the bloat we just removed. Two mechanisms:
- **Noise-bucket delta rules** — new rule-ids (`comment-commented-out-code`, `comment-decorative-banner`, `comment-blank-run`; **`comment-restate` deferred** — see § Deviations) folded into `test-lint-rules.sh`, riding its `--diff` / `--selftest` / zone / grandfather machinery. **Hard-fail repo-wide** (this noise is never legitimate anywhere). Escape hatch: the existing `// SMATCHET_DEVIATION(rule=<id>; reason=…; revisit=…)` grammar — no new suppressor syntax.
- **Soft ratio warning** — delta-aware: warn only when a PR both raises a touched file's comment ÷ (comment + code) ratio *and* pushes it past **0.50**. Advisory CI annotation only; never blocks; threshold is a config constant. Well-documented files that don't get worse never warn.

`comment_audit.py --diff <ref>` does the classification, emitting violation tuples in the format `test-lint-rules.sh` consumes. Modeled on the delta-gated pattern in [`docs/plans/shipped/high-integrity-cpp-enforcement.md`](docs/plans/shipped/high-integrity-cpp-enforcement.md) (extension of an existing mechanism → no new ADR).

## Files to modify

**New tooling (Phase 0):**
1. `scripts/dev/comment_audit.py` — read-only analyzer: walks first-party C++, tokenizes respecting string/char/raw-string literals, classifies each comment into taxonomy buckets, emits per-file + per-subsystem counts and a candidate-removal report (JSON + markdown). Establishes baseline + reproduces the § Baseline metrics table.
2. `scripts/dev/comment_strip.py` — deterministic mechanical pass over the unambiguous *cut* buckets only; hardcoded protect-list; `--dry-run` + per-file unified diff; refuses to touch `///` / `/** */` bodies and multi-line rationale.
3. `scripts/dev/assert-code-unchanged.sh` — wraps a shared comment-stripper to normalize (strip comments + collapse whitespace) the base vs head version of every changed file; non-empty diff = fail. The safety gate.
4. `tests/dev/comment_tooling/` fixtures + a ctest (or bats) wrapper — one fixture per bucket; assert protect-list survives, noise removed, code-token stream preserved.

**Sweep targets — every first-party `.cpp`/`.h`/`.hpp` under, each swept in Wave 1 (mechanical) then Wave 2 (judgment); Tracker is the pilot, then strict-zone batches first:**
5. `Source/Core/src/Tracker/`, `Source/Core/include/Tracker/` (**Pilot**, Phase 1).
6. `Source/Core/src/{Sync,Persistence,Config}/`, `Source/Core/include/{Sync,Persistence,Config}/` (strict).
7. `Source/Core/src/Commands/`, `Source/Core/include/Commands/`, `Source/Plugins/Mcp/` (strict). *(Mcp `src/` was flattened to `Source/Plugins/Mcp/` by #556 — path updated.)*
8. `Source/Core/include/` remaining headers (highest density, 34%).
9. `Source/Core/src/Ui/` (split into sub-PRs).
10. `Source/Core/src/` root + `Source/Plugins/{LuaConsole,Whisper}/` + `Source/Standalone/`.

**Regrowth guard (Phase 4):**
11. `scripts/dev/comment_audit.py` — add a `--diff <ref>` mode emitting noise-bucket violation tuples (`rule⇥basename:line⇥snippet`) for the lint gate to consume.
12. `scripts/dev/test-lint-rules.sh` — register the new `comment-*` delta rule-ids (repo-wide), wire the soft ratio-warning call, update the `--selftest` zone/rule assertion.
13. `tests/` — regrowth-guard fixtures (fresh noise fails / grandfathered passes / `SMATCHET_DEVIATION` suppresses / ratio warning is delta-aware + non-blocking).
14. Ratio-threshold + rule-list config constant (in `comment_audit.py` and/or the lint script).

Excluded everywhere: `Source/Core/ThirdParty/`, `ThirdParty/`, generated code, `build/`, `tests/` C++ (sweep scope; tooling fixtures under `tests/` are added, not swept).

## Existing utilities reused

- `scripts/dev/test-lint-rules.sh --diff` + `--selftest` — delta-lint gate; the automated validator that the protect-list preserved every `no-printf-stderr` / `no-raw-new` / `define-imgui` / `deviation-overdue` suppressor. Strict-zone batches lean on this. (Zone globs already use `Source/Core/...` on this branch.)
- `scripts/dev/is-pure-docs-diff.sh` — confirm comment diffs are correctly classified non-pure-docs (they touch `.cpp`/`.h`, so full build+test runs — expected).
- `clang-format` (PostToolUse hook per `.claude/CLAUDE.md`, or manual) — reflow after comment removal so block-comment trailers don't leave dangling continuation.
- `scripts/dev/test-all.sh` — test gate at each slice boundary.
- `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — dual-target build gate.
- `docs/plans/active/_plan-template.md` — this doc's structure.
- `AGENTS.md` § Merge gates, § Autonomous ship-loop, § Process rules (one build + one test per slice) — batch cadence.

## UX Pillar callouts

Comments are non-executable; correct comment removal cannot alter runtime behavior. The `assert-code-unchanged` gate proves it per batch.

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: No impact — zero codegen delta (comment-only). Build + code-unchanged check verify no accidental code edit.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: No impact — no control-flow or I/O change.
- **Pillar 3 (never crash)**: No impact — no logic change; RAII / bounds / sanitizer surface untouched. Risk is confined to accidental code edits, caught by build + tests + code-unchanged gate.
- **Pillar 4 (accessibility)**: No impact.

## Perf-review-system gates

Diff touches `Source/Core/`, so this section is mandatory. All gates resolve **N/A — comment-only diff, no executable-path change** (proven by `assert-code-unchanged`):

1. **PR-fast CI** — N/A on content; however the curated diff→scenario map keys on **file path**, so a touched `Source/Core/src/Tracker/*.cpp` may *auto-fire* a scenario despite comment-only content. Expected delta ≈ 0; baselines unchanged; no `perf-out-of-band` label needed unless a spurious path-match flakes (it will not regress).
2. **Pillar 2 static scanner** — N/A — no new sync-I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — N/A — does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** — N/A — adds no sync-stall path.
5. **Marker inventory** — N/A — `SMATCHET_UI_PERF_SCOPE` markers are on the protect-list (never removed/added).

**Override**: not anticipated; `perf-out-of-band` only if a path-classified scenario spuriously fails.

## Risks / non-goals

**Risks:**
- **Protect-list incompleteness** → a load-bearing lint marker removed. *Mitigation*: hardcoded whitelist + fixture test; strict-zone delta-lint catches every lint-suppressor removal automatically; batch strict zones first.
- **Accidental code edit** while removing comments. *Mitigation*: `assert-code-unchanged` (comment-stripped token stream must be byte-identical base vs head) + dual-target build + `test-all.sh`. This is the primary net. The post-impl code-line count (must stay ~73,526) is a second check.
- **Over-removal of genuine *why*-knowledge** (not catchable by any automated gate). *Mitigation*: "Moderate" depth, LLM errs toward keep, small reviewable diffs, CodeRabbit, human review on each PR.
- **Tokenizer edge cases** — `//` inside string/char/raw-string literals (`R"(...)"`), line-continuation (`\` at EOL splicing comment into code), comments inside multi-line macros. *Mitigation*: literal-aware tokenizer (shared by analyzer + stripper + code-unchanged check); macro-body comments on protect-list; build gate catches splices.
- **`clang-format` comment reflow** — `.clang-format` sets `ColumnLimit: 120` with `ReflowComments` on, so a Pass-2 single-line comment > 120 chars is auto-re-wrapped back to multiple lines, silently undoing the compression. *Mitigation*: Pass-2 compressed comments stay ≤ 120 chars; analyzer flags any candidate over the limit. (`SortIncludes: Never` + `IncludeBlocks: Preserve` mean removing include-section banners triggers no reorder churn.)
- **Line-number shift** — deleting comment lines renumbers everything below, changing `__LINE__` expansions (log line info, assert messages), `git blame`, and debug line tables. *Mitigation*: accept as cosmetic (fewer lines is the goal); the lint baseline keys on `(rule, basename, hash)` not line, so it is shift-robust; Phase 0 confirms no in-repo tooling hard-codes source line numbers.
- **Oversized batch PRs** — `Source/Core/src/Ui/` is the largest by lines (57 files, 26k lines); one PR could be unreviewable and risk CodeRabbit truncation. *Mitigation*: **batch-size cap** ≈ ≤ 25 files or ≤ ~1500 changed lines per PR; split large batches (esp. Ui + the 161-file `include/` tree) into sub-PRs.
- **Low-yield pilot** — Tracker is only 5% comments, so it may under-show Wave-2 value. *Mitigation*: gate measures % of *comment lines* removed (density-independent); schedule `Source/Core/include/` (34%) as the first post-pilot Wave-2 batch to re-confirm before the long tail.
- **Concurrent branch-swap daemon** — a merge-watcher/janitor process swaps `HEAD` mid-session (observed: branch moved several times while authoring this plan). *Mitigation*: commit early + often; re-verify `HEAD` immediately before every commit and re-checkout the working branch if it moved (per the watcher-janitor branch-swap rule).

**Non-goals:**
- Not touching ThirdParty, generated code, or `tests/` C++ (beyond adding tooling fixtures).
- Not reducing markdown/doc comments (different corpus, separate effort if wanted).
- Not refactoring, renaming, reformatting non-comment code, trimming `#include`s, or reducing `LOG_*` calls.
- Not enforcing a blanket "X% reduction" *or a hard comment-ratio cap* — the Phase-4 regrowth guard hard-fails only *never-legitimate noise*; its ratio check is advisory-only. Target the noise + self-evident + verbose buckets; report actual before/after.

## Verification

Per `AGENTS.md` § Verification automation — automated wherever physically possible.

- **Bucket A (pure-logic ctest, `test-rig`)**: fixture tests for `comment_audit.py` + `comment_strip.py` — one fixture per taxonomy bucket; assert (a) every protect-list marker survives, (b) each *cut* bucket is removed, (c) the stripped code-token stream is unchanged. This is the core automated proof of the tooling.
- **`assert-code-unchanged.sh` (per batch)**: comment-stripped + whitespace-normalized base vs head must be byte-identical for every changed file. Deterministic, zero manual steps — the safety gate.
- **Lint-delta gate (per batch)**: `scripts/dev/test-lint-rules.sh --diff` — zero new `(rule, file, snippet)` violations vs `origin/develop`. Validates the protect-list preserved all suppressors.
- **Regrowth-guard tests (Phase 4, Bucket A)**: fixtures for the new `comment-*` delta rules — fresh noise fails, grandfathered noise passes, `// SMATCHET_DEVIATION(rule=…)` suppresses on the next line, and the soft ratio warning fires delta-aware (worsened *and* > 0.50) while never setting a non-zero exit. `--selftest` covers the new rule/zone list.
- **Build gate (per batch)**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) — catches any line-continuation / macro splice.
- **Test gate (per batch)**: `scripts/dev/test-all.sh` — sanity; comment changes must not move any test.
- **Metrics + pilot gate**: `comment_audit.py` before/after counts per batch + cumulative, reported in each PR body and rolled into § Baseline metrics § Post-implementation diff (total lines / comment lines / comment % / code lines / per-subsystem). Pilot go/no-go: Wave-2 **marginal** reduction **≥ 20%** of the slice comment baseline (clean diffs) unlocks the repo-wide Wave 2; below that, Wave 1 ships repo-wide alone and Wave 2 is shelved.
- **Bucket E (ImGui Test Engine)**: N/A — no UI behavior change.
- **Manual residue**: the "is this comment self-evident / redundant?" judgment is inherently subjective and not fully automatable. *Deferred-automation action plan*: the analyzer's candidate report + `assert-code-unchanged` bound the risk to "only comments changed"; remaining judgment is covered by per-PR human + CodeRabbit review. Add a `docs/self-improvement/categories/tooling.md` entry if a heuristic for "self-evident doc" proves worth automating after the pilot + first header batch.

## Out of scope (flagged, not designed)

- **Markdown / `docs/` / script comments** — different corpus; separate plan if desired.
- **`tests/` C++ comments** — excluded per chosen scope; fold in via a follow-up batch if wanted.
- **ThirdParty** — vendored / out-of-tree; never swept.
- **Non-comment size reduction** (include pruning, dead-code removal, function splitting) — separate efforts; this plan is comments-only by construction (enforced by `assert-code-unchanged`).
- **A prose comment *style guide*** (how to author good comments) — out of scope; the Phase-4 regrowth guard enforces *mechanical noise* limits only, not authorship style. (The regrowth lint rule itself is now in scope — see Phase 4.)

## Implementation log
- **Phase 4 — regrowth guard** (this PR). Folded three repo-wide comment-regrowth delta rules into
  `test-lint-rules.sh` riding its existing `--diff` / `--selftest` / grandfather machinery:
  `comment-commented-out-code`, `comment-decorative-banner`, `comment-blank-run` — **hard-fail anywhere in
  first-party C++** (not just the strict zone; this noise is never legitimate). Classification is delegated to
  `comment_audit.py --diff <base>` (emits `rule\tbasename:line\tsnippet` for ADDED comment lines only, so
  grandfathered noise never trips). Escape hatch = the existing `// SMATCHET_DEVIATION(rule=comment-…; …)` on
  the line above — no new suppressor syntax. Added the **soft, delta-aware, never-blocking** comment-ratio
  warning (`comment_audit.py --ratio-warn`): warns only when a touched file's comment ratio both *rises* vs base
  AND exceeds 0.50 (well-documented files that don't get worse never warn). `--selftest` extended to assert the
  comment rule-ids appear in AGENTS.md § Tiered enforcement (kept in sync). **8 new git-integration fixtures**
  in `tests/dev/comment_tooling/` (fresh-noise-fails / grandfathered-passes / SMATCHET_DEVIATION-suppresses /
  ratio-warn-fires-on-rise / ratio-warn-silent-on-stable / non-blocking). Gates: `--selftest` PASS, all
  fixtures PASS, shell-lint 105/0, full `--diff origin/develop` PASS (strict-zone + comment-noise both clean).
  **`comment-restate` rule deferred** — see § Deviations.
- **Phase 2 — Wave 1, mechanical, repo-wide (batched).** Applied `comment_strip.py` to every remaining
  subsystem in lint-zone order (strict-zone batches → `src/Ui` batch 4 (#590) → `Plugins` + `Standalone`
  batch 6 (#591)). Cumulative Wave-1 mechanical removal across all batches ≈ **381 comment lines**, each batch
  carrying a green `assert-code-unchanged` (code-token residue byte-identical) proof + dual-target build +
  lint-delta + `test-all.sh`. Switched from whole-file `clang-format -i` to diff-scoped `git clang-format` after
  a whole-file pass swept 1,053 lines of pre-existing drift into one batch. Wave-1 repo-wide **complete**.
- **Phase 1 — pilot Wave 2 + GO/NO-GO GATE** (`Tracker`). LLM judgment pass removed **9 comment lines**
  (1,545 → 1,536) = **0.57% marginal** of the 1,581 pilot baseline — **far below the 20% gate**. Removals
  were all self-evident echo-docs in `include/Tracker/` (`/** Identical to X */` clusters, an empty-sentinel
  doc); zero net rationale-compression cuts. Qualitative finding (the real signal): Tracker comments are
  **overwhelmingly load-bearing** — contract/units/threading/nullability, plan+ADR cross-refs, why-rationale —
  and Wave 1 already cleared the mechanical noise, so judgment-yield is minimal. assert-code-unchanged PASS
  (comment-only, proven). **DECISION: do NOT roll Wave 2 out repo-wide; ship Wave 1 repo-wide alone (Phase 2)
  and shelve Wave 2 as low-ROI.** Honoring the plan's pilot-density caveat: Wave 2 may be *reconsidered* only
  if a one-off measurement probe on a high-density `Source/Core/include/` slice (34%) shows ≥20% marginal —
  but the pilot's evidence (self-evident-doc removal on the include/Tracker headers yielded ~8 lines, rest
  load-bearing) suggests broad Wave-2 ROI is low. Build gate deferred to CI for this proven-comment-only
  2-header diff (token-identity ⟹ compilation-identity).
- **Phase 1 — pilot Wave 1** (`Source/Core/{src,include}/Tracker/`). Mechanical strip removed **36
  comment lines across 17 files** — **2.3%** of the pilot comment baseline (1,581 lines for src+include
  Tracker; the plan's 738 was src-only/pre-merge). Low yield as predicted for comment-sparse Tracker
  (safety floor, not yield). Gates green: dual-target build, fixtures, lint-delta (no strict-zone
  suppressor dropped), **assert-code-unchanged PASS** (comment-only, proven). **Tooling hardening folded
  in** (two bugs in the merged Phase-0 scripts, surfaced running the pilot on the real corpus): (a) Windows
  UTF-8 stdio — the scripts crashed on non-ASCII comment chars (`→`); (b) the code-residue is now a
  literal-aware **token list**, not whitespace-collapse — removing a comment can let clang-format reflow
  adjacent code across lines + change whitespace around punctuation (`(\n const` → `(const`) without
  changing tokens, which the old residue false-failed. Gate re-validated PASS-on-comment-only / FAIL-on-code.
- **Phase 0** — tooling + baseline. Added `scripts/dev/comment_lib.py` (literal-aware tokenizer +
  code-token residue; correctly handles `//`/`/*` inside string/char/raw-string literals and the
  line-comment-resets-at-newline case), `comment_audit.py` (taxonomy classifier + per-subsystem
  baseline table + Phase-4 `--diff` regrowth mode), `comment_strip.py` (deterministic Wave-1 stripper:
  only `//`-line cut-blank/decorative buckets; never block bodies, doc comments, trailing-on-code, or
  protect-list), `assert-code-unchanged.sh` (the safety gate — base-vs-head code-token residue must be
  byte-identical), and `tests/dev/comment_tooling/` fixtures + `scripts/dev/test-comment-tooling.sh`
  (auto-enrolled by `test-all.sh`). Baseline re-confirmed (see § Post-implementation diff). Stale
  `Source/Plugins/Mcp/src/` path corrected to `Source/Plugins/Mcp/` (#556 flatten). Gate validated:
  PASS on a comment-only strip, FAIL on an injected code edit. No product-source edits.

## Deviations from plan
- **Wave 2 (LLM judgment pass) shelved repo-wide** — the Phase-1 pilot's marginal yield was 0.57% (9 lines),
  far below the 20% go/no-go gate; Tracker comments proved overwhelmingly load-bearing. Wave 1 shipped repo-wide
  alone; Wave 2 stays shelved (reconsider only on a measured high-density `include/` probe). The plan's § Phases
  explicitly authorized this branch.
- **`comment-restate` regrowth rule deferred** (Phase 4 listed four rule-ids; shipped three). "This comment just
  restates the next line of code" is a semantic judgment with a high false-positive rate — unsafe as a *hard-fail*
  delta gate (it would block legitimate clarifying comments). The other three buckets (commented-out code,
  decorative banner, blank-comment run) are mechanically unambiguous and never legitimate, so they hard-fail
  cleanly. Restate-detection is better served by the soft, non-blocking comment-ratio warning that *did* ship
  (a file accreting restate-comments raises its ratio and gets the advisory nudge). Backlog: revisit as an
  advisory-only (never-blocking) classifier if false-positive rate can be bounded.
- **Diff-scoped `git clang-format`** replaced whole-file `clang-format -i` mid-Phase-2 — whole-file formatting
  swept pre-existing drift (1,053 lines in one batch) into comment-only PRs. No plan change, just method.

## Verification (actual)
- **Phase 4 (this PR)** — `test-lint-rules.sh --selftest` PASS (AGENTS.md zone globs + comment rules in sync);
  `tests/dev/comment_tooling/test_comment_tooling.py` **all fixtures PASS** incl. 8 new Phase-4 git-integration
  cases; `test-shell-lint.sh` **105 passed / 0 failed**; `test-lint-rules.sh --diff origin/develop` **PASS exit 0**
  (strict-zone clean + comment-noise clean) — confirms the new diff-mode block doesn't break the existing
  strict-zone gate. `comment_audit.py --diff` / `--ratio-warn` smoke-tested clean on this (C++-free) worktree.
- **Phases 0–2** — per-batch: dual-target build PASS, `test-all.sh` PASS, lint-delta PASS, `assert-code-unchanged`
  **PASS (code-token residue byte-identical)** on every merged batch. Wave-1 repo-wide complete (#590, #591 + earlier
  strict-zone batches all merged). Final corpus audit recorded in § Post-implementation diff.
- **Not-run / N/A** — Bucket-E (no UI behavior change); perf scenarios (comment-only, expected Δ≈0).
