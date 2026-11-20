# Agent self-improvement — applied (archive)

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../../../agent-layer/docs/self-improvement/AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Closed entries. Archive moves immediately on Status → applied. Sorted by original surface date, latest first.
> **Bounded head**: this file holds the current + previous month only; older months are
> rotated into flat `applied-YYYY-MM.md` siblings by
> [`rotate-applied-md.sh`](../../../agent-layer/agents/scripts/core/rotate-applied-md.sh)
> (run automatically by `archive-backlog-entry.sh`).
>
> **Deleted-runtime banner (2026-05-21)** — applied entries below that reference the agentic-flow C++ runtime (`AgenticHandoffController`, `AgenticTriageController`, `AgentProposalStore`, `ClaudeCodeLocalRunner`, `PrCommentWatcher`, `PrCheckRunWatcher`, `HarnessRunState`, `CoderabbitCommentClassifier`, `CiFailureClassifier`, `dispatch_source` enum, sentinel-file protocol, `agent/<proposalId>` worktrees, `coderabbit-react-loop` design, `agents/handoff-implementer.md`, `agents/pr-iterator.md`) refer to code that **no longer exists** in the tree. The runtime was removed by v1 PR1 of [`../../plans/shipped/github-tracker-backend.md`](../../plans/shipped/github-tracker-backend.md) (merge sha `b1d241bc`, 2026-05-21). The future [`../../plans/shipped/smatchet-merge-watcher.md`](../../plans/shipped/smatchet-merge-watcher.md) revives a subset of the underlying needs in a different (host-daemon) shape; concepts there are NOT identity-mapped to the deleted runtime. Entries here are preserved as historical record of what was tried.

<!-- Latest first. Append on archival. -->

# `postmortem-owed.sh`'s batched GraphQL fetch times out at the default window, and the failure reads as "last 20 merges clean"

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-10-04
- **Observed on**: PR #2286 (merged 2026-10-04T01:59:46Z past a red `Bucket-E UI tests (Mesa headless GL)`). PR #2280 (merged with `Plan-lock gate` red under `plan-lock-out-of-band`) was hidden by the same blind spot. Reproduced from worktree `gracious-kilby-73a454` at `c9479274d`, which is the `origin/develop` tip on 2026-10-04.
- **Status**: open

## What happened

At SessionStart, `postmortem-owed.sh --list` printed `postmortem-owed: no gate escapes owed a postmortem (last 20 merges clean).` and the `--nudge` hook stayed silent. Both were wrong: #2286 had merged past a red check earlier the same day.

Trigger 1+2 reads every merged PR's checks in a single batched call (the `gh pr list --state merged --limit "$FETCH_N" --json …statusCheckRollup` read into `ROWS` in `agent-layer/agents/scripts/core/postmortem-owed.sh`):

```bash
done < <(gh pr list --repo "$REPO" --base develop --state merged --limit "$FETCH_N" \
            --json number,labels,mergedAt,mergeCommit,statusCheckRollup --jq "$JQ_ROWS" 2>/dev/null \
            | tr -d '\r' || true)
```

`FETCH_N` defaults to `SCAN_N * 3` = 60 (the `FETCH_N=` assignment). Each develop PR now carries about 52 check runs, and at `--limit 60` the GraphQL query returns `HTTP 504: 504 Gateway Timeout (https://api.github.com/graphql)`. That reproduced 3 times out of 3 on 2026-10-04. `--limit 20`, `30` and `40` all succeed.

`2>/dev/null` drops the error and `|| true` drops the exit code, so `ROWS` stays empty. With no rows, every trigger-1/2 check is skipped. The script then reaches the success branch (the `merges clean` echo) and reports the 20-merge window as clean, even though it read 0 merges.

With the fetch inside the timeout, the same script on the same tree reports both owed escapes:

```text
$ POSTMORTEM_FETCH_N=30 bash agent-layer/agents/scripts/core/postmortem-owed.sh --list
postmortem owed: PR #2280 — red-check: Plan-lock gate; override: plan-lock-out-of-band
postmortem owed: PR #2286 — red-check: Bucket-E UI tests (Mesa headless GL)
```

## Why it matters

This script is the post-merge safety net for every merge path that skips the poller (see the companion entry, [`2026-10-04-native-auto-merge-merges-past-a-red-non-required-check.md`](infra/2026-10-04-native-auto-merge-merges-past-a-red-non-required-check.md)). When it fails, it says "clean", which is the one output that tells the reader nothing needs doing. Its header calls the default modes advisory and fail-open when `gh` is missing. But a missing `gh` prints a "skipped (advisory)" notice (the two `skipped (advisory)` echoes near the top of the script). A failed fetch prints a clean bill of health instead. The silence grows as rollups grow, so every new check context added to develop moves the detector further past the timeout. Nothing in the window was ever going to be caught.

