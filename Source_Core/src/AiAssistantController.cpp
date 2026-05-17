#include "AiAssistantController.h"

#if defined(SMATCHET_WITH_AI)

#include "AgentsMdLoader.h"
#include "AiClientFactory.h"
#include "AppController.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "SmatchetUiSession.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <utility>

// `g_ui` is the UI-thread-owned UiDrawSession defined unconditionally in
// SmatchetUI.cpp (line 51). SmatchetUiSession.h only declares it `extern`
// behind `#if defined(SMATCHET_WITH_LUA_AUTOMATION)` because that header's
// other consumer (Lua glue) was the historical-only worker that needed it.
// Phase B's MainThreadDispatcher callbacks reach into the same global; we
// declare it directly here so the TU compiles regardless of the
// Lua-automation build flag.
extern UiDrawSession g_ui;

namespace {

AiProvider ProviderFromConfig(const TrackerConfig& cfg) {
    // Clamp identically to ConfigManager::Load — defence in depth in case
    // an out-of-range value sneaks through (e.g. cfg constructed in tests).
    switch (cfg.AiProviderKind) {
    case 0:
        return AiProvider::OpenAi;
    case 1:
        return AiProvider::Anthropic;
    case 2:
        return AiProvider::OllamaOpenAiCompat;
    case 3:
        return AiProvider::OllamaNative;
    default:
        return AiProvider::OpenAi;
    }
}

AiClientConfig BuildClientConfig(const TrackerConfig& cfg, AiProvider provider) {
    AiClientConfig out;
    switch (provider) {
    case AiProvider::Anthropic:
        out.ApiKey = cfg.AiAnthropicApiKey;
        out.BaseUrl = cfg.AiBaseUrl; // user-overridable; empty = provider default
        break;
    case AiProvider::OllamaNative:
        out.ApiKey.clear(); // Ollama-native has no API key
        out.BaseUrl = cfg.AiOllamaBaseUrl;
        break;
    case AiProvider::OllamaOpenAiCompat:
        out.ApiKey = cfg.AiApiKey;
        // OllamaOpenAi-compat uses the user's base URL (typically http://localhost:11434/v1)
        out.BaseUrl = cfg.AiBaseUrl.empty() ? cfg.AiOllamaBaseUrl : cfg.AiBaseUrl;
        break;
    case AiProvider::OpenAi:
    default:
        out.ApiKey = cfg.AiApiKey;
        out.BaseUrl = cfg.AiBaseUrl;
        break;
    }
    return out;
}

} // namespace

AiAssistantController::AiAssistantController(AppController& app) : app_(app) {
    const TrackerConfig cfg = ConfigManager::Load();
    const AiProvider provider = ProviderFromConfig(cfg);
    client_ = AiClientFactory::MakeAiClient(provider);
    if (client_) {
        cachedProviderName_ = client_->GetProviderName();
        clientConfig_ = BuildClientConfig(cfg, provider);
    } else {
        // Phase D providers return null today. The controller stays alive so the
        // panel can render an error strip; Submit() will short-circuit until the
        // user picks a supported provider in Preferences.
        cachedProviderName_.clear();
    }
    worker_ = std::thread(&AiAssistantController::WorkerLoop, this);
}

AiAssistantController::~AiAssistantController() {
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        shuttingDown_ = true;
        if (currentCancel_) {
            currentCancel_->store(true, std::memory_order_release);
        }
    }
    queueCv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void AiAssistantController::Submit(uint64_t turnGen, std::string prompt, std::vector<AiContextBlock> context) {
    if (!client_) {
        LOG_WARN("AiAssistantController::Submit dropped — no active client for provider '%s'.",
                 cachedProviderName_.c_str());
        // The UI guarantees a visible error strip via assistantLastError; the
        // caller's Send-button path sets that on the same UI tick when needed.
        return;
    }
    Request req;
    req.Prompt = std::move(prompt);
    req.Context = std::move(context);
    req.TurnGen = turnGen;
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        if (shuttingDown_) {
            return;
        }
        // Replace cancel atom — each turn owns its own atom so a Cancel of the
        // previous turn cannot flip the next turn's flag (a race the shared_ptr
        // ownership model neutralises: worker captures the previous shared_ptr;
        // UI rebinds currentCancel_ to a fresh shared_ptr for the new turn).
        currentCancel_ = std::make_shared<std::atomic<bool>>(false);
        pending_.push_back(std::move(req));
    }
    queueCv_.notify_one();
}

