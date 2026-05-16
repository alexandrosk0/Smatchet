#!/usr/bin/env python3
"""Filter compile_commands to first-party TUs and run cppcheck (see backlog/CPPCHECK_PLAN.md)."""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

TOP_LEVEL_DIRS = ("Source_Core", "Plugins", "Target_Standalone")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--compile-db",
        type=Path,
        default=Path("build/ninja-debug-msys2/compile_commands.json"),
        help="Path to compile_commands.json from CMake (CMAKE_EXPORT_COMPILE_COMMANDS=ON)",
    )
    p.add_argument(
        "--out-db",
        type=Path,
        default=Path("build/compile_commands.cppcheck.json"),
        help="Filtered compile DB written here (UTF-8, no BOM)",
    )
    p.add_argument(
        "--cache-dir",
        type=Path,
        default=Path("build/cppcheck-cache-source"),
        help="cppcheck --cppcheck-build-dir",
    )
    p.add_argument("-j", type=int, default=8, help="Parallel jobs for cppcheck")
    p.add_argument("--no-run", action="store_true", help="Only write filtered DB, skip cppcheck")
    args = p.parse_args()

    root = Path(__file__).resolve().parents[2]
    src_db = root / args.compile_db if not args.compile_db.is_absolute() else args.compile_db
    out_db = root / args.out_db if not args.out_db.is_absolute() else args.out_db
    cache_dir = root / args.cache_dir if not args.cache_dir.is_absolute() else args.cache_dir

    if not src_db.is_file():
        print(f"error: compile DB not found: {src_db}", file=sys.stderr)
        return 1

    with src_db.open(encoding="utf-8") as f:
        j = json.load(f)

    def keep(entry: dict) -> bool:
        abs_path = Path(entry["file"]).resolve()
        try:
            rel = abs_path.relative_to(root)
        except ValueError:
            return False  # outside repo (FetchContent _deps, system headers)
        parts = rel.parts
        if not parts or parts[0] not in TOP_LEVEL_DIRS:
            return False
        return "_deps" not in parts

    filt = [e for e in j if keep(e)]
    out_db.parent.mkdir(parents=True, exist_ok=True)
    with out_db.open("w", encoding="utf-8") as f:
        json.dump(filt, f, indent=2)
    print(f"wrote {len(filt)} entries -> {out_db.relative_to(root)}")

    if args.no_run:
        return 0

    cache_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        "cppcheck",
        f"--project={out_db}",
        "--suppress=*:*_deps*",
        "--suppress=*:*ThirdParty*",
        "--suppress=missingIncludeSystem",
        "--suppress=missingInclude",
        "--enable=warning,style,performance,portability",
        "--inline-suppr",
        "-j",
        str(args.j),
        f"--cppcheck-build-dir={cache_dir}",
        "--template={file}\t{line}\t{column}\t{severity}\t{id}\t{message}",
    ]
    print("running:", " ".join(cmd))
    r = subprocess.run(cmd, cwd=root)
    return int(r.returncode if r.returncode is not None else 1)


if __name__ == "__main__":
    raise SystemExit(main())
