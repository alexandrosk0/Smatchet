#!/usr/bin/env bash
# Fixture: rule 6 NEGATIVE — same var=$(... | head) shape under pipefail, but
# with the explicit `|| true` mitigation the author is meant to add. Rule 6
# must stay silent here. Also exercises the "head is NOT the terminal stage"
# carve-out (`| head -1 | tr` ends in tr, not head → not flagged).
set -euo pipefail
x=$(printf '%s\n' a b c | head -5 || true)
y=$(printf '%s\n' a b c | head -1 | tr -d ' ')
echo "$x $y"
