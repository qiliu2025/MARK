#pragma once

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>

inline std::filesystem::path VmfVariantDirectory(const std::wstring& productId) {
    const auto hour = productId.rfind(L"_H");
    const auto variant = productId.substr(0, hour);
    if (variant.empty() || variant.find(L"..") != std::wstring::npos
        || !std::all_of(variant.begin(), variant.end(), [](wchar_t c) {
            return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')
                || (c >= L'0' && c <= L'9') || c == L'_' || c == L'.' || c == L'-';
        })) throw std::runtime_error("Invalid VMF product identifier.");
    // Forecast and operational grids share filenames. Keep their model/grid/grade identity in the path.
    return variant;
}
