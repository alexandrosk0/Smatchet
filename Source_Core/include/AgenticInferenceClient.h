#ifndef SMATCHET_AGENTIC_INFERENCE_CLIENT_H
#define SMATCHET_AGENTIC_INFERENCE_CLIENT_H

#include "AgenticInferenceClientPure.h"
#include "ITrackerClient.h" // TrackerIssueComment

#include <string>
#include <vector>

// Synchronous LLM-call wrapper that produces `ProposalDraft` rows for the
// agentic triage flow. Reuses the existing AiClientFactory + IAiClient
// infrastructure (OpenAI / Anthropic / Ollama uniformly) — no new HTTP code
// paths are introduced here.
//
// Caller contract: this entry point is worker-thread-only. Wrap in
// `LaunchBackgroundTask` (Pattern A) when invoking from a UI-thread context;
// pillar 2 mandates no synchronous HTTP from ImGui frames.
//
// AgentProposal SQLite persistence (id, lifecycle state, source-tracker
// linkage) lives in AgentProposalStore; this client returns in-memory
// ProposalDraft rows only.

class AgenticInferenceClient {
  public:
    AgenticInferenceClient() = default;

    // Builds the system+user prompt, invokes the configured AI provider,
    // accumulates the streamed deltas, strips any markdown code-fence
    // wrapping, and runs the schema parser. Returns true on a parseable
    // envelope (even if zero proposals survive validation); false on
    // HTTP error / cancellation / unparseable response with outError set.
    //
    // The N most-recent comments parameter is bounded internally to 20
    // (decision #3 → token-budget cap). issueBody is trimmed to 8 KB.
    bool RequestProposals(const std::string& issueBody, const std::vector<TrackerIssueComment>& comments,
                          std::vector<AgenticInferenceClientPure::ProposalDraft>& outDrafts,
                          std::string& outError);
};

#endif // SMATCHET_AGENTIC_INFERENCE_CLIENT_H