void AiAssistantController::Cancel() {
    std::lock_guard<std::mutex> lk(queueMutex_);
    if (currentCancel_) {
        currentCancel_->store(true, std::memory_order_release);
    }
}

void AiAssistantController::WorkerLoop() {
    while (true) {
        Request req;
        AiCancelToken cancel;
        {
            std::unique_lock<std::mutex> lk(queueMutex_);
            queueCv_.wait(lk, [this] { return shuttingDown_ || !pending_.empty(); });
            if (shuttingDown_ && pending_.empty()) {
                return;
            }
            req = std::move(pending_.front());
            pending_.pop_front();
            cancel = currentCancel_;
        }
        // Pre-cancel check — Cancel might have fired between Submit and WorkerLoop pickup.
        if (cancel && cancel->load(std::memory_order_acquire)) {
            state_.store(State::Cancelled, std::memory_order_release);
            continue;
        }
        state_.store(State::InFlight, std::memory_order_release);
        RunRequest(req);
    }
}

void AiAssistantController::RunRequest(const Request& req) {
    if (!client_) {
        state_.store(State::Errored, std::memory_order_release);
        return;
    }

    AiChatRequest chatReq;
    chatReq.Model = []() -> std::string {
        // Re-read the model each turn so a Preferences change while a turn is
        // queued takes effect on the next Submit, without a per-turn config
        // snapshot leaking into the request struct.
        const TrackerConfig cfg = ConfigManager::Load();
        const AiProvider provider = ProviderFromConfig(cfg);
        switch (provider) {
        case AiProvider::Anthropic:
            return cfg.AiModelAnthropic;
        case AiProvider::OllamaNative:
        case AiProvider::OllamaOpenAiCompat:
            return cfg.AiModelOllama;
        case AiProvider::OpenAi:
        default:
            return cfg.AiModelOpenAi;
        }
    }();

    // System-prompt assembly: agents.md prefix + "## Current Smatchet context" header
    // (when any block has content) + each enabled block wrapped in
    // `<smatchet_context block="...">...</smatchet_context>` tags. File I/O for
    // agents.md happens here on the worker thread to honour pillar 2 (no synchronous
    // I/O reaches the UI thread). Layers are capped at 64 KB each by AgentsMdLoader,
    // so the total system-prompt overhead is bounded.
    chatReq.SystemPrompt.clear();
    {
        const TrackerConfig agentsCfg = ConfigManager::Load();
        const std::string agentsMd =
            AgentsMdLoader::LoadLayered(agentsCfg.AgentsMdGlobalPath, agentsCfg.ProjectAgentsMdPath);
        if (!agentsMd.empty()) {
            chatReq.SystemPrompt.append(agentsMd);
            if (chatReq.SystemPrompt.back() != '\n') {
                chatReq.SystemPrompt.push_back('\n');
            }
            chatReq.SystemPrompt.append("\n---\n\n");
        }
    }
    {
        std::string contextSection;
        for (const auto& block : req.Context) {
            if (block.Body.empty()) {
                continue;
            }
            if (!contextSection.empty()) {
                contextSection.push_back('\n');
            }
            contextSection.append("<smatchet_context block=\"");
            contextSection.append(block.Name);
            contextSection.append("\">\n");
            contextSection.append(block.Body);
            if (block.Body.back() != '\n') {
                contextSection.push_back('\n');
            }
            contextSection.append("</smatchet_context>\n");
        }
        if (!contextSection.empty()) {
            chatReq.SystemPrompt.append("## Current Smatchet context\n\n");
            chatReq.SystemPrompt.append(contextSection);
        }
    }
    AiMessage userMsg;
    userMsg.Role = "user";
    userMsg.Content = req.Prompt;
    chatReq.History.push_back(std::move(userMsg));

    // Capture turnGen by value so stale dispatcher tasks (after a newer Submit
    // or Cancel) can be dropped on the UI side via assistantTurnGen comparison.
    const uint64_t turnGen = req.TurnGen;
    AppController* app = &app_;

    auto onDelta = [app, turnGen](const AiStreamDelta& delta) {
        const std::string chunk = delta.TokenChunk;
        const bool isFinal = delta.IsFinal;
        app->mainThreadDispatcher.PostToMainThread([turnGen, chunk, isFinal]() {
            if (g_ui.assistantTurnGen != turnGen) {
                return; // stale callback — Cancel or newer Submit raced us
            }
            if (!chunk.empty()) {
                g_ui.assistantStreamBuf.append(chunk);
            }
            if (isFinal) {
                AiMessage assistantMsg;
                assistantMsg.Role = "assistant";
                assistantMsg.Content = g_ui.assistantStreamBuf;
                g_ui.assistantHistory.push_back(std::move(assistantMsg));
                g_ui.assistantStreamBuf.clear();
                g_ui.assistantInFlight = false;
            }
        });
    };

    auto onError = [this, app, turnGen](const AiStreamError& err) {
        const int httpStatus = err.HttpStatus;
        const std::string message = err.Message;
        const bool wasCancelled = err.WasCancelled;
        if (wasCancelled) {
            state_.store(State::Cancelled, std::memory_order_release);
        } else {
            state_.store(State::Errored, std::memory_order_release);
        }
        app->mainThreadDispatcher.PostToMainThread([turnGen, httpStatus, message, wasCancelled]() {
            if (g_ui.assistantTurnGen != turnGen) {
                return;
            }
            g_ui.assistantInFlight = false;
            if (wasCancelled) {
                // Scenario 3 contract — partial text retained in history.
                if (!g_ui.assistantStreamBuf.empty()) {
                    AiMessage assistantMsg;
                    assistantMsg.Role = "assistant";
                    assistantMsg.Content = g_ui.assistantStreamBuf + "\n[cancelled]";
                    g_ui.assistantHistory.push_back(std::move(assistantMsg));
                }
                g_ui.assistantStreamBuf.clear();
                g_ui.assistantLastError.clear();
            } else {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "API Error: %d ", httpStatus);
                g_ui.assistantLastError = std::string(buf) + message;
                g_ui.assistantStreamBuf.clear();
            }
        });
    };

    AiCancelToken cancel;
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        cancel = currentCancel_;
    }
    if (!cancel) {
        cancel = std::make_shared<std::atomic<bool>>(false);
    }

    try {
        client_->SendStreaming(clientConfig_, chatReq, onDelta, onError, cancel);
    } catch (const std::exception& ex) {
        LOG_ERROR("AiAssistantController: SendStreaming threw: %s", ex.what());
        AiStreamError err;
        err.HttpStatus = 0;
        err.Message = std::string("client exception: ") + ex.what();
        err.WasCancelled = false;
        onError(err);
    } catch (...) {
        LOG_ERROR("AiAssistantController: SendStreaming threw unknown exception");
        AiStreamError err;
        err.HttpStatus = 0;
        err.Message = "client unknown exception";
        err.WasCancelled = false;
        onError(err);
    }

    // Worker keeps running for the next request; UI thread transitions State::Idle
    // when it observes a stable assistantInFlight=false.
    if (state_.load(std::memory_order_acquire) == State::InFlight) {
        state_.store(State::Idle, std::memory_order_release);
    }
}

void AiAssistantController::AddAiContext(const AiContextBlock& block) {
    luaContext_.push_back(block);
}

void AiAssistantController::ClearAiContext() {
    luaContext_.clear();
}

std::vector<AiContextBlock> AiAssistantController::GetAiContext() const {
    return luaContext_;
}

#endif // SMATCHET_WITH_AI
