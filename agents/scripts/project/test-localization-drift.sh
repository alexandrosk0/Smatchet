#!/usr/bin/env bash
# test-localization-drift.sh — WARN-first check that a `T("<key>", "<fallback>")`
# call-site literal still matches the localization table's English column.
# ----------------------------------------------------------------------------
# WHY (tooling 2026-09-07 localization-t-fallback-wins-over-table-english-for-en-us)
#   For en-US, `SmatchetLocalization::T(key, fallback)` renders the CALL-SITE
#   fallback, not the kEntries English column (that column is only an overrides
#   lookup key). Editing one without the other is a silent no-op in English: a
#   renamed label in the table changes nothing on screen, and a bucket-E test
#   that targets the new label never resolves its item. This check compares the
#   two after C-escape decoding (so `\u2014` vs a literal em-dash, or `\"` vs a
#   raw quote in a raw string, never false-positives) and reports each drift as
#   rule `localization-english-fallback-drift`. Docs: docs/agent-rules/cpp-rules.md
#   § Localization.
#
# Modes:
#   (no args)          = --diff origin/develop
#   --diff[=]<base>    delta-gated: call sites on lines ADDED vs the merge-base of
#                      <base> (working tree included), plus EVERY call site of a
#                      key whose table row changed. Pre-existing drift elsewhere is
#                      grandfathered. Unresolvable <base> = SKIPPED (exit 0).
#   --all              every literal-fallback call site in the tree (calibration).
#   --strict           exit 1 on any drift (the post-calibration graduation knob).
#   --selftest         fixture repo, both directions + the delta scoping.
#
# Call sites: first-party C/C++ under Source/ (ThirdParty excluded) where the
# fallback argument is one or more adjacent string literals; a non-literal
# fallback (a variable, a ternary) or a key absent from the table is skipped.
# Override seams (read-only, for the selftest): LOC_TABLE, LOC_SCAN_ROOT.
#
# Exit: 0 clean or WARN-only · 1 drift under --strict / selftest failure ·
#       2 infra error (no python, bad usage). Emits `Passed: N  Failed: M`.
# selftest: asserts-failure
# ----------------------------------------------------------------------------
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$SCRIPT_DIR/../core/lib/resolve-py.sh"

usage() {
    sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

mode="diff"
base="origin/develop"
strict=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --diff)
            [ -n "${2:-}" ] || usage
            mode="diff"; base="$2"; shift 2 ;;
        --diff=*)
            mode="diff"; base="${1#--diff=}"
            [ -n "$base" ] || usage
            shift ;;
        --all) mode="all"; shift ;;
        --strict) strict=1; shift ;;
        --selftest) mode="selftest"; shift ;;
        -h|--help) usage ;;
        *) echo "test-localization-drift: unknown argument: $1" >&2; usage ;;
    esac
done

PY="$(resolve_py)" || { echo "test-localization-drift: no working python 3" >&2; exit 2; }

