#include "UiPreferencesScope.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

int wmain(int argc, wchar_t** argv) {
    namespace fs = std::filesystem;
    if (argc != 2) return 2;
    const auto root = fs::absolute(argv[1]);
    if (fs::exists(root)) throw std::runtime_error("Use a new test directory.");
    fs::create_directories(root / L"first/software");
    fs::create_directories(root / L"second/software");
    const auto scope = UiPreferencesScope::ForInstallation(root / L"first/software");
    if (scope != UiPreferencesScope::ForInstallation(root / L"first/software")) return 3;
    if (scope == UiPreferencesScope::ForInstallation(root / L"second/software")) return 4;
    std::ofstream(root / L"first/MARK.installation.json") << "{}";
    const auto installed = UiPreferencesScope::ForInstallation(root / L"first/software");
    if (scope == installed) return 5;
    std::ofstream(root / L"first/software/preferences.txt") << "changed";
    if (installed != UiPreferencesScope::ForInstallation(root / L"first/software")) return 6;
    const auto marker = root / L"first/MARK.installation.json";
    fs::last_write_time(marker, fs::last_write_time(marker) + std::chrono::seconds(4));
    if (installed == UiPreferencesScope::ForInstallation(root / L"first/software")) return 7;
    std::cout << "Installation preference scope checks passed.\n";
}
