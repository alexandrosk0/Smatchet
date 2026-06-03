# Plan — Agent-kit productization (version + manifest + publish path)

> **Slug**: `agent-kit-productization` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

Smatchet already built the **internals** of a portable agent harness and never shipped the **product wrapper**. Comparison against two other agentic repos made the gap precise:

- **ECC** (`affaan-m/ECC`) is a distributable Claude-Code plugin — harness-level version (`2.0.0-rc.1`), a manifest/catalog (63 agents / 249 skills / 79 commands), an install flow, a skills-first surface policy. Thin internals, strong packaging.
- **Ghostty** is the portability-primitive minimum — agents.md-spec canonical file, `CLAUDE.md→AGENTS.md` symlink, a governance doc (`AI_POLICY.md`) separated from instruction.

Smatchet is the inverse of ECC: **product-grade internals, no packaging.** Already in place — the portable/project split (`agents/core/` + `agents/_shared/` vs `agents/project/`), config indirection (`project.config.json` whose `_doc` literally documents the reuse recipe), a self-retargeting purity guard (`test-portable-purity.sh`, denylist generated from the config), a capability-tag→tool adapter table (which ECC lacks), and a de-facto installer (`setup-harness.sh`). Missing: a kit **version**, a **manifest**, consumer-facing **USAGE/governance**, an **install-into-a-foreign-repo** path, and — the real blocker — **actual purity** (the guard is a *baseline* guard today; its header concedes the prompts still embed Smatchet specifics, full de-Smatchet-ification a tracked-but-undone follow-up).

**Intended outcome — one sentence:** after this lands, `agents/core/` + `agents/_shared/` are a **versioned, manifested, governed kit** with a green path from "copy the portable tree + rewrite `project.config.json`" to a working install in another repo — with the publish/extract step *designed* and *gated on* the purity baseline reaching zero.

## Approach

Three phases, sequenced by risk and by a hard dependency: **publish is blocked on purity.**

**Phase A — Packaging (low risk, in-repo, ships value even solo).** Add the artifacts that turn a directory into a product: a SemVer `agents/VERSION` + `agents/CHANGELOG.md` (with explicit breaking-change semantics for an *agent* kit), a generated `agents/MANIFEST.md` catalog (+ `agents/manifest.json` machine form) produced by a scanner over `core/*.md` frontmatter and `_shared/skills/*/SKILL.md`, a parity selftest so the manifest can't drift, and a consumer-facing `agents/USAGE.md` + governance note (who may install, attribution — the Ghostty `AI_POLICY` shape, scoped to external reuse).

**Phase B — Purity to zero (the long pole; gates real reuse).** `test-portable-purity.sh` runs in *baseline* mode — it only fails on leakage *not already in* `docs/high-integrity/portable-purity-baseline.txt`. That baseline is the grandfathered Smatchet-ism debt. Drive it down: replace `Smatchet` / `Source/Core` / `SMATCHET_*` / `ITrackerClient` literals in `core/` + `_shared/` prompts with `project.config.json`-sourced placeholders or generic phrasing, burning the baseline toward empty. Add a `--count` burn-down readout and a zero-tolerance graduation flag. **No clean external install exists until this hits zero** — so Phase C *build* depends on it.

**Phase C — Install path + publish (design now, build gated on B).** Extend the installer with an into-a-target-repo mode that copies the portable tree and scaffolds a starter `project.config.json` from the schema. Design — but do **not** build until a real second consumer exists — the extract/publish topology (a `smatchet-agent-kit` repo via git subtree; main repo consumes back via subtree pull) in an ADR. Speculative extraction for a single-maintainer, single-consumer repo is upkeep with no payoff; the ADR captures the design so it's ready when a second consumer appears.

**Non-obvious trade-off, named:** the whole effort is only worth it if Smatchet wants external consumers or a second project. Phases A + B pay off **regardless** (a manifest is a nice overview; cleaner prompts reduce per-session tokens and reviewer confusion), so they proceed unconditionally. Phase C *build* is explicitly gated on a real second consumer to avoid productization-for-its-own-sake.

## Files to modify

Grouped by phase (list > 10 entries).

