#!/usr/bin/env python3
"""AppController.h fan-in ratchet for the `appcontroller-fan-in` gate.

Sibling of include_cycle_audit.py / dup_audit.py: counts the **quote-form**
`#include "AppController.h"` includers across `Source/` and FAILs on a regression
above the merge-base count. This is the *fan-in* lever (too many TUs depend on one
god-header) — distinct from include_cycle_audit.py's acyclicity/layer-DAG lever.

Verdict model — FAILS CLOSED (hard-FAIL, NOT WARN-first). Unlike the `duplication` /
`unused-symbol` calibration gates (fuzzy per-file text proxies), "a new TU
`#include`d AppController.h" is an exact, near-zero-false-positive signal and is
*precisely* the regression to block on sight (grill decision, docs/plans/appcontroller-fan-in.md).

Delta semantics (ratchet DOWN only): `--diff <ref>` counts includers at HEAD vs the
merge-base of <ref>. HEAD count <= base count -> pass (flat or ratcheting down). HEAD
count > base count -> FAIL, listing the NEW includer file(s) (those in HEAD's set but
not base's). A `// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=...; owner=...;
revisit=...)` on the nearest non-blank line ABOVE the offending `#include "AppController.h"`
suppresses that one new includer (the rare genuinely-needed new dependency).

Scope is `Source/`-WIDE on purpose (Standalone / Mobile / Plugins includers count too) —
deliberately NOT include_cycle_audit._in_scope, whose SWEEP_ROOTS is `Source/Core/` only
and would undercount the fan-in.

Modes:
  appcontroller_fan_in_audit.py                 # human report: current includer count + list
  appcontroller_fan_in_audit.py --list          # one includer repo-path per line
  appcontroller_fan_in_audit.py --diff <ref>    # DELTA gate: exit 1 on a NEW includer (FAILS CLOSED)
  appcontroller_fan_in_audit.py --selftest      # assert count/regression logic + live baseline + AGENTS.md row

Exit contract (so test-lint-rules.sh fails CLOSED): 0 = clean / ratcheted-down,
1 = NEW includer (blocking), >=2 = infra error (main wraps in try/except -> exit 2).

See docs/plans/appcontroller-fan-in.md (Phase 1 + fan-in ratchet gate).
"""

import argparse
import os
import re
import subprocess
import sys

# --- scope ------------------------------------------------------------------------------------
# Source/-WIDE (not just Source/Core/) so Standalone / Mobile / Plugins includers are counted.
SWEEP_ROOT = "Source/"
EXCLUDE_SUBSTR = ("/ThirdParty/", "ThirdParty/", ".generated.", "/generated/", "/Generated/")
CPP_EXT = (".cpp", ".h", ".hpp", ".cc", ".cxx")

RULE = "app-controller-fan-in"
TARGET_BASENAME = "AppController.h"

# Documented current fan-in (informational doc reference; the .cpp/.h split drifts so it is not
# asserted). The DOWN-only ratchet is enforced by `--diff` vs merge-base, NOT by this constant, so
# a legitimate concurrent includer addition does not need a same-PR bump — `--selftest` only WARNs
# on drift (it broke twice as a hard assert: 113->114 at authoring, then 114->115 when the omnibar
# feature #1261 landed a new includer right after). Bump opportunistically (e.g. after a phase that
# reduces fan-in) together with the AGENTS.md row.
BASELINE_FAN_IN = 115

# Quote-form include whose spelling's BASENAME is AppController.h (bare `"AppController.h"` or a
# path-qualified `"../include/AppController.h"`). Excludes AppControllerImpl.h etc. by basename.
_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')


def _is_target_include(spec):
    return os.path.basename(spec.replace("\\", "/")) == TARGET_BASENAME


def includer_lines(text):
    """Return the 1-based line numbers in `text` that `#include "...AppController.h"` (quote-form)."""
    out = []
    for i, line in enumerate(text.split("\n"), start=1):
        m = _INCLUDE_RE.match(line)
        if m and _is_target_include(m.group(1)):
            out.append(i)
    return out


def includer_set(file_texts):
    """{path -> [linenos]} for every in-scope file that includes AppController.h (>=1 match)."""
    out = {}
    for path, text in file_texts.items():
        lns = includer_lines(text)
        if lns:
            out[path] = lns
    return out


# --- git plumbing (UTF-8 forced — mirror include_cycle_audit.py) -------------------------------

