- 2026-11-16 · input-ux-w2-text-field · [debt] · P2 — Dictation cannot target a std::string field, so composers stay on fixed buffers

Details:
`DictationInsertionRouter` splices dictated text into a registered `char*` with a registered capacity and
keeps a sticky shadow of that pointer (`DictationInsertionRouter.h`, CPP_CODE_AUDIT #21). A `std::string`
moves its storage when it grows, so registering one would let the shadow splice into freed memory.
`SmatchetInput::Text` and `Multiline` therefore register nothing, and `SmatchetInput::TextBuf` (fixed
buffer) keeps the hook. The comment box, worklog description, AI composer, Quick Create and new-issue
description take dictation today and so were not moved to `std::string` by the shared text field
(INPUT_UX_AUDIT.md #2, #102).

Concrete next action:
Give `IDictationHost` / `DictationInsertionRouter` a string target: register a `std::string*` plus its item id
and splice by `resize` + `memmove`, re-reading the string every time (never caching `c_str()`), and unregister
it when the widget is blurred. Then let `SmatchetInput::Text` and `Multiline` register their string, and
move the composers. Cover it in `tests/Core/DictationInsertionRouter.test.cpp` (including a string that
grows between register and splice) and the AI-assistant dictation bucket-E test.

Status: open
Last-reviewed: 2026-11-16
