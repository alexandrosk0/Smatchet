#!/usr/bin/env bats
# tests/bats/dup_audit.bats
# ----------------------------------------------------------------------------
# Bats coverage for agents/scripts/core/dup_audit.py — the DRY-pillar duplication
# delta gate (dry-pillar-dup-gate / ADR-0015). BLOCKING (graduated WARN→blocking
# 2026-06-21): --diff exits 1 and prints [dup] FAIL on a NEW clone, exit 0 clean.
#
# Covers: --selftest; intra-file detection + path exclusion via --scan-file; and
# the --diff delta gate (new clone FAILs/exit 1, copy-then-rename still caught,
# grandfathered duplication is silent/exit 0, sub-threshold is silent,
# SMATCHET_DEVIATION suppresses) via throwaway git repos.
#
# Requires: bash, bats, a working python interpreter (python3/python/py).
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export AUD="$REPO_ROOT/agents/scripts/core/dup_audit.py"
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"
}

# Emit, to stdout, a C++ function body whose name is $1 and whose tokens use the var prefix $2
# (so $2 lets two callers be identical or identifier-renamed). ~520 normalized tokens (>> the
# 70-token floor) so it is unambiguously a clone.
_block() {
    "$PY" - "$1" "$2" <<'PY'
import sys
name, pfx = sys.argv[1], sys.argv[2]
body = " ".join("int %s%d = compute(%d) + lookup(%d);" % (pfx, i, i, i) for i in range(40))
print("void %s(){ %s }" % (name, body))
PY
}

# ---------- selftest ----------

@test "selftest passes (normalization + threshold invariants in sync)" {
    run "$PY" "$AUD" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"invariants hold"* ]]
}

# ---------- intra-file detection + exclusion (--scan-file) ----------

@test "--scan-file detects an intra-file copy-paste clone" {
    f="$BATS_TEST_TMPDIR/Intra.cpp"
    { _block p v; _block q v; } > "$f"
    run "$PY" "$AUD" --scan-file "$f"
    [ "$status" -eq 0 ]
    [[ "$output" == *"duplication"* ]]
}

@test "--scan-file finds nothing in a file with no large clone" {
    f="$BATS_TEST_TMPDIR/Clean.cpp"
    { echo "int a(){ return 1; }"; echo "double b(double x){ return x*2; }"; } > "$f"
    run "$PY" "$AUD" --scan-file "$f"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "--scan-file excludes a generated path" {
    f="$BATS_TEST_TMPDIR/Thing.generated.cpp"
    { _block p v; _block q v; } > "$f"
    run "$PY" "$AUD" --scan-file "$f"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# ---------- delta gate (--diff) via throwaway git repos ----------

# Init a repo with two distinct, clone-free scoped files.
_mk_repo() {
    local dir="$1"
    git init -q "$dir"
    git -C "$dir" config user.email t@t.t
    git -C "$dir" config user.name t
    mkdir -p "$dir/Source/Core/src/Tracker"
    echo "void a(){ int x=1; x+=2; }" > "$dir/Source/Core/src/Tracker/A.cpp"
    echo "void b(){ float y=3; y-=4; }" > "$dir/Source/Core/src/Tracker/B.cpp"
    git -C "$dir" add -A && git -C "$dir" commit -qm base
}

@test "--diff FAILs (exit 1) when a NEW copy-paste clone appears across two files" {
    repo="$BATS_TEST_TMPDIR/new"
    _mk_repo "$repo"
    { echo "void a(){ int x=1; x+=2; }"; _block clone v; } > "$repo/Source/Core/src/Tracker/A.cpp"
    { echo "void b(){ float y=3; y-=4; }"; _block clone v; } > "$repo/Source/Core/src/Tracker/B.cpp"
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 1 ]                 # blocking: a NEW clone fails CLOSED
    [[ "$output" == *"[dup] FAIL"* ]]
}

@test "--diff catches a copy-then-RENAME clone (identifier normalization)" {
    repo="$BATS_TEST_TMPDIR/rename"
    _mk_repo "$repo"
    { echo "void a(){ int x=1; x+=2; }"; _block clone v; } > "$repo/Source/Core/src/Tracker/A.cpp"
    { echo "void b(){ float y=3; y-=4; }"; _block other w; } > "$repo/Source/Core/src/Tracker/B.cpp"
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 1 ]
    [[ "$output" == *"[dup] FAIL"* ]]
}

