#include "AgenticInferenceClient.h"

#include "AiClientFactory.h"
#include "AiTypes.h"
#include "ConfigManager.h"
#include "IAiClient.h"
#include "Logger.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaxCommentsToInclude = 20;
constexpr std::size_t kMaxIssueBodyBytes = 8 * 1024;

// System prompt locked in `docs/design/agentic-flow-implementation.md` §
// "Decisions locked" → #3. Instructs the model to emit the JSON envelope
// only, with strict per-action payload shapes. Any deviation is filtered by
// `AgenticInferenceClientPure::ParseInferenceResponse`.
const char* kAgenticTriageSystemPrompt = "You are a triage agent. Given a tracker issue and its comments, propose "
                                         "actions the user should consider. Return ONLY a JSON object of the shape:\n"
                                         "\n"
                                         "{\n"
                                         "  \"proposals\": [\n"
                                         "    {\n"
                                         "      \"action\": \"CommentAdd | LabelAdd | LabelRemove | AssigneeSet | "
                                         "StateTransition | DerivedTicketCreate | ImplementIssue\",\n"
                                         "      \"rationale\": \"free-form one-paragraph explanation\",\n"
                                         "      \"payload\": { /* action-specific object, see below */ }\n"
                                         "    }\n"
                                         "  ]\n"
                                         "}\n"
                                         "\n"
                                         "Per-action payload shapes:\n"
                                         "- CommentAdd: { \"body\": \"string\" }\n"
                                         "- LabelAdd / LabelRemove: { \"label\": \"string\" }\n"
                                         "- AssigneeSet: { \"user\": \"github-username\" }\n"
                                         "- StateTransition: { \"state\": \"open | closed\" }\n"
                                         "- DerivedTicketCreate: { \"targetTracker\": \"jira | plane\", "
                                         "\"summary\": \"string\", \"description\": \"string\" }\n"
                                         "- ImplementIssue: { \"complexityHint\": \"low | medium | high\", "
                                         "\"approachOutline\": \"free-form\" }\n"
                                         "\n"
                                         "Do not return prose, explanations, or markdown. JSON only.";

AiProvider ProviderFromConfig(const TrackerConfig& cfg) {
    switch (cfg.AiProviderKind) {
    case 0:
        return AiProvider::OpenAi;
    case 1:
        return AiProvider::Anthropic;
    case 2:
        return AiProvider::OllamaOpenAiCompat;
    case 3:
        return AiProvider::OllamaNative;
    case 4:
        return AiProvider::DeepSeek;
    default:
        return AiProvider::OpenAi;
    }
}

std::string ResolveBaseUrl(const TrackerConfig& cfg, AiProvider provider) {
    switch (provider) {
    case AiProvider::Anthropic:
        return cfg.AiBaseUrl;
    case AiProvider::OllamaNative:
        return cfg.AiOllamaBaseUrl;
    case AiProvider::OllamaOpenAiCompat:
        return cfg.AiBaseUrl.empty() ? cfg.AiOllamaBaseUrl : cfg.AiBaseUrl;
    case AiProvider::DeepSeek:
        // Empty -> built-in default. Same fallback contract as
        // AiAssistantController::BuildClientConfig.
        return cfg.AiDeepSeekBaseUrl.empty() ? std::string("https://api.deepseek.com") : cfg.AiDeepSeekBaseUrl;
    case AiProvider::OpenAi:
    default:
        return cfg.AiBaseUrl;
    }
}

std::string ResolveApiKey(const TrackerConfig& cfg, AiProvider provider) {
    switch (provider) {
    case AiProvider::Anthropic:
        return cfg.AiAnthropicApiKey;
    case AiProvider::OllamaNative:
        return std::string();
    case AiProvider::DeepSeek:
        return cfg.AiDeepSeekApiKey;
    case AiProvider::OllamaOpenAiCompat:
    case AiProvider::OpenAi:
    default:
        return cfg.AiApiKey;
    }
}

std::string ResolveModel(const TrackerConfig& cfg, AiProvider provider) {
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

// Serialises issue body + N most-recent comments into the LLM user message.
// Comments are sorted newest-first by caller (or accepted in source order
// when caller didn't sort) and trimmed to `kMaxCommentsToInclude` to keep
// the prompt under ~8 KB. Bodies are quoted verbatim — comment content is
// LLM-trusted but never executed.
std::string BuildUserMessage(const std::string& issueBody, const std::vector<TrackerIssueComment>& comments) {
    std::string trimmedBody = issueBody;
    if (trimmedBody.size() > kMaxIssueBodyBytes)
        trimmedBody.resize(kMaxIssueBodyBytes);

    nlohmann::json root = nlohmann::json::object();
    root["issueBody"] = trimmedBody;

    nlohmann::json commentsArr = nlohmann::json::array();
    const std::size_t cap = comments.size() < kMaxCommentsToInclude ? comments.size() : kMaxCommentsToInclude;
    for (std::size_t i = 0; i < cap; ++i) {
        nlohmann::json c = nlohmann::json::object();
        c["author"] = comments[i].Author;
        c["body"] = comments[i].Body;
        c["createdAtSec"] = comments[i].CreatedAtSec;
        commentsArr.push_back(std::move(c));
    }
    root["comments"] = std::move(commentsArr);
    return root.dump(2);
}

} // namespace

