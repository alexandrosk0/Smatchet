#!/usr/bin/env python3
"""st_query.py - query the Sourcetrail .srctrldb code-graph index.

A code-navigation helper for agents: "where is X defined / what calls Y /
what does Z touch" answered from Sourcetrail's libclang-built symbol graph,
without re-parsing the C++ tree. Use this for *exact* symbol/caller/callee
facts (call graph, definition sites, members); fall back to grep for
exhaustive literal/symbol sweeps.

DB built by: Sourcetrail.exe index --full Smatchet.srctrlprj
Default DB : <repo-root>/Smatchet.srctrldb   (override with --db or $SMATCHET_SRCTRLDB)

Subcommands
  def     <name>        definition site(s) of a symbol            (file:line)
  refs    <name>        every reference/use of a symbol           (file:line)
  callers <name>        functions that CALL <name>                (caller @ call-site)
  callees <name>        functions <name> CALLS                    (callee @ call-site)
  members <name>        members of a class/struct/namespace/enum
  bases   <name>        base classes of a class/struct
  derived <name>        classes deriving from <name>
  file    <path-frag>   symbols defined in files matching <frag>
  name    <substr>      symbols whose qualified name contains <substr>
  schema                table counts + enum legend

Name matching: case-insensitive suffix match on the qualified name, so
`send`, `JiraClient::send`, and the fully-qualified form all resolve.
Use `name <substr>` first if unsure of the exact spelling.
"""
import argparse, os, sqlite3, sys

# --- Sourcetrail enum legends (from CoatiSoftware/Sourcetrail sources) ---
NODE_KIND = {
    1: "SYMBOL", 2: "TYPE", 4: "BUILTIN_TYPE", 8: "MODULE", 16: "NAMESPACE",
    32: "PACKAGE", 64: "STRUCT", 128: "CLASS", 256: "INTERFACE",
    512: "ANNOTATION", 1024: "GLOBAL_VARIABLE", 2048: "FIELD", 4096: "FUNCTION",
    8192: "METHOD", 16384: "ENUM", 32768: "ENUM_CONSTANT", 65536: "TYPEDEF",
    131072: "TYPE_PARAMETER", 262144: "FILE", 524288: "MACRO", 1048576: "UNION",
}
EDGE_KIND = {
    1: "MEMBER", 2: "TYPE_USAGE", 4: "USAGE", 8: "CALL", 16: "INHERITANCE",
    32: "OVERRIDE", 64: "TYPE_ARGUMENT", 128: "TEMPLATE_SPECIALIZATION",
    256: "INCLUDE", 512: "IMPORT", 2048: "MACRO_USAGE", 4096: "ANNOTATION_USAGE",
}
# source_location.type
LOC_TOKEN, LOC_SCOPE, LOC_QUALIFIER, LOC_LOCAL, LOC_SIGNATURE = 0, 1, 2, 3, 4
DEF_IMPLICIT, DEF_EXPLICIT = 1, 2

# --- NameHierarchy serialization delimiters (Sourcetrail) ---
_META, _NAME, _PART, _SIG = "\tm", "\tn", "\ts", "\tp"


def decode_name(s):
    """serialized_name -> (qualified_name, signature_or_None).

    Format: <delim>\\tm<el0>\\tn<el1>...  where each el is
    <name>\\ts<prefix>\\tp<postfix>  (prefix=return type, postfix=params).
    """
    if _META in s:
        delim, rest = s.split(_META, 1)
    else:
        delim, rest = "::", s
    if not delim:
        delim = "::"
    names, sig = [], None
    for el in rest.split(_NAME):
        nm, prefix, postfix = el, "", ""
        if _PART in nm:
            nm, tail = nm.split(_PART, 1)
            if _SIG in tail:
                prefix, postfix = tail.split(_SIG, 1)
            else:
                prefix = tail
        names.append(nm)
        if prefix or postfix:
            sig = ("%s %s%s" % (prefix, nm, postfix)).strip()
    qual = delim.join(n for n in names if n)
    return qual, sig


