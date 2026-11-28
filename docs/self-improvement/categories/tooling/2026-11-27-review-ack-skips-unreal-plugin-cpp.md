# The review-ack gate never treats Unreal plugin C++ as substantive

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-11-27
- **Observed on**: the #2500 fix in `Source/UnrealPlugins/SmatchetImGuiPlugin/Source/SmatchetImGuiPlugin/Private/` (branch `claude/batch28-unreal-pen`).
- **Status**: open

## What happened

`RA_CPP_GLOBS` in `agent-layer/agents/scripts/core/lib/review-ack.sh` lists `Source/Core`, `Source/Plugins`, `Source/Standalone` and `tests` only. A diff that changes only Unreal plugin C++ is therefore never substantive: pre-commit check B and `pre-ship.sh --ack-review` ask for no review artifact, and `pre-ship.sh --review-fingerprint` prints the sha256 of empty input (`e3b0c442…b855`), which a reviewer could stamp into `.review-findings.json` while it attests to nothing. The #2500 change was reviewed anyway (three skeptic lenses plus a gate code-review run), but nothing required it.

Two smaller gaps showed up in the same run: `--review-fingerprint` only prints the value although agent briefs describe it as writing `.review-findings.json`, and it fails hard (`cannot read …/review-ack.sh (incomplete checkout?)`) in a nested worktree whose `agent-layer` submodule was never initialized.

## Concrete next action

- Add `Source/UnrealPlugins/*.cpp` and `Source/UnrealPlugins/*.h` to `RA_CPP_GLOBS`, excluding `*/ThirdParty/*`, with a bats case for a plugin-only diff.
- Make `--review-fingerprint` warn (or exit non-zero) when the hashed diff is empty.
- Align the code-review agent brief with what `--review-fingerprint` does: it prints the fingerprint, and the reviewer writes the file.
