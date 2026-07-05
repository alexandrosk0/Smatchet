# Remote-container builds: GitHub release tarballs 403 through the agent proxy

- **Date**: 2026-07-05 · **Priority**: P3 · **Category**: infra
- **Session**: user-facing-text session (PRs #1614/#1615)

## Friction

In the Claude Code remote container, the network policy allows `git clone` but
returns **403 for GitHub release-asset and codeload tarball downloads**. The
`posix-core-check` configure fails at cpr's internal FetchContent of
`curl-7.80.0.tar.xz`. Also `xorg-dev`/`libgl1-mesa-dev` are not preinstalled
(glfw's configure needs them even though it never builds in that preset) and
need an `apt-get update` first.

## Workaround (validated this session)

```
git clone --depth 1 --branch curl-7_80_0 https://github.com/curl/curl.git \
    .fetchcontent-src/curl-manual
sudo apt-get update && sudo apt-get install -y xorg-dev libgl1-mesa-dev
cmake --preset posix-core-check \
    -DFETCHCONTENT_SOURCE_DIR_CURL=$PWD/.fetchcontent-src/curl-manual
```

## Proposal

Fold the two steps into a SessionStart hook or a
`scripts/dev/remote-container-bootstrap.sh` so future remote sessions get a
working `posix-core-check` lane without rediscovering the workaround.
