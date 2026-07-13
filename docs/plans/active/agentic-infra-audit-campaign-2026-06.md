# Plan — Agentic-infrastructure audit campaign (2026-06 salvage)

> **Slug**: `agentic-infra-audit-campaign-2026-06` (matches this file's basename without `.md`).
>
> **Status**: `active` — audit-campaign tracker. No fix-slice has been executed as such; **9 findings have since been closed incidentally by unrelated PRs, 1 is partially addressed, 57 remain open** (2026-07-13 reconciliation pass — see § Reconciliation pass (2026-07-13)). Flip to `shipped` only once every slice has merged (or been re-deferred) and § Implementation log is filled.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template. This is a meta-campaign (like `agentic-harness-campaign.md`): it ranks + sequences fix-slices, it does not re-author them.

## Context

A 2026-06-10/11 "improve all the agentic infrastructure of Smatchet" audit ran as a multi-agent `Workflow` fleet (~4.1M tokens) and **died on a session token limit before any agent wrote durable findings**; three salvage waves each died of a different cause (TPM starvation, 200K-context overflow, out-of-workspace permission deny), and the runtime deleted the run dir twice. What survived was a scriptable distillation of the dead agents' exploration transcripts — one slim evidence file per audit charter — preserved out-of-repo at `C:\Dev\salvage-2026-06-11\`. The failure modes were written into `docs/agent-rules/workflow-fleets.md` (PR #1141); two fixes already shipped (PR #1139, #1141).

This campaign is the **mining + dedup + verify + synthesize tail** that was never run. A fresh fleet (11 sonnet miners → 11 adversarial verifiers, all read-only against this worktree at HEAD) re-extracted the evidence, deduped each finding against the live self-improvement backlog, and refuted aggressively (the evidence is dated 2026-06-09/11 and much of it went stale within days). After this lands, the dead fleet's value is recovered as a ranked, verified, dependency-ordered fix backlog — at roughly ⅓ the original cost — and the evidence dir can be retired.

**Intended outcome (one sentence):** every confirmed agentic-infrastructure defect the dead fleet found is captured here as a PR-sized slice, ranked P0→P3 and de-noised of everything that was already fixed, already tracked, or intentional-by-design.

**Originating provenance:** dead fleet `agentic-infra-fresh-audit` (workflow `wf_9255a820`, 2026-06-09/11); salvage handoff `C:\Dev\salvage-2026-06-11\SALVAGE-INDEX.md`; per-charter miner output `C:\Dev\salvage-2026-06-11\results\*.md`; per-charter verifier verdicts `C:\Dev\salvage-2026-06-11\results\verify\*.md`.

## Exec summary — the systemic insight

**The dominant defect class is self-description drift: declarations diverge from implementation because almost nothing fails CI when they do.** Of 67 confirmed findings, the largest cluster is "a doc / prompt / docstring / baseline / config asserts X, but the code or live config does Y." Examples that survived adversarial refutation at HEAD:

- `AGENTS.md:48` says a PR merges when its **5** required checks pass — there are **7** at HEAD (`rule-docs-drift-01`, **P0**).
- `coderabbit-triage.md:170` declares a CI sync-check that **has never been implemented** (`core-agent-prompts-03`, P1); its Python sibling has no `--selftest` to catch the drift (`core-scripts-python-03`, P1).
- `merge-watcher.py:12-15` docstring still claims "Phase-1 scope: poll + observe only, no auto-merge" while the daemon auto-merges, triages, and notifies at HEAD (`merge-pipeline-05`, P1).
- `AGENTS.md:124` claims "no `.codex/` mirror" though `setup_codex()` generates one (`HP-03`, P2); three baselines/counts (`agent-size-baseline`, `STRUCTURE.md`, `portable-purity-baseline`) are stale by 30–60 lines (`rule-docs-drift-06/07`, `HP-08`).

The single highest-leverage systemic fix is **cheap drift-guards** — md↔py `--selftest`, template↔deployed parity checks, baseline-regen gates, a `Status: applied` migration lint — so the next audit is unnecessary. The self-improvement backlog machinery that is *supposed* to catch this is itself drifting: `applied.md` is excluded from all link/anchor validation (`self-improvement-loop-06`), ~59 `Status: applied` entries were never migrated out of live category files (`self-improvement-loop-07`), and category entries cite stale `scripts/dev/` paths a migration already moved (`self-improvement-loop-04`).

Three secondary themes, each a cheap themed slice:

1. **`python3` on Windows is a latent break** — the WinStore alias passes `command -v python3` but exits 49 on execution; bare-`python3` probes break `markdown_links.bats` (5/7 tests, **P0** `bats-coverage-01`), `agent_eval_*.bats` (P1), and `setup_pi()` (P3).
2. **Incomplete shell strict-mode** — ~20 scripts use `set -uo pipefail` or `set -u` without `-e`, silently swallowing command failures including in production gate logic (`merge-gates.sh`).
3. **Merge-pipeline has genuine correctness gaps, not cosmetics** — the only confirmed runtime-risk cluster: an override-label race that false-greens a gate (**P0** `merge-pipeline-01`), human/admin merges that leave no audit-ledger entry (**P0** `merge-pipeline-02`), `cr-out-of-band` dismissing the CodeRabbit gate when CR *never ran* (P1 `merge-pipeline-04`), and a **reversed sanitizer-preset selector** that builds ASAN when a TSAN failure is detected (P1 `core-scripts-python-02`).

**Triage verdict:** all 67 confirmed findings are agent-infrastructure / harness maintainability. **None is a user-observable product defect** — no confirmed finding touches `Source/Core/` runtime behaviour. Per `AGENTS.md` § Issue triage, that means **zero GitHub Issues are warranted**; this campaign's slice plan is the canonical artifact, and individual fixes route to `docs/self-improvement/categories/*` as they ship. (The fleet's only near-product-facing surface, the merge-pipeline correctness cluster, is CI/automation infrastructure, not shipped app behaviour.)

