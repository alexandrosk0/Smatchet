#!/usr/bin/env bash
# ci-check-resolve.sh — shared name->workflow resolution library (sourced, not run).
# ----------------------------------------------------------------------------
# Given a GitHub check-name string (the rendered status-context name), resolve
# which `.github/workflows/*.yml` file + job emits it, and surface that job's
# `on.pull_request` trigger shape: whether the WORKFLOW carries a
# `paths:` / `paths-ignore:` filter on its `pull_request` event, and whether the
# job carries the documented self-gate marker comment.
#
# WHY THIS EXISTS
#   Two CI gate-escape classes share one question — "given a check-name, which
#   workflow+job emits it and what are that job's PR triggers":
#     * required-context parity (test-required-context-parity.sh) — a REQUIRED
#       branch-protection context whose workflow is `paths:`-filtered deadlocks
#       every PR that misses those paths (the #880/#881/#882 wedge).
#     * (future) allow-list-present assertion (infra.md follow-up).
#   Build the map ONCE here; consumers source this file and call the API.
#
# NAME SHAPES GitHub renders for a job:
#   * `jobs.<id>.name: "Foo"`  -> the status context is "Foo".
#   * job with NO `name:`       -> the status context is the job id "<id>".
#   * templated `name: "Foo (${{ matrix.x }})"` -> flagged TEMPLATED; a concrete
#     required context matching the literal prefix (before the first `${{`)
#     resolves by prefix so a matrix-leg required context still resolves.
#
# PARSING: Python-only (`yaml.safe_load`) — no `yq` CI dependency. Python is
# already a hard dep (test-agent-contract.sh). The Windows python3 Store-alias
# stub passes `command -v` but exits 49 when run, so probe each candidate.
#
# SELF-GATE MARKER (the escape convention):
#   A job whose workflow legitimately runs unconditionally but internally
#   short-circuits (the #884 shape) carries the magic comment
#       # ci-required-context: self-gated
#   anywhere in that workflow file. The parity gate treats a self-gate-marked
#   context as parity-OK even if (hypothetically) a filter were present.
#
# API (source this file, then call):
#   ci_resolve_check "<check name>"
#       Emits one TAB-separated record to stdout and returns 0 on resolve:
#         <status>\t<workflow_file>\t<job_id>\t<has_pr_paths_filter>\t<self_gated>\t<templated>
#       <status> is RESOLVED or UNRESOLVABLE. On UNRESOLVABLE the remaining
#       fields are "-" and the return code is 1 (fail-closed: an unresolvable
#       required context is itself the deadlock risk).
#   ci_resolve_dump [<workflows_dir>]
#       Debug: emit every (check-name -> record) the map knows, one per line.
#
# ENV:
#   CI_WORKFLOWS_DIR   override the scanned dir (default .github/workflows);
#                      used by --selftest / bats to point at a fixture dir.
#
# This file is sourced; it defines functions + a $CI_RESOLVE_PY interpreter and
# does NOT `set -e` (the sourcing script owns shell options).
# ----------------------------------------------------------------------------

# Resolve a working python interpreter once (command -v alone is insufficient on
# Windows — the python3 Store-alias stub passes it but exits 49 when run). We
# PREFER an interpreter that can `import yaml` (the parser hard-requires PyYAML);
# only if none has yaml do we fall back to the first merely-runnable one (so the
# error surfaces as the clear "PyYAML not installed" message from the parser,
# not a confusing interpreter-missing error). On the msys2 UCRT64 box `python3`
# runs but lacks PyYAML while the Windows `python` has it — preferring the
# yaml-capable interpreter keeps a local `bash test-…parity.sh` working without
# a pip install, matching the CI behaviour where setup-python's python3 has it.
ci_resolve_py() {
    if [ -n "${CI_RESOLVE_PY:-}" ]; then printf '%s\n' "$CI_RESOLVE_PY"; return 0; fi
    local c p first=""
    for c in python3 python py; do
        p="$(command -v "$c" 2>/dev/null)" || continue
        "$p" -c "" >/dev/null 2>&1 || continue
        [ -n "$first" ] || first="$p"
        if "$p" -c "import yaml" >/dev/null 2>&1; then
            CI_RESOLVE_PY="$p"; printf '%s\n' "$p"; return 0
        fi
    done
    if [ -n "$first" ]; then CI_RESOLVE_PY="$first"; printf '%s\n' "$first"; return 0; fi
    return 1
}

