#!/usr/bin/env python3
"""Generate pi-native subagent files (.pi/agents/*.md) from the canonical
agent tree (agents/{core,project}/*.md).

Why this exists
---------------
pi's subagent extension discovers a FLAT `.pi/agents/*.md` dir and its parser
only understands four frontmatter keys: name, description, tools, model. The
canonical agents are Claude-Code-shaped (capability tags, harness-hints,
triggers, delegates-to, banners) — pi would find them in the wrong directory and,
even if found, run every one with all-default tools and the default model.

This generator bridges the gap deterministically:
  * flattens agents/{core,project}/ -> .pi/agents/
  * keeps name + description (description re-emitted as a quoted YAML scalar so
    colons/backticks in it can't break pi's real-YAML parser)
  * maps `capabilities:` -> pi `tools:` via the capability-adapter table
    (docs/harness/capability-adapter.md, "pi" column). Read-only agents (no
    file-edit/file-write capability) therefore never get edit/write tools.
  * resolves `model:` from a tier->pi-model map (env PI_MODEL_{OPUS,SONNET,HAIKU}
    overrides docs/harness/pi/model-map.json). Unmapped tier -> omit `model:` so
    the child pi uses the user's configured default model.
  * passes the body (system prompt) through unchanged — the richer orchestrator
    contract survives as prose even though pi doesn't enforce its frontmatter.

No third-party deps (PyYAML is NOT assumed present). The source frontmatter is
regular enough to parse with a small hand-rolled state machine.
"""
import json
import os
import re
import sys
from pathlib import Path

# capability tag -> pi tool name(s). Mirror docs/harness/capability-adapter.md.
# pi built-in / subagent-recognised tools: read, bash, edit, write, grep, find, ls.
CAP2TOOLS = {
    "file-read": ["read"],
    "file-write": ["write"],
    "file-edit": ["edit"],
    "text-search": ["grep"],
    "semantic-code-search": ["grep"],   # no pi semantic search -> text-search fallback
    "file-glob": ["find", "ls"],
    "file-skeleton": ["read"],           # no pi skeleton tool -> read fallback
    "shell": ["bash"],
    "git-history": ["bash"],
    # web-fetch intentionally dropped: pi has no built-in web tool here.
}
# Stable emit order so re-runs are byte-identical (idempotency probe relies on it).
TOOL_ORDER = ["read", "grep", "find", "ls", "bash", "edit", "write"]


def load_model_map(root: Path):
    """Tier -> pi model pattern. File first, env overrides. Empty => omit."""
    m = {"opus": "", "sonnet": "", "haiku": ""}
    cfg = root / "docs" / "harness" / "pi" / "model-map.json"
    if cfg.is_file():
        try:
            data = json.loads(cfg.read_text(encoding="utf-8"))
            for k in m:
                if isinstance(data.get(k), str):
                    m[k] = data[k].strip()
        except (ValueError, OSError):
            pass
    for k in m:
        env = os.environ.get("PI_MODEL_" + k.upper())
        if env is not None:
            m[k] = env.strip()
    return m


def split_frontmatter(text: str):
    """Return (frontmatter_lines, body) or (None, text) when no frontmatter."""
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    if not text.startswith("---\n"):
        return None, text
    end = text.find("\n---", 3)
    if end == -1:
        return None, text
    fm = text[4:end]
    body = text[end + 4:].lstrip("\n")
    return fm.split("\n"), body


