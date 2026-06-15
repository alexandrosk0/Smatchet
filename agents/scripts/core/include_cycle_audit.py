#!/usr/bin/env python3
"""Include-graph acyclicity + layer-DAG analyzer for the `core-include-dag` gate.

Sibling of dup_audit.py / function_size_audit.py: parses the **quote-form** `#include "..."`
edges of first-party `Source/Core/` headers + sources, resolves each include against the CMake
`SmatchetCoreInterface` include roots, builds the include graph, and FAILs on either of two
violation modes:

  (1) any strongly-connected component with > 1 node — a genuine include cycle (Tarjan, pure
      stdlib);
  (2) any edge from a LOWER architecture layer to a STRICTLY HIGHER layer — a layer back-edge.

Layer ranking (path-based; higher number = higher layer — dependencies must flow high->low):

  Ui/                         = 6
  AppController.*             = 5   (top orchestrator — strictly ABOVE Commands; see the plan's
                                     grill outcome: Commands/MainThreadDispatch.h -> AppController.h
                                     is a real back-edge the gate must catch, and it is neither an
                                     SCC>1 cycle nor a low->high edge under a coarse same-layer
                                     grouping — so AppController must rank above Commands)
  Commands/                   = 4
  Tracker/                    = 3
  Sync/ , Persistence/        = 2
  Config/                     = 1
  root include/*.h leaf hdrs  = 0   (no subdir; Diagnostics/Privacy/Imaging also rank 0 — low-level
                                     leaf/utility)

Scope (matches the dup/size audits' first-party scope): quote-form `#include "X.h"` only, under
`Source/Core/` (`include/` + `src/`), excluding ThirdParty + generated. Angle-bracket / system /
third-party includes are out of the DAG by construction (an include that resolves to none of the
roots — e.g. <string>, a cpr/imgui header — is simply not an edge).

Verdict model — FAILS CLOSED (unlike dup's WARN-first): `--diff` exits 1 on a NEW cycle / back-edge.
Delta semantics (grandfathering): a violating edge is keyed by `includer -> resolved-target` (both
repo-relative paths). All violations present at the merge-base are grandfathered; an edge fails only
when it is a violation at HEAD and was NOT a violation at base — i.e. a genuinely new cycle/back-edge.
A `// SMATCHET_DEVIATION(rule=include-cycle; ...)` on the nearest non-blank line ABOVE the offending
`#include` suppresses it (mirrors dup's _has_dup_deviation / _suppressed grammar).

Modes mirror dup_audit.py:

  include_cycle_audit.py                 # human report of current cycle + back-edge violations
  include_cycle_audit.py --list          # one `rule<TAB>includer:line<TAB>target` per violation
  include_cycle_audit.py --baseline-md   # deterministic markdown grandfather snapshot
  include_cycle_audit.py --diff <ref>    # DELTA gate: exit 1 on NEW violations (FAILS CLOSED)
  include_cycle_audit.py --selftest      # assert SCC + layer-rank + back-edge invariants hold

Exit contract (so test-lint-rules.sh fails CLOSED): 0 = clean / grandfathered, 1 = NEW violation
(blocking), >=2 = infra error (main wraps in try/except -> exit 2).

See docs/plans/core-include-dag.md + docs/high-integrity/include-cycle-baseline.md.
"""

import argparse
import os
import re
import subprocess
import sys

# --- scope ------------------------------------------------------------------------------------
# First-party Source/Core only; quote-form includes resolved against the SmatchetCore include
# roots below. (Source/Plugins / Source/Standalone are intentionally NOT in scope — the plan
# DAG-ifies Source/Core's graph specifically.)
SWEEP_ROOTS = ("Source/Core/include/", "Source/Core/src/")
EXCLUDE_SUBSTR = ("/ThirdParty/", "ThirdParty/", ".generated.", "/generated/", "/Generated/")
CPP_EXT = (".cpp", ".h", ".hpp", ".cc", ".cxx")

RULE = "include-cycle"

