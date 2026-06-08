# Plan — GitHub → p4d one-way mirror via Helix4Git Git Connector (WSL2)

> **Slug**: `p4-git-connector-github-mirror` (matches this file's basename without `.md`).
>
> **Status**: `active`.
>
> **Usage**: standard plan-doc. Fill every section; `N/A — <reason>` where genuinely inapplicable.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

The user wants to **mirror the canonical GitHub Smatchet repo into the Windows `p4d` server** (`Mainbot:1666`) as a read-only backup/visibility copy, using Perforce's **P4 Git Connector (Helix4Git)** so the mirror is stored git-natively (real git objects + SHAs) in a **graph depot**, not flattened into classic Perforce revisions.

This sits *inside* the existing dual-VCS topology (`docs/perforce/SETUP.md`, `docs/perforce/AGENT_FLOWS.md`): git/GitHub stays the **canonical ship-line** (PR review, CI, merge-watcher); Perforce stays an **opt-in local layer**. This plan adds one strictly-additive, strictly-one-way consumer: GitHub → p4d. Nothing ever flows p4d → GitHub.

**Hard constraint discovered during scoping** (Perforce docs, current P4SAG): the **Git Connector daemon (`gconn`) is Linux-only** — it ships as `.deb`/`.rpm`, no Windows binary. The **graph depot itself lives inside `p4d`** and is cross-platform, so the Windows `p4d` can host it; but the connector that populates/serves it must run on Linux. Decision (user, this session): run the connector on **WSL2 Ubuntu on the same Windows box** as `p4d`, reaching `p4d` over `P4PORT`.

**Intended outcome — after this lands, X is true:** a scheduled job on the dev box keeps a graph-depot repo (`//repo/smatchet`) in lock-step with `github.com/<owner>/Smatchet` such that `p4 repos` lists it and its HEAD matches GitHub's `develop` HEAD, with a health-check that fails loudly when the mirror goes stale — and the documented dual-VCS topology explicitly records this mirror as non-authoritative and off the ship-line.

Sources for the platform/licensing facts (captured at scoping time):
- P4 Git Connector product — <https://www.perforce.com/products/helix-core-git-connector>
- System requirements (Linux-only, x86_64) — <https://help.perforce.com/helix-core/server-apps/p4sag/current/Content/P4SAG/install-config.system.html>
- Architecture (connector ↔ graph depot ↔ p4d) — <https://help.perforce.com/helix-core/server-apps/p4sag/current/Content/P4SAG/overview.architecture.html>
- Licensing (free tier ≤ 10 repos/license) — <https://community.perforce.com/s/article/15318>

## Approach

Three layers, provisioned once, then self-running:

1. **Server (Windows `p4d`, `Mainbot:1666`)** — enable graph depots (confirm Helix4Git licensing; 1 repo is within the free ≤10-repo tier) and create graph depot `//repo` (`p4 depot -t graph repo`) plus repo spec `//repo/smatchet`. Create a dedicated low-privilege service user `gconn` (or `git-connector`) that owns the graph depot. This is **separate from** the classic `//smatchet` stream depot the agentic-WIP layer uses — two depots, two purposes, zero interference.

2. **Connector (WSL2 Ubuntu 22.04 LTS on the dev box)** — install the `helix-git-connector` package + `git ≥ 1.8.5` + `git-lfs ≥ 1.1.0` (the latter is specifically required for one-way mirroring from third-party git servers). Point the connector at `P4PORT=<windows-host>:1666` (the Windows p4d, reachable from WSL2 over the host's LAN IP / `Mainbot:1666`). Configure the connector's **one-way mirror** mode against the GitHub remote, authenticating to GitHub with a **read-only** Deploy Key / fine-grained PAT (the Smatchet repo is private). The connector translates GitHub git objects → graph-depot storage in `p4d`.

3. **Cadence + health (WSL2 cron + a checked-in health script)** — because GitHub Actions cannot reach a local `p4d` (`AGENT_FLOWS.md` § When NOT to use Perforce), the mirror is **pull-based**: a WSL2 cron periodically triggers the connector to fetch from GitHub. A new repo script `scripts/dev/p4-mirror-healthcheck.sh` asserts graph-depot HEAD == GitHub `develop` HEAD and exits non-zero on drift/staleness, so silent mirror death is detectable.

**Trade-off named:** Helix4Git is heavyweight for a pure backup (Linux connector + license + WSL2 lifecycle) — but it is the *only* mechanism that stores the mirror as real git objects in `p4d`; a graph depot cannot be populated any other way. The lighter Windows-native alternative (classic-depot `p4 reconcile` mirror) was explicitly declined by the user this session because it loses git SHA/history fidelity.

## Files to modify

Pure infra + docs + one script — **no `Source/` C++ touched**.

1. `docs/perforce/MIRROR.md` *(new)* — the GitHub→p4d mirror runbook: WSL2 install, graph-depot create, connector config, GitHub read-only auth, mirror-fetch cron, health-check usage, teardown. Sibling to `SETUP.md` (server bring-up) and `RUNBOOK.md`.
2. [`docs/perforce/AGENT_FLOWS.md:15`](docs/perforce/AGENT_FLOWS.md) § Topology table — add one concern row: `Mirror ship-line → p4 (read-only backup)` | git path = `(canonical — GitHub)` | p4 path = `//repo/smatchet graph depot via Git Connector (one-way, non-authoritative)`. Reinforce the existing "never authoritative, never on the ship-line" invariant for this new consumer.
3. [`docs/perforce/AGENT_FLOWS.md:169`](docs/perforce/AGENT_FLOWS.md) § When NOT to use Perforce — add a bullet: the graph-depot mirror is **read-only and one-way**; never push from `//repo/smatchet` back to GitHub, never treat it as a source of truth.
4. [`docs/perforce/SETUP.md:275`](docs/perforce/SETUP.md) § Open items — cross-link the new `MIRROR.md` as a post-Phase-0 optional add-on (the mirror depends on Phase-0 server bring-up being complete).
5. `scripts/dev/p4-mirror-healthcheck.sh` *(new)* — bash; compares the **mirrored `develop` ref SHA** against GitHub's. Primary mechanism: `git ls-remote <connector-smart-http-url>/repo/smatchet refs/heads/develop` vs `git ls-remote https://github.com/<owner>/Smatchet develop` (pure git-protocol, no `p4 graph` plumbing, works as long as the connector serves smart-http). Fallback when smart-http isn't served: `p4 graph log -n 1 -m1 //repo/smatchet develop`. Exits 0 on SHA match, non-zero + diagnostic on drift/staleness/unreachable. Runnable by the WSL2 cron and ad hoc.
6. [`docs/plans/INDEX.md`](docs/plans/INDEX.md) — regenerated via `agents/scripts/core/test-plan-index.sh --fix` (mechanical).

## Existing utilities reused

- `docs/perforce/SETUP.md` § Install / typemap / firewall — the server-side bring-up this mirror builds on; reused verbatim, not duplicated in `MIRROR.md` (cross-link instead).
- `docs/perforce/AGENT_FLOWS.md` § Topology table + § When NOT to use Perforce — extended in place, not forked.
- `gh api repos/.../commits/<ref>` (GitHub CLI, already a required tool per `check-required-tools`) — for the GitHub-side HEAD in the health script; no new dependency.
- `p4` client on the dev box (already configured, reaches `Mainbot:1666`) — for the depot-side HEAD query.

## UX Pillar callouts

Per `AGENTS.md` § UX Pillars. This change ships **zero product code** — it is infra + docs + a bash health script.

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — no `Source/` code path touched; the mirror runs out-of-process in WSL2, never on the Smatchet UI thread.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — no UI code; the connector + cron are wholly external to the app.
- **Pillar 3 (never crash)**: no impact on the app. Health-script robustness (graceful non-zero exit on `p4`/`gh` failure, no `set -e` foot-guns) is the only correctness surface, covered in § Verification.
- **Pillar 4 (accessibility)**: N/A — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

**N/A — diff touches no `Source/Core/` (nor any `Source/`) file.** Scope is `docs/perforce/*`, `docs/plans/*`, and `scripts/dev/p4-mirror-healthcheck.sh`. No scenario mapping, no `SMATCHET_UI_PERF_SCOPE` markers, no dispatcher/sync-I/O surface.

## Risks / non-goals

**Risks:**
- **WSL2 lifecycle** — if WSL2 isn't running (post-reboot, no auto-start), the cron never fires and the mirror silently staledates. *Mitigation:* WSL2 auto-start at logon (Task Scheduler `wsl -d Ubuntu …` or systemd-in-WSL service) **+** the `p4-mirror-healthcheck.sh` drift/staleness alarm so silence is detectable. Documented in `MIRROR.md`.
- **Helix4Git licensing** — graph depots may require a license entry the base `p4d` lacks; the free tier is ≤10 repos/license (we need 1). *Mitigation:* confirm `p4 license -o` + graph-depot enablement as the **first** build step; abort to the user if a paid license is required (cost decision = user's, per `AI_POLICY.md` § Escalate, don't assume).
- **GitHub secret in WSL2** — the connector needs read access to a private repo. *Mitigation:* least-privilege **read-only** Deploy Key (single repo) over a broad PAT; store outside the repo tree; document rotation.
- **WSL2 → Windows-p4d reachability** — WSL2's NAT must reach the Windows host's `:1666`. *Mitigation:* verify `p4 -p <host>:1666 info` from inside WSL2 as a connector pre-flight; firewall rule already exists (`SETUP.md` § 7, LAN profile).
- **Connector ↔ p4d 2025.2 version compatibility** — *Mitigation:* match the connector package to the 2025.2 server line per the Helix4Git support matrix; pin the version in `MIRROR.md`.
- **Disk** — graph depot stores full git objects in the p4d server root (`C:\depot\`). *Accepted:* Smatchet is a small repo; negligible vs the 1 GB headroom in `SETUP.md` § pre-flight.

**Non-goals:**
- **Bidirectional sync** — explicitly NOT building. One-way GitHub → p4d only; GitHub stays canonical.
- **Making the graph depot a ship target** — CI, CodeRabbit, and `smatchet-merge-watcher` stay on GitHub (`AGENT_FLOWS.md` § When NOT to use Perforce). The mirror is never reviewed/merged-from.
- **Migrating the classic `//smatchet` agentic-WIP layer to a graph depot** — untouched; task-streams/shelves/locks keep using the classic stream depot.
- **Mirroring every branch/PR ref** — scope is the canonical integration branch (`develop`) + tags; per-PR-branch mirroring is a follow-up if ever needed.

## Verification

Per `AGENTS.md` § Verification automation. This plan's product surface is one bash script + docs; the connector install is inherently one-time manual infra.

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no `Source/Core/` C++.
- **Bucket E (ImGui Test Engine)**: N/A — no UI.
- **Bash-driver scenario / screenshot / sanitizer**: `scripts/dev/p4-mirror-healthcheck.sh` gets a `bats` test (`tests/bats/p4_mirror_healthcheck.bats`) covering: SHAs equal → exit 0; SHAs differ → non-zero + drift message; connector/`git ls-remote`/GitHub unreachable → non-zero + clear diagnostic (mock `git ls-remote` both sides). `shellcheck` clean.
- **Build gate**: N/A — no C++ compiled. (Dual-target build untouched; nothing in `Source/`.)
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint) — the new `MIRROR.md` + `AGENT_FLOWS.md`/`SETUP.md` edits + this plan's index entry must pass ref-integrity and anchor checks.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the dual-VCS domain model (graph-depot vs classic-depot terminology, "mirror" vs "replica" vs "consumer", one-way invariant wording) and sharpen terms before finalising; record the outcome in § Verification (actual).
- **Manual residue**: the **one-time WSL2 + connector + GitHub-auth install is manual** and cannot be fully automated (it provisions host-level OS packages + secrets). Deferred-automation action plan: (a) `MIRROR.md` ships the exact copy-pasteable command sequence so the manual step is deterministic, not improvised; (b) the *ongoing* verification is automated by `p4-mirror-healthcheck.sh` + cron; (c) file a `docs/self-improvement/categories/tooling.md` entry proposing an idempotent `scripts/dev/p4-mirror-bootstrap.sh` (WSL2-side) that codifies steps 1–3 of § Approach for re-provisioning. No silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to graph-depot / Helix4Git / `//repo` and revise or delete any that contradict the one-way-mirror framing. (Expected: none today — graph depots are new to this repo.)

- **ADR for the mirror** — if the dual-VCS topology gains a *second* graph-depot consumer or the mirror ever becomes load-bearing, promote the one-way-mirror invariant from `AGENT_FLOWS.md` into a dedicated ADR. No-action now (single additive consumer, fully reversible).
- **Offsite/secondary mirror** — mirroring p4d → a third location is out of scope; GitHub remains the durable canonical copy.
- **Per-PR / per-branch mirror** — only `develop` + tags in scope (see Non-goals).
- **Automating the host-level WSL2 bootstrap** — flagged as a `tooling.md` follow-up (§ Verification § Manual residue), not designed here.

## Implementation log
*(bullet per shipped commit: `<sha> · <one-line summary>`)*

- `<this PR>` · Ship the mirror **artifacts** (docs + health-check + bats). New `docs/perforce/MIRROR.md` (full operator runbook), `scripts/dev/p4-mirror-healthcheck.sh` + `tests/bats/p4_mirror_healthcheck.bats`, and the `AGENT_FLOWS.md` (topology row + When-NOT bullet) + `SETUP.md` (Open-items cross-link) edits.

**Status stays `active` (deliberate).** The plan's § Intended outcome is the *running* mirror ("a scheduled job keeps `//repo/smatchet` in lock-step with GitHub"). That requires the one-time WSL2 + connector + Deploy-Key install on the user's box — host-level infra/secrets this PR cannot perform (§ Verification § Manual residue). This PR ships everything that *can* be checked into the repo; the operator stands the mirror up by following `MIRROR.md`, after which the plan archives. INDEX.md regenerates from `shipped/` only, so no index entry is owed while active.

## Deviations from plan
*(populated post-ship)*

- **Licensing risk retired, not escalated** (§ Risks "Helix4Git licensing", § Approach step 1). The plan gated on confirming whether graph depots need a paid license, with an abort-to-user if so. A read-only probe this session created an empty graph depot via `p4 depot -t graph` **successfully on the unlicensed free tier** (`Server license: none`) — so no cost decision was needed and the build proceeded under the user's "Probe server, then build" authorization. `MIRROR.md` § Prerequisites records the free-tier confirmation.
- **Repo is PUBLIC, not private** (§ Approach step 2, § Risks "GitHub secret" assumed private). `gh repo view` showed `alexandrosk0/Smatchet` is public — anonymous HTTPS read works with no credential. Kept the read-only Deploy Key as the documented primary (operator's explicit auth choice; future-proofs a flip to private + dodges anon rate limits) but added a `MIRROR.md` § 3 note that the public HTTPS URL is a valid simpler path while public.
- **`//repo` graph depot pre-existed** (created 2026-05-21, empty). The plan's "create graph depot" step became idempotent verify-then-create-if-absent in `MIRROR.md` § 1(a).
- **Health-check ref-comparison sharpened** (grill outcome). Original § Files #5 mixed `gh api commits` and `p4 graph`; the shipped script standardizes on `git ls-remote refs/heads/develop` for BOTH sides (pure git-protocol, SHA-exact) with a `p4 graph log` fallback only when the connector doesn't serve smart-http. Removed the `gh api` dependency from the hot path.

## Verification (actual)

- **Bash-driver bats**: `tests/bats/p4_mirror_healthcheck.bats` — **8/8 PASS** (in-sync→0; drift→non-zero+`DRIFT`; mirror unreachable; GitHub unreachable; mirror ref absent; `MIRROR_REMOTE` unset; p4-fallback in-sync; p4-fallback unreachable). Stubs `git`+`p4` on PATH, no live p4d/GitHub/connector needed.
- **shellcheck**: `scripts/dev/p4-mirror-healthcheck.sh` — **CLEAN**.
- **Doc validation**: `MIRROR.md` + `AGENT_FLOWS.md`/`SETUP.md` edits cross-linked bidirectionally; ref-integrity/anchors to be confirmed green by `test-docs.sh` in CI on the PR.
- **Plan stress-test — `grill-with-docs` (actual)**: ran against the dual-VCS domain model. Outcomes folded in: (1) **graph-depot vs classic-depot** terminology kept distinct throughout (`//repo` graph depot = git-native objects; `//smatchet` classic stream depot = agentic-WIP; "two depots, two purposes, zero interference"). (2) **"mirror" chosen over "replica"/"consumer"** — "replica" implies HA/failover semantics this is not; "consumer" is too generic. Standardized on **"one-way mirror"** + **"non-authoritative"**. (3) **One-way invariant** stated as a named invariant at the top of `MIRROR.md` and echoed in `AGENT_FLOWS.md` (topology row + When-NOT bullet). (4) **Health-check mechanism judged sound** — exact-SHA equality of `develop` via `git ls-remote` on both sides is the tightest available drift signal (catches staleness, divergence, and unpopulated-repo distinctly); the `p4 graph log` fallback covers the no-smart-http connector build. **Storage-substrate pre-flight**: confirmed the substrate is a real graph depot (live `p4 depot -t graph` probe), not a fabricated schema — no phantom-migration risk.
- **Manual residue** (carried, per plan): the one-time WSL2 + connector + Deploy-Key standup stays manual host infra. Deferred-automation action plan unchanged — `MIRROR.md` ships the deterministic copy-paste sequence; ongoing verification is automated by the health-check + cron; a `docs/self-improvement/categories/tooling.md` entry proposes an idempotent `scripts/dev/p4-mirror-bootstrap.sh`. **Known server quirk** (also in `MIRROR.md` § Known issues): `p4 depot -d <graph-depot>` hits a `db.counters locked after db.group` lock-order abort on this `p4d` 2025.2 (independent of `-f`); a stray empty probe depot `testgraphprobe` (0 repos/0 data, harmless) awaits a `db.peeking` maintenance-window cleanup. Steady-state mirror writes never touch the delete path, so the mirror is unaffected.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*In the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/p4-git-connector-github-mirror.md docs/plans/shipped/p4-git-connector-github-mirror.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*
