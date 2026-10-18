#include "AttAvailability.h"
#include "DateUtil.h"

#include <windows.h>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>

int wmain() {
    const QueryDate date = MakeQueryDate(2026, 5, 24);
    const int week = CalculateGpsWeek(date);
    const std::string listing =
        "COD0OPSFIN_20261440000_01D_30S_ATT.OBX.gz\n"
        "GRG0OPSULT_20261440600_01D_05M_ATT.OBX.gz\n"
        "WUM0MGXNRT_20261442300_02D_30S_ATT.OBX.gz\n"
        "IGS0DEMFIN_20261440000_01D_30S_ATT.OBX.gz\n"
        "COD0OPSFIN_20261440000_01D_05M_ORB.SP3.gz\n"
        "COD0OPSFIN_20261440000_01D_30S_CLK.CLK.gz\n";
    const auto parsed = ParseAttDirectoryListing(
        listing, week, L"https://example.invalid/products/2420/", "OPS");
    assert(parsed.size() == 4);
    assert(std::ranges::any_of(parsed, [](const auto& file) {
        return file.series == "COD0OPSFIN"
            && file.latency == "FINAL" && file.interval == "30S";
    }));
    assert(std::ranges::any_of(parsed, [](const auto& file) {
        return file.series == "GRG0OPSULT"
            && file.latency == "ULTRA_RAPID" && file.startHour == 6;
    }));
    assert(std::ranges::any_of(parsed, [](const auto& file) {
        return file.series == "WUM0MGXNRT"
            && file.latency == "NEAR_REAL_TIME" && file.duration == "02D";
    }));
    assert(std::ranges::none_of(parsed, [](const auto& file) {
        return file.fileName.find(".SP3") != std::string::npos
            || file.fileName.find(".CLK") != std::string::npos;
    }));

    const QueryDate reproDate = MakeQueryDate(2018, 5, 6);
    const int reproWeek = CalculateGpsWeek(reproDate);
    const auto repro = ParseAttDirectoryListing(
        "IGS2R03FIN_20181260000_01D_30S_ATT.OBX.gz\n",
        reproWeek, L"https://example.invalid/repro3/2000/", "REPRO3");
    assert(repro.size() == 1);
    assert(repro.front().project == "R03");
    assert(repro.front().latency == "REPROCESSING");
    assert(repro.front().constellation == "GRE");

    const auto database = std::filesystem::temp_directory_path()
        / L"gnss_downloader_att_test.sqlite";
    std::error_code ignored;
    std::filesystem::remove(database, ignored);
    std::filesystem::remove(database.wstring() + L"-wal", ignored);
    std::filesystem::remove(database.wstring() + L"-shm", ignored);
    SetEnvironmentVariableW(
        L"GNSS_DOWNLOADER_ATT_DB_PATH", database.c_str());

    std::vector<AttSourceScan> sources;
    const int priorities[] = {1, 3, 5, 6, 7};
    for (const int priority : priorities) {
        AttSourceScan source;
        source.sourceKey = std::to_string(priority);
        source.sourcePriority = priority;
        source.success = true;
        source.files = parsed;
        for (auto& file : source.files) {
            file.downloadUrl = L"https://example.invalid/"
                + std::to_wstring(priority) + L"/"
                + std::wstring(file.fileName.begin(), file.fileName.end());
        }
        sources.push_back(std::move(source));
    }
    std::wstring error;
    assert(StoreAttWeek(week, sources, error));
    const auto lookup = LoadAttAvailability(date);
    assert(lookup.indexed);
    assert(lookup.complete);
    assert(lookup.sourcesSuccessful == 5);
    assert(lookup.products.size() == 4);
    assert(std::ranges::all_of(lookup.products, [](const auto& product) {
        return product.routes.size() == 5;
    }));

    const auto stats = GetAttAvailabilityStats();
    assert(stats.completeWeeks == 1);
    assert(stats.products == 4);
    std::wcout << L"ATT/ORBEX availability tests passed.\n";
    return 0;
}
