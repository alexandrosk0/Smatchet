#include "AiController.h"

#include <cstdint>
#include <string>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "NetworkUsageTracker.h"

AiController::AiResult AiController::AnalyzeTicket(const std::string& key,
                                                   const std::string& summary,
                                                   const std::string& apiKey) {
    if (apiKey.empty()) return { false, "No API Key provided." };

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

    if (r.status_code == 200) {
        auto j = nlohmann::json::parse(r.text);
        return { true, j["choices"][0]["message"]["content"].get<std::string>() };
    }

    return { false, "API Error: " + std::to_string(r.status_code) };
}

