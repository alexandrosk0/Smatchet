#!/usr/bin/env bash
# Fixture: rule 4 — curl writes to file without sha256 verify within 10 lines.
set -euo pipefail
command -v curl >/dev/null 2>&1 || exit 2
curl -fsSL "https://example.com/x.bin" -o /tmp/x.bin
echo "downloaded but no checksum"
