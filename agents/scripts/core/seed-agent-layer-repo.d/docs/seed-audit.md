# `seed-audit.md` — per-path publication verdict

> Companion to [`seed-paths.txt`](seed-paths.txt). `seed-agent-layer-repo.sh` phase 4b refuses the
> phase-5 push unless **every** manifest pathspec has **exactly one** row in § Manifest rows whose
> verdict is `CLEAR`, or `SCRUB` that is both listed in `seed-scrub-paths.txt` and gone from the
> rewrite. Phase 3 refuses to rewrite at all if any commit touched a manifest path after the pin below.

**Audited through:** `8f8e1ef8239e6516fe0e2df7c5730734e7ed8c30` — every commit reachable from this develop commit that touches a
manifest path. Anything later is unaudited until re-swept (`seed-audit-sweep.py --since <pin>`).

The seed is an **allowlist, not a subtraction**: nothing reaches the public repo that this table has not
cleared. Clearing a path means checking **both** the working-tree file **and its history** for
Smatchet-internal identifiers, internal hostnames, ticket URLs, customer/user data, credentials and
third-party content that cannot be re-published. A scrubbed head over a leaky history still publishes
the leak.

## Verdicts

| Verdict | Meaning |
|---|---|
| `CLEAR` | Head **and** history checked; nothing project-internal, nothing secret, nothing un-republishable. Publishes as-is. |
| `SCRUB` | Publishes only after a paired `git filter-repo --invert-paths` pass. Add the path to `seed-scrub-paths.txt`; phase 3 applies it, and phase 4b refuses unless the path is then absent from the rewritten history. |
| `EXCLUDE` | Never publishes, so it must not appear in `seed-paths.txt`. Phase 4b refuses an `EXCLUDE` verdict on a manifest row as a contradiction. Here it appears only in § Decisions already taken, as a **design note** recording a decision so it is not re-proposed. |
| `PENDING` | Not cleared. **Blocks the push.** A `PENDING` row that names a decision (**D1**) is audited but waiting on the owner. Any other verdict text, a missing row, or a duplicate row also blocks. |

## Method

What was checked, so the verdicts can be weighed rather than trusted:

1. **History sweep** — `agents/scripts/core/seed-audit-sweep.py` over exactly what filter-repo keeps:
   every added line in `git log -p --cc --full-history --no-renames HEAD -- <75 pathspecs>`, plus the
   message and identities of each of those commits. **588 commits, 98,719 added lines, 0 binaries.**
   Coverage asserted: all 436 manifest files at HEAD appear in the scanned history.
2. **Independent secret scan** — gitleaks 8.30.1 `detect` over the **entire** repository history
   (2,576 commits, 53 MB), findings then filtered to manifest paths. 16 findings total, 5 inside the
   manifest.
3. **Third-party content** — every seeded file grepped for provenance claims ("ported from",
   "verbatim", "copied from", licence/copyright markers), then an **8-word shingle overlap** scan of all
   436 seeded files against the one local upstream source found (Whip-Process), so an *unattributed*
   copy cannot slip through.
4. **Triage** — every distinct matched value in every category was read in context and classified. No
   category was cleared by count alone.

**Limits.** This is a pattern-and-overlap audit, not a line-by-line human read of 98,719 lines. It will
not catch a leak that matches no pattern — e.g. prose naming an employer or a customer in plain words.
The residual risk is bounded by one fact: **the Smatchet repository is already public**, so the seed
re-publishes a subset of already-public history and adds no new exposure. The audit's job is to stop a
live leak or an un-republishable text being *doubled* into a second, reuse-oriented repository.

## Findings

### Secrets — none real (two scanners agree)

Every secret-shaped string in the seeded history is a synthetic fixture in the redaction test machinery
(`agents/scripts/core/redact-intent.py`, `agents/scripts/core/redaction-escape-oracle.py`,
`tests/bats/capture_intent.bats`), each checked by hand:

Values below are **deliberately truncated** so this document does not itself match the patterns it
reports on — otherwise phase 4b's own secret scan would flag the audit table.

| Shape | Value (truncated) | Why it is not a credential |
|---|---|---|
| GitHub | `ghp_ABCDEF…`, `github_pat_11ABCDEF…` | Sequential alphabet + digits |
| AWS | `AKIAIOSFODNN7…` | AWS's own documented example key |
| Stripe | `sk_live_abcd…` | Sequential placeholder (all 5 in-manifest gitleaks findings) |
| Slack | `xoxb-12345…` | Sequential placeholder |
| OpenAI / Google | `sk-proj-abcd…`, `AIzaabcd…` | Sequential placeholders |
| JWT | `eyJhbGciOiJIUzI1NiJ9…` | Decodes to `{"alg":"HS256"}.{"sub":"1"}` |
| Private keys | RSA header + `MIIEowIBAAKCAQEA…`; OpenSSH header + `b3BlbnNzaC1rZXktdjE…` | RSA-2048 DER prefix + `abcdef`; the OpenSSH `openssh-key-v1…none…ssh-ed` preamble. No key material |
| URL credentials | `https://user:<placeholder>@…`, `postgres://admin:<template slot>@…` | Literal placeholders / template slots |

