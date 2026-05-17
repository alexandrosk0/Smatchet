# Agent self-improvement — security

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-05-17 · code-review · [security] · P1 — `Source_Core/src/OpenAiClient.cpp:140-180` API key leak risk: provider error body forwarded to UI
  Details: `r.text.substr(0,600)` appended to `err.Message` and surfaced via `onError`. Provider 401/403 bodies routinely echo `Authorization` headers / request body / org identifiers; the redacted call chain ends at user-visible error UI.
  Concrete next action: sanitise before append; add `RedactProviderErrorBody` next to existing `RedactUrlForLog`. Surfaced by retrospective code-review sweep on PR #140.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [security] · P1 — `scripts/dev/coverage.sh:155` `python -c "...open(r'$XML_OUT').read()..."` interpolates user-controlled path into Python source
  Details: `SMATCHET_COVERAGE_OUTPUT_DIR` is user-controlled. A path containing `'` or `\` breaks the script or runs attacker-controlled Python under `set -euo pipefail`.
  Concrete next action: pass the path via `sys.argv` or `os.environ`, never via string-interpolation into the `-c` source. Surfaced by retrospective code-review sweep on PR #148.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [security] · P2 — `tests/support/GoldenImage.h:67` `std::strtol` parses PPM `w`, `h`, `maxv` without overflow / negative checks
  Details: A crafted PPM with `w=99999999 h=99999999` triggers overflow on 32-bit `size_t`; on 64-bit a multi-GB resize. Not user-reachable today (test-only) but the parser is small and shared with the screenshot-diff harness.
  Concrete next action: cap dimensions (`w > 16384 || h > 16384`) + reject negative / zero. Surfaced by retrospective code-review sweep on PR #146.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-rig → p4-blame · [security] · P1 — `CallstackParser::ParseCallstackText` regex is super-linear in line length
  Details: While shipping the carry-over A adversarial subcases, the orchestrator-spec "≥64 KiB single line within 50 ms" ceiling proved unachievable on MinGW UCRT. Standalone-probe timings under -O2 against the MSVC-format regex: 256 B → 1 ms, 512 B → 4 ms, 1 KiB → 21 ms, 2 KiB → 101 ms, 4 KiB → 403 ms. Above ~32 KiB the test runner stack-overflows (0xC00000FD) because `std::regex_search` recurses on input length under libstdc++. Smatchet only calls this on the user's pasted callstack text, so a malicious paste is the attack surface — but the size cap is mostly the user's display window. The 1 KiB / 100 ms test gate in `tests/Source_Core/CallstackParser.test.cpp` locks the current regression line; bumping it to the 64 KiB / 50 ms spec is the verification gate on this fix.
  Concrete next action: (a) clamp per-line input length to ~16 KiB before regex; (b) anchor the alternation with explicit length-bounded quantifiers; (c) replace `std::regex` with `re2` for this hot path. Option (a) is a one-liner with a backlog comment. Estimated cost 1-2 h.
  Status: open
  Last-reviewed: 2026-05-17