class Index:
    def __init__(self, db):
        self.c = sqlite3.connect(db)
        self.nodes = {}      # id -> (type, qualified, signature, raw)
        self.by_qual = []    # (qualified_lower, id)
        for nid, ntype, raw in self.c.execute(
                "select id, type, serialized_name from node"):
            qual, sig = decode_name(raw)
            self.nodes[nid] = (ntype, qual, sig, raw)
            self.by_qual.append((qual.lower(), nid))

    # -- resolution -------------------------------------------------------
    def resolve(self, name, kinds=None):
        """Return node ids whose qualified name == name or ends with ::name
        (case-insensitive). Optionally filter to a set of NodeKind ints."""
        q = name.lower()
        hits = []
        for ql, nid in self.by_qual:
            if ql == q or ql.endswith("::" + q) or ql.endswith("." + q):
                if kinds is None or self.nodes[nid][0] in kinds:
                    hits.append(nid)
        if not hits:  # fall back to substring so a near-miss still helps
            for ql, nid in self.by_qual:
                if q in ql and (kinds is None or self.nodes[nid][0] in kinds):
                    hits.append(nid)
        return hits

    def kind(self, nid):
        return NODE_KIND.get(self.nodes[nid][0], "?%d" % self.nodes[nid][0])

    def label(self, nid):
        t, qual, sig, _ = self.nodes[nid]
        return "%s  [%s]" % (sig or qual, NODE_KIND.get(t, "?%d" % t))

    # -- locations --------------------------------------------------------
    def _loc_rows(self, element_id, loc_types=None):
        q = ("select f.path, sl.start_line, sl.start_column, sl.type "
             "from occurrence o "
             "join source_location sl on sl.id = o.source_location_id "
             "join file f on f.id = sl.file_node_id "
             "where o.element_id = ?")
        rows = self.c.execute(q, (element_id,)).fetchall()
        if loc_types is not None:
            rows = [r for r in rows if r[3] in loc_types]
        return rows

    def definition(self, nid):
        """Best definition location: prefer SCOPE, then SIGNATURE, then TOKEN."""
        rows = self._loc_rows(nid)
        for want in (LOC_SCOPE, LOC_SIGNATURE, LOC_TOKEN):
            cand = sorted((r for r in rows if r[3] == want),
                          key=lambda r: (r[0], r[1]))
            if cand:
                return cand
        return []

    def references(self, nid):
        rows = [r for r in self._loc_rows(nid, {LOC_TOKEN})]
        return sorted(set(rows), key=lambda r: (r[0], r[1]))

    def edge_callsites(self, edge_id):
        return sorted(self._loc_rows(edge_id, {LOC_TOKEN}),
                      key=lambda r: (r[0], r[1]))

    # -- edges ------------------------------------------------------------
    def edges_from(self, nid, etype):
        return self.c.execute(
            "select id, target_node_id from edge "
            "where source_node_id=? and type=?", (nid, etype)).fetchall()

    def edges_to(self, nid, etype):
        return self.c.execute(
            "select id, source_node_id from edge "
            "where target_node_id=? and type=?", (nid, etype)).fetchall()


# --- formatting ---------------------------------------------------------
def rel(path):
    p = path.replace("\\", "/")
    for root in ("C:/Dev/Smatchet/", os.getcwd().replace("\\", "/") + "/"):
        if p.startswith(root):
            return p[len(root):]
    return p


def fmt_loc(r):
    return "%s:%d:%d" % (rel(r[0]), r[1], r[2])


def need(ix, name, kinds, what):
    ids = ix.resolve(name, kinds)
    if not ids:
        print("no %s match for %r (try: st_query.py name %s)" % (what, name, name))
        sys.exit(1)
    return ids


# --- subcommands --------------------------------------------------------
def cmd_def(ix, a):
    for nid in need(ix, a.name, None, "symbol"):
        print(ix.label(nid))
        locs = ix.definition(nid)
        if not locs:
            print("    (no recorded location - implicit/builtin)")
        for r in locs:
            print("    %s" % fmt_loc(r))


def cmd_refs(ix, a):
    for nid in need(ix, a.name, None, "symbol"):
        refs = ix.references(nid)
        print("%s  - %d reference(s)" % (ix.label(nid), len(refs)))
        for r in refs[:a.limit]:
            print("    %s" % fmt_loc(r))
        if len(refs) > a.limit:
            print("    ... +%d more (raise --limit)" % (len(refs) - a.limit))


def cmd_callers(ix, a):
    kinds = {4096, 8192}  # FUNCTION, METHOD
    for nid in need(ix, a.name, kinds, "function"):
        edges = ix.edges_to(nid, 8)  # CALL
        print("%s  <- %d caller(s)" % (ix.label(nid), len(edges)))
        for eid, src in edges:
            sites = ix.edge_callsites(eid)
            loc = ("  @ " + fmt_loc(sites[0])) if sites else ""
            print("    %s%s" % (ix.nodes[src][1] or "?", loc))


def cmd_callees(ix, a):
    kinds = {4096, 8192}
    for nid in need(ix, a.name, kinds, "function"):
        edges = ix.edges_from(nid, 8)
        print("%s  -> %d callee(s)" % (ix.label(nid), len(edges)))
        for eid, tgt in edges:
            sites = ix.edge_callsites(eid)
            loc = ("  @ " + fmt_loc(sites[0])) if sites else ""
            print("    %s%s" % (ix.nodes[tgt][1] or "?", loc))