if [ "$mode" = "selftest" ]; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    fail=0
    (
        set -e
        cd "$tmp"
        git init -q -b develop .
        git config user.email selftest@local
        git config user.name selftest
        mkdir -p Source/Core/src/Ui Source/ThirdParty
        cat > Source/Core/src/Loc.cpp <<'EOF'
const TranslationEntry kEntries[] = {
    {"a.same", "Save \u2014 now", u8"Enregistrer"},
    {"a.split", "Line one "
     "and two", u8"x"},
    {"a.old", "Reverted layout", u8"y"},
    {"a.table_edit", "Story group", u8"z"},
    {"a.quote", "Say \"hi\" \x41", u8"q"},
};
EOF
        cat > Source/Core/src/Ui/Use.cpp <<'EOF'
void Draw() {
    Label(SmatchetLocalization::T("a.same", "Save — now"));
    Label(T("a.split", "Line one and two"));
    Label(T("a.old", "Reverted query"));
    Label(T("a.table_edit", "Story group"));
    Label(T("a.missing_key", "Anything"));
    Label(T("a.same", runtimeFallback));
    Label(T("a.quote", R"(Say "hi" A)"));
}
EOF
        printf 'void V() { T("a.old", "vendored drift"); }\n' > Source/ThirdParty/V.cpp
        git add -A && git commit -qm base
    ) || { echo "test-localization-drift --selftest: FAIL — fixture setup" >&2; exit 1; }

    run_mode() { (cd "$tmp" && LOC_TABLE=Source/Core/src/Loc.cpp bash "$SCRIPT_DIR/test-localization-drift.sh" "$@" 2>&1); }

    # 1. --all: exactly the one real drift (a.old); escape (\u, \", \x) / raw-string /
    #    concatenated forms match; the unknown key / non-literal fallback /
    #    ThirdParty call sites are skipped.
    out="$(run_mode --all)"
    if ! grep -q 'Use.cpp:4.*T("a.old")' <<<"$out" || [ "$(grep -c 'localization-english-fallback-drift' <<<"$out")" -ne 1 ]; then
        echo "test-localization-drift --selftest: FAIL — --all did not report exactly the a.old drift:" >&2
        printf '%s\n' "$out" >&2
        fail=1
    fi
    # selftest: asserts-failure — --strict turns a drift into exit 1.
    if run_mode --all --strict >/dev/null; then
        echo "test-localization-drift --selftest: FAIL — --strict passed a known drift" >&2
        fail=1
    fi
    # 2. --diff: the pre-existing a.old drift is grandfathered (no changed lines).
    (cd "$tmp" && git checkout -qb feature)
    out="$(run_mode --diff develop)"
    if grep -q 'localization-english-fallback-drift' <<<"$out"; then
        echo "test-localization-drift --selftest: FAIL — --diff flagged an untouched pre-existing drift" >&2
        fail=1
    fi
    # 3. --diff: a TABLE-only English edit with the call site left behind is flagged
    #    at the (unchanged) call site; a new drifting call site is flagged too.
    (cd "$tmp" && sed -i 's/"Story group"/"Parent group"/' Source/Core/src/Loc.cpp &&
        printf 'void N() { T("a.same", "Save now"); }\n' >> Source/Core/src/Ui/Use.cpp)
    out="$(run_mode --diff develop)"
    if ! grep -q 'Use.cpp:5.*T("a.table_edit")' <<<"$out" || ! grep -q 'Use.cpp:10.*T("a.same")' <<<"$out" ||
        grep -q 'T("a.old")' <<<"$out"; then
        echo "test-localization-drift --selftest: FAIL — --diff missed a table-row or call-site drift:" >&2
        printf '%s\n' "$out" >&2
        fail=1
    fi
    if [ "$fail" -ne 0 ]; then
        echo "Passed: 0  Failed: 1"
        exit 1
    fi
    echo "test-localization-drift --selftest: PASS — escape-decoded match, drift found, delta scoping, table-row edits, --strict."
    echo "Passed: 1  Failed: 0"
    exit 0
fi

cd "$(git rev-parse --show-toplevel 2>/dev/null)" || { echo "test-localization-drift: not in a git work tree" >&2; exit 2; }
LOC_TABLE="${LOC_TABLE:-Source/Core/src/SmatchetLocalization.cpp}"
LOC_SCAN_ROOT="${LOC_SCAN_ROOT:-Source/}"
if [ ! -f "$LOC_TABLE" ]; then
    echo "test-localization-drift: SKIPPED — no localization table at $LOC_TABLE"
    echo "Passed: 0  Failed: 0  Skipped: 1"
    exit 0
fi

"$PY" - "$mode" "$base" "$strict" "$LOC_TABLE" "$LOC_SCAN_ROOT" <<'PY'
import re
import subprocess
import sys

MODE, BASE, STRICT, TABLE, SCAN_ROOT = sys.argv[1], sys.argv[2], sys.argv[3] == "1", sys.argv[4], sys.argv[5]
CPP_EXT = (".cpp", ".h", ".hpp", ".cc", ".cxx", ".inl")
RULE = "localization-english-fallback-drift"