# --- include-resolution roots (KEEP IN SYNC with CMakeLists.txt SmatchetCoreInterface
# target_include_directories). An `#include "X.h"` resolves by searching these IN ORDER, plus the
# includer's own directory (prepended at resolve time). A bare-name include that resolves to a real
# file under one of these is an edge; one that resolves nowhere (system / third-party) is not. ----
INCLUDE_ROOTS = (
    "Source/Core/include",
    "Source/Core/include/Tracker",
    "Source/Core/include/Sync",
    "Source/Core/include/Persistence",
    "Source/Core/include/Config",
    "Source/Core/include/Ui",
    "Source/Core/include/Diagnostics",
    "Source/Core/include/Privacy",
    "Source/Core/include/Imaging",
)

# Quote-form include only: `#include "X.h"` (NOT `#include <...>`). Leading whitespace allowed; the
# captured group is the include spelling exactly as written (may be bare `X.h` or `Sub/X.h`).
_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')


# --- layer ranking ----------------------------------------------------------------------------

def layer_rank(path):
    """Map a repo-relative Source/Core path to its architecture-layer rank (higher = higher layer).
    Path-based, deterministic. The AppController(5) > Commands(4) split is load-bearing (see the
    module docstring + the plan's grill outcome)."""
    p = path.replace("\\", "/")
    # Strip the include/ or src/ prefix so Ui/Foo.h and src/Ui/Foo.cpp rank identically.
    rel = p
    for pre in ("Source/Core/include/", "Source/Core/src/"):
        if rel.startswith(pre):
            rel = rel[len(pre):]
            break
    base = os.path.basename(rel)
    # AppController.* is the top orchestrator — strictly above Commands/. Match the file by name so
    # both AppController.h and any AppController*.cpp (LuaBindings/LuaStubs) rank 5.
    if base.startswith("AppController"):
        return 5
    if rel.startswith("Ui/"):
        return 6
    if rel.startswith("Commands/"):
        return 4
    if rel.startswith("Tracker/"):
        return 3
    if rel.startswith("Sync/") or rel.startswith("Persistence/"):
        return 2
    if rel.startswith("Config/"):
        return 1
    # Everything else — root include/*.h leaf headers (no subdir) + Diagnostics/Privacy/Imaging
    # leaf/utility — ranks 0 (low-level).
    return 0


# --- include resolution -----------------------------------------------------------------------

def _resolve(spec, includer, known_files):
    """Resolve an `#include "spec"` written in `includer` (repo-relative path) to a repo-relative
    first-party file, or None if it resolves to no first-party root (system / third-party — not an
    edge). Search order: the includer's own directory first, then INCLUDE_ROOTS in order. `spec` may
    be bare (`X.h`) or path-qualified (`Sub/X.h`); both are joined onto each root. `known_files` is
    the set of in-scope repo-relative paths (so resolution is O(1) membership, deterministic across
    HEAD/base)."""
    spec = spec.replace("\\", "/")
    # 1) the includer's own directory.
    inc_dir = os.path.dirname(includer)
    cand = _norm_join(inc_dir, spec)
    if cand in known_files:
        return cand
    # 2) each configured include root, in order.
    for root in INCLUDE_ROOTS:
        cand = _norm_join(root, spec)
        if cand in known_files:
            return cand
    return None


def _norm_join(root, spec):
    """Join + normalize to a forward-slash repo-relative path (collapses `./` and `Sub/../`)."""
    joined = root + "/" + spec if root else spec
    return os.path.normpath(joined).replace("\\", "/")


def parse_includes(path, text):
    """Yield (lineno, spec) for each quote-form #include in `text`. 1-based line numbers."""
    out = []
    for i, line in enumerate(text.split("\n"), start=1):
        m = _INCLUDE_RE.match(line)
        if m:
            out.append((i, m.group(1)))
    return out


# --- graph + Tarjan SCC -----------------------------------------------------------------------

