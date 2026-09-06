#!/usr/bin/env bash
# project-config.sh — load project.config.json into PC_* shell variables.
#
# Usage:  . scripts/dev/project-config.sh        # source it; sets PC_* in caller
#         bash scripts/dev/project-config.sh      # print the exports (debug)
#
# The portable agentic layer (agents/core, agents/_shared, docs/agent-rules,
# docs/harness, generic scripts) must read project-specific values from here
# instead of hardcoding them, so the layer is reusable in another project by
# rewriting project.config.json alone. See docs/PORTABILITY.md.
#
# Scalars -> PC_<UPPER_SNAKE>. Arrays -> space-joined PC_<NAME> (use `read -ra`).
# Uses python (a required tool; jq may be absent on the Windows dev env).

# Resolve the agent-layer root from this script's location (scripts/dev/ -> root
# of whichever tree carries this file). This script is deliberately dual-homed:
# it ships inside the agent layer (so the layer can bootstrap standalone) and in
# the host project (so the host works before the layer is checked out). Nothing
# below assumes which of the two it is running from.
_pc_self="${BASH_SOURCE[0]:-$0}"
_pc_layer_root="$(cd "$(dirname "$_pc_self")/../.." && pwd)"
# `git rev-parse --show-superproject-working-tree` prints the host tree when this
# copy lives inside a submodule, and nothing otherwise. Strip a trailing CR: on
# Windows the value can arrive with one and would corrupt every path built from it.
_pc_super="$(git -C "$_pc_layer_root" rev-parse --show-superproject-working-tree 2>/dev/null | tr -d '\r')"

# project.config.json resolution, most specific first:
#   rung 0  PC_CONFIG_FILE          — an exact file named by the caller (bats fixtures)
#   rung 1  SMATCHET_PROJECT_CONFIG — a file or a directory (documented escape hatch)
#   rung 2  the superproject working tree, when this copy lives in a submodule
#   rung 3  this tree's own root — the host pre-flip, the layer standalone
_pc_resolve_config() {
  if [ -n "${PC_CONFIG_FILE:-}" ]; then
    printf '%s\n' "$PC_CONFIG_FILE"
    return
  fi
  if [ -n "${SMATCHET_PROJECT_CONFIG:-}" ]; then
    if [ -d "$SMATCHET_PROJECT_CONFIG" ]; then
      printf '%s/project.config.json\n' "$SMATCHET_PROJECT_CONFIG"
    else
      printf '%s\n' "$SMATCHET_PROJECT_CONFIG"
    fi
    return
  fi
  if [ -n "$_pc_super" ] && [ -f "$_pc_super/project.config.json" ]; then
    printf '%s/project.config.json\n' "$_pc_super"
    return
  fi
  printf '%s/project.config.json\n' "$_pc_layer_root"
}
PC_CONFIG_FILE="$(_pc_resolve_config)"

if [ ! -f "$PC_CONFIG_FILE" ]; then
  echo "project-config.sh: $PC_CONFIG_FILE not found" >&2
  return 1 2>/dev/null || exit 1
fi

# Pick a python that actually RUNS. On Windows, `python3` (and sometimes
# `python`) can resolve to a non-functional Microsoft Store execution-alias stub
# that prints "Python was not found" and exits nonzero instead of running — so
# probe `--version` and require success before accepting a candidate, rather
# than trusting the first name `command -v` happens to find.
_pc_python=""
for _pc_cand in python3 python py; do
  if command -v "$_pc_cand" >/dev/null 2>&1 && "$_pc_cand" --version >/dev/null 2>&1; then
    _pc_python="$_pc_cand"
    break
  fi
done
if [ -z "$_pc_python" ]; then
  echo "project-config.sh: no working python found (tried python3, python, py)" >&2
  return 1 2>/dev/null || exit 1
fi

# The schema must follow the RESOLVED config, not this script's own root: pairing
# one repo's config with another's schema would validate the wrong required-key
# list, and the FileNotFoundError branch below would silently skip the gate.
PC_SCHEMA_FILE="${PC_SCHEMA_FILE:-$(dirname "$PC_CONFIG_FILE")/project.config.schema.json}"

# Dual-root pair. AGENT_LAYER_ROOT is the tree holding agents/, docs/agent-rules/
# and docs/harness/; PROJECT_ROOT is the tree holding plans, backlog entries,
# Source/ and project.config.json. Pre-flip both are the repo root, so every
# consumer rewritten to address a tree through one of these is a provable no-op.
# Both honour a caller-set value, which is what lets the layer's standalone CI
# force PROJECT_ROOT=$AGENT_LAYER_ROOT and the flip set AGENT_LAYER_ROOT=agent-layer
# without touching the resolution logic above.
#
# Note the bare name PROJECT_ROOT is also a LOCAL variable in the Unreal-plugin
# scripts under scripts/dev/local/ and scripts/publish/, where it means "the
# Unreal project to deploy into". None of those source this file and each assigns
# the variable before first use, so the export cannot leak in — do not "fix" the
# apparent collision. PC_PROJECT_ROOT is the namespaced alias for callers that
# prefer to avoid the bare name entirely.
AGENT_LAYER_ROOT="${AGENT_LAYER_ROOT:-$_pc_layer_root}"
PROJECT_ROOT="${PROJECT_ROOT:-$(dirname "$PC_CONFIG_FILE")}"
PC_AGENT_LAYER_ROOT="$AGENT_LAYER_ROOT"
PC_PROJECT_ROOT="$PROJECT_ROOT"
export AGENT_LAYER_ROOT PROJECT_ROOT PC_AGENT_LAYER_ROOT PC_PROJECT_ROOT

