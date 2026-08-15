#!/usr/bin/env bats
# tests/bats/resolve_py.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/lib/resolve-py.sh — the one blessed
# python-probe home (shell-lint rule 9 routes every literal probe here).
# Pins the CR #2019 round-1 contract points:
#   * a Python 2 `python` must NOT win the fallback (py3-only callers would
#     fail mid-script on `open(..., encoding=…)`) — version gate;
#   * the printed path is ABSOLUTE even when PATH carries a relative entry
#     (callers cd between resolving and invoking);
#   * SMATCHET_RESOLVE_PY_FORCE_NONE=1 forces the no-python branch (test seam).
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    LIB="$REPO_ROOT/agents/scripts/core/lib/resolve-py.sh"
    export LIB
    STUB_DIR="$(mktemp -d)"
    export STUB_DIR
}

teardown() {
    [ -n "${STUB_DIR:-}" ] && rm -rf "$STUB_DIR"
}

@test "resolve_py rejects a Python 2 stub even when it shadows every candidate name" {
    # Fake py2: runs fine (`-c` exits 0 for a plain exec probe) but fails the
    # version_info gate — the version gate must skip it. The stub dir goes
    # FIRST on PATH so it shadows every candidate name (the loop advances by
    # CANDIDATE, not by PATH entry, so a real python3 later on PATH is never
    # consulted once the stub wins the name lookup); system dirs stay on PATH
    # so bash/env keep resolving.
    for name in python3 python py; do
        cat > "$STUB_DIR/$name" <<'STUB'
#!/usr/bin/env bash
# Emulate python2: any -c payload mentioning version_info exits 1 (py2),
# a bare `-c ''` exec probe exits 0.
case "${2:-}" in
    *version_info*) exit 1 ;;
    *) exit 0 ;;
esac
STUB
        chmod +x "$STUB_DIR/$name"
    done
    run bash -c "PATH='$STUB_DIR:/usr/bin:/bin' bash -c '. \"$LIB\"; resolve_py'"
    [ "$status" -eq 1 ]
    [ -z "$output" ]
}

@test "resolve_py returns an ABSOLUTE path for a relative PATH entry" {
    mkdir "$STUB_DIR/bin"
    real_py="$(command -v python3 || command -v python)"
    [ -n "$real_py" ] || skip "no working python interpreter"
    ln -s "$real_py" "$STUB_DIR/bin/python3"
    run bash -c "cd '$STUB_DIR' && PATH='bin:/usr/bin:/bin' bash -c '. \"$LIB\"; resolve_py'"
    [ "$status" -eq 0 ]
    [[ "$output" == /* ]]
    # And the returned path still works after a directory change.
    run bash -c "cd '$STUB_DIR' && PATH='bin:/usr/bin:/bin' bash -c '. \"$LIB\"; p=\"\$(resolve_py)\"; cd / && \"\$p\" -c \"\"'"
    [ "$status" -eq 0 ]
}

@test "SMATCHET_RESOLVE_PY_FORCE_NONE=1 forces the no-python branch" {
    run bash -c ". '$LIB'; SMATCHET_RESOLVE_PY_FORCE_NONE=1 resolve_py"
    [ "$status" -eq 1 ]
    [ -z "$output" ]
}

@test "resolve_py finds a real Python 3 and prints an absolute path (happy path)" {
    command -v python3 >/dev/null 2>&1 || skip "no python3 on PATH"
    run bash -c ". '$LIB'; resolve_py"
    [ "$status" -eq 0 ]
    [[ "$output" == /* ]]
    "$output" -c 'import sys; raise SystemExit(0 if sys.version_info[0] >= 3 else 1)'
}
