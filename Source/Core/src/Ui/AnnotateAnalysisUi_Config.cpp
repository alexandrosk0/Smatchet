#include "AnnotateAnalysisUi_Internal.h"

#include "CachedTicketTypes.h"
#include "CompactDateFormat.h"
#include "Interfaces/IAppTicketData.h"
#include "ConfigManager.h"
#include "JiraClient.h"
#include "Logger.h"
#include "SmatchetFieldRender.h"
#include "StringUtil.h"
#include "TrackerFieldSchema.h"

#include "ConfigSaveWorker.h"

#include <algorithm>

namespace AnnotateInternal {

// Hand the Annotate config write to the single coalescing config-save worker (snapshot by value,
// latest-per-kind wins, serialized + off the UI thread). ConfigManager::SaveAnnotateAnalysis does a
// synchronous JSON encode + atomic file replace; even an edit-commit can breach the 6.94 ms UI budget
// on a slow disk. Replaces the former per-save detached-thread shim now that the shared worker exists.
void ScheduleAnnotateConfigSaveDetached(const AnnotateAnalysisConfig& cfg) {
    smatchet::config_save::EnqueueAnnotateConfig(cfg);
}

void LogAnnotateP4PathsIfChanged(const char* reason) {
    auto& st = State();
    if (st.annotateCfg.P4Executable != st.loggedP4Exe) {
        LOG_INFO("Annotate [%s]: p4_exe \"%s\" -> \"%s\"", reason, st.loggedP4Exe.c_str(),
                 st.annotateCfg.P4Executable.c_str());
        st.loggedP4Exe = st.annotateCfg.P4Executable;
    }
    if (st.annotateCfg.P4VcExecutable != st.loggedP4vcExe) {
        LOG_INFO("Annotate [%s]: p4vc_exe \"%s\" -> \"%s\"", reason, st.loggedP4vcExe.c_str(),
                 st.annotateCfg.P4VcExecutable.c_str());
        st.loggedP4vcExe = st.annotateCfg.P4VcExecutable;
    }
}

void HydrateAnnotateCfgDiskOnce() {
    if (State().annotateCfgDiskHydrated) {
        return;
    }
    // PILLAR2_INLINE // est-latency: 0.54ms (measured, small whole-file JSON read+parse of
    // smatchet_config.json; once-guarded). Per the inline-vs-async hydration policy
    // (SmatchetAiAssistantUi.cpp), a one-time read this small stays synchronous — a sub-ms
    // first-frame cost beats the complexity/latency of a background-thread + dispatcher round-trip.
    State().annotateCfg = ConfigManager::LoadAnnotateAnalysis();
    State().annotateCfgDiskHydrated = true;
    SetCallstackFieldIdHint(State().annotateCfg.CallstackTrackerFieldId);
}

namespace {

// Shared body for the three autoselect-Jira-field helpers: if the target field id is
// unset and a field whose (lowercased) name satisfies `nameMatches` exists, adopt its id,
// persist, and optionally refresh the callstack-field hint. `nameMatches` receives the
// already-lowercased field name. `fields` is the app-owned field catalog, extracted by
// callers that hold the full app object.
void MaybeAutoselectTrackerField(const std::vector<TrackerField>& fields, std::string& targetFieldId,
                                 bool (*nameMatches)(const std::string&), bool updateCallstackHint) {
    if (!targetFieldId.empty()) {
        return;
    }
    if (fields.empty()) {
        return;
    }
    const auto it = std::find_if(fields.begin(), fields.end(), [nameMatches](const TrackerField& f) {
        return nameMatches(ToLowerAsciiCopy(f.Name));
    });
    if (it != fields.end()) {
        targetFieldId = it->Id;
        ScheduleAnnotateConfigSaveDetached(State().annotateCfg);
        if (updateCallstackHint) {
            SetCallstackFieldIdHint(targetFieldId);
        }
    }
}

} // namespace

void MaybeAutoselectCallstackTrackerField(const std::vector<TrackerField>& availableFields) {
    MaybeAutoselectTrackerField(
        availableFields, State().annotateCfg.CallstackTrackerFieldId,
        [](const std::string& n) { return n == "callstack"; },
        /*updateCallstackHint=*/true);
}

void MaybeAutoselectLastFoundClTrackerField(const std::vector<TrackerField>& availableFields) {
    MaybeAutoselectTrackerField(
        availableFields, State().annotateCfg.LastFoundClTrackerFieldId,
        [](const std::string& n) { return n == "last found cl" || n == "lastfoundcl" || n == "last_found_cl"; },
        /*updateCallstackHint=*/false);
}

void MaybeAutoselectLastOccurrencesTrackerField(const std::vector<TrackerField>& availableFields) {
    // Defensive misspelling matches ("last occurances" / "last occurences") intentionally
    // kept — they match real, badly-named Jira fields, not our own strings.
    MaybeAutoselectTrackerField(
        availableFields, State().annotateCfg.LastOccurrencesTrackerFieldId,
        [](const std::string& n) {
            return n == "last occurrences" || n == "last occurances" || n == "last occurences" ||
                   n == "last_occurrences";
        },
        /*updateCallstackHint=*/false);
}

void ApplyShowRawCallstack(bool show) {
    State().showRaw = show;
    if (State().annotateCfg.ShowRawCallstack != show) {
        State().annotateCfg.ShowRawCallstack = show;
        ScheduleAnnotateConfigSaveDetached(State().annotateCfg);
    }
}

namespace {

std::string SanitizeChangelistDigitsFromField(const std::string& raw) {
    const std::string v = TrimCopy(raw);
    if (v.empty()) {
        return std::string();
    }
    size_t end = v.size();
    const size_t nl = v.find_first_of("\r\n");
    if (nl != std::string::npos) {
        end = nl;
    }
    std::string digits;
    for (size_t i = 0; i < end; ++i) {
        const char c = v[i];
        if (c >= '0' && c <= '9') {
            digits += c;
        } else if (!digits.empty()) {
            break;
        }
    }
    return digits;
}

std::string NormalizeJiraDateForBeforePicker(const std::string& raw) {
    const std::string v = TrimCopy(raw);
    if (v.empty()) {
        return std::string();
    }
    ParsedJiraDateTime p;
    if (TryParseJiraDateTime(v, p)) {
        return FormatJiraDateOrDateTimeForApi(true, p);
    }
    return std::string();
}

} // namespace

void TryFillBeforeChangelistAndDateFromJira(const IAppTicketData& ticketData, const std::string& issueKey) {
    if (issueKey.empty()) {
        State().beforeDateIso.clear();
        return;
    }
    const auto ticketsSnap = ticketData.GetActiveTicketsSnapshot();
    for (const auto& t : *ticketsSnap) {
        if (t.id != issueKey) {
            continue;
        }
        if (!State().annotateCfg.LastFoundClTrackerFieldId.empty()) {
            const std::string cl =
                SanitizeChangelistDigitsFromField(t.GetFieldValue(State().annotateCfg.LastFoundClTrackerFieldId));
            CopyToBuffer(State().atClBuf, cl);
        }
        if (!State().annotateCfg.LastOccurrencesTrackerFieldId.empty()) {
            State().beforeDateIso =
                NormalizeJiraDateForBeforePicker(t.GetFieldValue(State().annotateCfg.LastOccurrencesTrackerFieldId));
        } else {
            State().beforeDateIso.clear();
        }
        return;
    }
    State().beforeDateIso.clear();
    if (!State().annotateCfg.LastFoundClTrackerFieldId.empty()) {
        State().atClBuf[0] = '\0';
    }
}

void TryFillCallstackFromJira(const IAppTicketData& ticketData, const std::string& issueKey) {
    if (State().annotateCfg.CallstackTrackerFieldId.empty() || issueKey.empty()) {
        return;
    }
    const auto ticketsSnap = ticketData.GetActiveTicketsSnapshot();
    for (const auto& t : *ticketsSnap) {
        if (t.id != issueKey) {
            continue;
        }
        const std::string v = t.GetFieldValue(State().annotateCfg.CallstackTrackerFieldId);
        if (v.empty()) {
            return;
        }
        CopyToBuffer(State().callstackBuf, v);
        return;
    }
}

std::vector<std::string> SplitIgnoreKeywords(const std::string& multi) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : multi) {
        if (c == ',' || c == '\n' || c == '\r') {
            while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t')) {
                cur.pop_back();
            }
            size_t i = 0;
            while (i < cur.size() && (cur[i] == ' ' || cur[i] == '\t')) {
                ++i;
            }
            if (i < cur.size()) {
                out.push_back(cur.substr(i));
            }
            cur.clear();
        } else {
            cur += c;
        }
    }
    while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t')) {
        cur.pop_back();
    }
    size_t i = 0;
    while (i < cur.size() && (cur[i] == ' ' || cur[i] == '\t')) {
        ++i;
    }
    if (i < cur.size()) {
        out.push_back(cur.substr(i));
    }
    return out;
}

} // namespace AnnotateInternal
