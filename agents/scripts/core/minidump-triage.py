#!/usr/bin/env python3
"""minidump-triage.py — dependency-free Windows minidump (.dmp) triage.

Extracts the two highest-signal facts from a Windows minidump WITHOUT a native
debugger (cdb/WinDbg/kd) or any pip package:

  * the EXCEPTION RECORD — exception code (e.g. 0xC0000005 access violation),
    faulting address, and the crashing thread id (MINIDUMP_EXCEPTION_STREAM, 6);
  * the MODULE LIST — loaded module base address, size, and name, so the
    faulting address can be attributed to a module (MINIDUMP_MODULE_LIST, 4).

This is the hand-rolled scan a debug-detective previously re-derived per incident
(tooling backlog no-cdb-windbg-dump-triage-script). For a SYMBOLIZED stack use
cdb instead — canonical invocation and the (non-trivial) install recipe both in
docs/agent-rules/debug-techniques.md § Installing cdb. Not the bare
`-c "!analyze -v; q"` form: it walks only the faulting thread, which is the wrong
one on a hang dump. This script is the no-debugger fallback.

Minidump layout (see MINIDUMP_HEADER / MINIDUMP_DIRECTORY in DbgHelp.h):
  header @0: 'MDMP' magic, version, stream count, stream-directory RVA.
  directory: StreamCount * (StreamType u32, DataSize u32, RVA u32).
Only little-endian (every Windows minidump) is handled.

Usage:
  minidump-triage.py <dump.dmp>     triage one dump (text report on stdout)
  minidump-triage.py --selftest     synthesize a tiny fixture dump + assert
Exit: 0 ok / clean parse · 1 parse error / bad dump · 2 file not found.
"""
import struct
import sys

MINIDUMP_MAGIC = b"MDMP"
STREAM_MODULE_LIST = 4
STREAM_EXCEPTION = 6

# A small subset of NTSTATUS exception codes for human-readable triage.
EXCEPTION_NAMES = {
    0xC0000005: "EXCEPTION_ACCESS_VIOLATION",
    0xC00000FD: "EXCEPTION_STACK_OVERFLOW",
    0xC000001D: "EXCEPTION_ILLEGAL_INSTRUCTION",
    0xC0000094: "EXCEPTION_INT_DIVIDE_BY_ZERO",
    0x80000003: "EXCEPTION_BREAKPOINT",
    0xC0000409: "STATUS_STACK_BUFFER_OVERRUN",
    0xE06D7363: "MSVC C++ exception (0xE06D7363)",
}


class DumpError(Exception):
    pass


def _u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def _u64(buf, off):
    return struct.unpack_from("<Q", buf, off)[0]


def _read_minidump_string(buf, rva):
    # MINIDUMP_STRING: u32 byte-length, then UTF-16LE (no NUL terminator counted).
    if rva == 0 or rva + 4 > len(buf):
        return ""
    nbytes = _u32(buf, rva)
    start = rva + 4
    end = start + nbytes
    if end > len(buf):
        end = len(buf)
    try:
        return buf[start:end].decode("utf-16-le", errors="replace")
    except Exception:
        return ""


def parse_directory(buf):
    """Return {stream_type: (data_size, rva)} from the stream directory."""
    if len(buf) < 32 or buf[:4] != MINIDUMP_MAGIC:
        raise DumpError(
            "not a Windows minidump (missing 'MDMP' magic) — "
            "got %r" % (buf[:4],)
        )
    stream_count = _u32(buf, 8)
    dir_rva = _u32(buf, 12)
    streams = {}
    for i in range(stream_count):
        ent = dir_rva + i * 12
        if ent + 12 > len(buf):
            raise DumpError("stream directory truncated at entry %d" % i)
        stype = _u32(buf, ent)
        dsize = _u32(buf, ent + 4)
        rva = _u32(buf, ent + 8)
        streams[stype] = (dsize, rva)
    return streams


def parse_exception(buf, rva):
    """MINIDUMP_EXCEPTION_STREAM: u32 ThreadId, u32 pad, then MINIDUMP_EXCEPTION.

    MINIDUMP_EXCEPTION: u32 Code, u32 Flags, u64 Record, u64 Address, ...
    """
    if rva + 8 + 24 > len(buf):
        raise DumpError("exception stream truncated")
    thread_id = _u32(buf, rva)
    exc_off = rva + 8
    code = _u32(buf, exc_off)
    flags = _u32(buf, exc_off + 4)
    address = _u64(buf, exc_off + 16)
    return {
        "thread_id": thread_id,
        "code": code,
        "flags": flags,
        "address": address,
        "name": EXCEPTION_NAMES.get(code, "UNKNOWN"),
    }


