#include "Win32PickFiles.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#include <system_error>
#include <vector>

#include <ghc/filesystem.hpp>

#include "Logger.h"

namespace {

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int n =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) {
        return std::wstring();
    }
    std::vector<wchar_t> buf(static_cast<size_t>(n) + 1u, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), buf.data(), n);
    return std::wstring(buf.data());
}

std::string WideToUtf8(const wchar_t* w, int len) {
    if (!w || len <= 0) {
        return std::string();
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return std::string();
    }
    std::vector<char> buf(static_cast<size_t>(n) + 1u, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, buf.data(), n, nullptr, nullptr);
    return std::string(buf.data());
}

bool SetInitialFolder(IFileOpenDialog* dialog, const std::wstring& dirW) {
    if (dirW.empty()) {
        return false;
    }
    std::error_code ec;
    if (!ghc::filesystem::is_directory(ghc::filesystem::path(dirW), ec)) {
        return false;
    }
    IShellItem* folder = nullptr;
    const HRESULT hr = SHCreateItemFromParsingName(dirW.c_str(), nullptr, IID_PPV_ARGS(&folder));
    if (FAILED(hr) || !folder) {
        return false;
    }
    dialog->SetFolder(folder);
    folder->Release();
    return true;
}

} // namespace

bool SmatchetWin32PickOpenFilePaths(void* hwndOwner, bool allowMultiple, const std::string& initialDirUtf8,
                                    std::string* outLastDirectoryUtf8, std::vector<std::string>& outAbsPathsUtf8) {
    outAbsPathsUtf8.clear();
    if (outLastDirectoryUtf8) {
        outLastDirectoryUtf8->clear();
    }

    const HRESULT coinit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = (coinit == S_OK);
    if (FAILED(coinit) && coinit != RPC_E_CHANGED_MODE) {
        LOG_WARN("SmatchetWin32PickOpenFilePaths: CoInitializeEx failed hr=0x%08lx",
                 static_cast<unsigned long>(coinit));
        return false;
    }

    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        LOG_WARN("SmatchetWin32PickOpenFilePaths: CoCreateInstance(FileOpenDialog) failed hr=0x%08lx",
                 static_cast<unsigned long>(hr));
        if (needUninit) {
            CoUninitialize();
        }
        return false;
    }

    DWORD flags = 0;
    dialog->GetOptions(&flags);
    flags |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
    if (allowMultiple) {
        flags |= FOS_ALLOWMULTISELECT;
    }
    dialog->SetOptions(flags);

    const std::wstring dirW = Utf8ToWide(initialDirUtf8);
    SetInitialFolder(dialog, dirW);

    hr = dialog->Show(static_cast<HWND>(hwndOwner));
    if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED) && FAILED(hr)) {
        LOG_WARN("SmatchetWin32PickOpenFilePaths: Show failed hr=0x%08lx", static_cast<unsigned long>(hr));
    }
    if (FAILED(hr)) {
        dialog->Release();
        if (needUninit) {
            CoUninitialize();
        }
        return false;
    }

    IShellItemArray* items = nullptr;
    hr = dialog->GetResults(&items);
    dialog->Release();
    if (FAILED(hr) || !items) {
        if (needUninit) {
            CoUninitialize();
        }
        return false;
    }

    DWORD count = 0;
    items->GetCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        IShellItem* item = nullptr;
        if (FAILED(items->GetItemAt(i, &item)) || !item) {
            continue;
        }
        PWSTR pathW = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &pathW)) && pathW) {
            const int len = lstrlenW(pathW);
            outAbsPathsUtf8.push_back(WideToUtf8(pathW, len));
            CoTaskMemFree(pathW);
        }
        item->Release();
    }
    items->Release();

    if (needUninit) {
        CoUninitialize();
    }

    if (outAbsPathsUtf8.empty()) {
        return false;
    }

    if (outLastDirectoryUtf8) {
        const ghc::filesystem::path p(outAbsPathsUtf8.front());
        if (p.has_parent_path()) {
            *outLastDirectoryUtf8 = p.parent_path().generic_string();
        }
    }
    return true;
}

#else

bool SmatchetWin32PickOpenFilePaths(void*, bool, const std::string&, std::string*,
                                    std::vector<std::string>& outAbsPathsUtf8) {
    outAbsPathsUtf8.clear();
    return false;
}

#endif






