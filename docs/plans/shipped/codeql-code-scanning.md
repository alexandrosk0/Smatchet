# Plan — CodeQL c-cpp code scanning

> **Slug**: `codeql-code-scanning` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — the machine-readable lifecycle marker. Values: `active` (driving in-flight work) · `shipped` (post-ship sections populated + all cited PRs merged — this file belongs in `docs/plans/shipped/`) · `blocked` / `deferred` (paused — one-line why). **Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s this file active → shipped** (see § Archive). `agents/scripts/core/plan-archival-owed.sh` nags at SessionStart if any `active/` plan is marked `shipped` but never moved.
>
> **Usage**: copy this template to `docs/plans/active/<slug>.md` as the first step of any new plan. Fill every section. Sections that genuinely don't apply get `N/A — <one-line reason>`, not deletion — the headings drive the "did you consider this?" forcing function for every author + reviewer agent.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

No static-analysis / SAST gate covers the first-party C++ tree today. The 21 existing GitHub Actions workflows cover build/test, sanitizers (asan/tsan), coverage, perf, duplication, doc-validation, and mobile-security file-content checks — but no semantic dataflow analysis (taint, injection, use-after-free, null-deref reachability) over `Source/**`. CodeQL fills that gap.

`alexandrosk0/Smatchet` is a **PUBLIC, user-owned** (non-org) repo, so CodeQL code scanning runs **free** — no GitHub Advanced Security license (GHAS gates private repos only). Language is C/C++14 → CodeQL `c-cpp` pack, fully supported.

After this lands, a daily cron over `develop` (plus a PR trigger scoped to **trust-boundary paths only** — MCP / Lua / HTTP / SQLite / CLI / AI) runs a CodeQL `c-cpp` analysis whose findings surface as code-scanning **alerts** in the Security tab — **advisory**, not merge-blocking (WARN-first calibration, mirroring the `duplication` / `unused-symbol` precedent).

## Approach

Add an **Advanced-setup** CodeQL workflow (custom `.github/workflows/codeql.yml`), not GitHub's Default setup. Default setup's autobuild cannot drive this repo's custom CMake presets + `msvc-dev-cmd` environment + FetchContent dependency download; Advanced setup with `build-mode: manual` lets us run the exact preset CI already uses.

