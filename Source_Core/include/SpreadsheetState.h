#ifndef SPREADSHEET_STATE_H
#define SPREADSHEET_STATE_H

#include <string>
#include <cstdio>

struct SpreadsheetState {
    std::string SelectedId;
    std::string EditingId;
    std::string EditingFieldId;
    int EditingColumn = -1;
    char EditBuffer[512] = "";

    void SelectRow(const std::string& id) { SelectedId = id; }
    void ClearEditing() { EditingId = ""; EditingFieldId = ""; EditingColumn = -1; }

    void StartEditing(const std::string& id, int col, const std::string& val) {
        EditingId = id;
        EditingFieldId.clear();
        EditingColumn = col;
        CopyToEditBuffer(val);
    }

    void StartEditingField(const std::string& id, const std::string& fieldId, const std::string& val) {
        EditingId = id;
        EditingFieldId = fieldId;
        EditingColumn = -1;
        CopyToEditBuffer(val);
    }

    bool IsEditingField(const std::string& id, const std::string& fieldId) const {
        return EditingId == id && EditingFieldId == fieldId;
    }

private:
    void CopyToEditBuffer(const std::string& val) {
        std::snprintf(EditBuffer, sizeof(EditBuffer), "%s", val.c_str());
        EditBuffer[sizeof(EditBuffer) - 1] = '\0';
    }
};

#endif