The 11 remaining gitleaks findings are all host-side (`Source/`, `tests/Core/`, `tests/fuzz/`) and are
not seeded.

### Hosts, tickets, P4 — none internal

27 distinct URL hosts, all public (`github.com`, `claude.ai`, `agents.md`, `gradle.org`,
`msdl.microsoft.com`, `api.deepseek.com`, `docs.github.com`, `git-scm.com`, …), loopback, or placeholders
(`db.internal`, a hypothetical `*.proxy.corp` in one commit message). **Zero** Atlassian tenants, Jira
hosts, private IPs or `/browse/` ticket URLs. The 31 ticket-shaped keys are ADR / PR / UTF-8 / ISO-8601 /
CWE-78 / OSV ids and the repo's own work-item codes. P4: only generic `//depot/path` examples and
`localhost:1666`; one commit message names a local workspace (`smatchet_main_alexk`).

### Third-party content

- **`agents/_shared/skills/grill-with-docs/`** — three files verbatim from `mattpocock/skills`, which is
  **MIT**. MIT requires its notice to travel with copies and none did; **resolved** by adding
  `UPSTREAM-LICENSE` beside them.
- **Whip-Process** — see **D1**. Absorbed into Smatchet in August 2026 (plan
  `docs/plans/absorb-whip-process.md`, host-side). The only copy of the source found is a local folder with
  **no licence file and no git remote**, and its README describes it as "extracted from the project that grew
  it", so neither authorship nor licence can be established from the repository. The overlap scan found
  substantial upstream text in exactly the nine files that already say so, and nothing unattributed:

  | Seeded file | Overlap | Upstream source |
  |---|---|---|
  | `docs/agent-rules/work-items.md` | 62% | `Process.md`, `Conventions.md` |
  | `agents/_shared/skills/close-work-item/SKILL.md` | 24% | `Procedures/ClosingItem.md`, `ClosingReview.md` |
  | `docs/agent-rules/review-panels.md` | 22% | `Procedures/ReviewBasics.md` |
  | `agents/scripts/core/run-review.sh` | 18% | `Tools/run-review.ps1` |
  | `agents/_shared/skills/pre-implementation-review/SKILL.md` | 18% | `Procedures/PreImplementationReview.md` |
  | `agents/scripts/core/lib/review-guard.sh` | 17% | `Tools/review-guard.ps1` |
  | `agents/scripts/core/work_item_lint.py` | 17% | `Tools/check-docs.ps1` |
  | `agents/_shared/skills/address-review-feedback/SKILL.md` | 14% | `Procedures/AddressReviewFeedback.md` |
  | `agents/_shared/skills/adversarial-code-review/SKILL.md` | 3% | `Procedures/PostImplementationReview.md` |

  Everything else seeded is at or below 0.8% — incidental shared phrasing.

### Personal data

Commit identities on the seeded history: the owner's name with their **personal e-mail address** (631
author/committer lines), `GitHub <noreply@github.com>` (531, squash-merge committer), `Claude
<noreply@anthropic.com>` (10), `claude[bot]` (4). The same personal address also appears as a
redaction fixture in `redact-intent.py` and `capture_intent.bats`. See **D2**. (This document names
the address only by description, so it does not add one more copy.) Local user paths are the
owner's `alexk` handle, only inside redaction fixtures.

## Decisions for the owner

### D1 — Whip-Process-derived content · **BLOCKING**

Five rows stay `PENDING` until this is answered, because re-publishing text of unknown licence inside a
repository whose `LICENSE` says MIT would grant rights nobody has shown they hold. The answer applies to
**Smatchet as well** — it already carries the same text publicly.

- **(a) Rights confirmed.** The owner authored Whip-Process or holds permission under an MIT-compatible
  licence. Record the licence and attribution beside the ported files (as `grill-with-docs` now does) and
  flip the five rows to `CLEAR`.
- **(b) Rewrite.** Re-express the nine files in original words. Their *history* still contains the
  verbatim versions, so the paths also go in `seed-scrub-paths.txt` (history dropped) and the rewritten
  files are committed to the layer after the seed.
- **(c) Leave the subsystem out.** Scrub the nine files (plus the two ported test suites) from the seed.
  The layer then ships without the review-panel / work-item loop.

### D2 — owner's personal e-mail · non-blocking

