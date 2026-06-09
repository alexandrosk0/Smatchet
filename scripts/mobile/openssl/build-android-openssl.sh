#!/usr/bin/env bash
#
# build-android-openssl.sh — static OpenSSL for Android (arm64-v8a + x86_64),
# reproducible on Windows with **no MSYS2 and no Strawberry Perl**.
#
# The perl that ships with Git for Windows is stripped of a few pure-perl
# modules OpenSSL's ./Configure pulls in. Rather than install MSYS2 just for
# those, we put the first-party shims in ./perl-shim/ on PERL5LIB (for Configure
# + its end-of-run configdata.pm) and also copy them into the extracted tree so
# the generated Makefile's `perl -I.` recipes find them. perl is invoked as the
# space-free POSIX path (/usr/bin/perl) so the space in "C:\Program Files\Git"
# does not break unquoted $(PERL) recipes. See ./perl-shim/README.md and
# docs/mobile/ANDROID_BUILD.md §3.7 for the full rationale.
#
# Output layout (consumed by cmake/SmatchetThirdParty.cmake via
# SMATCHET_ANDROID_OPENSSL_BASE): <base>/<abi>/{lib,include}.
#
# Required env:
#   ANDROID_NDK_ROOT              path to the NDK (e.g. .../ndk/26.3.11579264)
#   SMATCHET_ANDROID_OPENSSL_BASE output base; per-ABI dirs created under it
# Optional env (defaults shown):
#   OPENSSL_VERSION=3.5.6
#   OPENSSL_SHA256=deae7c80cba99c4b4f940ecadb3c3338b13cb77418409238e57d7f31f2a3b736
#   OPENSSL_TARBALL=<path>        pre-downloaded openssl-<ver>.tar.gz (skips download)
#   ANDROID_API=24
#   MAKE=mingw32-make            native Win32 make (NO MSYS2); 'make' on Linux/macOS
#   MAKE_JOBS=8
#   WORK_DIR=<tmp>               scratch extract/build root
set -euo pipefail

OPENSSL_VERSION="${OPENSSL_VERSION:-3.5.6}"
OPENSSL_SHA256="${OPENSSL_SHA256:-deae7c80cba99c4b4f940ecadb3c3338b13cb77418409238e57d7f31f2a3b736}"
ANDROID_API="${ANDROID_API:-24}"
MAKE_JOBS="${MAKE_JOBS:-8}"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
shim_dir="${script_dir}/perl-shim"

die() { echo "build-android-openssl: $*" >&2; exit 1; }

[ -n "${ANDROID_NDK_ROOT:-}" ] || die "ANDROID_NDK_ROOT is unset"
[ -d "${ANDROID_NDK_ROOT}" ] || die "ANDROID_NDK_ROOT='${ANDROID_NDK_ROOT}' is not a directory"
[ -n "${SMATCHET_ANDROID_OPENSSL_BASE:-}" ] || die "SMATCHET_ANDROID_OPENSSL_BASE is unset"
[ -d "${shim_dir}" ] || die "perl-shim dir missing next to this script: ${shim_dir}"

# Native make: default to mingw32-make on Windows (Git Bash / MSYS uname starts
# MINGW/MSYS), plain make elsewhere. mingw32-make is the JetBrains/CLion or
# standalone MinGW make — NOT pacman's MSYS2 make.
default_make="make"
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) default_make="mingw32-make" ;;
esac
make_prog="${MAKE:-${default_make}}"
command -v "${make_prog}" >/dev/null 2>&1 || die "make program '${make_prog}' not found on PATH"

# perl: prefer the space-free POSIX path so $^X has no space (the C:\Program
# Files\Git\... path breaks unquoted $(PERL) recipes in the generated Makefile).
perl_bin="perl"
[ -x "/usr/bin/perl" ] && perl_bin="/usr/bin/perl"
command -v "${perl_bin}" >/dev/null 2>&1 || die "perl not found"

# Put the NDK clang toolchain on PATH so OpenSSL's android target detects it.
ndk_prebuilt=""
for host in windows-x86_64 linux-x86_64 darwin-x86_64; do
    if [ -d "${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt/${host}/bin" ]; then
        ndk_prebuilt="${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt/${host}/bin"
        break
    fi
