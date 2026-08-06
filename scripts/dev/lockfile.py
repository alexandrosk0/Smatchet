#!/usr/bin/env python3
"""Run a command under an exclusive file lock. Cross-platform `flock(1)` stand-in.

`flock` is a util-linux binary: absent on stock Windows, absent from Git Bash,
and an extra `pacman -S util-linux` step under MSYS2. Every host that can build
Smatchet already has Python, so the lock moves there instead
(`msvcrt.locking` on Windows, `fcntl.flock` on POSIX) and drops a required
tool from the setup matrix.

Usage:
    lockfile.py --lockfile PATH [options] -- CMD [ARG...]
    lockfile.py --lockfile PATH [options] --cmd 'SHELL STRING'

Options:
    --nonblock       Do not wait: if the lock is held, exit --busy-rc at once.
    --timeout SEC    Wait at most SEC seconds for the lock, then exit --busy-rc.
                     Default: wait forever. Mutually exclusive with --nonblock.
    --busy-rc N      Exit code when the lock could not be taken (default 1).
                     Pass 0 for "another holder has it, that is fine, be quiet".

Exit codes:
    The command's own exit code when it ran, --busy-rc when the lock was busy,
    2 on a usage error. 126/127 mirror the shell for not-executable/not-found.

flock equivalences:
    flock -x  FILE -- CMD   ->  lockfile.py --lockfile FILE -- CMD
    flock -n  FILE -- CMD   ->  lockfile.py --lockfile FILE --nonblock --busy-rc 1 -- CMD
    flock -w5 FILE -- CMD   ->  lockfile.py --lockfile FILE --timeout 5 -- CMD

The lock is advisory and released when the process exits, including on a crash
or a kill -9 — the OS drops it with the file handle, so a dead holder cannot
wedge the queue.
"""

import argparse
import errno
import os
import subprocess
import sys
import time

if os.name == "nt":
    import msvcrt
else:
    import fcntl

POLL_SECONDS = 0.05

# The errnos that mean "another process holds it", not "the lock is broken".
# POSIX flock(2) is documented to use either EACCES or EAGAIN inconsistently
# across kernels; Windows _locking() reports contention as EACCES. Anything
# else (ENOLCK, EIO, EBADF, ...) is a real fault and must not read as busy.
CONTENTION_ERRNOS = (errno.EACCES, errno.EAGAIN)


def _try_lock(fd):
    """Attempt a non-blocking exclusive lock. True on success, False if held."""
    try:
        if os.name == "nt":
            # Windows locks byte ranges, not whole files: lock one byte at
            # offset 0. Every participant locks the same byte, so the
            # exclusion is total. LK_NBLCK is the non-blocking variant
            # (LK_LOCK would retry internally for ~10s and raise after).
            os.lseek(fd, 0, os.SEEK_SET)
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
        else:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as exc:
        if exc.errno in CONTENTION_ERRNOS:
            return False
        raise
    return True


def _acquire(fd, nonblock, timeout):
    """Take the lock, honouring --nonblock / --timeout. True if acquired."""
    if _try_lock(fd):
        return True
    if nonblock:
        return False
    deadline = None if timeout is None else time.time() + timeout
    while deadline is None or time.time() < deadline:
        time.sleep(POLL_SECONDS)
        if _try_lock(fd):
            return True
    return False


def main(argv):
    parser = argparse.ArgumentParser(
        prog="lockfile.py",
        description="Run a command under an exclusive file lock.",
    )
    parser.add_argument("--lockfile", required=True, help="Lock file path (created if absent).")
    parser.add_argument("--cmd", help="Command as a single shell string.")
    parser.add_argument("--nonblock", action="store_true", help="Fail immediately if the lock is held.")
    parser.add_argument("--timeout", type=float, help="Seconds to wait for the lock (default: forever).")
    parser.add_argument("--busy-rc", type=int, default=1, help="Exit code when the lock is busy (default 1).")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="Command argv after --.")
    args = parser.parse_args(argv)

    if args.nonblock and args.timeout is not None:
        parser.error("--nonblock and --timeout are mutually exclusive")

    # argparse.REMAINDER keeps the leading "--" when one was passed.
    argv_cmd = args.command[1:] if args.command[:1] == ["--"] else args.command
    if bool(args.cmd) == bool(argv_cmd):
        parser.error("pass exactly one of --cmd 'string' or -- CMD [ARG...]")

    lockdir = os.path.dirname(os.path.abspath(args.lockfile))
    if lockdir and not os.path.isdir(lockdir):
        os.makedirs(lockdir, exist_ok=True)

    # O_RDWR (not O_WRONLY): msvcrt.locking needs a readable+writable handle.
    fd = os.open(args.lockfile, os.O_RDWR | os.O_CREAT, 0o644)
    try:
        if not _acquire(fd, args.nonblock, args.timeout):
            return args.busy_rc
        try:
            if args.cmd:
                return subprocess.call(args.cmd, shell=True)
            return subprocess.call(argv_cmd)
        except OSError as exc:
            sys.stderr.write("lockfile.py: cannot run command: %s\n" % exc)
            # Mirror the shell: 126 = found but not executable, 127 = not found.
            return 126 if exc.errno == errno.EACCES else 127
    finally:
        # Closing the descriptor releases the lock on both platforms; an
        # explicit unlock first keeps the Windows byte-range bookkeeping tidy.
        try:
            if os.name == "nt":
                os.lseek(fd, 0, os.SEEK_SET)
                msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(fd, fcntl.LOCK_UN)
        except OSError:
            pass
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
