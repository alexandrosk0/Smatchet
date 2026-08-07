#!/usr/bin/env python3
"""test_work_item_lint.py - regression suite for agents/scripts/core/work_item_lint.py.

Scenario set ported from Whip-Process Tools/test-check-docs.ps1 (Part 2 fixtures
+ Part 3 citations), re-seeded at this repo's layout (docs/work/...) and driven
through the module's functions directly instead of one PowerShell process per
scenario. The corpus part (Part 1) is not ported: docs/work/closed/ has no
adopted items yet and the always-on sweep of it lives in `--all`
(scripts/dev/test-docs.sh).

Each scenario builds a throwaway git repo from tests/fixtures/qa-mini-item/,
drives it through the close lifecycle (commit the item folder, collapse it,
write the summary), and asserts PASS on the happy paths plus one red per
invariant - including the false-green guards (nested drain cores, "]" inside a
legacy disposition, off-grammar sheet).

stdlib unittest only (no pytest on the dev boxes / CI runner). Run via
scripts/dev/test-work-item-lint.sh, which emits the canonical
`Passed: N  Failed: M` line.
"""

import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "agents", "scripts", "core"))
import work_item_lint as wil  # noqa: E402

FIXTURES = os.path.join(ROOT, "tests", "fixtures", "qa-mini-item")


def read_fixture(name):
    with open(os.path.join(FIXTURES, name), encoding="utf-8") as f:
        return f.read()


PLAN = read_fixture("3-plan.md")
SHEET = read_fixture("QA.md")
SPAWNED = read_fixture("Spawned.md")
SUMMARY = read_fixture("summary.md")
DRAINED = read_fixture("work-qa.md")
QA_HEADER = "# QA - standing checklist\n\nUsage header only; empty (pre-drain).\n"
ITEM = "07-qa-mini"
# Minimal project config: only what the linter reads (source classification).
CONFIG = '{"ci": {"path_filters": {"code_globs": ["Source/**"], "docs_ignore": []}}}\n'


def write_file(root, rel, text):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def git(root, *args):
    subprocess.run(
        ["git", "-C", root, *args], check=True, capture_output=True
    )


def init_repo(root):
    git(root, "init", "--quiet", "-b", "develop")
    git(root, "config", "user.email", "t@t")
    git(root, "config", "user.name", "t")
    git(root, "config", "core.autocrlf", "false")
    # A global commit.gpgsign=true would make the throwaway commits fail.
    git(root, "config", "commit.gpgsign", "false")


class TempRepoCase(unittest.TestCase):
    def new_repo(self):
        """A FRESH throwaway root per call - several scenarios build more than
        one repo inside a single test method."""
        td = tempfile.mkdtemp(prefix="wil-")
        self.addCleanup(shutil.rmtree, td, ignore_errors=True)
        return td


