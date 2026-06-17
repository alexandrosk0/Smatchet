#!/usr/bin/env python3
"""redaction-escape-oracle.py — adversarial second opinion for redact-intent.py.

The prompt-intent redactor (agents/scripts/core/redact-intent.py) is the
SECURITY boundary that strips secrets out of captured prompts before they are
written to disk / shipped off-host. Its own `--selftest` proves the formats it
already knows about. This oracle is the *independent* check: it feeds a broad
corpus of realistic secrets through `redact()` and asserts that NONE of the
planted secret values survive.

Two layers, in fail-safe order (over-redaction is fine, under-redaction is a
hard fail — mirrors redact-intent.py's contract):

  PRIMARY (deterministic, always runs)
      Every secret in CORPUS is embedded in a realistic line, redacted, and the
      high-entropy core MUST be absent from the output. A survivor is a leak.
      Doubles as a regression guard: if someone later weakens a redaction rule,
      the matching corpus row goes red.

  SECONDARY (defense-in-depth, skipped cleanly when `gitleaks` is absent)
      All redactor outputs are concatenated and scanned with `gitleaks`. Because
      gitleaks ships its own ruleset, it can flag a survivor in a format the
      planted corpus never anticipated. gitleaks is not yet on the dev image, so
      its absence is a SKIP, never a failure.

Exit codes:
  0  all checks pass (gitleaks layer may have been skipped)
  1  a planted secret survived redaction (PRIMARY or SECONDARY)
  2  the redactor module could not be loaded (environment problem, not a leak)

`--selftest` additionally exercises the DETECTION path: it runs the corpus
through an identity (no-op) "redactor" and asserts the oracle CATCHES every
planted secret. That proves the oracle can actually see a survivor rather than
rubber-stamping a redactor that silently leaks.
"""

import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
_REDACTOR_PATH = os.path.join(_HERE, "redact-intent.py")


def _load_redact():
    """Import redact() from the hyphenated redact-intent.py by file path.

    Fail-closed semantics, distinguishing two failure modes the caller must
    treat differently:

      * The redactor file is ABSENT (or exposes no callable redact) -> return
        None. The caller maps that to exit 2 (environment problem, not a leak):
        there is no security boundary present to exercise.
      * The redactor file is PRESENT but raises on import (syntax error, broken
        dependency, half-applied edit) -> raise RuntimeError. The caller maps
        that to exit 1 (hard fail). A redactor that cannot even load is a BROKEN
        security boundary; silently downgrading that to a soft skip would let a
        CI gate wave through prompts the redactor never actually scrubbed.
    """
    if not os.path.exists(_REDACTOR_PATH):
        return None
    try:
        spec = importlib.util.spec_from_file_location("redact_intent", _REDACTOR_PATH)
        if spec is None or spec.loader is None:
            return None
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    except FileNotFoundError:
        # Raced away between the existence check and import — treat as absent.
        return None
    except Exception as exc:
        # Present but unloadable: broken boundary, not a clean skip.
        raise RuntimeError("redact-intent.py present but failed to import: %r" % (exc,)) from exc
    fn = getattr(mod, "redact", None)
    return fn if callable(fn) else None