def build_graph(file_texts):
    """Build the include graph from {path -> text}. Returns (edges, adj):
      edges: list of (includer, lineno, target) for every quote-include that resolves to an
             in-scope first-party file (the DAG edges; unresolved includes are dropped).
      adj:   {node -> set(targets)} adjacency for SCC detection.
    """
    known = set(file_texts.keys())
    edges = []
    adj = {f: set() for f in known}
    for path, text in file_texts.items():
        for lineno, spec in parse_includes(path, text):
            target = _resolve(spec, path, known)
            if target is None or target == path:
                continue  # unresolved (system/third-party) or self-include — not a graph edge
            edges.append((path, lineno, target))
            adj[path].add(target)
    return edges, adj


def tarjan_sccs(adj):
    """Tarjan's SCC, pure stdlib, iterative (no recursion -> no depth-limit risk on a big graph).
    Returns a list of components, each a list of nodes; only components are returned (singletons
    included — the caller filters len > 1)."""
    index = {}
    low = {}
    on_stack = set()
    stack = []
    sccs = []
    counter = [0]

    def strongconnect(root):
        # Explicit work-stack of (node, neighbor-iterator-state) frames.
        work = [(root, 0)]
        succ = {}
        while work:
            node, pi = work[-1]
            if pi == 0:
                index[node] = counter[0]
                low[node] = counter[0]
                counter[0] += 1
                stack.append(node)
                on_stack.add(node)
                succ[node] = sorted(adj.get(node, ()))  # sorted -> deterministic
            recursed = False
            neighbors = succ[node]
            while pi < len(neighbors):
                w = neighbors[pi]
                pi += 1
                if w not in index:
                    work[-1] = (node, pi)
                    work.append((w, 0))
                    recursed = True
                    break
                elif w in on_stack:
                    low[node] = min(low[node], index[w])
            if recursed:
                continue
            work[-1] = (node, pi)
            # Done exploring `node`'s neighbors.
            if low[node] == index[node]:
                comp = []
                while True:
                    w = stack.pop()
                    on_stack.discard(w)
                    comp.append(w)
                    if w == node:
                        break
                sccs.append(comp)
            work.pop()
            if work:
                parent = work[-1][0]
                low[parent] = min(low[parent], low[node])

    for n in sorted(adj.keys()):
        if n not in index:
            strongconnect(n)
    return sccs


# --- violation detection ----------------------------------------------------------------------

class Violation(object):
    __slots__ = ("kind", "includer", "lineno", "target", "detail")

    def __init__(self, kind, includer, lineno, target, detail):
        self.kind = kind          # "cycle" | "back-edge"
        self.includer = includer
        self.lineno = lineno
        self.target = target
        self.detail = detail

    def key(self):
        # Path-pair key (line-independent so a reordered include doesn't fabricate a new violation).
        return "%s|%s|%s" % (self.kind, self.includer, self.target)


def _is_header(path):
    """True for a header file (.h/.hpp/...). A .cpp/.cc/.cxx is a translation unit — a graph SINK
    (nothing #includes it), so it can never lie inside an include cycle, and "a .cpp pulls in the
    higher-layer headers it implements against" is the NORMAL dependency direction, not a layer
    violation. The layer-DAG is a header->header constraint (that is what governs compile-time
    coupling + cycles), so only header includers participate in the back-edge / cycle check."""
    p = path.replace("\\", "/")
    return not p.endswith((".cpp", ".cc", ".cxx"))


