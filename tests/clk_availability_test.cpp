#include "ClkAvailability.h"
#include "DateUtil.h"

#include <windows.h>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>

int wmain() {
    const QueryDate date = MakeQueryDate(2025, 1, 1);
    const int week = CalculateGpsWeek(date);
    const std::string listing =
        "<a href=\"COD0OPSFIN_20250010000_01D_05S_CLK.CLK.gz\">x</a>\n"
        "IGS0OPSFIN_20250010000_01D_30S_CLK.CLK.gz\n"
        "igs" + std::to_string(week) + "3.clk_30s.Z\n"
        "igr" + std::to_string(week) + "3.clk.Z\n"
        "ignore.sp3.Z\n";
    const auto parsed = ParseClkDirectoryListing(
        listing, week, L"https://example.invalid/products/", "OPS");
    assert(parsed.size() == 4);
    assert(std::ranges::any_of(parsed, [](const auto& file) {
        return file.series == "COD0OPSFIN" && file.interval == "05S";
    }));
    assert(std::ranges::any_of(parsed, [](const auto& file) {
        return file.series == "IGS0OPSFIN" && file.interval == "30S";
    }));
    assert(std::ranges::any_of(parsed, [](const auto& file) {
        return file.series == "IGS0OPSRAP" && file.interval == "05M";
    }));

    const auto database = std::filesystem::temp_directory_path()
        / L"gnss_downloader_clk_test.sqlite";
    std::error_code ignored;
    std::filesystem::remove(database, ignored);
    std::filesystem::remove(database.wstring() + L"-wal", ignored);
    std::filesystem::remove(database.wstring() + L"-shm", ignored);
    SetEnvironmentVariableW(
        L"GNSS_DOWNLOADER_CLK_DB_PATH", database.c_str());

    std::vector<ClkSourceScan> sources;
    for (int index = 0; index < 7; ++index) {
        ClkSourceScan source;
        source.sourceKey = std::to_string(index + 1);
        source.sourcePriority = index + 1;
        source.success = true;
        source.files = parsed;
        for (auto& file : source.files) {
            file.downloadUrl = L"https://example.invalid/"
                + std::to_wstring(index) + L"/"
                + std::wstring(file.fileName.begin(), file.fileName.end());
        }
        sources.push_back(std::move(source));
    }
    std::wstring error;
    assert(StoreClkWeek(week, sources, error));
    const auto lookup = LoadClkAvailability(date);
    assert(lookup.indexed);
    assert(lookup.complete);
    assert(lookup.sourcesSuccessful == 7);
    assert(lookup.products.size() == 4);
    assert(std::ranges::all_of(lookup.products, [](const auto& product) {
        return product.routes.size() == 7;
    }));

    const auto stats = GetClkAvailabilityStats();
    assert(stats.completeWeeks == 1);
    assert(stats.products == 4);
    std::wcout << L"CLK availability tests passed.\n";
    return 0;
}
