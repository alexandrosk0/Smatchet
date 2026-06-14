// Regression guard for security audit #10 — "OfflineQueue serialized draft string bypasses
// audit redaction". OfflineQueueService::QueueCreateOffline enqueues the FULL serialized draft
// (replay must reconstruct real field values) but the AUDIT-TRAIL copy must be redacted
// structurally: the draft is parsed back into a JSON object and run through the audit trail's
// key-name redactor (BackendAuditTrail::RedactJson) BEFORE it lands in the trail, so sensitive
// nested `fields` values become "[redacted]". This test exercises that exact composition (the
// MakeAuditDraft helper is anonymous-namespace; its constituents IssueDraftHelpers::ToJson +
// BackendAuditTrail::RedactJson are the public security contract).

#include "BackendAuditTrail.h"
#include "IssueDraft.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>
#include <string>

namespace {

// Mirror of OfflineQueueService::MakeAuditDraft: parse the serialized payload and redact it.
nlohmann::json AuditDraftFromPayload(const std::string& payload) {
    nlohmann::json parsed = nlohmann::json::parse(payload, nullptr, false);
    if (parsed.is_discarded()) {
        return nlohmann::json{{"redaction_error", "draft payload not parseable for audit redaction"}};
    }
    return BackendAuditTrail::RedactJson(parsed);
}

} // namespace

TEST_CASE("OfflineQueue draft audit: sensitive field value is redacted in the audit copy but not the payload") {
    IssueDraft draft;
    draft.ProjectKey = "PROJ";
    draft.IssueTypeId = "10001";
    draft.FieldValues["token"] = "super-secret-token-value";
    draft.FieldValues["api_key"] = "ak_live_1234567890";
    draft.FieldValues["password"] = "hunter2";
    draft.FieldValues["priority"] = "High"; // non-sensitive — must survive intact

    const std::string payload = IssueDraftHelpers::ToJson(draft);

    // The ENQUEUED/REPLAYED payload retains every real value (replay must reconstruct the draft).
    CHECK(payload.find("super-secret-token-value") != std::string::npos);
    CHECK(payload.find("ak_live_1234567890") != std::string::npos);
    CHECK(payload.find("hunter2") != std::string::npos);

    // The AUDIT copy is structurally redacted.
    const nlohmann::json audit = AuditDraftFromPayload(payload);
    REQUIRE(audit.is_object());
    REQUIRE(audit.contains("fields"));
    const nlohmann::json& fields = audit["fields"];
    CHECK(fields["token"].get<std::string>() == "[redacted]");
    CHECK(fields["api_key"].get<std::string>() == "[redacted]");
    CHECK(fields["password"].get<std::string>() == "[redacted]");
    CHECK(fields["priority"].get<std::string>() == "High");

    // Belt-and-suspenders: the secret strings must not appear anywhere in the serialized audit copy.
    const std::string auditDump = audit.dump();
    CHECK(auditDump.find("super-secret-token-value") == std::string::npos);
    CHECK(auditDump.find("ak_live_1234567890") == std::string::npos);
    CHECK(auditDump.find("hunter2") == std::string::npos);
    CHECK(audit["projectKey"].get<std::string>() == "PROJ");
}

TEST_CASE("OfflineQueue draft audit: nested/array sensitive values inside a field are redacted") {
    IssueDraft draft;
    draft.ProjectKey = "PROJ";
    draft.IssueTypeId = "10001";
    // A field whose value is itself a JSON object/array carrying a sensitive nested key. ToJson
    // stores FieldValues as strings, so the redactor sees the string value — confirm a sensitive
    // FIELD ID (description/comment are on the sensitive list) redacts regardless of value shape.
    draft.FieldValues["description"] = "contains a token=abc embedded in body text";
    draft.FieldValues["comment"] = "[{\"token\":\"nested-secret\"}]";

    const std::string payload = IssueDraftHelpers::ToJson(draft);
    CHECK(payload.find("nested-secret") != std::string::npos); // payload keeps the real value

    const nlohmann::json audit = AuditDraftFromPayload(payload);
    const nlohmann::json& fields = audit["fields"];
    CHECK(fields["description"].get<std::string>() == "[redacted]");
    CHECK(fields["comment"].get<std::string>() == "[redacted]");
    CHECK(audit.dump().find("nested-secret") == std::string::npos);
}

TEST_CASE("OfflineQueue draft audit: unparseable payload yields a placeholder, never the raw string") {
    const nlohmann::json audit = AuditDraftFromPayload("{not valid json");
    REQUIRE(audit.is_object());
    CHECK(audit.contains("redaction_error"));
    CHECK(audit.dump().find("not valid json") == std::string::npos);
}
