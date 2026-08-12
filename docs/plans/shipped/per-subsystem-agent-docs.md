# Plan — Per-subsystem agent docs (rules + glossary + orientation)
<!-- plan-date: 2026-06-02 -->

> **Slug**: `per-subsystem-agent-docs` (matches this file's basename without `.md`).
>
> **Supersedes**: the never-filed `nested-subsystem-agents-md` rule-extraction intent + the `tracker-context-docs` glossary/orientation plan (now archived at `docs/plans/shipped/tracker-context-docs.md`). Both merged here.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

Two independent plans converged on the same seam — `Source/Core/src/<ctx>/`:

1. **Rule locality** — the per-subsystem invariants live centrally in the `agents/core/code-review.md` checklist (§ Subsystem invariants + § UI-thread non-blocking, lines 82–101), partly duplicated in the project specialist agents (e.g. `agents/project/tracker-backend.md`). The root `AGENTS.md` § Project rules holds *global* rules + the strict-zone *enumeration* (§ Tiered enforcement zones) — **not** the invariants themselves. An agent editing one Tracker file still loads the whole code-review checklist (every subsystem + the UI-thread block). Input-token tax every session; AGENTS.md § Tiered enforcement alone is a ~600-word bullet (evidence of central-doc bloat — a global lint rule, not extracted here).
2. **Domain orientation** — agents landing in `Source/Core/src/Tracker/` (~40 files, densest subsystem) get no glossary and no map. The repo's plan-doc rules + AGENTS.md grep already *assume* per-context `src/<ctx>/CONTEXT.md` files exist — they do not. The lone global glossary (`docs/CONTEXT.md`) carries a **stale** canonical term: `ITrackerClient` — an interface **confirmed deleted** (header gone; split into live `ITrackerBackend` + 5 role interfaces `ITrackerCollaboration` / `ITrackerConnectivity` / `ITrackerFieldCatalog` / `ITrackerIssueMutations` / `ITrackerIssueReader`).

These are complementary, not competing. Each subsystem wants **three** kinds of doc, one format-contract each: **rules** (imperative do/don't), **glossary** (what terms mean), **orientation** (how files relate / request flow). Doxygen / generated API docs were considered and rejected (LLMs read source directly; vexp gives semantic index + skeletons; Doxygen's `@param`/banner idiom collides with the anti-comment-bloat gates).

**Intended outcome — one sentence:** after this lands, each heavy `Source/Core/src/<ctx>/` owns up to three agent docs (`AGENTS.md` rules, `CONTEXT.md` glossary, `README.md` orientation), all registered in a root `CONTEXT-MAP.md` and guarded by one coverage+staleness gate; `code-review.md` shrinks to global rules + a "read the touched subsystem's leaf" pointer (root `AGENTS.md` gains only a guides index); no rule is duplicated central↔leaf; and Tracker is the full exemplar every later subsystem copies.

## Approach

**Three artifacts per subsystem, cleanly separated** (no overlap; each its own format contract):

| Artifact | Content | Format contract | Loaded how |
|---|---|---|---|
| `AGENTS.md` | imperative rules / invariants | agents.md spec | harness auto-loads (nearest-wins) |
| `CONTEXT.md` | domain glossary only (one-sentence term defs, relationships, zero impl detail) | `agents/_shared/skills/grill-with-docs/CONTEXT-FORMAT.md` | agent / semantic-search on demand |
| `README.md` | orientation: request flow, role-of-each-file; durable-by-construction (no `file:line`, no counts, freshness header) | **this plan's** orientation shape (grill-with-docs defines no README artifact) | agent / semantic-search on demand |

**Split scope by cost.** Rule extraction is **mechanical** (text already written, just relocate) → do it for the **5 subsystems that have extractable rules today**: `Tracker`, `Commands`, `Persistence`, `Sync` (from the code-review § Subsystem-invariants bullets) + `Ui` (the UI-thread-non-blocking checklist). These are **not** the strict-lint-zone set — `Ui` is a **light** zone (AGENTS.md § Tiered enforcement zones) yet carries the richest scoped checklist, while `Config` is **strict** (same zones list) but has no subsystem-specific rule to host today. Leaf creation is driven by *content present*, not by the lint-zone list. Glossary + orientation need a **grill** (careful human-authored domain work) → **Tracker exemplar only**; Commands / Persistence / Sync / Ui / flat-root deferred to per-subsystem follow-up grills (backlog). `Config` / `Diagnostics` / `Imaging` / `Privacy` get an `AGENTS.md` only if the discovery pass surfaces a real scoped rule — no empty-file theater.

**One root registry, one gate.** Root `CONTEXT-MAP.md` (skill-literal location) lists every context's three artifacts + the system-wide `docs/CONTEXT.md`, and doubles as the **harness-discovery index** for any harness that doesn't honor nearest-wins. A single gate `agents/scripts/project/test-subsystem-docs.sh` enforces both: **FAIL** on structural breakage (a leaf listed in `CONTEXT-MAP.md` missing on disk, an on-disk `src/<ctx>/AGENTS.md` absent from `CONTEXT-MAP.md`, or a rule string duplicated central↔leaf) and **WARN** (never blocks) on README staleness (diff vs `origin/develop` touches a context's `.cpp`/`.h` but not its `README.md`). The gate keys off the `CONTEXT-MAP.md` registry, **not** the lint strict-zone list — the two axes are independent (a strict-zone dir need not have a leaf; a leaf need not be strict-zone).

Every central-text removal leaves a **1-line stub + pointer** (preserves the delegation.md stub-and-link precedent; keeps external `AGENTS.md § <topic>` refs resolving). `docs/CONTEXT.md` stays the system-wide glossary, **untouched**, linked from the map (minor `UpdateField`/`TrackerIssueKey` overlap accepted, not migrated).

**Non-obvious trade-offs, named:**
- *Token win depends on load-on-demand.* If a harness eager-loads every nested `AGENTS.md`, the win inverts for that harness. Step 2 verifies per-harness load behavior **before** finalizing the `AGENTS.md` extraction; eager-loaders stay pointer-only (root index) until they support nearest-wins. This risk applies to `AGENTS.md` **only** — `CONTEXT.md`/`README.md` are read on demand, never auto-loaded.
- *Authoring against live headers.* The `ITrackerClient` rot proves central docs drift. Every Tracker term/rule in the new leaf docs is sourced from a header read during this work (Step 0) — cross-linked by header **name**, never `file:line`.

## Files to modify

Grouped (list > 10 entries).

**Scaffold — convention, registry, gate (uniform):**
1. `CONTEXT-MAP.md` (new, repo root) — registry of contexts: per-context the three artifacts + system-wide `docs/CONTEXT.md` + growth path. Root location is skill-literal so `grill-with-docs` infers structure; also the harness-discovery index.
2. `agents/scripts/project/test-subsystem-docs.sh` (new) — unified gate: FAIL on structural/parity breakage, WARN on README staleness. Reuses the `test-lint-rules.sh --diff origin/develop` delta mechanism; goes through `test-shell-lint.sh` (5 rules). `project/` dir, not `core/` — it hardcodes project paths (`Source/Core/src/*`); matches the placement of existing project-bound scripts (`test-lint-rules.sh`, `p4-*.sh`) and keeps `agents/scripts/core/` path-agnostic per the portable/project split. Confirm against `test-portable-purity` scope at implementation.
3. `scripts/dev/test-all.sh` (edit) — invoke the gate in the pre-push sequence (local + CI, not CI-only).
4. `docs/STRUCTURE.md` (edit) — add the 3-artifact per-subsystem convention to the normative taxonomy + portable/project boundary (leaves are project-specific → exempt from `test-portable-purity`).
5. `docs/harness/SETUP.md` (edit) — document nearest-wins discovery per harness (Claude on-demand subdir load, Codex/agents.md nearest-file, Cursor `.cursor/rules` glob, Aider/generic manual) + eager-load fallback (applies to `AGENTS.md` only).

**Rule extraction — mechanical, 5 subsystems with scoped rules today (from `nested-subsystem-agents-md`):**
6. `AGENTS.md` (edit) — **light touch.** AGENTS.md holds *global* rules + the strict-zone list (§ Tiered enforcement zones), not per-subsystem invariants, so nothing is extracted *from* it. Add a § Subsystem guides stub pointing at `CONTEXT-MAP.md` + the leaf set, and cross-link the strict-zone enumeration (§ Tiered enforcement zones) to the relevant leaves. Global rules untouched.
7. `agents/core/code-review.md:82` (edit) — **primary extraction source.** Replace § Subsystem invariants (82–89) + § UI-thread non-blocking (91–101) with "for each touched `Source/Core/src/<sub>/` file, read that dir's `AGENTS.md` and apply". Keep global C++14/dual-target/conventions/perf. Bump `version:`. **Side-effect fix:** deletes the stale `ITrackerClient.h` ref at line 83. **Eval note:** code-review is the subagent-eval Phase-1 MVP (AGENTS.md § Subagent eval) — this edit gets base-vs-head scored (advisory WARN, non-blocking).
8. `Source/Core/src/Tracker/AGENTS.md` (new) — backend-specific (`Jira*`/`Plane*`) no-leak into shared interfaces; HTTP only via `TrackerHttpClient` (flag direct `cpr::`); field-value flow catalog→parser→payload; writes wire to `OfflineQueueService` + `BackendAuditTrail`/`FieldEditAuditSource`. **Authored against live `ITrackerBackend.h` + 5 role headers — never `ITrackerClient`.**
9. `Source/Core/src/Commands/AGENTS.md` (new) — `const CommandContext&` sig, structured error envelope, `args` default `{}`; MCP/Lua/Scenarios route through `CommandRegistry` (flag bypass).
10. `Source/Core/src/Persistence/AGENTS.md` (new) — SQLite schema additive-only (flag drops/renames/type changes).
11. `Source/Core/src/Sync/AGENTS.md` (new) — offline-queue + audit-trail wiring for writes through Sync.
12. `Source/Core/src/Ui/AGENTS.md` (new — note `Ui/` is a **light** lint zone yet hosts the richest scoped checklist) — full UI-thread-non-blocking checklist (cpr/SQLite/p4/file-I/O off render path, `future::get` guard, no `join`/`sleep_for` on UI thread, mutex-across-I/O, dispatcher-drain chunking) + pointer to `docs/guides/imgui-draw-pattern.md`.

**Tracker domain docs — grilled exemplar (from `tracker-context-docs`):**
13. `Source/Core/src/Tracker/CONTEXT.md` (new) — glossary per `CONTEXT-FORMAT.md`. Roster grouped `## Backend abstraction` (`ITrackerBackend` + 5 roles: Collaboration/Connectivity/FieldCatalog/IssueMutations/IssueReader; `TrackerIssueKey`; set-replace semantics) / `## Field model` (`TrackerField`/`Family`/`Option`, Field catalog + cache) / `## Issue creation` (`IssueDraft`, `IssueCreatePipeline`, Field payload) / `## Query` (`JqlProjectScope`/`SuggestEngine`, Fixture backend). `CachedTicket` cross-referenced (owned by Persistence), not redefined.
14. `Source/Core/src/Tracker/README.md` (new) — orientation: backend-abstraction shape, create/update request flow, per-backend divergence points, fixture-vs-live split. Freshness header, no line numbers.

**Supersession:**
15. `tracker-context-docs` archived to `docs/plans/shipped/tracker-context-docs.md` (merged here via PR #717); `nested-subsystem-agents-md` was never filed as a standalone plan — its rule-extraction intent is absorbed here.

## Existing utilities reused

- `agents/scripts/project/test-lint-rules.sh --diff origin/develop` — delta-vs-develop diff mechanism, copied for the staleness half of the gate.
- `agents/scripts/core/is-pure-docs-diff.sh` — gate skips staleness when diff is pure-docs (no code → no staleness); also classifies this whole change docs-only → build/ctest skipped.
- `agents/scripts/core/test-doc-anchors.sh` + `test_doc_anchors.py`, `test-markdown-links.sh` — anchor + relative-link integrity for the new pointers and `CONTEXT-MAP.md` links; no new link-checker.
- `agents/scripts/core/test-portable-purity.sh` `--selftest` self-consistency pattern — mirrored by the new gate's structural selftest.
- `scripts/dev/test-all.sh` existing gate-invocation block — append, don't restructure.
- `agents/_shared/skills/grill-with-docs/CONTEXT-FORMAT.md` — glossary contract `CONTEXT.md` must satisfy.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: no impact — docs + one shell gate, zero runtime C++.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: no runtime impact; *strengthens* enforcement — the UI-thread checklist moves next to `Source/Core/src/Ui/`, loaded exactly when render code is edited.
- **Pillar 3 (never crash)**: no impact — no product code touched.
- **Pillar 4 (accessibility)**: no impact — out of scope.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

N/A — pure-docs + shell. The only `Source/Core/` additions are Markdown under `src/<ctx>/`; no `.cpp`/`.h` compiled into either target changes. `is-pure-docs-diff.sh` classifies the C++-tree portion docs-only, so PR-fast CI / Pillar-2 scanner / dispatcher-drain / bucket-E / marker-inventory gates do not fire.

## Risks / non-goals

**Risks:**
- **Harness eager-load inverts the token win** (AGENTS.md only). → Step 2 verifies load-on-demand per harness before finalizing; eager-loaders stay pointer-only via `CONTEXT-MAP.md` index.
- **Central ↔ leaf drift** (rule edited one place not the other; registry vs on-disk leaves diverge). → `test-subsystem-docs.sh` FAILs on registry-listed-leaf-missing, on-disk-leaf-absent-from-registry, or duplicated rule string; runs pre-push.
- **Glossary/orientation rot** (the `ITrackerClient` failure mode). → durable-by-construction authoring (no `file:line`/counts) + WARN staleness gate + every term sourced from a header read in Step 0. Residual: WARN catches file-touch divergence, not *semantic* staleness.
- **`code-review.md` loses self-containment** — reviewer must load the touched subsystem leaf. → explicit "read `Source/Core/src/<touched>/AGENTS.md`" process step; files are short, skeleton/semantic-search cheap.
- **Staleness false-positives** on no-op code edits. → accepted (WARN, non-blocking); PR label `subsystem-docs-out-of-band` documents intentional skips + reserves the future FAIL-graduation escape.
- **Stale external `ITrackerClient` refs beyond what extraction deletes** — confirmed live (header gone; `git grep ITrackerClient`): `agents/project/tracker-backend.md` (×5), `agents/core/debug-detective.md` (×4), `docs/CONTEXT.md` (×3), `docs/agent-rules/delegation.md` (×3), `agents/core/architect.md` (×2), `agents/core/coderabbit-triage.md` (×2) — **19 hits across 6 files** (excludes `code-review.md:83`, fixed by extraction). Historical/generated hits are intentionally **not** swept: `docs/adr/0003-github-as-itrackerclient.md` + `0007` + `0009` (dated immutable records — the name was correct when written), `docs/high-integrity/portable-purity-baseline.txt` (auto-generated snapshot), `docs/self-improvement/categories/applied.md` (archived); `docs/self-improvement/categories/infra.md` (×1) is a live backlog note to verify-or-repoint in the sweep. → extraction fixes `code-review.md:83` for free; the rest is a `mechanic` sweep, backlogged (below). Per AGENTS.md § Plan-doc family (its scope-reduction final-check-grep rule), grep the `ITrackerClient` keyword family + repoint/stub every hit touched by this PR.

**Non-goals:**
- Glossary/orientation for non-Tracker subsystems — per-subsystem follow-up grills (backlog).
- Leaf `AGENTS.md` for the 4 light dirs unless Step 1 surfaces a real rule.
- Touching `Source/Plugins/{Mcp,LuaConsole}` — separate tree; follow-up once pattern proven.
- Migrating `docs/CONTEXT.md` — left untouched, term overlap accepted (grill decision).
- Changing any enforcement gate, lint zone, or merge gate — purely relocating + adding readable docs.
- Trimming the non-subsystem root sections (ship-loops/merge-gates/delegation) — separate context-surface-diet plan.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps.

- **Bucket A (pure-logic ctest)**: `N/A — no C++ added.`
- **Bucket E (ImGui Test Engine)**: `N/A — no UI added.`
- **Gate scenario**: `bash agents/scripts/project/test-subsystem-docs.sh --diff origin/develop` + `tests/bats/subsystem_docs.bats` asserting — (a) clean exit when every `CONTEXT-MAP.md`-listed leaf is present + Tracker README untouched with no Tracker code change; (b) **FAIL** on a synthetic registry-listed leaf missing on disk, an on-disk `src/<ctx>/AGENTS.md` absent from the registry, and a rule string duplicated central↔leaf; (c) **WARN** line on a synthetic Tracker `.cpp` edit lacking a README touch; (d) data-driven discovery picks up a second `src/<ctx>/README.md`.
- **Doc integrity**: `test-doc-anchors.sh`, `test-markdown-links.sh`, `test-portable-purity.sh` green; gate run with `--selftest` to confirm its `CONTEXT-MAP.md` registry parses + matches on-disk leaves.
- **Shell lint**: `test-shell-lint.sh` on the new gate (5 rules).
- **Harness discovery (Step 2)**: per supported harness, open a file under `Source/Core/src/Tracker/` and confirm the leaf `AGENTS.md` is the cited rule source; record per-harness load behavior in `SETUP.md`.
- **Pure-docs classification**: `is-pure-docs-diff.sh` returns docs-only → build/ctest skipped.
- **Build gate**: `N/A — pure-docs; dual-target unaffected.`
- **Manual residue**: glossary/README *content* correctness is human-reviewed at PR (inherent to docs). The per-harness discovery check is semi-manual → deferred-automation action = `docs/self-improvement/categories/tooling.md` entry to script a headless nearest-wins probe once a fixture exists. No silent residue.

## Out of scope (flagged, not designed)

- **`mechanic` sweep of stale `ITrackerClient` → `ITrackerBackend` (+ 5 roles)** across `agents/project/tracker-backend.md`, `agents/core/architect.md`, `agents/core/debug-detective.md`, `agents/core/coderabbit-triage.md`, `docs/agent-rules/delegation.md`, `docs/CONTEXT.md` — real bug (split shipped, names never updated). Backlog `docs/self-improvement/categories/process.md`.
- **`Source/Plugins/Mcp/AGENTS.md`** — strict-zone dir outside `Source/Core/src`; follow-up once leaf pattern proven.
- **Glossary + orientation for Commands / Persistence / Sync / Ui / flat-root** — per-subsystem follow-up grills.
- **Root `AGENTS.md` global-section brevity** (ship-loops / merge-gates / delegation) — separate plan.
- **Graduating the staleness gate WARN → FAIL** — deferred until the convention proves out across ≥2 subsystems; PR-label escape built now so graduation is config-only.
- **Cursor `.cursor/rules` glob authoring** — if Step 2 shows Cursor needs per-glob files to honor locality, its own follow-up (documented in SETUP.md).

## Implementation log

- `4a4888ea` · 5 leaf `AGENTS.md` (Tracker/Commands/Persistence/Sync/Ui) + Tracker `CONTEXT.md`/`README.md` + `code-review.md` extraction (v3→v4) + root `AGENTS.md` § Subsystem guides stub + `CONTEXT-MAP.md` registry + `test-subsystem-docs.sh` gate.
- `fa729722` · `setup-harness.sh`/`.ps1` shim-gen + `.gitignore` shim pattern + `doc-validation.yml` gate wiring + `subsystem_docs.bats` (+ wrapper) + `STRUCTURE.md`/`SETUP.md` convention.

## Deviations from plan

- **Step 2 resolved → gitignored `CLAUDE.md` shims (new mechanism).** The plan posed a binary (auto-load vs pointer-only). claude-code-guide confirmed (via the Claude Code memory docs) that Claude Code lazy-loads nested `CLAUDE.md` but **not** nested `AGENTS.md`. User chose the third path: `setup-harness.sh`/`.ps1` generate a gitignored one-line `CLAUDE.md` (`@AGENTS.md`) beside each leaf — full auto-load, committed tree stays `AGENTS.md`-only. Added two files not in the plan: `setup-harness.{sh,ps1}` edits + `.gitignore` pattern.
- **Gate wired into `doc-validation.yml`, not `scripts/dev/test-all.sh`.** `test-all.sh` already auto-discovers `test-*.sh`, so no edit was needed there (the plan's "edit test-all.sh" row was a no-op). But the cheap doc CI lane (`doc-validation.yml`) invokes individual gates, **not** `test-all.sh` — so the gate was added there too (path triggers + a run step) to actually gate doc PRs. `Source/Core/src/**` *code* paths deliberately left out of the trigger (staleness is WARN-only; runs at pre-push).
- **Added a bats wrapper** `test-subsystem-docs-bats.sh` (the plan named the `.bats` file but not the `test-all.sh`-discoverable wrapper that runs it).
- **Scope held to the full plan in one PR** (5 leaves + Tracker exemplar + scaffold + gate) — the interdependence (code-review extraction needs all 5 leaves; gate needs the registry + leaves) made horizontal slicing produce broken intermediate states.

## Verification (actual)

- `test-subsystem-docs.sh` — structural / `--selftest` / `--diff origin/develop` all exit 0 on the real tree; negative duplication path confirmed.
- `tests/bats/subsystem_docs.bats` — **7/7 pass** (clean, selftest, missing-leaf FAIL, unregistered-leaf FAIL, central↔leaf duplication FAIL, README staleness WARN non-blocking, data-driven second-README discovery).
- Shim-gen — generates 5 gitignored `CLAUDE.md` shims; `git check-ignore` + `git status` confirm they're ignored.
- All cited symbols verified live (anti-rot): `ITrackerBackend` + 5 roles, `TrackerHttpClient`, `FieldEditAuditSource`, `CommandContext`, `CommandRegistry`, etc. — no `ITrackerClient` reintroduced.
- Relative-link integrity: leaf→root, leaf→docs, code-review→CONTEXT-MAP, CONTEXT→docs/CONTEXT all resolve.
- Pure-docs + shell + one CI-yaml; no C++ compiled → dual-target unaffected.
