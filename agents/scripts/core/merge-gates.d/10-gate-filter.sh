#!/usr/bin/env bash
# agents/scripts/core/merge-gates.d/10-gate-filter.sh
# ----------------------------------------------------------------------------
# The one `gh api graphql --jq` GATE_FILTER program — the 31-field projection
# that turns the GraphQL response into the fixed-order field stream the poll
# loop reads with `mapfile`. Relocated VERBATIM from merge-gates.sh (the former
# in-function `GATE_FILTER='...'` literal) into a single-quoted global so the
# entry point copies it byte-for-byte (no command-substitution newline trim).
# The three placeholders (__ORCH_USER__, __REQUIRED_CONTEXTS__,
# __BLOCK_ALLOWLIST_RE__) stay UNsubstituted here; poll_merge_gates splices them
# after copying, exactly as before. Field-order map lives at the poll loop's
# `local GATE_FILTER` comment. No logic change — pure relocation (bats is the net).
# ----------------------------------------------------------------------------

# shellcheck disable=SC2016  # single-quoted jq literal — $-refs are jq vars, not bash
_MG_GATE_FILTER_TEMPLATE='
.data.repository.pullRequest as $pr
| ($pr.headRefOid // "") as $sha
| ([$pr.labels.nodes[]?.name]) as $labels
# selfImpOnly — TRUE iff the PR diff is ENTIRELY under docs/self-improvement/**
# (the self-improvement backlog / postmortem ledger). Such a PR auto-skips the
# CR + Bugbot gates (user ask 2026-06-20; plan self-improvement-pr-review-exemption).
# Fail-safe to FALSE on any uncertainty: an empty file list (length 0, the vacuous
# all() guarded out), a >100-file page (pageInfo.hasNextPage, cannot see every
# path), or absent files (legacy fixtures) all yield FALSE for full gates. Computed
# here; consumed by the Bugbot bucket + the CR belt-and-suspenders downgrade below.
# (NB: no apostrophes in this single-quoted jq filter string.)
| (($pr.files.nodes // []) | map(.path)) as $changedPaths
| (($pr.files.pageInfo.hasNextPage // false)) as $filesOverflow
| (($changedPaths | length) > 0
   and ($filesOverflow | not)
   and ($changedPaths | all(startswith("docs/self-improvement/")))) as $selfImpOnly
| ($labels | any(. == "tests-out-of-band")) as $tests
| ($labels | any(. == "perf-out-of-band")) as $perf
| ($labels | any(. == "intent-out-of-band")) as $intent
| ($labels | any(. == "plan-lock-out-of-band")) as $planlock
| ($labels | any(. == "cr-out-of-band")) as $cr
# crDisposition — an explicit operator attestation supplied EITHER as a label
# prefixed `cr-disposition:` (e.g. `cr-disposition:rate-limit-acked`) OR as a
# grep-able `cr-disposition:<reason>` marker line in the PR BODY. Whenever
# `cr-out-of-band` would downgrade a CR block, this disposition is REQUIRED —
# `cr-out-of-band` alone is NOT honoured (PR-3 cr-out-of-band-disposition-trail,
# generalising the PR-2 cr-rate-limit-code-pr-auto-pause requirement to EVERY
# cr-out-of-band downgrade): it proves the operator consciously waived CR review
# with a recorded reason rather than reflexively slapping a generic override on.
# Body match: `cr-disposition:` followed by any non-empty reason on the line
# (regex tolerates leading whitespace / list markers). (NB: no apostrophes in
# this single-quoted jq filter string.)
| (($labels | any(startswith("cr-disposition:")))
   or (($pr.body // "") | test("cr-disposition:[[:space:]]*[^[:space:]]"; "i"))) as $crdisposition
| ($labels | any(. == "bugbot-out-of-band")) as $bb
| ((($pr.commits.nodes[0].commit.statusCheckRollup.contexts.nodes) // [])
   | map(. + {_k: (if .__typename == "CheckRun" then ["CheckRun", (.name // "")]
                   else ["StatusContext", (.context // "")] end)})
   | group_by(._k) | map(sort_by(.startedAt // "") | .[-1]) | map(del(._k))) as $ctx
| ([$ctx[] | select(.isRequired == true)]) as $req
# $blocking — the set the gate must wait on: REQUIRED contexts PLUS every
# non-required, non-advisory-named check (block-on-any-red — the allow-list
# regex now matches all names). The $failing set below already unions these
# (the #923 fix), but the PENDING count historically counted only $req — so a
# non-required blocking check still IN_PROGRESS (not yet terminal) was
# invisible: not failing (not terminal) and not pending (not required) →
# GATES_PASSED fired before ASAN/Coverage/Bucket finished and the merge beat
# the sanitizer to the line (#1237/#1232/#1227/#1220/#1198 ASAN/Coverage
# escapes). Counting $blocking (not $req) for pending closes it.
| ([$ctx[] | select(
      (.isRequired == true)
      or ((if .__typename == "CheckRun" then (.name // "") else (.context // "") end)
          | (test("__BLOCK_ALLOWLIST_RE__"; "i")
             and (ascii_downcase | contains("advisory") | not))))]) as $blocking
| (__REQUIRED_CONTEXTS__) as $reqNames
| ([$ctx[] | (if .__typename == "CheckRun" then (.name // "") else (.context // "") end)]) as $ctxNames
| ([$reqNames[] | select(. as $n | ($ctxNames | any(. == $n)) | not)]) as $reqAbsent
| ([$ctx[] | select(
      ((.__typename == "CheckRun" and .status == "COMPLETED" and ((.conclusion // "") | IN("FAILURE","TIMED_OUT","CANCELLED","ACTION_REQUIRED","STARTUP_FAILURE"))) or
       (.__typename == "StatusContext" and ((.state // "") | IN("FAILURE","ERROR"))))
      and
      # A failing check blocks if it is REQUIRED (unchanged), OR it is ANY
      # non-required check whose name does not contain "advisory"
      # (block-on-any-red — the spliced regex matches every name; see the
      # MERGE_GATES_BLOCK_ALLOWLIST_RE comment for the full rationale + the
      # curated-era history: #923 Coverage escape closed 2026-06-06; Perf
      # PR-fast 2026-06-07; Android security gate 2026-06-09 — the advisory
      # mobile jobs let a green develop ship mobile breakage #1021/#1064; Fuzz
      # smoke 2026-06-16 — #1301 merged past a RED fuzz-smoke whose libFuzzer
      # driver failed to compile, paired with the continue-on-error guard on
      # the stochastic fuzz STEP so only the deterministic build/ctest reds the
      # check). The curated list grew one gate-escape at a time; the flip
      # retires the curation so the next escape class is impossible by
      # construction. A red on an "advisory"-named check (none exist post-
      # rename; the token is the sanctioned future escape) still passes.
      ((.isRequired == true)
       or ((if .__typename == "CheckRun" then (.name // "") else (.context // "") end)
           | (test("__BLOCK_ALLOWLIST_RE__"; "i")
              and (ascii_downcase | contains("advisory") | not)))))]) as $failing
| ([$failing[] | select(
      ($tests and .__typename == "CheckRun" and .name == "Test-delta gate") or
      ($perf  and .__typename == "CheckRun" and ((.name // "") | startswith("Perf PR-fast"))) or
      ($intent and .__typename == "CheckRun" and .name == "Intent section") or
      ($planlock and .__typename == "CheckRun" and .name == "Plan-lock gate"))]) as $downgraded
| ([$pr.reviews.nodes[] | select(.author.login == "coderabbitai" or .author.login == "coderabbitai[bot]")]) as $crall
| (if ($crall | length) == 0 then "NONE"
   else (([$crall[] | select(.commit.oid == $sha)]) as $cur
         | if ($cur | length) == 0 then "STALE" else ($cur | sort_by(.submittedAt) | .[-1].state) end) end) as $crstate
| (if ($crall | length) == 0 then ""
   else (([$crall[] | select(.commit.oid == $sha)]) as $cur
         | if ($cur | length) > 0 then ($cur | sort_by(.submittedAt) | .[-1].body // "")
           else ($crall | sort_by(.submittedAt) | .[-1].body // "") end) end) as $crbody
| ([$pr.comments.nodes[]?
    | select(.author.login == "coderabbitai" or .author.login == "coderabbitai[bot]")
    | (.body // "")]
   | any(
       contains("skip review by coderabbit.ai")
       or (test("##[[:space:]]*Review skipped"; "i") and (ascii_downcase | contains("too many files"))))) as $crskip
# crReviewSkipped — the GENERIC terminal "Review skipped" (docs-only /
# path-filtered / trivial diff per .coderabbit.yaml): the "CodeRabbit"
# StatusContext is SUCCESS and its description says "Review skipped" WITHOUT
# "too many files" (the size-skip variant, handled by $crskip + the NONE size
# branch). This is TERMINAL — CR processed the PR and declined an incremental
# review, so no inline review will ever land and the NONE+status-SUCCESS grace
# must NOT wait it out (PR #976 burned ~10 cycles on a docs-only PR). Distinct
# from $crskip (a too-many-files BLOCK read from a CONVERSATION COMMENT); this
# reads the STATUS description and is a PASS signal.
| ([$pr.commits.nodes[0].commit.statusCheckRollup.contexts.nodes[]?
    | select(.__typename == "StatusContext" and .context == "CodeRabbit"
             and (.state == "SUCCESS"))
    | (.description // "")]
   | any((test("review skipped"; "i"))
         and ((test("too many files"; "i")) | not))) as $crreviewskipped
# crRateLimited — CR declined / deferred the review because it hit its rate
# limit (PR-2 cr-review-skipped-pure-docs-auto-downgrade /
# cr-rate-limit-code-pr-auto-pause). Distinct from $crreviewskipped (a clean
# path-filtered/docs skip, a PASS) and $crskip (the too-many-files size skip):
# a rate-limit skip is TEMPORARY — CR will re-review once its quota recovers, so
# it must NOT be treated as a terminal pass on a CODE PR. Read from BOTH the CR
# conversation-comment bodies AND the "CodeRabbit" StatusContext description
# (CR surfaces the rate-limit on either surface). Regex tolerates "rate limit",
# "rate-limited", "rate limited", and the common "try again later" phrasing.
# (NB: no apostrophes in this single-quoted jq filter string.)
| (([$pr.comments.nodes[]?
     | select(.author.login == "coderabbitai" or .author.login == "coderabbitai[bot]")
     | (.body // "")]
    + [$pr.commits.nodes[0].commit.statusCheckRollup.contexts.nodes[]?
       | select(.__typename == "StatusContext" and .context == "CodeRabbit")
       | (.description // "")])
   | any(test("rate.?limit"; "i") or test("try again later"; "i"))) as $crratelimited
# pureDocs — the PR diff is strictly within the is-pure-docs-diff.sh allow-list
# (docs/ , backlog/ , agents/scripts/ , or any *.md ANYWHERE). Mirrors that
# script over the PR file list so the poller can apply the IDENTICAL pure-docs
# verdict without a local checkout. Used by the rate-limit auto-downgrade
# (deliverable 1): a rate-limit skip on a pure-docs PR is harmless to fast-pass
# (markdown is never compiled), while a rate-limit skip on a CODE PR must pause /
# require an explicit disposition (deliverable 2). Fail-safe FALSE on an empty
# file list, a >100-file page (cannot see every path), or absent files.
| (($changedPaths | length) > 0
   and ($filesOverflow | not)
   and ($changedPaths | all(test("^(docs/|backlog/|agents/scripts/|.*[.]md$)")))) as $pureDocs
# Bugbot (cursor[bot]) — mirrors the $crall/$crstate machinery. $bball = all
# reviews authored by cursor[bot] (its summary review, always state COMMENTED).
# $bbterminal = TRUE when a cursor[bot] CONVERSATION (issue) comment body carries
# a couldn.t-run / usage-limit status message. The regex uses couldn.t (a jq `.`
# wildcard, NOT a literal apostrophe) so it (a) stays inside the bash
# single-quoted filter — a literal apostrophe here would close the string — and
# (b) tolerates a straight or unicode apostrophe. $bbstate folds the head-review
# state, the terminal signal, the engaged-but-stale signal, and the no-activity
# signal into one token — decision order is load-bearing (see the bash bucket):
# a head review wins, else TERMINAL (no-wedge), else STALE (prior-commit review
# → grace), else ABSENT (Bugbot-free / not engaged → never waited on).
| ([$pr.reviews.nodes[] | select(.author.login == "cursor" or .author.login == "cursor[bot]")]) as $bball
| ([$pr.comments.nodes[]?
    | select(.author.login == "cursor" or .author.login == "cursor[bot]")
    | (.body // "")]
   | any(test("couldn.t run"; "i") or test("usage limit"; "i"))) as $bbterminal
| (([$bball[] | select(.commit.oid == $sha)]) as $bbcur
   | if ($bbcur | length) > 0 then ($bbcur | sort_by(.submittedAt) | .[-1].state)
     elif $bbterminal then "TERMINAL"
     elif ($bball | length) > 0 then "STALE"
     else "ABSENT" end) as $bbstate
| (
    ($pr.state // "UNKNOWN"),
    $sha,
    (((($pr.commits.nodes[0].commit.statusCheckRollup.contexts.pageInfo.hasNextPage // false)
       or ($pr.reviews.pageInfo.hasNextPage // false)
       or ($pr.reviewThreads.pageInfo.hasNextPage // false)
       or ($pr.comments.pageInfo.hasNextPage // false)
       or ($pr.labels.pageInfo.hasNextPage // false)
       or (any($pr.reviewThreads.nodes[]?; .comments.pageInfo.hasNextPage // false)))) | tostring),
    ($tests | tostring),
    ($perf | tostring),
    ($req | length),
    ([$failing[] | select(. as $f | ($downgraded | any(.name == $f.name and .__typename == $f.__typename)) | not)] | length),
    ([$blocking[] | select(
        (.__typename == "CheckRun" and .status != "COMPLETED") or
        (.__typename == "StatusContext" and ((.state // "") | IN("PENDING","EXPECTED"))))] | length),
    ($downgraded | length),
    ([$downgraded[].name] | join(", ")),
    $crstate,
    (($crbody | split("\n"))[0] // ""),
    ([$pr.reviewThreads.nodes[] | select(.isResolved == false and .isOutdated == false
        and any(.comments.nodes[]; .author.login == "coderabbitai" or .author.login == "coderabbitai[bot]"))] | length),
    ([$pr.commits.nodes[0].commit.statusCheckRollup.contexts.nodes[]?
      | if (.__typename == "StatusContext" and .context == "CodeRabbit") then .state
        elif (.__typename == "CheckRun"
              and ((.name == "CodeRabbit") or (.name == "CR findings (0 actionable)"))
              and (.conclusion != null))
          then (if ((.conclusion) | IN("SUCCESS","NEUTRAL","SKIPPED")) then "SUCCESS" else .conclusion end)
        else empty end] | (.[0] // "ABSENT")),
    ([$pr.reviewThreads.nodes[]? | .comments.nodes[]?
      | select((.author.login == "coderabbitai" or .author.login == "coderabbitai[bot]") and (.commit.oid // "") == $sha)] | length),
    (([$pr.comments.nodes[] | select(.author.__typename != "Bot" and ((.author.login // "") | ascii_downcase) != ("__ORCH_USER__" | ascii_downcase))] | length)
     + ([$pr.reviewThreads.nodes[] | select(.isResolved == false and .isOutdated == false
          and any(.comments.nodes[]; .author.__typename != "Bot" and ((.author.login // "") | ascii_downcase) != ("__ORCH_USER__" | ascii_downcase)))] | length)),
    ($pr.reviewDecision // "NONE"),
    ($pr.mergeStateStatus // "UNKNOWN"),
    ($cr | tostring),
    ($crskip | tostring),
    (if ([$pr.commits.nodes[0].commit.statusCheckRollup.contexts.nodes[]?
          | select((.__typename == "StatusContext" and .context == "CodeRabbit")
                or (.__typename == "CheckRun"
                    and ((.name == "CodeRabbit") or (.name == "CR findings (0 actionable)"))))] | length) > 0
     then 1 else 0 end),
    ($reqAbsent | join(", ")),
    ($reqAbsent | length),
    ($crreviewskipped | tostring),
    $bbstate,
    ([$pr.reviewThreads.nodes[] | select(.isResolved == false and .isOutdated == false
        and any(.comments.nodes[]; .author.login == "cursor" or .author.login == "cursor[bot]"))] | length),
    ($bb | tostring),
    ($selfImpOnly | tostring),
    ($pureDocs | tostring),
    ($crratelimited | tostring),
    ($crdisposition | tostring)
  )
'
