#include "ObservationPostprocess.h"
#include <windows.h>
#include <iostream>
#include <stop_token>
#include <thread>
#include <chrono>

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) return 2;
    const std::wstring mode = argv[1];
    std::vector<std::filesystem::path> files;
    for (int i = 3; i < argc; ++i) files.emplace_back(argv[i]);
    const auto estimate = ObservationPostprocess::Inspect(files, argv[2]);
    std::cout << "files=" << estimate.files.size() << " sufficient=" << estimate.sufficient
        << " estimate_error=" << !estimate.error.empty() << '\n';
    for (const auto& volume : estimate.volumes)
        std::cout << "required=" << volume.required << " available=" << volume.available << '\n';
    if (mode == L"--estimate") return estimate.error.empty() ? 0 : 1;
    std::stop_source stop;
    if (mode == L"--cancel") stop.request_stop();
    std::jthread timedCancel;
    if (mode == L"--cancel-during") timedCancel = std::jthread([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); stop.request_stop();
    });
    const auto result = ObservationPostprocess::Run(estimate, argv[2], stop.get_token(),
        [&](std::size_t completed, std::size_t, const std::wstring&) {
            if (mode == L"--cancel-after-one" && completed >= 1) stop.request_stop();
        }, (mode == L"--cancel-after-one" || mode == L"--serial") ? 1u : 0u);
    std::cout << "succeeded=" << result.succeeded << " failed=" << result.failed
        << " cancelled=" << result.cancelled << " needsSpace=" << result.needsSpace
        << " remaining=" << result.remaining.size() << '\n';
    for (const auto& error : result.errors) {
        int n = WideCharToMultiByte(CP_UTF8, 0, error.data(), static_cast<int>(error.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, error.data(), static_cast<int>(error.size()), utf8.data(), n, nullptr, nullptr);
        std::cout << utf8 << '\n';
    }
    return 0;
}