## Concrete next action

1. **Fail loud, not clean.** Keep the fetch's exit status and stderr. If the fetch fails, or returns zero rows when the window should contain merges, print `postmortem-owed: merged-PR fetch failed (<first stderr line>) — window NOT scanned`. Print it on stderr in `--list` and `--nudge`, and exit non-zero in `--blocking`. Never fall through to the "merges clean" line. The `merges clean` echo should only be reachable when `${#ROWS[@]} -gt 0`.
2. **Fit the fetch under the timeout.** Page the window in chunks of at most 20, either with `gh api graphql --paginate` and `first: 20`, or with a loop of 20-PR `gh pr list --search "merged:<…"` calls. Alternatively, drop `statusCheckRollup` from the batch and read check runs per PR over REST (`commits/{sha}/check-runs`) only for the `SCAN_N` merges kept. Either way the cost scales with checks per PR, not the window size.
3. **Bats regression.** Run with a stub `gh` on `PATH` that exits 1 with `HTTP 504` for `pr list`. Assert the output does **not** contain `merges clean` and does contain `NOT scanned`. Also assert `--blocking` exits non-zero.

**Enumerator + replay**: the enumerator is the `gh pr list --state merged --limit $FETCH_N` row set (`ROWS`). Replayed on 2026-10-04 at the default `FETCH_N=60`, the call 504s and `ROWS` is empty, so action 1 prints `window NOT scanned` instead of `clean`. With action 2's 20-row chunks, the first chunk contains #2286 with `Bucket-E UI tests (Mesa headless GL)` as its curated red check, and it is flagged. That is the `POSTMORTEM_FETCH_N=30` output above.

Triggered-follow-up: when=pr-count:base=develop;since=2026-10-04;n=10; action=re-run postmortem-owed.sh --list at the default window and confirm it either scans or says NOT scanned, never a false clean; baseline=HTTP 504 at --limit 60 (3/3) and a false "last 20 merges clean" hiding #2280 + #2286, 2026-10-04; fired=2026-10-18
Follow-up check (2026-10-18): verified in code, live run not possible here. The 8 `fetch:` cases in the layer's `tests/bats/postmortem_owed.bats` pass: a 504 on every attempt, a non-transient failure and zero rows all print NOT scanned and never the clean line, and `--blocking` exits non-zero. A live `--list` in a cloud session had no gh auth and printed its advisory skip.
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — postmortem-owed.sh keeps the fetch's rc + first stderr line, retries a 5xx/timeout at a halved --limit (60→30→15), prints 'merged-PR fetch failed (…) — window NOT scanned' and exits 3 under --blocking; the clean line is reachable only when rows were read and reports the real count. Not done (optional): ≤20-per-page chunking, so a degraded retry scans fewer merges.
  Status: applied (2026-10-04)

- 2026-10-03 · deviation renewal (markers expiring 2026-12-31) · [debt] · P3 — the HTML and ADF Markdown engines each keep their own copy of the image-span and text-skip logic

Details:
md4c reports an image's alt text as ordinary text events between the image span's enter and leave
callbacks, so both engines collect it instead of emitting it. `MarkdownToHtml.cpp` and
`MarkdownToAdf.cpp` each repeat that logic:
- the `MD_SPAN_IMG` enter (push the src, raise the depth, clear the alt buffer);
- the leave tail (pop the src, clear the alt, lower the depth);
- the text-callback preamble (skip NUL chars, skip and log once on raw HTML under `MD_FLAG_NOHTML`,
  route text inside an image span into the alt buffer).

`dup_audit.py` reports these as clones, exempted by three markers in `MarkdownToHtml.cpp`. The markers
used to say that folding would couple two independent engines. It would not: both engines already
share `MarkdownConvert_Internal.h`, which holds their builder structs and `MdAttrToString`.

Concrete next action:
In `MarkdownConvert_Internal.h`:
- Add a `MdImageSpan` struct (depth, alt, src stack) and give both builders one in place of
  `imgSpanDepth` / `imgAltBuf` / `imgAltAccum` / `imgSrcStack`.
- Add inline helpers to enter an image span, pop its src, leave it, and absorb alt text, plus one for
  the NUL/raw-HTML skip that takes the engine name for its debug log.

Then move both engines onto the helpers and run the MarkdownConvert tests. Delete the three exemptions
(`revisit=2027-08-31`).

