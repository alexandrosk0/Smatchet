#ifndef SMATCHET_ISSUE_TABLE_SERIALIZER_H
#define SMATCHET_ISSUE_TABLE_SERIALIZER_H

#include <string>
#include <vector>

#include "IssueDraft.h"
#include "LocalCacheManager.h"
#include "TrackerFieldSchema.h"

/**
 * Tabular (CSV/TSV) and JSON (de)serialization for issue drafts + cached tickets.
 *
 * - Import: parses text from a file or clipboard into a vector of IssueDraft.
 *   Header row is interpreted as field IDs first; if a column isn't a known
 *   field id, it falls back to the catalog display name (case-insensitive) and
 *   then to a small set of special keys ("project", "issuetype", "parent",
 *   "attachments"). A "key" / "issue key" column sets ExistingIssueKey; bulk
 *   create pipeline then updates that issue instead of POSTing a new one.
 *   JSON import accepts either an array of objects or {"issues": [...]} wrappers.
 *   Per-row errors are collected but do not abort the whole batch.
 *
 * - Export: writes tickets to CSV/TSV/JSON using the provided field ids as
 *   columns. Missing values render as empty strings.
 */
namespace IssueTableSerializer {

enum class Format { Auto, Csv, Tsv, Json };

struct ImportRow {
    IssueDraft Draft;
    int SourceLine = 0;
    std::string Error;
};

struct ImportResult {
    std::vector<ImportRow> Rows;
    Format DetectedFormat = Format::Auto;
    std::string Error;
};

/**
 * Cheap format sniffer: JSON if starts with [ or {, TSV if header contains
 * tabs, CSV otherwise.
 */
Format DetectFormat(const std::string& text);

ImportResult ParseDrafts(const std::string& text, Format fmt, const std::vector<TrackerField>& catalog,
                         const std::string& fallbackProjectKey, const std::string& fallbackIssueTypeId,
                         const std::string& fallbackIssueTypeName);

/**
 * Serialize tickets to `fmt`. `fieldIds` = column order; if empty, uses union
 * of keys across all tickets (sorted for stability).
 */
std::string SerializeTickets(const std::vector<CachedTicket>& tickets, const std::vector<std::string>& fieldIds,
                             Format fmt);

} // namespace IssueTableSerializer

#endif
