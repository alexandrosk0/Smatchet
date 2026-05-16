# Git → Perforce migration

Status: **draft** (locked decisions, unimplemented).
Owner: orchestrator + future `p4-janitor` (replacement for `git-janitor`).
Originating prompt: "Make a plan to transition the project from git to perforce" (2026-05-15).

## Goal

Move Smatchet's source control from Git/GitHub to a self-hosted Perforce Helix Core depot. Preserve full commit history. Vendor every third-party dependency that today comes through `FetchContent`. Rewrite every workflow, agent, and script that touches `git` / `gh` so the project is fully Perforce-native after cutover.

## Decisions locked

Confirmed up-front in the originating prompt; do not relitigate without the user.

1. **Server**: Helix Core, on-prem self-hosted. Single `p4d` instance on a Windows host (developer's box for bring-up; promote to a dedicated server before team scale-out).
2. **History**: full import via `git p4` so every Git commit becomes a Perforce changelist with original author + timestamp + message preserved. No squash, no snapshot baseline.
3. **GitHub**: archive and abandon after cutover. Repository set read-only; no mirror, no Git-Fusion, no bidirectional sync.
4. **Third-party deps**: vendor every `FetchContent` source tree into `//smatchet/main/third_party/<dep>/` at the exact pin currently in `CMakeLists.txt`. No more configure-time Git clones.

## Out of scope (explicit)

- **Helix Swarm** code review tooling — pure-CL workflow at first; revisit Swarm in a follow-up if review friction warrants it.
- **Stream depot** layout — start with classic depot + branch specs; migrate to streams only if branching pain emerges.
- **GitHub Actions / CI replacement** — Smatchet has no live CI today (verified 2026-05-15: `.github/` does not exist). No work to migrate.
- **Public open-source posture** — archiving GitHub means losing the public read URL. If the user wants to keep an external read mirror later, that is a follow-up plan, not part of this one.
- **Smatchet's in-app `P4Blame` integration** (`Source_Core/src/P4Blame.cpp`) — already Perforce-aware, unaffected by where Smatchet's own source lives. Out of scope.

## Inventory of Git touchpoints (verified 2026-05-15)

### `FetchContent` declarations to vendor

All in `CMakeLists.txt` and `tests/CMakeLists.txt`:

| Dep | Pin | Source line |
|---|---|---|
| `nlohmann/json` | `v3.11.3` | `CMakeLists.txt:225` |
| `libcpr/cpr` | `1.9.2` | `CMakeLists.txt:230` |
| `SRombauts/SQLiteCpp` | `3.3.1` | `CMakeLists.txt:236` |
| `yhirose/cpp-httplib` | `v0.14.1` | `CMakeLists.txt:255` |
| `mity/md4c` | `release-0.5.2` | `CMakeLists.txt:263-265` |
| `gulrak/filesystem` | `v1.5.14` | `CMakeLists.txt:282-284` |
| `glfw/glfw` | `3.3.8` | `CMakeLists.txt:314` |
| `ThePhD/sol2` | `v2.20.6` | `CMakeLists.txt:382` |
| `ocornut/imgui` (docking) | `329c5a6b3be75ebf54506d3ae94b836ffcf19fa0` | `CMakeLists.txt:470-474` |
| `doctest/doctest` | `v2.4.11` | `tests/CMakeLists.txt:4-6` |

Lua tarball is already downloaded (not via Git) — verify in `CMakeLists.txt` and treat the same as the others (vendor the tarball into `//smatchet/main/third_party/lua/`).

### Files that invoke `git` / `gh` CLI

Confirmed by grep on 2026-05-15:

- `agents/git-janitor.md` — full rewrite into `agents/p4-janitor.md`.
- `agents/code-review.md` — reads "branch diff" via `git diff`.
- `agents/security-review.md` — same, plus `gitleaks` invocation against history.
- `agents/build-doctor.md` — references `git status` / `git log` for staleness diagnosis.
- `docs/harness/cursor/setup.md`, `docs/harness/claude-code/setup.md` — reference `git clone`.
- `docs/backlog/AGENT_SELF_IMPROVEMENT.md` — historical entries reference commit SHAs (informational, not mutated).
- `docs/design/imgui-test-engine-bucket-e-execution.md` — references `git` in plan-doc body (informational, leave for archaeology).
- `scripts/publish/release_github.ps1` — **delete** post-cutover (publishes GitHub release; no longer applicable).

### Repo-level Git artefacts

- `.gitignore` — translate to `.p4ignore` (subset works; `**` patterns are not all supported and need expansion).
- `.git/` — preserve for archaeology in a sibling folder (`C:\Dev\Smatchet.git-archive\`) post-cutover.
- `.github/` — does not exist (no CI to migrate).

## Phases

Implementation breaks into eight ordered phases. Each phase ships independently, each is reversible up to the cutover (Phase 6).

### Phase 0 — Perforce server bring-up

Goal: a working `p4d` on the developer host with an empty depot ready to receive commits.

1. Install Helix Core server (`p4d`) on the Windows host. Pin to a current LTS (e.g. `r24.2`).
2. Install P4V (GUI) and `p4` CLI on the developer machine. Verify `p4 info` connects to the server with `P4PORT=ssl:localhost:1666`.
3. Create the depot: `p4 depot smatchet`, type `local`. Defer streams until a real branching need exists.
4. Create one Perforce user matching the current Git author (`Alexandros Konstantonis <alexkonstantonis@gmail.com>`). Document the spec for future devs in `docs/perforce/USERS.md` (new file).
5. Create a workspace (client spec) `<user>_smatchet_dev` rooted at `C:\Dev\Smatchet_p4\`. Sparse view: `//smatchet/main/... //<client>/main/...`.
6. Configure `p4 typemap` for: `binary+w` on `*.png` / `*.jpg` / `*.dll` / `*.exe` / `*.lib`; `text+x` on `*.sh` / `*.py`; `unicode` for `.json` / `.md` (UTF-8); `text+w` for `.cpp` / `.h` / `.cmake`.
7. Configure server `unicode=1` and `case-sensitive=2` (Windows-host insensitivity, server-side preserve case).
8. Set up daily checkpoint + journal rotation via Windows Scheduled Task (`p4 admin checkpoint`).

Verification: `p4 info` succeeds; empty depot exists; one CL can be submitted on a throwaway file from the workspace.

### Phase 1 — Vendor third-party deps (still on Git)

Goal: eliminate every `FetchContent` Git fetch while still on Git, so Phase 5 import lands a self-contained tree.

For each dep in the inventory table:

1. Branch off `develop` as `chore/vendor-<dep>`.
2. `git clone <url> --branch <pin> --depth 1 third_party/<dep>/`. Strip `.git/`, tests-not-needed, docs-not-needed (case-by-case to keep depot lean).
3. Replace the `FetchContent_Declare` + `FetchContent_MakeAvailable` block in `CMakeLists.txt` with `add_subdirectory(third_party/<dep>)` (or a hand-written interface target for header-only libs like `nlohmann/json`, `ghc::filesystem`, `cpp-httplib`, `sol2`, `doctest`).
4. Verify `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` (per AGENTS.md dual-target rule).
5. Verify `cmake --build --preset ninja-test-msys2` for the doctest dep change.
6. Open one PR per dep, merge before the next. Order: smallest header-only first (`nlohmann/json`, `ghc_filesystem`, `cpp-httplib`, `sol2`, `doctest`), then small compiled (`md4c`, `SQLiteCpp`), then large compiled (`glfw`, `cpr`), then `imgui` (docking branch, custom backend wiring) last.
7. **Special cases**:
   - `cpr` pulls `curl` transitively. Either vendor `curl` too (recursive) or accept `cpr`'s built-in `mongoose`-style fallback. Decide during the `cpr` PR.
   - `imgui` is on the docking branch at a specific SHA — vendor the full tree, do not try to track upstream.
   - `glfw` brings its own CMake; ensure `GLFW_BUILD_DOCS=OFF`, `GLFW_BUILD_EXAMPLES=OFF`, `GLFW_BUILD_TESTS=OFF` are set before `add_subdirectory`.

Phase exit criterion: `grep -rE "FetchContent_Declare|GIT_REPOSITORY|GIT_TAG" CMakeLists.txt tests/CMakeLists.txt` returns nothing. Both presets build clean.

### Phase 2 — `.p4ignore` + line-ending policy

Goal: a `.p4ignore` ready to drop in alongside the depot import that excludes everything `.gitignore` excludes today.

1. Translate `.gitignore` (verified 2026-05-15, 92 lines) to `.p4ignore`. Notable translations:
   - `**/CMakeFiles/` → `CMakeFiles/` (Perforce ignores recurse by default from any depth).
   - `!.vscode/settings.json` (negation) — Perforce supports `!pattern` since 2017.1; keep as-is.
   - Per-pattern wildcards `Smatchet-debug-*.log` work unchanged.
2. Add `P4IGNORE=.p4ignore` to the workspace spec template documented in `docs/perforce/SETUP.md` (new).
3. Set `p4 set P4CHARSET=utf8` globally; verify `p4 typemap` covers Smatchet's mixed text/binary file set.

### Phase 3 — Author map + `git p4` dry-run

Goal: prove the import end-to-end on a throwaway depot before pointing at the real one.

1. Build the author map at `migration/git-authors.txt`:
   ```
   alexkonstantonis@gmail.com = alexk <alexkonstantonis@gmail.com>
   ```
   (One line today; add rows for any historical co-authors if `git shortlog -se develop` shows more.)
2. Stand up a *second* throwaway `p4d` (port `1667`) and run the import end-to-end against it:
   ```
   git p4 clone --bare --use-client-spec=false \
       --destination=C:\Dev\Smatchet.gitp4-test \
       //smatchet-test/main C:\Dev\Smatchet
   ```
   (Actual command path is **Git → P4**, so use `git p4 submit` after `git p4 sync`. Document the exact recipe under `docs/perforce/IMPORT.md` once verified.)
3. Validate: every Git commit on `develop` produced exactly one Perforce CL; CL author + timestamp + message match.
4. Spot-check three commits at random by `p4 print -q //smatchet-test/main/CMakeLists.txt#<rev>` against `git show <sha>:CMakeLists.txt`.
5. Discard the throwaway depot. Phase exits when the dry-run is clean.

### Phase 4 — Agent / script rewrite (still on Git)

Goal: rewrite every Git-touching agent + script to its Perforce equivalent, kept side-by-side with the Git version until Phase 6.

| Git artefact | Perforce replacement | Notes |
|---|---|---|
| `agents/git-janitor.md` | new `agents/p4-janitor.md` | Owns: submit shelved CLs, `p4 reconcile`, `p4 verify`, branch-spec hygiene. |
| `agents/code-review.md` § "branch diff" | `p4 diff -du //...@=<CL>` against pending shelve | Reviewer reads shelved CL, runs cppcheck/clang-tidy on the local pre-submit copy. |
| `agents/security-review.md` § `gitleaks` | swap to `trufflehog filesystem` against the workspace | `gitleaks` is Git-only; `trufflehog` runs on a checked-out tree. |
| `agents/build-doctor.md` git-state probes | `p4 opened`, `p4 changes -m 5`, `p4 fstat` | Same diagnostic intent, p4-shaped commands. |
| `scripts/publish/release_github.ps1` | **delete** | No GitHub releases post-cutover. Replace with `scripts/publish/release_local.ps1` that just stamps a CL number into the publish-build artefact. |
| `docs/harness/{cursor,claude-code}/setup.md` `git clone` | `p4 sync //smatchet/main/...` | Document workspace spec + P4PORT setup. |
| AGENTS.md § "Plan-doc safety" (`git add` + commit) | `p4 add` + `p4 submit -d 'wip(plan): <slug>'` | Same intent, p4 verbs. |
| AGENTS.md § "Plan revision after implementation" SHA references | CL number references | Same shape, different identifier. |

Each rewrite ships as its own PR on Git (we are still on Git in Phase 4). After cutover, future edits go through Perforce.

### Phase 5 — Real import

Goal: cutover-eligible Perforce depot containing every Git commit ever made.

1. Announce a 2-hour Git freeze on `develop` (no merges). Confirm with the user before starting — the freeze is the first irreversible step in the migration.
2. Final `git pull --rebase` on `develop`; tag the tip as `git-final-2026-05-15`.
3. Run the validated `git p4` recipe from Phase 3 against the real `//smatchet/main/`.
4. Submit the `.p4ignore` produced in Phase 2 as the first post-import CL.
5. Verify file count and total byte count match `git ls-tree -r develop` between Git tip and `//smatchet/main/...@head`.
6. `p4 verify -q //smatchet/main/...` reports zero `BAD!` revisions.
7. Build from a *fresh* `p4 sync` of the depot using both presets. Both must pass clean.

### Phase 6 — Cutover (irreversible)

1. Make the GitHub repo read-only via repo settings → "Archive this repository".
2. Push the `git-final-2026-05-15` tag to GitHub one last time so archaeology is anchored.
3. Move the local Git working copy aside: `mv C:\Dev\Smatchet C:\Dev\Smatchet.git-archive` (preserve `.git/` for offline lookups).
4. Promote the Perforce sync to the canonical path: `mv C:\Dev\Smatchet_p4 C:\Dev\Smatchet`.
5. Replace `agents/git-janitor.md` → `agents/p4-janitor.md` (Phase 4 produced it; this is the swap).
6. Submit a single CL `chore: cutover from git to perforce` containing: removed `.gitignore`, removed `scripts/publish/release_github.ps1`, replaced agent files, updated AGENTS.md cross-references.
7. Update `README.md` to point new contributors at `docs/perforce/SETUP.md`.

### Phase 7 — Post-cutover hardening

1. Update `scripts/setup-harness.sh` so harness adapters do not rely on `.git/` for repo-root detection — switch to a marker file (`AGENTS.md` already at repo root works).
2. Update `scripts/clear-session-context.sh` if it touches `.git/` (verify; likely does not).
3. Sweep `docs/` for any remaining `git`/`gh`/`PR #` references using `rg` and either rewrite or mark explicitly as "pre-cutover archaeology".
4. Schedule a weekly `p4 verify -q //smatchet/...` cron via Windows Task Scheduler.
5. Add a `docs/perforce/RUNBOOK.md` covering: backup/restore, common commands, `p4 unshelve` for cross-machine WIP.
6. Open a 30-day review item: revisit Helix Swarm and stream depot decisions with real-usage data.

## Risks

| Risk | Mitigation |
|---|---|
| `git p4 submit` mangles author/timestamp on a few historical merge commits | Phase 3 dry-run catches this on a throwaway depot; budget time to massage the author map. |
| Vendoring a dep at the pinned SHA breaks because of build-time platform detection (`cpr` + `curl` is the highest risk) | Each Phase 1 PR ships independently with the dual-target build verified. Roll back the single dep PR if it breaks. |
| `.p4ignore` semantic gaps vs `.gitignore` (negations, `**`) cause accidental check-ins | Phase 2 explicitly tests with `p4 reconcile -n` against a known-clean tree before cutover. |
| User loses GitHub-side workflow (PR review UI, issue tracker, code search) and finds Perforce equivalents painful | Out of scope for this plan; revisit via a Helix Swarm follow-up if it bites within 30 days. |
| Single `p4d` on a developer box is the source-control SPOF | Phase 0 includes daily checkpoints. Promote to dedicated server box as soon as a second contributor joins. |
| `imgui` docking branch SHA pin loses the upstream cherry-pick trail | Vendoring captures the tree; if a future ImGui upgrade is needed, redo the vendor exercise from a fresh upstream sync. Same trade-off as today's `FetchContent_Declare` lock. |
| Agent telemetry (`scripts/agent-tokens-report.py` etc.) keys on Git SHAs in the JSONL rows | Phase 4 audit; swap SHA columns to CL numbers, or accept "SHA = N/A post-cutover" as a column. |

## Open questions (resolve during implementation, not now)

- Is there value in keeping a one-way Git mirror via cron `git p4 sync --import-labels` for tooling that only speaks Git (vexp daemon? — verify)? Decision: defer; revisit if vexp re-indexing breaks.
- `vexp` index lives at `.vexp/` (already in `.gitignore`). Does it work the same against a Perforce workspace? Verify in Phase 0 against the throwaway depot before relying on it post-cutover.
- Per-PR review today happens via `gh pr view` + Claude `code-review` agent. Replacement: shelved CL + reviewer pulls via `p4 unshelve`. Worth automating with a `scripts/dev/review-cl.sh` helper? Decide in Phase 4.
- `caveman` mode and the harness adapter layer are oblivious to source-control choice. Confirm in Phase 7 sweep.

## Verification

To be filled in during implementation per AGENTS.md § Plan revision after implementation. Anchor scenarios planned today:

- **Phase 0 sanity**: `p4 info` succeeds; one trivial CL submits and re-syncs.
- **Phase 1 per-dep**: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` passes; `bash scripts/dev/test-all.sh` passes.
- **Phase 3 dry-run**: spot-check three random Git commits → CL contents byte-identical via `p4 print` vs `git show`.
- **Phase 5 real import**: `p4 verify -q //smatchet/main/...` zero errors; full build from fresh sync passes both presets.
- **Phase 6 cutover**: `p4 changes -m 1` returns the cutover CL; `agents/p4-janitor.md` present; `agents/git-janitor.md` absent.
- **Phase 7 hardening**: `rg -l "\b(git|gh) (push|pull|pr|issue)\b" docs/ agents/ scripts/` returns only files explicitly marked as archaeology.

`test-author` should audit this section before Phase 5 lands and convert any "user opens X and observes Y" residue into deterministic `scripts/dev/test-perforce-*.sh` checks.

## Implementation log

(empty — populate per AGENTS.md § Plan revision after implementation as each phase ships)

## Deviations from plan

(empty — populate as decisions diverge from this draft during implementation)