def find_violations(file_texts):
    """Return a list of Violation for the given {path -> text}. Two modes:
      - back-edge: a HEADER->HEADER edge between two NAMED architecture layers (both rank >= 1)
                   whose includer layer < target layer (dependency flowing low -> high).
      - cycle:     any edge that lies inside an SCC of size > 1 (a true include cycle).

    Why both endpoints must be NAMED layers (rank >= 1): rank 0 is the leaf/unranked floor (root
    `include/*.h` interface/model headers + Diagnostics/Privacy/Imaging utility). Those headers have
    NO position in the Ui>AppController>Commands>Tracker>Sync/Persistence>Config hierarchy, so a
    rank-0 header reaching DOWN into a named subsystem (e.g. ITrackerActivity.h -> Tracker/TrackerError.h,
    GridLiveContext.h -> Tracker/TrackerFieldSchema.h) is a forward dependency of an un-categorized
    header — NOT a low->high layer back-edge. Treating rank 0 as "below Config" would false-flag the
    ~30 such legitimate forward edges (the layer-model bug caught at Phase-0 authoring; see the plan
    § Deviations). The 4 real violations the plan targets are all NAMED->NAMED low->high edges
    (Config->Ui, Commands->AppController, Sync->AppController x2). Cycles (SCC>1) still apply to ALL
    nodes regardless of rank — a header include cycle is always wrong.
    """
    edges, adj = build_graph(file_texts)
    viols = []

    # Back-edges: header->header, both endpoints in a NAMED layer (rank >= 1), includer rank
    # strictly LESS than target rank (dependency flows low -> high).
    for includer, lineno, target in edges:
        if not (_is_header(includer) and _is_header(target)):
            continue
        ir, tr = layer_rank(includer), layer_rank(target)
        if ir >= 1 and tr >= 1 and ir < tr:
            viols.append(Violation(
                "back-edge", includer, lineno, target,
                "layer %d -> %d (%s -> %s)" % (ir, tr, _short(includer), _short(target))))

    # Cycles: any edge whose both endpoints sit in the same SCC of size > 1.
    sccs = [c for c in tarjan_sccs(adj) if len(c) > 1]
    in_cycle = set()
    for comp in sccs:
        in_cycle.update(comp)
    for includer, lineno, target in edges:
        if includer in in_cycle and target in in_cycle:
            viols.append(Violation(
                "cycle", includer, lineno, target,
                "include cycle (SCC of %d files)"
                % next(len(c) for c in sccs if includer in c)))
    return viols


def _short(path):
    p = path.replace("\\", "/")
    for pre in ("Source/Core/include/", "Source/Core/src/"):
        if p.startswith(pre):
            return p[len(pre):]
    return p


# --- git plumbing (UTF-8 forced — French locale strings / smart quotes; the #768 lesson) -------

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
    return (f.endswith(CPP_EXT) and f.startswith(SWEEP_ROOTS)
            and not any(s in f for s in EXCLUDE_SUBSTR))


def list_head_files():
    out = _git(["ls-files"] + [r + "**" for r in SWEEP_ROOTS])
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


# --- delta gate -------------------------------------------------------------------------------

