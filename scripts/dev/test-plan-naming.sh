#!/usr/bin/env bash
# test-plan-naming.sh — plan files under docs/plans/{active,shipped}/ must be
# kebab-case (lowercase, digits, dashes). `_`-prefixed files (templates,
# generated, README tombstones) are exempt. Keeps the plan tree consistent so
# the slug == filename == index key (agentic-layer-project-independence § naming).
set -uo pipefail
cd "$(git rev-parse --show-toplevel)"

bad=()
while IFS= read -r f; do
  [ -z "$f" ] && continue
  base="$(basename "$f")"
  case "$base" in _*|README.md) continue ;; esac
  [[ "$base" =~ ^[a-z0-9]+(-[a-z0-9]+)*\.md$ ]] || bad+=("$f")
done < <(find docs/plans/active docs/plans/shipped -maxdepth 1 -name '*.md' 2>/dev/null)

if [ "${#bad[@]}" -eq 0 ]; then
  echo "test-plan-naming: all plan files are kebab-case."
  echo "Passed: 1  Failed: 0"
  exit 0
fi
echo "test-plan-naming: ${#bad[@]} non-kebab plan file(s):" >&2
for b in "${bad[@]}"; do echo "  $b" >&2; done
echo "Passed: 0  Failed: 1"
exit 1
