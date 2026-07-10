#include <doctest/doctest.h>

#include "SmatchetPreferencesUi_detail.h"

#include <string>

// feat/dec-savediscard — bucket-A coverage for the pure working-copy helpers
// behind the Assistant Preferences tab's explicit Save / Discard flow. The tab
// stages AI-field edits into a working `TrackerConfig` and only commits them
// into the live cfg on Save; Discard reverts. CopyAssistantAiFields drives both
// seed + commit; AssistantAiFieldsDiffer drives the `Assistant *` dirty marker
// and the Save/Discard enable state. Both are defined `inline` in the _detail
// header behind SMATCHET_WITH_AI, so the doctest rig needs no extra .cpp link.

#if defined(SMATCHET_WITH_AI)

using SmatchetPreferencesUiDetail::AssistantAiFieldsDiffer;
using SmatchetPreferencesUiDetail::CopyAssistantAiFields;

TEST_CASE("AssistantAiFieldsDiffer — identical configs are not dirty" * doctest::test_suite("[high-risk]")) {
    TrackerConfig a;
    TrackerConfig b;
    CHECK_FALSE(AssistantAiFieldsDiffer(a, b));
}

TEST_CASE("AssistantAiFieldsDiffer — an edit to any staged AI field reads as dirty") {
    TrackerConfig saved;
    saved.AiProviderKind = 0;
    saved.AiApiKey = "sk-old";
    saved.AiModelOpenAi = "gpt-4o-mini";
    saved.AiReasoningEffort = "auto";
    saved.AgentsMdGlobalPath = "/old/agents.md";
    saved.AgentsMdAutoDiscoverProject = false;

    SUBCASE("provider kind") {
        TrackerConfig work = saved;
        work.AiProviderKind = 2;
        CHECK(AssistantAiFieldsDiffer(work, saved));
    }
    SUBCASE("api key") {
        TrackerConfig work = saved;
        work.AiApiKey = "sk-new";
        CHECK(AssistantAiFieldsDiffer(work, saved));
    }
    SUBCASE("model") {
        TrackerConfig work = saved;
        work.AiModelOpenAi = "gpt-4o";
        CHECK(AssistantAiFieldsDiffer(work, saved));
    }
    SUBCASE("reasoning effort") {
        TrackerConfig work = saved;
        work.AiReasoningEffort = "high";
        CHECK(AssistantAiFieldsDiffer(work, saved));
    }
    SUBCASE("agents.md path") {
        TrackerConfig work = saved;
        work.AgentsMdGlobalPath = "/new/agents.md";
        CHECK(AssistantAiFieldsDiffer(work, saved));
    }
    SUBCASE("auto-discover toggle") {
        TrackerConfig work = saved;
        work.AgentsMdAutoDiscoverProject = true;
        CHECK(AssistantAiFieldsDiffer(work, saved));
    }
}