TOKEN_RE = re.compile(r"""
    (?P<ws>\s+)
  | (?P<lc>//[^\n]*)
  | (?P<bc>/\*.*?\*/)
  | (?P<raw>(?:u8|u|U|L)?R"([^()\\\s]{0,16})\(.*?\)\2")
  | (?P<str>(?:u8|u|U|L)?"(?:[^"\\\n]|\\.)*")
  | (?P<chr>(?:u8|u|U|L)?'(?:[^'\\\n]|\\.)*')
  | (?P<id>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<num>\.?[0-9](?:[eEpP][+-]|[0-9A-Za-z_.'])*)
  | (?P<p>::|->|.)
""", re.S | re.X)


def tokenize(text):
    """[(kind, text, line)] with whitespace/comments dropped; raw strings are kind 'str'."""
    out, line = [], 1
    for m in TOKEN_RE.finditer(text):
        tok = m.group(0)
        for kind in ("raw", "str", "chr", "id", "num", "p"):
            if m.group(kind) is not None:
                out.append(("str" if kind == "raw" else kind, tok, line))
                break
        line += tok.count("\n")
    return out


SIMPLE = {"n": 10, "t": 9, "r": 13, "a": 7, "b": 8, "f": 12, "v": 11,
          "\\": 92, "'": 39, '"': 34, "?": 63}


