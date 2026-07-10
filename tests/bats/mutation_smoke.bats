#!/usr/bin/env bats
# Unit tests for scripts/dev/mutation-smoke.sh classify/floor/honesty logic.
# The real sweep needs a TSan build; here we fake it — a PATH `cmake` shim that
# no-ops the incremental build, and a fake test exe whose exit code simulates the
# oracle: it exits non-zero (KILLED) when the mutated text is present in the
# target file, zero (SURVIVED) otherwise. That lets us drive every classify path
# deterministically without compiling anything.

setup() {
    HARNESS="$(git rev-parse --show-toplevel)/scripts/dev/mutation-smoke.sh"
    TMP="$(mktemp -d)"
    cd "$TMP"
    git init -q
    git config user.email t@t; git config user.name t
    # a tiny "production" file with two mutatable lines
    printf 'int a() { return 1; }\nint b() { return 2; }\n' > prod.c
    git add prod.c; git commit -qm init

    # fake exe: KILLED (exit 1) iff the file contains the injected bug text.
    cat > fake_rig.sh <<'EOF'
#!/usr/bin/env bash
# "test" catches the a()-mutant (return 9) but NOT the b()-mutant (return 8).
grep -q 'return 9' prod.c && exit 1   # KILLED
exit 0                                # SURVIVED
EOF
    chmod +x fake_rig.sh

    # PATH shim: cmake --build is a no-op success; anything else passes through.
    mkdir bin
    cat > bin/cmake <<'EOF'
#!/usr/bin/env bash
[ "${1:-}" = "--build" ] && exit 0
exec /usr/bin/cmake "$@"
EOF
    chmod +x bin/cmake
    export PATH="$TMP/bin:$PATH"
}
teardown() { rm -rf "$TMP"; }

run_smoke() { run bash "$HARNESS" --corpus corpus.json --exe "$TMP/fake_rig.sh" --preset fake "$@"; }

@test "killed mutant scores and meets floor" {
    cat > corpus.json <<'EOF'
[{"id":"A","file":"prod.c","search":"return 1;","replace":"return 9;","expect":"killed"}]
EOF
    run_smoke --gate --floor 80
    [ "$status" -eq 0 ]
    [[ "$output" == *"A: KILLED"* ]]
    [[ "$output" == *"kill rate = 100%"* ]]
}

@test "a killed 'killed'-mutant that survives drops the rate below floor and --gate fails" {
    # b()-mutant is NOT caught by the fake rig -> SURVIVED, but corpus expects killed
    cat > corpus.json <<'EOF'
[{"id":"B","file":"prod.c","search":"return 2;","replace":"return 8;","expect":"killed"}]
EOF
    run_smoke --gate --floor 80
    [ "$status" -eq 1 ]
    [[ "$output" == *"B: SURVIVED"* ]]
    [[ "$output" == *"BELOW FLOOR"* ]]
}

@test "expect=survived is excluded from the floor (known gap, not scored)" {
    cat > corpus.json <<'EOF'
[{"id":"A","file":"prod.c","search":"return 1;","replace":"return 9;","expect":"killed"},
 {"id":"B","file":"prod.c","search":"return 2;","replace":"return 8;","expect":"survived"}]
EOF
    run_smoke --gate --floor 80
    [ "$status" -eq 0 ]                       # only A (killed) is scored -> 100%
    [[ "$output" == *"known gap"* ]]
}

@test "expect=survived that gets KILLED nudges to graduate (still passes)" {
    # mark the a()-mutant (which the rig kills) as a known gap -> flip nudge
    cat > corpus.json <<'EOF'
[{"id":"A","file":"prod.c","search":"return 1;","replace":"return 9;","expect":"survived"}]
EOF
    run_smoke
    [ "$status" -eq 0 ]
    [[ "$output" == *"flip corpus expect -> killed"* ]]
}

@test "an equivalent mutant that gets KILLED fails the honesty check under --gate" {
    # a()-mutant is killable, but mislabelled equivalent -> equiv_bad
    cat > corpus.json <<'EOF'
[{"id":"A","file":"prod.c","search":"return 1;","replace":"return 9;","expect":"equivalent"}]
EOF
    run_smoke --gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"corpus marks this equivalent but a test killed it"* ]]
}

@test "an equivalent mutant that survives is excluded and passes" {
    cat > corpus.json <<'EOF'
[{"id":"A","file":"prod.c","search":"return 1;","replace":"return 9;","expect":"killed"},
 {"id":"B","file":"prod.c","search":"return 2;","replace":"return 8;","expect":"equivalent"}]
EOF
    run_smoke --gate --floor 80
    [ "$status" -eq 0 ]
    [[ "$output" == *"B: SURVIVED (equivalent, excluded from floor)"* ]]
}

@test "refuses to run on a dirty tree" {
    echo "// scratch" >> prod.c
    cat > corpus.json <<'EOF'
[{"id":"A","file":"prod.c","search":"return 1;","replace":"return 9;","expect":"killed"}]
EOF
    run_smoke --gate
    [ "$status" -eq 3 ]
    [[ "$output" == *"working tree not clean"* ]]
}

@test "ambiguous search (multiple matches) is a spec error, not a silent pass" {
    printf 'x=1;\nx=1;\n' > prod.c; git add prod.c; git commit -qm dup
    cat > corpus.json <<'EOF'
[{"id":"A","file":"prod.c","search":"x=1;","replace":"x=9;","expect":"killed"}]
EOF
    run_smoke
    [[ "$output" == *"SPEC-ERROR"* ]]
    # tree must be clean afterward regardless
    git diff --quiet
}

@test "tree is clean after a full sweep" {
    cat > corpus.json <<'EOF'
[{"id":"A","file":"prod.c","search":"return 1;","replace":"return 9;","expect":"killed"},
 {"id":"B","file":"prod.c","search":"return 2;","replace":"return 8;","expect":"survived"}]
EOF
    run_smoke
    git diff --quiet
}