class ItemCloseTests(TempRepoCase):
    """The close lifecycle + QA invariants (Whip scenarios 1-14)."""

    def scenario(
        self,
        plan=PLAN,
        sheet=SHEET,
        summary=SUMMARY,
        work_qa_pre=QA_HEADER,
        work_qa_post=DRAINED,
        omit_sheet=False,
        commit_collapse=False,
        omit_work_qa=False,
    ):
        """Build the repo, commit the item folder, collapse it, run the item
        check. Returns (fails, printed_stdout). The repo is kept on self.td so
        a test can poke it further and re-run."""
        td = self.td = self.new_repo()
        init_repo(td)
        # -omit_work_qa models a repo with NO standing checklist at all (this
        # repo's current state), a different absence from omit_sheet's missing
        # per-item sheet.
        if not omit_work_qa:
            write_file(td, wil.STANDING_QA, work_qa_pre)
        write_file(td, f"{wil.ITEMS}/{ITEM}/3-plan.md", plan)
        if not omit_sheet:
            write_file(td, f"{wil.ITEMS}/{ITEM}/QA.md", sheet)
        write_file(td, f"{wil.ITEMS}/{ITEM}/Spawned.md", SPAWNED)
        git(td, "add", "-A")
        git(td, "commit", "--quiet", "-m", "item present")
        # collapse (working tree)
        shutil.rmtree(os.path.join(td, wil.ITEMS, ITEM))
        write_file(td, f"{wil.CLOSED}/{ITEM}.md", summary)
        if not omit_work_qa:
            write_file(td, wil.STANDING_QA, work_qa_post)
        if commit_collapse:
            git(td, "add", "-A")
            git(td, "commit", "--quiet", "-m", "collapse")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            fails = wil.run_item(td, ITEM, "HEAD")
        return fails, buf.getvalue()

    def assert_fires(self, fails, code):
        self.assertTrue(
            any(f.startswith(code) for f in fails), f"expected {code} in {fails}"
        )

    def test_happy_path_uncommitted_collapse(self):
        fails, _ = self.scenario()
        self.assertEqual(fails, [])

    def test_happy_path_committed_collapse(self):
        # HEAD = the deletion commit; the rev-list walk resolves the prior blob.
        fails, _ = self.scenario(commit_collapse=True)
        self.assertEqual(fails, [])

    def test_omitted_sheet_fires_qa_sheet_missing(self):
        fails, _ = self.scenario(omit_sheet=True)
        self.assert_fires(fails, "qa-sheet-missing")

    def test_legacy_bracketed_sheet_passes(self):
        # ANCHOR TEST. Sheets no longer record dispositions, so the bracket is
        # ACCEPTED, never emitted - incl. the undispositioned [ ] form.
        legacy = (
            "# S\n\n### Boot\n"
            "- [pass] **1.** Launch the app => main menu appears (durable)\n"
            "- [pass] **2.** Resize the window => layout reflows (durable)\n"
            "- [ ] **3.** Toggle the debug overlay => overlay hides, no crash\n"
        )
        fails, _ = self.scenario(sheet=legacy)
        self.assertEqual(fails, [])

    def test_dropped_durable_fires_qa_drain_missed(self):
        partial = "\n".join(
            l for l in DRAINED.splitlines() if "Resize the window" not in l
        )
        fails, _ = self.scenario(work_qa_post=partial)
        self.assert_fires(fails, "qa-drain-missed")

    def test_nested_core_false_green_guard(self):
        # Two durable cores, one a substring of the other; the SHORTER dropped.
        # A substring match would false-pass; distinct equality must red.
        plan = (
            "# P\n\n## QA\n\n### X\n- Arm the drone => motors spin (durable)\n"
            "- Arm the drone => motors spin up, HUD shows ARMED (durable)\n"
        )
        sheet = (
            "# S\n\n### X\n- **1.** Arm the drone => motors spin (durable)\n"
            "- **2.** Arm the drone => motors spin up, HUD shows ARMED (durable)\n"
        )
        work = "# QA\n\n## X\n- Arm the drone => motors spin up, HUD shows ARMED\n"
        fails, _ = self.scenario(plan=plan, sheet=sheet, work_qa_post=work)
        self.assert_fires(fails, "qa-drain-missed")

    def test_missing_arrow_fires_qa_line_malformed(self):
        sheet = "# S\n\n### Boot\n- **1.** Launch the app main menu appears (durable)\n"
        fails, _ = self.scenario(sheet=sheet)
        self.assert_fires(fails, "qa-line-malformed")

    def test_missing_index_fires_qa_line_malformed(self):
        sheet = "# S\n\n### Boot\n- Launch the app => main menu appears (durable)\n"
        fails, _ = self.scenario(sheet=sheet)
        self.assert_fires(fails, "qa-line-malformed")

    def test_bracket_inside_disposition_drains_by_core(self):
        # LEGACY BRANCH. The drain-core strip must land on the LAST "] " - the
        # old first-"]" strip corrupted the core.
        plan = "# P\n\n## QA\n\n### X\n- Arm the rig => motors spin (durable)\n"
        sheet = (
            "# S\n\n### X\n"
            "- [fail: fixed patched the [x] handler] **1.** Arm the rig => motors spin (durable)\n"
        )
        work = "# QA\n\n## X\n- Arm the rig => motors spin\n"
        fails, _ = self.scenario(plan=plan, sheet=sheet, work_qa_post=work)
        self.assertEqual(fails, [])

    def test_step_above_any_scenario_fires(self):
        # The case that used to be INVISIBLE: the old checker keyed on " => "
        # and skipped anything unmatched, so a whole off-grammar sheet closed
        # green.
        plan = "# P\n\n## QA\n\n### X\n- Arm the rig => motors spin (durable)\n"
        sheet = "# S\n\n- **1.** Arm the rig => motors spin (durable)\n"
        work = "# QA\n\n## X\n- Arm the rig => motors spin\n"
        fails, _ = self.scenario(plan=plan, sheet=sheet, work_qa_post=work)
        self.assert_fires(fails, "qa-step-outside-scenario")

    def test_index_gap_fires_qa_index_broken(self):
        plan = (
            "# P\n\n## QA\n\n### X\n- Arm the rig => motors spin (durable)\n"
            "- Disarm => motors stop (durable)\n"
        )
        sheet = (
            "# S\n\n### X\n- **1.** Arm the rig => motors spin (durable)\n"
            "- **3.** Disarm => motors stop (durable)\n"
        )
        work = "# QA\n\n## X\n- Arm the rig => motors spin\n- Disarm => motors stop\n"
        fails, _ = self.scenario(plan=plan, sheet=sheet, work_qa_post=work)
        self.assert_fires(fails, "qa-index-broken")

    def test_drain_to_wrong_scenario_fires(self):
        # Under an "anywhere in the file" match this passed, which is what let
        # the drain stay a judgement call instead of a move.
        plan = "# P\n\n## QA\n\n### X\n- Arm the rig => motors spin (durable)\n"
        sheet = "# S\n\n### X\n- **1.** Arm the rig => motors spin (durable)\n"
        work = "# QA\n\n## Y\n- Arm the rig => motors spin\n"
        fails, _ = self.scenario(plan=plan, sheet=sheet, work_qa_post=work)
        self.assert_fires(fails, "qa-drain-missed")

    def test_flat_unindexed_bracketed_sheet_passes(self):
        # FLAT GRAMMAR SITE. Flat mode is chosen by DETECTION (plan + sheet
        # both scenario-less), and the flat regex must never acquire an index
        # requirement.
        plan = (
            "# P\n\n## QA\n\nSetup: fresh launch.\n\n"
            "- Nothing to verify in PIE => docs and tooling only\n"
        )
        sheet = (
            "# S\n\n- [n/a: docs and tooling only] Nothing to verify in PIE"
            " => docs and tooling only\n"
        )
        fails, _ = self.scenario(plan=plan, sheet=sheet, work_qa_post=QA_HEADER)
        self.assertEqual(fails, [])

    def test_no_standing_qa_doc_soft_notes(self):
        # DEVIATION from the original (which fired qa-drain-missed): this repo
        # has not adopted a standing QA doc, so durable steps with no drain
        # destination are FLAGGED (a printed note), not failed - the linter
        # must not force the doc into existence as a side effect. The note
        # firing is the proof the branch ran; "no failure" alone would pass
        # with both sheets absent, testing nothing.
        fails, out = self.scenario(omit_work_qa=True)
        self.assertEqual([f for f in fails if f.startswith("qa-drain")], [])
        self.assertIn("qa-drain-unverifiable", out)

    # --- summary shape / mechanics (checks 1-6) ---

    def test_surviving_folder_and_missing_summary_fire(self):
        td = self.new_repo()
        init_repo(td)
        write_file(td, f"{wil.ITEMS}/{ITEM}/3-plan.md", "# P\n")
        git(td, "add", "-A")
        git(td, "commit", "--quiet", "-m", "item present")
        fails = wil.run_item(td, ITEM, "HEAD")
        self.assertTrue(any(f.startswith("folder-survived") for f in fails), fails)
        self.assertTrue(
            any(f.startswith("summary-missing-or-dup") for f in fails), fails
        )

    def test_broken_summary_shape_fires(self):
        bad = (
            "# QA Mini\n\n> Status: Open\n\n## Shipped\n\nx\n\n"
            "## Spawned work\n\nSee [gone](no-such-file.md).\n"
        )
        fails, _ = self.scenario(summary=bad)
        self.assert_fires(fails, "wrong-status-line")
        self.assert_fires(fails, "wrong-section-set")
        self.assert_fires(fails, "dangling-link")

    def test_orphaned_staged_file_fires(self):
        # A staged-shaped file stranded directly under docs/work/ (not inside
        # any items/NN-slug/ folder) is an orphan.
        fails, _ = self.scenario()
        self.assertEqual(fails, [])
        write_file(self.td, f"{wil.WORK}/2-design.md", "# stranded\n")
        fails = wil.run_item(self.td, ITEM, "HEAD")
        self.assertTrue(any(f.startswith("orphaned-staged-file") for f in fails), fails)


