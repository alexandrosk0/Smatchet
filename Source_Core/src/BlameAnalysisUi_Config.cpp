#include "BlameAnalysisUi_Internal.h"

#include "AppController.h"
#include "CompactDateFormat.h"
#include "ConfigManager.h"
#include "JiraClient.h"
#include "Logger.h"
#include "StringUtil.h"
#include "TrackerFieldSchema.h"

#include <algorithm>

namespace BlameInternal {

void LogBlameP4PathsIfChanged(const char* reason) {
    auto& st = State();
    if (st.blameCfg.P4Executable != st.loggedP4Exe) {
        LOG_INFO("Blame [%s]: p4_exe \"%s\" -> \"%s\"", reason, st.loggedP4Exe.c_str(),
                 st.blameCfg.P4Executable.c_str());
        st.loggedP4Exe = st.blameCfg.P4Executable;
    }
    if (st.blameCfg.P4VcExecutable != st.loggedP4vcExe) {
        LOG_INFO("Blame [%s]: p4vc_exe \"%s\" -> \"%s\"", reason, st.loggedP4vcExe.c_str(),
                 st.blameCfg.P4VcExecutable.c_str());
        st.loggedP4vcExe = st.blameCfg.P4VcExecutable;
    }
}

void SyncCallstackTrackerFieldBufFromCfg() {
    CopyToBuffer(State().callstackTrackerFieldBuf, State().blameCfg.CallstackTrackerFieldId);
}

void SyncJiraBlameAuxFieldBufsFromCfg() {
    CopyToBuffer(State().lastFoundClFieldBuf, State().blameCfg.LastFoundClTrackerFieldId);
    CopyToBuffer(State().lastOccurrencesFieldBuf, State().blameCfg.LastOccurrencesTrackerFieldId);
}

void HydrateBlameCfgDiskOnce() {
    if (State().blameCfgDiskHydrated) {
        return;
    }
    State().blameCfg = ConfigManager::LoadBlameAnalysis();
    SyncCallstackTrackerFieldBufFromCfg();
    SyncJiraBlameAuxFieldBufsFromCfg();
    State().blameCfgDiskHydrated = true;
}

void MaybeAutoselectCallstackTrackerField(const AppController& app) {
    if (!State().blameCfg.CallstackTrackerFieldId.empty()) {
        return;
    }
    const auto& fields = app.GetAvailableFields();
    if (fields.empty()) {
        return;
    }
    auto it = std::find_if(fields.begin(), fields.end(), [](const auto& f) {
        return ToLowerAsciiCopy(f.Name) == "callstack";
    });
    if (it != fields.end()) {
        State().blameCfg.CallstackTrackerFieldId = it->Id;
        SyncCallstackTrackerFieldBufFromCfg();
        ConfigManager::SaveBlameAnalysis(State().blameCfg);
    }
}

void MaybeAutoselectLastFoundClTrackerField(const AppController& app) {
    if (!State().blameCfg.LastFoundClTrackerFieldId.empty()) {
        return;
    }
    const auto& fields = app.GetAvailableFields();
    if (fields.empty()) {
        return;
    }
    const auto it = std::find_if(fields.begin(), fields.end(), [](const TrackerField& f) {
        const std::string n = ToLowerAsciiCopy(f.Name);
        return n == "last found cl" || n == "lastfoundcl" || n == "last_found_cl";
    });
    if (it != fields.end()) {
        State().blameCfg.LastFoundClTrackerFieldId = it->Id;
        SyncJiraBlameAuxFieldBufsFromCfg();
        ConfigManager::SaveBlameAnalysis(State().blameCfg);
    }
}

void MaybeAutoselectLastOccurrencesTrackerField(const AppController& app) {
    if (!State().blameCfg.LastOccurrencesTrackerFieldId.empty()) {
        return;
    }
    const auto& fields = app.GetAvailableFields();
    if (fields.empty()) {
        return;
    }
    const auto it = std::find_if(fields.begin(), fields.end(), [](const TrackerField& f) {
        const std::string n = ToLowerAsciiCopy(f.Name);
        return n == "last occurrences" || n == "last occurances" || n == "last occurences" || n == "last_occurrences";
    });
    if (it != fields.end()) {
        State().blameCfg.LastOccurrencesTrackerFieldId = it->Id;
        SyncJiraBlameAuxFieldBufsFromCfg();
        ConfigManager::SaveBlameAnalysis(State().blameCfg);
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

void TryFillBeforeChangelistAndDateFromJira(const AppController& app, const std::string& issueKey) {
    if (issueKey.empty()) {
        State().beforeDateIso.clear();
        return;
    }
    const auto ticketsSnap = app.GetActiveTicketsSnapshot();
    for (const auto& t : *ticketsSnap) {
        if (t.id != issueKey) {
            continue;
        }
        if (!State().blameCfg.LastFoundClTrackerFieldId.empty()) {
            const std::string cl =
                SanitizeChangelistDigitsFromField(t.GetFieldValue(State().blameCfg.LastFoundClTrackerFieldId));
            CopyToBuffer(State().atClBuf, cl);
        }
        if (!State().blameCfg.LastOccurrencesTrackerFieldId.empty()) {
            State().beforeDateIso =
                NormalizeJiraDateForBeforePicker(t.GetFieldValue(State().blameCfg.LastOccurrencesTrackerFieldId));
        } else {
            State().beforeDateIso.clear();
        }
        return;
    }
    State().beforeDateIso.clear();
    if (!State().blameCfg.LastFoundClTrackerFieldId.empty()) {
        State().atClBuf[0] = '\0';
    }
}

void TryFillCallstackFromJira(const AppController& app, const std::string& issueKey) {
    if (State().blameCfg.CallstackTrackerFieldId.empty() || issueKey.empty()) {
        return;
    }
    const auto ticketsSnap = app.GetActiveTicketsSnapshot();
    for (const auto& t : *ticketsSnap) {
        if (t.id != issueKey) {
            continue;
        }
        const std::string v = t.GetFieldValue(State().blameCfg.CallstackTrackerFieldId);
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

} // namespace BlameInternal
