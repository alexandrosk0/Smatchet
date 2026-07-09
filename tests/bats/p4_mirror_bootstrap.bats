#!/usr/bin/env bats
# tests/bats/p4_mirror_bootstrap.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/p4-mirror-bootstrap.sh.
#
# Stubs `p4`, `apt-get`, `sudo`, `git`, `git-lfs` on PATH; controls existing
# server state via env vars. No live p4d / WSL2 / apt needed.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    SCRIPT="$REPO_ROOT/scripts/dev/p4-mirror-bootstrap.sh"
    export SCRIPT

    export P4PORT="Mainbot:1666"
    export P4ADMIN_USER="alexk"
    export MIRROR_DEPOT="repo"
    export MIRROR_REPO_PATH="repo/smatchet"
    export GCONN_USER="gconn"

    STUB_BIN_DIR="$(mktemp -d)"
    export STUB_BIN_DIR
    export STUB_P4_LOG="$STUB_BIN_DIR/p4.calls"

    cat > "$STUB_BIN_DIR/p4" <<'STUB'
#!/usr/bin/env bash
# Stub p4. State via env:
#   STUB_P4_INFO_FAIL    — `p4 info` exits 1 (unreachable)
#   STUB_P4_HAVE_DEPOT   — `depots -t graph` lists the depot
#   STUB_P4_HAVE_REPO    — `repos` lists //repo/smatchet
#   STUB_P4_HAVE_USER    — `users -a` lists gconn
#   STUB_P4_HAVE_GRANT   — `show-permission` lists the gconn admin grant
# Mutating verbs (depot -i / repo -i / user -i / grant-permission) append one
# line to $STUB_P4_LOG so tests can assert exactly what was created.
args=("$@")
verb=""
for a in "${args[@]}"; do
    case "$a" in -p|-u) skip=1; continue ;; esac
    if [ "${skip:-0}" = 1 ]; then skip=0; continue; fi
    verb="$a"; break
done
case "$verb" in
    info)  [ -n "${STUB_P4_INFO_FAIL:-}" ] && exit 1; echo "Server address: stub"; exit 0 ;;
    depots) [ -n "${STUB_P4_HAVE_DEPOT:-}" ] && echo "Depot repo 2026/01/01 graph repo/... 'Default graph depot'"; exit 0 ;;
    repos)  [ -n "${STUB_P4_HAVE_REPO:-}" ] && echo "Repo //repo/smatchet 2026/01/01 'mirror'"; exit 0 ;;
    users)  [ -n "${STUB_P4_HAVE_USER:-}" ] && echo "gconn <gconn@localhost> (Git Connector service) accessed 2026/01/01"; exit 0 ;;
    show-permission) [ -n "${STUB_P4_HAVE_GRANT:-}" ] && echo "//repo/... * user gconn admin"; exit 0 ;;
    depot|repo|user) cat >/dev/null; echo "$verb -i" >> "$STUB_P4_LOG"; exit 0 ;;
    grant-permission) echo "grant-permission" >> "$STUB_P4_LOG"; exit 0 ;;
esac
exit 0
STUB
    chmod +x "$STUB_BIN_DIR/p4"

    for tool in git git-lfs; do
        printf '#!/usr/bin/env bash\nexit 0\n' > "$STUB_BIN_DIR/$tool"
        chmod +x "$STUB_BIN_DIR/$tool"
    done

    PATH="$STUB_BIN_DIR:$PATH"
    export PATH
}

teardown() {
    rm -rf "$STUB_BIN_DIR"
}

@test "everything present -> all OK, zero mutations, exit 0" {
    export STUB_P4_HAVE_DEPOT=1 STUB_P4_HAVE_REPO=1 STUB_P4_HAVE_USER=1 STUB_P4_HAVE_GRANT=1
    run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" != *"CREATED"* ]]
    [ ! -f "$STUB_P4_LOG" ]
}

@test "nothing present -> creates depot, repo, user, grant" {
    run bash "$SCRIPT" --server-only
    [ "$status" -eq 0 ]
    [[ "$output" == *"CREATED  graph depot //repo"* ]]
    [[ "$output" == *"CREATED  repo spec //repo/smatchet"* ]]
    [[ "$output" == *"CREATED  service user gconn"* ]]
    [[ "$output" == *"CREATED  grant: gconn admin"* ]]
    [ "$(sort "$STUB_P4_LOG" | uniq | wc -l)" -eq 4 ]
}

@test "partial state -> only the missing pieces are created" {
    export STUB_P4_HAVE_DEPOT=1 STUB_P4_HAVE_USER=1
    run bash "$SCRIPT" --server-only
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK       graph depot //repo exists"* ]]
    [[ "$output" == *"CREATED  repo spec //repo/smatchet"* ]]
    [[ "$output" == *"OK       service user gconn exists"* ]]
    [[ "$output" == *"CREATED  grant: gconn admin"* ]]
    run grep -c . "$STUB_P4_LOG"
    [ "$output" = "2" ]
}

@test "p4d unreachable -> die with runbook pointer, nonzero" {
    export STUB_P4_INFO_FAIL=1
    run bash "$SCRIPT" --server-only
    [ "$status" -ne 0 ]
    [[ "$output" == *"cannot reach p4d"* ]]
}

@test "p4 missing from PATH -> die with install pointer" {
    rm "$STUB_BIN_DIR/p4"
    run bash "$SCRIPT" --server-only
    [ "$status" -ne 0 ]
    [[ "$output" == *"'p4' not on PATH"* ]]
}

@test "--host-only with git+git-lfs present -> no p4 touched, guided prompts shown" {
    rm "$STUB_BIN_DIR/p4"
    run bash "$SCRIPT" --host-only
    [ "$status" -eq 0 ]
    [[ "$output" == *"git + git-lfs installed"* ]]
    [[ "$output" == *"mirrorhooks add repo/smatchet"* ]]
    [[ "$output" == *"p4-mirror-healthcheck.sh"* ]]
}

@test "--host-only with packages missing and no apt-get -> die pointing at WSL2 host" {
    rm "$STUB_BIN_DIR/p4" "$STUB_BIN_DIR/git" "$STUB_BIN_DIR/git-lfs"
    # Constrain PATH to the stub dir + a minimal shell env without git/apt-get.
    empty_env="$(mktemp -d)"
    for t in bash cat mktemp dirname grep awk sort uniq wc printf echo rm; do
        p="$(command -v "$t" 2>/dev/null)" && ln -s "$p" "$empty_env/$t"
    done
    run env PATH="$STUB_BIN_DIR:$empty_env" bash "$SCRIPT" --host-only
    rm -rf "$empty_env"
    [ "$status" -ne 0 ]
    [[ "$output" == *"no apt-get"* ]]
}

@test "unknown flag -> usage + exit 2" {
    run bash "$SCRIPT" --bogus
    [ "$status" -eq 2 ]
    [[ "$output" == *"usage"* ]]
}

@test "second run after a full create pass reports OK everywhere (idempotence)" {
    run bash "$SCRIPT" --server-only
    [ "$status" -eq 0 ]
    export STUB_P4_HAVE_DEPOT=1 STUB_P4_HAVE_REPO=1 STUB_P4_HAVE_USER=1 STUB_P4_HAVE_GRANT=1
    : > "$STUB_P4_LOG"
    run bash "$SCRIPT" --server-only
    [ "$status" -eq 0 ]
    [[ "$output" != *"CREATED"* ]]
    [ ! -s "$STUB_P4_LOG" ]
}
