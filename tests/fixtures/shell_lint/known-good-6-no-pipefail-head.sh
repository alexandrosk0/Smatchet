#!/usr/bin/env bash
# Fixture: rule 6 NEGATIVE — the var=$(... | head) shape WITHOUT pipefail. With
# pipefail off the shell discards the upstream producer's SIGPIPE/non-zero exit
# (the pipeline's RC is head's, which is 0), so the risk the rule guards is
# absent and it must not fire.
set -eu
x=$(printf '%s\n' a b c | head -5)
echo "$x"
