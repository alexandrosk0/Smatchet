#!/usr/bin/env python3
"""Unit tests for agents/_shared/token-tracking/agent-token-log.py:_infer_outcome.

Covers AGENTS.md § Agent output contract — the canonical telemetry classifier.
Run via `scripts/dev/test-agent-contract.sh` (auto-enrolled by test-all.sh).
"""

import importlib.util
import sys
from pathlib import Path


def _load_module():
    here = Path(__file__).resolve().parent
    canonical = here.parent / "agent-token-log.py"
    spec = importlib.util.spec_from_file_location("agent_token_log", canonical)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main() -> int:
    mod = _load_module()
    infer = mod._infer_outcome  # noqa: SLF001 — intentional internal access for test.

    failures: list[str] = []

    def check(name: str, text: str, want_outcome: str, want_reason_substring: str | None = None) -> None:
        got_outcome, got_reason = infer(text)
        if got_outcome != want_outcome:
            failures.append(
                f"  [{name}] outcome: want={want_outcome!r}, got={got_outcome!r}"
            )
            return
        if want_reason_substring is not None:
            if got_reason is None or want_reason_substring not in got_reason:
                failures.append(
                    f"  [{name}] reason: want substring {want_reason_substring!r}, got={got_reason!r}"
                )
                return
        print(f"  PASS {name}")

    # Case 1 — explicit Outcome: applied (canonical happy path).
    check(
        "explicit-applied",
        "Body text.\n\n## Outcome: applied\n\n## Self-improvement\n",
        "applied",
    )

    # Case 2 — explicit Outcome: failed (any explicit tag returns verbatim).
    check(
        "explicit-failed",
        "Body text.\n\n## Outcome: failed\n",
        "failed",
    )

    # Case 3 — explicit Outcome: halted with halt_reason.
    check(
        "explicit-halted-with-reason",
        (
            "Body text.\n\n"
            "halt_reason: cli-gap — MCP socket unreachable after 15 s\n"
            "## Outcome: halted\n"
        ),
        "halted",
        want_reason_substring="cli-gap",
    )

    # Case 4 — halt keyword without explicit Outcome → halted.
    check(
        "halt-keyword-no-tag",
        "I cannot proceed because the worktree is in an unexpected state.\n",
        "halted",
        want_reason_substring="cannot proceed",
    )

    # Case 5 — NEW behaviour. Self-improvement present, no Outcome, no halt keyword → partial.
    # Old behaviour returned `applied`; that silently green-washed halted runs and was removed.
    check(
        "self-improvement-no-outcome-no-halt-NEW-partial",
        "Body text.\n\n## Self-improvement\n- nothing surfaced this round.\n",
        "partial",
    )

    # Case 6 — empty input → partial (NEW default).
    check(
        "empty-input-default-partial",
        "",
        "partial",
    )

    # Case 7 — explicit Outcome: partial (verbatim passthrough).
    check(
        "explicit-partial",
        "## Outcome: partial\n",
        "partial",
    )

    # Case 8 — explicit Outcome: aborted (verbatim passthrough).
    check(
        "explicit-aborted",
        "## Outcome: aborted\n",
        "aborted",
    )

    # Case 9 — case-insensitive Outcome tag (re.IGNORECASE branch).
    check(
        "outcome-case-insensitive",
        "## OUTCOME: Applied\n",
        "applied",
    )

    print()
    if failures:
        print(f"FAIL ({len(failures)} failure{'s' if len(failures) != 1 else ''}):")
        for line in failures:
            print(line)
        return 1
    print("PASS (9 cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
