#include "SiteLogDb.h"

#ifdef NDEBUG
#undef NDEBUG  // Release test binaries must still execute their assertions.
#endif
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "sqlite3.h"

namespace {

const std::string kHistoryText =
    "0. Form\n"
    "     Date Prepared            : 2024-01-02\n"
    "1. Site Identification of the GNSS Monument\n"
    "     Site Name                : Temporal Test Station\n"
    "     Four Character ID        : TIME\n"
    "     9-character ID           : TIME00USA\n"
    "2. Site Location Information\n"
    "     Country                  : United States\n"
    "     Latitude (N is +)        : 40 30 00.0\n"
    "     Longitude (E is +)      : -073 01 31.73\n"
    "3. GNSS Receiver Information\n"
    "3.1 Receiver Type            : RECEIVER-A\n"
    "     Satellite System         : GPS\n"
    "     Serial Number            : RA\n"
    "     Firmware Version         : 1.0\n"
    "     Date Installed           : 2019-01-01\n"
    "     Date Removed             : 2020-06-01\n"
    "3.2 Receiver Type            : RECEIVER-B\n"
    "     Satellite System         : GPS+BDS\n"
    "     Serial Number            : RB\n"
    "     Firmware Version         : 2.0\n"
    "     Date Installed           : 2020-06-01\n"
    "     Date Removed             : 2021-06-01\n"
    "3.3 Receiver Type            : RECEIVER-C\n"
    "     Satellite System         : GPS+BDS+GAL\n"
    "     Serial Number            : RC\n"
    "     Firmware Version         : 3.0\n"
    "     Date Installed           : 2021-06-01\n"
    "4. GNSS Antenna Information\n"
    "4.1 Antenna Type             : ANTENNA-A\n"
    "     Serial Number            : AA\n"
    "     Antenna Reference Point  : BPA\n"
    "     Antenna Radome Type      : NONE\n"
    "     Date Installed           : 2019-01-01\n"
    "     Date Removed             : 2020-06-01\n"
    "4.2 Antenna Type             : ANTENNA-B\n"
    "     Serial Number            : AB\n"
    "     Antenna Reference Point  : BPA\n"
    "     Antenna Radome Type      : SCIS\n"
    "     Date Installed           : 2020-06-01\n"
    "     Date Removed             : 2021-06-01\n"
    "4.3 Antenna Type             : ANTENNA-C\n"
    "     Serial Number            : AC\n"
    "     Antenna Reference Point  : BPA\n"
    "     Antenna Radome Type      : SCIT\n"
    "     Date Installed           : 2021-06-01\n"
    "5. Surveyed Local Ties\n"
    "     Notes                    : end of equipment history\n";

const std::string kShortHeadingText =
    "3. GNSS Receiver\n"
    "Receiver Type: SHORT-RECEIVER\n"
    "Satellite System: GPS+BDS\n"
    "Date Installed: 2022-03-04\n"
    "4. GNSS Antenna\n"
    "Antenna Type: SHORT-ANTENNA\n"
    "Date Installed: 2022-03-04\n";

const std::string kGeodesyMlText = R"XML(
<geo:gnssReceiver xmlns:geo="urn:test"><geo:GnssReceiver>
  <geo:igsModelCode>GA-RECEIVER-A</geo:igsModelCode>
  <geo:satelliteSystem>GPS</geo:satelliteSystem>
  <geo:satelliteSystem>GAL</geo:satelliteSystem>
  <geo:manufacturerSerialNumber>GRA</geo:manufacturerSerialNumber>
  <geo:dateInstalled>2018-01-01T00:00:00Z</geo:dateInstalled>
  <geo:dateRemoved>2020-05-02T00:00:00Z</geo:dateRemoved>
</geo:GnssReceiver></geo:gnssReceiver>
<geo:gnssReceiver xmlns:geo="urn:test"><geo:GnssReceiver>
  <geo:igsModelCode>GA-RECEIVER-B</geo:igsModelCode>
  <geo:satelliteSystem>GPS</geo:satelliteSystem>
  <geo:satelliteSystem>BDS</geo:satelliteSystem>
  <geo:manufacturerSerialNumber>GRB</geo:manufacturerSerialNumber>
  <geo:dateInstalled>2020-05-02T00:00:00Z</geo:dateInstalled>
</geo:GnssReceiver></geo:gnssReceiver>
<geo:gnssAntenna xmlns:geo="urn:test"><geo:GnssAntenna>
  <geo:igsModelCode>GA-ANTENNA</geo:igsModelCode>
  <geo:antennaRadomeType>NONE</geo:antennaRadomeType>
  <geo:dateInstalled>2018-01-01T00:00:00Z</geo:dateInstalled>
</geo:GnssAntenna></geo:gnssAntenna>
)XML";

