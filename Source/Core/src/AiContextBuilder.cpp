#include "AiContextBuilder.h"

#include "AiXmlAttrEscape.h"
#include "BackendAuditTrail.h"
#include "CachedTicketTypes.h"
#include "ConfigManager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace AiContextBuilder {

namespace {

const char* BlockName(AiContextBlockKind k) {
    switch (k) {
    case AiContextBlockKind::MultiSelectedTickets:
        return "selected_tickets";
    case AiContextBlockKind::VisibleGridRows:
        return "visible_rows";
    case AiContextBlockKind::ActiveTicket:
        return "active_ticket";
    case AiContextBlockKind::ActiveView:
        return "active_view";
    case AiContextBlockKind::AuditTrail:
        return "audit_trail";
    }
    return "unknown";
}

std::string TicketSummaryLine(const CachedTicket& t) {
    // Prefer canonical fields when present. Falls back to id only when the cached
    // payload is sparse (e.g. a brand-new ticket inserted before fields hydrated).
    std::string key = t.id;
    std::string summary = t.GetFieldValue("summary");
    if (summary.empty()) {
        // Plane uses `name` in some catalog responses; tolerate either.
        summary = t.GetFieldValue("name");
    }
    std::string status = t.GetFieldValue("status");
    std::string line = "- ";
    line.append(key);
    if (!summary.empty()) {
        line.append(": ");
        line.append(summary);
    }
    if (!status.empty()) {
        line.append(" [");
        line.append(status);
        line.append("]");
    }
    return line;
}

void AppendBlock(std::string& out, const AiContextBlock& block) {
    if (block.Body.empty()) {
        return;
    }
    if (!out.empty()) {
        out.push_back('\n');
    }
    out.append("<smatchet_context block=\"");
    out.append(block.Name);
    out.append("\">\n");
    // Same breakout guard as ComposeSystemPrompt — bodies are tracker-influenceable.
    const std::string body = smatchet::ai::pure::NeutralizeContextBody(block.Body);
    out.append(body);
    if (!body.empty() && body.back() != '\n') {
        out.push_back('\n');
    }
    out.append("</smatchet_context>\n");
}

AiContextBlock MakeBlock(AiContextBlockKind kind, std::string body) {
    AiContextBlock b;
    b.Kind = kind;
    b.Name = BlockName(kind);
    b.Body = std::move(body);
    return b;
}

} // namespace

std::string BuildSelectionBody(const std::vector<CachedTicket>& tickets, const std::vector<std::size_t>& sortedIndices,
                               const std::set<int>& selectedRows) {
    if (selectedRows.empty() || sortedIndices.empty() || tickets.empty()) {
        return std::string();
    }
    std::string out;
    std::size_t emitted = 0;
    // `selectedRows` is `std::set<int>` so iteration is naturally sorted ascending —
    // that preserves the sort order shown in the grid (RectSel.Rows holds sort-order
    // indices). Cap at kRowsCap to bound the system-prompt size per plan rule.
    for (int row : selectedRows) {
        if (row < 0) {
            continue;
        }
        const std::size_t rowIdx = static_cast<std::size_t>(row);
        if (rowIdx >= sortedIndices.size()) {
            continue;
        }
        const std::size_t ticketIdx = sortedIndices[rowIdx];
        if (ticketIdx >= tickets.size()) {
            continue;
        }
        if (emitted >= kRowsCap) {
            break;
        }
        out.append(TicketSummaryLine(tickets[ticketIdx]));
        out.push_back('\n');
        ++emitted;
    }
    if (emitted == 0) {
        return std::string();
    }
    return out;
}

std::string BuildVisibleRowsBody(const std::vector<CachedTicket>& tickets,
                                 const std::vector<std::size_t>& visibleRows) {
    if (visibleRows.empty() || tickets.empty()) {
        return std::string();
    }
    std::string out;
    std::size_t emitted = 0;
    for (std::size_t i = 0; i < visibleRows.size(); ++i) {
        const std::size_t ticketIdx = visibleRows[i];
        if (ticketIdx >= tickets.size()) {
            continue;
        }
        if (emitted >= kRowsCap) {
            break;
        }
        out.append(TicketSummaryLine(tickets[ticketIdx]));
        out.push_back('\n');
        ++emitted;
    }
    if (emitted == 0) {
        return std::string();
    }
    return out;
}

std::string BuildActiveTicketBody(const CachedTicket* found) {
    if (!found) {
        return std::string();
    }
    std::string out;
    out.append("id: ").append(found->id).push_back('\n');
    // Emit canonical scalar fields in a stable order. The full field map would balloon
    // the prompt unbounded — the AI panel can ask for specific fields if it needs them.
    static const char* const kPrimaryFieldKeys[] = {"summary",  "name",     "status",   "issuetype",
                                                    "priority", "assignee", "reporter", "labels"};
    for (const char* key : kPrimaryFieldKeys) {
        const std::string val = found->GetFieldValue(key);
        if (val.empty()) {
            continue;
        }
        out.append(key).append(": ").append(val);
        out.push_back('\n');
    }
    return out;
}

