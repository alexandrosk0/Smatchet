#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

/// Markdown <-> Atlassian Document Format (ADF) and Markdown <-> HTML conversion for the
/// rich-text editing pipeline. Backs the long-text modal editor used by Jira description /
/// environment fields (ADF) and Plane description (HTML). See RICH_TEXT_EDITING_V2_PLAN.md
/// for the broader v2 design and the supported subset of constructs.
namespace MarkdownConvert {

/// Parse Markdown into Atlassian Document Format JSON.
/// Top-level shape: `{type:"doc", version:1, content:[...]}`.
///
/// Supported block types: paragraph, heading (1-6), bulletList, orderedList, listItem,
/// codeBlock (with optional language), blockquote, rule (horizontal rule), hardBreak.
/// Supported inline marks: strong, em, code, link, strike.
///
/// NOT supported (silently dropped): tables, images, mentions, smart links, panels, raw HTML.
/// The modal editor surfaces an "unrepresentable content" warning when these are detected
/// in the inverse direction.
nlohmann::json MarkdownToAdf(const std::string& md);

/// Walk an ADF document and emit Markdown for the supported subset (mirror of MarkdownToAdf).
/// `outDroppedNodeTypes`, if non-null, receives the `type` of every ADF node that was skipped
/// because it isn't representable in our Markdown subset (caller can show a "raw mode" banner).
std::string AdfToMarkdown(const nlohmann::json& adf, std::vector<std::string>* outDroppedNodeTypes = nullptr);

/// Parse Markdown into HTML covering the subset Plane's `description_html` accepts:
/// `<p>`, `<h1>`-`<h6>`, `<strong>`, `<em>`, `<code>`, `<pre><code>`, `<ul>`, `<ol>`, `<li>`,
/// `<a>`, `<br>`, `<hr>`, `<blockquote>`, `<s>`. Output is XHTML-style (self-closed void tags).
std::string MarkdownToHtml(const std::string& md);

/// Best-effort HTML-subset to Markdown conversion. Targets the tag set Plane emits in
/// `description_html` (and the same set MarkdownToHtml produces).
///
/// `outFellBack` (if non-null) is set to true when the input contains tags or attributes
/// outside the recognized allowlist — the caller should switch the modal to raw-mode and
/// preserve the original HTML verbatim instead of trusting the lossy conversion.
std::string HtmlSubsetToMarkdown(const std::string& html, bool* outFellBack = nullptr);

} // namespace MarkdownConvert