TEST_CASE("CopyAssistantAiFields — Save semantics: working AI fields flush into cfg, non-AI fields untouched") {
    TrackerConfig work;
    work.AiProviderKind = 3;
    work.AiApiKey = "sk-staged";
    work.AiAnthropicApiKey = "ant-staged";
    work.AiDeepSeekApiKey = "ds-staged";
    work.AiBaseUrl = "https://proxy.example";
    work.AiOllamaBaseUrl = "http://localhost:11434";
    work.AiDeepSeekBaseUrl = "https://api.deepseek.com";
    work.AiModelOpenAi = "gpt-4o";
    work.AiModelAnthropic = "claude-sonnet-4-6";
    work.AiModelOllama = "llama3";
    work.AiModelDeepSeek = "deepseek-chat";
    work.AiReasoningEffort = "medium";
    work.AiAllowCustomEndpointOpenAi = true;
    work.AiAllowCustomEndpointAnthropic = true;
    work.AgentsMdGlobalPath = "/g/agents.md";
    work.ProjectAgentsMdPath = "/p/agents.md";
    work.AgentsMdAutoDiscoverProject = true;

    TrackerConfig cfg;
    // A non-AI field that Save must NOT clobber.
    cfg.Email = "user@company.com";
    cfg.Domain = "company.atlassian.net";

    CopyAssistantAiFields(work, cfg);

    // AI fields committed.
    CHECK(cfg.AiProviderKind == 3);
    CHECK(cfg.AiApiKey == "sk-staged");
    CHECK(cfg.AiAnthropicApiKey == "ant-staged");
    CHECK(cfg.AiDeepSeekApiKey == "ds-staged");
    CHECK(cfg.AiBaseUrl == "https://proxy.example");
    CHECK(cfg.AiModelOpenAi == "gpt-4o");
    CHECK(cfg.AiModelDeepSeek == "deepseek-chat");
    CHECK(cfg.AiReasoningEffort == "medium");
    CHECK(cfg.AiAllowCustomEndpointOpenAi);
    CHECK(cfg.AiAllowCustomEndpointAnthropic);
    CHECK(cfg.AgentsMdGlobalPath == "/g/agents.md");
    CHECK(cfg.ProjectAgentsMdPath == "/p/agents.md");
    CHECK(cfg.AgentsMdAutoDiscoverProject);
    // Non-AI fields preserved.
    CHECK(cfg.Email == "user@company.com");
    CHECK(cfg.Domain == "company.atlassian.net");

    // After a Save the two are no longer dirty.
    CHECK_FALSE(AssistantAiFieldsDiffer(work, cfg));
}

TEST_CASE("CopyAssistantAiFields — Discard semantics: working reverts to cfg, dropping the edit") {
    TrackerConfig cfg;
    cfg.AiApiKey = "sk-saved";
    cfg.AiModelOpenAi = "gpt-4o-mini";
    cfg.AiReasoningEffort = "auto";

    TrackerConfig work = cfg;
    // User edits the working copy...
    work.AiApiKey = "sk-typed-but-not-saved";
    work.AiReasoningEffort = "high";
    CHECK(AssistantAiFieldsDiffer(work, cfg));

    // Discard: cfg -> work.
    CopyAssistantAiFields(cfg, work);

    CHECK(work.AiApiKey == "sk-saved");
    CHECK(work.AiReasoningEffort == "auto");
    CHECK_FALSE(AssistantAiFieldsDiffer(work, cfg));
}

// #1706 — closing the Preferences window without Save is a Discard-on-close: it
// must revert BOTH the working copy AND the InputText buffers. The pre-#1706 bug
// reset only the working-copy latch, leaving forceReseed unset, so the reopened
// tab kept displaying the discarded edit text (the function-static s_bufs buffers
// stayed seeded). This locks that both latches move together — removing the
// forceReseed=true from ResetAssistantPrefsSeedLatchesOnClose re-fails the second
// CHECK.
TEST_CASE("ResetAssistantPrefsSeedLatchesOnClose — reverts working copy AND forces buffer reseed" *
          doctest::test_suite("[high-risk]")) {
    using SmatchetPreferencesUiDetail::ResetAssistantPrefsSeedLatchesOnClose;

    // Pre-close state: window open, working copy seeded, no reseed pending.
    bool workingSeeded = true;
    bool forceReseed = false;

    ResetAssistantPrefsSeedLatchesOnClose(workingSeeded, forceReseed);

    CHECK_FALSE(workingSeeded); // reopen re-seeds the working copy from cfg
    CHECK(forceReseed);         // reopen also reseeds the InputText buffers (#1706)

    // Idempotent-ish: calling again from the reset state keeps both latched correctly.
    ResetAssistantPrefsSeedLatchesOnClose(workingSeeded, forceReseed);
    CHECK_FALSE(workingSeeded);
    CHECK(forceReseed);
}

#endif // SMATCHET_WITH_AI