Applied 2026-10-03:
- Both builders now derive from `MdEngineState` (code-block depth, `MdImageSpan`, raw-HTML log flag).
- The engines call `EnterImageSpan` / `PopImageSrc` / `LeaveImageSpan`, `MdLinkHref` and
  `ConsumeNonEmittedMdText`, all in `MarkdownConvert_Internal.h`. Text events also build their string
  only after the line-break early returns.
- The three dated exemptions are gone. One `revisit=never` marker remains for the md4c callback
  skeleton (the end of the span switch and the start of the text callback), which both engines
  implement in the same order.
- A differential run of 16 image cases gives byte-identical HTML and ADF before and after. New
  `MarkdownToHtml: images` goldens pin the correct cases.
- The run also exposed three pre-existing alt-text bugs, filed as alexandrosk0/Smatchet#2284: empty
  `<em></em>` before an image, double-escaped entities, and nested images.

Status: applied
Last-reviewed: 2026-10-03

- 2026-10-03 · deviation renewal (markers expiring 2026-12-31) · [debt] · P3 — the AI provider clients each keep their own copy of the URL helpers and the chat messages build

Details:
`AnthropicClient.cpp`, `OllamaClient.cpp` and `OpenAiClient.cpp` each define the same anonymous-namespace
`JoinUrl` and a `ResolveBaseUrl` that differs only in its default base URL; OpenAI's also trims a
trailing `/v1`. Ollama and OpenAI also build the chat `messages` array the same way: an optional leading
`{role: system}` entry, then one entry per `History` item. `dup_audit.py` reports these as clones,
exempted by two markers in `OllamaClient.cpp` (`revisit=2027-05-31`). The same three files carry further
grandfathered, unmarked clones in their reachability probes and streaming loops.

The markers used to say that sharing these would couple independent provider adapters. It would not:
the three clients already share `AiErrorRedact.h` (`smatchet::ai::pure`) and `AiWireIntrospect.h`, and
the helpers are pure string and JSON work with no provider policy in them.

The per-provider wire-introspection wrappers (`<Provider>BuildChatBodyJson` / `<Provider>ResolveChatUrl`)
are not part of this. Each has to live in its own TU to reach that TU's anonymous-namespace builders, so
their markers are `revisit=never`.

Concrete next action:
- Add a pure header next to `AiErrorRedact.h` with `JoinUrl(base, path)`,
  `ResolveBaseUrlOr(cfg, defaultBase)` and `AppendChatMessages(json& messages, systemPrompt, history)`.
- Move the three clients onto it, keeping OpenAI's `/v1` trim at its call site.
- Cover the helpers with a bucket-A test, and keep the existing `ai.dump-request` wire tests green.
- Delete the two `OllamaClient.cpp` exemptions.

