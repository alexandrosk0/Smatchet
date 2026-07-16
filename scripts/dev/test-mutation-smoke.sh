#!/usr/bin/env bash
# test-mutation-smoke.sh — local/CI mirror for the mutation-smoke harness+corpus
# (docs/plans/mutation-smoke-gate.md). Auto-enrolled by test-all.sh via the
# test-*.sh glob and by the Agentic self-tests lane. Validates the harness WITHOUT
# a TSan build: corpus JSON validity, `--list`, and (on a clean tree) the
# `--dry-run` apply/revert mechanics + tree-clean invariant. The real
# KILLED/SURVIVED sweep runs only in the tsan-linux-nightly advisory step.
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"
H="scripts/dev/mutation-smoke.sh"
CORPUS="scripts/dev/mutation-smoke-corpus.json"
pass=0; fail=0
ok()   { echo "  ok   - $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL - $1"; fail=$((fail+1)); }

# 1. corpus is valid JSON with the required keys + known expect values
if python3 - "$CORPUS" <<'PY'
import json, sys
c = json.load(open(sys.argv[1]))
assert isinstance(c, list) and c, "corpus must be a non-empty list"
seen = set()
for m in c:
    for k in ("id", "file", "search", "replace"):
        assert k in m and m[k] != "", f"{m.get('id','?')}: missing/empty {k}"
    assert m["id"] not in seen, f"duplicate id {m['id']}"
    seen.add(m["id"])
    assert m.get("expect", "killed") in ("killed", "survived", "equivalent"), \
        f"{m['id']}: bad expect {m.get('expect')}"
print(len(c))
PY
then ok "corpus is valid JSON with required keys + known expect values"
else bad "corpus JSON validation"; fi

# 2. --list works (tree-independent) and enumerates the corpus
if bash "$H" --list >/dev/null 2>&1; then ok "--list exits 0"; else bad "--list"; fi

# 3. --floor= flag form parses (flag-parity)
if bash "$H" --floor=90 --list >/dev/null 2>&1; then ok "--floor= flag form parses"; else bad "--floor= form"; fi

# 4. dry-run mechanics — only on a clean tree (the harness refuses a dirty one by
#    design; a dev session mid-edit legitimately skips this leg).
if git diff --quiet --ignore-submodules -- . 2>/dev/null; then
    if bash "$H" --dry-run >/dev/null 2>&1; then
        if git diff --quiet -- Source/ 2>/dev/null; then
            ok "--dry-run: apply/revert mechanics OK + tree clean afterward"
        else
            bad "--dry-run left Source/ dirty"; git checkout -- Source/ 2>/dev/null || true
        fi
    else
        bad "--dry-run exited non-zero on a clean tree"
    fi
else
    echo "  skip - --dry-run (working tree dirty; mechanics leg is clean-tree-only)"
fi

echo "test-mutation-smoke: Passed: $pass  Failed: $fail"
[ "$fail" -eq 0 ]
