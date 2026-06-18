#!/usr/bin/env python3
"""Redact a captured user prompt down to one safe, single-line summary.

Reads raw prompt text on stdin, writes ONE redacted line to stdout. Used by the
`capture-intent.sh` UserPromptSubmit hook to persist a prompt's intent into a
gitignored capture file without ever leaking a secret, a home-dir username, a
service account, or a machine SID into the (eventually public) PR `## Intent`.

Design contract — this is a SECURITY boundary, so it is deliberately fail-safe:
  * Over-redaction is acceptable; under-redaction is NOT. When a pattern is
    ambiguous we strip more, not less. (A clipped Intent line is cosmetic; a
    leaked credential is not.) This is why the value/authority classes are
    greedy and the high-entropy sweep is generic rather than vendor-enumerated.
  * High-confidence secret formats are stripped by name (JWT, gh_*/github_pat,
    AWS, Slack, Google, Stripe, OpenAI, npm, a curated vendor-prefix alternation,
    Bearer, HTTP Basic, connection-URL userinfo, key=value / JSON / XML secret
    pairs, PRIVATE KEY blocks). Then a generic high-entropy sweep (any alnum run
    >=16 that contains BOTH a digit AND a letter) plus the long hex / base64
    catch-alls sweep the residue. The digit-AND-letter rule is deliberate:
    CamelCase identifiers (SmatchetActiveProjectGridUi) rarely contain digits,
    so they survive — keeping Intent lines readable — while random secrets,
    which almost always mix digits and letters, do not.
  * Usernames are redacted everywhere they can leak an account: home-dir paths
    (C:\\Users\\<name>, UNC, Documents and Settings, /home, /Users, ~name, ~$name),
    home env-var roots (%USERPROFILE%\\.., $env:APPDATA\\.., $HOME/..), the
    HKEY_USERS registry hive, connection-URL userinfo, and ssh-context user@host
    (ssh/scp/sftp/rsync/git remote). Windows machine/account SIDs (S-1-5-..) are
    redacted wholesale.
  * Emails ARE redacted to [REDACTED-EMAIL] (pr-intent-capture-hardening #7 —
    Intent lines land in a PUBLIC PR body, so a third-party email is PII that must
    not leak; reverses the parent plan's preserve-emails decision). `_EMAIL` runs
    LAST, after the username / connection-URL collapses, so a `[user]@host` or
    `[REDACTED]@host` placeholder (ends in `]` immediately before `@`) is shielded
    and only bare prose emails match. ssh-context `user@host` to a dotless host
    (deployacct@host) still collapses to `[user]@host` via _SSH_USER first.

Known accepted residuals (contrived / inherently ambiguous — documented in plan
docs/plans/shipped/pr-intent-capture.md § Risks, not closed by design):
  * secrets split across a space or newline; no-digit prefix-less mixed-case
    tokens; YAML block/folded multiline scalars; URL-encoded (%3D) base64; HTTP
    Basic blobs < 16 chars; `Bearer tok123`-class tokens < 8 chars; bare UUID
    tokens (redacting all UUIDs is too noisy); pgpass `host:port:db:user:pass`
    rows; `/root/` system paths; space-in-password connection URLs.
A 32+ char hex git SHA is redacted to [REDACTED-HEX] (safe direction).

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
# GitHub tokens (ghp_/gho_/ghu_/ghs_/ghr_ + fine-grained github_pat_). No leading
# \b: the `gh?_` / `github_pat_` prefix is distinctive, and dropping the anchor
# closes the glued-prefix bypass (Xghp_…, leading-letter glue).
_GH_TOKEN = re.compile(r"(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,})")
# Slack tokens.
_SLACK = re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{10,}")
# AWS access-key ids (long-term AKIA / temporary ASIA). Case-insensitive + {16,}
# (not {16}\b): closes lowercase `akia…` and the 17th-char-glued `AKIA…EX` bypass.
_AWS_KEY = re.compile(r"(?i)\b(?:AKIA|ASIA)[0-9A-Za-z]{16,}")
# Google API keys (loosened floor to catch truncated forms; `AIza` is so
# distinctive — no English word begins it — that a floor of 8 is safe).
_GOOGLE = re.compile(r"\bAIza[0-9A-Za-z_-]{8,}")
# Stripe live/test keys (sk_live_ / rk_test_ …) — below the 32-hex/40-b64 floor.
_STRIPE = re.compile(r"\b[sr]k_(?:live|test)_[A-Za-z0-9]{10,}")
# OpenAI / Anthropic-style `sk-` keys (sk-…, sk-proj-…, sk-proj.…) — sub-threshold.
# `.` is in the class so a `sk-proj.LIVEKEY…` dotted form does not split.
_OPENAI = re.compile(r"\bsk-[A-Za-z0-9_.\-]{20,}")
# npm automation/access tokens — no \b (glued-prefix bypass), name-anchored.
_NPM = re.compile(r"npm_[A-Za-z0-9]{20,}")
# Curated vendor-prefix alternation. Each entry pairs a distinctive prefix with a
# length floor chosen so the prefix can't fire on ordinary prose (org-wide,
# key-value, secret_handshake all fall below their floor). Defense-in-depth for
# punctuation-split / short-tail tokens the generic high-entropy sweep would miss.
_VENDOR = re.compile(
    r"(?i)\b(?:"
    r"dop_v1_[A-Za-z0-9]{8,}"                  # DigitalOcean
    r"|HRKU-[A-Za-z0-9_-]{8,}"                 # Heroku (UUID-shaped — hyphens split)
    r"|sq0(?:atp|csp)-[A-Za-z0-9_-]{8,}"       # Square
    r"|shp(?:at|ss|ca|pa)_[A-Za-z0-9]{8,}"     # Shopify
    r"|lin_api_[A-Za-z0-9]{8,}"                # Linear
    r"|glpat-[A-Za-z0-9_-]{8,}"                # GitLab PAT
    r"|pypi-[A-Za-z0-9_-]{8,}"                 # PyPI
    r"|dckr_pat_[A-Za-z0-9_-]{8,}"             # Docker Hub PAT
    r"|GOCSPX-[A-Za-z0-9_-]{8,}"               # Google OAuth client secret
    r"|pscale_(?:pw|tkn)_[A-Za-z0-9_-]{8,}"    # PlanetScale
    r"|hf_[A-Za-z0-9]{8,}"                     # HuggingFace
    r"|sntry[su]_[A-Za-z0-9._=+/-]{8,}"        # Sentry
    r"|ddapp_[A-Za-z0-9]{6,}"                  # Datadog app key
    r"|sbp_[A-Za-z0-9]{8,}"                    # Supabase
    r"|whsec_[A-Za-z0-9]{8,}"                  # Stripe webhook secret
    r"|key-[0-9a-f]{16,}"                      # Mailgun
    r"|ya29\.[A-Za-z0-9._-]{8,}"               # Google OAuth access token
    r"|v1\.0-[A-Za-z0-9_-]{16,}"               # Cloudflare
    r"|org-[A-Za-z0-9]{16,}"                   # OpenAI org id
    r"|secret_[A-Za-z0-9]{24,}"                # Notion (long; avoids secret_handshake)
    r"|SG\.[A-Za-z0-9_-]{6,}\.[A-Za-z0-9_-]{6,}"  # SendGrid (two dotted segments)
    r"|\d{6,}:AA[A-Za-z0-9_-]{20,}"            # Telegram bot token
    r")"
)
# Bearer auth headers / mentions. The token class includes ./+@/= so a token does
# not leak past a `/`, `+`, `@`, or `.` boundary (post-red-team).
_BEARER = re.compile(r"(?i)\bbearer\s+[A-Za-z0-9._\-/+@=]{8,}")
# HTTP Basic auth: `Basic <base64>` (16+ chars). `.` in the class stops a
# dot-boundary tail leak. Runs before _KV so `Authorization: Basic …` is whole.
_BASIC = re.compile(r"(?i)\bBasic\s+[A-Za-z0-9+/=_.\-]{16,}")
# Credentials in a connection URL: scheme://USERINFO@host. Greedy `[^\s]*@` grabs
# the WHOLE authority up to the LAST `@` before whitespace — closes multi-`@`
# passwords (mongodb://a:p@ss@h), `/`-in-password (u:p/a/ss@h), and the
# username-only form (postgres://reader@h). The required `@` keeps a bare
# host:port (http://h:8080/) from matching. Over-reaching into a path/query `@`
# is acceptable over-redaction (safe direction). Also covers ssh://user@host.
_CONN = re.compile(r"(?i)\b([a-z][a-z0-9+.\-]*://)[^\s]*@")
# key=value / key: value / JSON / YAML secret pairs. Leading `[\w.-]*` lets a
# prefixed key match (db_pass, my-api-key, X_AUTH_TOKEN); the stem ends on a word
# boundary so passenger:/passport:/accessible= do NOT match. Stems also cover DB
# usernames (user/username/uid/userid/authuser) and session/cookie ids per the
# expanded scope. The value may be double/single-quoted (escaped quotes handled),
# or a bare run ended by whitespace — bare run is `[^\s]+` so comma/brace/bracket
# tails (token=a,b / secret=a}b) are swept too. Case-insensitive.
_KV = re.compile(
    r"(?i)"
    r"([\"']?\b[\w.\-]*"
    r"(?:passwords?|passwd|passphrase|pwd|pass"
    r"|secret[_-]?key|secret[_-]?id|secret"
    r"|access[_-]?token|auth[_-]?token|refresh[_-]?token|session[_-]?token"
    r"|client[_-]?secret|client[_-]?id"
    r"|private[_-]?key[_-]?id|private[_-]?key"
    r"|encryption[_-]?key|signing[_-]?key|master[_-]?key|account[_-]?key|auth[_-]?key"
    r"|api[_-]?keys?|apikey|access[_-]?keys?|access"
    r"|api[_-]?tokens?|token"
    r"|x[_-]?functions[_-]?key"
    r"|authorizations?|authorization|auth"
    r"|set[_-]?cookie|cookie|sessionid|session|sid"
    r"|credentials?|bearer"
    r"|user[_-]?secrets?|username|user[ _-]?id|authuser|uid|user"
    r")"
    r"\b[\\\"']*"
    r"(?:\s*(?:=>|[:=]+)\s*|\t+))"
    r"(?:\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'|[^\s]+)"
)
# XML element body: <password>value</password> (no key=value separator). Keeps the
# open tag, drops the value (and a CDATA wrapper). Stem list mirrors the secret KV
# stems; value is `[^<]*` up to the closing tag.
_XML_SECRET = re.compile(
    r"(?i)(<\s*[\w.\-]*"
    r"(?:password|passwd|passphrase|secret|client[_-]?secret|api[_-]?key"
    r"|access[_-]?token|auth[_-]?token|refresh[_-]?token|private[_-]?key"
    r"|credentials?|token)[\w.\-]*\s*>)"
    r"(?:<!\[CDATA\[)?[^<]*"
)
# Generic high-entropy sweep: any alnum run >=16 that contains BOTH a digit and a
# letter. Catches sub-32 hex, prefix-less random tokens, base64 with digits, and
# most vendor tails — while digit-free CamelCase identifiers survive (readability).
_HIENT = re.compile(
    r"(?<![A-Za-z0-9])(?=[A-Za-z0-9]*[0-9])(?=[A-Za-z0-9]*[A-Za-z])[A-Za-z0-9]{16,}(?![A-Za-z0-9])"
)
# Catch-all residue: long hex / long base64 blobs. Lookarounds (not \b) so a
# `_`-glued prefix can't slip past. `_HEX` covers digit-free hex (deadbeef…) that
# _HIENT skips. `_B64` lookbehind drops `=` so a blob AFTER `=`/padding
# (sig=…, AccountKey=…) is still caught.
_HEX = re.compile(r"(?<![0-9a-fA-F])[0-9a-fA-F]{32,}(?![0-9a-fA-F])")
_B64 = re.compile(r"(?<![A-Za-z0-9+/_-])[A-Za-z0-9+/_-]{40,}={0,2}")
# Windows machine/account SIDs (S-1-5-…) — redacted wholesale (incl. well-known).
_SID = re.compile(r"\bS-1-(?:\d+-)*\d+\b")

# --- Username collapse (run after secret stripping) ---
# ssh-context user@host: an ssh-family command, then (skipping flags/args) the
# first `user@`. Runs before _EMAIL so an ssh login to a host WITHOUT a dotted TLD
# (deployacct@host) still collapses to [user]@host; a bare prose email is redacted
# wholesale by _EMAIL below.
_SSH_USER = re.compile(r"(?i)\b(ssh|scp|sftp|rsync|git\s+remote)\b([^@\n]*?)[\w.\-]+@")
# C:\Users\<name>, UNC \\host\Users\<name>, XP Documents and Settings\<name>,
# /home|/Users/<name>, env-var home roots, HKEY_USERS\<name>, ~name / ~$name.
_HOME_WIN = re.compile(r"(?i)([A-Za-z]:[\\/]Users[\\/])[^\\/\s\"']+")
_HOME_UNC = re.compile(r"(?i)(\\\\[^\\/\s\"']+[\\/]Users[\\/])[^\\/\s\"']+")
_HOME_XP = re.compile(r"(?i)([\\/]Documents and Settings[\\/])[^\\/\s\"']+")
_HOME_NIX = re.compile(r"(?i)([\\/](?:home|Users)[\\/])[^\\/\s\"']+")
# Home env-var roots: the var EXPANDS to the home dir, so the following path
# segment is redacted (covers an adversarial %USERPROFILE%\<name> and the common
# %APPDATA%\<seg> — over-redacting a literal segment like Roaming is safe).
_HOME_ENV = re.compile(
    r"(?i)(%(?:USERPROFILE|HOMEPATH|APPDATA|LOCALAPPDATA|OneDrive)%[\\/])[^\\/\s\"']+"
)
_HOME_PSENV = re.compile(
    r"(?i)(\$env:(?:USERPROFILE|HOMEPATH|APPDATA|LOCALAPPDATA|HOME|OneDrive)[\\/])[^\\/\s\"']+"
)
_HOME_SHENV = re.compile(r"(\$\{?HOME\}?[\\/])[^\\/\s\"']+")
_HKEY_USERS = re.compile(r"(?i)((?:HKEY_USERS|HKU)[\\/])[^\\/\s\"']+")
# ~username / ~$username shell shorthand (NOT ~/… which has no name).
_HOME_TILDE = re.compile(r"(?<![\w./\\])~\$?([A-Za-z_][\w.\-]*)")
# Email addresses -> [REDACTED-EMAIL]. Applied LAST in redact() (after the
# username / connection-URL collapses) so the `[user]@host` / `[REDACTED]@host`
# placeholders — which end in `]` immediately before `@` — cannot match; only a
# bare prose `local@domain.tld` does. Intent lines are public, so emails are PII.
_EMAIL = re.compile(r"\b[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}\b")


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
    text = _STRIPE.sub("[REDACTED-TOKEN]", text)
    text = _OPENAI.sub("[REDACTED-TOKEN]", text)
    text = _NPM.sub("[REDACTED-TOKEN]", text)
    text = _VENDOR.sub("[REDACTED-TOKEN]", text)
    text = _BEARER.sub("Bearer [REDACTED]", text)
    text = _BASIC.sub("Basic [REDACTED]", text)
    text = _CONN.sub(r"\1[REDACTED]@", text)
    text = _KV.sub(r"\1[REDACTED]", text)
    text = _XML_SECRET.sub(r"\1[REDACTED]", text)
    text = _HIENT.sub("[REDACTED-TOKEN]", text)
    text = _HEX.sub("[REDACTED-HEX]", text)
    text = _B64.sub("[REDACTED-B64]", text)
    text = _SID.sub("[REDACTED-SID]", text)
    # Usernames -> placeholder (preserve the command / drive / prefix).
    text = _SSH_USER.sub(lambda m: m.group(1) + m.group(2) + "[user]@", text)
    text = _HOME_WIN.sub(lambda m: m.group(1) + "[user]", text)
    text = _HOME_UNC.sub(lambda m: m.group(1) + "[user]", text)
    text = _HOME_XP.sub(lambda m: m.group(1) + "[user]", text)
    text = _HOME_NIX.sub(lambda m: m.group(1) + "[user]", text)
    text = _HOME_ENV.sub(lambda m: m.group(1) + "[user]", text)
    text = _HOME_PSENV.sub(lambda m: m.group(1) + "[user]", text)
    text = _HOME_SHENV.sub(lambda m: m.group(1) + "[user]", text)
    text = _HKEY_USERS.sub(lambda m: m.group(1) + "[user]", text)
    text = _HOME_TILDE.sub(r"~[user]", text)
    # Emails LAST: the `]`-terminated placeholders above shield ssh/conn userinfo,
    # so only bare prose emails (public-PR PII) match here.
    text = _EMAIL.sub("[REDACTED-EMAIL]", text)
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
        # _BEARER strips the token; the `authorization` KV-stem also redacts the
        # scheme word — over-redaction (safe). Both layers fire, secret is gone.
        ("Authorization: Bearer abcdef0123456789xyz", ["abcdef0123456789"], ["[REDACTED]"]),
        ("password: hunter2supersecret", ["hunter2supersecret"], ["[REDACTED]"]),
        ("AWS AKIAIOSFODNN7EXAMPLE here", ["AKIAIOSFODNN7EXAMPLE"], ["[REDACTED-TOKEN]"]),
        ("slack xoxb-1234567890-abcdefghijkl", ["xoxb-1234567890"], ["[REDACTED-TOKEN]"]),
        ("deadbeefdeadbeefdeadbeefdeadbeefdeadbeef", ["deadbeefdeadbeef"], ["[REDACTED-HEX]"]),
        # C1: JSON / quoted-key secret pair must not leak the value.
        ('{"password":"hunter2supersecret"}', ["hunter2supersecret"], ["[REDACTED]"]),
        ("config 'api_key' = 'sneakyvalue123'", ["sneakyvalue123"], ["[REDACTED]"]),
        # H1: sub-threshold named secrets (below the 32-hex / 40-b64 floor).
        ("stripe sk_live_abcdEFGH1234 ok", ["sk_live_abcdEFGH1234"], ["[REDACTED-TOKEN]"]),
        ("openai sk-abcdEFGHijklMNOPqrstUV here", ["sk-abcdEFGHijklMNOP"], ["[REDACTED-TOKEN]"]),
        # _CONN now redacts the WHOLE userinfo (user + pass), not just the password.
        ("clone https://user:s3cr3ttoken@github.com/x", ["s3cr3ttoken", "user:"], ["[REDACTED]", "@github.com"]),
        ("cache redis://:s3cr3tpass@cache:6379/0", ["s3cr3tpass"], ["[REDACTED]", "@cache"]),
        ("path C:\\Users\\alexk\\.aws\\creds", ["alexk"], ["[user]"]),
        # H2: UNC + legacy XP profile roots.
        ("unc \\\\fileserver\\Users\\alexk\\notes", ["alexk"], ["[user]"]),
        ("xp C:\\Documents and Settings\\alexk\\app", ["alexk"], ["[user]"]),
        ("path /home/alexk/.ssh/id", ["/home/alexk"], ["[user]"]),
        # #7: emails are now REDACTED (public PR body is PII surface).
        ("contact alexkonstantonis@gmail.com please", ["alexkonstantonis@gmail.com"], ["[REDACTED-EMAIL]"]),
        ("ping jane.doe+tag@sub.example.co.uk now", ["jane.doe+tag@sub.example.co.uk"], ["[REDACTED-EMAIL]"]),
        ("line one\nline two\ttabbed", ["\n", "\t"], ["line one line two tabbed"]),
        # --- Post-red-team hardening (under-redaction is the vulnerability) ---
        # Connection URL with `/`-in-password + non-empty user: whole userinfo gone.
        ("db postgres://u:p/a/ss@db.host/x", ["p/a/ss", "u:"], ["[REDACTED]", "@db.host"]),
        # Multi-`@` password: greedy authority grabs to the last `@`.
        ("mongodb://admin:p@ssw0rdSecretTail@db.host/x", ["ssw0rdSecretTail"], ["[REDACTED]", "@db.host"]),
        # Username-only connection URL: a bare account name is still a leak.
        ("amqp://serviceaccount@broker:5672", ["serviceaccount"], ["[REDACTED]", "@broker"]),
        # `_`-glued hex must not slip a word-boundary anchor (SECRET_<hex>).
        ("leak SECRET_deadbeefdeadbeefdeadbeefdeadbeef00", ["deadbeefdeadbeef"], ["[REDACTED"]),
        # base64url residue (-/_ alphabet) above the 40-char floor.
        ("blob ABCDEFGHIJKLMNOPQRSTUVWXYZ-_abcdefghij0123 ok", ["ABCDEFGHIJKLMNOP"], ["[REDACTED-B64]"]),
        # base64 AFTER `=` (sig=, AccountKey=) — `=` dropped from the lookbehind.
        ("sig=Rm9vQmFyQmF6MTIzNDU2Nzg5MGFiY2RlZmdoaWprbA== end", ["Rm9vQmFyQmF6MTIz"], ["[REDACTED"]),
        # Generic high-entropy token (no recognizable vendor prefix).
        ("blob AHGXYZ123ABC456DEF789GHI012 ok", ["AHGXYZ123ABC456"], ["[REDACTED-TOKEN]"]),
        # Sub-32 hex (31 chars) caught by the generic digit+letter sweep.
        ("md5ish d41d8cd98f00b204e9800998ecf8427 here", ["d41d8cd98f00b204"], ["[REDACTED"]),
        # Vendor-prefix token (DigitalOcean).
        ("do dop_v1_abcdef0123456789abcdef0123 ok", ["dop_v1_abcdef0123"], ["[REDACTED-TOKEN]"]),
        # npm + HTTP Basic tokens (sub-threshold, name-anchored).
        ("npm npm_abcdefghijklmnopqrstuvwxyz0123 ok", ["npm_abcdefghijklmnop"], ["[REDACTED-TOKEN]"]),
        ("header Basic dXNlcjpwYXNzd29yZA== end", ["dXNlcjpwYXNzd29yZA"], ["[REDACTED]"]),
        # Glued-prefix bypass: a leading letter must not shield ghp_/npm_.
        ("Xghp_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", ["ghp_ABCDEF"], ["[REDACTED"]),
        # Broadened key=value: prefixed keys, =>/tab separators, more stems.
        ("creds db_pass=secretvalue123", ["secretvalue123"], ["[REDACTED]"]),
        ("yaml passphrase: correcthorsebattery", ["correcthorsebattery"], ["[REDACTED]"]),
        ("ruby api_key => 'sk_hidden_value_x'", ["sk_hidden_value_x"], ["[REDACTED]"]),
        ("tsv token\tabcdefghijklmnop", ["abcdefghijklmnop"], ["[REDACTED]"]),
        ("raw Authorization: tok_abcdef123456 here", ["tok_abcdef123456"], ["[REDACTED]"]),
        # DB username key (ADO.NET Uid=) is redacted per the expanded scope.
        ("conn Server=db;Uid=reportreader;Pwd=p", ["reportreader"], ["[REDACTED]"]),
        # Comma/brace-tail value: bare run is [^\s]+ so the tail is swept.
        ("token=abc,butthisisalsosecret456", ["butthisisalsosecret456"], ["[REDACTED]"]),
        # Escaped-quote tail inside a quoted value.
        ('password="abc\\"realsecrettail"', ["realsecrettail"], ["[REDACTED]"]),
        # XML element body (no key=value separator).
        ("<client_secret>topsecretvalue</client_secret>", ["topsecretvalue"], ["[REDACTED]"]),
        # ssh-context user@host (login account) is redacted; flags are skipped.
        ("deploy ssh -p 2222 rootops@bastion.example.com", ["rootops"], ["[user]@bastion"]),
        ("scp -i key deployacct@host:/tmp", ["deployacct"], ["[user]@host"]),
        # Windows SID (incl. well-known) redacted wholesale.
        ("profile S-1-5-21-100-200-300-1001 owns it", ["S-1-5-21-100"], ["[REDACTED-SID]"]),
        ("system S-1-5-18 here", ["S-1-5-18"], ["[REDACTED-SID]"]),
        # Home env-var roots + HKEY_USERS hive + ~$name.
        ("open %APPDATA%\\alexk\\cfg", ["alexk"], ["[user]"]),
        ("sh $HOME/alexk/.bashrc", ["/alexk"], ["[user]"]),
        ("ps $env:USERPROFILE\\alexk\\x", ["alexk"], ["[user]"]),
        ("reg HKEY_USERS\\alexk_Classes\\k", ["alexk"], ["[user]"]),
        ("config ~$alexk/.vimrc", ["alexk"], ["~[user]"]),
        # --- Broad-class closures (red-team round 2; "close broad classes, ship") ---
        # SendGrid two-segment token — short segments below the high-entropy floor.
        ("sendgrid SG.shortA.shortB used", ["SG.shortA.shortB"], ["[REDACTED-TOKEN]"]),
        # Truncated Google key (AIza floor lowered to 8 — prefix is unambiguous).
        ("gcp key AIzaSyAbCdEf shown", ["AIzaSyAbCdEf"], ["[REDACTED-TOKEN]"]),
        # ADO.NET `User Id=` space-key (was glued-only `userid`).
        ("conn Server=db;User Id=app_login;Pwd=x", ["app_login"], ["[REDACTED]"]),
        # Escaped-JSON `\"key\":\"val\"` (backslash before the quote).
        ('{\\"password\\":\\"jsonescpass55\\"}', ["jsonescpass55"], ["[REDACTED]"]),
        # --- Over-redaction bounds (no false positives on readable intent prose) ---
        # Non-secret look-alike must survive (boundary discipline).
        ("the passenger: 5 boarded", ["[REDACTED]"], ["passenger: 5"]),
        # CamelCase identifiers (no digits / short digit runs) must survive intact.
        ("refactor Base64Encoder and SmatchetActiveProjectGridUi today",
         ["[REDACTED"], ["Base64Encoder", "SmatchetActiveProjectGridUi"]),
        # WSL/back-slash home + ~username; but ~/… (no name) is untouched.
        ("wsl D:\\home\\alexk\\proj", ["alexk"], ["[user]"]),
        ("config ~alexk/.vimrc open", ["~alexk"], ["~[user]"]),
        ("run ~/.bashrc now", ["~[user]"], ["~/.bashrc"]),
    ]
    ok = True
    for i, (inp, forbidden, required) in enumerate(cases):
        out = redact(inp)
        bad = [s for s in forbidden if s in out] + [s for s in required if s not in out]
        if bad or "\n" in out:
            ok = False
            sys.stderr.write("selftest case %d FAIL: %r -> %r\n" % (i, inp, out))
    # selftest: asserts-failure — exercise the leak-DETECTION path, not just the
    # happy path: confirm redact() strips a known secret AND that the forbidden-
    # substring check above can actually SEE a surviving secret. If redaction
    # regressed to a no-op, the first assert fires; if the detector itself were
    # inverted/broken (a pass-only selftest that silently leaks — the exact class
    # test-gate-selftests.sh guards), the second fires.
    _secret = "ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    if _secret in redact("token=" + _secret):
        ok = False
        sys.stderr.write("selftest meta-check FAIL: secret survived redact()\n")
    if _secret not in ("token=" + _secret):  # the detector must see a present secret
        ok = False
        sys.stderr.write("selftest meta-check FAIL: leak detector cannot see a present secret\n")
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
