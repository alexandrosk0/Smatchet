#ifndef SPREADSHEET_STATE_H
#define SPREADSHEET_STATE_H

#include <string>

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
#ifdef _WIN32
        strncpy_s(EditBuffer, val.c_str(), sizeof(EditBuffer));
#else
        strncpy(EditBuffer, val.c_str(), sizeof(EditBuffer));
#endif
    }

    void StartEditingField(const std::string& id, const std::string& fieldId, const std::string& val) {
        EditingId = id;
        EditingFieldId = fieldId;
        EditingColumn = -1;
#ifdef _WIN32
        strncpy_s(EditBuffer, val.c_str(), sizeof(EditBuffer));
#else
        strncpy(EditBuffer, val.c_str(), sizeof(EditBuffer));
#endif
    }

    bool IsEditingField(const std::string& id, const std::string& fieldId) const {
        return EditingId == id && EditingFieldId == fieldId;
    }
};

#endif