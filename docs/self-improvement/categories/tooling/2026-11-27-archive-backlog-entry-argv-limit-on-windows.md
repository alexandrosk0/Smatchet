# `archive-backlog-entry.sh` fails on Windows: every `.md` path goes on one command line

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-11-27
- **Observed on**: archiving `test/2026-11-26-config-save-concurrency-stop-flush-flake.md` on branch `claude/batch28-followups`, Git Bash on Windows 11.
- **Status**: open

## What happened

`bash agent-layer/agents/scripts/core/archive-backlog-entry.sh --dry-run <entry>` stopped with `/c/Python314/python: Argument list too long` (line 98). The inbound-reference step passes every tracked and untracked `.md` file in the repo to the embedded Python program as arguments (`archive_py inbound-scan "$ENTRY" "$APPLIED" "${md_files[@]}"`). The repo has enough markdown files that the joined list is over the Windows command-line limit (32 767 characters). The entry was archived by hand instead, following the script's own steps (no inbound references, no relative links in the body).

## Proposed fix

Pass the file list on stdin, or as a NUL-separated list file, and have the Python program read it from there. Both `inbound-scan` and `inbound-fix` calls need it, including the residual re-scan before `git rm`. Add a bats case that runs the inbound scan with a file list longer than 32 767 characters.

**Enumerator + replay**: `git ls-files '*.md' | wc -c` on `develop` (33 710 on 2026-11-27, over the 32 767 limit), then the `--dry-run` command above in Git Bash.