# ci_resolve_check "<name>" — see header. Fail-closed: UNRESOLVABLE -> rc 1.
ci_resolve_check() {
    local name="$1"
    local wf_dir="${CI_WORKFLOWS_DIR:-.github/workflows}"
    local py
    py="$(ci_resolve_py)" || { echo "ci-check-resolve: no working python interpreter" >&2; return 2; }
    [ -d "$wf_dir" ] || { echo "ci-check-resolve: workflows dir not found: $wf_dir" >&2; return 2; }

    # Pass the check-name + dir through argv (never interpolated into source).
    CI_RESOLVE_NAME="$name" CI_RESOLVE_WFDIR="$wf_dir" "$py" - <<'PY'
import glob, os, sys

try:
    import yaml
except ImportError:
    sys.stderr.write("ci-check-resolve: PyYAML not installed (pip install pyyaml)\n")
    sys.exit(2)

name = os.environ["CI_RESOLVE_NAME"]
wf_dir = os.environ["CI_RESOLVE_WFDIR"]

SELF_GATE_MARKER = "# ci-required-context: self-gated"


def pr_block(on):
    """Return the pull_request mapping (or {} / None) from a parsed `on:`.

    YAML parses the bareword `on` key as the boolean True, so accept both.
    """
    if not isinstance(on, dict):
        return None
    for k in ("pull_request", True):
        if k in on:
            return on[k]
    return None


def has_pr_paths_filter(on):
    """True iff the pull_request trigger carries paths:/paths-ignore:."""
    blk = pr_block(on)
    if not isinstance(blk, dict):
        return False
    return ("paths" in blk) or ("paths-ignore" in blk)


def pr_triggered(on):
    """True iff this workflow runs on pull_request at all."""
    if not isinstance(on, dict):
        return False
    return ("pull_request" in on) or (True in on)


# Build the map: rendered-check-name -> record. A job with an explicit name:
# renders that string; without one, the job id. Templated names (containing
# ${{ ) are matched by their literal prefix.
records = []  # (literal_or_prefix, templated_bool, record_dict)

for path in sorted(glob.glob(os.path.join(wf_dir, "*.yml")) +
                   glob.glob(os.path.join(wf_dir, "*.yaml"))):
    try:
        with open(path, encoding="utf-8") as fh:
            raw = fh.read()
        doc = yaml.safe_load(raw)
    except Exception as exc:  # noqa: BLE001 — fail-closed on any parse error
        sys.stderr.write("ci-check-resolve: cannot parse %s: %s\n" % (path, exc))
        sys.exit(2)
    if not isinstance(doc, dict):
        continue
    on = doc.get("on", doc.get(True))
    jobs = doc.get("jobs")
    if not isinstance(jobs, dict):
        continue
    self_gated = SELF_GATE_MARKER in raw
    paths_filter = has_pr_paths_filter(on)
    is_pr = pr_triggered(on)
    wf_file = os.path.basename(path)
    for job_id, job in jobs.items():
        rendered = job_id
        if isinstance(job, dict) and job.get("name"):
            rendered = str(job["name"])
        templated = "${{" in rendered
        literal = rendered.split("${{")[0].rstrip() if templated else rendered
        records.append((literal, templated, {
            "workflow_file": wf_file,
            "job_id": str(job_id),
            "has_pr_paths_filter": paths_filter,
            "self_gated": self_gated,
            "pr_triggered": is_pr,
        }))


def emit(status, rec, templated):
    if rec is None:
        sys.stdout.write("UNRESOLVABLE\t-\t-\t-\t-\t-\n")
        return
    sys.stdout.write("%s\t%s\t%s\t%s\t%s\t%s\n" % (
        status,
        rec["workflow_file"],
        rec["job_id"],
        "true" if rec["has_pr_paths_filter"] else "false",
        "true" if rec["self_gated"] else "false",
        "true" if templated else "false",
    ))


# 1) exact literal match (non-templated).
for literal, templated, rec in records:
    if not templated and literal == name:
        emit("RESOLVED", rec, False)
        sys.exit(0)

# 2) templated prefix match: a required context naming a concrete matrix leg
#    (e.g. "Build (linux)") resolves against `name: Build (${{ matrix.os }})`.
for literal, templated, rec in records:
    if templated and literal and name.startswith(literal):
        emit("RESOLVED", rec, True)
        sys.exit(0)

# Fail-closed: no job renders this check name.
emit("UNRESOLVABLE", None, False)
sys.exit(1)
PY
}

# ci_resolve_dump [<dir>] — debug helper: list every known rendered check name
# and its record. Returns 0 unless the interpreter/dir is missing.
ci_resolve_dump() {
    local wf_dir="${1:-${CI_WORKFLOWS_DIR:-.github/workflows}}"
    local py
    py="$(ci_resolve_py)" || { echo "ci-check-resolve: no working python interpreter" >&2; return 2; }
    [ -d "$wf_dir" ] || { echo "ci-check-resolve: workflows dir not found: $wf_dir" >&2; return 2; }
    CI_RESOLVE_WFDIR="$wf_dir" "$py" - <<'PY'
import glob, os, sys
try:
    import yaml
except ImportError:
    sys.stderr.write("ci-check-resolve: PyYAML not installed\n"); sys.exit(2)
wf_dir = os.environ["CI_RESOLVE_WFDIR"]
MARKER = "# ci-required-context: self-gated"
for path in sorted(glob.glob(os.path.join(wf_dir, "*.yml")) +
                   glob.glob(os.path.join(wf_dir, "*.yaml"))):
    with open(path, encoding="utf-8") as fh:
        raw = fh.read()
    doc = yaml.safe_load(raw)
    if not isinstance(doc, dict):
        continue
    jobs = doc.get("jobs")
    if not isinstance(jobs, dict):
        continue
    for job_id, job in jobs.items():
        rendered = job_id
        if isinstance(job, dict) and job.get("name"):
            rendered = str(job["name"])
        sys.stdout.write("%s\t%s\t%s\t%s\n" % (
            rendered, os.path.basename(path), job_id,
            "self-gated" if MARKER in raw else "-"))
PY
}