@test "--diff is silent for duplication already present at base (grandfathered)" {
    repo="$BATS_TEST_TMPDIR/grand"
    git init -q "$repo"; git -C "$repo" config user.email t@t.t; git -C "$repo" config user.name t
    mkdir -p "$repo/Source/Core/src/Tracker"
    _block clone v > "$repo/Source/Core/src/Tracker/A.cpp"
    _block clone v > "$repo/Source/Core/src/Tracker/B.cpp"
    git -C "$repo" add -A && git -C "$repo" commit -qm base
    # Add a third copy of the SAME already-duplicated block — content is grandfathered.
    _block clone v > "$repo/Source/Core/src/Tracker/C.cpp"
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 0 ]
    [[ "$output" != *"[dup] FAIL"* ]]
}

@test "--diff is silent for a sub-threshold near-clone (< 70 tokens)" {
    repo="$BATS_TEST_TMPDIR/small"
    _mk_repo "$repo"
    small="void s(){ int v0=0; int v1=1; int v2=2; }"
    { echo "void a(){ int x=1; x+=2; }"; echo "$small"; } > "$repo/Source/Core/src/Tracker/A.cpp"
    { echo "void b(){ float y=3; y-=4; }"; echo "$small"; } > "$repo/Source/Core/src/Tracker/B.cpp"
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 0 ]
    [[ "$output" != *"[dup] FAIL"* ]]
}

@test "--diff PASSES silently when SMATCHET_DEVIATION suppresses a new clone" {
    repo="$BATS_TEST_TMPDIR/dev"
    _mk_repo "$repo"
    {
        echo "void a(){ int x=1; x+=2; }"
        echo "// SMATCHET_DEVIATION(rule=duplication; reason=test; owner=t; revisit=never)"
        _block clone v
    } > "$repo/Source/Core/src/Tracker/A.cpp"
    {
        echo "void b(){ float y=3; y-=4; }"
        echo "// SMATCHET_DEVIATION(rule=duplication; reason=test; owner=t; revisit=never)"
        _block clone v
    } > "$repo/Source/Core/src/Tracker/B.cpp"
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 0 ]
    [[ "$output" != *"[dup] FAIL"* ]]
}

# The suppression check is a per-LINE test, so a marker whose reason prose wraps onto following
# comment lines does NOT suppress — the nearest non-blank line above the clone is then prose, which
# carries no token. That used to surface as a bare FAIL, reading as "your reason text is wrong" when
# only the marker's SHAPE was (tooling 2026-08-07). Pin both halves of the fix: still a FAIL, but
# now with a hint that names the placement.
@test "--diff hints when a rule=duplication marker is present but WRAPPED (ineffective)" {
    repo="$BATS_TEST_TMPDIR/dev"
    _mk_repo "$repo"
    # The preamble must NOT itself be part of the clone: the maximal-token-run boundary can drift
    # ABOVE the human-meaningful start, and _suppressed also scans anywhere INSIDE the span — a
    # drifted span that happens to cover the marker line would suppress and mask the bug. A
    # structurally-unique preamble in A only (B is the bare block) pins the span to the block line,
    # which is the shape that actually bit.
    {
        echo "struct Preamble { int a; double b; char c; };"
        echo "// SMATCHET_DEVIATION(rule=duplication; reason=the two window-layout helpers are"
        echo "// long-standing structural twins; unifying them would couple independent"
        echo "// subsystems; owner=t; revisit=never)"
        _block clone v
    } > "$repo/Source/Core/src/Tracker/A.cpp"
    _block clone v > "$repo/Source/Core/src/Tracker/B.cpp"
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 1 ]
    [[ "$output" == *"[dup] FAIL"* ]]
    [[ "$output" == *"[dup] hint:"* ]]
    [[ "$output" == *"SINGLE line"* ]]
}

# The hint must stay specific to duplication markers: firing it on an unrelated rule-id would train
# readers to skip it.
@test "--diff does NOT hint when the nearby deviation is for a different rule" {
    repo="$BATS_TEST_TMPDIR/dev"
    _mk_repo "$repo"
    {
        echo "struct Preamble { int a; double b; char c; };"
        echo "// SMATCHET_DEVIATION(rule=function-too-long; reason=test; owner=t; revisit=never)"
        _block clone v
    } > "$repo/Source/Core/src/Tracker/A.cpp"
    _block clone v > "$repo/Source/Core/src/Tracker/B.cpp"
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 1 ]
    [[ "$output" == *"[dup] FAIL"* ]]
    [[ "$output" != *"[dup] hint:"* ]]
}