class CitationsTests(TempRepoCase):
    """Ledger-citation sweep (Whip scenarios 14-19, re-seeded on this repo's
    ledger set: DEFERRED/BACKLOG/IDEAS, bullet-shape entries, no BUG)."""

    LEDGER = "# Deferred\n\n- **DEF-001 - a real entry** - status: deferred\n"

    def citations(
        self,
        ledger=LEDGER,
        doc="# Notes\n",
        source=None,
        strict=False,
        closed_item=None,
        review_file=None,
        item_file=None,
    ):
        td = self.new_repo()
        init_repo(td)
        write_file(td, "project.config.json", CONFIG)
        write_file(td, f"{wil.WORK}/DEFERRED.md", ledger)
        write_file(td, f"{wil.WORK}/BACKLOG.md", "# Backlog\n")
        write_file(td, f"{wil.WORK}/IDEAS.md", "# Ideas\n")
        write_file(td, "docs/notes.md", doc)
        if closed_item:
            write_file(td, f"{wil.CLOSED}/{closed_item}.md", f"# {closed_item}\n\nClosed.\n")
        if review_file:
            write_file(td, f"{wil.ITEMS}/40-x/5-review-1-claude-opus.md", review_file)
        if item_file:
            write_file(td, f"{wil.ITEMS}/40-x/2-design.md", item_file)
        if source:
            write_file(td, "Source/X/Public/X.h", source)
        # `git ls-files` reads the index, so add is enough - no commit needed.
        git(td, "add", "-A")
        return wil.run_citations(td, strict)

    def assert_pass(self, result):
        fails, _notes = result
        self.assertEqual(fails, [])

    def assert_fires(self, result, text):
        fails, _notes = result
        self.assertTrue(any(text in f for f in fails), f"expected {text!r} in {fails}")

    def test_live_id_cited_passes(self):
        self.assert_pass(
            self.citations(doc="# Notes\n\nSee [DEF-001](work/DEFERRED.md).\n")
        )

    def test_dangling_id_fires(self):
        # The recorded false-negative: an injected DEF-999 once sailed through
        # a link-dense doc.
        self.assert_fires(
            self.citations(
                doc="# Notes\n\nSee [DEF-999](work/DEFERRED.md) and [DEF-001](work/DEFERRED.md).\n"
            ),
            "dangling-citation",
        )

    def test_empty_ledgers_no_citations_passes(self):
        # A fresh repo, not an alarm.
        self.assert_pass(self.citations(ledger="# Deferred\n"))

    def test_empty_ledgers_with_citation_fires_format_alarm(self):
        fails, _ = self.citations(
            ledger="# Deferred\n", doc="# Notes\n\nSee [DEF-001](work/DEFERRED.md).\n"
        )
        self.assertTrue(any("no-ledger-ids" in f for f in fails), fails)
        # ONE friendly summary, not a per-line flood.
        self.assertEqual(len(fails), 1, fails)

    def test_empty_ledgers_retired_forms_stay_legal(self):
        # Both accepted forms were rejected by a first empty-ledger
        # implementation that short-circuited before the exemptions.
        self.assert_pass(
            self.citations(ledger="# Deferred\n", doc="# Notes\n\nDEF-999 (retired) was folded in.\n")
        )
        self.assert_pass(
            self.citations(
                ledger="# Deferred\n",
                closed_item="13-rate-feedforward",
                doc="# Notes\n\nSuperseded: [DEF-999](work/closed/13-rate-feedforward.md).\n",
            )
        )

    def test_retired_marker_excuses_bare_prose_does_not(self):
        self.assert_pass(
            self.citations(doc="# Notes\n\nDEF-999 (retired) was folded into DEF-001.\n")
        )
        self.assert_fires(
            self.citations(doc="# Notes\n\nDEF-999 is retired.\n"), "dangling-citation"
        )

    def test_closed_link_exemption_requires_resolving_target(self):
        self.assert_pass(
            self.citations(
                closed_item="13-rate-feedforward",
                doc="# Notes\n\nSuperseded: [DEF-999](work/closed/13-rate-feedforward.md).\n",
            )
        )
        self.assert_fires(
            self.citations(doc="# Notes\n\nSuperseded: [DEF-999](work/closed/never-existed.md).\n"),
            "does not resolve",
        )

    def test_placeholder_targets_are_not_citations(self):
        # Docs that document this very check write exactly this shape; without
        # the exemption the gate re-reds on its own review prose each round.
        for ph in ("work/closed/…", "work/closed/...", "work/closed/<NN-slug>.md"):
            self.assert_pass(
                self.citations(doc=f"# Notes\n\nThe accepted form is [DEF-999]({ph}).\n")
            )
        # ...but a REAL-looking missing target still fails (the exemption stays
        # narrow).
        self.assert_fires(
            self.citations(doc="# Notes\n\nSee [DEF-999](work/closed/32-real-looking.md).\n"),
            "does not resolve",
        )

    def test_review_scratch_excluded_durable_artifacts_swept(self):
        # Review files quote tool output and are deleted at close - never a
        # durable dangling reference. The design file in the SAME folder stays
        # in scope.
        self.assert_pass(
            self.citations(review_file="# Review\n\nThe check reported DEF-999 and I disagree.\n")
        )
        self.assert_fires(
            self.citations(item_file="# Design\n\nThis builds on DEF-999.\n"),
            "dangling-citation",
        )

    def test_closed_content_grandfathered(self):
        self.assert_pass(self.citations(closed_item="13-rate-feedforward"))
        # A stale id INSIDE a closed summary is not swept.
        td = self.new_repo()
        init_repo(td)
        write_file(td, "project.config.json", CONFIG)
        write_file(td, f"{wil.WORK}/DEFERRED.md", self.LEDGER)
        write_file(td, f"{wil.WORK}/BACKLOG.md", "# Backlog\n")
        write_file(td, f"{wil.WORK}/IDEAS.md", "# Ideas\n")
        write_file(td, f"{wil.CLOSED}/09-old.md", "# Old\n\nResolved DEF-999 back then.\n")
        git(td, "add", "-A")
        fails, _ = wil.run_citations(td, strict=False)
        self.assertEqual(fails, [])

    def test_source_id_informational_unless_strict(self):
        src = "// Airmode is DEF-001.\nstruct X {};\n"
        fails, notes = self.citations(source=src)
        self.assertEqual(fails, [])
        self.assertTrue(any("ledger-id-in-source" in n for n in notes), notes)
        self.assert_fires(self.citations(source=src, strict=True), "ledger-id-in-source")


class CliTests(unittest.TestCase):
    """Flag-contract checks through the real CLI (Whip scenario 18)."""

    SCRIPT = os.path.join(ROOT, "agents", "scripts", "core", "work_item_lint.py")

    def run_cli(self, *args):
        return subprocess.run(
            [sys.executable, self.SCRIPT, *args], capture_output=True, text=True
        )

    def test_strict_outside_citations_rejected(self):
        r = self.run_cli("--item", "x", "--strict")
        self.assertEqual(r.returncode, 2)
        self.assertIn("--strict applies only to --citations", r.stderr)

    def test_ref_outside_item_rejected(self):
        r = self.run_cli("--citations", "--ref", "HEAD~1")
        self.assertEqual(r.returncode, 2)
        self.assertIn("--ref applies only to --item", r.stderr)

    def test_exactly_one_mode_required(self):
        r = self.run_cli()
        self.assertEqual(r.returncode, 2)
        r = self.run_cli("--citations", "--all")
        self.assertEqual(r.returncode, 2)


if __name__ == "__main__":
    result = unittest.main(exit=False, verbosity=2).result
    failed = len(result.failures) + len(result.errors)
    print(f"Passed: {result.testsRun - failed}  Failed: {failed}")
    sys.exit(1 if failed else 0)
