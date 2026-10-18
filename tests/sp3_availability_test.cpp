#include "DateUtil.h"
#include "Sp3Availability.h"

#include <windows.h>

#include <cassert>
#include <filesystem>
#include <iostream>

int wmain() {
    const QueryDate date = MakeQueryDate(2025, 1, 1);
    const int week = CalculateGpsWeek(date);
    const std::string listing =
        "<a href=\"COD0OPSFIN_20250010000_01D_05M_ORB.SP3.gz\">x</a>\n"
        "COD0OPSFIN_20250010000_01D_05M_ORB.SP3.gz\n"
        "igs" + std::to_string(week) + "3.sp3.Z\n";
    const auto parsed = ParseSp3DirectoryListing(
        listing, week, L"https://example.invalid/products/", "OPS");
    assert(parsed.size() == 2);
    assert(parsed[0].nominalDate.year == 2025);

    const auto database = std::filesystem::temp_directory_path()
        / L"gnss_downloader_sp3_test.sqlite";
    std::error_code ignored;
    std::filesystem::remove(database, ignored);
    std::filesystem::remove(database.wstring() + L"-wal", ignored);
    std::filesystem::remove(database.wstring() + L"-shm", ignored);
    SetEnvironmentVariableW(
        L"GNSS_DOWNLOADER_SP3_DB_PATH", database.c_str());

    std::vector<Sp3SourceScan> sources;
    for (int index = 0; index < 7; ++index) {
        Sp3SourceScan source;
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
    assert(StoreSp3Week(week, sources, error));
    const auto lookup = LoadSp3Availability(date);
    assert(lookup.indexed);
    assert(lookup.complete);
    assert(lookup.sourcesSuccessful == 7);
    assert(!lookup.products.empty());
    assert(lookup.products.front().routes.size() == 7);
    const auto range = LoadSp3AvailabilityRange(date, date);
    assert(range.expectedDays == 1);
    assert(range.indexedDays == 1);
    assert(range.complete);
    assert(range.records.size() == lookup.products.size());
    assert(!range.records.front().record.routes.empty());
    const auto families = LoadSp3ProductFamilyRange(date, date);
    assert(families.error.empty());
    assert(families.expectedDays == 1);
    assert(families.indexedDays == 1);
    assert(!families.families.empty());
    assert(families.families.front().availableDays == 1);
    assert(families.families.front().availableDates.size() == 1);
    assert(families.families.front().availableDates.front().year == 2025);
    const auto hydrated = LoadSp3ProductFamilyRoutes(
        families.families.front().familyKey, date, date);
    assert(hydrated.error.empty());
    assert(!hydrated.records.empty());
    assert(hydrated.records.front().record.routes.size() == 7);

    const auto stats = GetSp3AvailabilityStats();
    assert(stats.completeWeeks == 1);
    assert(stats.products == 2);
    std::wcout << L"SP3 availability tests passed.\n";
    return 0;
}
