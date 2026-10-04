- 2026-10-03 · deviation renewal (markers expiring 2026-12-31) · [debt] · P3 — the HTML and ADF Markdown engines each keep their own copy of the image-span and text-skip logic

Details:
md4c reports an image's alt text as ordinary text events between the image span's enter and leave
callbacks, so both engines collect it instead of emitting it. `MarkdownToHtml.cpp` and
`MarkdownToAdf.cpp` each repeat that logic:
- the `MD_SPAN_IMG` enter (push the src, raise the depth, clear the alt buffer);
- the leave tail (pop the src, clear the alt, lower the depth);
- the text-callback preamble (skip NUL chars, skip and log once on raw HTML under `MD_FLAG_NOHTML`,
  route text inside an image span into the alt buffer).

`dup_audit.py` reports these as clones, exempted by three markers in `MarkdownToHtml.cpp`. The markers
used to say that folding would couple two independent engines. It would not: both engines already
share `MarkdownConvert_Internal.h`, which holds their builder structs and `MdAttrToString`.

Concrete next action:
In `MarkdownConvert_Internal.h`:
- Add a `MdImageSpan` struct (depth, alt, src stack) and give both builders one in place of
  `imgSpanDepth` / `imgAltBuf` / `imgAltAccum` / `imgSrcStack`.
- Add inline helpers to enter an image span, pop its src, leave it, and absorb alt text, plus one for
  the NUL/raw-HTML skip that takes the engine name for its debug log.

Then move both engines onto the helpers and run the MarkdownConvert tests. Delete the three exemptions
(`revisit=2027-08-31`).

Status: open
Last-reviewed: 2026-10-03