def parse_modules(buf, rva):
    """MINIDUMP_MODULE_LIST: u32 count, then count * MINIDUMP_MODULE (108 bytes).

    MINIDUMP_MODULE: u64 BaseOfImage, u32 SizeOfImage, u32 CheckSum,
                     u32 TimeDateStamp, u32 ModuleNameRva, ... (we only need
                     base/size/name).
    """
    if rva + 4 > len(buf):
        raise DumpError("module list stream truncated")
    count = _u32(buf, rva)
    MODULE_SIZE = 108
    mods = []
    for i in range(count):
        m = rva + 4 + i * MODULE_SIZE
        if m + 24 > len(buf):
            raise DumpError("module entry %d truncated" % i)
        base = _u64(buf, m)
        size = _u32(buf, m + 8)
        name_rva = _u32(buf, m + 20)
        name = _read_minidump_string(buf, name_rva)
        mods.append({"base": base, "size": size, "name": name})
    return mods


def triage(path):
    try:
        with open(path, "rb") as fh:
            buf = fh.read()
    except FileNotFoundError:
        print("minidump-triage: file not found: %s" % path, file=sys.stderr)
        return 2
    except OSError as exc:
        print("minidump-triage: cannot read %s: %s" % (path, exc), file=sys.stderr)
        return 2

    try:
        streams = parse_directory(buf)
    except DumpError as exc:
        print("minidump-triage: %s" % exc, file=sys.stderr)
        return 1

    lines = []
    lines.append("== minidump triage: %s (%d bytes, %d streams) ==" % (
        path, len(buf), len(streams)))

    # --- Exception record ---
    if STREAM_EXCEPTION in streams:
        try:
            exc = parse_exception(buf, streams[STREAM_EXCEPTION][1])
            lines.append("")
            lines.append("EXCEPTION:")
            lines.append("  code      : 0x%08X  (%s)" % (exc["code"], exc["name"]))
            lines.append("  address   : 0x%016X" % exc["address"])
            lines.append("  thread id : %d" % exc["thread_id"])
            lines.append("  flags     : 0x%08X" % exc["flags"])
            faulting = _module_for_address(buf, streams, exc["address"])
            if faulting:
                lines.append("  in module : %s (base 0x%016X)" % (
                    faulting["name"], faulting["base"]))
        except DumpError as exc:
            lines.append("EXCEPTION: <unparseable: %s>" % exc)
    else:
        lines.append("")
        lines.append("EXCEPTION: <no exception stream — not a crash dump?>")

    # --- Module list ---
    if STREAM_MODULE_LIST in streams:
        try:
            mods = parse_modules(buf, streams[STREAM_MODULE_LIST][1])
            lines.append("")
            lines.append("MODULES (%d):" % len(mods))
            for mod in mods:
                short = mod["name"].rsplit("\\", 1)[-1] or mod["name"]
                lines.append("  0x%016X +0x%08X  %s" % (
                    mod["base"], mod["size"], short))
        except DumpError as exc:
            lines.append("MODULES: <unparseable: %s>" % exc)
    else:
        lines.append("")
        lines.append("MODULES: <no module-list stream>")

    print("\n".join(lines))
    return 0


def _module_for_address(buf, streams, addr):
    if STREAM_MODULE_LIST not in streams:
        return None
    try:
        mods = parse_modules(buf, streams[STREAM_MODULE_LIST][1])
    except DumpError:
        return None
    for mod in mods:
        if mod["base"] <= addr < mod["base"] + mod["size"]:
            return mod
    return None


