#!/usr/bin/env bash
# guard-auto-merge-arm.sh — PreToolUse guard: deny arming GitHub-NATIVE auto-merge
# outside agents/scripts/core/safe-merge.sh.
#
# Native auto-merge waits only on the branch-protection REQUIRED contexts, so it
# merges straight past a red or still-pending NON-required check — the class
# behind postmortems.md 2026-10-04 (#2286, armed through a harness PR tool) and
# the earlier bare `gh pr merge --auto` incidents. safe-merge.sh is the sanctioned
# arm path: it runs the full merge-gates poll and arms `--auto` only on
# GATES_PASSED. Every other arm path creates the same server-side auto-merge
# request, so this hook denies the ones a Claude Code session can reach:
#   * a GitHub-MCP auto-merge tool  (`mcp__<server>__enable_pr_auto_merge`);
#   * a harness PR tool `set_auto_merge` (bare or `mcp__<server>__set_auto_merge`)
#     unless its input explicitly DISABLES (enabled / enable / auto_merge /
#     autoMerge == false);
#   * a Bash / PowerShell command that RUNS `gh pr merge … --auto` (or a `gh api`
#     enablePullRequestAutoMerge mutation). The command is tokenized the way the
#     shell does — quotes, escapes, `$(…)` / backticks / `<(…)` (also inside
#     double quotes and unquoted heredoc bodies), redirections — and split into
#     simple commands on ; && || | & ( ) and newlines. Each simple command is
#     stripped of leading keywords / wrappers (if then elif else do while until
#     ! { time env VAR=val sudo nohup command exec xargs timeout <dur> nice …);
#     it arms when argv[0] is `gh`, its first two positionals are `pr merge`
#     (global flags such as `-R x` allowed anywhere) and a token is `--auto`.
#     Quoted text is never a command position, so a commit message or a PR body
#     that mentions the command passes. A heredoc body or `-c` string fed to a
#     shell (`bash <<EOF`, `sh -s <<EOF`, `bash -c '…'`) and an `eval` string
#     are executed, so they are checked too; any other heredoc body is text.
#     `safe-merge.sh <pr>` itself never matches (its own `--auto` runs inside the
#     script), and disarming (`--disable-auto`, `disable_pr_auto_merge`) is never
#     blocked. The tokenizer runs in python3 (as capture-intent.sh's does); with
#     no working python the older single-regex match is the fallback.
#
# OUT OF SCOPE: DIRECT merges — `gh pr merge N --squash` without `--auto`, the
# GitHub-MCP `merge_pull_request` tool — are not arming auto-merge and are not
# checked here (merge-gates.md governs them). Also out of reach: a script file
# the command runs, text piped into a shell, deliberately obfuscated words
# (`--a''uto`), a REST/GraphQL call made outside `gh api`, and the web UI.
#
# DEFENCE IN DEPTH ONLY: it covers one harness and the command text it is shown.
# The arm-path-independent backstop is the `All checks green (block-on-any-red)`
# aggregate check (agents/scripts/core/all-checks-green.sh) once it is a
# required context. See docs/agent-rules/merge-gates.md § Sanctioned non-admin
# merge path.
#
# Wiring: a PreToolUse matcher in settings.json.tmpl runs the deployed copy at
# .claude/hooks/ (the SessionStart hook sync in clear-session-context.sh copies
# every hook here; sync-settings-hooks.sh heals the matcher into an existing
# settings.json). No env override by design — the sanctioned path exists; a
# human who really wants native auto-merge arms it outside the agent.
#
# Protocol: tool-call JSON on stdin. Allow = exit 0 with no output. Deny = exit 0
# with a permissionDecision JSON. Unparseable input -> allow (fail-open guard).

set -u

json_escape() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  printf '%s' "$s"
}

deny() {
  printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"%s"}}' \
    "$(json_escape "$1 GitHub-native auto-merge waits only on branch-protection REQUIRED contexts, so it merges past a red or still-pending non-required check. Arm through the sanctioned wrapper instead: bash agents/scripts/core/safe-merge.sh <pr> (runs the full merge-gates poll, arms --auto only on GATES_PASSED). Disarming stays allowed. See docs/agent-rules/merge-gates.md § Sanctioned non-admin merge path.")"
  exit 0
}

INPUT="$(cat || true)"
[ -n "$INPUT" ] || exit 0