void CheckSqlite(sqlite3* database, const int status) {
    if (status != SQLITE_OK && status != SQLITE_ROW && status != SQLITE_DONE) {
        std::cerr << "SQLite status: " << status << " / "
                  << (database == nullptr ? "no database handle"
                                           : sqlite3_errmsg(database))
                  << '\n';
        std::abort();
    }
}

int EquipmentRowCount(const std::filesystem::path& path) {
    sqlite3* database = nullptr;
    CheckSqlite(database, sqlite3_open_v2(
        path.string().c_str(), &database, SQLITE_OPEN_READONLY, nullptr));
    sqlite3_stmt* statement = nullptr;
    CheckSqlite(database, sqlite3_prepare_v2(
        database,
        "SELECT COUNT(*) FROM site_log_equipment;",
        -1,
        &statement,
        nullptr));
    const int status = sqlite3_step(statement);
    CheckSqlite(database, status);
    const int count = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return count;
}

void DeleteEquipmentRows(const std::filesystem::path& path) {
    sqlite3* database = nullptr;
    CheckSqlite(database, sqlite3_open_v2(
        path.string().c_str(), &database, SQLITE_OPEN_READWRITE, nullptr));
    CheckSqlite(database, sqlite3_exec(
        database, "DELETE FROM site_log_equipment;", nullptr, nullptr, nullptr));
    sqlite3_close(database);
}

void AssertActive(const std::filesystem::path& path,
                  const std::string& date,
                  const std::string& receiver,
                  const std::string& systems,
                  const std::string& antenna) {
    std::unordered_map<std::string, SiteLogRecord> records;
    std::wstring error;
    assert(LoadSiteLogSummaryMap(path, date, records, error));
    assert(records.contains("TIME00USA"));
    const auto& record = records.at("TIME00USA");
    assert(record.receiverType == receiver);
    assert(record.receiverSystems == systems);
    assert(record.antennaType == antenna);
    assert(record.equipmentHistory.size() == 6);
}

}  // namespace