**Numbers:** 67 confirmed (4 P0 · 18 P1 · 27 P2 · 18 P3), 6 already-tracked (appendix), ~30 refuted as stale / by-design / no-repro (validating the refutation design — e.g. all of charter `ci-workflows`' P0 and 4 of its 9 candidates fell, the entire `vexp-*` finding family collapsed because the code was deleted in commit `4b18918c` one day after the evidence was gathered).

## Reconciliation pass (2026-07-13)

A read-only re-verification of all 67 confirmed findings against `develop` @ `a166404` (four parallel agents, one per file-cluster; each cited line re-located by symbol since 2026-06 line numbers have drifted). **Tally: 9 FIXED · 1 PARTIAL · 57 OPEN.** No slice of this campaign was ever run — every closure below is a side-effect of an unrelated PR that happened to touch the same file. The campaign remains a live backlog.

**FIXED (9) — incidentally, not by a campaign slice:**

| ID | Sev | How it closed |
|---|---|---|
| rule-docs-drift-01 | P0 | `AGENTS.md` § Merge gates was rewritten to block-on-any-red (all-gates-blocking flip); the "5 required checks" claim no longer exists. |
| rule-docs-drift-04 | P1 | `subagent-eval.md:5` now links the real `../plans/active/subagent-eval-agentic-coverage.md` (target present). |
| core-agent-prompts-02 | P1 | `perf-instrument.md` cites the correct `Source/Core/include/Ui/UiPerfMonitor.h`. |
| project-prompts-skills-01 | P1 | `historical-code-review` is now in `SKILL_ONLY_HELPERS` (parity gate passes). |
| rule-docs-drift-06 | P2 | `agent-size-baseline.md` matches `--baseline-md` output (in sync). |
| core-agent-prompts-04 | P2 | `perf-gatekeeper.md` banner now reads `read-edit`. |
| self-improvement-loop-07 | P2 | The `Status: applied` backlog migration ran — ~310 entries in `applied.md`; live category files ~0 (one stray in `test.md`). |
| merge-pipeline-09 | P3 | `merge-gates.graphql` now paginates `contexts` + `merge-gates.sh` fails closed on `PAGINATION_OVERFLOW`. |
| HP-08 | P3 | The stale `hooks-equivalent.md` Codex line is gone from `portable-purity-baseline.txt`. |

**PARTIAL (1):** `merge-pipeline-04` (P1) — `cr-out-of-band` no longer downgrades on its own; it now requires a paired `cr-disposition` attestation (PR-3 mechanism, not the proposed "≥1 CR StatusContext SUCCESS" check). A CR-never-ran bypass is still possible *with* an attestation, so the original defect is reduced, not closed. Keep the slice (B6), re-scoped to the residual.

**Highest-value still-OPEN:**
- `core-scripts-python-02` (P1) — sanitizer auto-act still selects an **ASAN preset for a TSAN failure** (`merge-watcher.py:2659-2662`; neither branch is `ninja-tsan-linux`). Cheapest real correctness fix in the set.
- The three remaining P0s: `merge-pipeline-01` (override-label race — no labeled-event timestamping), `merge-pipeline-02` (admin-merge ledger gap — [ADR-0017](docs/adr/0017-merge-time-snapshot-ledger.md) confirms the admin path is still "remaining writer to wire"), `bats-coverage-01` (5 bare `python3` probes in `markdown_links.bats`).
- merge-watcher NONE-state (`core-scripts-python-01` seed + `-04` parse), CR-thread pagination (`core-scripts-python-05` — query now requests `hasNextPage` but the reader ignores it), `postmortem-owed` blocking mode (`merge-pipeline-03`), and the false `merge-watcher.py` Phase-1 docstring (`merge-pipeline-05`).

**Corrections to the 2026-06 synthesis (found stale on re-read):**
- `self-improvement-loop-04` (P2) — **overstated.** Only ~1 genuinely-stale migrated ref remains; `gen-sbom` / `osv-scan` / `perf-compare` / `pre-ship` / `worktree-prune` legitimately still live under `scripts/dev/`. Re-scope before working.
- `self-improvement-loop-05` (P2) — **mostly resolved.** The cited `process.md:73` / `debt.md:47,59` are tiered now; one flat path persists (`debt.md:34`).
- `project-prompts-skills-07` and `core-scripts-python-06` — arguably **by-design** (the `ls` glob is required; `md_lint.py` documents its single rule as an intentional starting point). Confirm intent before filing as defects.

Everything not named above remains **OPEN** exactly as tabulated below; the per-finding evidence is preserved in this pass's agent transcripts (session scratch). The slice plan (§ below) is unchanged except that A0-fold / C-fast items whose findings are now FIXED can be dropped from their slices.

## Confirmed findings — ranked P0 → P3

Severity | ID | Cat | Title | File:line at HEAD | One-line fix | Effort

### P0 (4)

| Sev | ID | Cat | Title | File:line | Fix | Eff |
|---|---|---|---|---|---|---|
| P0 | rule-docs-drift-01 | process | `AGENTS.md` says "5 required CI checks"; `project.config.json` lists 7 | `AGENTS.md:48` / `project.config.json:56-64` | Change "5" → "7"; 7th is "Coverage (windows-2022 + OpenCppCoverage)" (added 2026-06-14) | S |
| P0 | bats-coverage-01 | test | `markdown_links.bats` fails 5/7 on Windows — bare `python3` probe hits WinStore alias (exit 49) | `tests/bats/markdown_links.bats:49,67,84,97,113` | Replace `run python3 -c` with a `for c in python3 python py` exec-validating probe or `skip` guard (per `agent_size.bats:25-29`) | S |
| P0 | merge-pipeline-01 | process | Override label applied before in-flight gate run causes false-green self-dismiss | `.github/workflows/coverage-gate.yml:62-75`; `agents/scripts/core/merge-watcher.py:2176` | Timestamp the `labeled` event; refuse merge if the latest passing run predates label application | M |
| P0 | merge-pipeline-02 | process | Human/admin merges produce no merge-snapshot ledger entry (ledger-blind) | `agents/scripts/core/merge-watcher.py:2192` (`_append_merge_snapshot` only on watcher path) | Post-merge CI step / git-janitor hook that appends a snapshot for every merge commit absent from the ledger | M |

### P1 (18)

| Sev | ID | Cat | Title | File:line | Fix | Eff |
|---|---|---|---|---|---|---|
| P1 | merge-pipeline-04 | process | `cr-out-of-band` dismisses the CR gate when CodeRabbit never ran (rate-limited/absent) | `agents/scripts/core/merge-gates.sh:389-396`; `docs/agent-rules/merge-gates.md:46` | Require ≥1 CR StatusContext SUCCESS (or explicit "Review skipped") before `cr-out-of-band` downgrades BLOCK→WARN; never to PASS | M |
| P1 | core-scripts-python-02 | infra | Reversed TSAN check — TSAN status triggers `ninja-clang-asan` (ASAN preset) instead of `ninja-tsan-linux` | `agents/scripts/core/merge-watcher.py:1994-1997` | Swap branches: `ninja-tsan-linux` on "tsan"/"threadsanitizer", ASAN preset on else | S |
| P1 | core-agent-prompts-03 | process | `coderabbit-triage.md` declares a CI sync-check that does not exist | `agents/core/coderabbit-triage.md:170` | Implement a grep-based rules-version-token CI check, or remove the false assurance (pairs with `core-scripts-python-03`) | M |
| P1 | core-scripts-python-03 | process | `coderabbit-triage.py` has no `--selftest` drift-guard against its `.md` rejection table | `agents/scripts/core/coderabbit-triage.py:64-67` | Add `--selftest` that parses the markdown table and asserts a compiled regex exists per entry | M |
| P1 | merge-pipeline-03 | process | `postmortem-owed.sh` is advisory-only — never blocks SessionStart even with open gate escapes | `agents/scripts/core/postmortem-owed.sh:33` | Add `--blocking` mode (exit 1 beyond a grace window); promote to a SessionStart gate | S |
| P1 | merge-pipeline-05 | infra | `merge-watcher.py` docstring falsely claims Phase-1-only (no auto-merge) | `agents/scripts/core/merge-watcher.py:12-15` | Replace stub language with the accurate capability list (auto-merge, CR-triage, sanitizer auto-act, notify) | S |
| P1 | core-scripts-python-01 | infra | `merge-watcher.py` does not seed `MERGE_GATES_PRIOR_NONE_HEAD/NONE_STREAK` into subprocess env | `agents/scripts/core/merge-watcher.py:485-487` | Seed both vars in `poll_one` alongside the existing three (pairs with `-04`) | S |
| P1 | core-scripts-bash-02 | infra | `smatchet-notify.sh` uses `set -o pipefail` only — failures silently continue (executed directly) | `agents/scripts/core/smatchet-notify.sh:27` | Change to `set -euo pipefail` | S |
| P1 | ci-workflows-03 | infra | `perf-pr-fast.yml` required context never reports on merge-queue refs | `.github/workflows/perf-pr-fast.yml:34-41` | Add `merge_group:` to `on:` (existing `changes` job already fast-exits non-PR events) | S |
| P1 | ci-workflows-04 | infra | `sanitizer-nightly.yml` FetchContent cache path wrong — cold-fetch every nightly | `.github/workflows/sanitizer-nightly.yml:63-65` | `path: build/ninja-clang-asan/_deps` → `.fetchcontent-src`; fix cache-key prefix | S |
| P1 | core-agent-prompts-02 | infra | `perf-instrument.md` cites wrong include path for `SMATCHET_UI_PERF_SCOPE` | `agents/core/perf-instrument.md:34` | `Source/Core/include/UiPerfMonitor.h` → `Source/Core/include/Ui/UiPerfMonitor.h` | S |
| P1 | rule-docs-drift-04 | infra | `subagent-eval.md` cross-links a non-existent plan path | `docs/agent-rules/subagent-eval.md:5` | `../plans/…` → `../plans/active/subagent-eval-agentic-coverage.md` | S |
| P1 | rule-docs-drift-05 | process | `delegation.md` header references the lift PR as "#TBD" | `docs/agent-rules/delegation.md:3` | Replace `#TBD` with the real PR number / shipped-plan ref | S |
| P1 | project-prompts-skills-01 | tooling | `historical-code-review` skill missing from `SKILL_ONLY_HELPERS` — parity gate exits 1 | `agents/scripts/core/test-skill-vs-agent-parity.sh:34-46` | Add `"historical-code-review"` to the array | S |
| P1 | bats-coverage-02 | test | `agent_eval_run/score.bats` 2-way python probe doesn't exec-validate (WinStore alias exits 49) | `tests/bats/agent_eval_run.bats:26`; `agent_eval_score.bats:26` | Replace with 3-way exec-validating loop + skip guard | S |
| P1 | HP-02 | infra | `lint-catch-all.py` invoked by lint hooks but never copied by setup — blocks `lint-cpp-common` scan | `agents/scripts/core/setup-harness.sh` (missing copy); template `docs/harness/claude-code/hooks/lint-catch-all.py` | Add `copy_template …/lint-catch-all.py .claude/hooks/lint-catch-all.py` to `setup_claude_code()` | S |
| P1 | hooks-session-lifecycle-03 | infra | `sync-settings-hooks.sh` additive-only — template removals/renames silently never propagate | `agents/scripts/core/sync-settings-hooks.sh:13-16` | Add a `--check` mode listing deployed hooks absent from template; document the limitation | M |
| P1 | hooks-session-lifecycle-04 | infra | `session-tree-banner.sh` silently falls back to `$PPID` (broken liveness) when lib missing | `agents/scripts/core/session-tree-banner.sh:34-38` | Emit a stderr WARN before the fallback shim block | S |

### P2 (27)

| Sev | ID | Cat | Title | File:line | Fix | Eff |
|---|---|---|---|---|---|---|
| P2 | merge-pipeline-06 | infra | `enforce_admins: false` — admins force-merge bypassing all branch protection | `project.config.json:66` | Set `enforce_admins: true`; route admins through `safe-admin-merge.sh` | S |
| P2 | core-scripts-python-04 | infra | `_parse_gate_carry` drops `none_head`/`none_streak` that `merge-gates.sh` emits | `agents/scripts/core/merge-watcher.py:313-332` | Extend parse + add `_bump_none_state` persistence (pairs with `-01`) | M |
| P2 | core-scripts-python-05 | infra | `_fetch_unresolved_cr_threads` ignores `pageInfo.hasNextPage` — >100 CR threads silently truncate | `agents/scripts/core/merge-watcher.py:1494,1525-1547` | Check `hasNextPage`; paginate or raise | M |
| P2 | ci-workflows-05 | security | `perf-full.yml` workflow-level `contents: write` overbroad for a scheduled workflow | `.github/workflows/perf-full.yml:17-20` | Workflow-level `contents: read`; elevate only in the push/baseline-bump job | M |
| P2 | core-scripts-bash-07 | security | Four scripts hardcode `alexandrosk0/Smatchet` REPO fallback — fork/rename targets wrong repo | `plan-archival-owed.sh:35`, `postmortem-owed.sh:60`, `setup-branch-protection.sh:35`, `setup-locks-ruleset.sh:43` | Replace bare fallback with `gh repo view --json nameWithOwner --jq …` + fail loudly if empty | M |
| P2 | core-scripts-bash-04 | infra | ~10 production scripts use `set -uo pipefail` (missing `-e`) — failures non-fatal | `agent-progress.sh:18`, `assert-code-unchanged.sh:12`, `check-main-repo-clean.sh:26`, `check-test-list.sh:18`, `followup-due-nudge.sh:39`, `issue-sweep.sh:20`, `merge-gates.sh:96`, `rewrite-plan-paths.sh:18`, `sync-settings-hooks.sh:21`, `tail-agent.sh:25` | Add `-e`; `\|\| true` on intentional-continue lines | M |
| P2 | core-scripts-bash-05 | infra | Five hook/utility scripts have `set -u` only (no `-e`/`pipefail`) | `clear-session-context.sh:21`, `session-heartbeat.sh:15`, `session-tree-banner.sh:22`, `lint-flush.sh:12`, `memory-drain-nudge.sh:23` | Upgrade to `set -euo pipefail`; wrap must-not-block hook bodies with `\|\| true` | M |
| P2 | core-scripts-bash-06 | tooling | Three test scripts have `set -u` only (missing `-e` and `pipefail`) | `test-lint-hook-split.sh:22`, `test-setup-harness.sh:18`, `test-skill-load-log.sh:14` | Change to `set -euo pipefail` | S |
| P2 | core-agent-prompts-04 | infra | `perf-gatekeeper.md` banner says `read-write`; convention is `read-edit` | `agents/core/perf-gatekeeper.md:24` | Change open+close banner to `read-edit` | S |
| P2 | core-agent-prompts-07 | process | `issue-janitor.md` Outcome contract has 3 of 5 states (missing `halted`, `failed`) | `agents/core/issue-janitor.md:63` | Set `applied \| halted \| failed \| partial \| aborted` per `delegation.md:147` | S |
| P2 | core-agent-prompts-08 | process | `test-rig.md` Outcome contract has 4 of 5 states (missing `halted`) | `agents/core/test-rig.md:83` | Add `halted` | S |
| P2 | project-prompts-skills-04 | process | `perf-gatekeeper` skill instructs `## Self-improvement` — an agent-only output section | `agents/_shared/skills/perf-gatekeeper/SKILL.md:63` | Remove the "End with ## Self-improvement…" line | S |
| P2 | project-prompts-skills-05 | process | `test-authoring` Pattern B points to stale `AppController.cpp` scenario-registration site | `agents/_shared/skills/test-authoring/SKILL.md:118` | Point to `Source/Core/src/Commands/Scenarios/SmatchetScenarioRegistry.cpp` | S |
| P2 | core-scripts-python-06 | tooling | `md_lint.py` has only one rule (MD028) despite "extensible" design | `agents/scripts/core/md_lint.py:47` | Add `check_md047` (final newline) + `check_md009` (trailing ws) | M |
| P2 | core-scripts-python-07 | debt | `_lock-json.py` docstring claims `datetime.utcnow` but code uses 3.12-safe `now(timezone.utc)` | `agents/scripts/core/_lock-json.py:24,36-40` | Fix docstring; drop the `utcnow` parenthetical | S |
| P2 | core-scripts-python-08 | test | `agent_size_audit.py --selftest` has no stale-baseline-entry check | `agents/scripts/core/agent_size_audit.py:357-420` | Add `--prune-stale-baseline` cross-referencing `git ls-files`; warn in `--selftest` | M |
| P2 | self-improvement-loop-06 | tooling | `applied.md` excluded from all anchor + link validation | `agents/scripts/core/test_doc_anchors.py:52` | Remove/narrow the `:(exclude)`; at minimum run anchor validation within `applied.md` | M |
| P2 | self-improvement-loop-07 | process | ~59 `Status: applied` entries never migrated from live category files to `applied.md` | `tooling.md` (29), `infra.md` (14), `process.md` (11), `debt.md` (3), `security.md` (1), `test.md` (1) | Drain to `applied.md`; add a CI lint that fails on `Status: applied` left in a live category file | M |
| P2 | self-improvement-loop-04 | tooling | Stale `scripts/dev/` path refs in backlog category files (migration not reflected) | `docs/self-improvement/categories/tooling.md` (29+ hits), `infra.md`, others | Sweep `scripts/dev/<name>` → `agents/scripts/core/`; annotate genuinely-missing as `(not yet created)` | S |
| P2 | self-improvement-loop-05 | process | Flat plan paths in category entries miss the tiered prefix | `process.md:73`; `debt.md:47,59` | Replace flat `docs/plans/<name>.md` with `shipped/`/`active/` paths; add format rule to entry-authoring spec | S |
| P2 | self-improvement-loop-08 | tooling | `tooling.md:310` applied entry masks an open autostart-wrapper env-var sub-item | `docs/self-improvement/categories/tooling.md:310` | Split: mark parent fully applied; file a new open entry for the env-var gap before next drain | S |
| P2 | bats-coverage-05 | test | `lock-staleness-sweep.sh` (scheduled CI job) has zero bats coverage | `agents/scripts/core/lock-staleness-sweep.sh` | Add `lock_staleness_sweep.bats` (REPO-unset, GH_TOKEN-absent, stale-lock fixture) + wrapper | M |
| P2 | bats-coverage-06 | test | `git-janitor.sh` has zero bats coverage | `agents/scripts/core/git-janitor.sh` | Add `git_janitor.bats` (dry-run + no-stale fast-exit, sandbox bare repo) + wrapper | M |
| P2 | bats-coverage-07 | test | `is-pure-docs-diff.sh`, `verify-cr-reply.sh`, `merge-gates-prompt.sh` — gate scripts with no bats | `agents/scripts/core/{is-pure-docs-diff,verify-cr-reply,merge-gates-prompt}.sh` | One fixture-driven bats file per script | M |
| P2 | HP-03 | process | `AGENTS.md:124` claims "no `.codex/` mirror" — stale since `setup_codex()` generates one | `AGENTS.md:124` | Update the Codex row to reflect the generated `.codex/` mirror | S |
| P2 | hooks-session-lifecycle-06 | tooling | `clear-session-context.sh` silently writes `_Session: unknown` when id extraction fails | `agents/scripts/core/clear-session-context.sh:28-40` | Emit a stderr WARN when `SESSION_ID` stays `unknown` | S |

### P3 (18)

| Sev | ID | Cat | Title | File:line | Fix | Eff |
|---|---|---|---|---|---|---|
| P3 | merge-pipeline-09 | debt | GraphQL `contexts(first: 100)` hard cap → exit-5 on repos with >100 CI contexts | `agents/scripts/core/merge-gates.graphql:18` | Raise `first` to 250 + add a pagination loop; document remaining cap | M |
| P3 | core-scripts-python-10 | debt | `merge-watcher.py`/`-cli.py` share registry helpers via fragile `importlib.util` dynamic import | `agents/scripts/core/merge-watcher.py:141-148` | Extract `read_registry`/`write_registry`/`registry_lock`/`watcher_root` into a normally-imported module | L |
| P3 | core-scripts-python-09 | debt | `comment_audit.py` uses bare `sys.path.insert` for sibling import, uncommented | `agents/scripts/core/comment_audit.py:24-25` | Add an intentional-marker comment or migrate to `Path(__file__).parent` | S |
| P3 | core-scripts-bash-08 | debt | `setup_pi()` calls bare `python3` instead of `find_python` — breaks on WinStore-alias stub | `agents/scripts/core/setup-harness.sh:352-353` | Use `if py="$(find_python)"; then "$py" …` to match the codex branch | S |
| P3 | core-scripts-bash-09 | tooling | `test-shell-lint.sh` warn path prints wrong shellcheck install hint (`npm install -g`) | `agents/scripts/core/test-shell-lint.sh:65` | Replace with scoop/brew/apt hints or the shellcheck install URL | S |
| P3 | core-agent-prompts-10 | debt | `mechanic.md` uses bash brace-expansion `scripts/{Automation,SmatchetHooks,RunLua}.lua` | `agents/core/mechanic.md:39` | Expand the brace list to literal paths | S |
| P3 | project-prompts-skills-06 | debt | `p4-annotate.md` lists `annotate` twice in `triggers:` | `agents/project/p4-annotate.md:17-18` | Remove the duplicate; add `- describe` if p4-describe triggering was intended | S |
| P3 | project-prompts-skills-07 | tooling | `drain-memory` inbox-locate uses an unquoted `*` glob in an `ls` instruction | `agents/_shared/skills/drain-memory/SKILL.md:21` | Replace with `find "$HOME/.claude/projects/" -maxdepth 2 -type d -name memory` | S |
| P3 | rule-docs-drift-07 | debt | `STRUCTURE.md` hardcodes stale "157" for the portable-purity baseline (actual 192) | `docs/STRUCTURE.md:69` | Replace "157" with "192" or reference the file directly | S |
| P3 | self-improvement-loop-09 | debt | `sort-applied-md.sh` header comments cite the stale pre-split path | `agents/scripts/core/sort-applied-md.sh:2,15` | Update comment lines to `agents/scripts/core/sort-applied-md.sh` | S |
| P3 | bats-coverage-08 | test | `issue_sweep.bats` never exercises the `--apply` mutation path | `tests/bats/issue_sweep.bats:24-58` | Add a `gh`-stubbed `--apply` test asserting `gh issue edit` is called | S |
| P3 | bats-coverage-09 | debt | `check-required-tools.sh` describes bats scope as "merge-gates regression suite" only (38 files exist) | `scripts/dev/check-required-tools.sh:20,66` | Change both to "tests/bats/*.bats (all agentic suites)" | S |
| P3 | HP-04 | process | `setup.md` "What the script generates" table documents ~half the actual artifacts | `docs/harness/claude-code/setup.md:15-27` | Regenerate from the real `setup_claude_code()` copy/link inventory | S |
| P3 | HP-05 | tooling | `test-portable-purity.sh` failure hint points to a nonexistent `scripts/dev/` path | `agents/scripts/core/test-portable-purity.sh:90` | Fix hint to `agents/scripts/core/test-portable-purity.sh --refresh` | S |
| P3 | HP-06 | infra | `setup-harness.sh` never wires `pretool-edit-p4-lock-check.sh` under p4 mode (doc claims it does) | `agents/scripts/core/setup-harness.sh` (p4 path); `docs/plans/shipped/git-to-perforce-migration.md:154` | Wire the hook copy in the `SMATCHET_AGENT_VCS=p4` branch, or retract the doc claim | S |
| P3 | HP-07 | debt | Orphan hook template `lint-cpp-pillar2.sh` — never copied by setup, never called by drain | `docs/harness/claude-code/hooks/lint-cpp-pillar2.sh` | Delete the orphan + update `pillar-1-2-perf-review-system.md`, or wire it | S |
| P3 | HP-08 | debt | Portable-purity baseline carries a stale Codex entry after de-branding | `docs/high-integrity/portable-purity-baseline.txt:190` | Drop the `hooks-equivalent.md	Smatchet` line; rerun `--refresh` | S |
| P3 | HP-09 | debt | `project.config.json` `harness.supported` missing `pi`; `HARNESS_SUPPORTED` has no consumer | `project.config.json:103` | Add `pi` to the list; delete the unconsumed block or add a consumer | S |

## Cross-charter dedup notes

Findings that share a root cause across charters were merged into single slices below; the overlaps:

- **`python3` WinStore-alias resolution** — `bats-coverage-01` (P0), `bats-coverage-02` (P1), `core-scripts-bash-08` (P3) are one root cause (bare-`python3` that doesn't exec-validate). → Slice **A1**.
- **Incomplete shell strict-mode** — `core-scripts-bash-02/04/05/06` are one root cause (`set` line missing `-e`/`pipefail`). → Slice **A2** (split by risk tier).
- **merge-watcher NONE-state** — `core-scripts-python-01` (seed) + `core-scripts-python-04` (parse) are two halves of one fix. → Slice **B3**.
- **CodeRabbit md↔py drift** — `core-agent-prompts-03` (md declares a check) + `core-scripts-python-03` (py lacks the selftest). → Slice **B4**.
- **Ledger blindness** — `merge-pipeline-02` (confirmed; human/admin path) overlaps the **already-tracked** `self-improvement-loop-03` (`tooling.md:149`, watcher/override path applied 2026-06-13). The confirmed half is the distinct human-merge gap; do not re-file the tracked half. → Slice **B6**.
- **`scripts/dev/` → `agents/scripts/core/` path drift** — `self-improvement-loop-04` (category prose) + `self-improvement-loop-09` (sort-applied-md.sh comments) + `HP-05` (purity-test hint). → folded into Slice **C2**.
- **Stale baselines/counts** — `rule-docs-drift-06` (agent-size 154→95), `rule-docs-drift-07` (STRUCTURE 157→192), `HP-08` (purity baseline entry). → Slice **C3** (regenerate together).
- **`AGENTS.md` prose drift** — `rule-docs-drift-01` (5→7 checks, P0) + `HP-03` (.codex mirror). Both are `AGENTS.md` edits → bundle in Slice **C1**.
- **delegation Outcome-contract states** — `core-agent-prompts-07` + `-08` both diverge from `delegation.md:147`. → Slice **C4**.

## Slice plan — PR-sized, dependency-ordered

Convention: gate/script changes carry their bats coverage **in the same slice** (per `AGENTS.md` § Verification automation); pure-docs/prose slices are cheap and may land in any order, in parallel, once Wave 0 is clear. None of these slices touches `Source/Core/` → every slice is a pure-docs / agentic-shell / CI-config / `project.config.json` diff, so the C++ build + perf gates are `N/A` throughout (doc-validation + bats + shell-lint are the live gates).

### Wave 0 — P0 correctness (do first)

- **A0 · `AGENTS.md` required-check count** (P0 `rule-docs-drift-01`) — one-line `5`→`7` fix. Trivial, unblocks nothing but is the cheapest P0. *Pure-docs.*
- **A1 · python3 interpreter-resolution hardening** (P0 `bats-coverage-01`, P1 `bats-coverage-02`, P3 `core-scripts-bash-08`) — replace bare-`python3` probes with the 3-way exec-validating loop; the bats edits **are** the coverage. Verify on the WinStore-alias path. *Script+bats.*
- **B1 · merge override-label race** (P0 `merge-pipeline-01`) — timestamp the `labeled` event in `coverage-gate.yml` + the watcher merge path; add a merge_watcher bats case for the race window. *Gate+bats.*
- **B2 · merge ledger for human/admin merges** (P0 `merge-pipeline-02`, P2 `merge-pipeline-06`) — post-merge snapshot-append for non-watcher merges + flip `enforce_admins: true`; bats for the append path. Note overlap with tracked `self-improvement-loop-03`. *Script+config+bats.*

### Wave 1 — P1 (correctness + drift-guards)

- **B3 · merge-watcher NONE-state** (P1 `core-scripts-python-01` + P2 `core-scripts-python-04`) — seed + parse `none_head`/`none_streak`, persist across cycles; bats. *Script+bats.*
- **B4 · CodeRabbit md↔py drift-guard** (P1 `core-agent-prompts-03` + `core-scripts-python-03`) — add `coderabbit-triage.py --selftest` + the CI check the `.md` already claims; bats. *Script+CI+bats.*
- **B5 · reversed sanitizer-preset selector** (P1 `core-scripts-python-02`) — swap the TSAN/ASAN branches; bats asserting preset↔status mapping. *Script+bats.*
- **B6 · `cr-out-of-band` requires CR to have run** (P1 `merge-pipeline-04`) — gate guard + `merge-gates.md` update; bats. *Gate+bats.*
- **B7 · postmortem-owed blocking mode + watcher docstring** (P1 `merge-pipeline-03` + `merge-pipeline-05`) — `--blocking` mode promoted to a SessionStart gate; rewrite the stale `merge-watcher.py` docstring; bats for the blocking exit. *Script+bats.*
- **A2 · shell strict-mode (tier 1: prod + notify)** (P1 `core-scripts-bash-02`, P2 `core-scripts-bash-04`) — add `-e` to `smatchet-notify.sh` + the ~10 production scripts, with `|| true` audit on continue-lines; shell-lint + existing bats. *Script.* (Tier 2 hooks/tests → Wave 2 to bound blast radius.)
- **D1 · CI workflow triggers + cache** (P1 `ci-workflows-03` + `-04`, P2 `ci-workflows-05`) — `merge_group:` on perf-pr-fast, cache-path fix on sanitizer-nightly, least-privilege on perf-full. *CI-config.*
- **D2 · setup-harness lint-catch-all copy** (P1 `HP-02`) — add the missing `copy_template`; extend `test-setup-harness.sh`. *Script+bats.*
- **E1 · session-lifecycle hook hardening** (P1 `hooks-session-lifecycle-03` + `-04`, P2 `-06`) — `sync-settings-hooks.sh --check`, stderr WARNs on the two silent fallbacks; bats. *Script+bats.*
- **C-fast · P1 prose/link fixes** (P1 `core-agent-prompts-02`, `rule-docs-drift-04`, `rule-docs-drift-05`, `project-prompts-skills-01`) — include-path, broken cross-link, `#TBD`, parity-array entry. *Pure-docs / one-line script.* Cheap, parallel.

### Wave 2 — P2 (hygiene, coverage, drift-guards)

- **B8 · CR-thread pagination** (P2 `core-scripts-python-05`) — honour `hasNextPage`; bats. *Script+bats.*
- **A2b · shell strict-mode (tier 2: hooks + tests)** (P2 `core-scripts-bash-05` + `-06`) — hooks wrapped `|| true`; bats. *Script.*
- **C5 · repo-derivation hardening** (P2 `core-scripts-bash-07`) — `gh repo view` fallback in 4 scripts; bats. *Script+bats (security).*
- **C2 · self-improvement backlog hygiene** (P2 `self-improvement-loop-04/05/06/07/08`, folds `-09`, `HP-05`) — drain `Status: applied`, fix path tiers, un-exclude `applied.md` from validation + add the migration lint, sweep stale `scripts/dev/` refs. *Docs+tooling; the validation-lint half carries bats.*
- **C3 · regenerate stale baselines** (P2 `rule-docs-drift-06`, P3 `rule-docs-drift-07`, `HP-08`) — regen agent-size + purity baselines, fix STRUCTURE count. *Docs/baseline.*
- **C1 · AGENTS.md .codex row** (P2 `HP-03`) — fold with A0 if not yet shipped, else standalone. *Pure-docs.*
- **C4 · delegation Outcome-contract states** (P2 `core-agent-prompts-07` + `-08`, P2 `-04`, P2 `project-prompts-skills-04` + `-05`) — agent/skill prose to match the contract. *Pure-docs (agent-contract gate).*
- **F1 · new bats suites for uncovered gate scripts** (P2 `bats-coverage-05/06/07`) — `lock-staleness-sweep`, `git-janitor`, the three gate scripts. *Bats-only.*
- **G1 · small python debt** (P2 `core-scripts-python-06/07/08`) — md_lint rules, `_lock-json` docstring, agent_size_audit baseline-prune; bats where logic changes. *Script+bats.*

### Wave 3 — P3 (debt; batch cheaply)

- **H1 · prose/string debt batch** (P3 `core-scripts-bash-09`, `core-agent-prompts-10`, `project-prompts-skills-06/07`, `self-improvement-loop-09`, `bats-coverage-09`, `HP-04`) — trivial one-liners; bundle into 1–2 docs PRs. *Pure-docs / strings.*
- **H2 · harness setup residue** (P3 `HP-06`, `HP-07`, `HP-09`) — p4-lock-check wiring-or-retract, orphan pillar2 delete, project.config harness block; bats where setup changes. *Script+config.*
- **H3 · merge-pipeline + python debt** (P3 `merge-pipeline-09`, `core-scripts-python-09/10`, `bats-coverage-08`) — GraphQL pagination cap, `comment_audit` import, the `importlib` registry refactor (L — may stand alone), issue_sweep `--apply` test. *Script+bats.*

## Files to modify

This meta-campaign authors **one** file: this tracker. Every fix lands in its slice's own PR with that slice's write set (named inline per slice above). No `Source/Core/` TU is touched by any slice. Grep-before-naming applies per slice, not here.

- `docs/plans/active/agentic-infra-audit-campaign-2026-06.md` (this file) — § Implementation log updated as each slice ships; flip to `shipped` + `git mv` to `docs/plans/shipped/` when the backlog is drained.

## Existing utilities reused

- `agent_size.bats:25-29` — the canonical 3-way `for c in python3 python py` exec-validating probe Slice A1 copies.
- `test-skill-vs-agent-parity.sh:34-46` `SKILL_ONLY_HELPERS` — the additive array Slice C-fast extends.
- `safe-admin-merge.sh` — the agent-side admin-merge guardrail Slice B2 routes through after `enforce_admins: true`.
- `merge-snapshots.jsonl` append path (`merge-watcher.py:2192`) — the writer Slice B2 generalises to non-watcher merges.
- `coverage.yml:41-47` / `doc-validation.yml:61-70` — proven `merge_group:` self-gate templates Slice D1 copies for `perf-pr-fast.yml`.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — no slice compiles a `Source/Core/` change; all diffs are docs / agentic-shell / CI-config / `project.config.json`.
- **Pillar 2 (UI-thread never blocks)**: no impact — no runtime code path touched.
- **Pillar 3 (never crash)**: no impact on the shipped app; several slices *improve* harness robustness (shell strict-mode, stderr WARNs on silent fallbacks).
- **Pillar 4 (accessibility)**: N/A — no UI surface in scope.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else N/A)

`N/A` for every slice — no slice modifies `Source/Core/`. The live gates are doc-validation (`scripts/dev/test-docs.sh`), the bats suites, and shell-lint; each slice declares its own verification inline.

## Risks / non-goals

- **Risk — stale evidence re-introduced.** The evidence is dated 2026-06-09/11; ~30 candidates already refuted as stale/by-design. *Mitigation:* every confirmed finding was re-read at HEAD by an adversarial verifier; each slice's implementing agent must re-confirm the cite before editing (the cited line may have moved again since this synthesis).
- **Risk — `enforce_admins: true` (Slice B2) locks out an emergency admin merge.** *Mitigation:* `safe-admin-merge.sh` remains the sanctioned bypass; coordinate the branch-protection flip with the maintainer rather than auto-applying.
- **Risk — shell strict-mode (`-e`) breaks scripts that relied on continue-on-error.** *Mitigation:* A2 split into risk tiers (prod first, hooks/tests second), every continue-line audited for an explicit `|| true`, existing bats run per script.
- **Non-goal — implementing the fixes.** This salvage was scoped READ-ONLY; this campaign is the stopping point. Slices are authored as a backlog, not executed here.
- **Non-goal — the `vexp-*` finding family.** The entire `hooks-session-lifecycle` vexp cluster (4 candidates) was refuted: `vexp-strip-agents-md.sh` was deleted in commit `4b18918c`; do not resurrect those findings.
- **Non-goal — re-filing already-tracked items.** The 6 appendix items already live in the backlog/plans; slices reference but do not duplicate them.

## Verification

Per `AGENTS.md` § Verification automation. For **this campaign doc** (a pure-docs plan PR):

- **Doc validation (blocks plan-doc PRs)**: `scripts/dev/test-docs.sh` green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint).
- **Plan stress-test — `grill-with-docs`**: this campaign was stress-tested by the 11-verifier adversarial pass that produced its confirmed set (each finding re-read at HEAD, 3-lens refutation for P0/P1); the refutation kill-rate (~30 of ~97 raw) is the stress-test evidence. Per-slice plans get their own grill at authoring time.
- **Per-slice verification**: declared inline above — gate/script slices carry bats in the same PR; CI-config slices validate on the workflow run; pure-docs slices ride doc-validation.
- **Manual residue**: the `enforce_admins: true` flip (Slice B2) and the threshold-class maintainer calls are intentionally human-gated — named here, not silent.

