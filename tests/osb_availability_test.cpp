#include "DateUtil.h"
#include "OsbAvailability.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <filesystem>
#include <format>
#include <iostream>

int wmain() {
    const QueryDate day = MakeQueryDate(2025, 1, 1);
    const int gpsWeek = CalculateOsbGpsWeek(day);
    assert(gpsWeek > 0);

    const std::string listing =
        "COD0OPSFIN_20250010000_01D_01D_OSB.BIA.gz\n"
        "COD0OPSRAP_20250010000_01D_01D_OSB.BIA.gz\n"
        "COD0MGXFIN_20250010000_01D_01D_OSB.BIA.gz\n"
        "EMR0OPSFIN_20250010000_01D_01D_OSB.BIA.gz\n"
        "GFZ0MGXRAP_20250010000_01D_01D_OSB.BIA.gz\n"
        "GRG0MGXFIN_20250010000_01D_01D_OSB.BIA.gz\n"
        "HUS0MGXFIN_20250010000_01D_01D_OSB.BIA.gz\n"
        "WUM0MGXFIN_20250010000_01D_01D_OSB.BIA.gz\n"
        "WUM0MGXFIN_20250010000_01D_30S_OSB.BIA.gz\n"
        "CAS0MGXRAP_20250010000_01D_01D_OSB.BIA.gz\n"
        "CAS1MGXRAP_20250010000_01D_01D_OSB.BIA.gz\n"
        "WCC0OPSFIN_20250010000_01D_01D_OSB.BIA.gz\n"
        "TUG0R03FIN_20250010000_01D_01D_OSB.BIA.gz\n"
        "COD0OPSFIN_20250010000_01D_01D_DCB.BIA.gz\n"
        "COD0OPSFIN_20250010000_01D_01D_DSB.BIA.gz\n"
        "WUM0MGXRAP_20250010000_01D_30S_ABS.BIA.gz\n";
    const auto parsed = ParseOsbDirectoryListing(
        listing, gpsWeek, L"https://example.invalid/products/", "OPS");
    assert(parsed.size() == 13);
    assert(std::ranges::none_of(parsed, [](const auto& file) {
        return file.fileName.find("_DCB") != std::string::npos
            || file.fileName.find("_DSB") != std::string::npos
            || file.fileName.find("_ABS") != std::string::npos;
    }));
    assert(std::ranges::count_if(parsed, [](const auto& file) {
        return file.series == "WUM0MGXFIN";
    }) == 2);
    assert(std::ranges::any_of(parsed, [](const auto& file) {
        return file.series == "CAS0MGXRAP"
            && file.biasClass == "CODE_ONLY";
    }));
    assert(std::ranges::any_of(parsed, [](const auto& file) {
        return file.series == "WCC0OPSFIN"
            && file.biasClass == "COMBINED_CODE_PHASE";
    }));

    const std::string legacyName = std::format("COM{}3.BIA.Z", gpsWeek);
    const auto legacy = ParseOsbDirectoryListing(
        legacyName, gpsWeek, L"https://example.invalid/legacy/", "LEGACY");
    assert(legacy.size() == 1);
    assert(legacy.front().series == "COD0MGXFIN");

    const auto database = std::filesystem::temp_directory_path()
        / L"gnss_downloader_osb_test.sqlite";
    std::error_code ignored;
    std::filesystem::remove(database, ignored);
    std::filesystem::remove(database.wstring() + L"-wal", ignored);
    std::filesystem::remove(database.wstring() + L"-shm", ignored);
    SetEnvironmentVariableW(
        L"GNSS_DOWNLOADER_OSB_DB_PATH", database.c_str());

    const std::array<const char*, 5> keys{
        "CDDIS", "IGN", "WHU", "CAS", "WCC"};
    std::vector<OsbSourceScan> sources;
    for (int index = 0; index < static_cast<int>(keys.size()); ++index) {
        OsbSourceScan source;
        source.sourceKey = keys[static_cast<std::size_t>(index)];
        source.sourcePriority = index + 1;
        source.success = true;
        if (index == 0 || index == 1) source.files = parsed;
        if (index == 0) {
            source.files.insert(
                source.files.end(), legacy.begin(), legacy.end());
        }
        for (auto& file : source.files) {
            file.downloadUrl = L"https://example.invalid/"
                + std::to_wstring(index) + L"/"
                + std::wstring(file.fileName.begin(), file.fileName.end());
        }
        sources.push_back(std::move(source));
    }

    std::wstring error;
    assert(StoreOsbWeek(gpsWeek, sources, error));
    const auto lookup = LoadOsbAvailability(day);
    assert(lookup.indexed);
    assert(lookup.complete);
    assert(lookup.sourcesSuccessful == 5);
    assert(lookup.sourcesExpected == 5);
    assert(lookup.products.size() == 13);
    assert(std::ranges::any_of(lookup.products, [](const auto& product) {
        return product.series == "COD0OPSFIN" && product.routes.size() == 2;
    }));
    assert(std::ranges::count_if(lookup.products, [](const auto& product) {
        return product.series == "WUM0MGXFIN";
    }) == 2);

    const auto stats = GetOsbAvailabilityStats();
    assert(stats.completeWeeks == 1);
    assert(stats.products == 14);
    assert(stats.sourceRoutes == 27);
    std::wcout << L"OSB availability tests passed.\n";
    return 0;
}
