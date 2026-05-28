#include "TrackerFieldPayload.h"

#include "TrackerFieldPayloadPure.h"

namespace TrackerFieldPayload {

bool FieldUsesAdfDocument(const TrackerField& field) {
    return TrackerFieldPayloadPure::FieldUsesAdfDocument(field);
}

std::string ExtractIssueKey(const std::string& value) {
    return TrackerFieldPayloadPure::ExtractIssueKey(value);
}

bool IsArrayLike(const TrackerField& field) {
    return TrackerFieldPayloadPure::IsArrayLike(field);
}

bool BuildValue(const TrackerField& field, const std::vector<std::string>& rawValues, nlohmann::json& outValue,
                std::string& outError) {
    return TrackerFieldPayloadPure::BuildValue(field, rawValues, outValue, outError);
}

std::vector<std::string> SplitCommaSeparatedValues(const std::string& input) {
    return TrackerFieldPayloadPure::SplitCommaSeparatedValues(input);
}

bool IsSprintField(const TrackerField& field) {
    return TrackerFieldPayloadPure::IsSprintField(field);
}

std::string ResolveSprintIdForAgile(const TrackerField& field, const std::string& rawValue) {
    return TrackerFieldPayloadPure::ResolveSprintIdForAgile(field, rawValue);
}

std::string ResolveDisplayValueForSubmittedSelection(const TrackerField& field, const std::string& value) {
    return TrackerFieldPayloadPure::ResolveDisplayValueForSubmittedSelection(field, value);
}

} // namespace TrackerFieldPayload