Already public through Smatchet's history, so this is a preference, not a leak. If wanted, the seed can
map it to `alexandrosk0@users.noreply.github.com` with `git filter-repo --mailmap` and replace the two
fixture occurrences with `--replace-text`. That needs a small seed-script change and is not wired today.

## Decisions already taken (not rows in the manifest)

These paths are deliberately **absent** from `seed-paths.txt`. Recorded so the omission reads as a
decision rather than an oversight:

| Path | Verdict | Why |
|---|---|---|
| `agents/project/` | `EXCLUDE` | Project-scoped by construction. This is the reason the manifest enumerates four explicit `agents/*` subtrees instead of one bare `agents/` prefix. |
| `agents/project/workflows/historical-review-sweep.js` | `EXCLUDE` | Self-identifies as non-portable. Already excluded wholesale by the `agents/project/` decision above, so it needs **no** paired scrub pass — the plan's row-8 text predates its move out of `agents/scripts/core/`. |
| `agents/README.md` | `EXCLUDE` | Sits directly under `agents/`, inside no allowed subtree, and its prose is host-specific ("every Smatchet subagent"). The layer needs its own. |
| `docs/self-improvement/categories/`, `postmortems.md`, `applied.md`, `*.jsonl` | `EXCLUDE` | Entries stay host-side (grill decision 4 / ADR-0025). Only the framework spec `AGENT_SELF_IMPROVEMENT.md` moves. |
| `docs/plans/`, `docs/work/`, `Source/`, `CMakeLists.txt` | `EXCLUDE` | Host content. `seed-agent-layer-repo.sh` phase 2 hard-fails if any of these ever appears in the manifest. |

## Manifest rows

