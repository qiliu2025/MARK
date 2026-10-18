#include "DateUtil.h"
#include "IonexAvailability.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <filesystem>
#include <iostream>

int wmain() {
    const QueryDate date = MakeQueryDate(2025, 1, 1);
    const std::string listing =
        "IGS0OPSFIN_20250010000_01D_02H_GIM.INX.gz\n"
        "IGS0OPSRAP_20250010000_01D_02H_GIM.INX.gz\n"
        "COD0OPSFIN_20250010000_01D_01H_GIM.INX.gz\n"
        "GFZ0OPSFIN_20250010000_01D_02H_ION.IOX.gz\n"
        "CAS0OPSRAP_20250010000_01D_02H_GIM.INX.gz\n"
        "UPC0OPSR15_20250010000_01D_15M_GIM.INX.gz\n"
        "IGS0OPSFIN_20250010000_01D_01D_ROT.INX.gz\n"
        "igsg0010.25i.Z\n"
        "igrg0010.25i.Z\n"
        "codg0010.25i.Z\n"
        "c1pg0010.25i.Z\n"
        "uqrg0010.25i.Z\n"
        "whrg0010.25i.Z\n"
        "emrg0010.25i.Z\n"
        "irtg0010.25i.Z\n"
        "roti0010.25f.Z\n"
        "COD0OPSFIN_20250010000_01D_01D_OSB.BIA.gz\n"
        "IGS0OPSFIN_20250020000_01D_02H_GIM.INX.gz\n";

    const auto parsed = ParseIonexDirectoryListing(
        listing, date, L"https://example.invalid/ionex/");
    assert(parsed.size() == 16);
    assert(std::ranges::count_if(parsed, [](const auto& file) {
        return file.contentType == "ROT";
    }) == 2);
    assert(std::ranges::any_of(parsed, [](const auto& file) {
        return file.center == "GFZ" && file.originalContent == "ION"
            && file.contentType == "GIM" && file.format == "IONEX_IOX";
    }));
    assert(std::ranges::any_of(parsed, [](const auto& file) {
        return file.series == "COD0OPS1DP" && file.latency == "PREDICTED";
    }));
    assert(std::ranges::any_of(parsed, [](const auto& file) {
        return file.series == "UPC0OPSR15" && file.interval == "15M";
    }));
    assert(std::ranges::any_of(parsed, [](const auto& file) {
        return file.series == "IGS0OPSRTS" && file.latency == "REAL_TIME"
            && file.interval == "20M";
    }));

    const auto database = std::filesystem::temp_directory_path()
        / L"gnss_downloader_ionex_test.sqlite";
    std::error_code ignored;
    std::filesystem::remove(database, ignored);
    std::filesystem::remove(database.wstring() + L"-wal", ignored);
    std::filesystem::remove(database.wstring() + L"-shm", ignored);
    SetEnvironmentVariableW(
        L"GNSS_DOWNLOADER_IONEX_DB_PATH", database.c_str());

    const std::array<const char*, 5> keys{
        "CDDIS", "BKG", "IGN", "WHU", "CAS"};
    std::vector<IonexSourceScan> sources;
    for (int index = 0; index < static_cast<int>(keys.size()); ++index) {
        IonexSourceScan source;
        source.sourceKey = keys[static_cast<std::size_t>(index)];
        source.sourcePriority = index + 1;
        source.success = true;
        if (index < 2) source.files = parsed;
        for (auto& file : source.files) {
            file.downloadUrl = L"https://example.invalid/"
                + std::to_wstring(index) + L"/"
                + std::wstring(file.fileName.begin(), file.fileName.end());
        }
        sources.push_back(std::move(source));
    }

    std::wstring error;
    assert(StoreIonexDate(date, sources, error));
    const auto lookup = LoadIonexAvailability(date);
    assert(lookup.indexed);
    assert(lookup.complete);
    assert(lookup.sourcesSuccessful == 5);
    assert(lookup.sourcesExpected == 5);
    // Five legacy names are exact modern aliases; the other three are distinct.
    assert(lookup.products.size() == 11);
    assert(std::ranges::all_of(lookup.products, [](const auto& product) {
        return product.routes.size() >= 2;
    }));
    assert(std::ranges::any_of(lookup.products, [](const auto& product) {
        return product.series == "IGS0OPSFIN" && product.contentType == "GIM"
            && product.routes.size() == 4;
    }));

    const auto range = LoadIonexProductFamilyRange(date, date);
    assert(range.error.empty());
    assert(range.complete);
    assert(range.expectedDays == 1);
    assert(range.indexedDays == 1);
    assert(range.families.size() == 11);
    const auto family = std::ranges::find_if(range.families,
        [](const auto& value) { return value.representative.series == "IGS0OPSFIN"; });
    assert(family != range.families.end());
    assert(family->availableDays == 1);
    const auto routes = LoadIonexProductFamilyRoutes(
        family->familyKey, date, date);
    assert(routes.error.empty());
    assert(routes.records.size() == 1);
    assert(routes.records.front().record.routes.size() == 4);

    const auto stats = GetIonexAvailabilityStats();
    assert(stats.completeDays == 1);
    assert(stats.products == 11);
    assert(stats.sourceRoutes == 32);
    std::wcout << L"IONEX availability tests passed.\n";
    return 0;
}
