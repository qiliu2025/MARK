#pragma once
#include "DownloadFolderState.h"

namespace DownloadSettings {
struct Options {
    bool datedFolders = true;
    bool askObservationPostprocess = false;
};
inline Options Load() {
    const auto path = DownloadFolderState::FilePath().wstring();
    return {
        GetPrivateProfileIntW(L"download_options", L"dated_folders", 1, path.c_str()) != 0,
        GetPrivateProfileIntW(L"download_options", L"ask_observation_postprocess", 0, path.c_str()) != 0
    };
}
inline bool Save(const Options& options) {
    const auto path = DownloadFolderState::FilePath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    return WritePrivateProfileStringW(L"download_options", L"dated_folders",
               options.datedFolders ? L"1" : L"0", path.c_str())
        && WritePrivateProfileStringW(L"download_options", L"ask_observation_postprocess",
               options.askObservationPostprocess ? L"1" : L"0", path.c_str());
}
}
