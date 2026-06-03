# DRY is an enforced Quality Pillar via a WARN-first duplication delta-gate

**Status:** accepted (2026-06-03)

DRY (Don't-Repeat-Yourself) becomes a first-class **enforced quality invariant**, not just a § Quality guideline. To house it, the existing **"UX Pillars"** umbrella is renamed **"Quality Pillars"** with two sub-groups: **UX** (1-4: Performance / No-freeze / No-crash / Accessibility — user-facing) and **Engineering** (5: DRY — code-maintainability). DRY is enforced by a **duplication delta-gate** that grandfathers all existing duplication and fails only NEW clones vs `origin/develop` — the same machinery as `function_size_audit.py` / `comment_audit.py`. The detector is a **custom Python scanner** (`dup_audit.py`, token-shingle rolling-hash), not jscpd/PMD-CPD, to match the repo's gate idiom and avoid a new CI toolchain. It ships **WARN-first** (advisory) and graduates to hard-block once a committed false-positive trigger is met (FP < 10% over ~20 PRs).

## Considered options

- **Aspirational principle only** (like Pillar 4 Accessibility) — a stated § Quality rule, no gate. *Rejected*: maintainer wanted real enforcement.
- **Absolute gate** (fail any clone over threshold) — *Rejected*: 477 first-party C++ files of ImGui-heavy code carry large existing duplication; an absolute gate would fail nearly everything. Delta-gate (grandfather) is the only workable model.
- **jscpd / PMD-CPD** detector — higher out-of-box detection, *Rejected* for now: adds a Node/Java CI dependency and wrapper work to fit the `--diff`/baseline/exemption model; custom scanner is repo-idiomatic. PMD-CPD kept as the fallback if calibration shows detection gaps.
- **Hard-block from day one** — *Rejected*: Moderate sensitivity (identifier-normalized, ~70-token clones) will produce false positives on ImGui boilerplate; WARN-first calibration avoids blocking legit PRs before the gate is tuned.
- **Add DRY as a 5th "UX Pillar"** — *Rejected*: DRY is engineering, not user-facing; a "UX Pillar" that isn't about UX is muddy. Hence the "Quality Pillars" umbrella.

## Consequences

- **Rename churn**: `## UX Pillars` → `## Quality Pillars`; `docs/agent-rules/ux-pillars.md` → `quality-pillars.md`; every `§ UX Pillars` / "UX Pillar N" cross-reference updated (the load-bearing stub + visual-validation-exception references must keep resolving).
- **New gate infra**: `dup_audit.py` (+ `--selftest` + bats), a `dup-scan.yml` CI job (mirrors `pillar2-scan.yml`), a `docs/high-integrity/dup-baseline.md` grandfather snapshot, and a `SMATCHET_DEVIATION(rule=duplication)` exemption rule-id.
- **Double-edged-DRY guardrail is a hard requirement**: the gate targets copy-paste only (token-normalized clones), exemptions are cheap and encouraged over abstraction, and a DRY-motivated refactor must not increase coupling across unrelated subsystems. Standing exemptions: dual-target forward-decls, per-backend `*Client` boilerplate, generated code.
- **WARN→block graduation** is a committed follow-up gated on calibration data, mirroring the subagent-eval graduation pattern — the gate is "enforced (calibration phase)", stronger than aspirational, not yet a merge block.
- **Owner**: `code-review` is the reviewer-of-record for duplication findings + exemption sign-off.
- Full design + thresholds + migration: `docs/plans/shipped/dry-pillar-dup-gate.md`.
