#include "HttpClient.h"

#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <filesystem>
#include <algorithm>
#include <iostream>
#include <stop_token>
#include <chrono>
#include <thread>

int wmain(const int argumentCount, wchar_t** arguments) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    const std::filesystem::path destination = argumentCount > 1
        ? std::filesystem::path(arguments[1])
        : std::filesystem::path(L"geonet-download-smoke.26d.gz");
    const int maximumSeconds = argumentCount > 3 ? std::clamp(_wtoi(arguments[3]), 2, 300) : 90;
    const std::wstring mode = argumentCount > 4 ? arguments[4] : L"download";
    std::stop_source stop;
    auto control = std::make_shared<DownloadControl>();
    std::atomic_bool finished = false, paused = false, resumed = false;
    std::atomic_uint64_t bytes = 0, totalBytes = 0;
    std::atomic_int64_t firstByteMilliseconds = -1;
    const auto start = std::chrono::steady_clock::now();
    std::jthread timer([&](std::stop_token timerStop) {
        while (!finished && !timerStop.stop_requested()) {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= std::chrono::seconds(maximumSeconds)
                || (mode == L"cancel" && elapsed >= std::chrono::milliseconds(500))) {
                stop.request_stop();
                control->resume();
                return;
            }
            if (mode == L"pause" && bytes > 0 && !paused.exchange(true)) {
                control->pause();
                for (int i = 0; i < 75 && !finished && !timerStop.stop_requested(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                control->resume();
                resumed = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    const auto result = HttpDownloadFile(
        argumentCount > 2 ? arguments[2] : L"geonet-jp://000842/2026-08-25/08422370.26d.gz",
        destination,
        stop.get_token(),
        [&](std::uint64_t transferred, std::uint64_t total) {
            bytes = transferred;
            totalBytes = total;
            if (transferred > 0 && firstByteMilliseconds < 0)
                firstByteMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
        },
        true, control, maximumSeconds, 1);
    finished = true;
    timer.request_stop();
    timer.join();
    std::wcout << L"control=" << mode << L" paused=" << paused << L" resumed=" << resumed
        << L" cancelled=" << result.cancelled << L"\n";
    std::wcout << L"transferred=" << bytes << L" total=" << totalBytes
        << L" first_byte_ms=" << firstByteMilliseconds << L"\n";
    if (mode == L"cancel") {
        if (!result.cancelled || std::filesystem::exists(destination)
            || std::filesystem::exists(destination.wstring() + L".part")) return 2;
        return 0;
    }
    if (!result.success) {
        std::wcerr << result.error << L"\n";
        return 1;
    }
    if (!result.error.empty()) {
        std::wcerr << L"Successful transfer retained a stale error: " << result.error << L"\n";
        return 3;
    }
    std::wcout << destination.wstring() << L" "
               << result.bytesWritten << L" bytes\n";
    return 0;
}
