#!/usr/bin/env bash
# check-pr-intent.sh — validate a PR body has a filled `## Intent` section BEFORE
# opening the PR out-of-band (GitHub MCP / REST `create_pull_request`), so the body
# can't ship without Intent and red the required-adjacent `Intent section` CI check
# (postmortems 2026-06-20 #1438: an API-opened PR with no `## Intent` reded the gate;
# the gate self-heals on a body edit, but the cheaper path is to never ship the miss).
# Local mirror of `.github/workflows/doc-validation.yml` job `Intent section` — keep
# the regex in sync with that job.
#
# Usage:
#   bash check-pr-intent.sh <body-file>     # validate a file
#   bash check-pr-intent.sh < body.txt      # or stdin
#   bash check-pr-intent.sh --selftest
#
# Exit: 0 = `## Intent` present + filled; 1 = missing/empty; 2 = usage / no python3.
#
# selftest: asserts-failure
set -euo pipefail

command -v python3 >/dev/null 2>&1 || { echo "check-pr-intent: python3 required" >&2; exit 2; }

# _check <body-text> — mirrors the doc-validation.yml `Intent section` python.
# Body goes through the ENV (never an inline arg into the python source) so a
# backtick / $() in a title can't re-parse — same discipline as the CI job.
_check() {
    PR_BODY="$1" python3 - <<'PY'
import os, re, sys
body = os.environ.get("PR_BODY", "") or ""
m = re.search(r'(?ms)^##\s+Intent\s*$(.*?)(?=^##\s|\Z)', body)
if not m:
    print("check-pr-intent: MISSING `## Intent` section. Add a one-line redacted "
          "statement of the originating ask (docs/agent-rules/ship-loops.md "
          "§ Intent capture), or apply the 'intent-out-of-band' label.", file=sys.stderr)
    sys.exit(1)
section = re.sub(r'(?s)<!--.*?-->', '', m.group(1)).strip()
if not section:
    print("check-pr-intent: EMPTY `## Intent` (only the template placeholder). "
          "Fill it with the originating ask.", file=sys.stderr)
    sys.exit(1)
# Pre-first-push review verdict — the review step otherwise leaves no trace
# (pre-first-push-review-step-is-unenforced-and-was-skipped). Comments are
# stripped FIRST so the template's commented placeholder cannot satisfy it.
# This proves a claim was RECORDED, never that the review ran — the entry is
# explicit that a check can do no more, which is why it lives in the body lint
# and not the pre-push hook. KEEP IN SYNC with doc-validation.yml `Intent section`.
stripped = re.sub(r'(?s)<!--.*?-->', '', body)
verdict_re = re.compile(
    r'(?mi)^\s*(?:[-*+]\s+)?(?:\[[ xX]\]\s+)?[*_`]*adversarial-code-review[*_`]*\s*:[*_]*\s*'
    r'(?:\d+\s+findings?\b(?:[\s,:;—–-]*(?![<,\s:;—–-])\S.*)?|n/a\s*[—–-]+\s*(?!<)\S.*)[ \t]*\r?$')
if not verdict_re.search(stripped):
    print("check-pr-intent: MISSING review verdict. Add a line "
          "`adversarial-code-review: N findings, <disposition>` or "
          "`adversarial-code-review: n/a — <reason>` "
          "(ship-loops.md § [pre-first-push gate] item 5).", file=sys.stderr)
    sys.exit(1)
print("check-pr-intent: OK — `## Intent` present and filled; review verdict recorded.")
PY
}