def cmd_members(ix, a):
    kinds = {64, 128, 16, 16384, 256, 1048576, 32}  # struct/class/ns/enum/...
    for nid in need(ix, a.name, kinds, "container"):
        edges = ix.edges_from(nid, 1)  # MEMBER
        print("%s  - %d member(s)" % (ix.label(nid), len(edges)))
        for _, tgt in sorted(edges, key=lambda e: ix.nodes[e[1]][1]):
            print("    %s" % ix.label(tgt))


def cmd_bases(ix, a):
    for nid in need(ix, a.name, {64, 128, 256}, "class"):
        edges = ix.edges_from(nid, 16)  # INHERITANCE source=derived
        print("%s  - %d base(s)" % (ix.label(nid), len(edges)))
        for _, tgt in edges:
            print("    %s" % ix.nodes[tgt][1])


def cmd_derived(ix, a):
    for nid in need(ix, a.name, {64, 128, 256}, "class"):
        edges = ix.edges_to(nid, 16)
        print("%s  - %d derived" % (ix.label(nid), len(edges)))
        for _, src in edges:
            print("    %s" % ix.nodes[src][1])


def cmd_file(ix, a):
    frag = a.path.lower().replace("\\", "/")
    fids = {fid: p for fid, p in ix.c.execute("select id, path from file")
            if frag in p.lower().replace("\\", "/")}
    if not fids:
        print("no file matches %r" % a.path)
        return
    for fid, p in sorted(fids.items(), key=lambda kv: kv[1]):
        syms = ix.c.execute(
            "select distinct o.element_id from occurrence o "
            "join source_location sl on sl.id=o.source_location_id "
            "where sl.file_node_id=? and sl.type=?", (fid, LOC_SCOPE)).fetchall()
        names = []
        for (eid,) in syms:
            n = ix.nodes.get(eid)
            if n and n[0] in (64, 128, 4096, 8192, 16, 16384, 1048576):
                names.append("    %s" % ix.label(eid))
        if names:
            print("%s  (%d def)" % (rel(p), len(names)))
            print("\n".join(sorted(set(names))[:a.limit]))


def cmd_name(ix, a):
    q = a.substr.lower()
    hits = sorted({(ix.nodes[nid][1], nid) for ql, nid in ix.by_qual if q in ql})
    print("%d match(es) for %r" % (len(hits), a.substr))
    for qual, nid in hits[:a.limit]:
        print("    %-50s [%s]" % (qual, ix.kind(nid)))
    if len(hits) > a.limit:
        print("    ... +%d more (raise --limit)" % (len(hits) - a.limit))


def cmd_schema(ix, a):
    for t in ("node", "edge", "symbol", "source_location", "occurrence",
              "file", "error"):
        try:
            n = ix.c.execute("select count(*) from %s" % t).fetchone()[0]
            print("  %-16s %8d" % (t, n))
        except sqlite3.Error:
            pass
    print("\nNodeKind:", ", ".join("%d=%s" % (k, v) for k, v in
          sorted(NODE_KIND.items())))
    print("\nEdgeKind:", ", ".join("%d=%s" % (k, v) for k, v in
          sorted(EDGE_KIND.items())))


def main():
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    default_db = os.environ.get("SMATCHET_SRCTRLDB",
                                os.path.join(repo, "Smatchet.srctrldb"))
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--db", default=default_db, help="path to .srctrldb")
    common.add_argument("--limit", type=int, default=40, help="max rows per result")
    ap = argparse.ArgumentParser(description=__doc__, parents=[common],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for c in ("def", "refs", "callers", "callees", "members", "bases", "derived"):
        sp = sub.add_parser(c, parents=[common])
        sp.add_argument("name")
    sub.add_parser("file", parents=[common]).add_argument("path")
    sub.add_parser("name", parents=[common]).add_argument("substr")
    sub.add_parser("schema", parents=[common])
    a = ap.parse_args()

    if not os.path.exists(a.db):
        print("DB not found: %s\nBuild it: Sourcetrail.exe index --full "
              "Smatchet.srctrlprj" % a.db)
        sys.exit(2)
    ix = Index(a.db)
    {
        "def": cmd_def, "refs": cmd_refs, "callers": cmd_callers,
        "callees": cmd_callees, "members": cmd_members, "bases": cmd_bases,
        "derived": cmd_derived, "file": cmd_file, "name": cmd_name,
        "schema": cmd_schema,
    }[a.cmd](ix, a)


if __name__ == "__main__":
    main()