HAVE_JQ=0
command -v jq >/dev/null 2>&1 && HAVE_JQ=1

json_field() { # $1 = jq filter, $2 = key for the no-jq sed fallback
  if [ "$HAVE_JQ" = 1 ]; then
    printf '%s' "$INPUT" | jq -r "$1 // empty" 2>/dev/null
  else
    printf '%s' "$INPUT" | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p" | head -n1
  fi
}

TOOL="$(json_field '.tool_name' 'tool_name')"

case "$TOOL" in
  mcp__*__enable_pr_auto_merge)
    deny "Blocked: $TOOL arms GitHub-native auto-merge."
    ;;
  set_auto_merge|mcp__*__set_auto_merge)
    if [ "$HAVE_JQ" = 1 ] && printf '%s' "$INPUT" \
         | jq -e '[.tool_input.enabled, .tool_input.enable, .tool_input.auto_merge, .tool_input.autoMerge] | any(. == false)' \
           >/dev/null 2>&1; then
      exit 0   # an explicit disable — disarming is always allowed
    fi
    deny "Blocked: $TOOL (enable) arms GitHub-native auto-merge."
    ;;
  Bash|PowerShell) ;;
  *) exit 0 ;;
esac

CMD="$(json_field '.tool_input.command' 'command')"
[ -n "$CMD" ] || exit 0

# Fast path: an arm needs the text `auto` somewhere (--auto, or the GraphQL
# enablePullRequestAutoMerge); every other command skips the tokenizer.
shopt -s nocasematch
[[ "$CMD" == *auto* ]] || exit 0
shopt -u nocasematch

DENY_MSG="Blocked: a bare \`gh pr merge … --auto\` arms GitHub-native auto-merge."

# ---------------------------------------------------------------- tokenizer
# Exit 10 = the command arms auto-merge, 11 = it does not, anything else = the
# tokenizer could not run (fall back to the regex below).
read -r -d '' ARM_PY <<'PY'
import re
import sys

MAX_DEPTH = 8
ASSIGN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(\[[^]]*\])?\+?=")
FD_WORD = re.compile(r"^(\d+|\{[A-Za-z_][A-Za-z0-9_]*\})$")
# Reserved words that may precede a command in the same simple command.
KEYWORDS = {"if", "then", "elif", "else", "fi", "do", "done", "while", "until",
            "!", "{", "}", "coproc"}
# Wrappers that run their argument vector: name -> options that take a value.
WRAPPERS = {
    "env": {"-u", "--unset", "-C", "--chdir", "-S", "--split-string"},
    "sudo": {"-u", "--user", "-g", "--group", "-h", "--host", "-p", "--prompt",
             "-C", "--close-from", "-D", "--chdir", "-r", "--role", "-t", "--type",
             "-U", "--other-user", "-T", "--command-timeout", "-R", "--chroot"},
    "doas": {"-u", "-C"},
    "timeout": {"-s", "--signal", "-k", "--kill-after"},
    "nice": {"-n", "--adjustment"},
    "xargs": {"-a", "--arg-file", "-d", "--delimiter", "-E", "-I", "-L", "-n",
              "--max-args", "-P", "--max-procs", "-s", "--max-chars",
              "--process-slot-var"},
    "time": {"-f", "--format", "-o", "--output"},
    "command": set(), "builtin": set(), "exec": {"-a"}, "nohup": set(),
    "setsid": set(), "stdbuf": {"-i", "--input", "-o", "--output", "-e", "--error"},
    "ionice": {"-c", "--class", "-n", "--classdata", "-p", "--pid"},
    "chronic": set(), "unbuffer": set(),
}
SHELLS = {"bash", "sh", "zsh", "dash", "ksh", "mksh", "ash", "fish"}
# gh flags whose value is a separate word (so it is not read as a positional).
GH_VALUE_FLAGS = {"-R", "--repo", "--hostname", "-b", "--body", "-F", "--body-file",
                  "-t", "--subject", "-A", "--author-email", "--match-head-commit"}


class Cmd(object):
    def __init__(self):
        self.words = []
        self.stdin = []   # heredoc / here-string bodies fed to this command