# (label, context-line template with {s}, secret as a list of FRAGMENTS).
#
# Each secret is stored split across fragments that are joined at runtime. The
# contiguous secret token therefore never appears as a literal in this file, so
# commit-time secret scanners (GitHub push protection, gitleaks pre-commit) do
# not block this corpus of synthetic test data — while the joined runtime value
# is still the realistic vendor-shaped token the redactor must strip, preserving
# per-rule regression coverage. Splits land mid-token so no single fragment
# matches a vendor detector. Non-vendor rows (password, userinfo) are single
# fragments — nothing flags them.
#
# Each secret is embedded in a plausible line so the redactor's context-aware
# rules (KV stems, Bearer, connection-string userinfo) fire the way they would
# on a real captured prompt. The absence assertion targets the joined secret, so
# a failure points straight at the vendor class that leaked.
CORPUS = [
    ("github-pat",
     "git remote set-url origin https://x:{s}@github.com/o/r.git",
     ["ghp_ABCDEFGHIJKLMNOPQR", "STUVWXYZ0123456789"]),
    ("github-fine-grained",
     "export GH_TOKEN={s}",
     ["github_pat_11ABCDEFG0aBcDeFgHiJ_", "kLmNoPqRsTuVwXyZ0123456789AbCdEfGhIjKlMnOpQ"]),
    ("jwt",
     "the Authorization token was {s} earlier",
     ["eyJhbGciOiJIUzI1NiJ9.",
      "eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4ifQ.",
      "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"]),
    ("aws-access-key-id",
     "aws_access_key_id = {s}",
     ["AKIAIOSFODN", "N7EXAMPLE"]),
    ("aws-secret-access-key",
     "aws_secret_access_key = {s}",
     ["wJalrXUtnFEMI/K7MDENG/", "bPxRfiCYEXAMPLEKEY"]),
    ("slack-bot-token",
     "slack incoming webhook uses {s} today",
     ["xoxb-1234567890-098", "7654321-ABCDEFGHIJKLMNOPQRSTUVWX"]),
    ("google-api-key",
     "the maps api key is {s}",
     ["AIzaSyA1234567890", "abcdefGHIJKLMNOPQRSTuvX"]),
    ("stripe-live-secret",
     "stripe secret key {s} configured",
     ["sk_live_abcdEFGH1234", "ijklMNOP5678qrst"]),
    ("openai-key",
     "OPENAI_API_KEY={s}",
     ["sk-abcdEFGHijklMNOP", "qrstUVWXyz0123456789ABCDdefg"]),
    ("openai-proj-key",
     "OPENAI_API_KEY={s}",
     ["sk-proj-abcdEFGHijklMNOPqrstUVWX", "0123456789abcdEFGHijklMNOPqrst"]),
    ("npm-token",
     "//registry.npmjs.org/:_authToken={s}",
     ["npm_ABCDEFGHIJKLMNOP", "QRSTUVWXYZ0123456789"]),
    ("password-kv",
     "the database password: {s} please keep it",
     ["hunter2-S3cretP@ssw0rd-longish"]),
    ("connection-string-userinfo",
     "DATABASE_URL=postgres://admin:{s}@db.internal:5432/app",
     ["Sup3rSecretDbPass"]),
    # The secret in a PEM key is the base64 key MATERIAL, not the public
    # BEGIN/END label. Embedding the markers lets _PRIVATE_KEY redact the whole
    # block; the single-line body also trips the _B64 sweep as a fallback. The
    # absence assertion targets the body, so whitespace-collapse can't mask a leak.
    ("pem-private-key",
     "deploy key -----BEGIN OPENSSH PRIVATE KEY----- {s} "
     "-----END OPENSSH PRIVATE KEY-----",
     ["b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAE", "bm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZWQ"]),
    ("bearer-token",
     "curl -H 'Authorization: Bearer {s}' https://api.example.com",
     ["abcdef0123456789", "ghijklmnopqrstuvwxyzABCD"]),
    ("hex-40",
     "the commit-signing key is {s} ok",
     ["deadbeefdeadbeef", "deadbeefdeadbeefdeadbeef"]),
    ("base64-blob",
     "payload {s} done",
     ["QUJDREVGR0hJSktMTU5PUFFS", "U1RVVldYWVowMTIzNDU2Nzg5YWJjZA=="]),
    ("generic-api-key-header",
     "x-api-key: {s}",
     ["live_sk_9aZ8bY7cX6dW", "5eV4fU3gT2hS1iR0jQpO"]),
]


def _cases():
    """Yield (label, redaction-input-line, joined-secret) for each corpus row.

    The secret is joined from its fragments here — the single point where the
    contiguous token comes into existence, at runtime only.
    """
    for label, template, frags in CORPUS:
        secret = "".join(frags)
        yield label, template.format(s=secret), secret


def _scan_corpus(redact_fn):
    """Return [(label, secret, output), ...] for every secret that SURVIVED."""
    escapes = []
    for label, line, secret in _cases():
        out = redact_fn(line)
        if secret in out:
            escapes.append((label, secret, out))
    return escapes