def decode(lit):
    """Bytes of one C/C++ string literal token, C escapes decoded."""
    m = re.match(r'(?:u8|u|U|L)?R"([^(]*)\((.*)\)\1"$', lit, re.S)
    if m:
        return m.group(2).encode("utf-8")
    body = lit[lit.index('"') + 1:-1]
    out, i = bytearray(), 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out += c.encode("utf-8")
            i += 1
            continue
        n = body[i + 1] if i + 1 < len(body) else ""
        if n in SIMPLE:
            out.append(SIMPLE[n]); i += 2
        elif n == "x":
            j = i + 2
            while j < len(body) and body[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(int(body[i + 2:j] or "0", 16) & 0xFF); i = j
        elif n in "01234567":
            j = i + 1
            while j < len(body) and j < i + 4 and body[j] in "01234567":
                j += 1
            out.append(int(body[i + 1:j], 8) & 0xFF); i = j
        elif n in ("u", "U"):
            width = 4 if n == "u" else 8
            out += chr(int(body[i + 2:i + 2 + width], 16)).encode("utf-8"); i += 2 + width
        else:
            out += n.encode("utf-8"); i += 2
    return bytes(out)


def concat(lits):
    return b"".join(decode(t[1]) for t in lits)


def parse_table(path):
    """{key: (english_bytes, english_source, row_first_line, row_last_line)}."""
    toks = tokenize(open(path, encoding="utf-8", errors="replace").read())
    entries = {}
    for i in range(len(toks) - 4):
        if toks[i][1] == "kEntries" and [t[1] for t in toks[i + 1:i + 5]] == ["[", "]", "=", "{"]:
            j = i + 5
            break
    else:
        return entries
    while j < len(toks) and toks[j][1] != "}":
        if toks[j][1] != "{":
            j += 1
            continue
        start, fields, cur, j = toks[j][2], [], [], j + 1
        ok = True
        while j < len(toks) and toks[j][1] != "}":
            if toks[j][1] == ",":
                fields.append(cur); cur = []
            elif toks[j][0] == "str":
                cur.append(toks[j])
            else:
                ok = False
            j += 1
        fields.append(cur)
        if ok and len(fields) >= 2 and len(fields[0]) == 1 and fields[1]:
            key = decode(fields[0][0][1]).decode("utf-8", "replace")
            src = " ".join(t[1] for t in fields[1])
            entries[key] = (concat(fields[1]), src, start, toks[j][2] if j < len(toks) else start)
        j += 1
        if j < len(toks) and toks[j][1] == ",":
            j += 1
    return entries


def find_calls(text):
    """Yield (key, fallback_bytes, fallback_source, first_line, last_line) per literal T() call."""
    toks = tokenize(text)
    n = len(toks)
    for i in range(n - 5):
        if toks[i][0] != "id" or toks[i][1] != "T" or toks[i + 1][1] != "(":
            continue
        if i > 0 and toks[i - 1][1] in (".", "->"):
            continue
        if toks[i + 2][0] != "str" or toks[i + 3][1] != ",":
            continue
        j, lits = i + 4, []
        while j < n and toks[j][0] == "str":
            lits.append(toks[j]); j += 1
        if not lits or j >= n or toks[j][1] != ")":
            continue
        key = decode(toks[i + 2][1]).decode("utf-8", "replace")
        yield key, concat(lits), " ".join(t[1] for t in lits), toks[i][2], toks[j][2]


def git(*args):
    p = subprocess.run(["git", *args], capture_output=True, text=True, encoding="utf-8", errors="replace")
    return p.returncode, p.stdout


def in_scope(path):
    return path.startswith(SCAN_ROOT) and path.endswith(CPP_EXT) and "ThirdParty/" not in path


def read(path):
    try:
        return open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def short(s, n=70):
    return s if len(s) <= n else s[:n - 3] + "..."


table = parse_table(TABLE)
if not table:
    print("test-localization-drift: ERROR — no kEntries rows parsed from %s" % TABLE, file=sys.stderr)
    sys.exit(2)

rc, out = git("ls-files", "--", SCAN_ROOT)
tracked = [f for f in out.splitlines() if in_scope(f) and f != TABLE]
targets = []          # (path, only_lines or None, only_keys or None)

if MODE == "all":
    targets = [(f, None, None) for f in tracked]
else:
    rc, mb = git("merge-base", BASE, "HEAD")
    mb = mb.strip()
    if rc != 0 or not mb:
        rc, mb = git("rev-parse", "--verify", "--quiet", BASE + "^{commit}")
        mb = mb.strip()
        if rc != 0 or not mb:
            print("test-localization-drift: SKIPPED — base ref %s does not resolve (shallow clone?)" % BASE)
            print("Passed: 0  Failed: 0  Skipped: 1")
            sys.exit(0)
        print("test-localization-drift: WARN — no merge-base with %s; diffing against its tip" % BASE,
              file=sys.stderr)
    rc, diff = git("diff", "--unified=0", "--no-color", "--no-ext-diff", mb, "--", SCAN_ROOT)
    added, cur = {}, None
    for ln in diff.splitlines():
        if ln.startswith("+++ "):
            cur = ln[6:] if ln.startswith("+++ b/") else None
        elif ln.startswith("@@") and cur:
            m = re.search(r"\+(\d+)(?:,(\d+))?", ln)
            first, count = int(m.group(1)), int(m.group(2) or "1")
            added.setdefault(cur, set()).update(range(first, first + count))
    changed_keys = set()
    if TABLE in added:
        for key, (_eng, _src, a, b) in table.items():
            if any(l in added[TABLE] for l in range(a, b + 1)):
                changed_keys.add(key)
    targets = [(f, added[f], None) for f in sorted(added) if in_scope(f) and f != TABLE]
    if changed_keys:
        targets += [(f, None, changed_keys) for f in tracked]

hits, seen, checked = [], set(), 0
for path, only_lines, only_keys in targets:
    for key, fb, fb_src, first, last in find_calls(read(path)):
        if (path, first, key) in seen or key not in table:
            continue
        if only_lines is not None and not any(l in only_lines for l in range(first, last + 1)):
            continue
        if only_keys is not None and key not in only_keys:
            continue
        seen.add((path, first, key))
        checked += 1
        eng, eng_src, row, _ = table[key]
        if fb != eng:
            hits.append((path, first, key, fb_src, eng_src, row))

scope = "%s, %d literal-fallback call site(s) compared" % (
    "whole tree" if MODE == "all" else "delta vs %s" % BASE, checked)
for path, line, key, fb_src, eng_src, row in sorted(hits):
    print('WARN %s\t%s:%d\tT("%s") fallback %s != table English %s (%s:%d)'
          % (RULE, path, line, key, short(fb_src), short(eng_src), TABLE, row))
if hits:
    print("test-localization-drift: %d call-site fallback(s) drift from the table English (%s). "
          "For en-US the call-site fallback is what renders - edit both together "
          "(docs/agent-rules/cpp-rules.md section Localization)." % (len(hits), scope))
else:
    print("test-localization-drift: no T() fallback / table English drift (%s)." % scope)
if hits and STRICT:
    print("Passed: 0  Failed: 1")
    sys.exit(1)
print("Passed: 1  Failed: 0")
sys.exit(0)
PY