def parse_fields(fm_lines):
    """Extract name, description, capabilities[], harness-hints.<h>.model.

    Hand-rolled: top-level `key: value`, top-level `key:` + `  - item` lists,
    and the two-level `harness-hints:` / `<harness>:` / `model:` nest. Good
    enough for the closed shape every canonical agent uses.
    """
    name = desc = None
    caps = []
    hint_models = {}            # harness -> model
    i, n = 0, len(fm_lines)
    while i < n:
        line = fm_lines[i]
        if re.match(r"^name:\s", line):
            name = line.split(":", 1)[1].strip()
        elif re.match(r"^description:\s", line):
            desc = line.split(":", 1)[1].strip()
        elif line.strip() == "capabilities:":
            i += 1
            while i < n and re.match(r"^\s+-\s", fm_lines[i]):
                caps.append(fm_lines[i].split("-", 1)[1].strip())
                i += 1
            continue
        elif line.strip() == "harness-hints:":
            i += 1
            cur = None
            while i < n and (fm_lines[i].startswith("  ") or not fm_lines[i].strip()):
                sub = fm_lines[i]
                mh = re.match(r"^  (\S+):\s*$", sub)            # "  claude-code:"
                mm = re.match(r"^    model:\s*(\S+)", sub)       # "    model: sonnet"
                if mh:
                    cur = mh.group(1)
                elif mm and cur:
                    hint_models[cur] = mm.group(1).strip()
                i += 1
            continue
        i += 1
    return name, desc, caps, hint_models


def caps_to_tools(caps):
    seen = set()
    for c in caps:
        for t in CAP2TOOLS.get(c, []):
            seen.add(t)
    return [t for t in TOOL_ORDER if t in seen]


def yaml_quote(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def resolve_model(hint_models, model_map):
    # Prefer an explicit pi hint, else map the claude-code tier alias.
    tier = hint_models.get("pi") or hint_models.get("claude-code")
    if not tier:
        return ""
    # If a pi hint is already a concrete pattern (contains "/" or "-"), use as-is.
    if "/" in tier:
        return tier
    return model_map.get(tier, tier if tier not in ("opus", "sonnet", "haiku") else "")


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else root / ".pi" / "agents"
    out.mkdir(parents=True, exist_ok=True)
    model_map = load_model_map(root)

    sources = sorted((root / "agents" / "core").glob("*.md")) + \
        sorted((root / "agents" / "project").glob("*.md"))

    written, skipped, by_name = 0, 0, {}
    keep = set()
    for src in sources:
        fm_lines, body = split_frontmatter(src.read_text(encoding="utf-8"))
        if fm_lines is None:
            skipped += 1
            continue
        name, desc, caps, hints = parse_fields(fm_lines)
        if not name or not desc:
            skipped += 1
            continue
        if name in by_name:
            print(f"  WARN  duplicate agent name '{name}' ({src} vs "
                  f"{by_name[name]}); keeping first, skipping second.", file=sys.stderr)
            skipped += 1
            continue
        by_name[name] = src

        tools = caps_to_tools(caps)
        model = resolve_model(hints, model_map)
        head = ["---", f"name: {name}", f"description: {yaml_quote(desc)}"]
        if tools:
            head.append("tools: " + ", ".join(tools))
        if model:
            head.append(f"model: {model}")
        head.append("---")
        gen = ("<!-- GENERATED by agents/scripts/core/gen-pi-agents.py from "
               f"agents/{src.parent.name}/{src.name}. Do NOT edit here; edit the "
               "canonical file and re-run setup-harness.sh pi. -->\n\n")
        dest = out / src.name
        dest.write_text("\n".join(head) + "\n" + gen + body + "\n", encoding="utf-8")
        keep.add(src.name)
        written += 1

    # Prune stale generated files (agent renamed/removed upstream).
    for f in out.glob("*.md"):
        if f.name not in keep:
            f.unlink()
            print(f"  prune   .pi/agents/{f.name} (no canonical source)")

    print(f"  gen-pi-agents  {written} agent(s) -> {out} ({skipped} skipped)")
    n_model = sum(1 for s in sources if s.name in keep)  # noqa: F841
    if not any(model_map.values()):
        print("  note  no tier->model map set (PI_MODEL_{OPUS,SONNET,HAIKU} or "
              "docs/harness/pi/model-map.json) — subagents use pi's default model.")


if __name__ == "__main__":
    main()