def _merge_base_or_ref(ref):
    p = subprocess.run(["git", "merge-base", ref, "HEAD"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    mb = p.stdout.strip()
    if p.returncode == 0 and mb:
        return mb
    sys.stderr.write("include_cycle_audit: WARN: `git merge-base %s HEAD` did not resolve "
                     "(shallow clone?) — falling back to %s tip; delta may false-flag.\n"
                     % (ref, ref))
    return ref


def _has_deviation(line):
    if "SMATCHET_DEVIATION" not in line:
        return False
    m = re.search(r"rule=([A-Za-z0-9_,-]+)", line)
    return bool(m) and RULE in [r.strip() for r in m.group(1).split(",")]


def _suppressed(includer, lineno):
    """True if a `// SMATCHET_DEVIATION(rule=include-cycle; ...)` sits on the nearest non-blank line
    ABOVE the offending #include (mirrors dup_audit._suppressed's nearest-non-blank-above grammar)."""
    try:
        with open(includer, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.read().split("\n")
    except OSError:
        return False
    idx = lineno - 2  # line ABOVE the include (lineno is 1-based; idx is 0-based one further up)
    while 0 <= idx < len(lines) and lines[idx].strip() == "":
        idx -= 1
    return 0 <= idx < len(lines) and _has_deviation(lines[idx])


def run_diff(ref):
    base = _merge_base_or_ref(ref)
    head_viols = find_violations(texts_head())
    base_keys = {v.key() for v in find_violations(texts_ref(base))}
    new = []
    for v in head_viols:
        if v.key() in base_keys:
            continue  # grandfathered: this violating edge already existed at base
        if _suppressed(v.includer, v.lineno):
            continue
        new.append(v)
    if not new:
        return 0
    print("FAIL: new Source/Core include-graph violation(s) vs %s "
          "(SCC>1 cycle or low->high layer back-edge):" % ref)
    for v in sorted(new, key=lambda v: (v.kind, v.includer, v.target)):
        print("  [%s] %s:%d -> %s  (%s)"
              % (v.kind, _short(v.includer), v.lineno, _short(v.target), v.detail))
    print("  Break the edge (relocate the type to a lower-layer leaf header, or invert via an "
          "interface), or add")
    print("  // SMATCHET_DEVIATION(rule=include-cycle; reason=...; owner=...; revisit=...) above "
          "the #include.")
    return 1


# --- reporting --------------------------------------------------------------------------------

def run_list():
    for v in sorted(find_violations(texts_head()), key=lambda v: (v.kind, v.includer, v.target)):
        print("%s\t%s:%d\t%s" % (RULE, v.includer, v.lineno, v.target))
    return 0


def run_baseline_md():
    viols = sorted(find_violations(texts_head()), key=lambda v: (v.kind, v.includer, v.target))
    print("# Include-cycle — grandfathered baseline")
    print()
    print("_Auto-generated. Do not hand-edit; run "
          "`bash agents/scripts/project/test-lint-rules.sh --include-cycle-baseline` and commit._")
    print("_The gate is a live merge-base delta vs `origin/develop` "
          "(include_cycle_audit.py --diff); this file is the informational ratchet snapshot — each "
          "later phase of `core-include-dag` deletes the line for the edge it kills._")
    print()
    print("## %s (%d grandfathered edges)" % (RULE, len(viols)))
    if viols:
        for v in viols:
            print("- `%s:%d` -> `%s` — %s (`%s`)"
                  % (_short(v.includer), v.lineno, _short(v.target), v.kind, v.detail))
    else:
        print("- (none)")
    print()
    print("## Totals")
    print("- violating edges grandfathered: %d" % len(viols))
    return 0


def run_report():
    viols = find_violations(texts_head())
    edges, _adj = build_graph(texts_head())
    print("## Include-graph audit — Source/Core quote-includes (current tree)\n")
    print("- resolved first-party edges: %d" % len(edges))
    print("- layer ranking: Ui=6 > AppController=5 > Commands=4 > Tracker=3 > "
          "Sync/Persistence=2 > Config=1 > root-leaf=0")
    print("- violations (SCC>1 cycle or low->high back-edge): %d\n" % len(viols))
    print("### Violating edges\n")
    if not viols:
        print("- (none — the graph is a clean layer-DAG)")
    for v in sorted(viols, key=lambda v: (v.kind, v.includer, v.target)):
        print("- [%s] `%s:%d` -> `%s` — %s"
              % (v.kind, _short(v.includer), v.lineno, _short(v.target), v.detail))
    return 0


# --- selftest ---------------------------------------------------------------------------------

def run_selftest():
    """Assert the SCC + layer-rank + back-edge invariants hold (the script's single-source-of-truth):
      * a synthetic 2-node include cycle is detected (SCC > 1);
      * a clean DAG is NOT flagged;
      * a known low->high back-edge fires; a same-layer and a high->low edge do NOT;
      * layer_rank returns the documented ranks.
    """
    miss = 0

    # --- layer_rank documented ranks ---
    rank_cases = [
        ("Source/Core/include/Ui/SmatchetUI.h", 6),
        ("Source/Core/include/AppController.h", 5),
        ("Source/Core/src/AppController_LuaBindings.cpp", 5),
        ("Source/Core/include/Commands/MainThreadDispatch.h", 4),
        ("Source/Core/include/Tracker/JiraClient.h", 3),
        ("Source/Core/include/Sync/TicketSyncService.h", 2),
        ("Source/Core/include/Persistence/LocalCacheManager.h", 2),
        ("Source/Core/include/Config/ConfigManager.h", 1),
        ("Source/Core/include/SmatchetUiModeIds.h", 0),
        ("Source/Core/include/Diagnostics/Foo.h", 0),
    ]
    for path, want in rank_cases:
        got = layer_rank(path)
        if got != want:
            print("SELFTEST FAIL: layer_rank(%s) = %d, want %d" % (path, got, want), file=sys.stderr)
            miss = 1

    # selftest: asserts-failure — a synthetic cycle (here) and a low->high back-edge (below) MUST
    # be detected; these are the gate's flag paths (the violation-emitting branch of find_violations).
    # --- a synthetic 2-node include cycle is detected (SCC > 1) ---
    cyc = {
        "Source/Core/include/A.h": '#include "B.h"\n',
        "Source/Core/include/B.h": '#include "A.h"\n',
    }
    cyc_v = [v for v in find_violations(cyc) if v.kind == "cycle"]
    if not cyc_v:
        print("SELFTEST FAIL: synthetic 2-node A<->B include cycle not detected", file=sys.stderr)
        miss = 1

    # --- a clean DAG is NOT flagged (high -> low only) ---
    dag = {
        "Source/Core/include/Ui/Top.h": '#include "Config/Low.h"\n',
        "Source/Core/include/Config/Low.h": '#include <string>\n',
    }
    if find_violations(dag):
        print("SELFTEST FAIL: clean high->low DAG was flagged", file=sys.stderr)
        miss = 1

    # --- a known low->high back-edge fires (Config(1) -> Ui(6)) ---
    back = {
        "Source/Core/include/Config/ConfigManager.h": '#include "Ui/ModeIds.h"\n',
        "Source/Core/include/Ui/ModeIds.h": '#include <cstdint>\n',
    }
    back_v = [v for v in find_violations(back) if v.kind == "back-edge"]
    if not back_v:
        print("SELFTEST FAIL: low->high back-edge (Config->Ui) not detected", file=sys.stderr)
        miss = 1

    # --- the load-bearing Commands(4) -> AppController(5) back-edge fires ---
    cmd_back = {
        "Source/Core/include/Commands/MainThreadDispatch.h": '#include "AppController.h"\n',
        "Source/Core/include/AppController.h": '#include <functional>\n',
    }
    if not [v for v in find_violations(cmd_back) if v.kind == "back-edge"]:
        print("SELFTEST FAIL: Commands->AppController back-edge not detected", file=sys.stderr)
        miss = 1

    # --- a same-layer edge does NOT fire (Sync(2) -> Persistence(2)) ---
    same = {
        "Source/Core/include/Sync/TicketSyncService.h": '#include "Persistence/LocalCacheManager.h"\n',
        "Source/Core/include/Persistence/LocalCacheManager.h": '#include <string>\n',
    }
    if find_violations(same):
        print("SELFTEST FAIL: same-layer Sync->Persistence edge was flagged", file=sys.stderr)
        miss = 1

    # --- a high -> low edge does NOT fire (AppController(5) -> Config(1)) ---
    hi_lo = {
        "Source/Core/include/AppController.h": '#include "Config/ConfigManager.h"\n',
        "Source/Core/include/Config/ConfigManager.h": '#include <string>\n',
    }
    if find_violations(hi_lo):
        print("SELFTEST FAIL: high->low AppController->Config edge was flagged", file=sys.stderr)
        miss = 1

    # --- an unresolved (angle-bracket / third-party) include is NOT an edge ---
    sys_inc = {
        "Source/Core/include/Config/ConfigManager.h": '#include <nlohmann/json.hpp>\n#include "DoesNotExist.h"\n',
    }
    if find_violations(sys_inc):
        print("SELFTEST FAIL: an unresolved/system include was treated as a graph edge", file=sys.stderr)
        miss = 1

    if miss:
        return 1
    print("selftest: SCC + layer-rank + back-edge invariants hold "
          "(Ui=6 > AppController=5 > Commands=4 > Tracker=3 > Sync/Persistence=2 > Config=1 > leaf=0)")
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
    ap.add_argument("--baseline-md", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    try:
        if args.selftest:
            sys.exit(run_selftest())
        if args.diff:
            sys.exit(run_diff(args.diff))
        if args.list:
            sys.exit(run_list())
        if args.baseline_md:
            sys.exit(run_baseline_md())
        sys.exit(run_report())
    except Exception as e:  # never crash-as-clean: surface as infra error (>=2)
        print("include_cycle_audit: ERROR: %s" % e, file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
