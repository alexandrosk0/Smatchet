#!/usr/bin/env python3
"""Redact a captured user prompt down to one safe, single-line summary.

Reads raw prompt text on stdin, writes ONE redacted line to stdout. Used by the
`capture-intent.sh` UserPromptSubmit hook to persist a prompt's intent into a
gitignored capture file without ever leaking a secret or a home-dir username
into the (eventually public) PR `## Intent` section.

Design contract — this is a SECURITY boundary, so it is deliberately fail-safe:
  * Over-redaction is acceptable; under-redaction is not. When a pattern is
    ambiguous we strip more, not less. (A clipped Intent line is cosmetic; a
    leaked credential is not.)
  * High-confidence secret formats are stripped by name (JWT, gh_*/github_pat,
    AWS, Slack, Google, Bearer, key=value secret pairs, PRIVATE KEY blocks),
    then two catch-alls (long hex / long base64 blobs) sweep the residue.
  * Home-dir paths that embed a username are collapsed to a `[user]` placeholder
    (C:\\Users\\<name>, /home/<name>, /Users/<name>).
  * Emails are intentionally NOT redacted (explicit scope decision).

Known accepted false-positive: a 32+ char hex string that is actually a git SHA
(not a secret) is redacted to [REDACTED-HEX]. Safe direction — see plan
docs/plans/active/pr-intent-capture.md § Risks.

Exit status is always 0 on a successful run (empty input -> empty output). The
caller treats any non-zero exit / crash as "write nothing" (never the raw
prompt). `--selftest` runs a built-in assertion suite (exit 1 on failure).
"""
import re
import sys

MAX_LEN = 500  # one-liner cap; longer prompts are truncated with an ellipsis marker

# --- High-confidence secret formats, stripped by name (order: specific first) ---

# PEM private-key blocks (multi-line) — match across newlines before we collapse.
_PRIVATE_KEY = re.compile(
    r"-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----.*?-----END [A-Z0-9 ]*PRIVATE KEY-----",
    re.DOTALL,
)
# JSON Web Token: three base64url segments.
_JWT = re.compile(r"\beyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+")
# GitHub tokens (ghp_/gho_/ghu_/ghs_/ghr_ + fine-grained github_pat_).
_GH_TOKEN = re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,})")
# Slack tokens.
_SLACK = re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{10,}")
# AWS access-key ids (long-term AKIA / temporary ASIA).
_AWS_KEY = re.compile(r"\b(?:AKIA|ASIA)[0-9A-Z]{16}\b")
# Google API keys.
_GOOGLE = re.compile(r"\bAIza[0-9A-Za-z_-]{35}\b")
# Bearer auth headers / mentions.
_BEARER = re.compile(r"(?i)\bbearer\s+[A-Za-z0-9._\-]{8,}")
# key=value / key: value secret pairs. Value may be quoted (spaces allowed) or a
# bare non-space run. Key match is case-insensitive; key + separator are kept.
_KV = re.compile(
    r"(?i)\b((?:password|passwd|pwd|secret(?:[_-]?key)?|token|access[_-]?token"
    r"|auth[_-]?token|refresh[_-]?token|session[_-]?token|api[_-]?key|apikey"
    r"|access[_-]?key|client[_-]?secret|private[_-]?key|bearer)\b\s*[:=]\s*)"
    r"(?:\"[^\"]*\"|'[^']*'|[^\s]+)"
)
# Catch-all residue: long hex / long base64 blobs (the user-enumerated sweep).
_HEX = re.compile(r"\b[0-9a-fA-F]{32,}\b")
_B64 = re.compile(r"\b[A-Za-z0-9+/]{40,}={0,2}")

# --- Home-dir username collapse (run after secret stripping) ---
_HOME_WIN = re.compile(r"(?i)([A-Za-z]:[\\/]Users[\\/])[^\\/\s\"']+")
_HOME_NIX = re.compile(r"(/(?:home|Users)/)[^/\s\"']+")


def redact(text):
    """Return a single redacted line for `text` (may be empty)."""
    if not text:
        return ""
    text = _PRIVATE_KEY.sub("[REDACTED-PRIVATE-KEY]", text)
    text = _JWT.sub("[REDACTED-JWT]", text)
    text = _GH_TOKEN.sub("[REDACTED-TOKEN]", text)
    text = _SLACK.sub("[REDACTED-TOKEN]", text)
    text = _AWS_KEY.sub("[REDACTED-TOKEN]", text)
    text = _GOOGLE.sub("[REDACTED-TOKEN]", text)
    text = _BEARER.sub("Bearer [REDACTED]", text)
    text = _KV.sub(r"\1[REDACTED]", text)
    text = _HEX.sub("[REDACTED-HEX]", text)
    text = _B64.sub("[REDACTED-B64]", text)
    # Home-dir usernames -> placeholder (preserve the drive/prefix + trailing path).
    text = _HOME_WIN.sub(lambda m: m.group(1) + "[user]", text)
    text = _HOME_NIX.sub(lambda m: m.group(1) + "[user]", text)
    # Collapse to a single line.
    text = re.sub(r"\s+", " ", text).strip()
    if len(text) > MAX_LEN:
        text = text[:MAX_LEN].rstrip() + " …[truncated]"
    return text


def _selftest():
    # (input, [forbidden substrings], [required substrings])
    cases = [
        ("token=ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", ["ghp_ABCDEF"], ["[REDACTED]"]),
        ("here is eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.dBjftJeZ4CVP", ["eyJhbGci"], ["[REDACTED-JWT]"]),
        ("Authorization: Bearer abcdef0123456789xyz", ["abcdef0123456789"], ["Bearer [REDACTED]"]),
        ("password: hunter2supersecret", ["hunter2supersecret"], ["[REDACTED]"]),
        ("AWS AKIAIOSFODNN7EXAMPLE here", ["AKIAIOSFODNN7EXAMPLE"], ["[REDACTED-TOKEN]"]),
        ("slack xoxb-1234567890-abcdefghijkl", ["xoxb-1234567890"], ["[REDACTED-TOKEN]"]),
        ("deadbeefdeadbeefdeadbeefdeadbeefdeadbeef", ["deadbeefdeadbeef"], ["[REDACTED-HEX]"]),
        ("path C:\\Users\\alexk\\.aws\\creds", ["alexk"], ["[user]"]),
        ("path /home/alexk/.ssh/id", ["/home/alexk"], ["[user]"]),
        ("contact alexkonstantonis@gmail.com please", ["[REDACTED"], ["alexkonstantonis@gmail.com"]),
        ("line one\nline two\ttabbed", ["\n", "\t"], ["line one line two tabbed"]),
    ]
    ok = True
    for i, (inp, forbidden, required) in enumerate(cases):
        out = redact(inp)
        bad = [s for s in forbidden if s in out] + [s for s in required if s not in out]
        if bad or "\n" in out:
            ok = False
            sys.stderr.write("selftest case %d FAIL: %r -> %r\n" % (i, inp, out))
    if ok:
        sys.stderr.write("redact-intent selftest: all %d cases pass\n" % len(cases))
    return 0 if ok else 1


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        return _selftest()
    try:
        raw = sys.stdin.read()
    except Exception:
        return 0  # fail-safe: no output rather than risk emitting raw text
    out = redact(raw)
    if out:
        sys.stdout.write(out + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
