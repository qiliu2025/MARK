#pragma once

#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <string>

namespace UiPreferencesScope {
inline std::wstring ForInstallation(const std::filesystem::path& applicationDirectory) {
    auto identity = applicationDirectory.lexically_normal().wstring();
    for (auto& character : identity) character = static_cast<wchar_t>(std::towlower(character));
    std::error_code error;
    const auto marker = applicationDirectory.parent_path() / L"MARK.installation.json";
    const auto modified = std::filesystem::last_write_time(marker, error);
    if (!error) identity += L":" + std::to_wstring(modified.time_since_epoch().count());
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto character : identity) {
        hash ^= static_cast<std::uint16_t>(character);
        hash *= 1099511628211ULL;
    }
    return std::to_wstring(hash);
}
}
