#!/usr/bin/env bash
# PreToolUse hook (template): warn when the Edit/Write tool is about to
# modify a file currently held under an exclusive Perforce `+l` lock by a
# different client.
#
# Always deployed by `setup-harness.sh` into `.claude/hooks/` and registered on
# the PreToolUse Edit|Write|MultiEdit matcher, but SELF-GATED: the first line of
# the body exits 0 immediately unless `SMATCHET_AGENT_VCS=p4`, so it is inert in
# the default git mode and only does work when the session opts into p4.
# `.claude/` is gitignored; the template here is what survives commit.
#
# This is a WARNING, not a hard block — the hook exits 0 even when a
# foreign lock is detected so agents can ignore for emergency fixes.
# Detection is logged to stderr (Claude Code surfaces hook stderr to the
# session). Set SMATCHET_P4_LOCK_HOOK_BLOCK=1 to upgrade warning → block
# (exit 2 to refuse the Edit).
#
# Phase 5 of `docs/plans/shipped/git-to-perforce-migration.md`.
#
# Triggered for: Edit, Write, MultiEdit (any tool that writes a file).
# Skipped when:
#   - SMATCHET_AGENT_VCS != p4 (opt-out by default)
#   - p4 CLI not on PATH (no-op rather than fail)
#   - target file is not inside the canonical client root
#   - target file is not tracked by p4 (a brand-new file can't be +l-held)
#   - exclusive lock IS held by the current client (we hold it; carry on)

set -u

# --- Opt-out: hook only fires when the session is in dual-VCS p4 mode ----
[ "${SMATCHET_AGENT_VCS:-git}" = "p4" ] || exit 0

# --- Skip if p4 CLI is unreachable (no-op rather than fail) --------------
P4_BIN="${P4_BIN:-p4}"
command -v "$P4_BIN" >/dev/null 2>&1 || exit 0

# --- Skip if env not configured ------------------------------------------
[ -n "${P4PORT:-}" ] || exit 0
[ -n "${P4USER:-}" ] || exit 0

# --- Read tool-input JSON from stdin -------------------------------------
INPUT="$(cat || true)"
[ -z "$INPUT" ] && exit 0

if command -v jq >/dev/null 2>&1; then
    FILE_PATH="$(printf '%s' "$INPUT" | jq -r '.tool_input.file_path // empty')"
else
    FILE_PATH="$(printf '%s' "$INPUT" | sed -n 's/.*"file_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)"
fi
[ -z "$FILE_PATH" ] && exit 0
[ -f "$FILE_PATH" ] || exit 0

# --- Probe the file's p4 status ------------------------------------------
# `p4 opened -a <file>` lists all clients that have it open. With -a we see
# every other client too — that's what surfaces a foreign +l lock.
opened_out=$("$P4_BIN" opened -a "$FILE_PATH" 2>&1 || true)

# No output = file not opened by anyone (no lock to warn about).
[ -z "$opened_out" ] && exit 0

# Skip the "file(s) not opened anywhere" sentinel.
echo "$opened_out" | grep -qiE 'not opened|no such file' && exit 0

# Look for exclusive opens by clients OTHER than ours.
# `p4 opened -a` output shape, exclusive line example:
#   //smatchet/main/foo.cpp#3 - edit default change (text+lw) by alex@other_client *exclusive*
#   //smatchet/main/foo.cpp#3 - edit default change (text+l) by alex@other_client *locked*
#
# Both `*exclusive*` (filetype `+l` modifier on add/edit) and `*locked*`
# (deliberate `p4 lock` action) count — neither lets us submit without
# resolving with the other client.
#
# We care when:
#   - line contains "*exclusive*" or "*locked*"
#   - and the trailing "by <user>@<client>" client != $P4CLIENT
our_client="${P4CLIENT:-}"

foreign_locks=$(echo "$opened_out" | awk -v me="$our_client" '
    /\*exclusive\*|\*locked\*/ {
        for (i=1; i<=NF; i++) if ($i == "by") { who = $(i+1); break }
        # who looks like "user@client"; extract the client side
        split(who, parts, "@")
        client = parts[2]
        if (client != "" && client != me) print $0
    }
')

if [ -n "$foreign_locks" ]; then
    echo "[pretool-edit-p4-lock] WARNING: ${FILE_PATH} is exclusively locked by another p4 client:" >&2
    echo "$foreign_locks" >&2
    echo "[pretool-edit-p4-lock] release with: p4 unlock <file> (lock owner) or p4 unlock -f <file> (admin)" >&2
    if [ "${SMATCHET_P4_LOCK_HOOK_BLOCK:-0}" = "1" ]; then
        echo "[pretool-edit-p4-lock] SMATCHET_P4_LOCK_HOOK_BLOCK=1 — refusing Edit" >&2
        exit 2
    fi
fi

exit 0