class Parser(object):
    """One pass over shell text; every simple command found (substitutions
    included) is appended to the shared `out` list."""

    def __init__(self, s, depth, out):
        self.s, self.n, self.depth, self.out = s, len(s), depth, out
        self.cmd = Cmd()
        self.buf = None
        self.quoted = False
        self.role = None          # None | redir | heredoc | herestring
        self.strip_tabs = False
        self.pending = []         # (cmd, delimiter, strip_tabs, expands)

    def add(self, text, quoted=False):
        if self.buf is None:
            self.buf = []
        self.buf.append(text)
        self.quoted = self.quoted or quoted

    def end_word(self):
        if self.buf is None:
            return
        word, quoted = "".join(self.buf), self.quoted
        role, self.buf, self.quoted, self.role = self.role, None, False, None
        if role == "heredoc":
            self.pending.append((self.cmd, word, self.strip_tabs, not quoted))
        elif role == "herestring":
            self.cmd.stdin.append(word)
        elif role is None:
            self.cmd.words.append(word)

    def end_cmd(self):
        self.end_word()
        self.role = None
        if self.cmd.words or self.cmd.stdin or any(p[0] is self.cmd for p in self.pending):
            self.out.append(self.cmd)
        self.cmd = Cmd()

    def redirect(self, role):
        if self.buf is not None and not self.quoted and FD_WORD.match("".join(self.buf)):
            self.buf = None       # `2>` — the glued word is the fd, not an argument
        else:
            self.end_word()
        self.role = role

    def run(self, i, stop_paren=False):
        s, n, paren = self.s, self.n, 0
        while i < n:
            c = s[i]
            nx = s[i + 1] if i + 1 < n else ""
            if c in " \t\r":
                self.end_word()
                i += 1
            elif c == "\n":
                self.end_cmd()
                i = self.read_heredocs(i + 1)
            elif c == "#" and self.buf is None:
                j = s.find("\n", i)
                i = n if j < 0 else j
            elif c == "\\":
                if nx != "\n":
                    self.add(nx, True)
                i += 2
            elif c == "'":
                j = s.find("'", i + 1)
                j = n if j < 0 else j
                self.add(s[i + 1:j], True)
                i = j + 1
            elif c == "$" and nx == "'":
                text, i = self.ansi_c(i + 2)
                self.add(text, True)
            elif c == '"':
                text, i = self.dq(i + 1, '"')
                self.add(text, True)
            elif c == "$" and nx == "(":
                j = self.dollar_paren(i)
                self.add(s[i:j])
                i = j
            elif c == "$" and nx == "{":
                j = self.skip_braces(i + 2)
                self.add(s[i:j])
                i = j
            elif c == "`":
                j = self.backtick(i + 1)
                self.add(s[i:j])
                i = j
            elif c in "<>" and nx == "(":
                self.end_word()
                i = self.subst(i + 2)
            elif c == "(":
                self.end_cmd()
                paren += 1
                i += 1
            elif c == ")":
                self.end_cmd()
                i += 1
                if stop_paren and paren == 0:
                    return i
                paren = max(0, paren - 1)
            elif c in ";|":
                self.end_cmd()
                i += 1
                while i < n and s[i] in ";&|":
                    i += 1
            elif c == "&":
                if nx == ">":
                    self.redirect("redir")
                    i += 3 if s.startswith("&>>", i) else 2
                else:
                    self.end_cmd()
                    i += 1
                    while i < n and s[i] == "&":
                        i += 1
            elif c == "<":
                if s.startswith("<<<", i):
                    self.redirect("herestring")
                    i += 3
                elif s.startswith("<<", i):
                    self.redirect("heredoc")
                    i += 2
                    self.strip_tabs = i < n and s[i] == "-"
                    if self.strip_tabs:
                        i += 1
                else:
                    self.redirect("redir")
                    i += 1
                    if i < n and s[i] in "&>":
                        i += 1
            elif c == ">":
                self.redirect("redir")
                i += 1
                if i < n and s[i] in ">&|":
                    i += 1
            else:
                self.add(c)
                i += 1
        self.end_cmd()
        return n

    def read_heredocs(self, i):
        s, n = self.s, self.n
        pending, self.pending = self.pending, []
        for cmd, delim, strip_tabs, expands in pending:
            lines = []
            while i < n:
                j = s.find("\n", i)
                line = s[i:] if j < 0 else s[i:j]
                i = n if j < 0 else j + 1
                if (line.lstrip("\t") if strip_tabs else line).rstrip("\r") == delim:
                    break
                lines.append(line)
            body = "\n".join(lines)
            cmd.stdin.append(body)
            if expands and self.depth < MAX_DEPTH:
                # An unquoted delimiter: $(...) and backticks in the body run.
                Parser(body, self.depth + 1, self.out).dq(0, None)
        return i

    def dq(self, i, term):
        s, n, buf = self.s, self.n, []
        while i < n:
            c = s[i]
            nx = s[i + 1] if i + 1 < n else ""
            if term is not None and c == term:
                return "".join(buf), i + 1
            if c == "\\" and nx:
                if nx == "\n":
                    i += 2
                elif nx in "$`\"\\":
                    buf.append(nx)
                    i += 2
                else:
                    buf.append(c)
                    i += 1
            elif c == "$" and nx == "(":
                j = self.dollar_paren(i)
                buf.append(s[i:j])
                i = j
            elif c == "$" and nx == "{":
                j = self.skip_braces(i + 2)
                buf.append(s[i:j])
                i = j
            elif c == "`":
                j = self.backtick(i + 1)
                buf.append(s[i:j])
                i = j
            else:
                buf.append(c)
                i += 1
        return "".join(buf), n

    def ansi_c(self, i):
        s, n, buf = self.s, self.n, []
        esc = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", "'": "'", '"': '"'}
        while i < n and s[i] != "'":
            if s[i] == "\\" and i + 1 < n:
                buf.append(esc.get(s[i + 1], "\\" + s[i + 1]))
                i += 2
            else:
                buf.append(s[i])
                i += 1
        return "".join(buf), i + 1

    def dollar_paren(self, i):
        if self.s.startswith("$((", i):
            return self.skip_parens(i + 3, 2)     # arithmetic, not a command
        return self.subst(i + 2)

    def subst(self, i):
        if self.depth >= MAX_DEPTH:
            return self.skip_parens(i, 1)
        return Parser(self.s, self.depth + 1, self.out).run(i, stop_paren=True)

    def skip_parens(self, i, depth):
        s, n = self.s, self.n
        while i < n and depth > 0:
            if s[i] == "(":
                depth += 1
            elif s[i] == ")":
                depth -= 1
            i += 1
        return i

    def skip_braces(self, i):
        s, n, depth = self.s, self.n, 1
        while i < n and depth > 0:
            if s[i] == "\\":
                i += 2
                continue
            if s[i] == "{":
                depth += 1
            elif s[i] == "}":
                depth -= 1
            i += 1
        return i

    def backtick(self, i):
        s, n, j, buf = self.s, self.n, i, []
        while j < n and s[j] != "`":
            if s[j] == "\\" and j + 1 < n and s[j + 1] in "`$\\":
                buf.append(s[j + 1])
                j += 2
            else:
                buf.append(s[j])
                j += 1
        if self.depth < MAX_DEPTH:
            Parser("".join(buf), self.depth + 1, self.out).run(0)
        return min(j + 1, n)


