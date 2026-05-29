#include "TicketFieldEditorDescriptionPure.h"

#include <doctest/doctest.h>

TEST_CASE("IsDescriptionLikeFieldId: known description-like field IDs") {
    CHECK(IsDescriptionLikeFieldId("body") == true);
    CHECK(IsDescriptionLikeFieldId("Body") == true);
    CHECK(IsDescriptionLikeFieldId("description") == true);
    CHECK(IsDescriptionLikeFieldId("customDescription") == true);
}

TEST_CASE("IsDescriptionLikeFieldId: non-description field IDs") {
    CHECK(IsDescriptionLikeFieldId("environment") == false);
    CHECK(IsDescriptionLikeFieldId("summary") == false);
    CHECK(IsDescriptionLikeFieldId("status") == false);
    CHECK(IsDescriptionLikeFieldId("") == false);
}
