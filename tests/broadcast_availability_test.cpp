#include "BroadcastAvailability.h"
#include "DateUtil.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <set>

int wmain() {
    const QueryDate date = MakeQueryDate(2025, 1, 1);
    const std::string listing =
        "<a href=\"brdc0010.25n.Z\">n</a>\n"
        "brdc0010.25g.gz\n"
        "BRDC00IGS_R_20250010000_01D_MN.rnx.gz\n"
        "BRDC00WRD_S_20250010000_01D_MN.rnx.gz\n"
        "BRDC00WRD_R_20250010000_01D_MN.rnx.gz\n"
        "BRDC00WRD_R_20250010000_01D_GN.rnx.gz\n"
        "BRDC00WRD_R_20250010000_01D_RN.rnx.gz\n"
        "BRDC00WRD_R_20250010000_01D_EN.rnx.gz\n"
        "BRDC00WRD_R_20250010000_01D_CN.rnx.gz\n"
        "BRDC00WRD_R_20250010000_01D_JN.rnx.gz\n"
        "BRDC00WRD_R_20250010000_01D_SN.rnx.gz\n"
        "BRDC00IGN_R_20250010000_01D_MN.rnx.gz\n"
        "BRDC00IGN_R_20250010000_01D_GN.rnx.gz\n"
        "BRDC00IGN_R_20250010000_01D_RN.rnx.gz\n"
        "BRDC00IGN_R_20250010000_01D_EN.rnx.gz\n"
        "BRDM00DLR_S_20250010000_01D_MN.rnx.gz\n"
        "BRD400DLR_S_20250010000_01D_MN.rnx.gz\n"
        "BRDM00CAS_S_20250010000_01D_MN.rnx.gz\n"
        "BRDC00IGS_R_20250010000_01D_MN.rnx.gz\n" // duplicate
        "ABCD00CHL_R_20250010000_01D_MN.rnx.gz\n" // station file
        "BRDC00IGN_R_20250010000_01D_CN.rnx.gz\n" // unsupported IGN type
        "BRDM00DLR_S_20250010000_01D_GN.rnx.gz\n" // unsupported DLR type
        "BRDC00IGS_R_20240010000_01D_MN.rnx.gz\n"; // wrong year

    const auto parsed = ParseBroadcastDirectoryListing(
        listing, date.year, L"https://example.invalid/brdc/");
    assert(parsed.size() == 18);
    assert(std::ranges::count_if(parsed, [](const auto& file) {
        return file.series == "BRDC00WRD";
    }) == 8);
    assert(std::ranges::count_if(parsed, [](const auto& file) {
        return file.series == "BRDC00IGN";
    }) == 4);
    assert(std::ranges::any_of(parsed, [](const auto& file) {
        return file.series == "BRDM00DLR" && file.producer == "DLR";
    }));
    const auto historical = ParseBroadcastDirectoryListing(
        "brdm0010.25p.Z\n", date.year,
        L"https://example.invalid/historical/");
    assert(historical.size() == 1);
    assert(historical.front().logicalKey
        == std::ranges::find_if(parsed, [](const auto& file) {
            return file.series == "BRDM00DLR";
        })->logicalKey);

    std::set<std::string> keys;
    for (const auto& file : parsed) keys.insert(file.logicalKey);
    assert(keys.size() == 18);

    const auto database = std::filesystem::temp_directory_path()
        / L"gnss_downloader_broadcast_test.sqlite";
    std::error_code ignored;
    std::filesystem::remove(database, ignored);
    std::filesystem::remove(database.wstring() + L"-wal", ignored);
    std::filesystem::remove(database.wstring() + L"-shm", ignored);
    SetEnvironmentVariableW(
        L"GNSS_DOWNLOADER_BROADCAST_DB_PATH", database.c_str());

    const std::array<const char*, 8> sourceKeys{
        "CDDIS", "BKG", "WHU", "KASI", "IGN", "ESA", "SOPAC", "CAS"};
    std::vector<BroadcastSourceScan> sources;
    for (int index = 0; index < static_cast<int>(sourceKeys.size()); ++index) {
        BroadcastSourceScan source;
        source.sourceKey = sourceKeys[static_cast<std::size_t>(index)];
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
    assert(StoreBroadcastDate(date, sources, error));
    const auto lookup = LoadBroadcastAvailability(date);
    assert(lookup.indexed);
    assert(lookup.complete);
    assert(lookup.sourcesSuccessful == 8);
    assert(lookup.products.size() == 18);
    assert(std::ranges::all_of(lookup.products, [](const auto& product) {
        return product.routes.size() == 8;
    }));

    const auto stats = GetBroadcastAvailabilityStats();
    assert(stats.completeDays == 1);
    assert(stats.products == 18);
    assert(stats.sourceRoutes == 18 * 8);
    std::wcout << L"Broadcast availability tests passed.\n";
    return 0;
}
