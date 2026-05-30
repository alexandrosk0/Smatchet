// WhisperApiPayload — doctest coverage for the WhisperApiClient pure helpers
// (response parsing + endpoint URL). Exercises happy + malformed + empty +
// shape-error paths without firing a real HTTP request.
//
// The recorded sample lives at tests/_mocks/openai-whisper-response.json; the
// shape is copied inline as a literal here so the test passes whether or not
// the fixture is bundled at runtime. Future scenario / bucket-E tests can
// read the same fixture from disk.

#include <doctest/doctest.h>

#include "WhisperApiClient.h"

#include <string>

using smatchet::whisper::pure::CloudEndpointUrl;
using smatchet::whisper::pure::ParseWhisperResponse;

TEST_CASE("WhisperApiClient: cloud endpoint URL is the documented OpenAI Whisper path") {
    const std::string url = CloudEndpointUrl();
    CHECK(url == "https://api.openai.com/v1/audio/transcriptions");
}

TEST_CASE("ParseWhisperResponse: documented {\"text\": \"...\"} shape") {
    std::string text;
    std::string err;
    CHECK(ParseWhisperResponse("{\"text\":\"Hello world.\"}", text, err));
    CHECK(text == "Hello world.");
    CHECK(err.empty());
}

TEST_CASE("ParseWhisperResponse: verbose-json with extra fields still parses") {
    // verbose_json shape carries `task`, `language`, `duration`, `segments`,
    // and a top-level `text`. Whisper docs guarantee the `text` field across
    // both response_format values.
    const std::string body =
        "{\"task\":\"transcribe\",\"language\":\"en\",\"duration\":1.0,\"segments\":[],"
        "\"text\":\"Hello world.\"}";
    std::string text;
    std::string err;
    CHECK(ParseWhisperResponse(body, text, err));
    CHECK(text == "Hello world.");
    CHECK(err.empty());
}

TEST_CASE("ParseWhisperResponse: empty body is a parse failure") {
    std::string text;
    std::string err;
    CHECK_FALSE(ParseWhisperResponse("", text, err));
    CHECK(text.empty());
    CHECK(!err.empty());
}

TEST_CASE("ParseWhisperResponse: malformed JSON is a parse failure") {
    std::string text;
    std::string err;
    CHECK_FALSE(ParseWhisperResponse("{not json", text, err));
    CHECK(text.empty());
    CHECK(!err.empty());
}

TEST_CASE("ParseWhisperResponse: non-object JSON is rejected") {
    std::string text;
    std::string err;
    CHECK_FALSE(ParseWhisperResponse("[\"text\"]", text, err));
    CHECK(text.empty());
    CHECK(!err.empty());
}

TEST_CASE("ParseWhisperResponse: missing text field is rejected") {
    std::string text;
    std::string err;
    CHECK_FALSE(ParseWhisperResponse("{\"foo\":\"bar\"}", text, err));
    CHECK(text.empty());
    CHECK(!err.empty());
}

TEST_CASE("ParseWhisperResponse: non-string text field is rejected") {
    std::string text;
    std::string err;
    CHECK_FALSE(ParseWhisperResponse("{\"text\":42}", text, err));
    CHECK(text.empty());
    CHECK(!err.empty());
}

TEST_CASE("ParseWhisperResponse: empty-string text is a success with empty out") {
    std::string text;
    std::string err;
    CHECK(ParseWhisperResponse("{\"text\":\"\"}", text, err));
    CHECK(text.empty());
    CHECK(err.empty());
}