def _git(args):
    p = subprocess.run(["git"] + args, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if p.returncode != 0:
        raise RuntimeError("git %s failed (%d): %s" % (" ".join(args), p.returncode, p.stderr.strip()))
    return p.stdout


def _git_ok(args):
    p = subprocess.run(["git"] + args, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    return p.stdout if p.returncode == 0 else ""


def _in_scope(f):
    f = f.replace("\\", "/")
    return (f.endswith(CPP_EXT) and f.startswith(SWEEP_ROOT)
            and not any(s in f for s in EXCLUDE_SUBSTR))


def list_head_files():
    out = _git(["ls-files", SWEEP_ROOT + "**"])
    if not out.strip():
        out = _git(["ls-files"])
    return sorted({f.replace("\\", "/") for f in out.splitlines() if _in_scope(f)})


def list_ref_files(ref):
    out = _git(["ls-tree", "-r", "--name-only", ref])
    return sorted({f.replace("\\", "/") for f in out.splitlines() if _in_scope(f)})


def _read_head(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def texts_head():
    out = {}
    for f in list_head_files():
        text = _read_head(f)
        if text:
            out[f] = text
    return out


def texts_ref(ref):
    out = {}
    for f in list_ref_files(ref):
        text = _git_ok(["show", "%s:%s" % (ref, f)])
        if text:
            out[f] = text
    return out


# --- deviation suppression (mirror include_cycle_audit._suppressed) ----------------------------

def _has_deviation(line):
    if "SMATCHET_DEVIATION" not in line:
        return False
    m = re.search(r"rule=([A-Za-z0-9_,-]+)", line)
    return bool(m) and RULE in [r.strip() for r in m.group(1).split(",")]


def _suppressed_in_text(text, lineno):
    """True if the nearest non-blank line ABOVE line `lineno` carries the fan-in deviation."""
    lines = text.split("\n")
    idx = lineno - 2  # 0-based line directly above the 1-based include line
    while 0 <= idx < len(lines) and lines[idx].strip() == "":
        idx -= 1
    return 0 <= idx < len(lines) and _has_deviation(lines[idx])


# --- delta gate -------------------------------------------------------------------------------

def _merge_base_or_ref(ref):
    p = subprocess.run(["git", "merge-base", ref, "HEAD"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    mb = p.stdout.strip()
    if p.returncode == 0 and mb:
        return mb
    sys.stderr.write("appcontroller_fan_in_audit: WARN: `git merge-base %s HEAD` did not resolve "
                     "(shallow clone?) — falling back to %s tip; delta may false-flag.\n"
                     % (ref, ref))
    return ref


def regression(base_paths, head_map):
    """Pure core (no git): given the set of base includer paths and the HEAD {path -> (text, linenos)}
    map, return the sorted list of NEW, UN-suppressed includer paths that push the count UP.
    Empty list => pass (flat / ratcheted-down / all-new-suppressed). Shared by run_diff + selftest."""
    head_paths = set(head_map.keys())
    if len(head_paths) <= len(base_paths):
        return []  # ratchet down or flat — never a regression by count
    offenders = []
    for path in sorted(head_paths - base_paths):
        text, lns = head_map[path]
        if all(_suppressed_in_text(text, ln) for ln in lns):
            continue  # genuinely-needed new includer with a fan-in deviation marker
        offenders.append(path)
    return offenders


def run_diff(ref):
    base = _merge_base_or_ref(ref)
    head_inc = includer_set(texts_head())
    base_inc = includer_set(texts_ref(base))
    head_map = {p: (_read_head(p), lns) for p, lns in head_inc.items()}
    offenders = regression(set(base_inc.keys()), head_map)
    if not offenders:
        return 0
    print("FAIL: new `#include \"AppController.h\"` includer(s) vs %s "
          "(fan-in ratchet is DOWN-only):" % ref)
    for p in offenders:
        print("  + %s" % p)
    print("  AppController.h is Risk #1 (god-header fan-in). Depend on a narrower header / interface,")
    print("  or — if a new includer is genuinely unavoidable — add")
    print("  // SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=...; owner=...; revisit=...) "
          "on the line above the #include.")
    return 1


# --- reporting --------------------------------------------------------------------------------

def run_list():
    for p in sorted(includer_set(texts_head()).keys()):
        print(p)
    return 0


def run_report():
    inc = includer_set(texts_head())
    print("## AppController.h fan-in audit — Source/-wide quote-includers (current tree)\n")
    print("- includers: %d (documented baseline %d)" % (len(inc), BASELINE_FAN_IN))
    cpp = sum(1 for p in inc if p.endswith((".cpp", ".cc", ".cxx")))
    print("- split: %d .cpp / %d .h" % (cpp, len(inc) - cpp))
    print("- ratchet: DOWN-only (a new includer FAILS the gate unless it carries a "
          "SMATCHET_DEVIATION(rule=app-controller-fan-in; ...) marker)")
    return 0


# --- selftest ---------------------------------------------------------------------------------

def run_selftest():
    """Assert the count + regression-logic invariants AND the live baseline / AGENTS.md row."""
    miss = 0

    # selftest: asserts-failure — a NEW un-suppressed includer MUST be detected (the gate's job).
    base = {"Source/Core/src/A.cpp"}
    head = {
        "Source/Core/src/A.cpp": ('#include "AppController.h"\n', [1]),
        "Source/Standalone/New.cpp": ('#include "AppController.h"\n', [1]),  # new, no deviation
    }
    if regression(base, head) != ["Source/Standalone/New.cpp"]:
        print("SELFTEST FAIL: a new un-suppressed AppController.h includer was not flagged", file=sys.stderr)
        miss = 1

    # A new includer carrying the fan-in deviation marker is suppressed (no regression).
    head_dev = {
        "Source/Core/src/A.cpp": ('#include "AppController.h"\n', [1]),
        "Source/Standalone/New.cpp": (
            '// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=x; owner=y; revisit=z)\n'
            '#include "AppController.h"\n', [2]),
    }
    if regression(base, head_dev) != []:
        print("SELFTEST FAIL: a deviation-marked new includer was wrongly flagged", file=sys.stderr)
        miss = 1

    # Ratchet DOWN (fewer includers than base) is never a regression.
    if regression({"Source/a.cpp", "Source/b.cpp"},
                  {"Source/a.cpp": ('#include "AppController.h"\n', [1])}) != []:
        print("SELFTEST FAIL: a ratchet-down was wrongly flagged as a regression", file=sys.stderr)
        miss = 1

    # basename matcher: AppControllerImpl.h is NOT a match; bare + path-qualified AppController.h are.
    if _is_target_include("AppControllerImpl.h"):
        print("SELFTEST FAIL: AppControllerImpl.h wrongly matched as AppController.h", file=sys.stderr)
        miss = 1
    if not (_is_target_include("AppController.h") and _is_target_include("../include/AppController.h")):
        print("SELFTEST FAIL: bare/path-qualified AppController.h not matched", file=sys.stderr)
        miss = 1

    # Live baseline: WARN (never FAIL) on drift. The DOWN-only ratchet is enforced by `--diff` vs
    # merge-base, NOT by this constant — so a legitimate concurrent includer add/remove must not break
    # the selftest (it did twice as a hard assert: 113->114 at authoring, 114->115 when omnibar #1261
    # landed). The constant is an informational doc reference; bump it + the AGENTS.md row when
    # convenient. Skipped if not in a git tree.
    try:
        live = len(includer_set(texts_head()))
        if live != BASELINE_FAN_IN:
            print("appcontroller_fan_in_audit: selftest NOTE: live AppController.h fan-in = %d, documented "
                  "BASELINE_FAN_IN = %d — bump the constant + the AGENTS.md row when convenient "
                  "(informational; the --diff merge-base ratchet is the enforcement)."
                  % (live, BASELINE_FAN_IN), file=sys.stderr)
    except Exception as e:
        print("appcontroller_fan_in_audit: selftest WARN: could not count live fan-in (%s)" % e,
              file=sys.stderr)

    # AGENTS.md must document the rule row (delta-gated contract-card).
    try:
        with open("AGENTS.md", "r", encoding="utf-8", errors="replace") as fh:
            if RULE not in fh.read():
                print("SELFTEST FAIL: rule '%s' missing from AGENTS.md" % RULE, file=sys.stderr)
                miss = 1
    except OSError:
        print("appcontroller_fan_in_audit: selftest WARN: AGENTS.md not readable from CWD", file=sys.stderr)

    if miss:
        return 1
    print("selftest: fan-in count/regression logic + live baseline (%d) + AGENTS.md row OK"
          % BASELINE_FAN_IN)
    return 0


def _utf8_stdio():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def main():
    _utf8_stdio()
    ap = argparse.ArgumentParser()
    ap.add_argument("--diff", metavar="REF")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    try:
        if args.selftest:
            sys.exit(run_selftest())
        if args.diff:
            sys.exit(run_diff(args.diff))
        if args.list:
            sys.exit(run_list())
        sys.exit(run_report())
    except Exception as e:  # never crash-as-clean: surface as infra error (>=2)
        print("appcontroller_fan_in_audit: ERROR: %s" % e, file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
