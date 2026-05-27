#include "BlameAnalysisUi_Internal.h"

#include "CallstackParser.h"
#include "Logger.h"
#include "P4Blame.h"

#include <chrono>
#include <utility>

namespace BlameInternal {

void JoinWorkerIfNeeded() {
    if (State().worker.Thread.joinable() && !State().worker.Running.load()) {
        State().worker.Thread.join();
    }
}

void MirrorWorkerToDisplay() {
    std::lock_guard<std::mutex> lkW(State().worker.Mutex);
    if (State().worker.Rows.empty() && !State().worker.Running.load()) {
        return;
    }
    std::lock_guard<std::mutex> lkD(State().displayMutex);
    State().displayRows = State().worker.Rows;
}

void ResetDetailAfterRunComplete() {
    if (!State().worker.PendingPublish.exchange(false)) {
        return;
    }
    std::lock_guard<std::mutex> lkD(State().displayMutex);
    const size_t n = State().displayRows.size();
    State().detailFuts.assign(n, std::shared_future<DetailPack>());
    State().detailPhase.assign(n, 0);
    State().detailData.assign(n, DetailPack{});
    State().detailScrolled.assign(n, false);
}

void WorkerThreadMain(size_t n) {
    BlameAnalysisConfig cfg = State().worker.Cfg;
    const std::string atCl = State().worker.AtChangelist;
    P4ChangelistDescribeCache* cache = State().worker.Cache.get();
    LOG_INFO("Blame worker: started rows=%zu atCl=\"%s\" p4_exe=\"%s\"", n, atCl.c_str(), cfg.P4Executable.c_str());
    try {
        int failures = 0;
        for (size_t i = 0; i < n; ++i) {
            if (State().worker.Cancel.load()) {
                LOG_INFO("Blame worker: cancelled at row %zu/%zu (failures=%d)", i, n, failures);
                break;
            }
            BlameRow row;
            {
                std::lock_guard<std::mutex> lk(State().worker.Mutex);
                if (i >= State().worker.Rows.size()) {
                    break;
                }
                row = State().worker.Rows[i];
            }
            P4LineBlame b = P4BlameLine(cfg, row.PathForP4, row.Parsed.LineNumber, atCl);
            if (!b.Error.empty()) {
                LOG_WARN("Blame worker: row=%zu path=%s line=%d err=%s", i + 1, row.PathForP4.c_str(),
                         row.Parsed.LineNumber, b.Error.c_str());
                ++failures;
            }
            if (!b.Changelist.empty() && cache) {
                P4ChangelistDetails d = cache->GetOrFetch(cfg, b.Changelist);
                if (!d.Date.empty()) {
                    b.Date = d.Date;
                }
                if (!d.Author.empty() && b.User.empty()) {
                    b.User = d.Author;
                }
            }
            {
                std::lock_guard<std::mutex> lk(State().worker.Mutex);
                if (i < State().worker.Rows.size()) {
                    State().worker.Rows[i].Blame = std::move(b);
                }
            }
            State().worker.Progress = static_cast<int>(i + 1);
        }
        if (!State().worker.Cancel.load()) {
            LOG_INFO("Blame worker: finished rows=%zu failures=%d", n, failures);
        }
    } catch (const std::exception& ex) {
        LOG_ERROR("Blame worker: exception: %s", ex.what());
    } catch (...) {
        LOG_ERROR("Blame worker: unknown exception");
    }
    State().worker.PendingPublish = true;
    State().worker.Running = false;
}

void StartWorker(std::vector<BlameRow> rows, BlameAnalysisConfig cfg, std::string atCl) {
    JoinWorkerIfNeeded();
    LOG_INFO("Blame: StartWorker rows=%zu atCl=\"%s\"", rows.size(), atCl.c_str());
    State().worker.Cancel = false;
    State().worker.Progress = 0;
    State().worker.PendingPublish = false;
    State().worker.Cfg = std::move(cfg);
    State().worker.AtChangelist = std::move(atCl);
    const int cap =
        State().worker.Cfg.ChangelistCacheMaxEntries > 0 ? State().worker.Cfg.ChangelistCacheMaxEntries : 512;
    State().worker.Cache = std::make_unique<P4ChangelistDescribeCache>(cap);
    const size_t n = rows.size();
    State().worker.Total = n;
    {
        std::lock_guard<std::mutex> lk(State().worker.Mutex);
        State().worker.Rows = std::move(rows);
    }
    {
        std::lock_guard<std::mutex> lkW(State().worker.Mutex);
        std::lock_guard<std::mutex> lkD(State().displayMutex);
        State().displayRows = State().worker.Rows;
        State().detailFuts.assign(n, std::shared_future<DetailPack>());
        State().detailPhase.assign(n, 0);
        State().detailData.assign(n, DetailPack{});
        State().detailScrolled.assign(n, false);
    }
    State().worker.Running = true;
    State().worker.Thread = std::thread(WorkerThreadMain, n);
}

void RunBlameProcessFromBuffers() {
    State().lastUiStatus.clear();
    State().blameCfg.P4Executable = State().p4Exe;
    State().blameCfg.P4VcExecutable = State().p4vcExe;
    State().blameCfg.TimelapseCommandTemplate = State().timeTpl;
    State().blameCfg.ChangeCommandTemplate = State().changeTpl;
    State().blameCfg.AiChatUrl = State().aiUrl;
    State().blameCfg.PathRemaps.clear();
    if (State().remapFrom[0] != '\0') {
        State().blameCfg.PathRemaps.push_back({State().remapFrom, State().remapTo});
    }
    LogBlameP4PathsIfChanged("process");
    std::vector<std::string> keywords = SplitIgnoreKeywords(std::string(State().ignoreBuf.data()));
    if (keywords.empty()) {
        keywords = State().blameCfg.DefaultIgnoreKeywords;
    }
    std::vector<ParsedCallstackFrame> parsed = ParseCallstackText(std::string(State().callstackBuf));
    std::vector<BlameRow> rows;
    for (auto& p : parsed) {
        if (FrameMatchesIgnoreKeywords(p, keywords)) {
            continue;
        }
        BlameRow br;
        br.Parsed = std::move(p);
        br.PathForP4 = ApplyPathRemaps(br.Parsed.FilePath, State().blameCfg.PathRemaps);
        rows.push_back(std::move(br));
        if (static_cast<int>(rows.size()) >= State().maxFramesVal) {
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lk(State().displayMutex);
        State().displayRows.clear();
        State().detailFuts.clear();
        State().detailPhase.clear();
        State().detailData.clear();
        State().detailScrolled.clear();
    }
    if (rows.empty()) {
        State().lastUiStatus = "No stack frames parsed (check format and ignore list).";
    } else {
        StartWorker(std::move(rows), State().blameCfg, std::string(State().atClBuf));
    }
}

void EnsureDetailLoading(size_t idx, const BlameAnalysisConfig& cfg, const std::string& atCl) {
    std::lock_guard<std::mutex> lk(State().displayMutex);
    if (idx >= State().displayRows.size()) {
        return;
    }
    if (idx >= State().detailPhase.size() || State().detailPhase[idx] != 0) {
        return;
    }
    State().detailPhase[idx] = 1;
    const std::string path = State().displayRows[idx].PathForP4;
    LOG_DEBUG("Blame detail: async load start idx=%zu path=%s", idx, path.c_str());
    std::future<DetailPack> fut = std::async(std::launch::async, [cfg, path, atCl]() {
        DetailPack p;
        p.Lines = P4AnnotateFile(cfg, path, atCl, p.Error);
        for (auto& ln : p.Lines) {
            if (!ln.Changelist.empty()) {
                P4ChangelistDetails d = State().tooltipClCache.GetOrFetch(cfg, ln.Changelist);
                if (!d.Date.empty()) {
                    ln.Date = d.Date;
                }
                if (!d.Author.empty() && ln.User.empty()) {
                    ln.User = d.Author;
                }
            }
        }
        return p;
    });
    State().detailFuts[idx] = fut.share();
}

void PollDetails() {
    for (size_t i = 0; i < State().detachedDetailFuts.size();) {
        if (!State().detachedDetailFuts[i].valid() ||
            State().detachedDetailFuts[i].wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            State().detachedDetailFuts.erase(
                State().detachedDetailFuts.begin() +
                static_cast<std::vector<std::shared_future<DetailPack>>::difference_type>(i));
        } else {
            ++i;
        }
    }
    for (size_t i = 0; i < State().detachedClHoverFuts.size();) {
        if (!State().detachedClHoverFuts[i].valid() ||
            State().detachedClHoverFuts[i].wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            State().detachedClHoverFuts.erase(
                State().detachedClHoverFuts.begin() +
                static_cast<std::vector<std::shared_future<P4ChangelistDetails>>::difference_type>(i));
        } else {
            ++i;
        }
    }

    std::lock_guard<std::mutex> lk(State().displayMutex);
    for (size_t i = 0; i < State().detailFuts.size(); ++i) {
        if (i >= State().detailPhase.size() || State().detailPhase[i] != 1) {
            continue;
        }
        if (!State().detailFuts[i].valid()) {
            continue;
        }
        if (State().detailFuts[i].wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            continue;
        }
        std::string pathForLog;
        if (i < State().displayRows.size()) {
            pathForLog = State().displayRows[i].PathForP4;
        }
        try {
            State().detailData[i] = State().detailFuts[i].get();
        } catch (const std::exception& ex) {
            LOG_WARN("Blame detail: idx=%zu path=%s async exception=%s", i, pathForLog.c_str(), ex.what());
            State().detailData[i].Error = std::string("detail load failed: ") + ex.what();
        } catch (...) {
            LOG_WARN("Blame detail: idx=%zu path=%s async unknown exception", i, pathForLog.c_str());
            State().detailData[i].Error = "detail load failed";
        }
        if (!State().detailData[i].Error.empty()) {
            LOG_WARN("Blame detail: idx=%zu path=%s err=%s", i, pathForLog.c_str(),
                     State().detailData[i].Error.c_str());
        }
        State().detailPhase[i] = 2;
    }
}

} // namespace BlameInternal