def base(word):
    b = re.split(r"[\\/]", word)[-1].lower()
    return b[:-4] if b.endswith(".exe") else b


def skip_wrapper(name, w):
    takes, i = WRAPPERS[name], 0
    while i < len(w):
        t = w[i]
        if t == "--":
            i += 1
            break
        if name == "env" and ASSIGN.match(t):
            i += 1
            continue
        if not t.startswith("-") or t == "-":
            break
        if name == "command" and t in ("-v", "-V"):
            return None                            # a lookup, nothing runs
        i += 2 if t in takes else 1
    w = w[i:]
    if name == "timeout" and w:
        w = w[1:]                                  # the DURATION
    return w


def strip(words):
    w = list(words)
    while w:
        if w[0] in KEYWORDS or ASSIGN.match(w[0]):
            w = w[1:]
            continue
        b = base(w[0])
        if b in WRAPPERS:
            w = skip_wrapper(b, w[1:])
            if w is None:
                return []
            continue
        break
    return w


def is_arm(w):
    if base(w[0]) != "gh":
        return False
    pos, auto, i = [], False, 1
    while i < len(w):
        t = w[i]
        if t in GH_VALUE_FLAGS:
            i += 2
            continue
        if t == "--auto" or (t.startswith("--auto=") and t[7:].lower() not in ("false", "0", "f")):
            auto = True
        elif not t.startswith("-"):
            pos.append(t)
        i += 1
    if pos[:2] == ["pr", "merge"] and auto:
        return True
    return pos[:1] == ["api"] and any("enablepullrequestautomerge" in t.lower() for t in w)


