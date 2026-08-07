#!/usr/bin/env python3
"""work_item_lint.py - work-item close + ledger-citation linter.

Port of Whip-Process Tools/check-docs.ps1 (-Item / -Citations; the -Audit mode
was consumer-specific and is not ported). Layout mapping is this repo's
docs/agent-rules/work-items.md:

    items            docs/work/items/NN-slug/
    closed summaries docs/work/closed/NN-slug.md
    ledgers          docs/work/DEFERRED.md (DEF-NN), BACKLOG.md (BL-NN),
                     IDEAS.md (IDEA-NN)   - bugs are GitHub Issues, so the
                     original's BUG ledger id is dropped
    standing QA doc  docs/work/QA.md (does not exist yet - see --item notes)

Modes, exactly one per run:

  --item NN-slug   the MECHANICAL subset of a work item's close: folder
                   collapsed, exactly one closed summary of the right shape,
                   no orphaned staged files, links resolve, and - when the
                   item's plan carried a `## QA` section - the five QA
                   invariants (qa-sheet-missing / -line-malformed /
                   -drain-missed / -step-outside-scenario / -index-broken).
                   The judgment probes (unfaithful/bloated summary, durable
                   content dropped) stay with an independent review and are
                   NOT checked here. Checks the WORKING TREE: closure is the
                   agent's file actions; review and commit come after. The
                   per-item QA.md / 3-plan.md are deleted at close, so their
                   content is read from git (--ref, default HEAD).
  --citations      repo-wide ledger-citation hygiene over tracked files. Docs
                   (*.md) must cite ids that EXIST in their ledger; source
                   files must cite none at all (comments state the current
                   implementation - a ledger id in source goes stale the day
                   the ledger drains). Write "DEF-010 (retired)" to mention a
                   resolved id as provenance without it counting as a
                   citation. --strict promotes source hits to failures.
  --all            the always-on repo-wide subset for scripts/dev/test-docs.sh:
                   the citations sweep + shape/link checks over every closed
                   summary + the orphaned-staged-file scan. The per-item
                   close checks (folder collapsed, QA drain) are close-time
                   acts and only run under --item.
  --selftest       self-contained fixture round-trip, asserting at least one
                   FAILURE case (test-gate-selftests.sh convention).

Exit 0 = pass; 1 = one or more checks failed (numbered list); 2 = usage.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

WORK = "docs/work"
ITEMS = f"{WORK}/items"
CLOSED = f"{WORK}/closed"
STANDING_QA = f"{WORK}/QA.md"
# Ledger id -> ledger file. The original parsed '### <ID>' headings; this
# repo's ledgers define entries as '- **<ID> - title**' bullets (work-items.md
# entry shape). Both shapes are parsed so a future format shift degrades
# gracefully instead of silently defining nothing.
LEDGER_FOR = {"DEF": "DEFERRED.md", "BL": "BACKLOG.md", "IDEA": "IDEAS.md"}
ID_PAT = re.compile(r"\b(DEF|BL|IDEA)-\d{2,3}\b")
LEDGER_DEF_PATS = [
    re.compile(r"^-\s+\*\*((?:DEF|BL|IDEA)-\d+)\b"),
    re.compile(r"^###\s+((?:DEF|BL|IDEA)-\d+)\b"),
]
# Review / resolution files are TRANSIENT SCRATCH, deleted when the round's
# subject closes, so a stale id in them can never become a durable dangling
# reference - which is the only thing the citations sweep exists to prevent.
# They are also the one place that QUOTES tool output verbatim: a reviewer
# reporting on "DEF-010" (or pasting this very check's error text) is
# describing an id, not citing it. Excluded deliberately; the durable
# artifacts a round produces - spec / design / plan / Spawned.md / the closed
# summary - stay in scope.
TRANSIENT_LEAF = re.compile(r"^(\d+-)?(close-)?(review|resolution)-")
STAGED_RE = re.compile(
    r"/(\d+-(specification|design|plan|review-.*|resolution-.*)\.md|Spawned\.md)$"
)
SOURCE_EXTS = (".h", ".hpp", ".hh", ".c", ".cc", ".cpp", ".cs", ".inl")


def read_doc(root, rel):
    p = os.path.join(root, rel)
    if os.path.isfile(p):
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    return []


def git(root, *args):
    r = subprocess.run(
        ["git", "-C", root, *args], capture_output=True, text=True,
        encoding="utf-8", errors="replace",
    )
    return r.returncode, r.stdout


def read_deleted_from_git(root, ref, rel):
    """The per-item QA.md / 3-plan.md are DELETED from the working tree at
    close, so their content is read from git: the newest commit (from ref)
    whose tree still holds the blob. At the pre-commit gate ref=HEAD still has
    the folder, so this resolves to HEAD; if a flow committed the collapse
    first it walks back one commit."""
    rc, out = git(root, "show", f"{ref}:{rel}")
    if rc == 0:
        return out.splitlines()
    rc, revs = git(root, "rev-list", ref, "--", rel)
    if rc == 0:
        for c in revs.split():
            rc2, out = git(root, "show", f"{c}:{rel}")
            if rc2 == 0:
                return out.splitlines()
    return []


def resolve_doc_path(base_dir, target):
    """Normalize '<base_dir>/<target>' (a markdown link relative to the
    summary's dir) to a repo-relative path, collapsing . and .. without
    touching the filesystem."""
    segs = []
    for s in (base_dir + "/" + target).split("/"):
        if s in ("", "."):
            continue
        if s == "..":
            if segs:
                segs.pop()
        else:
            segs.append(s)
    return "/".join(segs)


def glob_to_re(glob):
    """ci.path_filters-style glob -> anchored regex. '**' spans path
    separators; '*' does not."""
    out = []
    i = 0
    while i < len(glob):
        c = glob[i]
        if c == "*":
            if glob[i : i + 2] == "**":
                out.append(".*")
                i += 2
                if i < len(glob) and glob[i] == "/":
                    i += 1
                continue
            out.append("[^/]*")
        elif c == "?":
            out.append("[^/]")
        else:
            out.append(re.escape(c))
        i += 1
    return re.compile("^" + "".join(out) + "$")


def load_code_globs(root, notes=None):
    """Source-file classification comes from project.config.json
    ci.path_filters.code_globs - the project-specific value table - so this
    portable file carries no project directory literals."""
    try:
        with open(os.path.join(root, "project.config.json"), encoding="utf-8") as f:
            cfg = json.load(f)
        return [glob_to_re(g) for g in cfg["ci"]["path_filters"]["code_globs"]]
    except (OSError, KeyError, ValueError) as e:
        # A config break must weaken the gate VISIBLY, not silently.
        if notes is not None:
            notes.append(
                "note: code-globs-unreadable: project.config.json"
                f" ci.path_filters.code_globs unavailable ({e}) - source"
                " citation rule not applied."
            )
        return []


def work_tree_files(root):
    out = []
    base = os.path.join(root, WORK)
    for dirpath, _dirs, files in os.walk(base):
        for f in files:
            full = os.path.join(dirpath, f)
            out.append(os.path.relpath(full, root).replace("\\", "/"))
    return sorted(out)


# ============================ shared checks =================================
# Used by --item (the closing item's summary) and --all (every closed summary).

def check_summary_shape(root, rel, fails):
    summary = read_doc(root, rel)

    # Status line is Closed with a date.
    status = next((l for l in summary if re.match(r"^>\s*Status:", l)), "")
    if not re.match(r"^>\s*Status:\s*Closed\s*\(\d{4}-\d{2}-\d{2}\)\s*$", status):
        fails.append(
            f"wrong-status-line: {rel}: '{status}' (want '> Status: Closed (YYYY-MM-DD)')"
        )

    # Section set is exactly Shipped / Key decisions & reasons / Spawned work,
    # in order.
    heads = [re.sub(r"^##\s+", "", l).strip() for l in summary if re.match(r"^##\s+", l)]
    want = ["Shipped", "Key decisions & reasons", "Spawned work"]
    if heads != want:
        fails.append(
            f"wrong-section-set: {rel}: [{', '.join(heads)}] (want [{', '.join(want)}])"
        )

    # Relative links in the summary resolve (a file or a directory on disk).
    for line in summary:
        for m in re.finditer(r"\]\(([^)]+)\)", line):
            t = m.group(1)
            if re.match(r"^[a-z]+://", t):
                continue
            t = t.split("#")[0]
            if t == "":
                continue
            target = resolve_doc_path(CLOSED, t)
            if not os.path.exists(os.path.join(root, target)):
                fails.append(f"dangling-link: {rel}: {t}")


def check_orphans(work_tree, fails, exclude_item=None):
    """No staged file in an invalid location. Several items may be open at
    once, each legitimately keeping staged files in its own
    docs/work/items/NN-slug/ folder, so a staged file is flagged only when it
    is NOT directly inside such a folder - i.e. left stray under docs/work/ or
    in closed/. The item being closed is covered by folder-survived."""
    valid = re.compile("^" + re.escape(ITEMS) + r"/\d+-[^/]+/[^/]+$")
    for rel in work_tree:
        if exclude_item and rel.startswith(f"{ITEMS}/{exclude_item}/"):
            continue
        if STAGED_RE.search(rel) and not valid.match(rel):
            fails.append(f"orphaned-staged-file: {rel}")


# ============================ CITATIONS MODE ================================

def run_citations(root, strict):
    """Returns (fails, notes). Two distinct rules:

    docs (*.md)  - a cited DEF/BL/IDEA id must EXIST in its ledger. Catches
                   ids orphaned by a renumber.
    source       - no ledger id at all (files matching the configured
                   code_globs with a source extension).

    docs/work/closed/* is grandfathered: pre-adoption summaries are stale by
    design. Source hits are informational by default; strict promotes them to
    failures."""
    fails, notes = [], []
    defined = set()
    for _k, ledger in LEDGER_FOR.items():
        for l in read_doc(root, f"{WORK}/{ledger}"):
            for pat in LEDGER_DEF_PATS:
                m = pat.match(l)
                if m:
                    defined.add(m.group(1))

    # Zero parsed ids is AMBIGUOUS: a brand-new consumer with empty-but-present
    # ledgers (valid), or a ledger whose entry format changed so the regexes
    # silently matched nothing (a bug that would then flood every citation as
    # dangling). Disambiguate by whether any doc actually cites an id - empty
    # ledgers + zero citations is a clean empty project, not an alarm.
    empty_ledgers = not defined
    saw_doc_cite = False
    src_hits = []

    code_res = load_code_globs(root, notes)

    # Tracked files only: walking the tree would drag in gitignored build
    # output (multi-MB generated sources).
    rc, out = git(root, "ls-files")
    scan = [l for l in out.splitlines() if l] if rc == 0 else []
    if not scan:
        fails.append("no-files: 'git ls-files' returned nothing - not a work tree?")

    for rel in scan:
        is_md = rel.endswith(".md")
        is_source = rel.endswith(SOURCE_EXTS) and any(p.match(rel) for p in code_res)
        if not (is_md or is_source):
            continue
        if rel.startswith(f"{CLOSED}/"):
            continue  # grandfathered (pre-adoption summaries)
        if TRANSIENT_LEAF.match(rel.rsplit("/", 1)[-1]):
            continue
        full = os.path.join(root, rel)
        if not os.path.isfile(full):
            continue  # staged-deleted
        with open(full, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
        for n, line in enumerate(lines, 1):
            for m in ID_PAT.finditer(line):
                cid = m.group(0)
                if is_source:
                    src_hits.append(f"{rel}:{n}: {cid}")
                    continue
                if cid in defined:
                    continue
                # The two accepted forms below are legal REGARDLESS of whether
                # the ledgers still hold entries, so they are tested FIRST. A
                # consumer that has retired every open entry legitimately has
                # empty ledgers plus legal citations.
                # A retired id is fine PROVIDED the doc ties THAT id to its
                # successor. The test must be id-specific: "is there a closed/
                # link somewhere nearby" passes anything in a link-dense doc.
                # Two accepted forms:
                #   [DEF-010](../work/closed/13-rate-feedforward.md)  <- link text
                #   DEF-010 (retired)                                 <- no successor
                id_re = re.escape(cid)
                # Match "closed/" not the full work path: a link from inside
                # docs/work/ is relative. The TARGET MUST EXIST: a link is only
                # a successor if it resolves, otherwise
                # "[DEF-999](closed/never-existed.md)" silences the check it
                # should trip.
                lm = re.search(
                    r"\[[^\]]*" + id_re + r"[^\]]*\]\(([^)]*[Cc]losed/[^)]*)\)", line
                )
                if lm:
                    target = re.sub(r"#.*$", "", lm.group(1))
                    # A PLACEHOLDER target illustrates the grammar; it does not
                    # cite the id. No real repo path holds a non-ASCII char, a
                    # literal "...", or an angle-bracket stand-in - docs that
                    # document this very grammar legitimately show the SHAPE
                    # with a stand-in target. Deliberately narrow: exempting
                    # whole code spans instead would blind the check to real
                    # citations, which are routinely written inside backticks.
                    if re.search(r"[^\x20-\x7E]|\.\.\.|[<>]", target):
                        continue
                    base = os.path.dirname(os.path.join(root, rel))
                    if os.path.exists(os.path.join(base, target)):
                        continue
                    fails.append(
                        f"dangling-citation: {rel}:{n} cites {cid} via a link that does not resolve: {target}"
                    )
                    continue
                if re.match(r"^\S+\s*\(retired\)", line[m.start():]):
                    continue
                # Unexcused. Under empty ledgers this is the evidence that the
                # ledger format changed (one summary below); otherwise a plain
                # dangling citation.
                saw_doc_cite = True
                if empty_ledgers:
                    continue
                fails.append(
                    f"dangling-citation: {rel}:{n} cites {cid} with no successor named"
                    f" - link the {CLOSED}/ item that resolved it, or mark it '(retired)'"
                )

    # Empty ledgers WITH citations = the format-change alarm (one friendly
    # summary, not a flood). Empty ledgers with NO citations = a clean new
    # project; nothing to check.
    if empty_ledgers and saw_doc_cite:
        fails.append(
            "no-ledger-ids: docs cite ledger ids but 0 ledger entries parsed"
            " - ledger format changed?"
        )

    if src_hits:
        if strict:
            fails.extend(f"ledger-id-in-source: {h}" for h in src_hits)
        else:
            n_files = len({h.split(":")[0] for h in src_hits})
            notes.append(
                f"ledger-id-in-source: {len(src_hits)} occurrence(s) in {n_files} file(s)"
                " - comments state the current implementation. Re-run with --strict to list them."
            )
    return fails, notes


# ============================ ITEM CLOSE MODE ===============================

def run_item(root, item, ref):
    fails = []
    work_tree = work_tree_files(root)
    summary_path = f"{CLOSED}/{item}.md"

    # 1. Folder survived - the collapse must delete docs/work/items/<item>/.
    folder_hits = [p for p in work_tree if p.startswith(f"{ITEMS}/{item}/")]
    if folder_hits:
        fails.append(
            f"folder-survived: {ITEMS}/{item}/ still holds {len(folder_hits)} file(s)"
        )

    # 2. Summary exists and is unique.
    summary_hits = [p for p in work_tree if p == summary_path]
    if len(summary_hits) != 1:
        fails.append(
            f"summary-missing-or-dup: expected exactly one {summary_path},"
            f" found {len(summary_hits)}"
        )

    # 3. Orphaned staged files (the closing item itself is covered by check 1).
    check_orphans(work_tree, fails, exclude_item=item)

    # 4-6. Summary body shape + links.
    if len(summary_hits) == 1:
        check_summary_shape(root, summary_path, fails)

    # 7-11. QA invariants. Gate only items whose plan carried a `## QA`
    # section (grandfather: earlier items have none, so every QA check skips).
    # The deleted sheet / plan are read from git; the drain target (standing
    # QA doc) is read from the working tree.
    item_dir = f"{ITEMS}/{item}"
    plan_text = read_deleted_from_git(root, ref, f"{item_dir}/3-plan.md")
    has_qa = any(re.match(r"^\s*##\s+QA\b", l) for l in plan_text)
    if has_qa:
        check_qa(root, ref, item_dir, plan_text, fails)
    return fails


def check_qa(root, ref, item_dir, plan_text, fails):
    qa_text = read_deleted_from_git(root, ref, f"{item_dir}/QA.md")

    # 7. qa-sheet-missing.
    if not qa_text:
        fails.append(
            f"qa-sheet-missing: plan has '## QA' but no QA.md in git for {item_dir}"
        )
        return

    # The sheet is HIERARCHICAL when either the plan's `## QA` or the sheet
    # itself declares a `###` scenario: sections name the play scenario each
    # step drains into, and indices restart at 1 within each. Reading the plan
    # too means a sheet that simply forgot its scenarios still fails, instead
    # of being quietly demoted to the lenient flat path. Flat (pre-hierarchy)
    # sheets are checked under the older rules: same grammar without the
    # index, drain matched anywhere in the standing doc.
    in_plan_qa = False
    plan_has_scenario = False
    for l in plan_text:
        if re.match(r"^##\s+QA\b", l):
            in_plan_qa = True
            continue
        if re.match(r"^##\s", l):
            in_plan_qa = False
            continue
        if in_plan_qa and re.match(r"^###\s+\S", l):
            plan_has_scenario = True
            break
    hierarchical = plan_has_scenario or any(re.match(r"^###\s+\S", l) for l in qa_text)

    # A step is ANY bullet - deliberately NOT "a bullet containing =>".
    # Keying on the arrow is what let an entirely off-grammar sheet skip every
    # check below and close green. Explanatory prose therefore belongs in
    # indented continuation lines, not in bullets.
    #
    # The disposition bracket is OPTIONAL: sheets no longer record results, so
    # a current sheet is bare and only legacy ones carry `[...]`. The two
    # grammars are NOT the same pattern - the hierarchical one requires
    # `**N.**`, the flat one must not (the only flat sheets are legacy
    # unindexed ones, and requiring an index there would red a correct close).
    grammar = (
        re.compile(r"^- (\[.+\] )?\*\*\d+\.\*\* .+ => .+$")
        if hierarchical
        else re.compile(r"^- (\[.+\] )?.+ => .+$")
    )
    steps = []
    scenario = None
    for l in qa_text:
        m = re.match(r"^###\s+(.+?)\s*$", l)
        if m:
            scenario = m.group(1)
            continue
        if re.match(r"^##\s", l):
            scenario = None  # a non-scenario heading closes the section
            continue
        if not l.startswith("- "):
            continue

        # 10. qa-step-outside-scenario - a step with no `###` above it has no
        #     drain target. Flat sheets have no scenarios; they drain by core.
        if hierarchical and scenario is None:
            fails.append(f"qa-step-outside-scenario: '{l.strip()}'")
            continue
        # 8. qa-line-malformed - grammar only. An undispositioned line is NOT
        #    an error: results may arrive late or never, so the sheet is a
        #    script, not a form.
        if not grammar.match(l):
            fails.append(f"qa-line-malformed: '{l.strip()}'")
            continue
        im = re.search(r"\*\*(\d+)\.\*\*", l)
        steps.append((scenario, int(im.group(1)) if im else 0, l))

    # 11. qa-index-broken - indices restart at 1 per scenario and increment by
    #     1, so a citation ("QA <Scenario> <N>") always resolves to exactly
    #     one step. Flat sheets carry no indices and are cited by bare number.
    if hierarchical:
        by_scenario = {}
        for sc, idx, line in steps:
            by_scenario.setdefault(sc, []).append(idx)
        for sc, idxs in by_scenario.items():
            expected = 1
            for idx in idxs:
                if idx != expected:
                    fails.append(
                        f"qa-index-broken: scenario '{sc}' expected index {expected}, got {idx}"
                    )
                expected += 1

    # 9. qa-drain-missed - each (durable) step's core maps to a DISTINCT
    #    standing-doc step line UNDER THE SCENARIO ITS OWN SECTION NAMES.
    #    Checking the named scenario (not "anywhere in the file") is what
    #    makes the drain a move rather than a judgement call: a step drained
    #    to the wrong scenario fails here instead of reading as a correct
    #    close. Only lines inside a scenario count - the usage header's
    #    bullets document the grammar.
    durable = [(sc, l) for sc, _idx, l in steps if re.search(r"\(durable\)\s*$", l)]
    if not durable:
        return
    if not os.path.isfile(os.path.join(root, STANDING_QA)):
        # This repo has no standing QA checklist yet. The drain has a source
        # but no destination - flagged, not invented: creating the doc is a
        # deliberate act (work-items.md), not a linter side effect. Soft note,
        # not a failure, so closes are not blocked on a doc that is not part
        # of the adopted process yet.
        print(
            f"note: qa-drain-unverifiable: {len(durable)} durable step(s) but no"
            f" {STANDING_QA} - standing QA doc not adopted; drain not checked."
        )
        return
    ledger = {}
    cur = None
    for l in read_doc(root, STANDING_QA):
        m = re.match(r"^##\s+(.+?)\s*$", l)
        if m:
            cur = None if m.group(1) == "How this doc works" else m.group(1)
            if cur is not None:
                ledger.setdefault(cur, [])
            continue
        if cur is None or not l.startswith("- "):
            continue
        ledger[cur].append(l[2:].strip())
    for sc, line in durable:
        # Strip in four steps, and keep the leading "- " strip SEPARATE from
        # the bracket one. TRAP: they used to share one pattern
        # ('^- \[.+\] (.+)$'), which silently broke the moment sheets went
        # bare - with no bracket the match fails, the "- " survives, the index
        # strip (anchored at ^\*\*) then also fails, and EVERY durable step
        # reports un-drained. The bracket strip stays greedy to the LAST "] "
        # so a "]" inside a legacy disposition can't truncate the core.
        core = line[2:]
        bm = re.match(r"^\[.+\] (.+)$", core)
        if bm:
            core = bm.group(1)  # legacy bracketed sheets only
        core = re.sub(r"^\*\*\d+\.\*\*\s*", "", core)
        core = re.sub(r"\s*\(durable\)\s*$", "", core).strip()

        if hierarchical:
            if sc not in ledger:
                fails.append(
                    f"qa-drain-missed: {STANDING_QA} has no scenario '{sc}' for step: '{core}'"
                )
                continue
            if core in ledger[sc]:
                ledger[sc].remove(core)
            else:
                fails.append(
                    f"qa-drain-missed: not drained to '{sc}' in {STANDING_QA}: '{core}'"
                )
        else:
            # Flat sheets name no destination; the core may land anywhere.
            for k in ledger:
                if core in ledger[k]:
                    ledger[k].remove(core)
                    break
            else:
                fails.append(
                    f"qa-drain-missed: durable step not drained to {STANDING_QA}: '{core}'"
                )


# ============================== --all MODE ==================================

def run_all(root, strict):
    fails, notes = run_citations(root, strict)
    closed_dir = os.path.join(root, CLOSED)
    if os.path.isdir(closed_dir):
        for f in sorted(os.listdir(closed_dir)):
            if f.endswith(".md"):
                check_summary_shape(root, f"{CLOSED}/{f}", fails)
    check_orphans(work_tree_files(root), fails)
    return fails, notes


# ============================== SELFTEST ====================================

def selftest():
    """Fixture round-trip in a throwaway git repo: a correct close passes, a
    broken summary and a dangling citation FAIL (the convention: a selftest
    must prove the gate can red, not only that it can green)."""
    # selftest: asserts-failure — a dangling ledger citation and a broken close
    # (surviving folder, bad summary shape) must each be flagged.
    with tempfile.TemporaryDirectory() as td:
        def w(rel, text):
            p = os.path.join(td, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w", encoding="utf-8") as f:
                f.write(text)

        def sh(*args):
            subprocess.run(
                ["git", "-C", td, *args], check=True, capture_output=True
            )

        sh("init", "--quiet", "-b", "develop")
        sh("config", "user.email", "t@t")
        sh("config", "user.name", "t")
        # A global commit.gpgsign=true would make the throwaway commits fail.
        sh("config", "commit.gpgsign", "false")
        w(f"{WORK}/DEFERRED.md", "# Deferred\n\n- **DEF-12 - real entry**\n")
        w(f"{WORK}/BACKLOG.md", "# Backlog\n\n*(No open entries.)*\n")
        w(f"{WORK}/IDEAS.md", "# Ideas\n\n*(No open entries.)*\n")
        w(f"{ITEMS}/03-fix/3-plan.md", "# Plan\n\n## QA\n\n### Boot\n\n- step\n")
        w(
            f"{ITEMS}/03-fix/QA.md",
            "# QA\n\n### Boot\n\n- **1.** launch => clean exit\n",
        )
        w("docs/note.md", "Cites DEF-12 and also DEF-99 here.\n")
        sh("add", "-A")
        sh("commit", "--quiet", "-m", "base")

        # Citations: DEF-12 defined -> ok; DEF-99 -> dangling.
        fails, _ = run_citations(td, strict=False)
        assert any("dangling-citation" in f and "DEF-99" in f for f in fails), fails
        assert not any("DEF-12" in f for f in fails), fails

        # Item close, broken: folder still present + no summary.
        fails = run_item(td, "03-fix", "HEAD")
        assert any(f.startswith("folder-survived") for f in fails), fails
        assert any(f.startswith("summary-missing-or-dup") for f in fails), fails

        # Correct close: folder collapsed, well-formed summary.
        import shutil

        shutil.rmtree(os.path.join(td, ITEMS, "03-fix"))
        w(
            f"{CLOSED}/03-fix.md",
            "# 03-fix\n\n> Status: Closed (2026-01-01)\n\n"
            "## Shipped\n\nx\n\n## Key decisions & reasons\n\nx\n\n"
            "## Spawned work\n\nNone.\n",
        )
        fails = run_item(td, "03-fix", "HEAD")
        # The QA sheet is durable-free, so only shape checks apply - none fail.
        assert fails == [], fails

        # Broken summary shape reds.
        w(
            f"{CLOSED}/03-fix.md",
            "# 03-fix\n\n> Status: Open\n\n## Shipped\n\nx\n",
        )
        fails = run_item(td, "03-fix", "HEAD")
        assert any(f.startswith("wrong-status-line") for f in fails), fails
        assert any(f.startswith("wrong-section-set") for f in fails), fails

    print("work_item_lint selftest: PASS")
    return 0


# ================================ CLI =======================================

def report(label, fails, notes):
    for n in notes:
        print(n)
    if not fails:
        print(f"work_item_lint {label}: PASS")
        return 0
    print(f"work_item_lint {label}: FAIL ({len(fails)})")
    for i, f in enumerate(fails, 1):
        print(f"  {i}. {f}")
    return 1


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="work-item close + ledger-citation linter (see module docstring)"
    )
    ap.add_argument("--item", metavar="NN-slug", help="check a work item's close")
    ap.add_argument("--citations", action="store_true", help="repo-wide citation sweep")
    ap.add_argument("--all", action="store_true", dest="all_mode",
                    help="citations + closed-summary shapes + orphan scan (CI mode)")
    ap.add_argument("--selftest", action="store_true", help="fixture round-trip")
    ap.add_argument("--strict", action="store_true",
                    help="promote ledger-id-in-source hits to failures")
    ap.add_argument("--ref", default="HEAD",
                    help="git ref the deleted per-item files are read from (--item only)")
    ap.add_argument("--root", default=None, help="repo root (default: git toplevel)")
    args = ap.parse_args(argv)

    modes = [m for m, on in [
        ("item", args.item), ("citations", args.citations),
        ("all", args.all_mode), ("selftest", args.selftest),
    ] if on]
    if len(modes) != 1:
        ap.error("pass exactly one of --item / --citations / --all / --selftest")
    # --strict only qualifies the citation sweep; accepting it elsewhere
    # silently does nothing. --ref only feeds the deleted-file reads.
    if args.strict and modes[0] not in ("citations", "all"):
        ap.error("--strict applies only to --citations / --all")
    if args.ref != "HEAD" and modes[0] != "item":
        ap.error("--ref applies only to --item")

    if args.selftest:
        return selftest()

    root = args.root
    if root is None:
        r = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True
        )
        rc, out = r.returncode, r.stdout
        if rc != 0 or not out.strip():
            print("work_item_lint: not inside a git work tree", file=sys.stderr)
            return 2
        root = out.strip()

    if args.item:
        return report(f"close {args.item}", run_item(root, args.item, args.ref), [])
    if args.citations:
        fails, notes = run_citations(root, args.strict)
        return report("citations", fails, notes)
    fails, notes = run_all(root, args.strict)
    return report("all", fails, notes)


if __name__ == "__main__":
    sys.exit(main())
