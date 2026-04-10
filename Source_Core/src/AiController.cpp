#include "AiController.h"

#include <cstdint>
#include <string>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "Logger.h"
#include "NetworkUsageTracker.h"
#include "StringUtil.h"

AiController::AiResult AiController::AnalyzeTicket(const std::string& key,
                                                   const std::string& summary,
                                                   const std::string& apiKey) {
    if (apiKey.empty()) {
        LOG_WARN("AiController: AnalyzeTicket called without API key (key=%s)", key.c_str());
        return { false, "No API Key provided." };
    }

    // Construct the prompt
    std::string prompt = "You are a senior software engineer. Analyze this Jira ticket:\n"
                         "Key: " + key + "\n"
                         "Summary: " + summary + "\n"
                         "Task: Provide a 3-bullet point technical action plan.";

    // Example using an OpenAI-compatible endpoint (works for most LLM providers)
    nlohmann::json body = {
        {"model", "gpt-3.5-turbo"},
        {"messages", nlohmann::json::array({
            {{"role", "user"}, {"content", prompt}}
        })}
    };

    const std::string bodyStr = body.dump();
    auto r = cpr::Post(cpr::Url{"https://api.openai.com/v1/chat/completions"},
                       cpr::Header{{"Content-Type", "application/json"},
                                   {"Authorization", "Bearer " + apiKey}},
                       cpr::Body{bodyStr});
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::OpenAi,
        static_cast<std::uint64_t>(bodyStr.size()), r);
    LOG_INFO("AiController: completion request key=%s status=%d bytes=%zu",
             key.c_str(),
             static_cast<int>(r.status_code),
             r.text.size());

    if (r.status_code == 200) {
        try {
            const auto j = nlohmann::json::parse(r.text);
            const auto itChoices = j.find("choices");
            if (itChoices == j.end() || !itChoices->is_array() || itChoices->empty()) {
                LOG_ERROR("AiController: malformed response (missing choices array) key=%s body=%s",
                          key.c_str(),
                          TruncateForLog(r.text, 300).c_str());
                return {false, "API Error: malformed response (missing choices)"};
            }
            const nlohmann::json& first = (*itChoices)[0];
            const auto itMessage = first.find("message");
            if (itMessage == first.end() || !itMessage->is_object()) {
                LOG_ERROR("AiController: malformed response (missing message object) key=%s body=%s",
                          key.c_str(),
                          TruncateForLog(r.text, 300).c_str());
                return {false, "API Error: malformed response (missing message)"};
            }
            const auto itContent = itMessage->find("content");
            if (itContent == itMessage->end() || !itContent->is_string()) {
                LOG_ERROR("AiController: malformed response (missing content string) key=%s body=%s",
                          key.c_str(),
                          TruncateForLog(r.text, 300).c_str());
                return {false, "API Error: malformed response (missing content)"};
            }
            return { true, itContent->get<std::string>() };
        } catch (const std::exception& ex) {
            LOG_ERROR("AiController: parse exception key=%s err=%s body=%s",
                      key.c_str(),
                      ex.what(),
                      TruncateForLog(r.text, 300).c_str());
            return { false, std::string("API Error: invalid JSON response (") + ex.what() + ")" };
        }
    }

    LOG_ERROR("AiController: API failure key=%s status=%d cprErr=%d msg=%s",
              key.c_str(),
              static_cast<int>(r.status_code),
              static_cast<int>(r.error.code),
              r.error.message.c_str());
    return { false, "API Error: " + std::to_string(r.status_code) };
}