| # | Path | Verdict | Reviewer | Date | Notes |
|---|---|---|---|---|---|
| 1 | `agents/core/` | `CLEAR` | Claude (agent) | 2026-09-13 | No secrets, internal hosts or tickets in head or history. `p4-janitor.md` shows `P4USER=alexk` (the owner's handle, already public). Host-literal prose is the de-Smatchet-ification follow-up, not a publication blocker. |
| 2 | `agents/_shared/` | `PENDING` | Claude (agent) | 2026-09-13 | BLOCKED on **D1**: `skills/address-review-feedback`, `skills/close-work-item`, `skills/pre-implementation-review` (14–24% verbatim) and the post-implementation section of `skills/adversarial-code-review` (3%) carry Whip-Process text. Everything else clear; `skills/grill-with-docs` is MIT upstream and now ships its notice (`UPSTREAM-LICENSE`). |
| 3 | `agents/scripts/core/` | `PENDING` | Claude (agent) | 2026-09-13 | BLOCKED on **D1**: `lib/review-guard.sh`, `run-review.sh`, `work_item_lint.py` are ports of Whip-Process PowerShell tools (17–18% textual overlap). Every secret-shaped string in this subtree is a synthetic redaction fixture (see § Findings, Secrets). |
| 4 | `agents/scripts/project/` | `CLEAR` | Claude (agent) | 2026-09-13 | Generic `//depot/path` examples, `localhost:1666`, the public gradle.org checksum page. No findings. |
| 5 | `docs/agent-rules/` | `PENDING` | Claude (agent) | 2026-09-13 | BLOCKED on **D1**: `work-items.md` is **62% verbatim** from Whip-Process `Process.md` + `Conventions.md`; `review-panels.md` 22% from `Procedures/ReviewBasics.md`. Rest clear: public hosts only (`msdl.microsoft.com`, `api.deepseek.com`), the owner's `C:/Dev/Smatchet` checkout layout in `process-rules.md`. |
| 6 | `docs/harness/` | `CLEAR` | Claude (agent) | 2026-09-13 | `SETUP.md` names the owner's `C:/Dev/Smatchet` checkout (machine layout, not sensitive). The pi subagent example is copied at setup time into gitignored `.pi/` and is never tracked, so nothing third-party ships from here. |
| 7 | `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` | `CLEAR` | Claude (agent) | 2026-09-13 | Framework spec only — no entry text. Its `## Index` links to host-side category files, which will dangle layer-side: a link-checker concern, not a publication one. |
| 8 | `docs/high-integrity/portable-purity-baseline.txt` | `CLEAR` | Claude (agent) | 2026-09-13 | Path + literal lists only. |
| 9 | `docs/high-integrity/agent-size-baseline.md` | `CLEAR` | Claude (agent) | 2026-09-13 | Path + line-count lists only. |
| 10 | `AGENTS.md` | `CLEAR` | Claude (agent) | 2026-09-13 | Links to public third-party docs only (`agents.md`, `raw.githubusercontent.com/mattpocock/skills`). Root rulebook only — leaf `AGENTS.md` files stay host-side (asserted in phase 4b). |
| 11 | `scripts/dev/project-config.sh` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings. |
| 12 | `scripts/dev/test-all.sh` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings. |
| 13 | `scripts/dev/test-docs.sh` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings. |
| 14 | `tests/bats/agent_size.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 15 | `tests/bats/android_openssl_failfast.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 16 | `tests/bats/archive_backlog_entry.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | `example.com` link fixture only. |
| 17 | `tests/bats/capture_intent.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | Synthetic token fixtures only — verified placeholders (see § Findings, Secrets). Also uses the owner's real e-mail as a redaction fixture: **D2**, non-blocking. |
| 18 | `tests/bats/coderabbit_triage.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 19 | `tests/bats/cr_oob_review_backfill.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 20 | `tests/bats/dead_export_audit.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 21 | `tests/bats/dup_audit.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 22 | `tests/bats/fail_open_authoring.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 23 | `tests/bats/fleet_preflight.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 24 | `tests/bats/fleet_rescope.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 25 | `tests/bats/followup_due_nudge.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 26 | `tests/bats/function_size.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 27 | `tests/bats/fuzz_closure_audit.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 28 | `tests/bats/gate_selftests.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 29 | `tests/bats/git_janitor.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 30 | `tests/bats/harness_provisioned_doctor.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 31 | `tests/bats/historical_review_ledger_reconcile.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 32 | `tests/bats/historical_review_survivors.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 33 | `tests/bats/include_cycle_audit.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 34 | `tests/bats/is_pure_docs_diff.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 35 | `tests/bats/issue_sweep.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 36 | `tests/bats/lint_rules.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 37 | `tests/bats/lock_claim.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 38 | `tests/bats/lock_staleness_sweep.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 39 | `tests/bats/markdown_links.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | `example.com` / `a.b` / `ftp://server` link fixtures only. |
| 40 | `tests/bats/merge_gates.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 41 | `tests/bats/merge_watcher.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 42 | `tests/bats/merge_watcher_integration.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 43 | `tests/bats/migrate_bugs_to_issues.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 44 | `tests/bats/oob_label_impl.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 45 | `tests/bats/panel_verdicts.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | Comment reference to the Whip-Process absorption plan only; suite authored first-party (absorption Phase 4), not a port. |
| 46 | `tests/bats/plan_archival_owed.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 47 | `tests/bats/plan_index_robustness.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 48 | `tests/bats/plan_lock_gate.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 49 | `tests/bats/postmortem_owed.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 50 | `tests/bats/pre_push_guard.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 51 | `tests/bats/preship_review_artifact.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 52 | `tests/bats/repo_health_facts_nudge.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 53 | `tests/bats/required_context_adr_consistency.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 54 | `tests/bats/required_context_parity.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 55 | `tests/bats/resolve_py.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 56 | `tests/bats/resolve_repo.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 57 | `tests/bats/review_ack_gate.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 58 | `tests/bats/review_guard.bats` | `PENDING` | Claude (agent) | 2026-09-13 | BLOCKED on **D1**: self-declares "test semantics ported from Tools/test-run-review.ps1". Textual overlap below the scan floor — a translation, so D1 may reasonably clear it along with the tool it tests. |
| 59 | `tests/bats/run_review.bats` | `PENDING` | Claude (agent) | 2026-09-13 | BLOCKED on **D1**: self-declares "test semantics ported from Tools/test-run-review.ps1". Textual overlap is low (0.5%) — a translation, so D1 may reasonably clear it along with the tool it tests. |
| 60 | `tests/bats/safe_admin_merge.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 61 | `tests/bats/safe_merge.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 62 | `tests/bats/script_freshness.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 63 | `tests/bats/session_registry.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 64 | `tests/bats/setup_branch_protection.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 65 | `tests/bats/shell_lint.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 66 | `tests/bats/small_helper_audit.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 67 | `tests/bats/subsystem_docs.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | `t@t.test` fixture identity only. |
| 68 | `tests/bats/sync_issue_labels.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 69 | `tests/bats/unwatched_pr_nudge.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 70 | `tests/bats/verifier_preship_wiring.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | Loopback (`127.0.0.1`) test endpoints only. |
| 71 | `tests/bats/verifier_review_gate.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | Comment reference to the Whip-Process absorption plan only; the panel end-to-end section is first-party, not a port. |
| 72 | `tests/bats/work_item_owed.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 73 | `tests/bats/workflow_job_mask.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 74 | `tests/bats/workflow_watchdog.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 75 | `tests/bats/worktree_prune.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |

## Status

**70 of 75 rows cleared.** 5 rows are `PENDING`, all blocked on **D1** and nothing else. The push stays
refused until D1 is answered and those rows carry a non-`PENDING` verdict.

When the tree moves past the pin (it will — the surface takes several commits a day), re-sweep only the
delta, triage the new values, update the affected rows, and move **Audited through:**:

```bash
python3 agents/scripts/core/seed-audit-sweep.py --since <pin>
```
