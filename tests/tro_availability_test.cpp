#include "DateUtil.h"
#include "Sp3Availability.h"
#include "TroAvailability.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <filesystem>
#include <format>
#include <iostream>

int wmain() {
    const QueryDate date = MakeQueryDate(2025, 1, 1);
    const std::string modernListing =
        "IGS0OPSFIN_20250010000_01D_05M_ZIM200CHE_TRO.TRO.gz\n"
        "COD0OPSFIN_20250010000_01D_01H_TRO.TRO.gz\n"
        "COD0OPSFIN_20250010000_01D_02H_TRO.TRP.gz\n"
        "ASI0EPNFIN_20250010000_01D_01H_TRO.TRO.gz\n"
        "EUR0OPSRAP_20250010000_01D_01H_TRO.TRO.gz\n"
        "EUR0OPSFIN_20243640000_07D_01H_TRO.TRO.gz\n"
        "EUR0OPSFIN_20243640000_07D_01H_TRO.SUM.gz\n"
        "COD0OPSFIN_20250010000_01D_01D_SOL.SNX.gz\n";
    const auto modern = ParseTroDirectoryListing(
        modernListing, date, L"https://example.invalid/tro/", "MIXED");
    assert(modern.size() == 6);
    assert(std::ranges::any_of(modern, [](const auto& file) {
        return file.productFamily == "IGS_SITE"
            && file.station == "ZIM200CHE" && file.interval == "05M";
    }));
    assert(std::ranges::any_of(modern, [](const auto& file) {
        return file.productFamily == "CODE_TRP"
            && file.format == "BERNESE_TRP" && file.interval == "02H";
    }));
    assert(std::ranges::any_of(modern, [](const auto& file) {
        return file.productFamily == "EPN_COMBINED"
            && file.duration == "07D" && file.latency == "FINAL";
    }));

    const auto igsLegacy = ParseTroDirectoryListing(
        "zim20010.25zpd.gz", date,
        L"https://example.invalid/zpd/", "IGS_ZPD");
    assert(igsLegacy.size() == 1);
    assert(igsLegacy.front().station == "ZIM2");
    assert(igsLegacy.front().logicalKey == modern.front().logicalKey
        || std::ranges::any_of(modern, [&](const auto& file) {
            return file.productFamily == "IGS_SITE"
                && file.logicalKey == igsLegacy.front().logicalKey;
        }));

    const auto codeLegacy = ParseTroDirectoryListing(
        "COD25001.TRP.Z\nCOD25001.TRO.Z", date,
        L"https://example.invalid/code/", "CODE_TROP");
    assert(codeLegacy.size() == 2);
    assert(std::ranges::any_of(codeLegacy, [](const auto& file) {
        return file.productFamily == "CODE_TRO" && file.interval == "01H";
    }));

    const int week = CalculateGpsWeek(date);
    const int day = static_cast<int>((std::chrono::sys_days{
        std::chrono::year{date.year}
        / std::chrono::month{static_cast<unsigned>(date.month)}
        / std::chrono::day{static_cast<unsigned>(date.day)}}
        - std::chrono::sys_days{
            std::chrono::year{GpsWeekStartDate(week).year}
            / std::chrono::month{static_cast<unsigned>(GpsWeekStartDate(week).month)}
            / std::chrono::day{static_cast<unsigned>(GpsWeekStartDate(week).day)}}).count());
    const std::string legacyEpnListing = std::format(
        "as0{:04}{}.tro.Z\neu0{:04}7.tro.Z", week, day, week);
    const auto legacyEpn = ParseTroDirectoryListing(
        legacyEpnListing, date, L"https://example.invalid/epn/",
        "EPN_REPRO2", week);
    assert(legacyEpn.size() == 2);
    assert(std::ranges::all_of(legacyEpn, [](const auto& file) {
        return file.solution == "E02" && file.latency == "REPROCESSING";
    }));

    const auto database = std::filesystem::temp_directory_path()
        / L"gnss_downloader_tro_availability_test.sqlite";
    std::error_code ignored;
    std::filesystem::remove(database, ignored);
    std::filesystem::remove(database.wstring() + L"-wal", ignored);
    std::filesystem::remove(database.wstring() + L"-shm", ignored);
    SetEnvironmentVariableW(
        L"GNSS_DOWNLOADER_TRO_DB_PATH", database.c_str());

    const std::array<const char*, 8> keys{
        "CDDIS", "IGN", "ESA", "WHU", "CODE",
        "EPN-BKG", "EPN-R1", "EPN-R2"};
    std::vector<TroSourceScan> sources;
    for (int index = 0; index < static_cast<int>(keys.size()); ++index) {
        TroSourceScan source;
        source.sourceKey = keys[static_cast<std::size_t>(index)];
        source.sourcePriority = index + 1;
        source.success = true;
        if (index < 2) source.files = modern;
        for (auto& file : source.files) {
            file.downloadUrl = L"https://example.invalid/"
                + std::to_wstring(index) + L"/"
                + std::wstring(file.fileName.begin(), file.fileName.end());
        }
        sources.push_back(std::move(source));
    }
    std::wstring error;
    assert(StoreTroDate(date, sources, error));
    const auto lookup = LoadTroAvailability(date);
    assert(lookup.indexed);
    assert(lookup.complete);
    assert(lookup.sourcesSuccessful == 8);
    assert(lookup.sourcesExpected == 8);
    assert(lookup.products.size() == 6);
    assert(std::ranges::all_of(lookup.products, [](const auto& product) {
        return product.routes.size() == 2;
    }));
    const auto stats = GetTroAvailabilityStats();
    assert(stats.completeDays == 1);
    assert(stats.products == 6);
    assert(stats.sourceRoutes == 12);

    SetEnvironmentVariableW(L"GNSS_DOWNLOADER_TRO_DB_PATH", nullptr);
    std::filesystem::remove(database, ignored);
    std::filesystem::remove(database.wstring() + L"-wal", ignored);
    std::filesystem::remove(database.wstring() + L"-shm", ignored);
    std::wcout << L"TRO availability tests passed.\n";
    return 0;
}
