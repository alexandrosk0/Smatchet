# Offline / restricted-network builds

> Slice 6 of [docs/design/applied/first-time-setup-hardening.md](../design/applied/first-time-setup-hardening.md). Niche audience — contributors behind restrictive firewalls, corporate proxies that block `https://github.com`, air-gapped CI runners, or first-time setups where re-downloading every dependency on every fresh clone is painful.

Smatchet pulls 11 third-party libraries via CMake `FetchContent` at configure time. The very first configure on a fresh clone takes ~5 min (see [BUILD.md § First-time verification](../../BUILD.md)). Every subsequent fresh clone repeats those downloads unless you steer FetchContent to a pre-populated cache.

This doc shows three options, in order of effort:

1. **Pre-populate `build/<preset>/_deps/`** from a peer who already built (cheapest)
2. **Override `FETCHCONTENT_SOURCE_DIR_<NAME>`** per dependency to a local clone (most flexible)
3. **Override `FETCHCONTENT_BASE_DIR`** to a shared network drive (one-time setup, persistent)

## Dependency inventory

Pinned in [CMakeLists.txt](../../CMakeLists.txt), [cmake/ImGuiTestEngine.cmake](../../cmake/ImGuiTestEngine.cmake), and [tests/CMakeLists.txt](../../tests/CMakeLists.txt):

| FetchContent name | Repo | Pin | Source-of-truth |
|---|---|---|---|
| `json` | `nlohmann/json` | `v3.11.3` | `CMakeLists.txt` |
| `cpr` | `libcpr/cpr` | `1.9.2` | `CMakeLists.txt` |
| `sqlitecpp` | `SRombauts/SQLiteCpp` | `3.3.1` | `CMakeLists.txt` |
| `httplib` | `yhirose/cpp-httplib` | `v0.14.1` | `CMakeLists.txt` |
| `md4c` | `mity/md4c` | `release-0.5.2` | `CMakeLists.txt` |
| `ghc_filesystem` | `gulrak/filesystem` | `v1.5.14` | `CMakeLists.txt` |
| `glfw` | `glfw/glfw` | `3.3.8` | `CMakeLists.txt` |
| `sol2` | `ThePhD/sol2` | `v2.20.6` | `CMakeLists.txt` |
| `imgui` | `ocornut/imgui` | `329c5a6b3be75ebf54506d3ae94b836ffcf19fa0` (docking) | `CMakeLists.txt` |
| `imgui_test_engine` | `ocornut/imgui_test_engine` | `8568767ad4c53d6ce02d65f01a09d30fb630bd80` | `cmake/ImGuiTestEngine.cmake` |
| `doctest` | `doctest/doctest` | `v2.4.11` | `tests/CMakeLists.txt` |

When in doubt, the canonical answer is `grep -rE "FetchContent_Declare|GIT_REPOSITORY|GIT_TAG" CMakeLists.txt cmake/ tests/`.

## Option 1 — Copy `_deps/` from a peer

If a teammate already has a clean build in the same preset, the cheapest path is to copy their populated `_deps/` directory:

```powershell
# On the peer's machine, with build/ninja-iter-msys2/ populated:
Compress-Archive -Path build\ninja-iter-msys2\_deps -DestinationPath smatchet-deps-iter.zip

# On yours, after `cmake --preset ninja-iter-msys2` has run far enough to create build/ninja-iter-msys2/:
Expand-Archive -Path smatchet-deps-iter.zip -DestinationPath build\ninja-iter-msys2\
```

Then re-run `cmake --preset ninja-iter-msys2`. FetchContent detects the existing populated state via the `_subbuild/` marker files and skips the download.

**Cache hit semantics** — `_deps/` only re-uses across:

- **Same preset name**: `build/ninja-iter-msys2/_deps` does not feed `build/ninja-debug-msys2/_deps`. Each preset has its own copy.
- **Same compiler + same generator**: a `_deps/` built under MSYS2 UCRT64 will not reliably feed a Visual Studio MSVC preset; pre-built object files diverge. Header-only deps (`json`, `ghc_filesystem`, `httplib`, `sol2`, `doctest`) port; compiled deps (`cpr`, `sqlitecpp`, `glfw`, `imgui`, `md4c`, `imgui_test_engine`) do not.
- **Same `GIT_TAG`**: if the source tree bumps a pin (e.g. `v3.11.3` → `v3.11.4`), the stale `_deps/<name>-src/` will not match. Wipe that subdir and let FetchContent re-fetch.

If a deps copy is large or stale, just delete it: `rm -rf build/<preset>/_deps && cmake --preset <preset>`.

## Option 2 — `FETCHCONTENT_SOURCE_DIR_<NAME>` per dependency

CMake honours `FETCHCONTENT_SOURCE_DIR_<NAME>` (uppercased dependency name) as an override pointing at a pre-existing source tree on disk. FetchContent skips the clone entirely and uses the directory as-is.

