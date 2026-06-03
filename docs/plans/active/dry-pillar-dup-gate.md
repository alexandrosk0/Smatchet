# Plan — DRY pillar + duplication delta-gate

> **Slug**: `dry-pillar-dup-gate` (matches this file's basename without `.md`).

> **Usage**: adds **DRY (Don't-Repeat-Yourself)** as an **enforced** quality pillar backed by a duplication-detection **delta gate** (grandfather existing, fail only NEW duplication — like `function_size_audit.py` / `comment_audit.py`). Decided via `grill-with-docs` (2026-06-03). **DRAFT — grill in progress; forks marked `‹GRILL›` are unresolved.**

> **Mandatory rules cross-link**: `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template. Touches no `Source/` runtime code (gate tooling + docs + CI) → perf-gate `N/A`.

## Context

The four existing **UX Pillars** (Performance / No-freeze / No-crash / Accessibility) are user-facing invariants; Pillars 1-3 auto-fail PRs, Pillar 4 is aspirational. Code-quality lives separately in § Quality (RAII, small focused functions) + the **Tiered high-integrity gates** (`function-too-long`/`too-branchy`, comment-bloat, no-raw-new, narrowing) — all **delta-gated** vs `origin/develop` with grandfathered baselines + `SMATCHET_DEVIATION` exemptions.

There is **no DRY / duplication enforcement today** (verified: no jscpd/cpd/pmd/simian in the repo or CI). At 477 first-party C++ files of ImGui-heavy draw code, real copy-paste duplication accumulates with no gate. Maintainer direction (2026-06-03): make DRY an **enforced pillar** with a duplication delta-gate, mirroring the high-integrity gate machinery.

**Tension to respect (non-negotiable design constraint):** DRY is double-edged — a naive gate pushes **premature abstraction + coupling**, the opposite failure of "small focused functions". The gate must target **copy-paste clones** (high token-identical similarity), NOT "similar-shaped" code, and intentional duplication (test goldens, dual-target forward-decls, per-backend boilerplate, generated code) must be cheaply exemptible.

## Approach

**Locked:**
- DRY is a **5th enforced pillar** with a **duplication delta-gate** (grandfather existing, fail NEW only). `‹GRILL: pillar framing — rename "UX Pillars"→"Quality Pillars" (UX 1-4 + Engineering 5) vs a separate "Engineering Pillars" section vs 5th row in the existing table›`

**`‹GRILL›` — open design forks (to resolve in this session):**

1. **Detector tool** — `‹GRILL: jscpd (Node, proven, multi-lang, JSON out) vs PMD-CPD (Java, mature C++ tokenizer) vs a custom Python token-hash scanner matching the repo's function_size_audit.py / comment_audit.py idiom (no new toolchain dep, full control, but reinvents clone detection)›`
2. **What counts as a clone** — `‹GRILL: token-based vs line-based; minimum clone size (CPD default 100 tokens, jscpd default 50 tokens / 5 lines); cross-file only vs also intra-file; ignore-identifiers/literals normalization (catches renamed copy-paste) vs exact-token (fewer false positives)›`
3. **Gate verdict** — `‹GRILL: hard block (auto-fail PR, like function-too-long) vs WARN-first advisory (like the soft 100-line func-size tier / comment-ratio), graduating to block once calibrated›`
4. **Scope / zones** — `‹GRILL: whole first-party C++ (Source/Core + Plugins + Standalone) vs the strict zone only; exclude tests/ ThirdParty/ generated; intra- vs cross-file per zone›`
5. **Baseline + exemptions** — `‹GRILL: baseline snapshot format (docs/high-integrity/dup-baseline.md, like function-size-baseline) + exemption marker — reuse SMATCHET_DEVIATION(rule=duplication) vs a dedicated marker; standing exemptions for test goldens / dual-target / per-backend boilerplate / generated code›`
6. **Owner agent** — `‹GRILL: who owns DRY review in the pillar table — code-review (existing) vs a new dup-specialist; the gate is automated so the owner is the reviewer-of-record for exemption sign-off›`
7. **Over-abstraction guardrail** — `‹GRILL: how the protocol prevents the gate from forcing premature abstraction — documented "exemption is cheap; do not abstract across unrelated contexts" guidance + the gate's copy-paste-only targeting + a review rule that a DRY fix must not increase coupling›`

**Likely shape (pending grill):** a `agents/scripts/.../dup_audit.py --diff origin/develop` scanner (keyed like `function_size_audit.py`), a `dup-scan.yml` CI job (mirrors `pillar2-scan.yml`), a `docs/high-integrity/dup-baseline.md` snapshot, `SMATCHET_DEVIATION`-style exemptions, a `docs/agent-rules/` or § Tiered-enforcement rule entry, and the pillar-table addition.

## Files to modify

*(finalized post-grill — depends on tool + framing forks)*. Anticipated:
- `AGENTS.md` / `docs/agent-rules/ux-pillars.md` — pillar addition + framing.
- `agents/scripts/{core,project}/dup_audit.py` (+ tests) — the delta scanner. **new**.
- `.github/workflows/dup-scan.yml` — CI gate. **new**.
- `docs/high-integrity/dup-baseline.md` — grandfather snapshot. **new**.
- `AGENTS.md` § Tiered enforcement + `docs/high-integrity/` — rule entry + `SMATCHET_DEVIATION` rule-id.
- `agents/core/code-review.md` (or new agent) — owner + exemption-review duties.
- `scripts/dev/test-all.sh` / `pre-ship.sh` — wire the local delta check.

## Existing utilities reused

- `agents/scripts/core/function_size_audit.py` — the **template** delta-scanner (`--diff origin/develop`, `(rule, basename, …)` keying, baseline-grandfather, `--selftest`).
- `agents/scripts/core/comment_audit.py` — second delta-scanner pattern (`--diff`, baseline).
- `.github/workflows/pillar2-scan.yml` — CI gate-job template.
- `SMATCHET_DEVIATION` marker grammar — exemption mechanism.
- `docs/high-integrity/{baseline,function-size-baseline}.md` — baseline snapshot format.
- `scripts/dev/pre-ship.sh` + `test-lint-rules.sh` — local pre-push gate wiring.

## UX Pillar callouts

`N/A — tooling + docs + CI only; zero runtime / UI code. (This plan ADDS a pillar; it doesn't change the runtime that pillars 1-4 govern.)`

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

`N/A — no Source/ diff.` Local checks: `pre-ship.sh` + shell-lint + the new scanner's `--selftest` + bats.

**Override**: `N/A`.

## Risks / non-goals

- **Risk — false positives push premature abstraction (the core DRY hazard).** Mitigation: copy-paste-only targeting (high token-identical threshold, not structural similarity); cheap exemptions; an explicit review rule that a DRY fix must not raise coupling; WARN-first if calibration is uncertain. `‹GRILL 3/7›`
- **Risk — noisy on ImGui boilerplate / generated code.** Mitigation: zone scoping + standing exemptions + tuned min-token threshold + exclude generated/ThirdParty. `‹GRILL 2/4/5›`
- **Risk — huge existing duplication makes an absolute gate unusable.** Mitigation: **delta gate** (grandfather baseline) — locked.
- **Risk — new CI toolchain dep (Node for jscpd / Java for PMD-CPD).** Mitigation: weigh a custom Python scanner (no new dep, repo-idiomatic) in the tool fork. `‹GRILL 1›`
- **Non-goal**: retroactively de-duplicating the grandfathered baseline (forward-only, like the other high-integrity gates).
- **Non-goal**: detecting semantic/behavioural duplication (only textual/token clones).
- **Non-goal**: cross-language duplication; tests / ThirdParty / generated code.

## Verification

*(finalized post-grill)*. Anticipated:
- Scanner `--selftest` (rule/threshold in sync with the doc) + bats fixtures (a known clone → flagged; an exempted clone → passed; a sub-threshold near-clone → passed).
- `--diff origin/develop` on a synthetic PR that adds a copy-paste clone → fails; the same with an exemption marker → passes.
- Baseline snapshot reproducible; `dup-scan.yml` green on current develop (all existing dup grandfathered).
- Shell-lint + pre-ship + doc-anchor/plan-index gates green.
- Calibration: dry-run the scanner over recent merged PRs; confirm the false-positive rate is acceptable before flipping WARN→block (if WARN-first chosen).

## Out of scope (flagged, not designed)

- Retroactive de-duplication sweep of the grandfathered baseline (separate follow-up).
- Semantic / cross-language duplication detection.
- A duplication metric dashboard / trend tracking.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
