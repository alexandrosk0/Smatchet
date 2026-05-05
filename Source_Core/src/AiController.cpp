#include "AiController.h"

#include <cstdint>
#include <string>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "Logger.h"
#include "NetworkUsageTracker.h"
#include "StringUtil.h"

AiController::AiResult AiController::AnalyzeTicket(const std::string& key, const std::string& summary,
                                                   const std::string& apiKey, const std::string& model,
                                                   const std::string& baseUrl) {
    if (apiKey.empty()) {
        LOG_WARN("AiController: AnalyzeTicket called without API key (key=%s)", key.c_str());
        return {false, "No API Key provided."};
    }

    // Construct the prompt
    std::string prompt = "You are a senior software engineer. Analyze this Jira ticket:\n"
                         "Key: " +
                         key +
                         "\n"
                         "Summary: " +
                         summary +
                         "\n"
                         "Task: Provide a 3-bullet point technical action plan.";

    // Example using an OpenAI-compatible endpoint (works for most LLM providers)
    const std::string modelId = model.empty() ? std::string("gpt-4o-mini") : model;
    std::string apiBase = baseUrl.empty() ? std::string("https://api.openai.com") : baseUrl;
    while (!apiBase.empty() && apiBase.back() == '/') {
        apiBase.pop_back();
    }
    if (apiBase.find("http://") != 0 && apiBase.find("https://") != 0) {
        apiBase = "https://" + apiBase;
    }
    const std::string endpoint = apiBase + "/v1/chat/completions";

    nlohmann::json body = {{"model", modelId},
                           {"messages", nlohmann::json::array({{{"role", "user"}, {"content", prompt}}})}};

    const std::string bodyStr = body.dump();
    auto r = cpr::Post(cpr::Url{endpoint},
                       cpr::Header{{"Content-Type", "application/json"}, {"Authorization", "Bearer " + apiKey}},
                       cpr::Body{bodyStr}, cpr::ConnectTimeout{5000}, cpr::Timeout{60000});
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::OpenAi, static_cast<std::uint64_t>(bodyStr.size()), r);
    // Log host only; full endpoint can encode sensitive routing for custom base URLs.
    std::string endpointHost = apiBase;
    {
        const size_t schemeEnd = endpointHost.find("://");
        if (schemeEnd != std::string::npos) {
            endpointHost = endpointHost.substr(schemeEnd + 3);
        }
    }
    LOG_INFO("AiController: completion request key=%s model=%s host=%s status=%d bytes=%zu", key.c_str(),
             modelId.c_str(), endpointHost.c_str(), static_cast<int>(r.status_code), r.text.size());

    constexpr size_t kMaxAiResponseBytes = 4u * 1024u * 1024u;
    if (r.text.size() > kMaxAiResponseBytes) {
        LOG_ERROR("AiController: response too large (%zu bytes) key=%s", r.text.size(), key.c_str());
        return {false, "API Error: response too large."};
    }

    if (r.status_code == 200) {
        try {
            const auto j = nlohmann::json::parse(r.text);
            const auto itChoices = j.find("choices");
            if (itChoices == j.end() || !itChoices->is_array() || itChoices->empty()) {
                LOG_ERROR("AiController: malformed response (missing choices array) key=%s body=%s", key.c_str(),
                          TruncateForLog(r.text, 300).c_str());
                return {false, "API Error: malformed response (missing choices)"};
            }
            const nlohmann::json& first = (*itChoices)[0];
            const auto itMessage = first.find("message");
            if (itMessage == first.end() || !itMessage->is_object()) {
                LOG_ERROR("AiController: malformed response (missing message object) key=%s body=%s", key.c_str(),
                          TruncateForLog(r.text, 300).c_str());
                return {false, "API Error: malformed response (missing message)"};
            }
            const auto itContent = itMessage->find("content");
            if (itContent == itMessage->end() || !itContent->is_string()) {
                LOG_ERROR("AiController: malformed response (missing content string) key=%s body=%s", key.c_str(),
                          TruncateForLog(r.text, 300).c_str());
                return {false, "API Error: malformed response (missing content)"};
            }
            return {true, itContent->get<std::string>()};
        } catch (const std::exception& ex) {
            LOG_ERROR("AiController: parse exception key=%s err=%s body=%s", key.c_str(), ex.what(),
                      TruncateForLog(r.text, 300).c_str());
            return {false, std::string("API Error: invalid JSON response (") + ex.what() + ")"};
        }
    }

    LOG_ERROR("AiController: API failure key=%s status=%d cprErr=%d msg=%s", key.c_str(),
              static_cast<int>(r.status_code), static_cast<int>(r.error.code), r.error.message.c_str());
    return {false, "API Error: " + std::to_string(r.status_code)};
}

AiController::AiResult AiController::ChatCompletion(const std::string& message, const std::vector<std::string>& context,
                                                    const std::string& apiKey, const std::string& model,
                                                    const std::string& baseUrl) {
    if (apiKey.empty()) {
        return {false, "No API Key provided."};
    }

    const std::string modelId = model.empty() ? std::string("gpt-4o-mini") : model;
    std::string apiBase = baseUrl.empty() ? std::string("https://api.openai.com") : baseUrl;
    while (!apiBase.empty() && apiBase.back() == '/') {
        apiBase.pop_back();
    }
    if (apiBase.find("http://") != 0 && apiBase.find("https://") != 0) {
        apiBase = "https://" + apiBase;
    }
    const std::string endpoint = apiBase + "/v1/chat/completions";

    nlohmann::json messages = nlohmann::json::array();
    
    std::string systemPrompt = "You are a senior software engineer using Smatchet, a Jira and P4 integration tool.";
    if (!context.empty()) {
        systemPrompt += "\n\nSession Context:\n";
        for (const auto& ctx : context) {
            systemPrompt += "- " + ctx + "\n";
        }
    }
    
    messages.push_back({{"role", "system"}, {"content", systemPrompt}});
    messages.push_back({{"role", "user"}, {"content", message}});

    nlohmann::json body = {{"model", modelId}, {"messages", messages}};

    const std::string bodyStr = body.dump();
    auto r = cpr::Post(cpr::Url{endpoint},
                       cpr::Header{{"Content-Type", "application/json"}, {"Authorization", "Bearer " + apiKey}},
                       cpr::Body{bodyStr}, cpr::ConnectTimeout{5000}, cpr::Timeout{60000});
    
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::OpenAi, static_cast<std::uint64_t>(bodyStr.size()), r);

    if (r.status_code == 200) {
        try {
            const auto j = nlohmann::json::parse(r.text);
            return {true, j["choices"][0]["message"]["content"].get<std::string>()};
        } catch (...) {
            return {false, "API Error: failed to parse response."};
        }
    }

    return {false, "API Error: " + std::to_string(r.status_code)};
}