**Phase A — packaging:**
1. `agents/VERSION` (new) — SemVer, seed `0.1.0`. Single line.
2. `agents/CHANGELOG.md` (new) — Keep-a-Changelog format. Breaking-change semantics defined inline: **MAJOR** = agent removed/renamed, capability tag dropped, or agent output-contract change; **MINOR** = new agent/skill or additive capability; **PATCH** = prompt wording / bugfix. Seed with the shipped `per-subsystem-agent-docs` work + this plan.
3. `agents/scripts/core/gen-kit-manifest.py` (new) — scan the **portable kit only** — `agents/core/*.md` frontmatter (`name`, `description`, `complexity`, `capabilities`, `triggers`, `delegates-to`, `version`, `read-only`) + `agents/_shared/skills/*/SKILL.md` → emit `MANIFEST.md` (human table, ECC-roster shape) + `manifest.json` (machine). Reuses the python-probe pattern from `test-portable-purity.sh`. **Scope = portable kit only**: `agents/project/*` is intentionally excluded (project-bound; see § Non-goals).
4. `agents/scripts/core/gen-kit-manifest.sh` (new) — thin wrapper (git-root cd + python probe), passes `test-shell-lint.sh` (5 rules).
5. `agents/MANIFEST.md` (new, generated) — the catalog. Header carries the `agents/VERSION` + a "generated, do not hand-edit" marker.
6. `agents/manifest.json` (new, generated) — machine form for harness tooling.
7. `agents/USAGE.md` (new) — consumer-facing: what the kit is, who it's for, the copy-tree + rewrite-`project.config.json` install recipe (lifted from the config `_doc` + `docs/PORTABILITY.md`), pointer to the § Harness adapter capability table, and a short external-use governance / attribution note (Ghostty `AI_POLICY` shape, reuse-scoped).
8. `agents/scripts/core/test-kit-manifest.sh` (new) — selftest: `MANIFEST.md`/`manifest.json` ↔ on-disk agent/skill set parity (every `core/` agent and `_shared/` skill present exactly once; `agents/VERSION` is valid SemVer; `CHANGELOG.md` has an entry for the current `VERSION`). FAIL on drift.
9. `project.config.json` (edit) — add `test-kit-manifest` to `guards.doc_validation`; add a `kit` block (`version_file: agents/VERSION`, `manifest: agents/MANIFEST.md`).
10. `tests/bats/kit_manifest.bats` (new) — covers manifest parity, SemVer-format, changelog-entry-present, and a hand-edited-manifest FAIL.
11. `docs/STRUCTURE.md` (edit) — taxonomy: where `VERSION` / `CHANGELOG.md` / `MANIFEST.md` / `USAGE.md` live + the "kit = `core` + `_shared` + `docs/agent-rules` + `docs/harness`" product boundary.
12. `scripts/dev/test-all.sh` (edit) — invoke `test-kit-manifest.sh` in the doc-validation group.

**Phase B — purity to zero:**
13. `agents/core/*.md` + `agents/_shared/**` (edit, sweep) — replace project literals (`Smatchet`, `Source/Core`, `SMATCHET_*`, `ITrackerClient`, build presets) with `project.config.json`-sourced placeholders or generic phrasing; burn down `portable-purity-baseline.txt`. Mechanical, `mechanic`-agent shaped; per-file, reviewable.
14. `docs/high-integrity/portable-purity-baseline.txt` (shrinks each sweep PR) — the burn-down ledger.
15. `agents/scripts/core/test-portable-purity.sh` (edit) — add `--count` (print baseline size) for burn-down tracking + a `--strict` zero-tolerance mode that fails on ANY leakage (graduation switch, flipped once baseline empties).
16. `docs/PORTABILITY.md` (edit) — document the version/manifest/release model + the "purity-baseline = 0 before publish" bar.

**Phase C — install + publish (design-only build-deferred):**
17. `agents/scripts/core/install-kit.sh` (new, Phase C) — `<target-repo-path>`: copy the four portable dirs into the target, scaffold `project.config.json` from `project.config.schema.json`, then run `setup-harness.sh <harness>` rooted in the target. Until built, `setup-harness.sh`'s hardcoded `cd "$(dirname)/../../.."` git-root assumption stays in-repo-only.
18. `docs/adr/NNNN-agent-kit-extraction.md` (new, Phase C) — design the subtree topology + the main-repo-consumes-back flow; **build deferred** until a second consumer exists.

## Existing utilities reused

- `project.config.json` + its `_doc` reuse recipe + `scripts/dev/project-config.sh` (scripts source it) — the parameterization seam; manifest + install read from it, never hardcode.
- `agents/scripts/core/test-portable-purity.sh` — baseline mechanism + denylist-generated-from-config; Phase B extends it, doesn't replace it.
- `agents/scripts/core/setup-harness.sh` — the existing per-harness linker; Phase C wraps it for foreign targets.
- `docs/PORTABILITY.md` — existing portable/project boundary doc; gains the release model.
- `agents/scripts/core/test-shell-lint.sh`, the `guards.doc_validation` group, `tests/bats/` harness — gate the new scripts.
- The python-executable-probe block in `test-portable-purity.sh` — copy verbatim into the manifest generator (handles the Windows `python3` Store-alias stub).
- Agent frontmatter schema already in `core/*.md` (`name`/`description`/`capabilities`/`triggers`/`delegates-to`/`version`) — the manifest's input shape; no new metadata invented.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: no impact — docs + shell/python tooling, zero runtime C++.
- **Pillar 2 (UI-thread never blocks)**: no impact — no UI code touched.
- **Pillar 3 (never crash)**: no impact — no product code touched.
- **Pillar 4 (accessibility)**: no impact — out of scope.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

