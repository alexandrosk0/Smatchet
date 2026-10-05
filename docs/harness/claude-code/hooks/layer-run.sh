#!/usr/bin/env bash
# layer-run.sh — run an agent-layer script from a hook command.
#
# settings.json names its hook commands from $CLAUDE_PROJECT_DIR, the host. A layer
# script is under the project dir before the flip and under its agent-layer/ mount
# after it, so a command naming the script by a fixed path breaks on one side of
# the flip (bash exits 127, and every nudge and session baseline goes with it).
# This resolves the layer (layer-root.sh) and runs the script there.
#
# Usage: bash "$CLAUDE_PROJECT_DIR/.claude/hooks/layer-run.sh" <layer-relative script> [args...]
# Exit: the script's own; 2 on a usage error or a path outside the layer.
set -u

_lr_dir="${BASH_SOURCE[0]%/*}"
[ "$_lr_dir" = "${BASH_SOURCE[0]}" ] && _lr_dir=.
# shellcheck source=docs/harness/claude-code/hooks/layer-root.sh
. "$_lr_dir/layer-root.sh" || exit 2

[ "$#" -ge 1 ] || { echo "layer-run.sh: usage: layer-run.sh <layer-relative script> [args...]" >&2; exit 2; }
_lr_script="$1"
shift
case "$_lr_script" in
    ''|/*|*..*) echo "layer-run.sh: refusing a path outside the layer: $_lr_script" >&2; exit 2 ;;
esac
hook_layer_root "${CLAUDE_PROJECT_DIR:-$(pwd)}"
exec bash "$HOOK_LAYER/$_lr_script" "$@"
