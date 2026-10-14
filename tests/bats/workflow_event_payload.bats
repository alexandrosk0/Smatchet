#!/usr/bin/env bats
# tests/bats/workflow_event_payload.bats
# ----------------------------------------------------------------------------
# Property: every workflow under .github/workflows that is triggered on
# `pull_request` / `pull_request_target` AND reads the PR body or title from
# the event payload (`github.event.pull_request.body` / `.title`) must list
# `edited` in that trigger's `types:`.
#
# Why (process 2026-09-07 rerunning-a-body-reading-gate-replays-a-frozen-
# payload): such a check's input is the payload frozen when the run started.
# `gh run rerun` replays that same payload, so a run that failed on the body can
# never pass on a rerun — and its fresh FAILURE replaces the run's earlier
# conclusion (on #2180 a harmless CANCELLED became a blocking FAILURE). The only
# re-trigger that reads the CURRENT body is a body edit, i.e. the `edited`
# activity type. Without it such a gate has no re-trigger at all short of a new
# commit.
#
# GitHub's default activity types for both events are opened / synchronize /
# reopened, so a trigger with NO `types:` filter does NOT receive `edited` and
# is a violation. Exempt: a trigger whose types are all `closed` (e.g.
# lock-cleanup.yml) — a closed PR has no gate left to re-run.
#
# The property is asserted over the population, not a named instance, so it
# stays true when a new payload-reading workflow lands. Parsing is stdlib-only
# Python (no PyYAML): a small reader for the `on:` block that understands the
# block, flow-list, flow-mapping and bare-scalar trigger forms. A payload-reading
# workflow whose `on:` block it cannot read FAILS (fail closed), never skips.
#
# selftest: NEGATIVE fixtures (no types filter, types without edited,
# pull_request_target, list / scalar / flow-mapping forms) MUST be flagged —
# proves the property fires rather than passing vacuously.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    FIX="$BATS_TEST_TMPDIR/wf"
    mkdir -p "$FIX"
}

