#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>

inline bool DownloadFileLooksValid(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0) return false;
    std::ifstream input(path, std::ios::binary);
    std::array<char, 512> buffer{};
    input.read(buffer.data(), buffer.size());
    std::string prefix(buffer.data(), static_cast<std::size_t>(input.gcount()));
    if (prefix.empty()) return false;
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    if (extension == L".gz") return size >= 20 && prefix.starts_with("\x1f\x8b");
    if (extension == L".z") return size >= 3 && prefix.starts_with("\x1f\x9d");
    if (extension == L".zip") return size >= 22 && prefix.starts_with("PK");
    if (extension == L".7z") return size >= 32 && prefix.starts_with("7z\xbc\xaf\x27\x1c");
    std::transform(prefix.begin(), prefix.end(), prefix.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto first = prefix.find_first_not_of(" \r\n\t");
    if (first != std::string::npos) prefix.erase(0, first);
    return !prefix.starts_with("<!doctype html") && !prefix.starts_with("<html")
        && !prefix.starts_with("<head") && !prefix.starts_with("<body");
}
