#ifndef SMATCHET_WIN32_PICK_FILES_H
#define SMATCHET_WIN32_PICK_FILES_H

#include <string>
#include <vector>

/**
 * Shows the system open-file dialog (multi-select when allowMultiple).
 * @param hwndOwner optional HWND; may be null
 * @param initialDirUtf8 UTF-8 directory to start in; empty = default
 * @param outLastDirectoryUtf8 if non-null and at least one file chosen, set to parent dir of first file (UTF-8)
 * @param outAbsPathsUtf8 absolute paths of chosen files
 * @return true if user confirmed with at least one file
 */
bool SmatchetWin32PickOpenFilePaths(void* hwndOwner, bool allowMultiple, const std::string& initialDirUtf8,
                                    std::string* outLastDirectoryUtf8, std::vector<std::string>& outAbsPathsUtf8);

#endif