N/A — no Source/Core code touched; no C++ source anywhere. The diff is not pure-docs (is-pure-docs-diff.sh allow-lists agents/scripts, markdown, and docs, but agents/VERSION, agents/manifest.json, project.config.json, scripts/dev/test-all.sh and tests/bats are deny-listed), so test-all.sh runs in full. With no compiled change the build, ctest, and perf gates are no-ops; verification reduces to shell-lint plus bats plus doc-validation.

## Risks / non-goals

**Risks:**
- **Portable dirs are not pure today** (baseline-grandfathered Smatchet-isms; the guard concedes it). Shipping an install path now would emit a Smatchet-contaminated kit. → **Phase C build is hard-gated on `portable-purity-baseline.txt` = 0** (Phase B). Phase A packaging is honest about this in `USAGE.md` ("alpha — prompts still carry project specifics; track the burn-down").
- **Manifest drift** (generated file goes stale vs the tree). → `test-kit-manifest.sh` parity selftest + bats; regenerate in the same PR as any agent add/remove.
- **Single-maintainer over-engineering** — Phase C extraction is high-upkeep, low-payoff without a second consumer. → Phase C *build* gated on a real second consumer; A + B stand alone.
- **Version-semantics bikeshed** for an agent kit. → breaking-change rule defined explicitly in `CHANGELOG.md` (above), not left to judgment.
- **`ITrackerClient` still in `project.config.json` `literals`** (line 15) though the interface is deleted — correct as a *denylist* term (blocks reintroduction into portable prompts), but flag in the Phase B sweep so it's not mistaken for a live symbol.

**Non-goals:**
- Building the separate `smatchet-agent-kit` repo now — design + ADR only.
- De-Smatchet-ifying `agents/project/*` — those are *meant* to be project-specific; they stay out of the portable set.
- Changing the capability-tag → tool adapter table — already product-grade.
- Publishing to any marketplace / plugin registry.
- Versioning `agents/project/*` agents as part of the kit — kit = `core` + `_shared` only.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps.

- **Bucket A (pure-logic ctest)**: `N/A — no C++ added.`
- **Bucket E (ImGui Test Engine)**: `N/A — no UI added.`
- **Gate scenario (Phase A)**: `bash agents/scripts/core/test-kit-manifest.sh` + `tests/bats/kit_manifest.bats` — (a) clean when `MANIFEST.md`/`manifest.json` match the on-disk agent+skill set; (b) **FAIL** on a hand-added agent missing from the manifest, a malformed `agents/VERSION`, and a `VERSION` with no matching `CHANGELOG.md` entry; (c) regenerating via `gen-kit-manifest.sh` makes (a) pass.
- **Burn-down (Phase B)**: `bash agents/scripts/core/test-portable-purity.sh --count` prints baseline size; CI asserts it is **monotone non-increasing** vs `origin/develop` (a sweep PR may only shrink it). `--strict` stays off until the baseline empties.
- **Shell lint**: `test-shell-lint.sh` on every new script (5 rules).
- **Doc integrity**: `test-markdown-links.sh`, `test-doc-anchors.sh`, the `guards.doc_validation` group green (now including `test-kit-manifest`).
- **Install dry-run (Phase C, deferred)**: `install-kit.sh --dry-run <tmpdir>` lists the copied tree + scaffolded config without writing — bats-covered when Phase C builds.
- **Build gate**: `N/A — no compile. Shell-lint + bats only.`
- **Manual residue**: none for A/B. Phase C's "second consumer exists" gate is a human judgment call, not an automatable check — recorded as the explicit build-trigger in the ADR, not silent residue.

## Out of scope (flagged, not designed)

- **Marketplace / registry publication** — even after extraction, distribution channel is a separate effort.
- **A `release-kit.sh`** (bundle VERSION-bump + CHANGELOG + manifest-regen into one release command) — nice-to-have once the manual release proves out.
- **Skills-first surface-precedence policy** (the ECC steal) — a separate small charter edit, tracked in the rule-content suggestions batch (#1).
- **5-principle preamble + context-budget rule** (ECC steals #2/#3) — separate charter-tightening plan.
- **Coverage-floor gate** (ECC steal #4) — its own plan; orthogonal to packaging.

## Implementation log
*(populated post-ship — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
