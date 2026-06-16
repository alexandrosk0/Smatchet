# Plan — CodeQL c-cpp code scanning

> **Slug**: `codeql-code-scanning` (matches this file's basename without `.md`).
>
> **Status**: `active` — the machine-readable lifecycle marker. Values: `active` (driving in-flight work) · `shipped` (post-ship sections populated + all cited PRs merged — this file belongs in `docs/plans/shipped/`) · `blocked` / `deferred` (paused — one-line why). **Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s this file active → shipped** (see § Archive). `agents/scripts/core/plan-archival-owed.sh` nags at SessionStart if any `active/` plan is marked `shipped` but never moved.
>
> **Usage**: copy this template to `docs/plans/active/<slug>.md` as the first step of any new plan. Fill every section. Sections that genuinely don't apply get `N/A — <one-line reason>`, not deletion — the headings drive the "did you consider this?" forcing function for every author + reviewer agent.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

No static-analysis / SAST gate covers the first-party C++ tree today. The 21 existing GitHub Actions workflows cover build/test, sanitizers (asan/tsan), coverage, perf, duplication, doc-validation, and mobile-security file-content checks — but no semantic dataflow analysis (taint, injection, use-after-free, null-deref reachability) over `Source/**`. CodeQL fills that gap.

`alexandrosk0/Smatchet` is a **PUBLIC, user-owned** (non-org) repo, so CodeQL code scanning runs **free** — no GitHub Advanced Security license (GHAS gates private repos only). Language is C/C++14 → CodeQL `c-cpp` pack, fully supported.

After this lands, every PR that touches `Source/**` (and a weekly cron over `develop`) runs a CodeQL `c-cpp` analysis whose findings surface as code-scanning **alerts** in the Security tab — **advisory**, not merge-blocking (WARN-first calibration, mirroring the `duplication` / `unused-symbol` precedent).

## Approach

Add an **Advanced-setup** CodeQL workflow (custom `.github/workflows/codeql.yml`), not GitHub's Default setup. Default setup's autobuild cannot drive this repo's custom CMake presets + `msvc-dev-cmd` environment + FetchContent dependency download; Advanced setup with `build-mode: manual` lets us run the exact preset CI already uses.

Build on `windows-2022` (matches `build-and-test.yml`'s `windows-msvc` job): `ilammy/msvc-dev-cmd` → `cmake --preset ninja-iter-msvc` → `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone`. The full (non-light) feature set is deliberate — it pulls Core + Plugins (incl. MCP/AI/Lua) into the traced compile so CodeQL's DB covers the security-sensitive trust boundaries (`security-review`'s surface: MCP / CLI / Lua / HTTP / SQLite).

**Critical trade-off — sccache must be OFF for the CodeQL build.** CodeQL traces real compiler invocations to build its database; an sccache cache-hit serves a cached `.obj` with no compiler launch, so the tracer sees nothing and the DB is incomplete/empty. Set `SMATCHET_NO_CCACHE=1` (honored at `CMakeLists.txt:154`, which gates the `find_program(... ccache sccache)` wrap on that env var being undefined). This means a cold full compile (~30–45 min) every run — acceptable because the workflow is **PR-path-gated on `Source/**` + weekly cron**, not run on every PR.

Findings are inherently advisory: the `CodeQL` check goes **green on successful analysis** regardless of how many alerts it finds (alerts land in the Security tab, not as a check failure), and the check is **not** added to `agents/scripts/core/merge-gates.sh`'s meant-to-block allow-list. Graduation to blocking (allow-list + `AGENTS.md` § Merge gates) is a deliberate follow-up after a calibration window, not part of this plan.

## Files to modify

1. `.github/workflows/codeql.yml` *(new)* — the CodeQL workflow. Triggers: `pull_request` filtered to `paths: ['Source/**', '.github/workflows/codeql.yml', '.github/codeql/**']`; `schedule` weekly cron; `workflow_dispatch` for manual runs. Single job on `windows-2022`: checkout → `msvc-dev-cmd` → `github/codeql-action/init@v3` (`languages: c-cpp`, `build-mode: manual`, `config-file: ./.github/codeql/codeql-config.yml`) → CMake configure+build with `SMATCHET_NO_CCACHE=1` in `env:` → `github/codeql-action/analyze@v3`. `permissions: { security-events: write, contents: read, actions: read }` (SARIF upload needs `security-events: write`).
2. `.github/codeql/codeql-config.yml` *(new)* — scope + query selection. `paths: [Source]` and `paths-ignore: [ThirdParty, build, '**/_deps/**']` so analysis + alerts cover only first-party code (FetchContent deps are still *compiled* for a complete DB, but excluded from alerting). `queries: security-extended` start point (can dial to `security-and-quality` or back to default after calibration).

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

- **Build time / runner minutes** — sccache-off cold compile is ~30–45 min/run. Mitigation: PR-path-gated on `Source/**` + weekly cron (not every PR); public-repo Actions minutes are free.
- **Manual build-mode brittleness** — if the CMake configure/build step fails inside the CodeQL runner (FetchContent network, MSVC env drift), `analyze` produces an empty/partial DB. Mitigation: the build step fails loud (non-zero exit reds the workflow run) — a broken build can't silently green-wash an empty DB; investigate as a normal CI break.
- **Alert noise on first run** — `security-extended` over a multi-MLOC tree may surface a large initial backlog. Mitigation: advisory-only (no merge block); triage/dismiss in the Security tab; dial query suite down if noise outweighs signal during calibration.
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
- **Custom CodeQL queries** — repo-specific `.ql` packs for Smatchet invariants (e.g. sync-I/O-on-UI-thread as a query). No-action this round; the built-in `security-extended` suite is the starting baseline.
- **SARIF-as-required-check / PR annotations threshold** — configuring CodeQL to red a PR on new alerts. No-action: intentionally advisory until graduated.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped (empirically ~62% of post-ship plans drifted stale-in-place). Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
   > **Keep the literal `<slug>` placeholder in this committed step — do NOT
   > expand it to this plan's real filename.** Writing the actual basename here
   > manufactures a `docs/plans/shipped/<name>.md` path that points at a file
   > still living in `active/` (the move hasn't happened yet), which
   > `test-plan-ref-integrity.sh` reports as a dangling self-reference. The gate
   > carves out the *placeholder* form on the Archive `git mv` line; the
   > expanded form defeats that carve-out. Run the literal command with your
   > slug substituted at the shell — never bake the expansion into the file.
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md` (the gates resolve it against any tier; PR #890), so the move can't break them. Write new plan references tier-less.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*
