#pragma once

#include "ConfigManager.h"
#include "TicketGridSortPure.h" // CompareFieldValuesForSort / IsTrackerDateOrDateTimeField / IsAttachmentFieldId
#include "TrackerFieldSchema.h"

#include <string>
#include <unordered_map>
#include <vector>

std::string DisplayValueForTrackerDateField(const std::string& fieldId, const TrackerField* field,
                                            const std::string& currentValue, const std::string& dateFormatOption = {},
                                            int thresholdDays = 0);

class TrackerFieldCatalogIndex {
  public:
    explicit TrackerFieldCatalogIndex(const std::vector<TrackerField>& fields);

    const TrackerField* Find(const std::string& fieldId) const;
    std::string DisplayName(const std::string& fieldId) const;

  private:
    std::unordered_map<std::string, const TrackerField*> FieldById;
};

struct TicketGridColumn {
    enum class Kind { Id, FieldValue };

    enum class RenderPlan {
        PlainText,
        SpecialAttachment,
        SpecialWatchers,
        SpecialVotes,
        SpecialWorklog,
        SpecialProgress,
        SpecialIssueRestriction,
        SpecialTimeSpent,
        Labels,
        Cascading,
        MultiSelect,
        SingleSelect,
        DateTimeEditor,
        TextEditor
    };

    Kind ColumnKind = Kind::Id;
    std::string Key;
    std::string Label;
    std::string FieldId;
    RenderPlan Plan = RenderPlan::PlainText;
    bool IsDateLike = false;
    bool CatalogReadOnly = false;
    bool NeedsAllowEditsCheck = false;
};

class TicketGridColumnsBuilder {
  public:
    static std::vector<TicketGridColumn> Build(const ViewDefinition& view, const TrackerFieldCatalogIndex& catalog);
};
