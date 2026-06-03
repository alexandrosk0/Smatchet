# Plan — DRY Quality Pillar + duplication delta-gate

> **Slug**: `dry-pillar-dup-gate` (matches this file's basename without `.md`).
>
> **Usage**: adds **DRY** as an **enforced Engineering Quality Pillar** backed by a duplication-detection **delta-gate** (grandfather existing, fail NEW only — like `function_size_audit.py` / `comment_audit.py`). Decided + grilled via `grill-with-docs` (2026-06-03); core decision in **[ADR-0015](../../adr/0015-dry-quality-pillar-duplication-gate.md)**.
>
> **Mandatory rules cross-link**: `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template. Touches **no `Source/` runtime code** (gate tooling + docs + CI) → perf-gate `N/A`.

## Context

The four existing **UX Pillars** (Performance / No-freeze / No-crash / Accessibility) are user-facing; Pillars 1-3 auto-fail PRs, Pillar 4 is aspirational. Code-quality lives in § Quality + the **Tiered high-integrity gates** (`function-too-long`/`too-branchy`, comment-bloat, no-raw-new, narrowing) — all **delta-gated** vs `origin/develop` with grandfathered baselines + `SMATCHET_DEVIATION` exemptions.

There is **no DRY / duplication enforcement** today (verified: no jscpd/cpd/pmd/simian in repo or CI). At **477** first-party C++ files of ImGui-heavy draw code, copy-paste duplication accumulates ungated. Maintainer direction (2026-06-03): make DRY an **enforced pillar** with a duplication delta-gate, mirroring the high-integrity machinery.

**Non-negotiable design constraint — DRY is double-edged.** A naive gate forces **premature abstraction + coupling**, the opposite failure of "small focused functions". The gate targets **copy-paste clones** (token-normalized identity), NOT "similar-shaped" code; intentional duplication (test goldens, dual-target forward-decls, per-backend `*Client` boilerplate, generated code) is cheaply exemptible; a DRY fix must never increase cross-subsystem coupling.

## Approach

**All forks resolved (grill-with-docs, 2026-06-03):**

1. **Pillar framing** — rename **"UX Pillars" → "Quality Pillars"**, two sub-groups: **UX** (Pillars 1-4) + **Engineering** (Pillar 5: **DRY**). Engineering pillars are enforced like UX 1-3.
2. **Detector** — a **custom Python `dup_audit.py`** (token-shingle + rolling-hash / winnowing), matching `function_size_audit.py`: `--diff origin/develop` keying, baseline-grandfather, `--selftest`, `SMATCHET_DEVIATION` exemptions. No new CI toolchain. **PMD-CPD is the documented fallback** if calibration shows detection gaps.
3. **Sensitivity (Moderate)** — token-based, **min clone ≈ 70 tokens / ≈ 8 lines** (tunable in calibration), **identifier-normalized** (catches copy-then-rename) + literal/whitespace-normalized, **cross-file primary** + **opt-in intra-file** (intra-file overlaps the function-size gate, so off by default).
4. **Verdict — WARN-first, graduate to block.** Ships non-blocking advisory (a `[dup] WARN` line, like the soft func-size tier); flips to **hard-block** once the committed trigger is met: **false-positive rate < 10% over ~20 PRs** (measured during calibration). "Enforced (calibration phase)", not indefinite advisory.
5. **Scope / zones** — whole **first-party C++** (`Source/Core` + `Source/Plugins` + `Source/Standalone`, the `comment_audit.py` SWEEP_ROOTS); **exclude** `tests/`, `ThirdParty/`, generated. Cross-file primary; intra-file opt-in per zone later.
6. **Baseline + exemptions** — grandfather snapshot at `docs/high-integrity/dup-baseline.md` (format mirrors `function-size-baseline.md`); exemption marker **`SMATCHET_DEVIATION(rule=duplication; reason=…; owner=…; revisit=…)`** above the clone. **Standing exemptions**: dual-target forward-decls (`SmatchetUI.h` etc.), per-backend `*Client` parallel boilerplate (`JiraClient`/`PlaneClient`/`GitHubClient`), generated code. (Test goldens are already out-of-scope via the `tests/` exclusion.)
7. **Owner** — **`code-review`** is the reviewer-of-record for duplication findings + exemption sign-off (the gate is automated; the owner approves intentional-clone exemptions).
8. **Over-abstraction guardrail** — (a) the gate only flags **copy-paste** (token-normalized clones), never structural similarity; (b) documented guidance: *"an exemption is cheap — prefer `SMATCHET_DEVIATION(rule=duplication)` over abstracting across unrelated contexts"*; (c) a review rule: a DRY-motivated refactor that introduces a shared helper coupling two otherwise-independent subsystems is a **code-review CRITICAL**, not an improvement. This guardrail is co-equal with the gate.

## Files to modify

- `docs/adr/0015-dry-quality-pillar-duplication-gate.md` — **done** (decision record).
- `AGENTS.md` — **edit**. `## UX Pillars` → `## Quality Pillars` (UX 1-4 + Engineering 5: DRY row + invariant + owner `code-review`); add the `duplication` rule to § Tiered enforcement (delta-gated, WARN-first, zones, `SMATCHET_DEVIATION` rule-id).
- `docs/agent-rules/ux-pillars.md` → **rename** `docs/agent-rules/quality-pillars.md`; add the **Engineering Pillars** sub-section (Pillar 5 DRY: invariant, gate, tools, owner, WARN→block graduation trigger).
- **All `§ UX Pillars` / "UX Pillar N" cross-references** — **grep + update** to "Quality Pillars" (the load-bearing AGENTS.md stub, the visual-validation-exception references, `ship-loops.md`, any agent prompt). Preserve external `AGENTS.md § <subsection>` resolution.
- `agents/scripts/core/dup_audit.py` — **new**. The delta scanner (`--diff`, `--list`, `--scan-file`, `--selftest`, baseline-grandfather, exemption-aware).
- `tests/bats/dup_audit.bats` — **new**. Fixtures (clone flagged / exempted clone passes / sub-threshold passes / identifier-renamed clone flagged / generated-path excluded).
- `.github/workflows/dup-scan.yml` — **new**. CI job (mirrors `pillar2-scan.yml`); WARN-only initially (non-failing) per the graduation gate.
- `docs/high-integrity/dup-baseline.md` — **new**. Grandfather snapshot.
- `agents/scripts/project/test-lint-rules.sh` — **edit**. Wire `dup_audit.py --diff` into the local delta gate (WARN tier).
- `scripts/dev/test-all.sh` / `pre-ship.sh` — **edit**. Include the dup scan in the local pre-push set.
- `agents/core/code-review.md` — **edit**. DRY-finding + exemption-sign-off duties + the coupling-CRITICAL guardrail rule.
- `docs/self-improvement/categories/process.md` — **edit (small)**. Backlog the WARN→block graduation as a tracked follow-up with the calibration trigger.

## Existing utilities reused

- `agents/scripts/core/function_size_audit.py` — **template** delta-scanner (`--diff`/`--list`/`--scan-file`/`--selftest`, `(rule, …)` keying, baseline-grandfather, soft-WARN tier).
- `agents/scripts/core/comment_audit.py` — second delta-scanner pattern (note the **UTF-8 decode fix** from #768 — `dup_audit.py` must decode git output as utf-8 from day one).
- `.github/workflows/pillar2-scan.yml` — CI gate-job template.
- `SMATCHET_DEVIATION` marker grammar + `docs/high-integrity/function-size-baseline.md` — exemption + baseline-snapshot formats.
- `scripts/dev/pre-ship.sh` + `test-lint-rules.sh` — local pre-push wiring.
- The subagent-eval **WARN→BLOCK graduation** precedent (`docs/agent-rules/subagent-eval.md`) — the calibration-gated graduation model.

## UX Pillar callouts

`N/A — tooling + docs + CI only; zero runtime / UI code. This plan ADDS a (Quality) pillar; it does not change the runtime the existing pillars govern.`

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

`N/A — no Source/ diff.` Local checks: `pre-ship.sh` + shell-lint + `dup_audit.py --selftest` + bats.

**Override**: `N/A`.

## Risks / non-goals

- **Risk — false positives push premature abstraction (the core DRY hazard).** Mitigation: copy-paste-only targeting; WARN-first calibration; cheap exemptions; the coupling-CRITICAL review rule (guardrail 8). The gate does not graduate to block until the FP trigger is met.
- **Risk — custom scanner's detection quality is worse than CPD.** Mitigation: known technique (token-shingle + rolling-hash/winnowing), `--selftest` + bats fixtures; PMD-CPD is the documented fallback if calibration reveals gaps.
- **Risk — noisy on ImGui boilerplate / per-backend `*Client` / generated code.** Mitigation: zone scoping + standing exemptions + the ~70-token threshold + identifier-normalization tuned in calibration.
- **Risk — the "Quality Pillars" rename breaks `§ UX Pillars` cross-references.** Mitigation: grep every reference (AGENTS.md stub, visual-validation exception, ship-loops, agent prompts) and update atomically; keep `quality-pillars.md` anchors resolving.
- **Risk — identifier-normalization over-flags renamed-but-legitimately-distinct code.** Mitigation: it's the reason for WARN-first; calibration measures it; threshold/normalization are tunable before block-graduation.
- **Non-goal**: retroactively de-duplicating the grandfathered baseline (forward-only).
- **Non-goal**: semantic/behavioural or cross-language duplication; tests / ThirdParty / generated code.
- **Non-goal**: flipping to hard-block within this plan — that's the calibration follow-up (backlogged with the trigger).

## Verification

- **`dup_audit.py --selftest`** — threshold + zones + rule-id in sync with this doc (mirrors `function_size_audit.py --selftest`).
- **Bats (`tests/bats/dup_audit.bats`)** — a known cross-file clone → flagged; the same with `SMATCHET_DEVIATION(rule=duplication)` → passes; a sub-threshold near-clone → passes; an identifier-renamed clone → flagged (proves normalization); a `ThirdParty/`/generated path → excluded.
- **`--diff origin/develop`** on a synthetic PR adding a copy-paste clone → emits a `[dup] WARN` (and, post-graduation, fails); same with an exemption marker → clean.
- **Baseline reproducible**; `dup-scan.yml` green (WARN-only) on current develop — all existing duplication grandfathered, zero new.
- **Rename integrity** — doc-anchor + plan-index gates green; no dangling `§ UX Pillars` reference (grep clean); visual-validation-exception references still resolve.
- **Shell-lint + pre-ship** clean on all new scripts.
- **Calibration (the graduation gate)** — over ~20 real PRs, record `dup_audit.py` flags + the human verdict (true clone vs false positive); when FP rate < 10%, file the WARN→block flip PR. Until then the gate is advisory.

## Out of scope (flagged, not designed)

- Retroactive de-duplication sweep of the grandfathered baseline (separate follow-up plan).
- The WARN→block flip itself (calibration follow-up, backlogged with the trigger).
- Semantic / cross-language duplication; a duplication-trend dashboard.
- Additional Engineering Pillars beyond DRY (the umbrella leaves room; none designed here).

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit)*

- **Slice 1 — scanner core (`feat/dup-audit-scanner`).** Shipped the detector + its tests + the
  grandfather snapshot, WARN-first and self-contained (NOT yet wired into any blocking gate — that
  is Slice 2). Files: `agents/scripts/core/dup_audit.py` (token-shingle + rolling-hash + winnowing
  clone detector; reuses `comment_lib.code_tokens` for literal-aware tokenization, then normalizes
  identifiers→`ID` + literals→`LIT` so copy-then-rename is caught; modes `--diff`/`--scan-file`/
  `--list`/`--baseline-md`/`--selftest` mirroring `function_size_audit.py`; delta-grandfather by
  normalized content-hash; `SMATCHET_DEVIATION(rule=duplication)` exemption checked across the clone
  span); `tests/bats/dup_audit.bats` (9 cases: selftest, intra-file detect, generated-path exclude,
  new-clone WARN, copy-then-rename caught, grandfathered silent, sub-threshold silent, deviation
  suppresses) + `agents/scripts/core/test-dup-audit-bats.sh` (auto-run by `test-all.sh`);
  `docs/high-integrity/dup-baseline.md` (snapshot: **449 cross-file clones** grandfathered, min 70
  tokens). Verified: selftest + 9/9 bats + shellcheck clean + `--diff origin/develop` clean (slice
  adds no C++). Observed (calibration note): the v1 baseline includes header include-guard/include
  boilerplate clones at `*.h:1` — a known false-positive class to consider skipping during the
  WARN→block calibration; harmless while grandfathered + WARN-first.

- **Slice 2 — wiring the gate live, WARN-first (`feat/dup-quality-pillar-wire`, stacked on Slice 1).**
  Made the duplication gate **live as advisory** without the pillar rename (see § Deviations for the
  reorder). Files: `.github/workflows/dup-scan.yml` (advisory CI job — runs `dup_audit.py --diff`,
  always exits 0, NOT a required check); `agents/scripts/project/test-lint-rules.sh` (advisory
  `dup_audit.py --diff` appended to `--diff` mode — never touches `$rc`; new `--dup-baseline` regen
  mode; `--selftest` now asserts the `duplication` rule is in AGENTS.md + runs `dup_audit.py
  --selftest`); `AGENTS.md` § Tiered enforcement (the `duplication` rule paragraph — delta-gated,
  WARN-first, copy-paste-only, `SMATCHET_DEVIATION(rule=duplication)`, standing exemptions, the
  coupling-CRITICAL guardrail); `agents/core/code-review.md` (reviewer-of-record DRY duties +
  exemption sign-off + the coupling-CRITICAL guardrail); `docs/self-improvement/categories/process.md`
  (the WARN→block graduation trigger backlog). `pre-ship.sh` inherits the advisory dup WARN for free
  (it calls `test-lint-rules.sh --diff`). Verified: `test-lint-rules.sh --selftest` green (incl.
  `dup_audit.py --selftest`), `--dup-baseline` byte-stable, shell-lint gate green.

## Deviations from plan
*(populated post-ship)*

- **Slice reorder — wiring (Slice 2) shipped before the "Quality Pillars" rename (now Slice 3).**
  The plan's § Files-to-modify bundles the AGENTS.md `## UX Pillars`→`## Quality Pillars` rename +
  `ux-pillars.md`→`quality-pillars.md` + cross-reference sweep with the gate wiring. A reference
  inventory found **81 files** mentioning "UX Pillar"/"ux-pillars" (most are *shipped/historical*
  plan docs + the plan-template `## UX Pillar callouts` section that cascades to every active plan),
  so a blanket rename would blow the CodeRabbit file-ceiling and rewrite history. Reordered to land
  the **enforcement value first** (Slice 2 = wiring, gate live as WARN) and isolate the cosmetic
  framing rename into its own reviewable **Slice 3** — using the doc-anchors substring-match property
  to keep a `**UX Pillars**` sub-group anchor so `§ UX Pillars` refs resolve without touching the 81
  files (only the ~6 `ux-pillars.md` path links + the section heading change). The § Tiered-enforcement
  `duplication` rule references "Engineering Quality Pillar 5" forward (ADR-0015), so the framing is
  consistent even before the umbrella heading is renamed.

## Verification (actual)
*(populated post-ship)*
