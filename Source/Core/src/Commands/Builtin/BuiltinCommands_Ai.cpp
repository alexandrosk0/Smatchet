// ai.* - debug commands for AI assistant configuration and HTTP shape.
// Five operations:
//   ai.list-models       - list public model catalog for a provider (no network).
//   ai.dump-request      - build the would-be request (URL + headers + body),
//                          headers redacted; no network.
//   ai.probe             - run IAiClient::ProbeReachability (network).
//   ai.send-once         - synchronous single-shot SendStreaming, returns
//                          collected deltas + final + HTTP status (network).
//   ai.validate-prefs    - run AiPrefsValidator against ConfigManager::Load()
//                          (no network).
// API keys NEVER appear as CLI arguments. All commands read keys from
// ConfigManager::Load(). ai.dump-request redacts x-api-key / Authorization
// header values before emitting them.
// Build-time gate: every command short-circuits with a friendly failure when
// SMATCHET_WITH_AI is off (the IAiClient layer is absent in that build).

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

// fan-in Phase 5: this TU never touches the AppController (its registrar takes an unused
// AppController& satisfied by the fwd-decl in BuiltinCommands_Internal.h), so AppController.h
// is dropped — one includer off the fan-in count with no facet needed.
#include <nlohmann/json.hpp> // this TU constructs nlohmann::json directly.
#include "Logger.h"

#if defined(SMATCHET_WITH_AI)
#include "AiClientFactory.h"
#include "AiModelCatalog.h"
#include "AiPrefsValidator.h"
#include "AiTypes.h"
#include "ConfigManager.h"
#include "IAiClient.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;

#if defined(SMATCHET_WITH_AI)

using builtin_detail::PInt;
using builtin_detail::PString;