def _build_fixture():
    """Synthesize a minimal-but-valid minidump with an exception + 1 module."""
    # Layout: header(32) | dir(2*12) | exception stream | module list | strings.
    HEADER = 32
    DIRENT = 12
    dir_rva = HEADER
    body = dir_rva + 2 * DIRENT

    # Exception stream: u32 ThreadId, u32 pad, MINIDUMP_EXCEPTION
    # (u32 code, u32 flags, u64 record, u64 address, u32 nparams, u32 align,
    #  15*u64 params) — we write the leading, meaningful prefix + pad.
    exc_rva = body
    exc = struct.pack("<II", 4242, 0)  # ThreadId=4242, pad
    exc += struct.pack("<II", 0xC0000005, 0)  # code=AV, flags
    exc += struct.pack("<Q", 0)  # ExceptionRecord
    exc += struct.pack("<Q", 0x0000000140001234)  # ExceptionAddress (in module)
    exc += struct.pack("<I", 0)  # NumberParameters
    exc += struct.pack("<I", 0)  # __unusedAlignment
    exc += struct.pack("<Q", 0) * 15  # ExceptionInformation[15]
    exc_size = len(exc)

    # Module list: u32 count, then MINIDUMP_MODULE(108) with name RVA.
    mod_rva = exc_rva + exc_size
    name_rva = mod_rva + 4 + 108  # MINIDUMP_STRING right after the module record
    mod = struct.pack("<I", 1)  # count
    module = struct.pack("<Q", 0x0000000140000000)  # BaseOfImage
    module += struct.pack("<I", 0x00010000)  # SizeOfImage (covers addr above)
    module += struct.pack("<I", 0)  # CheckSum
    module += struct.pack("<I", 0)  # TimeDateStamp
    module += struct.pack("<I", name_rva)  # ModuleNameRva
    module += b"\x00" * (108 - len(module))  # pad to 108
    mod += module
    mod_size = len(mod)

    # MINIDUMP_STRING for the module name (u32 byte-len + UTF-16LE).
    name = "Smatchet.exe".encode("utf-16-le")
    string_blob = struct.pack("<I", len(name)) + name

    # Header: magic, version, StreamCount, DirRva, CheckSum, TimeDateStamp, Flags
    header = MINIDUMP_MAGIC
    header += struct.pack("<I", 0xA793)  # version (MINIDUMP_VERSION lo)
    header += struct.pack("<I", 2)  # NumberOfStreams
    header += struct.pack("<I", dir_rva)  # StreamDirectoryRva
    header += struct.pack("<I", 0)  # CheckSum
    header += struct.pack("<I", 0)  # TimeDateStamp
    header += struct.pack("<Q", 0)  # Flags
    header = header[:HEADER]

    directory = struct.pack("<III", STREAM_EXCEPTION, exc_size, exc_rva)
    directory += struct.pack("<III", STREAM_MODULE_LIST, mod_size, mod_rva)

    return header + directory + exc + mod + string_blob


def selftest():
    import io
    import tempfile
    import os

    blob = _build_fixture()
    fd, path = tempfile.mkstemp(suffix=".dmp")
    os.close(fd)
    try:
        with open(path, "wb") as fh:
            fh.write(blob)

        # 1. Directory parse finds both streams.
        streams = parse_directory(blob)
        assert STREAM_EXCEPTION in streams, "exception stream not found"
        assert STREAM_MODULE_LIST in streams, "module list not found"

        # 2. Exception fields decode correctly.
        exc = parse_exception(blob, streams[STREAM_EXCEPTION][1])
        assert exc["code"] == 0xC0000005, "code=%#x" % exc["code"]
        assert exc["name"] == "EXCEPTION_ACCESS_VIOLATION"
        assert exc["thread_id"] == 4242, exc["thread_id"]
        assert exc["address"] == 0x0000000140001234, "%#x" % exc["address"]

        # 3. Module list + name decode; faulting addr attributes to the module.
        mods = parse_modules(blob, streams[STREAM_MODULE_LIST][1])
        assert len(mods) == 1, len(mods)
        assert mods[0]["name"] == "Smatchet.exe", mods[0]["name"]
        owner = _module_for_address(blob, streams, exc["address"])
        assert owner and owner["name"] == "Smatchet.exe", "addr not in module"

        # 4. Full triage() prints a report and returns 0 on the fixture.
        cap = io.StringIO()
        old = sys.stdout
        sys.stdout = cap
        try:
            rc = triage(path)
        finally:
            sys.stdout = old
        assert rc == 0, "triage rc=%s" % rc
        out = cap.getvalue()
        assert "EXCEPTION_ACCESS_VIOLATION" in out
        assert "Smatchet.exe" in out

        # 5. A non-minidump blob is rejected (rc 1), not crashed.
        # selftest: asserts-failure — a bad (non-MDMP) blob MUST triage to rc 1, not 0.
        bad_fd, bad_path = tempfile.mkstemp(suffix=".dmp")
        os.close(bad_fd)
        try:
            with open(bad_path, "wb") as fh:
                fh.write(b"NOTADUMP" + b"\x00" * 64)
            old_err = sys.stderr
            sys.stdout = io.StringIO()
            sys.stderr = io.StringIO()  # the expected "not a minidump" diag
            try:
                rc_bad = triage(bad_path)
            finally:
                sys.stdout = old
                sys.stderr = old_err
            assert rc_bad == 1, "bad-dump rc=%s (expected 1)" % rc_bad
        finally:
            os.unlink(bad_path)
    finally:
        os.unlink(path)

    print("minidump-triage --selftest: PASS")
    print("Passed: 1  Failed: 0")
    return 0


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    if argv[1] in ("--selftest", "-t"):
        try:
            return selftest()
        except AssertionError as exc:
            print("minidump-triage --selftest: FAIL — %s" % exc, file=sys.stderr)
            print("Passed: 0  Failed: 1")
            return 1
    if argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0
    return triage(argv[1])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
