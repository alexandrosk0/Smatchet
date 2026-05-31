#!/usr/bin/env bash
# agents/scripts/core/merge-gates-prompt.sh
# ----------------------------------------------------------------------------
# `ask_user_question` shell shim.
#
# The merge-gates poller (`merge-gates.sh`) needs to halt for user input on
# certain return codes (1=blocked, 2=timeout, 3=gh-down, 4=PR-closed,
# 5=pagination-overflow). The real orchestrator wires this to the
# `AskUserQuestion` MCP tool. This shim provides:
#
#   1. A stubbable bash function `ask_user_question` that bats tests override
#      via $MERGE_GATES_TEST_ANSWER (canned answer for fixture-driven tests).
#   2. A fallback interactive path that prints options and reads stdin —
#      only fires when run from a real terminal without the test env var.
#
# The orchestrator-side wiring (real MCP `AskUserQuestion` invocation) is
# orchestrator prose, not bash — this script only declares the contract.
# ----------------------------------------------------------------------------

# Inputs:
#   $1 — prompt label (free text)
#   $2..$N — option strings (one per option)
# Output (stdout):
#   The user's selected option string, verbatim.
# Exit:
#   0 — selection made
#   1 — empty/invalid selection
ask_user_question() {
    local label="${1:?ask_user_question: label required}"
    shift
    local options=("$@")

    if [ "${#options[@]}" -eq 0 ]; then
        echo "ask_user_question: at least one option required" >&2
        return 1
    fi

    # Test path: canned answer overrides everything.
    if [ -n "${MERGE_GATES_TEST_ANSWER:-}" ]; then
        printf '%s\n' "$MERGE_GATES_TEST_ANSWER"
        return 0
    fi

    # Interactive fallback — refuse when stdin is not a TTY, otherwise `read`
    # blocks automation indefinitely (CI, bats without canned answer, headless
    # orchestrator). Tests set MERGE_GATES_TEST_ANSWER for canned answers.
    if [ ! -t 0 ]; then
        echo "ask_user_question: stdin is not a TTY; set MERGE_GATES_TEST_ANSWER for non-interactive runs" >&2
        return 1
    fi
    printf '%s\n' "$label" >&2
    local i=1
    for opt in "${options[@]}"; do
        printf '  %d) %s\n' "$i" "$opt" >&2
        i=$((i+1))
    done
    printf 'Choice [1-%d]: ' "${#options[@]}" >&2
    local choice
    read -r choice
    if ! [[ "$choice" =~ ^[0-9]+$ ]] || [ "$choice" -lt 1 ] || [ "$choice" -gt "${#options[@]}" ]; then
        echo "ask_user_question: invalid choice '$choice'" >&2
        return 1
    fi
    printf '%s\n' "${options[$((choice-1))]}"
}
