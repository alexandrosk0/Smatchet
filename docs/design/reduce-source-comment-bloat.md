# Plan — Reduce source-comment bloat across first-party C++

> **Slug**: `reduce-source-comment-bloat` (matches this file's basename without `.md`).

## Context

First-party C++ source files have grown noticeably, and the growth is disproportionately **comments**, not code. A baseline scan found ~8.6k full-line `//` comments in `Source_Core` alone (3063 in `include/`, 5556 in `src/`), of which ~1773 are `///` Doxygen doc lines — before counting `Plugins/`, `Target_Standalone/`, inline trailing comments, and `/* */` blocks. The total first-party comment corpus is ~12k+ lines.

The bloat is concentrated in three places: (1) **API-doc volume** (`///` + `/** */` blocks, the single biggest category), (2) **organizational/decorative banners** (e.g. `// 3. THE REST OF YOUR INCLUDES`), and (3) **verbose multi-line rationale** that could be tightened. A meaningful slice of the inline comments, however, is dense *why*-knowledge not recoverable from the code (e.g. [AppController.h:46](Source_Core/include/AppController.h:46) — `unique_ptr<incomplete-type>` sizeof reasoning, GCC-13 include-order constraint). Those must survive.

**Intended outcome**: after this lands, first-party C++ comment line-count drops materially (noise + self-evident docs removed, verbose blocks tightened) **with zero behavior change and zero loss of load-bearing or genuine-rationale comments**, proven mechanically (code-token stream identical before/after) rather than by eyeball.

Originating request: user observation that source files are growing due to comments; clarified via `AskUserQuestion` (scope = all first-party C++; inline depth = moderate; API docs = remove-when-self-evident; execution = hybrid script + LLM, batched by subsystem).

## Approach

A **taxonomy-driven, tooling-gated, batched** sweep. Three pieces:

1. **A comment taxonomy** (below) splits every comment into *cut* (mechanical, script-safe), *compress/judge* (LLM), and *protect* (never touch) buckets. The protect-list is the safety crux: legacy lint markers, `SMATCHET_DEVIATION`, perf annotations, build-divergence macro comments, and genuine *why*-rationale.

2. **Two scripts** under `scripts/dev/`: a read-only **analyzer** that classifies + counts every comment (establishes the before/after metric and the candidate-removal report), and a deterministic **mechanical stripper** that removes only the unambiguous *cut* buckets behind a hardcoded protect-list with dry-run + per-file diff. The stripper never touches doc bodies or multi-line rationale — those are LLM-only. A third **`assert-code-unchanged`** check strips all comments+whitespace from the pre- and post-edit versions of each file and requires the residue to be byte-identical: this proves only comments changed and is the gate that makes the whole sweep safe.

3. **Per-subsystem batches**, one PR each, each running build (dual-target) + lint-delta + tests + the assert-code-unchanged check. Batching by lint **zone** matters: strict-zone trees (`Tracker`, `Sync`, `Persistence`, `Config`, `Commands`, `Mcp`) fail CI on any *new* `(rule, file, snippet)` violation — so removing a `// custom-deleter` marker next to a raw `new` is caught automatically, validating the protect-list against the strongest available net.

The non-obvious trade-off: "self-evident" (API docs) and "redundant" (inline) are judgment calls a script cannot make safely, so the LLM pass is load-bearing — mitigated by erring toward keep, small reviewable diffs, CodeRabbit, and the mechanical code-unchanged proof.

### Comment taxonomy

**CUT — script-detectable, mechanical:**
- Blank/empty comment lines (`//` alone, empty `*` inside a block being trimmed).
- Decorative banners / separators (`// =====`, `// -----`, `// ####`, ASCII dividers, shouting section headers carrying no info like `// 3. THE REST OF YOUR INCLUDES`).
- Restate-the-code one-liners (`// constructor`, `i++; // increment i`, `return x; // return x`) — comment paraphrases the adjacent line, adds nothing.
- Commented-out code (`// foo();`, `// int x = ...;`) — **flagged for review, not auto-deleted** (false-positive risk; some are intentional reference).

**COMPRESS / JUDGE — LLM:**
- Verbose multi-line `//` rationale → tighten to the essential *why* (keep the fact, cut the prose).
- API doc comments (`///`, `/** */`) where symbol name + signature are self-evident → **remove** (per chosen policy); keep where they encode non-obvious contract: units, lifetime, threading, nullability, side effects, invariants.
- `@param`/`@return`-style prose that only echoes the signature → drop.

**PROTECT — never touch (hardcoded script whitelist + LLM instruction):**
- Legacy lint markers + their explanatory tail: `// CLI stdout — product output, not logging`, `// pre-logger-init — LOG_* unavailable`, `// C-ABI handle`, `// custom-deleter — make_unique inapplicable` (42 occurrences, 5 files).
- `// SMATCHET_DEVIATION(...)` suppressor lines (govern next-line lint).
- Perf annotations: `/* PILLAR2_WORKER_ONLY */`, `// est-latency: <N>ms`; and `SMATCHET_UI_PERF_SCOPE(...)` (code, not comment — but adjacent, do not disturb).
- Build-divergence macro comments documenting `SMATCHET_WITH_*` / `SMATCHET_EMBEDDED_IN_UNREAL` / include-order constraints — load-bearing knowledge.
- Genuine *why*-rationale (the AppController unique_ptr / GCC-13 cases and their kin).
- Cross-refs to design docs / ADRs (`see docs/design/...`) — navigation aids.
- `// clang-format off|on`, `// NOLINT`, IWYU pragmas (none found today; protect if introduced).
- Closing-brace namespace labels on long blocks (`} // namespace Foo`) — navigation aid, keep.

### Phases

**Phase 0 — Tooling + baseline (1 PR).** Build the analyzer, the mechanical stripper, the assert-code-unchanged check, and their fixture tests. Lock the protect-list. Emit the baseline comment-count report. This PR ships no product-source comment edits — only `scripts/dev/` + `tests/`.

**Phases 1–6 — Per-subsystem batches (1 PR each), in lint-zone order (strict first, where the delta-lint net is strongest):**
- **Batch A** — `Source_Core/{src,include}/Tracker/` (strict zone).
- **Batch B** — `Source_Core/{src,include}/{Sync,Persistence,Config}/` (strict zone).
- **Batch C** — `Source_Core/{src,include}/Commands/` + `Plugins/Mcp/src/` (strict zone).
- **Batch D** — `Source_Core/{src,include}/Ui/` (light zone; largest corpus — e.g. [SmatchetAiAssistantUi.cpp](Source_Core/src/Ui/SmatchetAiAssistantUi.cpp) 391 lines).
- **Batch E** — `Source_Core/` root `src` + `include` (`AppController*`, `Ai*`, `Logger`, etc.).
- **Batch F** — `Plugins/{LuaConsole,Whisper}` + `Target_Standalone/`.

Per-batch pipeline: analyzer report → mechanical strip (dry-run → apply) → LLM judgment pass → `clang-format -i` → dual-target build → lint-delta gate → `test-all.sh` → assert-code-unchanged → PR → CodeRabbit → merge gates.

## Files to modify

**New tooling (Phase 0):**
1. `scripts/dev/comment_audit.py` — read-only analyzer: walks first-party C++, tokenizes respecting string/char/raw-string literals, classifies each comment into taxonomy buckets, emits per-file + per-subsystem counts and a candidate-removal report (JSON + markdown). Establishes baseline.
2. `scripts/dev/comment_strip.py` — deterministic mechanical pass over the unambiguous *cut* buckets only; hardcoded protect-list; `--dry-run` + per-file unified diff; refuses to touch `///` / `/** */` bodies and multi-line rationale.
3. `scripts/dev/assert-code-unchanged.sh` — wraps a shared comment-stripper to normalize (strip comments + collapse whitespace) the base vs head version of every changed file; non-empty diff = fail. The safety gate.
4. `tests/dev/comment_tooling/` fixtures + a ctest (or bats) wrapper — one fixture per bucket; assert protect-list survives, noise removed, code-token stream preserved.

**Sweep targets (Phases 1–6), grouped by batch — every first-party `.cpp`/`.h`/`.hpp` under:**
5. `Source_Core/src/Tracker/`, `Source_Core/include/Tracker/` (Batch A).
6. `Source_Core/src/{Sync,Persistence,Config}/`, `Source_Core/include/{Sync,Persistence,Config}/` (Batch B).
7. `Source_Core/src/Commands/`, `Source_Core/include/Commands/`, `Plugins/Mcp/src/` (Batch C).
8. `Source_Core/src/Ui/`, `Source_Core/include/Ui/` (Batch D).
9. `Source_Core/src/*`, `Source_Core/include/*` root (Batch E).
10. `Plugins/LuaConsole/`, `Plugins/Whisper/`, `Target_Standalone/` (Batch F).

Excluded everywhere: `Source_Core/ThirdParty/`, `ThirdParty/`, `UnrealPlugins/`, generated code, `build/`, `tests/` (not in chosen scope).

## Existing utilities reused

- `scripts/dev/test-lint-rules.sh --diff` + `--selftest` (`scripts/dev/`) — delta-lint gate; the automated validator that the protect-list preserved every `no-printf-stderr` / `no-raw-new` / `define-imgui` / `deviation-overdue` suppressor. Strict-zone batches lean on this.
- `scripts/dev/is-pure-docs-diff.sh` — confirm comment diffs are correctly classified non-pure-docs (they touch `.cpp`/`.h`, so full build+test runs — expected).
- `clang-format` (PostToolUse hook per `.claude/CLAUDE.md`, or manual) — reflow after comment removal so block-comment trailers don't leave dangling continuation.
- `scripts/dev/test-all.sh` — test gate at each slice boundary.
- `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — dual-target build gate.
- `docs/design/_plan-template.md` — this doc's structure.
- `AGENTS.md` § Merge gates, § Autonomous ship-loop, § Process rules (one build + one test per slice) — batch cadence.

## UX Pillar callouts

Comments are non-executable; correct comment removal cannot alter runtime behavior. The `assert-code-unchanged` gate proves it per batch.

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: No impact — zero codegen delta (comment-only). Build + code-unchanged check verify no accidental code edit.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: No impact — no control-flow or I/O change.
- **Pillar 3 (never crash)**: No impact — no logic change; RAII / bounds / sanitizer surface untouched. Risk is confined to accidental code edits, caught by build + tests + code-unchanged gate.
- **Pillar 4 (accessibility)**: No impact.

## Perf-review-system gates

Diff touches `Source_Core/`, so this section is mandatory. All gates resolve **N/A — comment-only diff, no executable-path change** (proven by `assert-code-unchanged`):

1. **PR-fast CI** — N/A on content; however the curated diff→scenario map keys on **file path**, so a touched `Source_Core/src/Tracker/*.cpp` may *auto-fire* a scenario despite comment-only content. Expected delta ≈ 0; baselines unchanged; no `perf-out-of-band` label needed unless a spurious path-match flakes (it will not regress).
2. **Pillar 2 static scanner** — N/A — no new sync-I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — N/A — does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** — N/A — adds no sync-stall path.
5. **Marker inventory** — N/A — `SMATCHET_UI_PERF_SCOPE` markers are on the protect-list (never removed/added).

**Override**: not anticipated; `perf-out-of-band` only if a path-classified scenario spuriously fails.

## Risks / non-goals

**Risks:**
- **Protect-list incompleteness** → a load-bearing lint marker removed. *Mitigation*: hardcoded whitelist + fixture test; strict-zone delta-lint catches every lint-suppressor removal automatically; batch strict zones first.
- **Accidental code edit** while removing comments. *Mitigation*: `assert-code-unchanged` (comment-stripped token stream must be byte-identical base vs head) + dual-target build + `test-all.sh`. This is the primary net.
- **Over-removal of genuine *why*-knowledge** (not catchable by any automated gate). *Mitigation*: "Moderate" depth, LLM errs toward keep, small reviewable diffs, CodeRabbit, human review on each PR.
- **Tokenizer edge cases** — `//` inside string/char/raw-string literals (`R"(...)"`), line-continuation (`\` at EOL splicing comment into code), comments inside multi-line macros. *Mitigation*: literal-aware tokenizer (shared by analyzer + stripper + code-unchanged check); macro-body comments on protect-list; build gate catches splices.
- **`clang-format` reflow churn** after removal enlarges diffs. *Mitigation*: format inline per edit; accept (formatting is house policy anyway).

**Non-goals:**
- Not touching ThirdParty, Unreal plugins, generated code, or `tests/`.
- Not reducing markdown/doc comments (different corpus, separate effort if wanted).
- Not refactoring, renaming, reformatting non-comment code, trimming `#include`s, or reducing `LOG_*` calls.
- Not enforcing a blanket "X% reduction" — target the noise + self-evident + verbose buckets; report actual before/after.

## Verification

Per `AGENTS.md` § Verification automation — automated wherever physically possible.

- **Bucket A (pure-logic ctest, `test-rig`)**: fixture tests for `comment_audit.py` + `comment_strip.py` — one fixture per taxonomy bucket; assert (a) every protect-list marker survives, (b) each *cut* bucket is removed, (c) the stripped code-token stream is unchanged. This is the core automated proof of the tooling.
- **`assert-code-unchanged.sh` (per batch)**: comment-stripped + whitespace-normalized base vs head must be byte-identical for every changed file. Deterministic, zero manual steps — the safety gate.
- **Lint-delta gate (per batch)**: `scripts/dev/test-lint-rules.sh --diff` — zero new `(rule, file, snippet)` violations vs `origin/develop`. Validates the protect-list preserved all suppressors.
- **Build gate (per batch)**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) — catches any line-continuation / macro splice.
- **Test gate (per batch)**: `scripts/dev/test-all.sh` — sanity; comment changes must not move any test.
- **Metric**: analyzer before/after comment-line counts per batch + cumulative, reported in each PR body.
- **Bucket E (ImGui Test Engine)**: N/A — no UI behavior change.
- **Manual residue**: the "is this comment self-evident / redundant?" judgment is inherently subjective and not fully automatable. *Deferred-automation action plan*: the analyzer's candidate report + `assert-code-unchanged` bound the risk to "only comments changed"; remaining judgment is covered by per-PR human + CodeRabbit review. Add a `docs/backlog/agent-self-improvement/tooling.md` entry if a heuristic for "self-evident doc" proves worth automating after Batch A/B experience.

## Out of scope (flagged, not designed)

- **Markdown / `docs/` / script comments** — different corpus; separate plan if desired.
- **`tests/` C++ comments** — excluded per chosen scope; fold in via a follow-up batch if wanted.
- **ThirdParty / UnrealPlugins** — vendored / out-of-tree; never swept.
- **Non-comment size reduction** (include pruning, dead-code removal, function splitting) — separate efforts; this plan is comments-only by construction (enforced by `assert-code-unchanged`).
- **A repo-wide comment style guide / lint rule** to prevent regrowth — natural follow-up once the corpus is trimmed; no-action here.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*
