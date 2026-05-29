# Shell-script self-review checklist

Five rules every `scripts/dev/*.sh` ships through, enforced by `scripts/dev/test-shell-lint.sh` (auto-runs via `scripts/dev/test-all.sh` at the pre-push gate). Each rule closes a real CodeRabbit finding class from session 2026-05-28. Plan: [`../plans/shipped/shell-script-self-review-lint.md`](../plans/shipped/shell-script-self-review-lint.md).

Bypass: `SMATCHET_SKIP_SHELL_LINT=1` (logged; emergency-only).

---

## 1. Dependency preflight (rule id `SHELL_LINT_DEPS`)

Every external command in the closed 19-entry allowlist must have a `command -v <name>` (or `which <name>` / `type -p <name>`) guard somewhere in the script body.

**Allowlist** (`curl`, `gh`, `cmake`, `python`, `python3`, `jq`, `7z`, `cppcheck`, `clang-format`, `clang-tidy`, `shellcheck`, `actionlint`, `bats`, `p4`, `cl.exe`, `clang-cl`, `link.exe`, `cygpath`, `tasklist`). `git` is intentionally **omitted** — it's effectively a shell builtin in any dev environment and preflighting it generates ~25 spurious violations across the existing tree.

**Why** — PR #482's `git-janitor.sh` shipped without a `python` preflight; on python3-only hosts the script silently produced empty PR_STATE and wrong "expected MERGED" refusal output.

**Bad**:
```bash
set -euo pipefail
python -c 'import json; ...'
```

**Good**:
```bash
set -euo pipefail
command -v python >/dev/null 2>&1 || { echo "python required" >&2; exit 2; }
python -c 'import json; ...'
```

**Allowlist drift** — using a tool not on the allowlist (e.g. `node`, `npm`, `ripgrep`, `docker`) emits a non-blocking `INFO: tool '<name>' not in allowlist; consider adding to scripts/dev/test-shell-lint.sh`. Extending the allowlist is a one-line change in that script.

---

## 2. shellcheck clean (rule id `SHELL_LINT_SHELLCHECK`)

Run `shellcheck` against every script; fail on these codes regardless of shellcheck's default severity gating:

| Code | What it catches |
|---|---|
| SC2086 | Unquoted variable expansion (word-split / glob expansion) |
| SC2046 | Word-split on unquoted command substitution |
| SC2128 | Array referenced as scalar (silently picks element 0) |
| SC2155 | `local foo=$(...)` masks the command's return value from `set -e` |
| SC2068 | `$@` referenced unquoted in array context |

**Why** — PR #478's `p4-git-sync-check.sh` had unquoted `$git_not_p4` / `$p4_not_git` in `printf`, causing word-splitting / glob expansion on paths with spaces.

**Bad**:
```bash
echo $input        # SC2086 — word-splits / globs
local foo=$(...)   # SC2155 — masks $?
printf '%s\n' $@   # SC2068 — array as scalar
```

**Good**:
```bash
echo "$input"
local foo
foo=$(...)
printf '%s\n' "$@"
```

**Intentional word-splitting** (rare — e.g. expanding `set -- $a` to split a `"X Y Z"` tuple, or splitting a caller-passed multi-arg string) gets a `# shellcheck disable=SC2086` annotation on the line above, with a rationale comment naming why the split is intentional.

---

## 3. `curl -f` everywhere (rule id `SHELL_LINT_CURL_FAIL`)

Every `curl` invocation must carry `-f` or `--fail`. Without it, a 4xx/5xx server response lands as a "successful" download (silent body containing an HTML error page, not the expected payload).

**Why** — PR #477's Font Awesome download used `curl -sSL` without `-f`; the upstream URL returned a 404 HTML page that was saved as `fa-solid-900.ttf`, breaking icon rendering until detected at runtime.

**Bad**:
```bash
curl -sSL "$URL" -o assets/font.ttf
```

**Good**:
```bash
curl -fsSL "$URL" -o assets/font.ttf
```

`-f` is even appropriate on probe-style requests (e.g. localhost health checks) — without it, a server-side 500 silently masquerades as "endpoint responded", giving false success.

---

## 4. sha256 verify on file downloads (rule id `SHELL_LINT_SHA256`)

Every `curl -o <path>` / `curl --output <path>` must be followed within 10 lines by `sha256sum -c` or `--checksum`. Supply-chain integrity check; also detects the silent-404-as-HTML case from rule #3 even when somebody forgets the `-f`.

**Why** — PR #477's Font Awesome download (`fa-solid-900.ttf` from the `6.x` mutable branch) had no checksum verify; even with `-f` added later, the upstream tag could be silently re-pointed.

**Bad**:
```bash
curl -fsSL "$URL" -o assets/font.ttf
mv assets/font.ttf assets/fa-solid-900.ttf
```

**Good**:
```bash
curl -fsSL "$URL" -o assets/font.ttf
echo "$EXPECTED_SHA  assets/font.ttf" | sha256sum -c -
mv assets/font.ttf assets/fa-solid-900.ttf
```

---

## 5. `--key=value` ↔ `--key value` parity (rule id `SHELL_LINT_FLAG_PARITY`)

When a script's argument parser has a `--<flag>)` case-branch **AND that branch is value-taking** (consumes `$2` or does `shift 2`), it must also have a `--<flag>=*)` case branch. Vice versa: an `--<flag>=*)` case must also have a bare `--<flag>)` twin.

Boolean flags (`--<flag>)` with `shift` only, no `$2`) are skipped — `--verbose` doesn't need a `--verbose=*)` twin.

**Why** — PR #477's CLI parser supported `--threshold <N>` with input validation, but `--threshold=<N>` skipped that validation entirely. Either form should accept (or reject) the same inputs.

**Bad**:
```bash
case "$1" in
    --threshold)
        [[ "$2" =~ ^[0-9]+$ ]] || { echo "must be int" >&2; exit 2; }
        THRESHOLD="$2"; shift 2 ;;
esac
# missing --threshold=*) — `script --threshold=foo` parses silently
```

**Good**:
```bash
case "$1" in
    --threshold)
        [[ "$2" =~ ^[0-9]+$ ]] || { echo "must be int" >&2; exit 2; }
        THRESHOLD="$2"; shift 2 ;;
    --threshold=*)
        THRESHOLD="${1#--threshold=}"
        [[ "$THRESHOLD" =~ ^[0-9]+$ ]] || { echo "must be int" >&2; exit 2; }
        shift ;;
esac
```

---

## Running the lint manually

```bash
# Repo-wide
bash scripts/dev/test-shell-lint.sh

# Single script
bash scripts/dev/test-shell-lint.sh --target scripts/dev/my-script.sh

# Bypass (emergency only — logged)
SMATCHET_SKIP_SHELL_LINT=1 bash scripts/dev/test-shell-lint.sh
```

Output format: `<path>:<line>: <rule-id>: <message>`. Final summary line `Passed: N  Failed: M` is consumed by `scripts/dev/test-all.sh`.
