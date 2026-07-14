#pragma once

// Build-time win: json appears here only as a by-value return-type declaration
// (MarkdownToAdf) and a by-const-reference parameter (AdfToMarkdown) — neither
// needs the complete type in the header. MarkdownConvert.cpp includes the full
// <nlohmann/json.hpp> to build/walk the ADF tree.
#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

/// Markdown <-> Atlassian Document Format (ADF) and Markdown <-> HTML conversion for the
/// rich-text editing pipeline. Backs the long-text modal editor used by Jira description /
/// environment fields (ADF) and Plane description (HTML). See docs/plans/rich-text-editing-v2-remaining.md
/// for the broader v2 design and the supported subset of constructs.
namespace MarkdownConvert {

/// Bitmask passed to md4c (`MD_FLAG_*`). Single source of truth for the rich-text pipeline
/// and the Markdown preview (keeps parser behavior aligned).
unsigned Md4cParserFlags() noexcept;

/// Parse Markdown into Atlassian Document Format JSON.
/// Top-level shape: `{type:"doc", version:1, content:[...]}`.
/// **Markdown → ADF:** paragraph, heading (1-6), bulletList, orderedList, listItem (incl.
/// task checkbox prefix in text), codeBlock, blockquote, rule, hardBreak, table
/// (tableRow / tableHeader / tableCell), inline images as `mediaInline` (external URL).
/// **ADF → Markdown:** mirrors the above; also emits mentions, `inlineCard` (as URL text),
/// `media` / `mediaInline` / `mediaSingle`, and tables as pipe Markdown. Nodes with no
/// Markdown equivalent are listed in `outDroppedNodeTypes` when using `AdfToMarkdown`.
/// Raw HTML in Markdown is not parsed as HTML (md4c `NOHTML`); it becomes plain text.
nlohmann::json MarkdownToAdf(const std::string& md);

/// Walk an ADF document and emit Markdown for the supported subset (mirror of MarkdownToAdf).
/// `outDroppedNodeTypes`, if non-null, receives the `type` of every ADF node that was skipped
/// because it isn't representable in our Markdown subset (caller can show a "raw mode" banner).
std::string AdfToMarkdown(const nlohmann::json& adf, std::vector<std::string>* outDroppedNodeTypes = nullptr);

/// Parse Markdown into HTML covering the subset Plane's `description_html` is expected to
/// tolerate (verify against live sanitizer): `<p>`, `<h1>`-`<h6>`, `<strong>`, `<em>`,
/// `<code>`, `<pre><code>`, `<ul>`, `<ol>`, `<li>`, `<a>`, `<br>`, `<hr>`, `<blockquote>`,
/// `<s>`, `<table>`, `<thead>`, `<tbody>`, `<tr>`, `<th>`, `<td>`, `<img>`. Output is
/// XHTML-style (self-closed void tags where applicable).
std::string MarkdownToHtml(const std::string& md);

/// Best-effort HTML-subset to Markdown conversion. Targets the tag set Plane emits in
/// `description_html` (and the same set MarkdownToHtml produces).
/// `outFellBack` (if non-null) is set to true when the input contains tags or attributes
/// outside the recognized allowlist — the caller should switch the modal to raw-mode and
/// preserve the original HTML verbatim instead of trusting the lossy conversion.
std::string HtmlSubsetToMarkdown(const std::string& html, bool* outFellBack = nullptr);

} // namespace MarkdownConvert
