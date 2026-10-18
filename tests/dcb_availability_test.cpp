#include "DateUtil.h"
#include "DcbAvailability.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <filesystem>
#include <iostream>

int wmain() {
    const std::string operational =
        "CAS0OPSRAP_20250460000_01D_01D_DCB.BIA.gz\n"
        "CAS0OPSRAP_20250460000_01D_01D_OSB.BIA.gz\n"
        "CAS1MGXRAP_20250460000_01D_01D_DCB.BSX.gz\n"
        "DLR0OPSFIN_20250010000_03L_07D_DCB.BIA.gz\n"
        "DLR0MGXFIN_20250010000_03L_01D_DCB.BSX.gz\n"
        "GFZ0OPSRAP_20250460000_01D_01D_DCB.BIA.gz\n";
    const auto parsed = ParseDcbDirectoryListing(
        operational, 2025, L"https://example.invalid/bias/", false);
    assert(parsed.size() == 5);
    assert(std::ranges::none_of(parsed, [](const auto& file) {
        return file.fileName.find("OSB") != std::string::npos;
    }));
    assert(std::ranges::count_if(parsed, [](const auto& file) {
        return file.center == "DLR";
    }) == 2);
    assert(std::ranges::any_of(parsed, [](const auto& file) {
        return file.center == "DLR" && file.interval == "07D"
            && file.scope == "SATELLITE"
            && file.coverageEnd.month == 3 && file.coverageEnd.day == 31;
    }));

    const std::string code =
        "P1C12502.DCB.Z\n"
        "P1P22502.DCB.Z\n"
        "P1P22502_ALL.DCB.Z\n"
        "P1C12502_RINEX.DCB.Z\n"
        "P2C22502_RINEX.DCB.Z\n"
        "COD0OPSFIN_20250460000_01D_01D_OSB.BIA.gz\n";
    const auto codeParsed = ParseDcbDirectoryListing(
        code, 2025, L"https://example.invalid/CODE/2025/", true);
    assert(codeParsed.size() == 5);
    assert(std::ranges::all_of(codeParsed, [](const auto& file) {
        return file.center == "COD" && file.coverageStart.month == 2
            && file.coverageEnd.month == 2 && file.coverageEnd.day == 28;
    }));

    const auto database = std::filesystem::temp_directory_path()
        / L"gnss_downloader_dcb_test.sqlite";
    std::error_code ignored;
    std::filesystem::remove(database, ignored);
    std::filesystem::remove(database.wstring() + L"-wal", ignored);
    std::filesystem::remove(database.wstring() + L"-shm", ignored);
    SetEnvironmentVariableW(
        L"GNSS_DOWNLOADER_DCB_DB_PATH", database.c_str());

    const std::array<const char*, 4> keys{"CDDIS", "IGN", "CAS", "CODE"};
    std::vector<DcbSourceScan> sources;
    for (int index = 0; index < static_cast<int>(keys.size()); ++index) {
        DcbSourceScan source;
        source.sourceKey = keys[static_cast<std::size_t>(index)];
        source.sourcePriority = index + 1;
        source.success = true;
        if (index < 3) source.files = parsed;
        if (index == 3) source.files = codeParsed;
        for (auto& file : source.files) {
            file.downloadUrl = L"https://example.invalid/"
                + std::to_wstring(index) + L"/"
                + std::wstring(file.fileName.begin(), file.fileName.end());
        }
        sources.push_back(std::move(source));
    }
    std::wstring error;
    assert(StoreDcbYear(2025, sources, error));
    const auto lookup = LoadDcbAvailability(MakeQueryDate(2025, 2, 15));
    assert(lookup.indexed);
    assert(lookup.complete);
    assert(lookup.sourcesSuccessful == 4);
    assert(lookup.products.size() == 10);
    assert(std::ranges::none_of(lookup.products, [](const auto& product) {
        return product.variant.find("OSB") != std::string::npos;
    }));
    assert(std::ranges::any_of(lookup.products, [](const auto& product) {
        return product.variant == "P1P2_ALL"
            && product.scope == "SATELLITE_STATION";
    }));

    const auto stats = GetDcbAvailabilityStats();
    assert(stats.completeYears == 1);
    assert(stats.products == 10);
    std::wcout << L"DCB availability tests passed.\n";
    return 0;
}