int main() {
    const auto cdata = ParseSiteLogEquipmentHistory(R"XML(
<geo:gnssReceiver><geo:GnssReceiver>
<geo:igsModelCode codeListValue="LEICA SR530"><![CDATA[LEICA SR530]]></geo:igsModelCode>
<geo:satelliteSystem><![CDATA[GPS]]></geo:satelliteSystem>
<geo:satelliteSystem><![CDATA[GLO]]></geo:satelliteSystem>
<geo:firmwareVersion><![CDATA[5.10]]></geo:firmwareVersion>
<geo:dateInstalled><![CDATA[2002-10-04T00:00:00Z]]></geo:dateInstalled>
<geo:dateRemoved><![CDATA[2006-07-26T00:00:00Z]]></geo:dateRemoved>
</geo:GnssReceiver></geo:gnssReceiver>
<geo:gnssAntenna><geo:GnssAntenna>
<geo:igsModelCode><![CDATA[LEIAT504        LEIS]]></geo:igsModelCode>
<geo:dateInstalled><![CDATA[2002-10-04T00:00:00Z]]></geo:dateInstalled>
</geo:GnssAntenna></geo:gnssAntenna>)XML");
    assert(cdata.size() == 2);
    assert(cdata[0].type == "LEICA SR530");
    assert(cdata[0].systems == "GPS+GLO");
    assert(cdata[0].firmware == "5.10");
    assert(cdata[0].dateInstalled == "2002-10-04T00:00:00Z");
    assert(cdata[0].dateRemoved == "2006-07-26T00:00:00Z");
    assert(cdata[1].type == "LEIAT504        LEIS");
    const auto epochs = ParseSiteLogEquipmentHistory(kHistoryText);
    assert(epochs.size() == 6);
    assert(epochs[0].kind == "receiver");
    assert(epochs[0].sequence == 1);
    assert(epochs[0].type == "RECEIVER-A");
    assert(epochs[0].systems == "GPS");
    assert(epochs[0].dateInstalled == "2019-01-01");
    assert(epochs[0].dateRemoved == "2020-06-01");
    assert(epochs[1].kind == "receiver");
    assert(epochs[1].sequence == 2);
    assert(epochs[1].type == "RECEIVER-B");
    assert(epochs[1].systems == "GPS+BDS");
    assert(epochs[1].dateInstalled == "2020-06-01");
    assert(epochs[1].dateRemoved == "2021-06-01");
    assert(epochs[2].kind == "receiver");
    assert(epochs[2].sequence == 3);
    assert(epochs[2].type == "RECEIVER-C");
    assert(epochs[2].systems == "GPS+BDS+GAL");
    assert(epochs[2].dateInstalled == "2021-06-01");
    assert(epochs[2].dateRemoved.empty());
    assert(epochs[3].kind == "antenna");
    assert(epochs[3].sequence == 1);
    assert(epochs[3].type == "ANTENNA-A");
    assert(epochs[3].radome == "NONE");
    assert(epochs[4].kind == "antenna");
    assert(epochs[4].sequence == 2);
    assert(epochs[4].type == "ANTENNA-B");
    assert(epochs[4].radome == "SCIS");
    assert(epochs[5].kind == "antenna");
    assert(epochs[5].sequence == 3);
    assert(epochs[5].type == "ANTENNA-C");
    assert(epochs[5].radome == "SCIT");

    // M3G/GeodesyML-derived summaries often omit the word "Information" in
    // section headings.  They must still produce a usable current snapshot.
    const auto shortHeading = ParseSiteLogEquipmentHistory(kShortHeadingText);
    assert(shortHeading.size() == 2);
    assert(shortHeading[0].kind == "receiver");
    assert(shortHeading[0].type == "SHORT-RECEIVER");
    assert(shortHeading[1].kind == "antenna");
    assert(shortHeading[1].type == "SHORT-ANTENNA");

    // GA GeodesyML contains repeated GnssReceiver/GnssAntenna blocks.  The
    // XML extractor must retain every interval, including removed hardware,
    // rather than selecting only the open block.
    const auto geodesyHistory = ParseSiteLogEquipmentHistory(kGeodesyMlText);
    assert(geodesyHistory.size() == 3);
    assert(geodesyHistory[0].kind == "receiver");
    assert(geodesyHistory[0].sequence == 1);
    assert(geodesyHistory[0].systems == "GPS+GAL");
    assert(geodesyHistory[0].dateRemoved
           == "2020-05-02T00:00:00Z");
    assert(geodesyHistory[1].kind == "receiver");
    assert(geodesyHistory[1].sequence == 2);
    assert(geodesyHistory[2].kind == "antenna");
    assert(geodesyHistory[2].sequence == 1);

    const auto parsed = ParseSiteLogText(
        kHistoryText,
        "https://example.invalid/site-log/TIME00USA_20240102.log",
        "https://example.invalid/site-log/",
        1);
    assert(parsed.stationId == "TIME00USA");
    assert(parsed.fourCharId == "TIME");
    assert(parsed.equipmentHistory.size() == epochs.size());
    assert(parsed.equipmentHistory.front().stationId == "TIME00USA");
    assert(parsed.equipmentHistory.front().sourceUrl == parsed.sourceUrl);

    // Keep the test database in the build/work tree rather than C:\TEMP;
    // some deployments intentionally have a full system drive.
    const auto path = std::filesystem::current_path()
        / "gnss_site_log_history_test.sqlite";
    std::error_code removeError;
    std::filesystem::remove(path, removeError);

    std::wstring error;
    if (!StoreSiteLogRecords(path, {parsed}, error)) {
        std::wcerr << L"Store failed for " << path.wstring() << L": "
                   << error << L'\n';
        std::abort();
    }
    std::cerr << "history test DB: " << path.string() << '\n';
    assert(EquipmentRowCount(path) == static_cast<int>(epochs.size()));

    // Verify the migration path used for databases built before the history
    // table existed: deleting the narrow rows and rebuilding them from the
    // retained raw Site Log must be idempotent and lossless.
    DeleteEquipmentRows(path);
    assert(EquipmentRowCount(path) == 0);
    int recordsProcessed = 0;
    int epochsStored = 0;
    assert(BackfillSiteLogEquipment(
        path, error, &recordsProcessed, &epochsStored));
    assert(recordsProcessed == 1);
    assert(epochsStored == static_cast<int>(epochs.size()));
    assert(EquipmentRowCount(path) == static_cast<int>(epochs.size()));
    assert(BackfillSiteLogEquipment(
        path, error, &recordsProcessed, &epochsStored));
    assert(EquipmentRowCount(path) == static_cast<int>(epochs.size()));

    // Installed dates are inclusive; removed dates are exclusive.  The
    // selected scalar fields must therefore follow the actual equipment
    // interval, rather than the Site Log document's revision_date.
    AssertActive(path, "2018-12-31", "", "", "");
    AssertActive(path, "2019-12-31", "RECEIVER-A", "GPS", "ANTENNA-A");
    AssertActive(path, "2020-06-01", "RECEIVER-B", "GPS+BDS", "ANTENNA-B");
    AssertActive(path, "2021-06-01", "RECEIVER-C", "GPS+BDS+GAL", "ANTENNA-C");
    AssertActive(path, "2026-08-30", "RECEIVER-C", "GPS+BDS+GAL", "ANTENNA-C");
    AssertActive(path, "", "RECEIVER-C", "GPS+BDS+GAL", "ANTENNA-C");

    std::filesystem::remove(path, removeError);
    std::cout << "SiteLogHistoryTest passed\n";
    return 0;
}
