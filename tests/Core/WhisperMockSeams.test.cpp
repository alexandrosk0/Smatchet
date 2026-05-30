#include <doctest/doctest.h>

#include "WhisperApiClient.h"

#include <string>

using smatchet::whisper::WhisperApiClient;

TEST_CASE("WhisperApiClient: mock-response seam is initially inactive") {
    WhisperApiClient::ClearMockResponse();
    CHECK_FALSE(WhisperApiClient::MockResponseActive());
}

TEST_CASE("WhisperApiClient: SetMockResponse activates the seam") {
    WhisperApiClient::ClearMockResponse();
    WhisperApiClient::SetMockResponse("hello world");
    CHECK(WhisperApiClient::MockResponseActive());
    WhisperApiClient::ClearMockResponse();
    CHECK_FALSE(WhisperApiClient::MockResponseActive());
}

TEST_CASE("WhisperApiClient: SetMockResponse with empty string still activates") {
    WhisperApiClient::ClearMockResponse();
    WhisperApiClient::SetMockResponse("");
    CHECK(WhisperApiClient::MockResponseActive());
    WhisperApiClient::ClearMockResponse();
    CHECK_FALSE(WhisperApiClient::MockResponseActive());
}

TEST_CASE("WhisperApiClient: ClearMockResponse is idempotent") {
    WhisperApiClient::ClearMockResponse();
    WhisperApiClient::ClearMockResponse();
    WhisperApiClient::ClearMockResponse();
    CHECK_FALSE(WhisperApiClient::MockResponseActive());
}

TEST_CASE("WhisperApiClient: SetMockResponse re-activates after clear") {
    WhisperApiClient::ClearMockResponse();
    WhisperApiClient::SetMockResponse("first");
    CHECK(WhisperApiClient::MockResponseActive());
    WhisperApiClient::ClearMockResponse();
    CHECK_FALSE(WhisperApiClient::MockResponseActive());
    WhisperApiClient::SetMockResponse("second");
    CHECK(WhisperApiClient::MockResponseActive());
    WhisperApiClient::ClearMockResponse();
}

TEST_CASE("WhisperApiClient: SetMockResponse overwrites existing mock") {
    WhisperApiClient::ClearMockResponse();
    WhisperApiClient::SetMockResponse("first");
    WhisperApiClient::SetMockResponse("second");
    CHECK(WhisperApiClient::MockResponseActive());
    WhisperApiClient::ClearMockResponse();
}
