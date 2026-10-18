#include "InstallSpace.h"
#include "SetupFiles.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 2, "Pass a fresh test directory.");
        const auto root = fs::absolute(argv[1]).lexically_normal();
        Require(!fs::exists(root), "The test directory must not exist.");
        constexpr std::uint64_t GB = 1'000'000'000ULL;
        const mark::setup::SpacePlan plan{10 * GB, 8 * GB, {2 * GB, 3 * GB}};
        const auto one = mark::setup::CalculateSpace(plan, GB, 0, 1);
        const auto two = mark::setup::CalculateSpace(plan, GB, 0, 2);
        Require(one.peak == 12 * GB && two.peak == 13 * GB, "Preparation peak is incorrect.");
        Require(one.required == 12'360'000'000ULL, "Growth allowance is incorrect.");
        Require(mark::setup::ChoosePreparationWorkers(plan, GB, 0, one.required, true) == 1,
            "Low-space installation did not fall back to one worker.");
        Require(mark::setup::ChoosePreparationWorkers(plan, GB, 0, two.required, true) == 2,
            "Parallel preparation was not enabled when space was available.");
        Require(mark::setup::ChoosePreparationWorkers(plan, GB, 0, two.required, false) == 1,
            "Memory limit was ignored.");
        Require(mark::setup::CalculateSpace({GB, GB, {GB / 8, GB / 4}}, GB, 0, 1).peak == 2 * GB,
            "Archive extraction peak was ignored.");
        bool rejected = false;
        try { mark::setup::CalculateSpace({}, GB, 0, 1); } catch (const std::runtime_error&) { rejected = true; }
        Require(rejected, "Missing package metadata was accepted.");
        rejected = false;
        try { mark::setup::AddBytes(UINT64_MAX, 1); } catch (const std::runtime_error&) { rejected = true; }
        Require(rejected, "Size overflow was accepted.");
        fs::create_directories(root);
        std::atomic_bool cancelled = false;
        const auto makeSource = [&](const wchar_t* name) { const auto p = root / name; fs::create_directory(p); std::ofstream(p / L"ready.txt") << "ready"; return p; };
        auto source = makeSource(L"locked-source");
        const auto destination = root / L"installed";
        HANDLE handle = CreateFileW(source.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        Require(handle != INVALID_HANDLE_VALUE, "Unable to hold the test directory open.");
        std::jthread release([handle] { std::this_thread::sleep_for(std::chrono::milliseconds(350)); CloseHandle(handle); });
        Require(mark::setup::PublishPreparedDirectory(source, destination, cancelled, std::chrono::seconds(3)) == 0,
            "A temporary folder lock was not retried.");
        release.join();
        Require(fs::exists(destination / L"ready.txt") && !fs::exists(source), "Published files were lost.");
        source = makeSource(L"existing-source");
        Require(mark::setup::PublishPreparedDirectory(source, destination, cancelled) == ERROR_DIR_NOT_EMPTY,
            "An existing installation was overwritten.");
        Require(fs::exists(source / L"ready.txt") && fs::exists(destination / L"ready.txt"), "Existing files were changed.");
        const auto legacySource = makeSource(L"legacy-source");
        const auto legacyTarget = root / L"legacy-open-target";
        fs::create_directory(legacyTarget);
        handle = CreateFileW(legacyTarget.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        Require(handle != INVALID_HANDLE_VALUE, "Unable to reproduce an open folder handle.");
        const auto removed = RemoveDirectoryW(legacyTarget.c_str());
        std::error_code renameError;
        fs::rename(legacySource, legacyTarget, renameError);
        CloseHandle(handle);
        Require(renameError ? fs::exists(legacySource / L"ready.txt") : fs::exists(legacyTarget / L"ready.txt"),
            "The locked-target control lost its source files.");
        std::cout << "Open-target control: removed=" << removed << " rename_error=" << renameError.value() << '\n';
        source = makeSource(L"open-target-source");
        const auto openTarget = root / L"open-empty-target";
        fs::create_directory(openTarget);
        handle = CreateFileW(openTarget.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        Require(handle != INVALID_HANDLE_VALUE, "Unable to open the target directory.");
        const auto published = mark::setup::PublishPreparedDirectory(source, openTarget, cancelled);
        CloseHandle(handle);
        Require(published == 0 && fs::exists(openTarget / L"ready.txt"), "An open empty target folder prevented installation.");
        cancelled = true;
        Require(mark::setup::PublishPreparedDirectory(source, root / L"cancelled", cancelled) == ERROR_CANCELLED,
            "Cancellation was ignored.");
        std::cout << "Setup space calculation and folder publication checks passed.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