# Emit `KEY=value` lines (arrays space-joined). eval them into the caller.
# Fail-fast (exit 2) on a malformed config or a missing required top-level key
# rather than degrading into silent PC_*="" — a no-deps gate against the
# schema's `required` list (full JSON-Schema validation runs in CI).
_pc_exports="$("$_pc_python" - "$PC_CONFIG_FILE" "$PC_SCHEMA_FILE" <<'PY'
import json, shlex, sys
c = json.load(open(sys.argv[1], encoding="utf-8"))

# No-deps required-key gate (the full schema validation is a CI step).
try:
    schema = json.load(open(sys.argv[2], encoding="utf-8"))
    missing = [k for k in schema.get("required", []) if k not in c]
    if missing:
        print("project-config.sh: %s missing required key(s): %s"
              % (sys.argv[1], ", ".join(missing)), file=sys.stderr)
        sys.exit(2)
except FileNotFoundError:
    pass  # schema optional at load time; CI enforces it

def emit(key, val):
    if isinstance(val, list):
        val = " ".join(str(x) for x in val)
    elif isinstance(val, bool):
        val = "true" if val else "false"
    print("export PC_%s=%s" % (key, shlex.quote(str(val))))

p = c.get("project", {})
emit("PROJECT_NAME", p.get("name", ""))
emit("ENV_PREFIX", p.get("env_prefix", ""))
emit("PROJECT_LITERALS", p.get("literals", []))

b = c.get("build", {})
emit("BUILD_PRESETS", b.get("presets", []))
emit("BUILD_TARGETS", b.get("targets", []))
emit("BUILD_EXE_PATH", b.get("exe_path", ""))
emit("BUILD_MSVC_TOOLSET_PIN", b.get("msvc_toolset_pin", ""))

pf = c.get("perf", {})
emit("PERF_BUDGET_MS", pf.get("frame_budget_ms", ""))
emit("PERF_FLOOR_MS", pf.get("fps_floor_ms", ""))
emit("PERF_FREEZE_MS", pf.get("freeze_ms", ""))
emit("PERF_BASELINES_DIR", pf.get("baselines_dir", ""))
emit("PERF_POLICY", pf.get("policy", ""))

ln = c.get("lint", {})
z = ln.get("zones", {})
emit("LINT_STRICT_ZONES", z.get("strict", []))
emit("LINT_LIGHT_ZONES", z.get("light", []))
emit("LINT_EXEMPT_ZONES", z.get("exempt", []))
emit("LINT_RULES", ln.get("rules", []))
emit("LINT_DEVIATION_KEYWORD", ln.get("deviation_keyword", ""))
emit("LINT_BASELINE", ln.get("baseline", ""))

v = c.get("vcs", {})
emit("VCS_PRIMARY", v.get("primary", ""))
emit("VCS_OPTIONAL_LAYER", v.get("optional_layer") or "")
emit("VCS_P4_STREAMS", v.get("p4_streams", []))
emit("VCS_PROTECTED_BRANCHES", v.get("protected_branches", []))

ci = c.get("ci", {})
# Derived from branch_protection.required_contexts — the single source for the
# required-checks list (ci.required_checks was a verbatim duplicate, removed).
emit("CI_REQUIRED_CHECKS", c.get("branch_protection", {}).get("required_contexts", []))
emit("CI_CODE_GLOBS", ci.get("path_filters", {}).get("code_globs", []))
emit("CI_DOCS_IGNORE", ci.get("path_filters", {}).get("docs_ignore", []))

mg = c.get("merge_gates", {})
emit("CR_BOT_LOGINS", mg.get("cr_bot_logins", []))
emit("OVERRIDE_LABELS", mg.get("override_labels", []))

emit("VISUAL_TRIGGER_GLOBS", c.get("visual_validation", {}).get("trigger_globs", []))
emit("GOLDEN_DIR", c.get("golden", {}).get("artifacts_dir", ""))

h = c.get("harness", {})
emit("HARNESS_DISCOVERY_MODE", h.get("discovery_mode", ""))
emit("HARNESS_SUPPORTED", h.get("supported", []))

emit("ENABLED_PROJECT_AGENTS", c.get("agents", {}).get("enabled_project", []))
PY
)" || { echo "project-config.sh: failed to parse $PC_CONFIG_FILE" >&2; return 1 2>/dev/null || exit 1; }

# Lines are `export PC_...=...`. When sourced, eval them into the caller (they
# self-export); when executed directly, print them for debugging.
if (return 0 2>/dev/null); then
  eval "$_pc_exports"
else
  # The two roots are plain shell assignments above, not part of the python
  # output, so echo them alongside it to keep the debug dump complete.
  printf 'export PC_PROJECT_ROOT=%s\n' "$(printf '%q' "$PC_PROJECT_ROOT")"
  printf 'export PC_AGENT_LAYER_ROOT=%s\n' "$(printf '%q' "$PC_AGENT_LAYER_ROOT")"
  printf '%s\n' "$_pc_exports"
fi
