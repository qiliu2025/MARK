#pragma once

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <filesystem>
#include <string>

namespace DownloadFolderState {

inline std::filesystem::path FilePath() {
    // Optional portable/test profile; normal installations retain their existing INI.
    std::wstring overridePath(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"GNSS_DOWNLOADER_UI_STATE_PATH",
        overridePath.data(), static_cast<DWORD>(overridePath.size()));
    if (length > 0 && length < overridePath.size()) {
        overridePath.resize(length);
        const std::filesystem::path path(overridePath);
        if (path.is_absolute()) return path;
    }
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData))) {
        const auto path = std::filesystem::path(localAppData)
            / L"GNSSDownloader" / L"ui_state.ini";
        CoTaskMemFree(localAppData);
        return path;
    }
    return std::filesystem::current_path() / L"ui_state.ini";
}

inline std::wstring Load(const wchar_t* key,
                         const std::filesystem::path& fallback) {
    const auto path = FilePath().wstring();
    std::wstring value(32'768, L'\0');
    const DWORD length = GetPrivateProfileStringW(
        L"download_folders", key, L"", value.data(),
        static_cast<DWORD>(value.size()), path.c_str());
    if (length == 0 || length >= value.size() - 1) {
        return fallback.wstring();
    }
    value.resize(length);
    return value;
}

inline bool Save(const wchar_t* key, const std::wstring& folder) {
    if (folder.empty()) return false;
    const auto path = FilePath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    return WritePrivateProfileStringW(
        L"download_folders", key, folder.c_str(), path.c_str()) != FALSE;
}

}  // namespace DownloadFolderState