## Out of scope (flagged, not designed)

**Deferral residue-sweep** — before flipping this campaign to `shipped`, grep `docs/self-improvement/categories/`, `agents/*.md`, and `docs/adr/` for stray references to the refuted `vexp-*` cluster and the stale counts fixed in Slice C3.

- **The `code-review-handover-pr947-951.md` evidence** (separate older investigation, not part of this fleet) — preserved in the salvage dir; mine independently if useful. No-action here.
- **`harness-portability` HP-01** (ps1/sh setup parity) — already tracked: `kill-powershell-minimize-toolchain.md` Phase 7 deletes `setup-harness.ps1` rather than bringing it to parity. No slice; see appendix.
- **The refuted set** (~30 candidates) — recorded in `C:\Dev\salvage-2026-06-11\results\verify\*.md` § Refuted for each charter; not carried. No-action by design.

## Already-known appendix (tracked elsewhere — not slices)

These survived mining but the verifier found the root cause already tracked; listed for traceability, not for re-filing:

| ID | Sev | Tracked in |
|---|---|---|
| HP-01 | P1 | `docs/plans/active/kill-powershell-minimize-toolchain.md` Phase 7 (delete `setup-harness.ps1`, line 92) |
| merge-pipeline-07 | P2 | `docs/self-improvement/categories/process.md` — `cr-out-of-band-disposition-trail` (2026-06-07, lines 29-33) |
| self-improvement-loop-03 | P1 | `docs/self-improvement/categories/tooling.md:149` — `mandatory-merge-snapshot-on-override-merge` (applied 2026-06-13, watcher path; admin path deferred) |
| bats-coverage-03 | P1 | `docs/self-improvement/categories/test.md:29-33` — bats suites not auto-enrolled by `test-all.sh` (open) |
| bats-coverage-04 | P1 | `docs/self-improvement/categories/test.md:29-33` — same entry (markdown_links no-wrapper half) |
| project-prompts-skills-02 | P1 | `docs/plans/tooling-process-backlog-sweep.md` Slice 9, item #27 (parity script not wired into CI) |

