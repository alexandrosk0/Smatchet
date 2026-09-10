#!/usr/bin/env bash
# classify-layer-consumers.sh — the agent-layer consumer classification baseline.
# ----------------------------------------------------------------------------
# WHY THIS EXISTS (plan agent-surface-extraction-repo, Phase A row 5a)
#   The agent surface (agents/, docs/agent-rules/, docs/harness/) is moving into
#   its own repository, mounted back here as a submodule at agent-layer/. Every
#   layer script that names a HOST path (docs/plans/, docs/self-improvement/,
#   backlog/, Source/, project.config.json) breaks silently at the flip: the
#   path still resolves, it just resolves inside the layer, where the file does
#   not exist. `grep -l` over agents/scripts/core/*.sh returns ~60 such files.
#
#   The plan's prose frames the rewire as binary — "layer scripts read layer
#   content, host scripts read host content". The class that framing hides is
#   DUAL: a script living in the layer that reads BOTH trees in one file. Those
#   cannot be fixed by a single root substitution; they need both roots sourced
#   and each call site routed to the right one. This baseline is what proves the
#   dual list closed before any edit lands.
#
# CONTRACT (same shape as test-portable-purity.sh's baseline guard)
#   default / --check   diff the current classification against the committed
#                       baseline; exit 1 on any drift, printing the delta
#   --regen             rewrite the baseline from the current tree
#
#   Baseline: docs/high-integrity/agent-layer-consumer-baseline.tsv
#   Columns:  script <TAB> host_paths <TAB> layer_paths <TAB> class
#             (path lists are comma-joined and sorted; "-" when empty)
#
# CLASSES (exclusive, evaluated in this order)
#   dual           names both a host path and a layer path -> needs BOTH roots
#   host-only      names only host paths      -> route through $PROJECT_ROOT
#   layer-only     names only layer paths     -> route through $AGENT_LAYER_ROOT
#   self-locating  names neither by literal; resolves everything from its own
#                  location -> survives the flip untouched
#
# A "path" here is a literal appearing anywhere in the file, comments included.
# That is deliberate: a stale comment naming a path that no longer resolves is
# itself a defect this sweep should surface, and excluding comments would need a
# shell parser to do correctly.
#
# LITERALS ARE NOT THE WHOLE STORY. A script can hand a host tree to a helper
# without ever naming a host path itself: plan-lock-gate.sh computes
# `root="$(git rev-parse --show-toplevel)"` and exports it as LTC_PROJ, and
# lock-table-cache.sh is the file that reads docs/plans/ under it. On literals
# alone the gate classifies layer-only and the plan's row 5f finding — that it
# hard-exits on the first post-flip PR — is invisible. So a HOST_VARS list names
# the variables whose VALUE is a host tree; assigning one counts as a host hit.
set -uo pipefail

BASELINE="docs/high-integrity/agent-layer-consumer-baseline.tsv"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$SCRIPT_DIR/lib/resolve-py.sh"
PY="$(resolve_py)" || { echo "classify-layer-consumers: no working python found" >&2; exit 2; }

mode="check"
case "${1:-}" in
  --regen|--refresh) mode="regen" ;;
  --check|"") mode="check" ;;
  -h|--help) sed -n '2,40p' "${BASH_SOURCE[0]}"; exit 0 ;;
  *) echo "usage: $0 [--check|--regen]" >&2; exit 2 ;;
esac

current="$("$PY" - <<'PY'
import glob, io, os, re, sys

# Host trees: content that stays in the Smatchet repo after the flip.
HOST = [
    r"docs/plans/",
    r"docs/self-improvement/",
    r"backlog/",
    r"Source/",
    r"project\.config\.json",
]
# Layer trees: content that moves into the-unwilling-agentic-bunch.
# `agents/` is matched only with a following path segment so the bare English
# word "agents" in prose does not classify a script as a layer consumer.
LAYER = [
    r"agents/",
    r"docs/agent-rules/",
    r"docs/harness/",
    r"AGENTS\.md",
]
# Variables whose value is a HOST tree root. Assigning one is a host hit even
# when the script names no host path: the tree it hands over is the host's, and
# post-flip a self-locating derivation of it lands in the layer instead.
#   LTC_PROJ — lock-table-cache.sh's "repo root that holds docs/plans/"
HOST_VARS = [r"LTC_PROJ="]

def hits(text, pats):
    found = set()
    for p in pats:
        for m in re.finditer(p, text):
            found.add(m.group(0))
    return sorted(found)

rows = []
for path in sorted(glob.glob("agents/scripts/core/*.sh")
                   + glob.glob("agents/scripts/core/lib/*.sh")
                   + glob.glob("agents/scripts/project/*.sh")):
    path = path.replace(os.sep, "/")
    with io.open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    h = hits(text, HOST) + hits(text, HOST_VARS)
    l = hits(text, LAYER)
    if h and l:
        cls = "dual"
    elif h:
        cls = "host-only"
    elif l:
        cls = "layer-only"
    else:
        cls = "self-locating"
    rows.append("%s\t%s\t%s\t%s" % (path, ",".join(h) or "-", ",".join(l) or "-", cls))

sys.stdout.write("\n".join(rows) + "\n")
PY
)" || { echo "classify-layer-consumers: classification failed" >&2; exit 2; }

if [ "$mode" = "regen" ]; then
  mkdir -p "$(dirname "$BASELINE")"
  printf '%s\n' "$current" > "$BASELINE"
  echo "classify-layer-consumers: baseline regenerated ($(printf '%s\n' "$current" | grep -c .) rows)"
  exit 0
fi

if [ ! -f "$BASELINE" ]; then
  echo "classify-layer-consumers: $BASELINE missing — run $0 --regen" >&2
  exit 1
fi

# Strip CR so a CRLF-checked-out baseline (Windows autocrlf) still compares.
if diff -u <(tr -d '\r' < "$BASELINE") <(printf '%s\n' "$current") > /tmp/.clc.$$ 2>&1; then
  rm -f /tmp/.clc.$$
  echo "classify-layer-consumers: classification matches the baseline ($(grep -c . "$BASELINE") rows)."
  exit 0
fi

echo "classify-layer-consumers: classification DRIFTED from $BASELINE" >&2
cat /tmp/.clc.$$ >&2
rm -f /tmp/.clc.$$
echo "" >&2
echo "A new dual-class script is a REWIRE OBLIGATION, not a baseline bump:" >&2
echo "route its host paths through \$PROJECT_ROOT and its layer paths through" >&2
echo "\$AGENT_LAYER_ROOT, then re-run with --regen." >&2
exit 1