Build on `windows-2022` (matches `build-and-test.yml`'s `windows-msvc` job): `ilammy/msvc-dev-cmd` → `cmake --preset ninja-iter-msvc` → `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone`. The full (non-light) feature set is deliberate — it pulls Core + Plugins (incl. MCP/AI/Lua) into the traced compile so CodeQL's DB covers the security-sensitive trust boundaries (`security-review`'s surface: MCP / CLI / Lua / HTTP / SQLite). **One build only** (Standalone) — code behind `#ifdef SMATCHET_EMBEDDED_IN_UNREAL` / the `SmatchetCore_DX12` target is rendering-backend glue (low taint-surface) and the Standalone target doesn't compile those branches, so CodeQL won't see them; a second DX12 trace is a deferred follow-up, not v1.

**Cadence — hybrid (grill-resolved).** A **daily cron** over `develop` (`0 5 * * *`, offset 1 h from `sanitizer-nightly`'s `0 4` so they don't contend for the runner) + `workflow_dispatch`, plus a **PR trigger scoped to trust-boundary paths only** (the `paths:` filter in Files-to-modify item 1). Rationale: the no-sccache cold compile is ~30–45 min and findings are advisory (don't gate merge), so per-PR-on-all-`Source/**` would add that latency to nearly every code PR for no merge-decision change — the repo already runs its heavy ASan/UBSan analysis nightly-not-per-PR for this exact reason (`sanitizer-nightly.yml:4-8`). Scoping the PR trigger to the trust boundaries catches issues pre-merge on the diffs that matter while keeping ordinary code PRs fast.

**Query suite — staged escalation ladder (grill-resolved).** Start at the **default `security`** suite (high-confidence security queries only), then escalate in two gated steps once each baseline is triaged-clean:
1. **v1 — default `security`**: ship. Confirm signal:noise on the Security tab.
2. **→ `security-extended`**: once the v1 alert backlog is triaged to zero open, switch the config `queries:` to `security-extended` (adds lower-precision security queries). One-line config PR.
3. **→ `security-and-quality`**: once `security-extended` is likewise triaged-clean, add the maintainability/reliability pack. One-line config PR.

Each rung is a one-line `codeql-config.yml` change; the ladder applies the same WARN-first calibration logic to *query volume* that the advisory/blocking decision applies to *enforcement* — avoid dumping a large low-precision backlog on the first run, which just trains the tab to be ignored.

**Critical trade-off — sccache must be OFF for the CodeQL build.** CodeQL traces real compiler invocations to build its database; an sccache cache-hit serves a cached `.obj` with no compiler launch, so the tracer sees nothing and the DB is incomplete/empty. Set `SMATCHET_NO_CCACHE=1` (honored at `CMakeLists.txt:154`, which gates the `find_program(... ccache sccache)` wrap on that env var being undefined). This means a cold full compile (~30–45 min) every run — acceptable because the workflow is **daily-cron + PR-trigger scoped to trust-boundary paths**, not run on every code PR.

Findings are inherently advisory: the `CodeQL` check goes **green on successful analysis** regardless of how many alerts it finds (alerts land in the Security tab, not as a check failure), and the check is **not** added to `agents/scripts/core/merge-gates.sh`'s meant-to-block allow-list. Graduation to blocking (allow-list + `AGENTS.md` § Merge gates) is a deliberate follow-up after a calibration window, not part of this plan.

## Files to modify

1. `.github/workflows/codeql.yml` *(new)* — the CodeQL workflow.
   - **Triggers**: `schedule` daily `cron: '0 5 * * *'`; `workflow_dispatch: {}`; `pull_request` (base `develop`) filtered to the trust-boundary `paths:` set —
     ```yaml
     paths:
       - 'Source/Plugins/Mcp/**'
       - 'Source/Plugins/LuaConsole/**'
       - 'Source/Core/src/Tracker/**'
       - 'Source/Core/src/Persistence/**'
       - 'Source/Core/src/Sync/**'
       - 'Source/Core/src/Commands/**'
       - 'Source/Core/src/Config/**'
       - 'Source/Core/src/Ai*.cpp'
       - 'Source/Core/src/OpenAi*.cpp'
       - '.github/workflows/codeql.yml'
       - '.github/codeql/**'
     ```
     **No `merge_group` trigger** — advisory + no merge queue on this user-owned repo, so it need not report on the synthetic queue ref (unlike the meant-to-block workflows that keep it future-proofed).
   - **Single job** on `windows-2022`, `timeout-minutes: 90` (cold no-sccache full build + DB extraction + query), `concurrency: { group: codeql-${{ github.ref }}, cancel-in-progress: true }`, `permissions: { security-events: write, contents: read, actions: read }` (SARIF upload needs `security-events: write`). Cron checks out `ref: develop` (mirrors `sanitizer-nightly.yml:43`).
   - **Steps**: checkout → `ilammy/msvc-dev-cmd@v1` → `github/codeql-action/init@v3` (`languages: c-cpp`, `build-mode: manual`, `config-file: ./.github/codeql/codeql-config.yml`) → CMake configure+build with `SMATCHET_NO_CCACHE=1` in the job `env:` → `github/codeql-action/analyze@v3`. Set `env: FORCE_JAVASCRIPT_ACTIONS_TO_NODE24: "true"` (repo convention — Node 20 deprecated 2026-06-16; all workflows set it). Pin actions to `@v3`/`@v1` major tags per repo convention.
2. `.github/codeql/codeql-config.yml` *(new)* — scope + query selection. `paths: [Source]` and `paths-ignore: [ThirdParty, build, '**/_deps/**']` so analysis + alerts cover only first-party code (FetchContent deps are still *compiled* for a complete DB, but excluded from alerting). `queries:` starts at the **default `security`** suite (v1); the staged ladder in § Approach escalates it to `security-extended` then `security-and-quality` via one-line config PRs once each baseline is triaged-clean.

No product C++ / CMake / agentic-contract files change — this is a CI-config + plan-doc diff only. (Grep confirmed no pre-existing `codeql` workflow or `.github/codeql/` dir.)

## Existing utilities reused

- `CMakeLists.txt:154` (`NOT DEFINED ENV{SMATCHET_NO_CCACHE}` guard) — the existing knob that disables the ccache/sccache compiler wrap; the workflow sets it to keep the CodeQL tracer honest. No new build plumbing.
- `ilammy/msvc-dev-cmd@v1` + `cmake --preset ninja-iter-msvc` — the exact toolchain bring-up + preset already proven in `build-and-test.yml` `windows-msvc`; reused verbatim, not reinvented.
- `sanitizer-nightly.yml` cron pattern — the model for "heavy, scheduled, not-per-PR" analysis lanes; the CodeQL schedule block mirrors its cadence shape.

## UX Pillar callouts

Per `AGENTS.md` § UX Pillars. This change adds a CI workflow + config; it touches **no product code, no UI thread, no runtime path**.

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — CI-only, no runtime code.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — no product code.
- **Pillar 3 (never crash)**: no impact at runtime; *net-positive over time* — CodeQL surfaces null-deref / use-after-free / uninitialized-read classes that feed Pillar 3 hardening (advisory alerts, no auto-fix).
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: N/A — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

**N/A — the diff is CI-config + plan-doc only; no `Source/Core/` (or any product C++) file changes.** PR-fast CI, Pillar-2 static scanner, dispatcher-drain, bucket-E visible-cue harness, and marker-inventory all N/A for the same reason. No `perf-out-of-band` label needed.

## Risks / non-goals

- **Build time / runner minutes** — sccache-off cold compile is ~30–45 min/run. Mitigation: daily cron + PR-trigger scoped to trust-boundary paths only (not every code PR); public-repo Actions minutes are free.
- **Manual build-mode brittleness** — if the CMake configure/build step fails inside the CodeQL runner (FetchContent network, MSVC env drift), `analyze` produces an empty/partial DB. Mitigation: the build step fails loud (non-zero exit reds the workflow run) — a broken build can't silently green-wash an empty DB; investigate as a normal CI break.
- **Alert noise on first run** — even the default `security` suite over a multi-MLOC tree may surface a backlog. Mitigation: advisory-only (no merge block); start at the lowest-volume default suite (not `security-extended`); triage/dismiss in the Security tab; escalate the query ladder only after each baseline is clean (§ Approach).
- **sccache-omission regression** — a future edit could drop `SMATCHET_NO_CCACHE=1` and silently gut the DB. Mitigation: an inline comment in the workflow citing this plan + `CMakeLists.txt:154`; accepted residual risk (no automated guard this round).
- **Non-goal: merge-blocking.** This plan ships CodeQL advisory only — it does **not** add the check to `merge-gates.sh` allow-list or amend `AGENTS.md` § Merge gates. Graduation is a separate, post-calibration plan.
- **Non-goal: Android/mobile + Lua-as-language.** `c-cpp` only this round; CodeQL also supports the repo's Lua? (no — CodeQL has no Lua pack) and Java/Kotlin (Android) — Android SAST stays with `mobile-security.yml`. No second language pack added here.
- **Non-goal: autobuild / Default setup.** Explicitly rejected (can't drive the custom presets); not revisited.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps where physically possible.

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no C++ logic added; nothing unit-testable.
- **Bucket E (ImGui Test Engine)**: N/A — no UI surface.
- **Bash-driver scenario / screenshot / sanitizer**: N/A — no runtime behavior changes.
- **Build gate**: N/A — no product C++ / CMake source changes (the workflow *invokes* the existing `ninja-iter-msvc` build; it does not modify build inputs). The CodeQL workflow run itself is the build exercise.
- **Workflow self-verification (the real gate)**: on the PR, the `CodeQL` workflow run must (a) complete the manual CMake build green, (b) upload SARIF, (c) appear as a green check, and (d) populate the repo Security → Code scanning tab with `c-cpp` alerts (count > 0 acceptable — they're advisory). Confirm `SMATCHET_NO_CCACHE=1` took effect by checking the build log shows compiler invocations (not "sccache" wrapping) and the resulting DB is non-trivial in the `init`/`analyze` step summary. If `actionlint` is available locally, lint both new YAML files before push.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms before finalising; record the outcome. Required for every plan — do not delete.
- **Manual residue**: the Security-tab alert-count check is a one-time human glance post-merge (first run). Deferred-automation action: a follow-up could assert alert-DB non-emptiness via `gh api` in-workflow — logged here, no `tooling.md` entry yet (revisit at graduation). No silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them. (Nothing deferred from a prior plan feeds this one; sweep expected empty.)

- **Merge-gate graduation** — adding `CodeQL` to `merge-gates.sh` allow-list + `AGENTS.md` § Merge gates so findings block. Follow-up plan after a calibration window proves signal:noise.
- **Additional language packs** — Android Java/Kotlin CodeQL analysis. No-action: mobile SAST stays with `mobile-security.yml`; revisit if mobile C++/Java surface grows.
- **DX12 / Unreal-embedded coverage** — a second traced build (`--target SmatchetCore_DX12`) to union the `#ifdef SMATCHET_EMBEDDED_IN_UNREAL` branches into the DB. Follow-up: low taint-surface (rendering glue) + doubles the cold compile; revisit if the DX12 path grows external-input handling.
- **Query-suite escalation** — moving the ladder from default `security` → `security-extended` → `security-and-quality` (§ Approach). Each rung is its own one-line config PR, gated on the prior baseline being triaged-clean — intentionally NOT done in this plan's PR.
- **Custom CodeQL queries** — repo-specific `.ql` packs for Smatchet invariants (e.g. sync-I/O-on-UI-thread as a query). No-action this round; the built-in suites are the starting baseline.
- **SARIF-as-required-check / PR annotations threshold** — configuring CodeQL to red a PR on new alerts. No-action: intentionally advisory until graduated.

## Implementation log
- `639846ee` (squash-merged to develop as `886d2d37`, PR #1346) · feat(ci): add CodeQL c-cpp advisory code scanning — two new files: `.github/workflows/codeql.yml` (Advanced setup, `build-mode: manual`, `ninja-iter-msvc` on `windows-2022`, `SMATCHET_NO_CCACHE=1`, hybrid `schedule` + `workflow_dispatch` + trust-boundary-`paths` `pull_request` triggers, advisory) and `.github/codeql/codeql-config.yml` (`paths: [Source]` scope + default security suite, escalation ladder documented as comments).

## Deviations from plan
- **Checkout omits `ref:`** (§ Files-to-modify item 1 said "Cron checks out `ref: develop` (mirrors `sanitizer-nightly.yml:43`)"). `sanitizer-nightly` can hardcode `develop` because it is cron-only; this workflow also has a `pull_request` trigger, where `ref: develop` would scan `develop` instead of the PR head and defeat the PR trigger the hybrid cadence relies on. The repo default branch *is* `develop`, so omitting `ref:` satisfies the cron intent (schedule scans `develop`) **and** gives PR runs the PR-merge head — correct for all three triggers.
- **`queries:` left unset rather than naming a `security` suite** (§ Approach / § Files-to-modify called v1 the "default `security`" suite). CodeQL has no bare `security` suite alias; the default suite — run when `queries:` is unset — *is* the high-confidence security set. The config documents the escalation ladder (→ `security-extended` → `security-and-quality`) as commented lines. No behavioural difference from the plan's intent.

## Verification (actual)
- **CodeQL workflow self-ran on PR #1346** (the diff touched `.github/workflows/codeql.yml` + `.github/codeql/**`, both in the `paths:` filter) — **passed** (run 27664343557).
  - Step timing: Configure (`ninja-iter-msvc`) ~5.3 min · Build `SmatchetStandalone` ~20.8 min · Perform CodeQL analysis ~7 min — all green.
  - The ~21-min cold compile confirms `SMATCHET_NO_CCACHE=1` took effect (real compiler invocations traced, not an empty/no-op build) → the database is non-trivial.
  - SARIF uploaded: tool=CodeQL, 58 rules (default security suite), **0 results** → clean baseline; 0 open `c-cpp` alerts in Security → Code scanning.
- **Advisory confirmed**: `CodeQL analyze (c-cpp)` reported green/SUCCESS; not in `agents/scripts/core/merge-gates.sh` allow-list; did not block merge.
- **doc-validation**: `scripts/dev/test-docs.sh` green (13/13) before push.
- **Branch protection**: all 9 required checks on PR #1346 green or skipped; admin-squash-merged to `develop` as `886d2d37`.
- **Not-run (observational, no action)**: the daily cron's first scheduled run (fires 05:00 UTC) and per-PR behaviour on a non-boundary-path code PR — both confirmable in CI over the coming days, no code change owed.