## Provenance & salvage artifacts

- **Evidence** (out-of-repo, git-safe): `C:\Dev\salvage-2026-06-11\evidence\*.transcript.slim.md` (10 charters) + recovered `harness-portability` (from Jun-9 finder transcript `agent-aed39f02749e71d16.jsonl`).
- **Miner output**: `C:\Dev\salvage-2026-06-11\results\<charter>.md` (11 files, ~97 raw findings).
- **Verifier verdicts**: `C:\Dev\salvage-2026-06-11\results\verify\<charter>.md` (11 files — full verdict tables, confirmed/known/refuted splits, re-verification notes).
- **Rule-doc born from the debacle**: `docs/agent-rules/workflow-fleets.md` (PR #1141).
- **Already shipped from this debacle**: PR #1139 (`postmortem-owed.sh` fix + bats), PR #1141 (`workflow-fleets.md`).

## Implementation log
- 2026-07-13 · **reconciliation pass, no slice shipped** · four-agent read-only re-verification of all 67 findings vs `develop` @ `a166404` → **9 FIXED** (all incidental — closed by unrelated PRs, not by a campaign slice), **1 PARTIAL** (`merge-pipeline-04`), **57 OPEN**. Details + the FIXED mechanisms + three synthesis corrections in § Reconciliation pass (2026-07-13). No fix authored here — this is a status update only.

*(future entries — bullet per shipped slice: `<sha> · <slice> · <one-line summary>`)*

## Deviations from plan
*(none yet)*
