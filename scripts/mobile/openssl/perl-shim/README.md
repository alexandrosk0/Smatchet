# `perl-shim/` — Git-for-Windows perl modules for OpenSSL `./Configure`

This directory is put on `PERL5LIB` (by [`../build-android-openssl.sh`](../build-android-openssl.sh))
so the **perl that ships with Git for Windows** can run OpenSSL's `./Configure` for the
Android cross-build — **without installing MSYS2 or Strawberry Perl**.

## Why it exists

Git for Windows bundles a deliberately *stripped* perl (`v5.38.2` at
`/usr/bin/perl`). OpenSSL's `Configure` pulls a chain of pure-perl modules that the
strip removed:

```
Configure  ->  use OpenSSL::config  ->  use IPC::Cmd  ->  require Params::Check
   Params::Check:6   use Locale::Maketext::Simple        # MISSING
   IPC::Cmd::can_run ->  require ExtUtils::MakeMaker; MM->maybe_command(...)   # MISSING
configdata.pm (generated, auto-run)  ->  use Pod::Usage  # MISSING
```

All three are **pure-perl** (no XS / no compiled component), so providing them on
`PERL5LIB` is enough — nothing needs to be built.

## Why shims, not the real modules

The real `ExtUtils::MakeMaker` drags in a large `MM_Unix` / `MM_Any` / `Pod::*`
cascade (~17 files) that OpenSSL only ever touches at **one** point:
`IPC::Cmd::can_run` -> `MM->maybe_command($path)`, a five-line "is this path an
executable file" test. So instead of vendoring third-party (GPL/Artistic) perl into
the repo we provide minimal, first-party shims for exactly the three leaves OpenSSL
reaches:

| File | Replaces | OpenSSL only needs |
|---|---|---|
| `Locale/Maketext/Simple.pm` | the i18n front-end | a gettext-style `loc()` for (rarely-hit) error text |
| `ExtUtils/MakeMaker.pm` | the whole MakeMaker tree | `MM->maybe_command($path)` |
| `Pod/Usage.pm` | the POD-rendering usage printer | a load-safe `pod2usage()` (never called on a normal Configure run) |

## The other half of the fix (not in this dir)

A shim alone is not enough — Git's perl lives at `C:\Program Files\Git\...`, and the
**space** in that path breaks OpenSSL's generated `Makefile` (unquoted `$(PERL)`
recipes fail with `/usr/bin/sh: /c/Program: No such file`). The build script sidesteps
this by invoking perl as `/usr/bin/perl` (whose `$^X` is the space-free POSIX path)
rather than the full `C:\Program Files\...` path. Both halves are required; see
[`docs/mobile/ANDROID_BUILD.md` §3.7](../../../../docs/mobile/ANDROID_BUILD.md#37-build-static-openssl-for-both-abis).

## Pinned to OpenSSL 3.5.6

These shims cover exactly what `./Configure` from OpenSSL **3.5.6** requires. A major
OpenSSL bump could reach further into these modules; if `Configure` starts failing
with a new `Can't locate .../X.pm`, extend the matching shim (or vendor that one leaf).