```powershell
# One-time clone each dep into a shared offline mirror:
mkdir C:\offline-deps
git clone --depth 1 --branch v3.11.3   https://github.com/nlohmann/json.git              C:\offline-deps\json
git clone --depth 1 --branch 1.9.2     https://github.com/libcpr/cpr.git                 C:\offline-deps\cpr
git clone --depth 1 --branch 3.3.1     https://github.com/SRombauts/SQLiteCpp.git        C:\offline-deps\sqlitecpp
git clone --depth 1 --branch v0.14.1   https://github.com/yhirose/cpp-httplib.git        C:\offline-deps\httplib
git clone --depth 1 --branch release-0.5.2 https://github.com/mity/md4c.git              C:\offline-deps\md4c
git clone --depth 1 --branch v1.5.14   https://github.com/gulrak/filesystem.git          C:\offline-deps\ghc_filesystem
git clone --depth 1 --branch 3.3.8     https://github.com/glfw/glfw.git                  C:\offline-deps\glfw
git clone --depth 1 --branch v2.20.6   https://github.com/ThePhD/sol2.git                C:\offline-deps\sol2
git clone --depth 1 --branch v2.4.11   https://github.com/doctest/doctest.git            C:\offline-deps\doctest
# imgui + imgui_test_engine are pinned to commit SHAs, not tags — clone full and checkout:
git clone https://github.com/ocornut/imgui.git              C:\offline-deps\imgui
git -C C:\offline-deps\imgui checkout 329c5a6b3be75ebf54506d3ae94b836ffcf19fa0
git clone https://github.com/ocornut/imgui_test_engine.git  C:\offline-deps\imgui_test_engine
git -C C:\offline-deps\imgui_test_engine checkout 8568767ad4c53d6ce02d65f01a09d30fb630bd80
```

Then point CMake at the mirror at configure time:

```powershell
cmake --preset ninja-iter-msys2 `
    -DFETCHCONTENT_SOURCE_DIR_JSON="C:/offline-deps/json" `
    -DFETCHCONTENT_SOURCE_DIR_CPR="C:/offline-deps/cpr" `
    -DFETCHCONTENT_SOURCE_DIR_SQLITECPP="C:/offline-deps/sqlitecpp" `
    -DFETCHCONTENT_SOURCE_DIR_HTTPLIB="C:/offline-deps/httplib" `
    -DFETCHCONTENT_SOURCE_DIR_MD4C="C:/offline-deps/md4c" `
    -DFETCHCONTENT_SOURCE_DIR_GHC_FILESYSTEM="C:/offline-deps/ghc_filesystem" `
    -DFETCHCONTENT_SOURCE_DIR_GLFW="C:/offline-deps/glfw" `
    -DFETCHCONTENT_SOURCE_DIR_SOL2="C:/offline-deps/sol2" `
    -DFETCHCONTENT_SOURCE_DIR_DOCTEST="C:/offline-deps/doctest" `
    -DFETCHCONTENT_SOURCE_DIR_IMGUI="C:/offline-deps/imgui" `
    -DFETCHCONTENT_SOURCE_DIR_IMGUI_TEST_ENGINE="C:/offline-deps/imgui_test_engine"
```

**Note the uppercased name** — CMake matches the variable to the `FetchContent_Declare(<name> ...)` first argument, uppercased. The first arg `ghc_filesystem` becomes `GHC_FILESYSTEM`; `imgui_test_engine` becomes `IMGUI_TEST_ENGINE`.

This is the right option when the firewall lets you clone these repos once (e.g. on the side of an SSH bastion) but not on every fresh build.

## Option 3 — `FETCHCONTENT_BASE_DIR` to a network share

If a team shares a network drive that holds a populated FetchContent layout, override the entire base directory once:

```powershell
$env:FETCHCONTENT_BASE_DIR = "\\fileserver\smatchet-deps\ucrt64-iter"
cmake --preset ninja-iter-msys2
```

The base dir contains `<name>-src/` + `<name>-build/` + `<name>-subbuild/` for each FetchContent declaration. Same cache-hit semantics as Option 1 — compiler + generator must match.

This is the right option for a static team that builds frequently against the same preset.

## When NOT to do this

Most contributors should accept the one-time ~5 min cost and skip this doc. The options above add operational complexity (mirror maintenance, drift between `GIT_TAG` pins and mirror state) that isn't worth it for occasional builds.

You want this doc when:

- Your firewall blocks `https://github.com` outright.
- You're cutting fresh worktrees / containers daily and the FetchContent download dominates the workflow.
- You're on a metered or air-gapped network.
- You need bit-exact reproducibility against a fixed mirror snapshot for compliance / audit.

## Related

- [BUILD.md](../../BUILD.md) — top-level build recipes + first-time verification.
- [scripts/dev/doctor.sh](../../scripts/dev/doctor.sh) — toolchain pre-flight checks (Slices 2/3).
- [docs/design/git-to-perforce-migration.md](../design/git-to-perforce-migration.md) — if you're reading this because you're on the Perforce-migration path, that plan eventually vendors every FetchContent dep into `//smatchet/main/third_party/`, killing the network-dependency entirely. Phase 1 of that plan supersedes this doc.