done
[ -n "${ndk_prebuilt}" ] || die "could not find NDK llvm prebuilt bin under ${ANDROID_NDK_ROOT}"
export PATH="${ndk_prebuilt}:${PATH}"

objdump_bin="${ndk_prebuilt}/llvm-objdump"
[ -x "${objdump_bin}" ] || objdump_bin="${ndk_prebuilt}/llvm-objdump.exe"

# Shim on PERL5LIB drives Configure proper + its end-of-run configdata.pm run.
# (make recipes get the shim a second way: copied into the tree, found via -I.)
export PERL5LIB="${shim_dir}"
export ANDROID_NDK_ROOT

work_dir="${WORK_DIR:-${TMPDIR:-/tmp}/smatchet-openssl-build}"
mkdir -p "${work_dir}"

# Acquire + verify the source tarball.
tarball="${OPENSSL_TARBALL:-}"
if [ -z "${tarball}" ]; then
    tarball="${work_dir}/openssl-${OPENSSL_VERSION}.tar.gz"
    if [ ! -f "${tarball}" ]; then
        url="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz"
        echo "=== downloading ${url} ==="
        if command -v curl >/dev/null 2>&1; then
            curl -fsSL "${url}" -o "${tarball}"
        elif command -v wget >/dev/null 2>&1; then
            wget -qO "${tarball}" "${url}"
        else
            die "no curl/wget available to fetch OpenSSL; set OPENSSL_TARBALL"
        fi
    fi
fi
[ -f "${tarball}" ] || die "tarball not found: ${tarball}"

actual_sha="$(sha256sum "${tarball}" | cut -d' ' -f1)"
[ "${actual_sha}" = "${OPENSSL_SHA256}" ] || \
    die "sha256 mismatch on ${tarball}: got ${actual_sha} want ${OPENSSL_SHA256}"
echo "=== sha256 OK: ${actual_sha} ==="

# OpenSSL Configure target  ->  Android ABI dir (CMake derives <base>/<abi>).
build_one() {
    target="$1"
    abi="$2"
    prefix="${SMATCHET_ANDROID_OPENSSL_BASE}/${abi}"
    abi_build="${work_dir}/${target}"

    echo "=== [${abi}] extract ==="
    rm -rf "${abi_build}"
    mkdir -p "${abi_build}"
    tar xzf "${tarball}" -C "${abi_build}"
    src="${abi_build}/openssl-${OPENSSL_VERSION}"
    [ -d "${src}" ] || die "extracted source dir missing: ${src}"

    # Second copy of the shim so the generated Makefile's `perl -I.` recipes
    # resolve it (mingw32-make mangles the PERL5LIB path, so -I. is the reliable one).
    cp -r "${shim_dir}/." "${src}/"

    echo "=== [${abi}] Configure (${target}) ==="
    (
        cd "${src}"
        "${perl_bin}" ./Configure "${target}" \
            "-D__ANDROID_API__=${ANDROID_API}" \
            no-shared no-tests no-apps no-docs \
            --libdir=lib --prefix="${prefix}"
    )

    echo "=== [${abi}] ${make_prog} -j${MAKE_JOBS} ==="
    "${make_prog}" -C "${src}" -j"${MAKE_JOBS}"

    echo "=== [${abi}] install_sw -> ${prefix} ==="
    rm -rf "${prefix}"
    "${make_prog}" -C "${src}" install_sw

    echo "=== [${abi}] arch check (llvm-objdump -f) ==="
    for lib in libcrypto.a libssl.a; do
        arch="$("${objdump_bin}" -f "${prefix}/lib/${lib}" 2>/dev/null | grep -m1 'architecture' || true)"
        echo "  ${lib}: ${arch}"
        case "${target}:${arch}" in
            android-arm64:*aarch64*) ;;
            android-x86_64:*x86?64*) ;;
            *) die "[${abi}] unexpected architecture for ${lib}: '${arch}'" ;;
        esac
    done
    echo "=== [${abi}] DONE ==="
}

build_one android-arm64  arm64-v8a
build_one android-x86_64 x86_64

echo "ALL_DONE_OK — OpenSSL ${OPENSSL_VERSION} static libs under ${SMATCHET_ANDROID_OPENSSL_BASE}/{arm64-v8a,x86_64}"
