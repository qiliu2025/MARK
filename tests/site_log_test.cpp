#include "SiteLogDb.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <unordered_map>

int main() {
    const std::string text =
        "0. Form\n"
        "     Date Prepared            : 2025-01-02\n"
        "1. Site Identification of the GNSS Monument\n"
        "     Site Name                : Test Station\n"
        "     Four Character ID        : TEST\n"
        "     9-character ID           : TEST00USA\n"
        "     IERS DOMES Number        : 12345M001\n"
        "2. Site Location Information\n"
        "     City or Town             : Example\n"
        "     Country                  : United States\n"
        "     X coordinate (m)         : 1.0\n"
        "     Y coordinate (m)         : 2.0\n"
        "     Z coordinate (m)         : 3.0\n"
        "     Latitude (N is +)        : 40 30 00.0\n"
        "     Longitude (E is +)       : -0730131.73\n"
        "     Elevation (m,ellips.)    : 10.5\n"
        "3. GNSS Receiver Information\n"
        "3.1 Receiver Type            : TEST RECEIVER\n"
        "     Satellite System         : GPS+GLO+GAL\n"
        "     Serial Number            : R123\n"
        "     Firmware Version         : 1.2\n"
        "     Date Installed           : 2020-01-01T00:00Z\n"
        "     Elevation Cutoff Setting : 0 deg\n"
        "3.x  Satellite System         : (GPS+GLO+GAL+BDS+QZSS+SBAS)\n"
        "4. GNSS Antenna Information\n"
        "4.1 Antenna Type             : TEST ANTENNA NONE\n"
        "     Serial Number            : A123\n"
        "     Antenna Reference Point  : BPA\n"
        "     Antenna Radome Type      : NONE\n"
        "     Date Installed           : 2020-01-01T00:00Z\n"
        "11. On-Site, Point of Contact Agency Information\n"
        "     Agency                  : Example Agency\n";

    const auto record = ParseSiteLogText(
        text,
        "https://files.igs.org/pub/station/log/test00usa_20250102.log",
        "https://files.igs.org/pub/station/log/",
        1);
    assert(record.stationId == "TEST00USA");
    assert(record.fourCharId == "TEST");
    assert(record.domes == "12345M001");
    assert(record.siteName == "Test Station");
    assert(record.country == "United States");
    assert(record.hasXyz && record.hasCoordinate);
    assert(record.elevation > 10.4 && record.elevation < 10.6);
    assert(record.latitude > 40.49 && record.latitude < 40.51);
    assert(record.longitude < -73.02 && record.longitude > -73.03);
    assert(record.receiverType == "TEST RECEIVER");
    assert(record.receiverSystems == "GPS+GLO+GAL");
    assert(record.antennaType == "TEST ANTENNA NONE");
    assert(record.antennaRadome == "NONE");
    assert(record.agency == "Example Agency");

    const auto sectionStart = text.find("11. On-Site");
    const auto spacedHeading = text.substr(0, sectionStart)
        + "11.   On-Site, Point of Contact Agency Information\n"
        + "     Agency                   : Spaced Agency\n"
        + "       Contact Name           : Spaced Contact\n";
    const auto spacedRecord = ParseSiteLogText(
        spacedHeading,
        "https://files.igs.org/pub/station/log/test00usa_20250103.log",
        "https://files.igs.org/pub/station/log/",
        1);
    assert(spacedRecord.agency == "Spaced Agency");
    assert(spacedRecord.operatorName == "Spaced Contact");

    // Regional portals such as Hong Kong SatRef use the spelled-out
    // "Nine Character ID" label and often publish a .txt URL rather than a
    // .log filename.  The parser must still retain the full station ID and
    // prefer the document's Date Prepared over a compact filename date.
    const std::string hkText =
        "0. Form\n"
        "     Date Prepared            : 2026-06-23\n"
        "1. Site Identification of the GNSS Monument\n"
        "     Site Name                : Chek Lap Kok\n"
        "     Nine Character ID        : HKCL00HKG\n"
        "2. Site Location Information\n"
        "     Country or Region        : HKG\n"
        "     Latitude (N is +)        : +221745.03141\n"
        "     Longitude (E is +)      : +1135427.79510\n"
        "3. GNSS Receiver Information\n"
        "3.1 Receiver Type            : TRIMBLE ALLOY\n"
        "     Satellite System         : GPS+GLO+GAL+BDS\n";
    const auto hkRecord = ParseSiteLogText(
        hkText,
        "https://www.geodetic.gov.hk/common/SiteLog/HKCL_log.txt",
        "https://www.geodetic.gov.hk/common/SiteLog/",
        1ULL << 4);
    assert(hkRecord.stationId == "HKCL00HKG");
    assert(hkRecord.fourCharId == "HKCL");
    assert(hkRecord.country == "HKG");
    assert(hkRecord.revisionDate == "2026-06-23");
    assert(hkRecord.hasCoordinate);
    assert(hkRecord.receiverType == "TRIMBLE ALLOY");

    const auto path = std::filesystem::temp_directory_path()
        / "gnss_site_log_test.sqlite";
    std::error_code removeError;
    std::filesystem::remove(path, removeError);

    std::wstring error;
    auto historical = record;
    historical.sourceUrl =
        "https://files.igs.org/pub/station/mgexfinallog/test_20200101.log";
    historical.sourceDirectory =
        "https://files.igs.org/pub/station/mgexfinallog/";
    historical.fileName = "test_20200101.log";
    historical.stationId.clear();
    historical.siteName = "Historical Test Station";
    historical.revisionDate = "2020-01-01";
    historical.networkMask = 2;
    assert(StoreSiteLogRecords(path, {record, historical}, error));
    std::vector<SiteLogRecord> loaded;
    assert(LoadSiteLogRecords(path, loaded, error));
    assert(loaded.size() == 1);
    std::unordered_map<std::string, SiteLogRecord> map;
    assert(LoadSiteLogSummaryMap(path, "2025-12-31", map, error));
    assert(map.contains("TEST00USA") && map.contains("TEST"));
    assert(map.at("TEST00USA").siteName == "Test Station");
    assert(map.at("TEST00USA").networkMask == 3);
    std::unordered_map<std::string, SiteLogRecord> globalOnlyMap;
    assert(LoadSiteLogSummaryMap(path, "2025-12-31", globalOnlyMap, error, 1));
    assert(globalOnlyMap.at("TEST00USA").siteName == "Test Station");
    std::unordered_map<std::string, SiteLogRecord> mgexOnlyMap;
    assert(LoadSiteLogSummaryMap(path, "2021-01-01", mgexOnlyMap, error, 2));
    assert(mgexOnlyMap.at("TEST").siteName == "Historical Test Station");
    std::unordered_map<std::string, SiteLogRecord> absentNetworkMap;
    assert(LoadSiteLogSummaryMap(path, "2025-12-31", absentNetworkMap, error, 4));
    assert(absentNetworkMap.empty());
    std::unordered_map<std::string, SiteLogRecord> historicalMap;
    assert(LoadSiteLogSummaryMap(path, "2021-01-01", historicalMap, error));
    assert(historicalMap.at("TEST").siteName == "Historical Test Station");
    std::filesystem::remove(path, removeError);

    // Equipment history is selected by installation/removal dates, not by
    // the date on which the Site Log document happened to be published.
    const std::string temporalText =
        "0. Form\n"
        "     Date Prepared            : 2019-02-01\n"
        "1. Site Identification of the GNSS Monument\n"
        "     Four Character ID        : HIST\n"
        "     9-character ID           : HIST00USA\n"
        "2. Site Location Information\n"
        "     Country                  : United States\n"
        "3. GNSS Receiver Information\n"
        "3.1 Receiver Type            : RECEIVER-A\n"
        "     Satellite System         : GPS\n"
        "     Date Installed           : 2019-01-01\n"
        "     Date Removed             : 2021-01-01\n"
        "3.2 Receiver Type            : RECEIVER-B\n"
        "     Satellite System         : GPS+BDS\n"
        "     Date Installed           : 2021-01-01\n"
        "4. GNSS Antenna Information\n"
        "4.1 Antenna Type             : ANTENNA-A\n"
        "     Date Installed           : 2019-01-01\n"
        "     Date Removed             : 2022-01-01\n"
        "4.2 Antenna Type             : ANTENNA-B\n"
        "     Date Installed           : 2022-01-01\n";
    const auto temporalRecord = ParseSiteLogText(
        temporalText,
        "https://files.igs.org/pub/station/log/hist_20190201.log",
        "https://files.igs.org/pub/station/log/",
        1);
    assert(temporalRecord.equipmentHistory.size() == 4);
    const auto temporalPath = std::filesystem::temp_directory_path()
        / "gnss_site_log_temporal_test.sqlite";
    std::filesystem::remove(temporalPath, removeError);
    assert(StoreSiteLogRecords(temporalPath, {temporalRecord}, error));
    std::unordered_map<std::string, SiteLogRecord> temporalMap;
    assert(LoadSiteLogSummaryMap(
        temporalPath, "2020-01-01", temporalMap, error));
    assert(temporalMap.at("HIST").receiverType == "RECEIVER-A");
    assert(temporalMap.at("HIST").receiverSystems == "GPS");
    assert(temporalMap.at("HIST").antennaType == "ANTENNA-A");
    assert(temporalMap.at("HIST").equipmentHistory.size() == 4);
    temporalMap.clear();
    assert(LoadSiteLogSummaryMap(
        temporalPath, "2021-06-01", temporalMap, error));
    assert(temporalMap.at("HIST").receiverType == "RECEIVER-B");
    assert(temporalMap.at("HIST").receiverSystems == "GPS+BDS");
    assert(temporalMap.at("HIST").antennaType == "ANTENNA-A");
    temporalMap.clear();
    assert(LoadSiteLogSummaryMap(
        temporalPath, "2022-02-01", temporalMap, error));
    assert(temporalMap.at("HIST").receiverType == "RECEIVER-B");
    assert(temporalMap.at("HIST").antennaType == "ANTENNA-B");
    std::filesystem::remove(temporalPath, removeError);
    std::cout << "SiteLogDbTest passed\n";
    return 0;
}