namespace {

bool ResolveProvider(const std::string& key, AiProvider& out) { return AiClientFactory::ProviderFromString(key, out); }

const char* AiProviderDisplayName(AiProvider p) {
    switch (p) {
    case AiProvider::OpenAi:
        return "openai";
    case AiProvider::Anthropic:
        return "anthropic";
    case AiProvider::OllamaOpenAiCompat:
        return "ollama-openai";
    case AiProvider::OllamaNative:
        return "ollama-native";
    case AiProvider::DeepSeek:
        return "deepseek";
    }
    return "unknown";
}

AiClientConfig BuildClientConfigForProvider(const TrackerConfig& cfg, AiProvider provider) {
    AiClientConfig out;
    switch (provider) {
    case AiProvider::Anthropic:
        out.ApiKey = cfg.AiAnthropicApiKey;
        out.BaseUrl = cfg.AiBaseUrl;
        break;
    case AiProvider::OllamaNative:
        out.ApiKey.clear();
        out.BaseUrl = cfg.AiOllamaBaseUrl;
        break;
    case AiProvider::OllamaOpenAiCompat:
        out.ApiKey = cfg.AiApiKey;
        out.BaseUrl = cfg.AiBaseUrl.empty() ? cfg.AiOllamaBaseUrl : cfg.AiBaseUrl;
        break;
    case AiProvider::DeepSeek:
        out.ApiKey = cfg.AiDeepSeekApiKey;
        out.BaseUrl = cfg.AiDeepSeekBaseUrl.empty() ? std::string("https://api.deepseek.com") : cfg.AiDeepSeekBaseUrl;
        break;
    case AiProvider::OpenAi:
    default:
        out.ApiKey = cfg.AiApiKey;
        out.BaseUrl = cfg.AiBaseUrl;
        break;
    }
    return out;
}

std::string ResolveModelId(const TrackerConfig& cfg, AiProvider provider) {
    switch (provider) {
    case AiProvider::Anthropic:
        return cfg.AiModelAnthropic;
    case AiProvider::OllamaNative:
    case AiProvider::OllamaOpenAiCompat:
        return cfg.AiModelOllama;
    case AiProvider::DeepSeek:
        return cfg.AiModelDeepSeek;
    case AiProvider::OpenAi:
    default:
        return cfg.AiModelOpenAi;
    }
}

std::string RedactedKey(const std::string& key) {
    if (key.empty()) {
        return std::string("<empty>");
    }
    std::string out = "<redacted ";
    out += std::to_string(key.size());
    out += " chars>";
    return out;
}

// Mirror the body shape each client emits without depending on their internal
// helpers. Drift risk is low: changing the wire body is a wire-level decision
// that should also update this debug dumper.
nlohmann::json BuildAnthropicBody(const std::string& model, const std::string& systemPrompt, const std::string& prompt,
                                  int maxTokens) {
    nlohmann::json body;
    body["model"] = model;
    body["stream"] = true;
    body["max_tokens"] = (maxTokens > 0) ? maxTokens : 4096;
    if (!systemPrompt.empty()) {
        body["system"] = systemPrompt;
    }
    nlohmann::json messages = nlohmann::json::array();
    nlohmann::json userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = prompt;
    messages.push_back(std::move(userMsg));
    body["messages"] = std::move(messages);
    return body;
}

nlohmann::json BuildOpenAiBody(const std::string& model, const std::string& systemPrompt, const std::string& prompt,
                               int maxTokens) {
    nlohmann::json body;
    body["model"] = model;
    body["stream"] = true;
    // Mirror OpenAiClient::BuildChatBody — always emit max_tokens so the
    // debug dump matches the wire body that reasoning-style models depend on
    // (gemma-4-31b, o1-style) to reach `content` instead of exhausting on
    // `reasoning_content`.
    body["max_tokens"] = (maxTokens > 0) ? maxTokens : 4096;
    nlohmann::json messages = nlohmann::json::array();
    if (!systemPrompt.empty()) {
        nlohmann::json sys;
        sys["role"] = "system";
        sys["content"] = systemPrompt;
        messages.push_back(std::move(sys));
    }
    nlohmann::json userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = prompt;
    messages.push_back(std::move(userMsg));
    body["messages"] = std::move(messages);
    return body;
}

nlohmann::json BuildOllamaNativeBody(const std::string& model, const std::string& systemPrompt,
                                     const std::string& prompt) {
    nlohmann::json body;
    body["model"] = model;
    body["stream"] = true;
    nlohmann::json messages = nlohmann::json::array();
    if (!systemPrompt.empty()) {
        nlohmann::json sys;
        sys["role"] = "system";
        sys["content"] = systemPrompt;
        messages.push_back(std::move(sys));
    }
    nlohmann::json userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = prompt;
    messages.push_back(std::move(userMsg));
    body["messages"] = std::move(messages);
    return body;
}

// Mirror OpenAiClient::ResolveBaseUrl — strip a trailing "/v1" or "/v1/" so
// debug dumps match the wire body when callers configure the natural
// LM Studio / Ollama-OpenAI-compat / OpenAI-docs form ending in /v1.
std::string StripOpenAiV1Suffix(std::string base) {
    if (base.size() >= 4 && base.compare(base.size() - 4, 4, "/v1/") == 0) {
        base.resize(base.size() - 4);
    } else if (base.size() >= 3 && base.compare(base.size() - 3, 3, "/v1") == 0) {
        base.resize(base.size() - 3);
    }
    return base;
}

std::string ResolveEndpointUrl(AiProvider provider, const std::string& baseUrl) {
    auto join = [](std::string base, const char* path) {
        if (base.empty()) {
            return std::string(path);
        }
        if (base.back() == '/') {
            base.pop_back();
        }
        return base + path;
    };
    switch (provider) {
    case AiProvider::Anthropic:
        return join(baseUrl.empty() ? std::string("https://api.anthropic.com") : baseUrl, "/v1/messages");
    case AiProvider::OllamaNative:
        return join(baseUrl.empty() ? std::string("http://localhost:11434") : baseUrl, "/api/chat");
    case AiProvider::OllamaOpenAiCompat:
    case AiProvider::OpenAi:
    default:
        return join(StripOpenAiV1Suffix(baseUrl.empty() ? std::string("https://api.openai.com") : baseUrl),
                    "/v1/chat/completions");
    }
}

void RegisterListModelsCommand(CommandRegistry& reg) {
    // --- ai.list-models -----------------------------------------------------
    {
        Command c = MakeCommand(
            "ai.list-models", "List the hardcoded public model catalog for an AI provider (no network).",
            [](const nlohmann::json& args, const CommandContext&) {
                const std::string providerStr = args.value("provider", std::string("openai"));
                AiProvider p;
                if (!ResolveProvider(providerStr, p)) {
                    return CommandResult::Failure(ErrorCode::ValidationError, "unknown provider '" + providerStr + "'");
                }
                const std::vector<smatchet::ai::ModelOption> models = smatchet::ai::KnownModels(p);
                nlohmann::json arr = nlohmann::json::array();
                for (std::size_t i = 0; i < models.size(); ++i) {
                    nlohmann::json m;
                    m["id"] = models[i].Id;
                    m["display_name"] = models[i].DisplayName;
                    arr.push_back(std::move(m));
                }
                nlohmann::json out;
                out["provider"] = providerStr;
                out["models"] = std::move(arr);
                out["count"] = static_cast<int>(models.size());
                return CommandResult::Success(std::move(out));
            });
        ParamSpec providerParam = PString("provider", "openai|anthropic|ollama-openai|ollama-native|deepseek");
        providerParam.Default = std::make_shared<nlohmann::json>("openai");
        providerParam.Enum = {"openai", "anthropic", "ollama-openai", "ollama-native", "deepseek"};
        c.Params = {std::move(providerParam)};
        c.Idempotent = true;
        reg.Register(std::move(c));
    }
}

void RegisterDumpRequestCommand(CommandRegistry& reg) {
    // --- ai.dump-request ----------------------------------------------------
    {
        Command c = MakeCommand(
            "ai.dump-request",
            "Construct the would-be HTTP request (URL + headers + body) for a provider. Headers are redacted; no "
            "network call. Reads keys from ConfigManager::Load().",
            [](const nlohmann::json& args, const CommandContext&) {
                const std::string providerStr = args.value("provider", std::string("openai"));
                AiProvider provider;
                if (!ResolveProvider(providerStr, provider)) {
                    return CommandResult::Failure(ErrorCode::ValidationError, "unknown provider '" + providerStr + "'");
                }
                const std::string prompt = args.value("prompt", std::string());
                if (prompt.empty()) {
                    return CommandResult::Failure(ErrorCode::ValidationError, "prompt is required");
                }
                const std::string systemPrompt = args.value("system", std::string());

                const TrackerConfig cfg = ConfigManager::Load();
                const AiClientConfig clientCfg = BuildClientConfigForProvider(cfg, provider);
                const std::string model = args.value("model", ResolveModelId(cfg, provider));

                nlohmann::json body;
                nlohmann::json headers;
                std::string url = ResolveEndpointUrl(provider, clientCfg.BaseUrl);

                switch (provider) {
                case AiProvider::Anthropic:
                    body = BuildAnthropicBody(model, systemPrompt, prompt, 0);
                    headers["accept"] = "text/event-stream";
                    headers["content-type"] = "application/json";
                    headers["cache-control"] = "no-cache";
                    headers["anthropic-version"] = "2023-06-01";
                    headers["x-api-key"] = RedactedKey(clientCfg.ApiKey);
                    break;
                case AiProvider::OllamaNative:
                    body = BuildOllamaNativeBody(model, systemPrompt, prompt);
                    headers["accept"] = "application/x-ndjson";
                    headers["content-type"] = "application/json";
                    break;
                case AiProvider::OpenAi:
                case AiProvider::OllamaOpenAiCompat:
                default:
                    body = BuildOpenAiBody(model, systemPrompt, prompt, 0);
                    headers["accept"] = "text/event-stream";
                    headers["content-type"] = "application/json";
                    headers["cache-control"] = "no-cache";
                    headers["authorization"] = std::string("Bearer ") + RedactedKey(clientCfg.ApiKey);
                    break;
                }

                nlohmann::json out;
                out["provider"] = providerStr;
                out["method"] = "POST";
                out["url"] = std::move(url);
                out["headers"] = std::move(headers);
                out["body"] = std::move(body);
                out["model"] = model;
                return CommandResult::Success(std::move(out));
            });
        ParamSpec providerParam = PString("provider", "openai|anthropic|ollama-openai|ollama-native|deepseek", true);
        providerParam.Enum = {"openai", "anthropic", "ollama-openai", "ollama-native", "deepseek"};
        c.Params = {
            std::move(providerParam),
            PString("prompt", "User prompt text.", true),
            PString("system", "Optional system prompt."),
            PString("model", "Optional model override; defaults to the provider's persisted ConfigManager value."),
        };
        c.Idempotent = true;
        reg.Register(std::move(c));
    }
}

void RegisterProbeCommand(CommandRegistry& reg) {
    // --- ai.probe -----------------------------------------------------------
    {
        Command c = MakeCommand(
            "ai.probe",
            "Probe the chosen AI provider's reachability (IAiClient::ProbeReachability). Network call. Reads "
            "keys from ConfigManager::Load(); returns empty error on success.",
            [](const nlohmann::json& args, const CommandContext&) {
                const std::string providerStr = args.value("provider", std::string("openai"));
                AiProvider provider;
                if (!ResolveProvider(providerStr, provider)) {
                    return CommandResult::Failure(ErrorCode::ValidationError, "unknown provider '" + providerStr + "'");
                }
                std::unique_ptr<IAiClient> client = AiClientFactory::MakeAiClient(provider);
                if (!client) {
                    return CommandResult::Failure(ErrorCode::HandlerError,
                                                  std::string("no client implementation for provider '") + providerStr +
                                                      "'");
                }
                const TrackerConfig cfg = ConfigManager::Load();
                AiClientConfig clientCfg = BuildClientConfigForProvider(cfg, provider);
                // Tight probe timeouts: don't hold the CLI on a half-open host.
                clientCfg.ConnectTimeoutMs = 5000;
                clientCfg.TotalTimeoutMs = 10000;
                const std::string err = client->ProbeReachability(clientCfg);
                nlohmann::json out;
                out["provider"] = providerStr;
                out["ok"] = err.empty();
                out["error"] = err;
                return CommandResult::Success(std::move(out));
            });
        ParamSpec providerParam = PString("provider", "openai|anthropic|ollama-openai|ollama-native|deepseek", true);
        providerParam.Enum = {"openai", "anthropic", "ollama-openai", "ollama-native", "deepseek"};
        c.Params = {std::move(providerParam)};
        c.AsyncSafe = false;
        c.Idempotent = true;
        reg.Register(std::move(c));
    }
}

// Collected outcome of a single-shot streaming call, accumulated across the
// onDelta / onError callbacks plus any thrown exception.
struct SendOnceResult {
    std::string finalContent;
    int deltasCount = 0;
    bool sawFinal = false;
    int httpStatus = 0;
    std::string errorMessage;
    std::string responseBodyExcerpt;
    bool errored = false;
};

// Run the synchronous SendStreaming call with a best-effort timeout watchdog,
// collecting deltas + errors into `res`. Behaviour is byte-for-byte identical
// to the inlined handler body it was extracted from.
void RunSendOnceStream(IAiClient& client, const AiClientConfig& clientCfg, const AiChatRequest& chatReq,
                       SendOnceResult& res) {
    std::mutex collectMu;

    auto onDelta = [&](const AiStreamDelta& d) {
        std::lock_guard<std::mutex> lk(collectMu);
        if (!d.TokenChunk.empty()) {
            res.finalContent.append(d.TokenChunk);
        }
        ++res.deltasCount;
        if (d.IsFinal) {
            res.sawFinal = true;
        }
    };
    auto onError = [&](const AiStreamError& e) {
        std::lock_guard<std::mutex> lk(collectMu);
        res.errored = true;
        res.httpStatus = e.HttpStatus;
        res.errorMessage = e.Message;
        // Surface the redacted body excerpt the client built into
        // err.Message so the CLI consumer sees the same shape the
        // Logger captured.
        res.responseBodyExcerpt = e.Message;
    };

    AiCancelToken cancel = std::make_shared<std::atomic<bool>>(false);

    // Best-effort timeout watchdog: a side thread flips the cancel
    // atom after `timeoutMs`. cpr's Timeout already enforces the
    // HTTP envelope; this is belt-and-braces for the rare case the
    // stream stalls inside libcurl's read loop after the headers
    // arrive.
    std::atomic<bool> done{false};
    std::thread watchdog;
    if (clientCfg.TotalTimeoutMs > 0) {
        watchdog = std::thread([&done, cancel, totalMs = clientCfg.TotalTimeoutMs]() {
            const auto start = std::chrono::steady_clock::now();
            while (!done.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
                if (elapsed.count() > totalMs + 1000) {
                    cancel->store(true, std::memory_order_release);
                    return;
                }
            }
        });
    }

    try {
        client.SendStreaming(clientCfg, chatReq, onDelta, onError, cancel);
    } catch (const std::exception& e) {
        res.errored = true;
        res.errorMessage = std::string("exception: ") + e.what();
    } catch (...) {
        res.errored = true;
        res.errorMessage = "unknown exception";
    }

    done.store(true, std::memory_order_release);
    if (watchdog.joinable()) {
        watchdog.join();
    }
}

CommandResult RunSendOnce(const nlohmann::json& args) {
    const std::string providerStr = args.value("provider", std::string("openai"));
    AiProvider provider;
    if (!ResolveProvider(providerStr, provider)) {
        return CommandResult::Failure(ErrorCode::ValidationError, "unknown provider '" + providerStr + "'");
    }
    const std::string prompt = args.value("prompt", std::string());
    if (prompt.empty()) {
        return CommandResult::Failure(ErrorCode::ValidationError, "prompt is required");
    }
    const std::string systemPrompt = args.value("system", std::string());
    const int timeoutMs = static_cast<int>(args.value("timeout-ms", 30000));

    std::unique_ptr<IAiClient> client = AiClientFactory::MakeAiClient(provider);
    if (!client) {
        return CommandResult::Failure(ErrorCode::HandlerError,
                                      std::string("no client implementation for provider '") + providerStr + "'");
    }

    const TrackerConfig cfg = ConfigManager::Load();
    AiClientConfig clientCfg = BuildClientConfigForProvider(cfg, provider);
    clientCfg.ConnectTimeoutMs = 5000;
    clientCfg.TotalTimeoutMs = (timeoutMs > 0) ? timeoutMs : 30000;

    AiChatRequest chatReq;
    chatReq.Model = args.value("model", ResolveModelId(cfg, provider));
    chatReq.SystemPrompt = systemPrompt;
    AiMessage userMsg;
    userMsg.Role = "user";
    userMsg.Content = prompt;
    chatReq.History.push_back(std::move(userMsg));

    SendOnceResult res;
    RunSendOnceStream(*client, clientCfg, chatReq, res);

    nlohmann::json out;
    out["provider"] = providerStr;
    out["ok"] = !res.errored;
    out["http_status"] = res.httpStatus;
    out["final_content"] = res.finalContent;
    out["deltas_count"] = res.deltasCount;
    out["saw_final"] = res.sawFinal;
    out["error_message"] = res.errorMessage;
    // Cap response body excerpt to 1 KiB - the client already
    // redacted, but extra defence-in-depth against log spam if a
    // future provider returns a giant HTML stack trace.
    if (res.responseBodyExcerpt.size() > 1024) {
        res.responseBodyExcerpt.resize(1024);
        res.responseBodyExcerpt += "...";
    }
    out["response_body_excerpt"] = res.responseBodyExcerpt;
    return CommandResult::Success(std::move(out));
}

void RegisterSendOnceCommand(CommandRegistry& reg) {
    // --- ai.send-once -------------------------------------------------------
    {
        Command c = MakeCommand(
            "ai.send-once",
            "Send a single chat turn through the chosen AI provider synchronously. Collects streamed deltas + the "
            "final message. Network call. Reads keys from ConfigManager::Load(); returns HTTP status + body excerpt "
            "for failures so 400/401/429/5xx are diagnosable without spelunking the runtime log.",
            [](const nlohmann::json& args, const CommandContext&) { return RunSendOnce(args); });
        ParamSpec providerParam = PString("provider", "openai|anthropic|ollama-openai|ollama-native|deepseek", true);
        providerParam.Enum = {"openai", "anthropic", "ollama-openai", "ollama-native", "deepseek"};
        ParamSpec timeoutParam = PInt("timeout-ms", "Total HTTP envelope timeout (ms). Default 30000.", 30000);
        c.Params = {
            std::move(providerParam),
            PString("prompt", "User prompt text.", true),
            PString("system", "Optional system prompt."),
            PString("model", "Optional model override; defaults to provider's persisted ConfigManager value."),
            std::move(timeoutParam),
        };
        c.AsyncSafe = false;
        c.Idempotent = false;
        reg.Register(std::move(c));
    }
}

void RegisterValidatePrefsCommand(CommandRegistry& reg) {
    // --- ai.validate-prefs --------------------------------------------------
    {
        Command c = MakeCommand(
            "ai.validate-prefs",
            "Run AiPrefsValidator against ConfigManager::Load(). Returns errors[] (block save) + warnings[] (inform). "
            "No network.",
            [](const nlohmann::json&, const CommandContext&) {
                const TrackerConfig cfg = ConfigManager::Load();
                const smatchet::ai::PrefsValidation v = smatchet::ai::ValidateAiPrefs(cfg);
                nlohmann::json errs = v.Errors;
                nlohmann::json warns = v.Warnings;
                nlohmann::json out;
                out["ok"] = v.IsOk();
                out["errors"] = std::move(errs);
                out["warnings"] = std::move(warns);
                AiProvider effective;
                switch (cfg.AiProviderKind) {
                case 0:
                    effective = AiProvider::OpenAi;
                    break;
                case 1:
                    effective = AiProvider::Anthropic;
                    break;
                case 2:
                    effective = AiProvider::OllamaOpenAiCompat;
                    break;
                case 3:
                    effective = AiProvider::OllamaNative;
                    break;
                case 4:
                    effective = AiProvider::DeepSeek;
                    break;
                default:
                    effective = AiProvider::OpenAi;
                    break;
                }
                out["active_provider"] = AiProviderDisplayName(effective);
                return CommandResult::Success(std::move(out));
            });
        c.Idempotent = true;
        reg.Register(std::move(c));
    }
}

} // namespace

void RegisterAiCommands(CommandRegistry& reg, AppController& /*app*/) {
    RegisterListModelsCommand(reg);
    RegisterDumpRequestCommand(reg);
    RegisterProbeCommand(reg);
    RegisterSendOnceCommand(reg);
    RegisterValidatePrefsCommand(reg);
}

#endif // SMATCHET_WITH_AI

} // namespace cmd
} // namespace smatchet