std::string BuildActiveTicketBody(const std::vector<CachedTicket>& tickets, const std::string& activeIssueId) {
    if (activeIssueId.empty() || tickets.empty()) {
        return std::string();
    }
    // O(n) fallback path: linear scan by id. Callers with an id→index map should
    // resolve the pointer themselves and use the pointer overload to skip this.
    const auto it =
        std::find_if(tickets.begin(), tickets.end(), [&](const CachedTicket& t) { return t.id == activeIssueId; });
    const CachedTicket* found = (it == tickets.end()) ? nullptr : &(*it);
    return BuildActiveTicketBody(found);
}

std::string BuildActiveViewBody(const ViewDefinition* view) {
    if (!view) {
        return std::string();
    }
    std::string out;
    if (!view->Name.empty()) {
        out.append("name: ").append(view->Name).push_back('\n');
    }
    if (!view->Id.empty()) {
        out.append("id: ").append(view->Id).push_back('\n');
    }
    if (!view->Jql.empty()) {
        out.append("query: ").append(view->Jql).push_back('\n');
    }
    if (!view->ColumnOrder.empty()) {
        out.append("columns:");
        for (const std::string& col : view->ColumnOrder) {
            out.push_back(' ');
            out.append(col);
        }
        out.push_back('\n');
    }
    return out;
}

std::string BuildAuditTrailBody(const std::vector<std::string>& recentJsonLines) {
    if (recentJsonLines.empty()) {
        return std::string();
    }
    std::string out;
    std::size_t emitted = 0;
    // Most-recent-first per plan rule.
    for (std::size_t i = recentJsonLines.size(); i-- > 0;) {
        if (emitted >= kAuditCap) {
            break;
        }
        out.append("- ").append(recentJsonLines[i]).push_back('\n');
        ++emitted;
    }
    if (emitted == 0) {
        return std::string();
    }
    return out;
}

std::vector<AiContextBlock> BuildAll(const Inputs& in) {
    std::vector<AiContextBlock> out;
    out.reserve(5);

    const std::vector<CachedTicket>* ticketsPtr = in.Tickets ? in.Tickets.get() : nullptr;
    static const std::vector<CachedTicket> kEmptyTickets;
    const std::vector<CachedTicket>& tickets = ticketsPtr ? *ticketsPtr : kEmptyTickets;

    // Block 0 — Selection.
    {
        std::string body;
        if (in.EnableSelection && in.SortedIndices && in.SelectedRows) {
            body = BuildSelectionBody(tickets, *in.SortedIndices, *in.SelectedRows);
        }
        out.push_back(MakeBlock(AiContextBlockKind::MultiSelectedTickets, std::move(body)));
    }
    // Block 1 — VisibleRows.
    {
        std::string body;
        if (in.EnableVisibleRows && in.VisibleRows) {
            body = BuildVisibleRowsBody(tickets, *in.VisibleRows);
        }
        out.push_back(MakeBlock(AiContextBlockKind::VisibleGridRows, std::move(body)));
    }
    // Block 2 — ActiveTicket.
    {
        std::string body;
        if (in.EnableActiveTicket) {
            if (in.PreResolvedActiveTicket && in.PreResolvedActiveTicket->id == in.ActiveIssueId) {
                // O(1) fast path: caller resolved the active ticket via its IdIndex.
                // Guarded so a stale pre-resolved pointer (id != ActiveIssueId) never
                // surfaces the wrong ticket — on mismatch fall back to the scan.
                body = BuildActiveTicketBody(in.PreResolvedActiveTicket);
            } else {
                body = BuildActiveTicketBody(tickets, in.ActiveIssueId);
            }
        }
        out.push_back(MakeBlock(AiContextBlockKind::ActiveTicket, std::move(body)));
    }
    // Block 3 — ActiveView.
    {
        std::string body;
        if (in.EnableActiveView) {
            body = BuildActiveViewBody(in.ActiveView);
        }
        out.push_back(MakeBlock(AiContextBlockKind::ActiveView, std::move(body)));
    }
    // Block 4 — AuditTrail.
    {
        std::string body;
        if (in.EnableAuditTrail) {
            if (in.DeferAuditTrailFetch) {
                // UX pillar 2: BackendAuditTrail::ReadRecentEvents performs a
                // synchronous SQLite + filesystem read that must not run on the
                // UI thread. Emit a sentinel body; the worker thread
                // (AiAssistantController::RunRequest) replaces it with the real
                // body before the request is dispatched.
                body = kDeferredAuditTrailSentinel;
            } else {
                // ReadRecentEvents returns JSON objects; serialise back to strings so the
                // builder stays free of UI / SQLite / dispatcher coupling. Cap is also
                // applied inside BuildAuditTrailBody.
                std::vector<nlohmann::json> events = BackendAuditTrail::ReadRecentEvents(kAuditCap).value_or({});
                std::vector<std::string> lines;
                lines.reserve(events.size());
                std::transform(events.begin(), events.end(), std::back_inserter(lines),
                               [](const nlohmann::json& ev) { return ev.dump(); });
                body = BuildAuditTrailBody(lines);
            }
        }
        out.push_back(MakeBlock(AiContextBlockKind::AuditTrail, std::move(body)));
    }
    return out;
}

std::string MergeEnabled(const Inputs& in) {
    const std::vector<AiContextBlock> blocks = BuildAll(in);
    std::string out;
    for (const AiContextBlock& b : blocks) {
        AppendBlock(out, b);
    }
    return out;
}

} // namespace AiContextBuilder