Resolution: applied 2026-10-06 (backlog-sweep-2026-10, PR #2296) — header-only Source/Core/include/AiWirePure.h (smatchet::ai::pure) holds JoinUrl (exactly one '/' between base and path), ResolveBaseUrlOr (trailing '/' trimmed; a configured malformed base never falls back to the default host), BuildChatMessages (system + history: Ollama, OpenAI) and BuildHistoryMessages (Anthropic); the three clients use it, OpenAI's /v1 trim stays at its call site, both OllamaClient duplication markers are gone. Request URLs are identical before/after across 55 base×endpoint cases except bases ending in '//', which now join correctly; wire bodies byte-identical. Covered by tests/Core/AiWirePure.test.cpp.
Status: applied (2026-10-06; was: open)
Last-reviewed: 2026-10-06

# dup_audit delta gate false-flags a clone in UNCHANGED files (winnow boundary drift)

**Category**: tooling · **Priority**: P2 · **Status**: applied (fixed in the N12 slice-3 PR)

## What happened

N12 slice-3 (a Tracker-only refactor) tripped `dup_audit.py --diff origin/develop` with:

```
[dup] FAIL CodeColorView.cpp:97 <-> CppSyntaxLex.cpp:113 — 74-token copy-paste clone
```

Neither file was in the diff. Probe of the module confirmed both files were **byte-identical**
at the merge-base and at HEAD, yet `find_clones` surfaced an *extra* maximal-clone boundary
`(97,113)` ntok 74 at HEAD that it did not surface at base `(98,124)` ntok 71. Winnowing is
corpus-sensitive: adding tokens in an unrelated changed file shifts which shingles seed the
extension for a pre-existing clone between two unchanged files, so the drifted boundary gets a
base-absent `content_hash` and slips past the hash-only grandfathering.

## Impact

A blocking DRY gate can fail a PR over duplication in files the author never touched, keyed on the
happenstance of what other files the diff perturbs. Every "gate, don't trust" property assumes the
gate flags the author's own new duplication; this violated it.

## Fix (applied)

`new_clones_vs` now grandfathers a clone whose **every** occurrence is in a file unchanged (by
normalized token stream) vs the base — you cannot duplicate code *into* a file without changing it,
so an all-unchanged-files clone is definitionally pre-existing, whatever boundary the winnow drift
selected. Sound: it cannot mask a genuinely-new clone (a new clone has ≥1 occurrence in a changed
file). Pinned by a `--selftest` case that stubs `find_clones` to emit a drifted base-absent hash for
an unchanged pair and asserts it stays grandfathered (fails on the old code, passes on the new).

## Preventing recurrence

The selftest locks the invariant. A stronger follow-up (not done — low value): make winnowing seed
selection deterministic w.r.t. corpus so boundaries don't drift at all; deferred as the guard fully
closes the false-positive class.
  Resolution: verified-in-tree 2026-10-04 (backlog-sweep-2026-10) — dup_audit.py grandfathers clones between files whose normalized token streams are unchanged at base and head (the boundary-drift case), with selftest coverage; nothing left to do.
  Status: applied (2026-10-04)

# The grid search box still inlines the Esc-clears-search block

- **Category**: debt
- **Priority**: P3
- **Date**: 2026-11-18
- **Observed on**: PR #2443 (input UX 4.4 action keys), DRY gate after its develop merge.
- **Status**: applied (2026-11-20; was: open)

## What happened

The settings search (`SmatchetPreferencesUi.cpp`, #2443) and the grid search (`SmatchetGridHeaderUi.cpp`, #2435) each had the same block that makes Esc clear the query and leave the box (clear the buffer, `ClearActiveID`, drop `InputTextDeactivatedState`). #2443 moved it to `SmatchetInput::ClearSearchOnEscape` and the settings search calls it, but `SmatchetGridHeaderUi.cpp` was held by plan-lock `pane-picker-jira-sites`, so the grid copy stayed inline and the helper carries a `SMATCHET_DEVIATION(rule=duplication)` marker (revisit 2027-01-31).

## Concrete next action

Once `pane-picker-jira-sites` is released, replace the inline block before `##GridFilter` in `SmatchetGridHeaderUi.cpp` with `SmatchetInput::ClearSearchOnEscape(pane.gridSearchInputId, pane.gridSearchBuf);` (add `#include "Ui/SmatchetInput.h"` if missing), then delete the deviation marker above `ClearSearchOnEscape` in `Source/Core/src/Ui/SmatchetInput.cpp`. The `GridSearch` bucket-E tests cover the Esc behaviour.

## Resolution

Applied 2026-11-20 (input UX Wave 2 lock deferrals, branch `claude/vigorous-khayyam-8e2d16`): the grid search box calls `SmatchetInput::ClearSearchOnEscape(pane.gridSearchInputId, pane.gridSearchBuf)` and the inline block is gone; the `SMATCHET_DEVIATION(rule=duplication)` marker above the helper is deleted. Behaviour unchanged, covered by the existing `GridSearch` bucket-E Esc cases.

- 2026-11-19 · input-ux-w2-action-keys · [debt] · P2 — Archive the `input-ux-w2-action-keys` plan (move to `shipped/` and add the INDEX row) once `docs/plans/INDEX.md` is free

Details:
PR #2443 (Wave 2 piece 4.4) left its plan under `docs/plans/active/` although the Implementation log, Deviations and
Verification (actual) are filled in. Plan-lock `pane-picker-jira-sites` (branch `claude/plus-dropdown-two-jira-261a42`)
holds `docs/plans/INDEX.md`, and both the pre-push gate and the CI `Plan-lock gate` refuse a diff that touches it, so
the index row and the `git mv` were taken out of the PR (owner instruction) instead of overriding the lock.

Concrete next action:
After #2443 merges and the lock is released: in a docs-only change, flip the plan's Status to `shipped`,
`git mv` it from `docs/plans/active/` into `docs/plans/shipped/` (same basename), delete its
`## Archive` block, run `bash agent-layer/agents/scripts/core/test-plan-index.sh --fix`, and ship it as a pure-docs PR.

Resolution: applied 2026-11-20 (input UX Wave 2 lock-deferrals PR, branch `claude/vigorous-khayyam-8e2d16`). The
plan's Status is `shipped`, it moved to `docs/plans/shipped/input-ux-w2-action-keys.md` with its `## Archive` block
deleted, and `test-plan-index.sh --fix` regenerated `docs/plans/INDEX.md`. It went in alongside the row-14 code it was
waiting on, not as a separate docs PR, so the plan's last open item and its archive land together.
Status: applied (2026-11-20; was: open)
Last-reviewed: 2026-11-20
