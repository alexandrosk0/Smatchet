#!/usr/bin/env bash
# 75-pr-comments.sh — pr-numbered-temporal-comments rule (advisory) (sourced by test-lint-rules.sh, not run directly).

# pr-numbered-temporal-comments — WARN-first over first-party C++ comments. A comment that pins a
# DEVELOPMENT pull-request NUMBER (`// PR 5`, `// PR #1104`, `PR#1218`, `PR12`, `PRn`) is a temporal
# scaffold: it documents which PR introduced a change rather than the durable intent, and rots the
# moment the PR is squash-merged + the number forgotten. Rewrite each to present-tense intent (drop
# the PR-number token; keep the technical meaning). This gate stops the pattern re-accumulating after
# the one-shot sweep.
#
# A bash linter cannot tell a dev-PR token from the GitHub PRODUCT term "pull request" by meaning, so
# the regex is deliberately narrow — it fires ONLY on `PR` immediately followed by an OPTIONAL space,
# an OPTIONAL `#`, then a DIGIT (the dev-PR-number shape). Product-domain usages that carry no number
# ("PR-only columns", "type:pr", "per-PR enrichment", "[PR] prefix", "tell PRs apart") do NOT match.
# GitHub Issue refs (`#1081`), ADR refs (`ADR-0012`) and commit hashes never match (no `PR` prefix).
#
# WARN-FIRST / ADVISORY (calibration phase, mirrors the bare-json + unused-symbol + interface-doc
# gates; never touches $rc). Diff-scoped to the CHANGED first-party C++ files (the sweep cleaned the
# tree, so a whole-tree scan would be quiet today, but diff-scoping keeps it fast + focuses the WARN
# on newly-introduced text). A `// SMATCHET_DEVIATION(rule=pr-numbered-temporal-comments; ...)` on the
# line above suppresses (rare — e.g. a comment that must cite a specific historical PR for an audit).
# The whole-tree set is available via --scan-pr-comments for a campaign sweep.
PR_COMMENT_RE='\bPR ?#?[0-9]|\bPR[0-9]'

scan_pr_comment_file() {
    # $1 = a first-party .cpp/.h/.hpp file. Emits `pr-numbered-temporal-comments\t<f>:<line>` per
    # COMMENT line carrying a dev-PR-number token. Only comment lines fire (code that happens to
    # contain a `PR<digit>` token in a string/identifier is out of scope — this is a comment rule).
    local f="$1"
    [ -f "$f" ] || return 0
    case "$f" in *.cpp|*.h|*.hpp) ;; *) return 0 ;; esac
    local lineno=0 prev_dev_rule=""
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno+1))
        # A SMATCHET_DEVIATION on the line ABOVE suppresses the next non-blank line.
        if [[ "$line" =~ $DEV_RE ]]; then
            local body="${BASH_REMATCH[1]}" kv
            prev_dev_rule=""
            IFS=';' read -ra kvs <<< "$body"
            for kv in "${kvs[@]}"; do kv="${kv# }"; case "$kv" in rule=*) prev_dev_rule="${kv#rule=}" ;; esac; done
            continue
        fi
        if [[ "$line" =~ ^[[:space:]]*$ ]]; then continue; fi
        local suppress="$prev_dev_rule"; prev_dev_rule=""
        # Only consider COMMENT lines: a `//` line-comment, or a `*` / `/*` block-comment body.
        case "$line" in
            *'//'*|*'/*'*) ;;
            *'*'*) [[ "$line" =~ ^[[:space:]]*\* ]] || continue ;;
            *) continue ;;
        esac
        if [[ "$line" =~ $PR_COMMENT_RE ]]; then
            if [ "$suppress" != "pr-numbered-temporal-comments" ]; then
                printf 'pr-numbered-temporal-comments\t%s:%s\n' "$f" "$lineno"
            fi
        fi
    done < "$f"
}

compute_pr_comment_violations() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files \
            'Source/Core/**' 'Source/Plugins/**' 'Source/Standalone/**' 'Source/UnrealPlugins/**' \
            2>/dev/null \
            | grep -E '\.(cpp|h|hpp)$' | grep -vE '(^|/)ThirdParty/' || true
    )
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_pr_comment_file "$f"; done
}