def shell_payloads(w, cmd):
    if base(w[0]) not in SHELLS:
        return []
    has_c = has_s = False
    first, i = None, 1
    while i < len(w):
        t = w[i]
        if t in ("-o", "+o", "-O", "+O", "--rcfile", "--init-file"):
            i += 2
            continue
        if t == "--":
            first = w[i + 1] if i + 1 < len(w) else None
            break
        if len(t) > 1 and t[0] in "-+":
            if not t.startswith("--"):
                has_c = has_c or "c" in t[1:]
                has_s = has_s or "s" in t[1:]
            i += 1
            continue
        first = t
        break
    if has_c:
        return [first] if first is not None else []
    if has_s or first is None:
        return list(cmd.stdin)     # the shell reads its script from stdin
    return []


def arms(text, depth=0):
    if depth > MAX_DEPTH:
        return False
    out = []
    Parser(text, depth, out).run(0)
    for cmd in out:
        w = strip(cmd.words)
        if not w:
            continue
        if is_arm(w):
            return True
        if base(w[0]) == "eval" and arms(" ".join(w[1:]), depth + 1):
            return True
        for payload in shell_payloads(w, cmd):
            if arms(payload, depth + 1):
                return True
    return False


def main():
    text = sys.stdin.buffer.read().decode("utf-8", "replace")
    if len(sys.argv) > 1 and sys.argv[1] == "PowerShell":
        # PowerShell: backtick is the escape / line-continuation character.
        text = text.replace("`\r\n", " ").replace("`\n", " ").replace("`", "\\")
    sys.exit(10 if arms(text) else 11)


try:
    main()
except SystemExit:
    raise
except Exception:
    sys.exit(12)
PY

for py in python3 python; do
  command -v "$py" >/dev/null 2>&1 || continue
  printf '%s' "$CMD" | "$py" -c "$ARM_PY" "$TOOL" 2>/dev/null
  case $? in
    10) deny "$DENY_MSG" ;;
    11) exit 0 ;;
  esac
done

# ------------------------------------------------- fallback: no working python
# Join `\`-newline continuations so a wrapped `gh pr merge … \ --auto` is one line.
bsnl=$'\\\n'
CMD="${CMD//"$bsnl"/ }"

# Drop heredoc BODIES: a `gh pr merge … --auto` line inside `cat <<EOF … EOF`
# (a doc or script being written) is text, not a command. Best-effort, one level.
strip_heredoc() {
  local line state=0 delim="" out=""
  local hdre='<<-?[[:space:]]*["'"'"']?([A-Za-z_][A-Za-z0-9_]*)'
  while IFS= read -r line || [ -n "$line" ]; do
    if [ "$state" = 1 ]; then
      [[ "$line" =~ ^[[:space:]]*"$delim"[[:space:]]*$ ]] && state=0
      continue
    fi
    [[ "$line" =~ $hdre ]] && { delim="${BASH_REMATCH[1]}"; state=1; }
    out+="$line"$'\n'
  done <<< "$1"
  printf '%s' "$out"
}

# `gh pr merge … --auto` at a COMMAND position only: line start or after a
# ; & | ( ` && || separator, optionally behind VAR=value assignments and a
# command/env/exec/time/nohup/sudo wrapper, `gh` possibly path-qualified.
cmdpos='(^|[;&|(`]|&&|\|\|)[[:space:]]*'
assigns='([A-Za-z_][A-Za-z0-9_]*=[^[:space:]]*[[:space:]]+)*'
wrappers='((command|env|exec|time|nohup|sudo)[[:space:]]+)*'
ghbin='([^[:space:];&|()]*/)?gh(\.exe)?'
pattern="${cmdpos}${assigns}${wrappers}${ghbin}[[:space:]]+pr[[:space:]]+merge([[:space:]]+[^;&|[:space:]]+)*[[:space:]]+--auto(=[^[:space:];&|]*)?([[:space:];&|)]|\$)"

if printf '%s\n' "$(strip_heredoc "$CMD")" | grep -qE -- "$pattern"; then
  deny "$DENY_MSG"
fi
exit 0