bool AgenticInferenceClient::RequestProposals(const std::string& issueBody,
                                              const std::vector<TrackerIssueComment>& comments,
                                              std::vector<AgenticInferenceClientPure::ProposalDraft>& outDrafts,
                                              std::string& outError) {
    outDrafts.clear();
    outError.clear();

    const TrackerConfig cfg = ConfigManager::Load();
    const AiProvider provider = ProviderFromConfig(cfg);

    std::unique_ptr<IAiClient> client = AiClientFactory::MakeAiClient(provider);
    if (!client) {
        outError = "AiClientFactory returned null for provider " + AiClientFactory::ProviderToString(provider);
        LOG_ERROR("AgenticInferenceClient: %s", outError.c_str());
        return false;
    }

    AiClientConfig clientCfg;
    clientCfg.ApiKey = ResolveApiKey(cfg, provider);
    clientCfg.BaseUrl = ResolveBaseUrl(cfg, provider);
    // The agentic triage call is one-shot non-streaming-feel; the LLM should
    // emit ≤ a few KB of JSON. Keep the total-envelope cap generous for
    // reasoning-tuned models (claude-opus, o1) that may think for minutes
    // before producing the final JSON.
    clientCfg.TotalTimeoutMs = 600000;

    AiChatRequest chatReq;
    chatReq.Model = ResolveModel(cfg, provider);
    chatReq.SystemPrompt = kAgenticTriageSystemPrompt;
    AiMessage userMsg;
    userMsg.Role = "user";
    userMsg.Content = BuildUserMessage(issueBody, comments);
    chatReq.History.push_back(std::move(userMsg));

    // Accumulate streamed deltas into a single buffer; the parser runs once
    // when the stream terminates (IsFinal=true). Bundle B SH1 — cap total
    // accumulation at kMaxLlmResponseBytes; a hostile or compromised provider
    // can stream gigabytes of delta chunks before terminating, OOMing the
    // process. When the cap is breached we set the cancel atom (the HTTP layer
    // honours it within a chunk-poll iteration) and latch the cap-breach error
    // for the post-stream handler below to surface to the caller.
    std::string accumulated;
    AiStreamError captured;
    bool sawError = false;
    bool capExceeded = false;
    AiCancelToken cancel = std::make_shared<std::atomic<bool>>(false);

    auto onDelta = [&accumulated, &capExceeded, &cancel](const AiStreamDelta& d) {
        if (capExceeded) {
            // Already aborted; subsequent chunks during in-flight cancellation
            // must not extend the buffer.
            return;
        }
        if (AgenticInferenceClientPure::WouldExceedResponseCap(accumulated.size(), d.TokenChunk.size())) {
            capExceeded = true;
            cancel->store(true);
            LOG_ERROR("AgenticInferenceClient: response exceeded %zu-byte cap at %zu bytes (probable provider error or "
                      "attack) — cancelling stream",
                      AgenticInferenceClientPure::kMaxLlmResponseBytes, accumulated.size());
            return;
        }
        accumulated += d.TokenChunk;
    };
    auto onError = [&captured, &sawError](const AiStreamError& e) {
        captured = e;
        sawError = true;
    };

    LOG_INFO("AgenticInferenceClient: dispatching triage request provider='%s' model='%s' commentCount=%zu",
             AiClientFactory::ProviderToString(provider).c_str(), chatReq.Model.c_str(), comments.size());

    client->SendStreaming(clientCfg, chatReq, onDelta, onError, cancel);

    if (capExceeded) {
        outError = "LLM response exceeded 10 MB cap (probable provider error or attack)";
        LOG_ERROR("AgenticInferenceClient: %s (acceptedBytes=%zu)", outError.c_str(), accumulated.size());
        return false;
    }
    if (sawError) {
        outError = "LLM stream error: " + captured.Message;
        LOG_ERROR("AgenticInferenceClient: %s (httpStatus=%d cancelled=%d)", outError.c_str(), captured.HttpStatus,
                  captured.WasCancelled ? 1 : 0);
        return false;
    }
    if (accumulated.empty()) {
        outError = "LLM returned empty response";
        LOG_WARN("AgenticInferenceClient: %s", outError.c_str());
        return false;
    }

    std::vector<AgenticInferenceClientPure::ProposalDraft> drafts;
    std::string parseError;
    if (!AgenticInferenceClientPure::ParseInferenceResponse(accumulated, drafts, parseError)) {
        outError = "schema parse failed: " + parseError;
        LOG_WARN("AgenticInferenceClient: %s (responseBytes=%zu)", outError.c_str(), accumulated.size());
        return false;
    }
    outDrafts = std::move(drafts);
    LOG_INFO("AgenticInferenceClient: parsed %zu valid proposals (responseBytes=%zu)", outDrafts.size(),
             accumulated.size());
    return true;
}
