# `dup_audit.py` flags shared include prologues, so every god-file split buys 4-5 exemptions

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-08-16
- **Observed on**: the full deviation re-evaluation, [`docs/audits/DEVIATION_AUDIT_2026-08-16.md`](../../../audits/DEVIATION_AUDIT_2026-08-16.md) § S6
- **Status**: open

## What happened

Of the 131 single-line `duplication` markers in `Source/`, **73 (55%)** sit above an `#include` /
`using` / `namespace` prologue, or say as much in their `reason=`. They are not exempting
copy-pasted logic; they are exempting the fact that four sibling TUs carved out of one god-file
necessarily open with the same include block.

Root cause is in the tokenizer, not the code: `agents/scripts/core/dup_audit.py` has no
include-block handling anywhere. `normalize_token()` maps identifiers to `ID` and passes
punctuation through, so `#include "AppControllerImpl.h"` tokenizes exactly like executable code. A
shared prologue of ~70+ tokens therefore clears `MIN_CLONE_TOKENS = 70` and reports as a
cross-file copy-paste clone.

The cost compounds: `god-file-splits` is an endorsed refactor, and each split adds one exemption
per sibling TU. The seven largest families in the tree today —
`CliCommandRunner` (5), `AppController_LuaBindings` (4), `ConfigManager` (4), `MarkdownConvert` (3),
`ActiveProjectGrid` (3), the `Scenarios/` TUs, the `Builtin/` command TUs — are all this one shape,
and each carries a `revisit=when a shared <X> TU prologue header is introduced` that nobody is
committed to landing.

`Source/Core/src/Ui/SmatchetUI_MainMenu.cpp:12` already names the real fix in its revisit:
`when the dup auditor scopes cross-file clones to logic blocks`. It has not fired.

## Why it matters

Every one of those 73 exemptions is a line of prose a reviewer must read and a future auditor must
re-evaluate, standing in for a tokenizer decision. It also inverts the gate's signal: a reviewer
who sees `SMATCHET_DEVIATION(rule=duplication)` at the top of a TU learns nothing, because half of
them mean "this file has includes".

## Concrete next action

Teach `dup_audit.py` to drop contiguous preprocessor runs before shingling: in `_tokens_with_lines`
(or a filter immediately after it), skip tokens whose source line's first non-space character is
`#`, and skip a leading `using`-declaration run at file scope. Enumerator for the verification
sweep: the 73 markers are exactly the `rule=duplication` markers in
`git ls-files 'Source/**' | grep -E '\.(cpp|h|hpp)$'` whose next non-blank line starts with
`#include`, `using`, or `namespace`. Replaying the motivating case: remove the marker at
`Source/Core/src/Config/ConfigManager_Load.cpp:18` and re-run `dup_audit.py --diff origin/develop`
— it must stay green, where today it FAILs on the `ConfigManager` prologue clone. Then retire the
73 in one sweep and drop the seven dead `when a shared <X> prologue header is introduced` triggers
with them.

Guard against over-correction: keep flagging a clone that merely *starts* in a prologue and
continues into real logic — skip the preprocessor tokens, do not skip the span that contains them.

Triggered-follow-up: when=pr-count:base=develop;since=2026-08-16;n=20; action=re-measure the include-prologue share of duplication exemptions; baseline=73 of 131 (55%) on 2026-08-16; fired=never