def _gitleaks_scan(redacted_lines):
    """Run gitleaks over the concatenated redactor output.

    Returns (status, detail):
      status == "skip"  gitleaks not installed, or it errored (tool problem) —
                        never fails the security gate on an absent/broken tool.
      status == "pass"  gitleaks ran and found nothing.
      status == "fail"  gitleaks flagged a survivor in the redacted output.
    """
    exe = shutil.which("gitleaks")
    if not exe:
        return ("skip", "gitleaks not on PATH")

    tmpdir = tempfile.mkdtemp(prefix="redaction-oracle-")
    src = os.path.join(tmpdir, "redacted.txt")
    report = os.path.join(tmpdir, "gitleaks.json")
    try:
        with open(src, "w") as fh:
            fh.write("\n".join(redacted_lines) + "\n")
        # `detect --no-git` is supported across gitleaks v8.x; a non-zero exit
        # with a written report means findings, a non-zero exit with no report
        # means the tool itself errored (treated as skip, not a leak).
        # Bound the scan: a wedged/hung gitleaks must not stall the gate. A
        # timeout surfaces as subprocess.TimeoutExpired, caught below and mapped
        # to SKIP (tool problem, not a redaction leak) — never a silent hang.
        proc = subprocess.run(
            [exe, "detect", "--no-git", "--source", tmpdir,
             "--report-format", "json", "--report-path", report],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=120,
        )
        if proc.returncode == 0:
            return ("pass", "no findings")
        if os.path.isfile(report) and os.path.getsize(report) > 2:
            return ("fail", "gitleaks reported findings in redacted output")
        return ("skip", "gitleaks exited %d without a report" % proc.returncode)
    except Exception as exc:  # tool invocation problem, not a redaction leak
        return ("skip", "gitleaks invocation error: %r" % (exc,))
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def _run_live(redact_fn):
    """PRIMARY + SECONDARY against the real redactor. Return exit code 0/1."""
    rc = 0

    escapes = _scan_corpus(redact_fn)
    if escapes:
        rc = 1
        for label, secret, out in escapes:
            sys.stderr.write(
                "ESCAPE [%s]: secret survived redaction -> %r\n" % (label, out)
            )
    else:
        sys.stderr.write(
            "redaction-escape-oracle: PRIMARY pass — %d/%d secrets redacted\n"
            % (len(CORPUS), len(CORPUS))
        )

    redacted = [redact_fn(line) for _, line, _ in _cases()]
    status, detail = _gitleaks_scan(redacted)
    if status == "fail":
        rc = 1
        sys.stderr.write("redaction-escape-oracle: SECONDARY FAIL — %s\n" % detail)
    elif status == "skip":
        sys.stderr.write("redaction-escape-oracle: SECONDARY skip — %s\n" % detail)
    else:
        sys.stderr.write("redaction-escape-oracle: SECONDARY pass — %s\n" % detail)

    return rc


def _selftest():
    """Prove the oracle can SEE a survivor, then run the live check.

    A no-op "redactor" must let every planted secret through; the oracle MUST
    report exactly len(CORPUS) escapes. If the corpus check were inverted or the
    absence test broken (a pass-only oracle — the class test-gate-selftests.sh
    guards), this assert fires. Then the real redactor must produce zero escapes.
    """
    try:
        redact_fn = _load_redact()
    except RuntimeError as exc:
        sys.stderr.write(
            "redaction-escape-oracle selftest: redactor present but unloadable — %s\n" % exc
        )
        return 1
    if redact_fn is None:
        sys.stderr.write("redaction-escape-oracle selftest: cannot load redact()\n")
        return 2

    ok = True

    # selftest: asserts-failure — the detection path. A no-op redactor leaks
    # every secret; the oracle must catch all of them, else it is rubber-stamping.
    leaked = _scan_corpus(lambda text: text)
    if len(leaked) != len(CORPUS):
        ok = False
        sys.stderr.write(
            "selftest meta-check FAIL: identity redactor leaked %d/%d but the "
            "oracle only flagged the survivors it could see\n"
            % (len(leaked), len(CORPUS))
        )

    # The real redactor must redact the whole corpus.
    escapes = _scan_corpus(redact_fn)
    if escapes:
        ok = False
        for label, secret, out in escapes:
            sys.stderr.write(
                "selftest FAIL [%s]: secret survived redact() -> %r\n" % (label, out)
            )

    if ok:
        sys.stderr.write(
            "redaction-escape-oracle selftest: detection proven + all %d corpus "
            "secrets redacted\n" % len(CORPUS)
        )
    return 0 if ok else 1


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        return _selftest()
    try:
        redact_fn = _load_redact()
    except RuntimeError as exc:
        sys.stderr.write(
            "redaction-escape-oracle: redactor present but unloadable — %s\n" % exc
        )
        return 1
    if redact_fn is None:
        sys.stderr.write(
            "redaction-escape-oracle: cannot load redact() from %s\n" % _REDACTOR_PATH
        )
        return 2
    return _run_live(redact_fn)


if __name__ == "__main__":
    sys.exit(main())