# scan <workflows-dir> — one line per workflow: <file> TAB <verdict> TAB <detail>
# verdict: ok | violation | exempt | skip (not a PR-triggered payload reader) |
#          unparsed (a payload reader whose on: block could not be read).
scan() {
    python3 - "$1" <<'PY'
import os
import re
import sys

PR_EVENTS = ("pull_request", "pull_request_target")
# GitHub's default activity types when the trigger has no `types:` filter.
DEFAULT_TYPES = ["opened", "synchronize", "reopened"]
PAYLOAD_RE = re.compile(r"github\.event\.pull_request\.(?:body|title)\b")


class ParseError(Exception):
    pass


def strip_comment(line):
    """Drop a YAML comment: '#' at line start or after whitespace, unquoted."""
    quote = None
    for i, c in enumerate(line):
        if quote:
            if c == quote:
                quote = None
        elif c in "'\"":
            quote = c
        elif c == "#" and (i == 0 or line[i - 1] in " \t"):
            return line[:i]
    return line


def parse_flow(s):
    """Parse a YAML flow value: scalar, [list] or {mapping}."""
    s = s.strip()
    pos = 0

    def ws():
        nonlocal pos
        while pos < len(s) and s[pos] in " \t":
            pos += 1

    def scalar(stop):
        nonlocal pos
        ws()
        if pos < len(s) and s[pos] in "'\"":
            q = s[pos]
            end = s.find(q, pos + 1)
            if end < 0:
                raise ParseError("unterminated quote")
            v = s[pos + 1:end]
            pos = end + 1
            return v
        start = pos
        while pos < len(s) and s[pos] not in stop:
            pos += 1
        v = s[start:pos].strip()
        return None if v in ("", "null", "~") else v

    def value():
        nonlocal pos
        ws()
        if pos >= len(s):
            return None
        c = s[pos]
        if c == "[":
            pos += 1
            out = []
            ws()
            if pos < len(s) and s[pos] == "]":
                pos += 1
                return out
            while True:
                out.append(value())
                ws()
                if pos < len(s) and s[pos] == ",":
                    pos += 1
                    continue
                if pos < len(s) and s[pos] == "]":
                    pos += 1
                    return out
                raise ParseError("bad flow list")
        if c == "{":
            pos += 1
            out = {}
            ws()
            if pos < len(s) and s[pos] == "}":
                pos += 1
                return out
            while True:
                k = scalar(":,}")
                ws()
                v = None
                if pos < len(s) and s[pos] == ":":
                    pos += 1
                    ws()
                    if pos < len(s) and s[pos] not in ",}":
                        v = value()
                out[k] = v
                ws()
                if pos < len(s) and s[pos] == ",":
                    pos += 1
                    continue
                if pos < len(s) and s[pos] == "}":
                    pos += 1
                    return out
                raise ParseError("bad flow mapping")
        return scalar(",]}")

    v = value()
    ws()
    if pos != len(s):
        raise ParseError("trailing text in flow value: %r" % s)
    return v


def as_types(v):
    if v is None:
        return None
    if isinstance(v, str):
        return [v]
    if isinstance(v, list):
        return [x for x in v if isinstance(x, str)]
    raise ParseError("types is not a list")


def triggers(lines):
    """-> {event: [types] | None (no filter)} from the top-level on: key."""
    for i, raw in enumerate(lines):
        m = re.match(r"""^(?:on|"on"|'on')\s*:(.*)$""", raw)
        if not m:
            continue
        inline = strip_comment(m.group(1)).strip()
        if inline:
            v = parse_flow(inline)
            if isinstance(v, str):
                return {v: None}
            if isinstance(v, list):
                return {e: None for e in v}
            if isinstance(v, dict):
                return {e: (as_types(c.get("types")) if isinstance(c, dict) else None)
                        for e, c in v.items()}
            raise ParseError("unreadable inline on: value")
        block = []
        for r in lines[i + 1:]:
            t = strip_comment(r).rstrip()
            if not t.strip():
                continue
            if not r[0].isspace():
                break                       # next top-level key
            block.append((len(t) - len(t.lstrip()), t.strip()))
        if not block:
            raise ParseError("empty on: block")
        ev_indent = block[0][0]
        out = {}
        j = 0
        while j < len(block):
            ind, text = block[j]
            if ind != ev_indent:
                raise ParseError("unexpected indent: %r" % text)
            if text.startswith("- "):       # block list of event names
                out[parse_flow(text[2:])] = None
                j += 1
                continue
            m2 = re.match(r"""^["']?([A-Za-z_]+)["']?\s*:(.*)$""", text)
            if not m2:
                raise ParseError("unreadable event line: %r" % text)
            ev, rest = m2.group(1), m2.group(2).strip()
            k = j + 1
            kids = []
            while k < len(block) and block[k][0] > ev_indent:
                kids.append(block[k])
                k += 1
            types = None
            if rest and rest not in ("null", "~"):
                cfg = parse_flow(rest)
                types = as_types(cfg.get("types")) if isinstance(cfg, dict) else None
            elif kids:
                kid_indent = kids[0][0]
                for n, (kind, ktext) in enumerate(kids):
                    if kind != kid_indent:
                        continue
                    km = re.match(r"""^["']?types["']?\s*:(.*)$""", ktext)
                    if not km:
                        continue
                    krest = km.group(1).strip()
                    if krest:
                        types = as_types(parse_flow(krest))
                    else:
                        types = []
                        for gind, gtext in kids[n + 1:]:
                            if gtext.startswith("-") and gind >= kid_indent:
                                types.append(parse_flow(gtext[1:]))
                            elif gind <= kid_indent:
                                break
            out[ev] = types
            j = k
        return out
    raise ParseError("no top-level on: key")


root = sys.argv[1]
for name in sorted(os.listdir(root)):
    if not name.endswith((".yml", ".yaml")):
        continue
    path = os.path.join(root, name)
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().splitlines()
    reads = any(PAYLOAD_RE.search(strip_comment(ln)) for ln in lines)
    try:
        trig = triggers(lines)
    except ParseError as e:
        if reads:
            print("%s\tunparsed\t%s" % (name, e))
        else:
            print("%s\tskip\tnot a payload reader" % name)
        continue
    pr = {e: trig[e] for e in PR_EVENTS if e in trig}
    if not reads or not pr:
        print("%s\tskip\t%s" % (name, "no PR trigger" if reads else "not a payload reader"))
        continue
    bad = []
    checked = 0
    for ev, types in sorted(pr.items()):
        eff = types if types is not None else DEFAULT_TYPES
        if set(eff) <= {"closed"}:
            continue                        # closed-only: nothing left to re-run
        checked += 1
        if "edited" not in eff:
            bad.append("%s types=%s%s" % (ev, ",".join(eff),
                       " (no types filter: GitHub default)" if types is None else ""))
    if bad:
        print("%s\tviolation\t%s" % (name, "; ".join(bad)))
    elif checked == 0:
        print("%s\texempt\tclosed-only" % name)
    else:
        print("%s\tok\tedited present" % name)
PY
}

# verdict_of <scan-output> <file> — the verdict column for one workflow.
verdict_of() {
    printf '%s\n' "$1" | awk -F'\t' -v f="$2" '$1 == f { print $2 }'
}

