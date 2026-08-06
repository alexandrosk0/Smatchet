#!/usr/bin/env bash
# test-tooltip-wrapwidth.sh — assert every BeginTooltip+MarkdownPreviewRender::Render
# block in Source/Core/src/ also sets opts.wrapWidth.
#
# MarkdownPreviewRender::Render ignores ImGui's PushTextWrapPos and uses
# GetContentRegionAvail().x instead.  In a fresh BeginTooltip() window that
# value is near-zero, producing an ultra-narrow vertical strip.  The only fix
# is opts.wrapWidth.  This script catches any new site that forgets the field.
#
# Exits: 0 = all-pass, 1 = violation found, 2 = missing prerequisite.
# Prints: "Passed: N  Failed: M"   (consumed by test-all.sh aggregate)

set -euo pipefail
cd "$(dirname "$0")/../.."

SRC_DIR="Source/Core/src"

if [ ! -d "$SRC_DIR" ]; then
    echo "Source dir not found: $SRC_DIR" >&2
    exit 2
fi

# Exec-validate each candidate, don't just resolve it. On Windows `python3`
# resolves to the Microsoft Store App Execution Alias stub -- on PATH, but it
# exits non-zero when run. Probe ORDER alone cannot fix that (a box with only
# the stub aliased for `python` fails the same way); running the candidate can.
if [ -z "${PYTHON:-}" ]; then
    for candidate in python python3 py; do
        if command -v "$candidate" >/dev/null 2>&1 && "$candidate" -c "" >/dev/null 2>&1; then
            PYTHON="$candidate"
            break
        fi
    done
fi
if [ -z "${PYTHON:-}" ]; then
    echo "no working python3 / python found" >&2
    exit 2
fi

"$PYTHON" - "$SRC_DIR" <<'PYEOF'
import sys
import os

src_dir = sys.argv[1]
checked = 0   # tooltip blocks that contain MarkdownPreviewRender::Render
violations = []

# #430 (fail-closed): os.listdir(src_dir) saw only top-level *.cpp, so the real
# markdown-tooltip sites under Ui/ and Commands/Scenarios/ were never scanned —
# a new offending site there was silently skipped and the gate printed
# "Failed: 0" / exited 0 despite claiming full-tree coverage. Walk the whole
# tree so every *.cpp under src_dir is checked.
cpp_paths = []
for dirpath, _dirnames, filenames in os.walk(src_dir):
    for fname in filenames:
        if fname.endswith('.cpp'):
            cpp_paths.append(os.path.join(dirpath, fname))

for fpath in sorted(cpp_paths):
    fname = os.path.relpath(fpath, src_dir)
    with open(fpath, encoding='utf-8', errors='replace') as f:
        lines = f.readlines()

    in_tooltip = False
    tooltip_start = 0
    block = []

    for lineno, line in enumerate(lines, 1):
        if 'BeginTooltip' in line and 'EndTooltip' not in line:
            in_tooltip = True
            tooltip_start = lineno
            block = [line]
        elif in_tooltip:
            block.append(line)
            if 'EndTooltip' in line:
                joined = ''.join(block)
                if 'MarkdownPreviewRender::Render' in joined:
                    checked += 1
                    if 'opts.wrapWidth' not in joined:
                        violations.append((fname, tooltip_start, lineno))
                in_tooltip = False
                block = []

for fname, start, end in violations:
    print('FAIL  {}:{}-{}  BeginTooltip..EndTooltip has MarkdownPreviewRender::Render without opts.wrapWidth'.format(
        fname, start, end))

passed = checked - len(violations)
print('Passed: {}  Failed: {}'.format(passed, len(violations)))
sys.exit(1 if violations else 0)
PYEOF
