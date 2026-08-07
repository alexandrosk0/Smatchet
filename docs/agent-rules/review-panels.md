# Review panels

Shared mechanics for the multi-model panel reviews that gate work items — the pre-implementation and
post-implementation reviews of [work-items.md](work-items.md), plus the addresser that resolves
them. Ported from Whip-Process (`Procedures/ReviewBasics.md`); contract wording carried verbatim.
Review *policy* (problems only, verify then stop, dispositions) lives in
[work-items.md → Review](work-items.md#review). The launcher is
`agents/scripts/core/run-review.sh` (Phase 2 of docs/plans/absorb-whip-process.md); the contracts
below bind that port.

## Review independently

Form your own findings from the artifacts / code **before** reading any sibling review file — the
panel's worth is *independent* passes, so a review that merely consolidates others' findings adds
anchoring, not signal. Cross-checking peers to set a finding's disposition is fine once your own
pass is done.

## Reviewer identity: the launcher names the file

Review files are named for their author so a panel writes in parallel without collision:
**`[prefix]review-[round]-[harness]-[model].md`** — prefix per gate (`4-` pre-implementation, `5-`
post-implementation), e.g. `4-review-1-claude-opus.md`. (Whip also names a `close-` closing-review
prefix; the close here has no panel review, so that prefix is **reserved, not active** — noted so
the naming grammar still matches the source if a closing review is ever ported.)

**The launcher owns the naming.** It runs one model per invocation from the roster (a
`review-panel` block in `project.config.json` when the Phase 2 port lands; Whip's source of record
is `Tools/review-roster.psd1` — per-harness model-selection flag + a list of `{tag, id}` models),
takes the round from its required round argument, and hands each reviewer the exact output path; the
reviewer writes there — it does not choose its own harness, model, or round.

- **harness** — a short hyphenless lowercase tag for the agent/tool (`claude`, `codex`, `cursor`,
  `opencode`).
- **model** — a short lowercase tag for the LLM (`opus`, `sonnet`, `sol`, `deepseek`, …).
- **round** — **one number for the whole invocation** (every leg shares it), so a leg that trips
  can't desync the numbering. It's a **required argument** — never inferred from folder state, so
  naming is always explicit. Re-run the same round (optionally restricted to specific legs) to fill
  a tripped leg in place; bump it for a fresh pass. A round is **one fresh invocation of the gate**.
  Revising the *current* pass (correcting, defending, or re-verifying when challenged — e.g. asked
  "are you sure?") updates the same file in place; a challenge is not a new round. (Resolution files
  instead track the *review* round they resolve.)

## Output file template

Problems only, verified — no praise, no restatement
([work-items.md → Review](work-items.md#review)). The gate sets the title and the "against …" line;
the body shape is:

```text
# <title per the gate> (pass N)

Problems only. Reviewed commit `<hash>` (<subject>) against <what>. No praise, no restatement.

---

## 1. <one-line problem statement>

<What's wrong, where (file:line / artifact §), and why it matters — verified against the actual
code/artifact.>

- **Disposition:** fix | defer (→ <destination>) | accept — <what to change / why accepted>

---

## 2. <next problem>
...
```

If the pass finds nothing actionable, the body is a single line saying so — that is the stop signal.

## Verdict trailer → verifier gate

A post-implementation leg (`5-` prefix) ends its review file with a machine-readable trailer: a
`## Verdict` heading followed by one fenced `json` object (fields and veto policy:
adversarial-code-review skill § Panel leg). The verdict lives *inside* the leg's review file
because the write guard's expected set is exactly the review file paths — a sibling verdict file
would trip the stray-path report. Pre-implementation legs (`4-`) carry no trailer: the verifier
gate scores code diffs, not artifacts.

The flow (run by the addresser after the round is resolved — address-review-feedback skill
step 6): `agents/scripts/core/panel_verdicts.py --subject NN --round N` collects one sample per
leg → `scripts/dev/verifier-sidecar.py aggregate` merges the samples into one verdict →
`review-ack.sh --record --branch --verdict` pins it to the branch content. Degradation is
asymmetric by design: a leg with *no* trailer is skipped with a warning (one lost sample, not a
lost round); a trailer that exists but is malformed is fatal (it may hide a veto); zero usable
samples is fatal (the adapter never invents a verdict). The panel is the first real producer of
`hard_veto` — only a veto blocks (`review-ack.sh --check` exits 3); the aggregate score stays
advisory until calibrated.

## Write-guard contracts

The launcher guards against panel legs mutating the working tree — reviewers read code and write
exactly one review file each. The guard tool ports in Phase 2; these two contracts are load-bearing
and bind the port (they are what makes the guard trustworthy — a port that loses either has ported
the shape, not the tool):

**Dirty-state is keyed on a content signature, not path membership.** The guard's baseline maps each
dirty path to `<XY status letters>:<sha256 of the working-tree file, or "absent">:<index blob id, or
"-">` — status letters + file hash + index blob, so a leg that *edits a file that was already dirty*
still trips the report: the path was in the baseline either way, but its signature changed. A guard
keyed on path membership alone would read such an edit as clean. Untracked files are enumerated
(`git status -uall`) so a new file inside an untracked directory is a distinct path, and a rename
(`R old -> new`) records both sides. The stray-path report covers all four drift shapes against the
expected set: new paths, re-edited (signature-changed) paths, deleted paths, and reverted
(disappeared-from-dirty) paths.

**Git failure returns UNVERIFIED — the guard must not fail open.** When `.git` is missing or the
`git status` call itself fails, the guard returns null/UNVERIFIED, **never** an empty "clean"
result: null means *could not verify*, an empty set means *verified clean*, and conflating them
turns every git outage into a silent pass. While legs are still running, a clean interim report is
stated as "clean **so far**", not "clean" — the verdict is only final once every leg has landed.