@test "every PR-triggered workflow that reads the PR body/title lists 'edited'" {
    run scan "$REPO_ROOT/.github/workflows"
    [ "$status" -eq 0 ]
    [ -n "$output" ]
    # Fail closed: a violation OR a payload reader the parser could not read.
    run grep -E $'\t(violation|unparsed)\t' <<< "$output"
    if [ "$status" -eq 0 ]; then
        echo "workflow(s) read github.event.pull_request.body/title without an 'edited' re-trigger" >&2
        echo "(add 'edited' to the trigger's types:, or see the header for the exemption):" >&2
        echo "$output" >&2
        return 1
    fi
}

@test "non-vacuity: the real tree has a payload reader the property actually checks" {
    run scan "$REPO_ROOT/.github/workflows"
    [ "$status" -eq 0 ]
    # doc-validation.yml's Intent section job reads the body; it must be seen
    # AND classified as checked-and-ok, not skipped.
    [ "$(verdict_of "$output" doc-validation.yml)" = ok ]
    # The closed-only exemption is exercised by a real workflow too.
    [ "$(verdict_of "$output" lock-cleanup.yml)" = exempt ]
}

@test "selftest: a pull_request trigger with NO types filter is a violation" {
    cat > "$FIX/a.yml" <<'YML'
name: A
on:
  pull_request:
    branches: [develop]
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - env:
          PR_BODY: ${{ github.event.pull_request.body }}
        run: echo "$PR_BODY"
YML
    run scan "$FIX"
    [ "$(verdict_of "$output" a.yml)" = violation ]
    [[ "$output" == *"no types filter"* ]]
}

@test "selftest: types without edited is a violation (title read counts too)" {
    cat > "$FIX/b.yml" <<'YML'
name: B
on:
  pull_request:
    types: [opened, synchronize, reopened, labeled]
jobs:
  j:
    if: contains(github.event.pull_request.title, 'wip')
    runs-on: ubuntu-latest
    steps:
      - run: echo hi
YML
    run scan "$FIX"
    [ "$(verdict_of "$output" b.yml)" = violation ]
}

@test "selftest: pull_request_target without edited is a violation" {
    cat > "$FIX/c.yml" <<'YML'
name: C
on:
  pull_request_target:
    types:
      - opened
      - synchronize
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - env:
          T: ${{ github.event.pull_request.title }}
        run: echo "$T"
YML
    run scan "$FIX"
    [ "$(verdict_of "$output" c.yml)" = violation ]
}

@test "selftest: scalar, list and flow-mapping on: forms are all read" {
    printf 'on: pull_request\njobs:\n  j:\n    if: github.event.pull_request.body\n' > "$FIX/d.yml"
    printf 'on: [push, pull_request]\njobs:\n  j:\n    if: github.event.pull_request.body\n' > "$FIX/e.yml"
    printf 'on: { pull_request: { branches: [develop], types: [opened] } }\njobs:\n  j:\n    if: github.event.pull_request.body\n' > "$FIX/f.yml"
    printf 'on: { pull_request: { types: [opened, edited] } }\njobs:\n  j:\n    if: github.event.pull_request.body\n' > "$FIX/g.yml"
    run scan "$FIX"
    [ "$(verdict_of "$output" d.yml)" = violation ]
    [ "$(verdict_of "$output" e.yml)" = violation ]
    [ "$(verdict_of "$output" f.yml)" = violation ]
    [ "$(verdict_of "$output" g.yml)" = ok ]
}

@test "selftest: edited in a block list at the key's own indent passes" {
    cat > "$FIX/h.yml" <<'YML'
name: H
"on":
  # comment lines inside the block are ignored
  pull_request:
    types:
    - opened
    - 'edited'   # trailing comment
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - env:
          PR_BODY: ${{ github.event.pull_request.body }}
        run: echo "$PR_BODY"
YML
    run scan "$FIX"
    [ "$(verdict_of "$output" h.yml)" = ok ]
}

@test "selftest: closed-only trigger is exempt; a mention in a comment is not a read" {
    cat > "$FIX/i.yml" <<'YML'
name: I
on:
  pull_request:
    types: [closed]
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - env:
          PR_BODY: ${{ github.event.pull_request.body }}
        run: echo "$PR_BODY"
YML
    cat > "$FIX/k.yml" <<'YML'
name: K
on:
  pull_request:
    branches: [develop]
# reads nothing; this comment names github.event.pull_request.body only
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - run: echo hi
YML
    run scan "$FIX"
    [ "$(verdict_of "$output" i.yml)" = exempt ]
    [ "$(verdict_of "$output" k.yml)" = skip ]
}

@test "selftest: a payload reader with an unreadable on: block fails closed" {
    printf 'on: { pull_request: [unterminated\njobs:\n  j:\n    if: github.event.pull_request.body\n' > "$FIX/m.yml"
    run scan "$FIX"
    [ "$(verdict_of "$output" m.yml)" = unparsed ]
}