run_selftest() {
    local rc=0
    # Failure path: a body with no `## Intent` MUST be rejected.
    _check "## Summary"$'\n\n'"no intent here" >/dev/null 2>&1 || rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "check-pr-intent --selftest: FAIL — did not block a body missing ## Intent" >&2
        return 1
    fi
    # Pass path: a filled `## Intent` + a findings-form verdict MUST be accepted.
    rc=0
    _check "## Intent"$'\n\n'"Fix the thing the user asked for."$'\n\n'"adversarial-code-review: 4 findings, all fixed" >/dev/null 2>&1 || rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "check-pr-intent --selftest: FAIL — blocked a body WITH intent + findings verdict" >&2
        return 1
    fi
    # Pass path: the n/a escape form MUST be accepted.
    rc=0
    _check "## Intent"$'\n\n'"Fix it."$'\n\n'"- adversarial-code-review: n/a — trivial one-line doc fix" >/dev/null 2>&1 || rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "check-pr-intent --selftest: FAIL — blocked the n/a verdict escape" >&2
        return 1
    fi
    # Empty-section path (template placeholder only) MUST be rejected.
    rc=0
    _check "## Intent"$'\n\n'"<!-- placeholder -->" >/dev/null 2>&1 || rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "check-pr-intent --selftest: FAIL — did not block an empty ## Intent" >&2
        return 1
    fi
    # Verdict-missing path: intent filled but NO verdict line MUST be rejected,
    # and for the RIGHT reason — a bare non-zero would also be produced by a
    # python error or a broken Intent regex, so assert the message names it.
    local out
    out="$(_check "## Intent"$'\n\n'"Fix it, no review recorded." 2>&1)" && {
        echo "check-pr-intent --selftest: FAIL — did not block a body with no review verdict" >&2
        return 1
    }
    case "$out" in
        *"MISSING review verdict"*) ;;
        *) echo "check-pr-intent --selftest: FAIL — verdict-missing body rejected for the wrong reason:" >&2
           printf '%s\n' "$out" | sed 's/^/    /' >&2
           return 1 ;;
    esac
    # A verdict living ONLY inside an HTML comment (the untouched template) MUST
    # be rejected — comments are stripped before the search. The comment is
    # MULTI-LINE deliberately: on a single-line `<!-- adversarial... -->` the
    # `<!--` prefix already breaks the line-start match, so that shape rejects
    # even WITHOUT the strip and proves nothing about it (found by mutation:
    # `stripped = body` stayed green against the single-line form). Inside a
    # multi-line comment the verdict line stands alone and WOULD match raw.
    out="$(_check "## Intent"$'\n\n'"Fix it."$'\n\n'"<!--"$'\n'"adversarial-code-review: 0 findings"$'\n'"-->" 2>&1)" && {
        echo "check-pr-intent --selftest: FAIL — a commented-out verdict satisfied the check" >&2
        return 1
    }
    case "$out" in
        *"MISSING review verdict"*) ;;
        *) echo "check-pr-intent --selftest: FAIL — commented-verdict body rejected for the wrong reason:" >&2
           printf '%s\n' "$out" | sed 's/^/    /' >&2
           return 1 ;;
    esac
    # The template's n/a line uncommented but NOT filled in — the reason is still
    # the literal `<reason the diff is trivial>` placeholder — MUST be rejected.
    # Without the `(?!<)` guard this false-passed: `\S.*` happily accepted `<r…`,
    # so following the template's instructions halfway earned a green verdict.
    out="$(_check "## Intent"$'\n\n'"Fix it."$'\n\n'"adversarial-code-review: n/a — <reason the diff is trivial>" 2>&1)" && {
        echo "check-pr-intent --selftest: FAIL — an unfilled n/a placeholder satisfied the check" >&2
        return 1
    }
    case "$out" in
        *"MISSING review verdict"*) ;;
        *) echo "check-pr-intent --selftest: FAIL — placeholder n/a rejected for the wrong reason:" >&2
           printf '%s\n' "$out" | sed 's/^/    /' >&2
           return 1 ;;
    esac
    # The FINDINGS branch has the same placeholder hole: a count filled in but
    # the disposition left as the literal `<disposition>` template MUST be
    # rejected. The naive guard `[,:—–-]*\s*(?!<)\S` still PASSES this via
    # backtracking — the separator class gives back the comma, `\S` matches the
    # comma itself, and the lookahead never sees the `<`. The separator class
    # and the disposition-start class must be DISJOINT so backtracking cannot
    # reassign the comma. (CodeRabbit's committable suggestion had exactly the
    # backtracking hole; verified before adopting the disjoint form.)
    out="$(_check "## Intent"$'\n\n'"Fix it."$'\n\n'"adversarial-code-review: 0 findings, <disposition>" 2>&1)" && {
        echo "check-pr-intent --selftest: FAIL — an unfilled findings-disposition placeholder satisfied the check" >&2
        return 1
    }
    case "$out" in
        *"MISSING review verdict"*) ;;
        *) echo "check-pr-intent --selftest: FAIL — placeholder findings-form rejected for the wrong reason:" >&2
           printf '%s\n' "$out" | sed 's/^/    /' >&2
           return 1 ;;
    esac
    # A bare count with NO disposition stays accepted — `0 findings` is a
    # complete verdict; the guard's job is placeholders, not prose quality.
    rc=0
    _check "## Intent"$'\n\n'"Fix it."$'\n\n'"adversarial-code-review: 0 findings" >/dev/null 2>&1 || rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "check-pr-intent --selftest: FAIL — blocked a bare-count verdict (0 findings)" >&2
        return 1
    fi
    # ... including with TRAILING whitespace (Markdown hard-break spaces, a
    # stray tab, or a CR from a CRLF body). Invisible to the author, so
    # rejecting it produces an undebuggable red; `[ \t]*\r?` before `$` eats it.
    rc=0
    _check "## Intent"$'\n\n'"Fix it."$'\n\n'"adversarial-code-review: 0 findings  " >/dev/null 2>&1 || rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "check-pr-intent --selftest: FAIL — blocked a bare-count verdict with trailing whitespace" >&2
        return 1
    fi
    echo "check-pr-intent --selftest: PASS"
}

case "${1:-}" in
    --selftest) run_selftest; exit $? ;;
    --help | -h) sed -n '2,12p' "$0"; exit 0 ;;
    "") BODY="$(cat)" ;;
    *)
        [ -f "$1" ] || { echo "check-pr-intent: no such file: $1" >&2; exit 2; }
        BODY="$(cat "$1")"
        ;;
esac
_check "$BODY"
