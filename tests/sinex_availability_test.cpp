#include "DateUtil.h"
#include "SinexAvailability.h"
#include "Sp3Availability.h"

#include <windows.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

bool SameDate(const QueryDate& value, const int year,
              const int month, const int day) {
    return value.year == year && value.month == month && value.day == day;
}

} // namespace

int main() {
    const QueryDate modernDate = MakeQueryDate(2026, 1, 4);
    const int modernWeek = CalculateGpsWeek(modernDate);
    const std::string modernListing = R"(
        COD0OPSFIN_20260040000_01D_01D_SOL.SNX.gz
        ESA0OPSULT_20260041800_02D_01D_SOL.SNX.gz
        IGS0OPSSNX_20260040000_07D_07D_CRD.SNX.gz
        COD0OPSFIN_20260040000_01D_01D_ORB.SP3.gz
        COD0OPSFIN_20260040000_01D_01D_BIA.BIA.gz
    )";
    const auto modern = ParseSinexDirectoryListing(
        modernListing, modernWeek,
        L"https://example.test/products/2400/", "OPS");
    if (modern.size() != 3) {
        std::cerr << "expected 3 modern SINEX files, got "
                  << modern.size() << "\n";
        return 1;
    }
    bool daily = false;
    bool ultra = false;
    bool coordinates = false;
    for (const auto& file : modern) {
        if (file.center == "COD" && file.contentType == "SOL") {
            daily = file.productClass == "DAILY"
                && SameDate(file.coverageEnd, 2026, 1, 4);
        }
        if (file.center == "ESA") {
            ultra = file.latency == "ULTRA_RAPID"
                && file.startHour == 18
                && SameDate(file.coverageEnd, 2026, 1, 6);
        }
        if (file.center == "IGS" && file.contentType == "CRD") {
            coordinates = file.productClass == "WEEKLY"
                && SameDate(file.coverageEnd, 2026, 1, 10);
        }
    }
    if (!daily || !ultra || !coordinates) {
        std::cerr << "modern SINEX metadata classification failed\n";
        return 2;
    }

    const int legacyWeek = 2200;
    const std::string legacyListing = R"(
        cod22000.snx.Z
        eur22007.snx.Z
        eur22007.ssc.Z
        igs22P22000.snx.Z
        IGS22P10.snx.Z
        igs22000.erp.Z
    )";
    const auto legacy = ParseSinexDirectoryListing(
        legacyListing, legacyWeek,
        L"https://example.test/products/2200/", "OPS");
    if (legacy.size() != 5) {
        std::cerr << "expected 5 legacy SINEX/SSC files, got "
                  << legacy.size() << "\n";
        return 3;
    }
    bool ssc = false;
    bool cumulative = false;
    for (const auto& file : legacy) {
        if (file.format == "SSC")
            ssc = file.contentType == "CRD";
        if (file.productClass == "CUMULATIVE_LEGACY")
            cumulative = file.center == "IGS";
    }
    if (!ssc || !cumulative) {
        std::cerr << "legacy SINEX/SSC classification failed\n";
        return 4;
    }

    const QueryDate cumulativeEnd = MakeQueryDate(2026, 1, 10);
    const int cumulativeWeek = CalculateGpsWeek(cumulativeEnd);
    const auto cumulativeFiles = ParseSinexDirectoryListing(
        "IGS0OPSSNX_1994002_2026010_00U_CRD.SNX.gz",
        cumulativeWeek, L"https://example.test/products/2400/", "OPS");
    if (cumulativeFiles.size() != 1
        || cumulativeFiles.front().productClass != "CUMULATIVE"
        || !SameDate(cumulativeFiles.front().coverageStart, 1994, 1, 2)
        || !SameDate(cumulativeFiles.front().coverageEnd, 2026, 1, 10)) {
        std::cerr << "cumulative SINEX classification failed\n";
        return 5;
    }

    const auto database = std::filesystem::temp_directory_path()
        / L"gnss_downloader_sinex_availability_test.sqlite";
    std::error_code removeError;
    std::filesystem::remove(database, removeError);
    std::filesystem::remove(database.wstring() + L"-wal", removeError);
    std::filesystem::remove(database.wstring() + L"-shm", removeError);
    SetEnvironmentVariableW(
        L"GNSS_DOWNLOADER_SINEX_DB_PATH", database.c_str());
    const std::array<std::pair<const char*, int>, 6> sources{{
        {"CDDIS", 1}, {"BKG", 2}, {"WHU", 3},
        {"IGN", 5}, {"ESA", 6}, {"SOPAC", 7},
    }};
    std::vector<SinexSourceScan> scans;
    for (const auto& [key, priority] : sources) {
        SinexSourceScan scan;
        scan.sourceKey = key;
        scan.sourcePriority = priority;
        scan.success = true;
        auto file = modern.front();
        file.downloadUrl = L"https://example.test/"
            + std::to_wstring(priority) + L"/"
            + std::wstring(file.fileName.begin(), file.fileName.end());
        scan.files.push_back(std::move(file));
        scans.push_back(std::move(scan));
    }
    std::wstring storeError;
    if (!StoreSinexWeek(modernWeek, scans, storeError)) {
        std::wcerr << L"SINEX database store failed: " << storeError << L"\n";
        return 6;
    }
    const auto lookup = LoadSinexAvailability(modernDate);
    if (!lookup.indexed || !lookup.complete
        || lookup.sourcesSuccessful != 6 || lookup.sourcesExpected != 6
        || lookup.products.size() != 1
        || lookup.products.front().routes.size() != 6) {
        std::cerr << "SINEX database round-trip failed\n";
        return 7;
    }
    SetEnvironmentVariableW(L"GNSS_DOWNLOADER_SINEX_DB_PATH", nullptr);
    std::filesystem::remove(database, removeError);
    std::filesystem::remove(database.wstring() + L"-wal", removeError);
    std::filesystem::remove(database.wstring() + L"-shm", removeError);
    std::cout << "SINEX parser tests passed\n";
    return 0;
}
