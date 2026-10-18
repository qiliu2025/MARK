#include "DateUtil.h"
#include "IgsData.h"
#include "HttpClient.h"
#include "ObservationSearchDb.h"
#include "SpaceWeather.h"

#include "sqlite3.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Row {
    std::string stationKey;
    std::string stationId;
    std::string fourCharId;
    std::string country;
    double latitude = 0.0;
    double longitude = 0.0;
    bool hasCoordinate = true;
    int rinexFormat = 2;
    std::string fileVariant;
    std::string sampleInterval = "30S";
    // Optional date whitelist used by the reference-frame window regression.
    // An empty list preserves the historical test fixture behavior (present
    // on every date supplied to CreateSource).
    std::vector<std::string> availableDates;
};

std::string FileKeyForStation(const Row& row) {
    std::string key = row.stationKey;
    std::transform(key.begin(), key.end(), key.begin(),
        [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    if (!row.fileVariant.empty()) key += ":" + row.fileVariant;
    if (key.rfind("four:", 0) == 0) return key;
    return key;
}

bool Exec(sqlite3* database, const char* sql) {
    char* message = nullptr;
    const int status = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    if (status != SQLITE_OK) {
        std::cerr << "SQLite: " << (message == nullptr
            ? sqlite3_errmsg(database) : message) << '\n';
        sqlite3_free(message);
        return false;
    }
    return true;
}

bool CheckWeatherPreview(const std::filesystem::path& directory) {
    const auto path = directory / "space-weather-preview.sqlite";
    sqlite3* db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    const bool created = Exec(db,
        "CREATE TABLE daily(date TEXT,kp_max REAL,ap_daily REAL,dst_min REAL,"
        "dst_mean REAL,f107_daily REAL,f107_81day REAL,kp_count INTEGER,dst_count INTEGER,source TEXT,status TEXT);"
        "INSERT INTO daily VALUES('2018-06-01',3,10,-50,-25,70,80,8,24,'fixture','complete');"
        "INSERT INTO daily VALUES('2018-06-02',5,20,-100,-50,150,80,8,24,'fixture','complete');"
        "INSERT INTO daily VALUES('2018-06-03',0,0,NULL,NULL,NULL,NULL,8,0,'fixture','partial');");
    sqlite3_close(db);
    if (!created) return false;
    const auto start = MakeQueryDate(2018, 6, 1), end = MakeQueryDate(2018, 6, 4);
    const auto check = [&](const ObservationRangeOptions& options, const std::vector<bool>& expected, int missing) {
        const auto preview = PreviewSpaceWeatherDates(path, start, end, options);
        const auto selection = SelectSpaceWeatherDates(path, start, end, options);
        if (!preview.error.empty() || preview.days.size() != expected.size()
            || preview.requestedDays != 4 || preview.missingDays != missing) return false;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            const auto& day = preview.days[i];
            if (day.retained != expected[i] || day.retained != selection.Contains(day.ordinal)) return false;
            if (day.retained != (day.failedMetrics == 0 && day.missingMetrics == 0)) return false;
        }
        return true;
    };
    ObservationRangeOptions options;
    if (!check(options, {true,true,true,true}, 0)) return false;
    options.kpMaxFilter = {SpaceWeatherFilterMode::KeepRange, 0, 3};
    if (!check(options, {true,false,true,false}, 1)) return false;
    options.kpMaxFilter = {SpaceWeatherFilterMode::KeepAbove, 3, 3};
    if (!check(options, {false,true,false,false}, 1)) return false;
    options.kpMaxFilter = {SpaceWeatherFilterMode::ExcludeAbove, 3, 3};
    if (!check(options, {true,false,true,false}, 1)) return false;
    options = {};
    options.dstMinFilter = {SpaceWeatherFilterMode::KeepAbove, -50, -50};
    if (!check(options, {true,true,false,false}, 2)) return false;
    options.dstMinFilter = {SpaceWeatherFilterMode::ExcludeAbove, -50, -50};
    if (!check(options, {false,false,false,false}, 2)) return false;
    options.dstMinFilter = {SpaceWeatherFilterMode::KeepRange, -100, -50};
    if (!check(options, {true,true,false,false}, 2)) return false;
    options = {};
    options.f107Filter = {SpaceWeatherFilterMode::KeepRange, 70, 150};
    if (!check(options, {true,true,false,false}, 2)) return false;
    options.f107Filter = {SpaceWeatherFilterMode::KeepAbove, 70, 70};
    if (!check(options, {false,true,false,false}, 2)) return false;
    options.f107Filter = {SpaceWeatherFilterMode::ExcludeAbove, 70, 70};
    if (!check(options, {true,false,false,false}, 2)) return false;
    options.useAdjustedF107 = true;
    if (!check(options, {false,false,false,false}, 4)) return false;
    options.useAdjustedF107 = false;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) return false;
    const bool upgraded = Exec(db,
        "ALTER TABLE daily ADD COLUMN f107_adjusted REAL;"
        "UPDATE daily SET f107_adjusted=90 WHERE date='2018-06-01';"
        "UPDATE daily SET f107_adjusted=65 WHERE date='2018-06-02';");
    sqlite3_close(db);
    if (!upgraded) return false;
    options = {};
    options.f107Filter = {SpaceWeatherFilterMode::KeepAbove, 80, 80};
    options.useAdjustedF107 = true;
    if (!check(options, {true,false,false,false}, 2)) return false;
    const auto adjusted = PreviewSpaceWeatherDates(path, start, end, options);
    if (adjusted.days[0].values.f107 != 90 || adjusted.days[1].values.f107 != 65) return false;
    options.useAdjustedF107 = false;
    if (!check(options, {false,true,false,false}, 2)) return false;
    options.f107Filter = {SpaceWeatherFilterMode::ExcludeAbove, 70, 70};
    options.kpMaxFilter = {SpaceWeatherFilterMode::KeepRange, 0, 3};
    options.dstMinFilter = {SpaceWeatherFilterMode::KeepAbove, -50, -50};
    if (!check(options, {true,false,false,false}, 2)) return false;
    const auto combined = PreviewSpaceWeatherDates(path, start, end, options);
    if (combined.retainedDays != 1 || combined.days[1].failedMetrics != 5
        || combined.days[2].missingMetrics != 6 || combined.days[3].missingMetrics != 7) return false;
    if (PreviewSpaceWeatherDates(path, end, start, options).error.empty()) return false;
    if (PreviewSpaceWeatherDates(directory / "absent.sqlite", start, end, options).error.empty()) return false;
    std::cout << "Space-weather preview threshold, intersection and missing-date regression passed.\n";
    return true;
}

bool CreateSource(
    const std::filesystem::path& path,
    const ObservationNetwork network,
    const std::vector<Row>& rows,
    const std::vector<std::string>& dates = {"2025-01-01"}) {
    sqlite3* database = nullptr;
    const std::string utf8 = path.string();
    if (sqlite3_open_v2(
            utf8.c_str(), &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        std::cerr << "Cannot create source DB: " << path << '\n';
        if (database != nullptr) sqlite3_close(database);
        return false;
    }
    const bool schemaOkay = Exec(database,
        "CREATE TABLE daily_scans("
        " observation_date TEXT PRIMARY KEY,status TEXT NOT NULL,"
        " is_provisional INTEGER NOT NULL DEFAULT 0,"
        " source_url TEXT NOT NULL DEFAULT '',"
        " station_count INTEGER NOT NULL DEFAULT 0,"
        " file_count INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE daily_files("
            " observation_date TEXT NOT NULL,station_key TEXT NOT NULL,"
        " station_id TEXT NOT NULL,four_char_id TEXT NOT NULL,"
        " country TEXT NOT NULL,rinex_format INTEGER NOT NULL,"
        " sample_interval TEXT NOT NULL,has_coordinate INTEGER NOT NULL,"
        " latitude REAL,longitude REAL,url_override TEXT NOT NULL DEFAULT '',"
        " available_networks INTEGER NOT NULL DEFAULT 0,"
        " is_preferred INTEGER NOT NULL DEFAULT 1,"
        " file_name TEXT NOT NULL DEFAULT '',"
        " file_key TEXT NOT NULL DEFAULT '',"
        " PRIMARY KEY(observation_date,file_key));");
    if (!schemaOkay) {
        sqlite3_close(database);
        return false;
    }
    for (const auto& date : dates) {
        sqlite3_stmt* scan = nullptr;
        if (sqlite3_prepare_v2(database,
                "INSERT INTO daily_scans(observation_date,status)"
                " VALUES(?,'complete');", -1, &scan, nullptr) != SQLITE_OK) {
            sqlite3_close(database);
            return false;
        }
        sqlite3_bind_text(scan, 1, date.c_str(), -1, SQLITE_TRANSIENT);
        const bool inserted = sqlite3_step(scan) == SQLITE_DONE;
        sqlite3_finalize(scan);
        if (!inserted) {
            sqlite3_close(database);
            return false;
        }
    }
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(
            database,
            "INSERT INTO daily_files(observation_date,station_key,station_id,"
            "four_char_id,country,rinex_format,sample_interval,has_coordinate,"
            "latitude,longitude,url_override,available_networks,is_preferred,"
            "file_name,file_key) VALUES(?,?,?,?,?,?,?,?,?,?,'',?,1,?,?);",
            -1, &raw, nullptr) != SQLITE_OK) {
        std::cerr << "Cannot prepare source row: " << sqlite3_errmsg(database)
                  << '\n';
        sqlite3_close(database);
        return false;
    }
    for (const auto& date : dates) for (const auto& row : rows) {
        if (!row.availableDates.empty()
            && std::ranges::find(row.availableDates, date)
                == row.availableDates.end()) {
            continue;
        }
        sqlite3_reset(raw);
        sqlite3_clear_bindings(raw);
        sqlite3_bind_text(raw, 1, date.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(raw, 2, row.stationKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(raw, 3, row.stationId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(raw, 4, row.fourCharId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(raw, 5, row.country.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(raw, 6, row.rinexFormat);
        sqlite3_bind_text(raw, 7, row.sampleInterval.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(raw, 8, row.hasCoordinate ? 1 : 0);
        if (row.hasCoordinate) {
            sqlite3_bind_double(raw, 9, row.latitude);
            sqlite3_bind_double(raw, 10, row.longitude);
        } else {
            sqlite3_bind_null(raw, 9);
            sqlite3_bind_null(raw, 10);
        }
        sqlite3_bind_int64(raw, 11,
            static_cast<sqlite3_int64>(NetworkBit(network)));
        const std::string fileName = row.stationKey
            + (row.rinexFormat == 2 ? ".25o.gz" : ".rnx.gz");
        const std::string fileKey = FileKeyForStation(row);
        sqlite3_bind_text(raw, 12, fileName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(raw, 13, fileKey.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(raw) != SQLITE_DONE) {
            std::cerr << "Cannot insert source row: " << sqlite3_errmsg(database)
                      << '\n';
            sqlite3_finalize(raw);
            sqlite3_close(database);
            return false;
        }
    }
    sqlite3_finalize(raw);
    sqlite3_close(database);
    return true;
}

bool CheckIgsHighRateImport(const std::filesystem::path& directory) {
    const auto source = directory / "gnss_availability.sqlite";
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(
            source.string().c_str(), &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (database != nullptr) sqlite3_close(database);
        return false;
    }
    const bool created = Exec(database,
        "CREATE TABLE daily_scans("
        " network TEXT NOT NULL,observation_date TEXT NOT NULL,"
        " status TEXT NOT NULL,is_provisional INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY(network,observation_date));"
        "CREATE TABLE daily_source_scans("
        " network TEXT NOT NULL,observation_date TEXT NOT NULL,"
        " source_id INTEGER NOT NULL,status TEXT NOT NULL,"
        " PRIMARY KEY(network,observation_date,source_id));"
        "CREATE TABLE daily_files("
        " network TEXT NOT NULL,station_key TEXT NOT NULL,"
        " station_id TEXT NOT NULL,four_char_id TEXT NOT NULL,"
        " country TEXT NOT NULL,has_coordinate INTEGER NOT NULL,"
        " latitude REAL,longitude REAL);"
        "CREATE TABLE daily_observation_files("
        " network TEXT NOT NULL,observation_date TEXT NOT NULL,"
        " station_key TEXT NOT NULL,rinex_format INTEGER NOT NULL,"
        " sample_interval TEXT NOT NULL);"
        "CREATE TABLE highrate_daily_observations("
        " network TEXT NOT NULL,observation_date TEXT NOT NULL,"
        " station_key TEXT NOT NULL,station_id TEXT NOT NULL,"
        " four_char_id TEXT NOT NULL,rinex_format_mask INTEGER NOT NULL,"
        " sample_interval TEXT NOT NULL,has_data INTEGER NOT NULL,"
        " file_count INTEGER NOT NULL DEFAULT 0,"
        " source_mask INTEGER NOT NULL DEFAULT 0);"
        "INSERT INTO daily_scans(network,observation_date,status)"
        " VALUES('IGS','2025-01-01','complete');"
        "INSERT INTO daily_source_scans(network,observation_date,source_id,status)"
        " VALUES"
        " ('IGS','2025-01-01',1,'complete'),"
        " ('IGS','2025-01-01',2,'complete'),"
        " ('IGS','2025-01-01',3,'complete'),"
        " ('IGS','2025-01-01',4,'complete'),"
        " ('IGS','2025-01-01',5,'complete'),"
        " ('IGS','2025-01-01',6,'complete'),"
        " ('IGS','2025-01-01',7,'complete');"
        "INSERT INTO daily_files(network,station_key,station_id,four_char_id,"
        " country,has_coordinate,latitude,longitude)"
        " VALUES('IGS','TEST00USA','TEST00USA','TEST','USA',1,40.0,-105.0);"
        "INSERT INTO highrate_daily_observations("
        " network,observation_date,station_key,station_id,four_char_id,"
        " rinex_format_mask,sample_interval,has_data,file_count,source_mask)"
        " VALUES('IGS','2025-01-01','FOUR:TEST','TEST00USA','TEST',"
        " 2,'01S',1,96,15);");
    sqlite3_close(database);
    if (!created) return false;

    ObservationSearchBuildOptions buildOptions;
    buildOptions.sourceDirectory = directory;
    buildOptions.outputPath = directory / "igs_highrate_search.sqlite";
    buildOptions.networkMask = NetworkBit(ObservationNetwork::Igs)
        | NetworkBit(ObservationNetwork::Mgex);
    const auto build = BuildObservationSearchIndex(buildOptions);
    if (!build.complete || build.unifiedStations != 1U) {
        std::wcerr << L"IGS high-rate regression build failed: "
                   << build.error << L'\n';
        return false;
    }

    ObservationRangeOptions options;
    options.sampleInterval = "1S";
    options.rinexVersion = 3;
    for (const auto network : {
             ObservationNetwork::Igs, ObservationNetwork::Mgex}) {
        NetworkSelection selection;
        selection.mask = NetworkBit(network);
        const auto result = QueryObservationSearchIndex(
            buildOptions.outputPath,
            MakeQueryDate(2025, 1, 1), MakeQueryDate(2025, 1, 1),
            selection, options, false, {});
        if (!result.complete || result.stations.size() != 1U) {
            std::cerr << "IGS high-rate profile missing for network "
                      << static_cast<unsigned long long>(selection.mask)
                      << " count=" << result.stations.size() << '\n';
            return false;
        }
        const auto& station = result.stations.front();
        if (station.stationId != "TEST00USA" || station.fourCharId != "TEST"
            || station.country != "USA" || !station.hasCoordinate
            || std::abs(station.latitude - 40.0) >= 1.0e-9
            || std::abs(station.longitude + 105.0) >= 1.0e-9) {
            std::cerr << "IGS high-rate alias did not inherit daily metadata.\n";
            return false;
        }
    }
    return true;
}

bool Near(const double left, const double right) {
    return std::abs(left - right) < 1.0e-9;
}

bool CheckNetworkQuery(
    const std::filesystem::path& indexPath,
    const ObservationNetwork network,
    const std::size_t expectedCount,
    const std::string& expectedFour,
    const double expectedLatitude,
    const double expectedLongitude,
    const RinexFormatMask expectedFormats = RinexFormatBit(
        RinexFormat::Rinex2)) {
    NetworkSelection selection;
    selection.mask = NetworkBit(network);
    const auto result = QueryObservationSearchIndex(
        indexPath,
        MakeQueryDate(2025, 1, 1),
        MakeQueryDate(2025, 1, 1),
        selection,
        ObservationRangeOptions{},
        false,
        {});
    if (!result.complete || result.stations.size() != expectedCount) {
        std::cerr << "Unexpected query result for network "
                  << static_cast<unsigned long long>(selection.mask)
                  << ": complete=" << result.complete
                  << " count=" << result.stations.size() << '\n';
        return false;
    }
    if (expectedCount == 1) {
        const auto& station = result.stations.front();
        if (station.fourCharId != expectedFour
            || !station.hasCoordinate
            || !Near(station.latitude, expectedLatitude)
            || !Near(station.longitude, expectedLongitude)
            || EffectiveRinexFormatMask(station) != expectedFormats) {
            std::cerr << "Unexpected station identity/coordinate for network "
                      << static_cast<unsigned long long>(selection.mask)
                      << '\n';
            return false;
        }
    }
    return true;
}

bool CheckRinexFilter(
    const std::filesystem::path& indexPath,
    const int version,
    const RinexFormatMask expectedFormat) {
    NetworkSelection selection;
    selection.mask = NetworkBit(ObservationNetwork::Ngs);
    ObservationRangeOptions options;
    options.rinexVersion = version;
    const auto result = QueryObservationSearchIndex(
        indexPath,
        MakeQueryDate(2025, 1, 1),
        MakeQueryDate(2025, 1, 1),
        selection,
        options,
        false,
        {});
    return result.complete
        && result.stations.size() == 1U
        && result.stations.front().fourCharId == "OKAK"
        && EffectiveRinexFormatMask(result.stations.front()) == expectedFormat;
}

bool CheckIntervalAliases(const std::filesystem::path& indexPath) {
    NetworkSelection selection;
    selection.mask = NetworkBit(ObservationNetwork::Ngs);
    for (const std::string interval : {"1S", "01S", "1sec", "1Hz"}) {
        ObservationRangeOptions options;
        options.sampleInterval = interval;
        options.rinexVersion = 3;
        const auto result = QueryObservationSearchIndex(
            indexPath,
            MakeQueryDate(2025, 1, 1),
            MakeQueryDate(2025, 1, 1),
            selection,
            options,
            false,
            {});
        if (!result.complete || result.stations.size() != 1U
            || result.stations.front().fourCharId != "OKAK") {
            std::cerr << "One-second interval alias failed: " << interval
                      << " count=" << result.stations.size() << '\n';
            return false;
        }
    }
    return true;
}

bool CheckDeferredHydration(
    const std::filesystem::path& directory) {
    if (_wputenv_s(
            L"GNSS_DOWNLOADER_INDEX_DIR", directory.wstring().c_str()) != 0) {
        std::cerr << "Cannot set GNSS_DOWNLOADER_INDEX_DIR for hydration test.\n";
        return false;
    }

    const auto makeTarget = [](
            const char* id,
            const char* country,
            const ObservationNetwork network,
            const double latitude,
            const double longitude) {
        StationFile station;
        station.stationId = id;
        station.fourCharId = id;
        station.country = country;
        station.availableNetworks = NetworkBit(network);
        station.hasCoordinate = true;
        station.latitude = latitude;
        station.longitude = longitude;
        return station;
    };
    const std::vector<StationFile> wanted{
        makeTarget("OKAK", "USA", ObservationNetwork::Ngs,
            35.0713, -98.2154),
        makeTarget("OKAK", "USA", ObservationNetwork::SopacGarner,
            53.4112, -168.3599),
        makeTarget("STAR", "AUS", ObservationNetwork::GaGdc,
            -36.6153, 143.2574),
        makeTarget("STAR", "USA", ObservationNetwork::SopacGarner,
            46.8508, -121.7939),
        makeTarget("STAR", "USA", ObservationNetwork::PangaCwu,
            46.8508, -121.7939)
    };
    NetworkSelection selection;
    selection.mask = NetworkBit(ObservationNetwork::Ngs)
        | NetworkBit(ObservationNetwork::GaGdc)
        | NetworkBit(ObservationNetwork::SopacGarner)
        | NetworkBit(ObservationNetwork::PangaCwu);
    const auto hydrated = LoadObservationDateRangeFilesForStations(
        MakeQueryDate(2025, 1, 1),
        MakeQueryDate(2025, 1, 1),
        selection,
        ObservationRangeOptions{},
        wanted);
    if (!hydrated.error.empty() || hydrated.stations.size() != 4U
        || hydrated.periodFiles.size() != 4U) {
        std::wcerr << L"Unexpected deferred hydration result: error='"
                   << hydrated.error << L"' stations="
                   << hydrated.stations.size() << L" files="
                   << hydrated.periodFiles.size() << L"\n";
        return false;
    }

    bool sawNgsOkak = false;
    bool sawSopacOkak = false;
    bool sawGaStar = false;
    bool sawSopacPangaStar = false;
    for (const auto& station : hydrated.stations) {
        if (station.stationId == "OKAK"
            && station.availableNetworks == NetworkBit(ObservationNetwork::Ngs)
            && std::abs(station.latitude - 35.0713) < 1.0e-9) {
            sawNgsOkak = true;
        }
        if (station.stationId == "OKAK"
            && station.availableNetworks == NetworkBit(ObservationNetwork::SopacGarner)
            && std::abs(station.latitude - 53.4112) < 1.0e-9) {
            sawSopacOkak = true;
        }
        if (station.stationId == "STAR"
            && station.availableNetworks == NetworkBit(ObservationNetwork::GaGdc)
            && std::abs(station.latitude + 36.6153) < 1.0e-9) {
            sawGaStar = true;
        }
        if (station.stationId == "STAR"
            && (station.availableNetworks
                & (NetworkBit(ObservationNetwork::SopacGarner)
                    | NetworkBit(ObservationNetwork::PangaCwu)))
                == (NetworkBit(ObservationNetwork::SopacGarner)
                    | NetworkBit(ObservationNetwork::PangaCwu))
            && std::abs(station.latitude - 46.8508) < 1.0e-9) {
            sawSopacPangaStar = true;
        }
    }
    return sawNgsOkak && sawSopacOkak && sawGaStar && sawSopacPangaStar;
}

bool CheckStationDateWindows(
    const std::filesystem::path& directory) {
    const auto start = MakeQueryDate(2020, 5, 16);
    const auto end = MakeQueryDate(2020, 5, 17);
    const std::vector<std::string> dates{"2020-05-16", "2020-05-17"};
    const auto four = [](const char* code) {
        return std::string("FOUR:") + code;
    };
    auto makeRow = [&](const char* code,
                       std::vector<std::string> availableDates) {
        Row row;
        row.stationKey = four(code);
        row.stationId = code;
        row.fourCharId = code;
        row.country = "USA";
        row.latitude = 35.0;
        row.longitude = -98.0;
        row.availableDates = std::move(availableDates);
        return row;
    };
    const auto source = directory / "ngs_availability.sqlite";
    if (!CreateSource(source, ObservationNetwork::Ngs, {
            makeRow("EARL", {"2020-05-16"}),
            makeRow("LATE", {"2020-05-17"}),
            makeRow("CROS", dates),
            makeRow("GAPS", dates)}, dates)) {
        return false;
    }

    const auto indexPath = directory / "window_search.sqlite";
    ObservationSearchBuildOptions buildOptions;
    buildOptions.sourceDirectory = directory;
    buildOptions.outputPath = indexPath;
    buildOptions.networkMask = NetworkBit(ObservationNetwork::Ngs);
    const auto build = BuildObservationSearchIndex(buildOptions);
    if (!build.complete || build.unifiedStations != 4) {
        std::cerr << "Station-window index build failed: complete="
                  << build.complete << " stations=" << build.unifiedStations
                  << '\n';
        std::wcerr << L"Station-window index error: " << build.error << L'\n';
        return false;
    }

    NetworkSelection selection;
    selection.mask = NetworkBit(ObservationNetwork::Ngs);
    const auto ordinary = QueryObservationSearchIndex(
        indexPath, start, end, selection, ObservationRangeOptions{}, false, {});
    if (!ordinary.complete || ordinary.expectedDays != 2
        || ordinary.stations.size() != 4U) {
        std::cerr << "Ordinary cross-boundary query changed unexpectedly.\n";
        return false;
    }

    // LATE has no file on the first query day. Its display equipment must
    // come from the second day, while EARL retains the earlier receiver.
    const int firstDay = static_cast<int>(std::chrono::sys_days{
        std::chrono::year{2020}/5/16}.time_since_epoch().count());
    sqlite3* equipmentDatabase = nullptr;
    if (sqlite3_open(indexPath.string().c_str(), &equipmentDatabase) != SQLITE_OK) {
        if (equipmentDatabase) sqlite3_close(equipmentDatabase);
        return false;
    }
    const std::string equipmentSql =
        "INSERT INTO site_records(record_id,station_uid,applicability_mask,tier,network_order) "
        "SELECT 1,station_uid,4,0,0 FROM stations WHERE four_char_id='LATE';"
        "INSERT INTO site_records(record_id,station_uid,applicability_mask,tier,network_order) "
        "SELECT 2,station_uid,4,0,0 FROM stations WHERE four_char_id='EARL';"
        "INSERT INTO site_segments(record_id,kind,first_day,last_day_exclusive,equipment_type,systems) "
        "SELECT record_id,0," + std::to_string(firstDay) + "," + std::to_string(firstDay+1)
        + ",'OLD RECEIVER','GPS' FROM site_records;"
        "INSERT INTO site_segments(record_id,kind,first_day,last_day_exclusive,equipment_type,systems) "
        "SELECT record_id,0," + std::to_string(firstDay+1) + "," + std::to_string(firstDay+2)
        + ",'NEW RECEIVER','GPS+GLONASS' FROM site_records;"
        "INSERT INTO site_segments(record_id,kind,first_day,last_day_exclusive,equipment_type,radome) "
        "SELECT record_id,1," + std::to_string(firstDay) + "," + std::to_string(firstDay+2)
        + ",'TEST ANTENNA NONE','NONE' FROM site_records;";
    const bool equipmentInserted = Exec(equipmentDatabase, equipmentSql.c_str());
    sqlite3_close(equipmentDatabase);
    if (!equipmentInserted) return false;
    const auto displayed = QueryObservationSearchIndex(
        indexPath, start, end, selection, ObservationRangeOptions{}, false, {}, true);
    if (!displayed.complete || !displayed.siteLogMetadataIncluded
        || displayed.stations.size() != ordinary.stations.size()) {
        std::cerr << "Display metadata changed query completion or station count.\n";
        return false;
    }
    for (const auto& station : displayed.stations) {
        const auto prior = std::ranges::find_if(ordinary.stations, [&](const StationFile& row) {
            return row.stationId == station.stationId;
        });
        if (prior == ordinary.stations.end() || prior->availableDays != station.availableDays
            || prior->expectedDays != station.expectedDays) return false;
        if (station.fourCharId == "LATE"
            && (station.siteLogReceiver != "NEW RECEIVER"
                || station.siteLogSystems != "GPS+GLO" || station.siteLogRadome != "NONE")) {
            std::cerr << "Display equipment does not match first available day.\n";
            return false;
        }
        if (station.fourCharId == "EARL" && station.siteLogReceiver != "OLD RECEIVER") return false;
    }
    ObservationRangeOptions receiverFilter;
    receiverFilter.receiverInclude = {"NEW RECEIVER"};
    const auto filtered = QueryObservationSearchIndex(
        indexPath, start, end, selection, receiverFilter, false, {}, true);
    if (!filtered.complete || filtered.stations.size() != 1U
        || filtered.stations.front().fourCharId != "LATE") return false;

    ObservationRangeOptions windows;
    windows.restrictStationDates = true;
    windows.stationDateWindows = {
        {start, start, {"EARL", "LATE"}},
        {start, end, {"CROS"}},
        {start, start, {"GAPS"}},
        {end, end, {"GAPS"}}
    };
    const auto restricted = QueryObservationSearchIndex(
        indexPath, start, end, selection, windows, false, {});
    if (!restricted.complete || restricted.stations.size() != 3U) {
        std::cerr << "Unexpected restricted station count: "
                  << restricted.stations.size() << '\n';
        return false;
    }
    const auto findStation = [&](const char* code) -> const StationFile* {
        const auto found = std::ranges::find_if(restricted.stations,
            [&](const StationFile& station) {
                return station.fourCharId == code;
            });
        return found == restricted.stations.end() ? nullptr : &*found;
    };
    const auto* early = findStation("EARL");
    const auto* cross = findStation("CROS");
    const auto* gap = findStation("GAPS");
    if (early == nullptr || cross == nullptr || gap == nullptr
        || findStation("LATE") != nullptr
        || early->availableDays != 1 || early->expectedDays != 1
        || early->completenessPercent != 100.0
        || cross->availableDays != 2 || cross->expectedDays != 2
        || gap->availableDays != 2 || gap->expectedDays != 2) {
        std::cerr << "Station date-window availability/denominator mismatch.\n";
        return false;
    }

    // A frame window that names no stations is fail-closed.
    ObservationRangeOptions emptyWindows;
    emptyWindows.restrictStationDates = true;
    const auto empty = QueryObservationSearchIndex(
        indexPath, start, end, selection, emptyWindows, false, {});
    if (!empty.complete || !empty.stations.empty()) {
        std::cerr << "Empty station-date windows did not exclude all stations.\n";
        return false;
    }

    // With a common two-day window, the one-day EARL station is filtered by
    // both completeness policies while the continuous CROS/GAPS stations
    // remain eligible.
    ObservationRangeOptions thresholds;
    thresholds.restrictStationDates = true;
    thresholds.stationDateWindows = {
        {start, end, {"EARL", "CROS", "GAPS"}}
    };
    thresholds.completenessMode = CompletenessMode::RatioAtLeast;
    thresholds.minimumCompletenessPercent = 100.0;
    const auto ratio = QueryObservationSearchIndex(
        indexPath, start, end, selection, thresholds, false, {});
    if (!ratio.complete || ratio.stations.size() != 2U
        || std::ranges::any_of(ratio.stations, [](const StationFile& station) {
            return station.fourCharId == "EARL";
        })) {
        std::cerr << "RatioAtLeast did not remove partial station.\n";
        return false;
    }
    thresholds.completenessMode = CompletenessMode::DaysAtLeast;
    thresholds.minimumAvailableDays = 2;
    const auto days = QueryObservationSearchIndex(
        indexPath, start, end, selection, thresholds, false, {});
    if (!days.complete || days.stations.size() != 2U) {
        std::cerr << "DaysAtLeast did not remove partial station.\n";
        return false;
    }

    // The eager manifest must apply the same window before retaining daily
    // files: EARL contributes one file and CROS/GAPS two each; LATE is absent.
    if (_wputenv_s(L"GNSS_DOWNLOADER_INDEX_DIR",
                   directory.wstring().c_str()) != 0) {
        return false;
    }
    const auto hydrated = LoadObservationDateRangeFilesForStations(
        start, end, selection, windows, restricted.stations);
    if (!hydrated.error.empty() || hydrated.stations.size() != 3U
        || hydrated.periodFiles.size() != 5U
        || std::ranges::any_of(hydrated.periodFiles,
            [](const DatedStationFile& file) {
                return file.station.fourCharId == "LATE";
            })) {
        std::wcerr << L"Station-window hydration mismatch: error='"
                   << hydrated.error << L"' stations=" << hydrated.stations.size()
                   << L" files=" << hydrated.periodFiles.size() << L"\n";
        return false;
    }
    return true;
}

bool CheckDailyObservationCompleteness(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    const auto source = directory / "gnss_availability.sqlite";
    sqlite3* database = nullptr;
    if (sqlite3_open(source.string().c_str(), &database) != SQLITE_OK) {
        if (database) sqlite3_close(database);
        return false;
    }
    const bool created = Exec(database,
        "CREATE TABLE daily_scans(network TEXT,observation_date TEXT,status TEXT,is_provisional INTEGER DEFAULT 0);"
        "CREATE TABLE daily_source_scans(network TEXT,observation_date TEXT,source_id INTEGER,status TEXT);"
        "CREATE TABLE daily_files(network TEXT,station_key TEXT,station_id TEXT,four_char_id TEXT,"
        "country TEXT,has_coordinate INTEGER,latitude REAL,longitude REAL);"
        "CREATE TABLE daily_observation_files(network TEXT,observation_date TEXT,station_key TEXT,"
        "rinex_format INTEGER,sample_interval TEXT);"
        "CREATE TABLE daily_observation_quality(network TEXT,station_key TEXT,observation_date TEXT,"
        "observations_json TEXT,latency_json TEXT,position_json TEXT,residuals_json TEXT,"
        "multipath_json TEXT,cycle_slips_json TEXT,PRIMARY KEY(network,station_key,observation_date));"
        "INSERT INTO daily_scans VALUES('IGS','2025-01-01','complete',0),('IGS','2025-01-02','complete',0);"
        "INSERT INTO daily_source_scans SELECT s.network,s.observation_date,p.id,s.status FROM daily_scans s "
        "CROSS JOIN (SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 "
        "UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7) p;"
        "INSERT INTO daily_files VALUES"
        "('IGS','PASS00USA','PASS00USA','PASS','USA',1,30,-90),"
        "('IGS','DIPS00USA','DIPS00USA','DIPS','USA',1,31,-91),"
        "('IGS','WGHT00USA','WGHT00USA','WGHT','USA',1,32,-92),"
        "('IGS','MISS00USA','MISS00USA','MISS','USA',1,33,-93),"
        "('IGS','ZERO00USA','ZERO00USA','ZERO','USA',1,34,-94),"
        "('IGS','NONE00USA','NONE00USA','NONE','USA',1,35,-95),"
        "('IGS','ABOV00USA','ABOV00USA','ABOV','USA',1,36,-96);"
        "INSERT INTO daily_observation_files SELECT 'IGS',s.observation_date,f.station_key,3,'30S'"
        " FROM daily_scans s CROSS JOIN daily_files f;"
        "INSERT INTO daily_observation_quality(network,station_key,observation_date,observations_json,multipath_json) VALUES"
        "('IGS','PASS00USA','2025-01-01','{\"GPS\":100,\"GPS Obs\":90}','{\"GPS MP1\":20}'),"
        "('IGS','PASS00USA','2025-01-02','{\"GPS\":100,\"GPS Obs\":95}','{\"GPS MP1\":20}'),"
        "('IGS','DIPS00USA','2025-01-01','{\"GPS\":100,\"GPS Obs\":80}','{\"GPS MP1\":10}'),"
        "('IGS','DIPS00USA','2025-01-02','{\"GPS\":100,\"GPS Obs\":100}','{\"GPS MP1\":50}'),"
        "('IGS','WGHT00USA','2025-01-01','{\"GPS\":100,\"GPS Obs\":80}','{\"GPS MP1\":40}'),"
        "('IGS','WGHT00USA','2025-01-02','{\"GPS\":1000,\"GPS Obs\":1000}','{\"GPS MP1\":10}'),"
        "('IGS','MISS00USA','2025-01-01','{\"GPS\":100,\"GPS Obs\":100}','{\"GPS MP1\":20}'),"
        "('IGS','ZERO00USA','2025-01-01','{\"GPS\":0,\"GPS Obs\":100}','{\"GPS MP1\":20}'),"
        "('IGS','ZERO00USA','2025-01-02','{\"GPS\":100,\"GPS Obs\":100}','{\"GPS MP1\":20}'),"
        "('IGS','NONE00USA','2025-01-01','{\"GPS\":100}','{\"GPS MP1\":20}'),"
        "('IGS','NONE00USA','2025-01-02','{\"GPS\":100,\"GPS Obs\":100}','{\"GPS MP1\":20}'),"
        "('IGS','ABOV00USA','2025-01-01','{\"GPS\":100,\"GPS Obs\":110}','{\"GPS MP1\":50}'),"
        "('IGS','ABOV00USA','2025-01-02','{\"GPS\":100,\"GPS Obs\":100}','{\"GPS MP1\":50}');"
        "UPDATE daily_observation_quality SET latency_json='{\"Latency\":1}',"
        "position_json='{\"ENV Detrended Time Series\":-6,\"North Detrended\":3,\"Vertical Detrended\":0}',"
        "residuals_json='{\"East\":-6,\"North\":3,\"Vertical\":0}',"
        "cycle_slips_json='{\"GPS\":0,\"BDS\":0}';"
        "UPDATE daily_observation_quality SET latency_json='{\"Latency\":40}',"
        "position_json='{\"ENV Detrended Time Series\":-40,\"North Detrended\":40,\"Vertical Detrended\":40}',"
        "residuals_json='{\"East\":-40,\"North\":40,\"Vertical\":40}',"
        "cycle_slips_json='{\"GPS\":600,\"BDS\":600}'"
        " WHERE station_key='DIPS00USA' AND observation_date='2025-01-02';"
        "UPDATE daily_observation_quality SET multipath_json='{\"GPS MP1\":0}'"
        " WHERE station_key='ZERO00USA' AND observation_date='2025-01-01';");
    sqlite3_close(database);
    if (!created) return false;
    ObservationSearchBuildOptions build;
    build.sourceDirectory = directory;
    build.outputPath = directory / "daily_quality_search.sqlite";
    build.networkMask = NetworkBit(ObservationNetwork::Igs);
    const auto built = BuildObservationSearchIndex(build);
    if (!built.complete || built.unifiedStations != 7) {
        std::wcerr << L"Daily quality fixture build failed: " << built.error << L'\n';
        return false;
    }
    NetworkSelection selection;
    selection.mask = NetworkBit(ObservationNetwork::Igs);
    const auto first = MakeQueryDate(2025, 1, 1), last = MakeQueryDate(2025, 1, 2);
    const auto check = [&](const QueryDate& start, const QueryDate& end,
                           const ObservationRangeOptions& options,
                           std::vector<std::string> expected) {
        const auto result = QueryObservationSearchIndex(build.outputPath, start, end,
            selection, options, false, {});
        std::vector<std::string> actual;
        for (const auto& station : result.stations) actual.push_back(station.fourCharId);
        std::ranges::sort(actual);
        std::ranges::sort(expected);
        if (!result.complete || actual != expected) {
            std::wcerr << L"Daily quality query failed: " << result.error << L'\n';
            std::cerr << "Expected";
            for (const auto& name : expected) std::cerr << ' ' << name;
            std::cerr << "; actual";
            for (const auto& name : actual) std::cerr << ' ' << name;
            std::cerr << '\n';
            return false;
        }
        return true;
    };
    ObservationRangeOptions options;
    options.sampleInterval = "30S";
    options.rinexVersion = 3;
    options.completenessMode = CompletenessMode::RatioAtLeast;
    options.minimumCompletenessPercent = 100;
    if (!check(first, last, options, {"PASS","DIPS","WGHT","MISS","ZERO","NONE","ABOV"})) return false;
    options.observationCompletenessEnabled = true;
    options.minimumObservationCompletenessPercent = 90;
    // A day at exactly 90% passes. Neither an equal-weight nor a heavily
    // weighted good day may hide a bad day. Missing/invalid days fail.
    if (!check(first, last, options, {"PASS","ABOV"})) return false;
    if (!check(last, last, options, {"PASS","DIPS","WGHT","ZERO","NONE","ABOV"})) return false;
    options.minimumObservationCompletenessPercent = 100;
    if (!check(first, last, options, {"ABOV"})) return false;
    options.minimumObservationCompletenessPercent = 90;
    options.restrictStationDates = true;
    options.stationDateWindows = {{last,last,{"DIPS"}}};
    if (!check(first, last, options, {"DIPS"})) return false;
    options.restrictStationDates = false;
    options.stationDateWindows.clear();

    const auto weatherPath = directory / "space_weather_availability.sqlite";
    if (sqlite3_open(weatherPath.string().c_str(), &database) != SQLITE_OK) return false;
    const bool weatherCreated = Exec(database,
        "CREATE TABLE daily(date TEXT,kp_max REAL,ap_daily REAL,dst_min REAL,dst_mean REAL,"
        "f107_daily REAL,f107_81day REAL,kp_count INTEGER,dst_count INTEGER,source TEXT,status TEXT);"
        "INSERT INTO daily VALUES('2025-01-01',5,10,-10,-5,100,100,8,24,'fixture','complete'),"
        "('2025-01-02',1,10,-10,-5,100,100,8,24,'fixture','complete');");
    sqlite3_close(database);
    if (!weatherCreated) return false;
    if (_wputenv_s(L"GNSS_DOWNLOADER_INDEX_DIR", directory.wstring().c_str()) != 0) return false;
    options.kpMaxFilter = {SpaceWeatherFilterMode::KeepRange,0,3};
    if (!check(first, last, options, {"PASS","DIPS","WGHT","ZERO","NONE","ABOV"})) return false;
    options.kpMaxFilter = {};

    // All six metrics are daily. A good date cannot compensate for a bad or
    // unknown date at 100% coverage, even if the old period mean would pass.
    options.observationCompletenessEnabled = false;
    options.meanMultipathEnabled = true;
    options.maximumMeanMultipathCentimeters = 30;
    if (!check(first, last, options, {"PASS","NONE"})) return false;
    options.meanMultipathEnabled = false;
    options.latencyEnabled = true;
    options.maximumMeanLatencyHours = 24;
    if (!check(first, last, options, {"PASS","WGHT","ZERO","NONE","ABOV"})) return false;
    options.latencyEnabled = false;
    options.positionTimeSeriesEnabled = true;
    options.maximumMeanAbsoluteDetrendedPositionMillimeters = 30;
    if (!check(first, last, options, {"PASS","WGHT","ZERO","NONE","ABOV"})) return false;
    options.positionTimeSeriesEnabled = false;
    options.residualsEnabled = true;
    options.maximumMeanAbsoluteResidualMillimeters = 30;
    if (!check(first, last, options, {"PASS","WGHT","ZERO","NONE","ABOV"})) return false;
    options.residualsEnabled = false;
    options.cycleSlipsEnabled = true;
    options.maximumMeanDailyCycleSlips = 1000;
    if (!check(first, last, options, {"PASS","WGHT","ZERO","NONE","ABOV"})) return false;
    options.cycleSlipsEnabled = false;

    // Coverage now counts the intersection of file, metadata and enabled
    // quality dates. Missing QC dates stay in the denominator.
    options.minimumCompletenessPercent = 50;
    options.observationCompletenessEnabled = true;
    if (!check(first, last, options, {"PASS","DIPS","WGHT","MISS","ZERO","NONE","ABOV"})) return false;
    options.meanMultipathEnabled = true;
    if (!check(first, last, options, {"PASS","WGHT","MISS","ZERO","NONE"})) return false;
    const auto qualified = QueryObservationSearchIndex(build.outputPath,
        first, last, selection, options, false, {});
    for (const auto& station : qualified.stations) {
        const bool bothDays = station.fourCharId == "PASS";
        const bool firstDay = bothDays || station.fourCharId == "MISS";
        const bool lastDay = bothDays || station.fourCharId != "MISS";
        if (!station.calendarCoverage
            || station.availableDays != (bothDays ? 2 : 1)
            || station.expectedDays != 2
            || station.completenessPercent != (bothDays ? 100.0 : 50.0)
            || CalendarHasDay(station.calendarCoverage->available,
                              CalendarOrdinal(2025,1,1)) != firstDay
            || CalendarHasDay(station.calendarCoverage->available,
                              CalendarOrdinal(2025,1,2)) != lastDay) {
            std::cerr << "Qualified coverage retained an invalid quality date.\n";
            return false;
        }
    }
    // Recheck lazily hydrated rows so a relaxed date-coverage threshold cannot
    // reintroduce files from a day with unknown or failed quality.
    ObservationRangeOptions unfiltered;
    const auto allStations = QueryObservationSearchIndex(build.outputPath,
        first, last, selection, unfiltered, false, {});
    std::vector<DatedStationFile> manifest;
    for (const auto& station : allStations.stations) {
        for (const auto& date : {first, last}) {
            DatedStationFile file;
            file.date = date;
            file.station = station;
            manifest.push_back(std::move(file));
        }
    }
    const auto manifestCheck = FilterObservationSearchDatedFiles(
        build.outputPath, selection, options, manifest, false, {});
    if (!manifestCheck.complete || manifest.size() != 6) {
        std::cerr << "Daily quality was not applied to hydrated download files.\n";
        return false;
    }
    for (const auto& file : manifest) {
        const auto match = std::ranges::find_if(qualified.stations,
            [&](const StationFile& station) {
                return station.stationId == file.station.stationId;
            });
        if (match == qualified.stations.end()
            || !CalendarHasDay(match->calendarCoverage->available,
                CalendarOrdinal(file.date.year, file.date.month, file.date.day))) {
            std::cerr << "Download manifest disagrees with qualified dates.\n";
            return false;
        }
    }
    options.completenessMode = CompletenessMode::DaysAtLeast;
    options.minimumAvailableDays = 2;
    if (!check(first, last, options, {"PASS"})) return false;
    options.minimumAvailableDays = 1;
    if (!check(first, last, options, {"PASS","WGHT","MISS","ZERO","NONE"})) return false;
    options.completenessMode = CompletenessMode::RatioAtLeast;
    options.minimumCompletenessPercent = 100;
    options.meanMultipathEnabled = false;
    options.observationCompletenessEnabled = false;

    const auto future = AddDays(last, 1);
    options.onlineSnapshots = ObservationSearchOnlineSnapshots(build.outputPath, selection, future);
    if (options.onlineSnapshots.empty()) {
        std::cerr << "Daily quality fixture has no last-known-day snapshot.\n";
        return false;
    }
    if (!check(first, future, options, {"PASS","DIPS","WGHT","MISS","ZERO","NONE","ABOV"})) return false;
    // Last-known-day quality is never copied to fill a missing future day.
    for (int metric = 0; metric < 6; ++metric) {
        options.observationCompletenessEnabled = metric == 0;
        options.latencyEnabled = metric == 1;
        options.positionTimeSeriesEnabled = metric == 2;
        options.residualsEnabled = metric == 3;
        options.meanMultipathEnabled = metric == 4;
        options.cycleSlipsEnabled = metric == 5;
        if (!check(first, future, options, {})) return false;
    }
    if (sqlite3_open(source.string().c_str(), &database) != SQLITE_OK) {
        std::cerr << "Cannot reopen quality fixture.\n"; return false;
    }
    const bool systemFixture = Exec(database,
        "UPDATE daily_observation_quality SET "
        "observations_json='{\"GPS\":100,\"GPS Obs\":95,\"GLO\":100,\"GLO Obs\":20}',"
        "multipath_json='{\"GPS MP1\":0.5,\"GPS MP2\":0,\"GLO MP1\":10}',"
        "cycle_slips_json='{\"GPS\":5,\"GLO\":100,\"QZS\":0}' "
        "WHERE station_key='PASS00USA';");
    sqlite3_close(database);
    if (!systemFixture) { std::cerr << "Cannot update constellation fixture.\n"; return false; }
    std::cout << "Checking constellation-specific quality and download dates.\n";
    options = {};
    options.sampleInterval = "30S";
    options.rinexVersion = 3;
    options.completenessMode = CompletenessMode::RatioAtLeast;
    options.minimumCompletenessPercent = 100;
    options.restrictStationDates = true;
    options.stationDateWindows = {{first,last,{"PASS"}}};
    options.observationCompletenessEnabled = true;
    options.minimumObservationCompletenessPercent = 90;
    options.observationCompletenessSystem = "GPS";
    if (!check(first,last,options,{"PASS"})) return false;
    options.observationCompletenessSystem = "GLONASS";
    if (!check(first,last,options,{})) return false;
    options.observationCompletenessSystem.clear();
    if (!check(first,last,options,{})) return false;
    options.observationCompletenessSystem = "BDS";
    options.minimumObservationCompletenessPercent = 0;
    if (!check(first,last,options,{})) return false;
    options.observationCompletenessEnabled = false;
    options.meanMultipathEnabled = true;
    options.maximumMeanMultipathCentimeters = 1;
    options.multipathSystem = "GPS";
    if (!check(first,last,options,{"PASS"})) return false;
    options.multipathSystem = "GLO";
    if (!check(first,last,options,{})) return false;
    options.multipathSystem.clear();
    if (!check(first,last,options,{})) return false;
    options.multipathSystem = "GAL";
    if (!check(first,last,options,{})) return false;
    options.meanMultipathEnabled = false;
    options.cycleSlipsEnabled = true;
    options.maximumMeanDailyCycleSlips = 10;
    options.cycleSlipsSystem = "GPS";
    if (!check(first,last,options,{"PASS"})) return false;
    auto systemFiles = manifest;
    std::erase_if(systemFiles, [](const DatedStationFile& file) {
        return file.station.fourCharId != "PASS";
    });
    const auto systemManifest = FilterObservationSearchDatedFiles(
        build.outputPath,selection,options,systemFiles,false,{});
    if (!systemManifest.complete || systemFiles.size() != 2) {
        std::wcerr << L"Constellation manifest failed: " << systemManifest.error << L'\n';
        std::cerr << "Retained " << systemFiles.size() << " files; expected 2.\n";
        return false;
    }
    options.cycleSlipsSystem = "GLO";
    if (!check(first,last,options,{})) return false;
    const auto rejectedSystemManifest = FilterObservationSearchDatedFiles(
        build.outputPath,selection,options,systemFiles,false,{});
    if (!rejectedSystemManifest.complete || !systemFiles.empty()) return false;
    options.cycleSlipsSystem.clear();
    if (!check(first,last,options,{})) return false;
    options.cycleSlipsSystem = "QZSS";
    if (!check(first,last,options,{"PASS"})) return false;
    options.cycleSlipsSystem = "GAL";
    if (!check(first,last,options,{})) return false;
    options.cycleSlipsEnabled = false;
    options.latencyEnabled = true;
    options.maximumMeanLatencyHours = 10;
    if (!check(first,last,options,{"PASS"})) return false;
    std::cout << "Daily quality and constellation filters: thresholds, missing values, aliases, coverage and download dates passed.\n";
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && (std::string(argv[1]) == "--online-tail-smoke"
        || std::string(argv[1]) == "--online-highrate-smoke")) {
        const auto day = MakeQueryDate(2026, 8, 2);
        NetworkSelection selection{};
        selection.mask = NetworkBit(argc > 2 && std::string(argv[2]) == "MGEX"
            ? ObservationNetwork::Mgex : ObservationNetwork::Igs);
        ObservationRangeOptions filter;
        filter.sampleInterval = std::string(argv[1]) == "--online-highrate-smoke" ? "01S" : "30S";
        filter.onlineSnapshots = ObservationSearchOnlineSnapshots(
            DefaultObservationSearchIndexPath(), selection, day);
        const auto query = QueryObservationSearchIndex(DefaultObservationSearchIndexPath(),
            day, day, selection, filter, false);
        if (!query.complete || query.stations.empty()) {
            std::wcerr << L"Snapshot query failed: " << query.error << L"\n";
            return 1;
        }
        auto found = std::find_if(query.stations.begin(), query.stations.end(),
            [](const StationFile& station) { return station.fourCharId == "ZIM2"; });
        if (found == query.stations.end()) found = query.stations.begin();
        std::cout << "Future candidates=" << query.stations.size() << ", selected="
                  << found->stationId << ", pending=" << found->unverifiedDays << std::endl;
        auto files = LoadObservationDateRangeFilesForStations(day, day, selection, filter,
            std::span<const StationFile>(&*found, 1), {},
            [](const ObservationManifestProgress& progress) {
                std::wcout << progress.currentSource << std::endl;
            });
        if (!files.error.empty() || files.periodFiles.empty()) {
            std::wcerr << L"Online manifest failed: " << files.error << L" / " << files.warning << L"\n";
            return 1;
        }
        const auto& file = files.periodFiles.front().station;
        std::wcout << L"Verified route=" << file.downloadUrl << std::endl;
        const auto bytes = HttpGetByteRange(file.downloadUrl, L"0-1023", {}, 2U * 1024U * 1024U);
        std::cout << "Files=" << files.periodFiles.size() << ", RINEX="
                  << (file.format == RinexFormat::Rinex34 ? 3 : 2)
                  << ", data HTTP=" << bytes.statusCode << ", bytes=" << bytes.body.size() << std::endl;
        if (!bytes.ok()) { std::wcerr << bytes.error << std::endl; return 1; }
        const auto destination = std::filesystem::path("tmp/observation-online-tail-smoke") / file.fileName;
        std::filesystem::create_directories(destination.parent_path());
        const auto downloaded = HttpDownloadFile(file.downloadUrl, destination, {}, {}, false, nullptr, 120, 2);
        std::cout << "Full download=" << downloaded.success << ", bytes=" << downloaded.bytesWritten
                  << ", file=" << file.fileName << std::endl;
        if (!downloaded.success) std::wcerr << downloaded.error << std::endl;
        return downloaded.success && downloaded.bytesWritten > 0 ? 0 : 1;
    }

    const auto suffix = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const auto temporaryRoot = std::filesystem::weakly_canonical(
        std::filesystem::temp_directory_path());
    const auto directory = std::filesystem::weakly_canonical(temporaryRoot
        / ("gnss-observation-identity-test-" + std::to_string(suffix)));
    if (directory.parent_path() != temporaryRoot) {
        std::cerr << "Test directory escaped the intended temporary root.\n";
        return 1;
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error) {
        std::cerr << "Cannot create test directory: " << directory << '\n';
        return 1;
    }
    if (!CheckWeatherPreview(directory)) {
        std::cerr << "Space-weather preview regression failed.\n";
        return 1;
    }

    const auto four = [](const char* code) {
        return std::string("FOUR:") + code;
    };
    const auto okakNgs = directory / "ngs_availability.sqlite";
    const auto okakSopac = directory / "sopac_garner_availability.sqlite";
    const auto starGa = directory / "ga_gdc_availability.sqlite";
    const auto starPanga = directory / "panga_cwu_availability.sqlite";

    // Build the SOPAC source once with both its OKAK and STAR rows.
    const bool created = CreateSource(okakNgs, ObservationNetwork::Ngs, {
            Row{four("OKAK"), "OKAK", "OKAK", "USA", 35.0713, -98.2154,
                true, 2, "r2"},
            Row{four("OKAK"), "OKAK", "OKAK", "USA", 35.0713, -98.2154,
                true, 3, "r3", "01S"}})
        && CreateSource(okakSopac, ObservationNetwork::SopacGarner, {
            Row{four("OKAK"), "OKAK", "OKAK", "USA", 53.4112, -168.3599},
            Row{four("STAR"), "STAR", "STAR", "USA", 46.8508, -121.7939}})
        && CreateSource(starGa, ObservationNetwork::GaGdc, {
            Row{four("STAR"), "STAR", "STAR", "AUS", -36.6153, 143.2574}})
        && CreateSource(starPanga, ObservationNetwork::PangaCwu, {
            // Deliberately omit PANGA's coordinate: the central builder must
            // still merge this row with SOPAC STAR via the explicit alias,
            // while unrelated short-code pairs remain split.
            Row{four("STAR"), "STAR", "STAR", "USA", 0.0, 0.0, false}});
    if (!created) {
        std::filesystem::remove_all(directory, error);
        return 1;
    }

    ObservationSearchBuildOptions options;
    options.sourceDirectory = directory;
    options.outputPath = directory / "observation_search.sqlite";
    options.networkMask = NetworkBit(ObservationNetwork::Ngs)
        | NetworkBit(ObservationNetwork::GaGdc)
        | NetworkBit(ObservationNetwork::SopacGarner)
        | NetworkBit(ObservationNetwork::PangaCwu);
    const auto build = BuildObservationSearchIndex(options);
    if (!build.complete) {
        std::wcerr << L"Identity regression build failed: " << build.error << '\n';
        std::filesystem::remove_all(directory, error);
        return 1;
    }

    // Four physical stations are expected: OKAK/NGS, OKAK/SOPAC,
    // STAR/GA-GDC, and STAR/SOPAC+PANGA.  In particular, PANGA STAR must
    // merge with SOPAC STAR while GA STAR remains separate.
    const bool stationCountOkay = build.unifiedStations == 4;
    if (!stationCountOkay) {
        std::cerr << "Expected 4 unified stations, got "
                  << build.unifiedStations << '\n';
        std::filesystem::remove_all(directory, error);
        return 1;
    }
    const bool queriesOkay =
        CheckNetworkQuery(
            options.outputPath, ObservationNetwork::Ngs, 1,
            "OKAK", 35.0713, -98.2154,
            static_cast<RinexFormatMask>(
                RinexFormatBit(RinexFormat::Rinex2)
                | RinexFormatBit(RinexFormat::Rinex34)))
        && CheckNetworkQuery(
            options.outputPath, ObservationNetwork::GaGdc, 1,
            "STAR", -36.6153, 143.2574)
        && CheckNetworkQuery(
            options.outputPath, ObservationNetwork::PangaCwu, 1,
            "STAR", 46.8508, -121.7939)
        && CheckRinexFilter(
            options.outputPath, 2, RinexFormatBit(RinexFormat::Rinex2))
        && CheckRinexFilter(
            options.outputPath, 3, RinexFormatBit(RinexFormat::Rinex34));

    const bool intervalAliasesOkay = queriesOkay
        && CheckIntervalAliases(options.outputPath);

    const bool hydrationOkay = intervalAliasesOkay
        && CheckDeferredHydration(directory);

    sqlite3* database = nullptr;
    bool masksOkay = false;
    if (queriesOkay
        && sqlite3_open_v2(
            options.outputPath.string().c_str(), &database,
            SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(
                database,
                "SELECT four_char_id,available_networks FROM stations "
                "ORDER BY station_uid;",
                -1, &statement, nullptr) == SQLITE_OK) {
            int okakMasks = 0;
            int starMasks = 0;
            while (sqlite3_step(statement) == SQLITE_ROW) {
                const std::string code(reinterpret_cast<const char*>(
                    sqlite3_column_text(statement, 0)));
                const auto mask = static_cast<ObservationNetworkMask>(
                    sqlite3_column_int64(statement, 1));
                if (code == "OKAK") okakMasks |= (mask == 4 ? 1 : 2);
                if (code == "STAR") {
                    if ((mask & NetworkBit(ObservationNetwork::GaGdc)) != 0) {
                        starMasks |= 1;
                    }
                    if ((mask & NetworkBit(ObservationNetwork::SopacGarner)) != 0
                        && (mask & NetworkBit(ObservationNetwork::PangaCwu)) != 0) {
                        starMasks |= 2;
                    }
                }
            }
            masksOkay = okakMasks == 3 && starMasks == 3;
        }
        sqlite3_finalize(statement);
    }
    if (database != nullptr) sqlite3_close(database);

    const auto windowDirectory = directory / "station-window-test";
    std::filesystem::create_directories(windowDirectory, error);
    const bool stationWindowsOkay =
        error ? false : CheckStationDateWindows(windowDirectory);
    // A tail date is a candidate, not a fabricated daily-file fact.
    ObservationRangeOptions tailOptions;
    tailOptions.sampleInterval = "30S";
    tailOptions.completenessMode = CompletenessMode::RatioAtLeast;
    tailOptions.minimumCompletenessPercent = 100;
    NetworkSelection tailSelection{};
    tailSelection.mask = NetworkBit(ObservationNetwork::Ngs);
    const auto knownDay = MakeQueryDate(2025, 1, 1);
    const auto futureDay = MakeQueryDate(2025, 1, 3);
    tailOptions.onlineSnapshots = ObservationSearchOnlineSnapshots(
        options.outputPath, tailSelection, futureDay);
    const auto tail = QueryObservationSearchIndex(options.outputPath,
        knownDay, futureDay, tailSelection, tailOptions, false);
    const auto onlyTail = QueryObservationSearchIndex(options.outputPath,
        AddDays(knownDay, 1), futureDay, tailSelection, tailOptions, false);
    const auto historical = QueryObservationSearchIndex(options.outputPath,
        knownDay, knownDay, tailSelection, tailOptions, false);
    auto strictOptions = tailOptions;
    strictOptions.onlineSnapshots.clear();
    const auto strictTail = QueryObservationSearchIndex(options.outputPath,
        knownDay, futureDay, tailSelection, strictOptions, false);
    const auto historicalHole = QueryObservationSearchIndex(options.outputPath,
        AddDays(knownDay, -1), futureDay, tailSelection, tailOptions, false);
    auto dailyOptions = tailOptions;
    dailyOptions.dailyCoverageMode = DailyCoverageMode::StationsAtLeast;
    dailyOptions.minimumDailyStations = 1;
    const auto dailyTail = QueryObservationSearchIndex(options.outputPath,
        knownDay, futureDay, tailSelection, dailyOptions, false);
    dailyOptions.minimumDailyStations = 2;
    const auto insufficientDaily = QueryObservationSearchIndex(options.outputPath,
        knownDay, futureDay, tailSelection, dailyOptions, false);
    tailOptions.rinexVersion = 3; // NGS fixture has R3 only at 1S, not 30S.
    const auto wrongInterval = QueryObservationSearchIndex(options.outputPath,
        knownDay, futureDay, tailSelection, tailOptions, false);
    const bool tailOkay = tail.complete && tail.stations.size() == 1
        && tail.stations[0].availableDays == 3 && tail.stations[0].unverifiedDays == 2
        && onlyTail.complete && onlyTail.stations.size() == 1
        && onlyTail.stations[0].unverifiedDays == 2
        && historical.complete && historical.stations.size() == 1
        && historical.stations[0].unverifiedDays == 0
        && strictTail.complete && strictTail.stations.empty()
        && wrongInterval.complete && wrongInterval.stations.empty()
        && historicalHole.complete && historicalHole.stations.empty()
        && dailyTail.complete && dailyTail.stations.size() == 1
        && dailyTail.stations[0].unverifiedDays == 2
        && insufficientDaily.complete && insufficientDaily.stations.empty();
    if (!tailOkay) std::cerr << "Online tail snapshot regression failed.\n";

    const auto cappedPath = directory / "capped-observation.sqlite";
    std::filesystem::copy_file(options.outputPath, cappedPath, error);
    sqlite3* cappedDb = nullptr;
    bool cutoffOkay = !error && sqlite3_open(cappedPath.string().c_str(), &cappedDb) == SQLITE_OK;
    if (cutoffOkay) cutoffOkay = Exec(cappedDb,
        "INSERT INTO metadata(key,value) VALUES('data_cutoff_date','2025-01-01');");
    if (cappedDb) sqlite3_close(cappedDb);
    if (cutoffOkay) {
        auto cappedOptions = tailOptions;
        cappedOptions.rinexVersion = 0;
        cappedOptions.completenessMode = CompletenessMode::Ignore;
        const auto cappedKnown = QueryObservationSearchIndex(cappedPath,
            knownDay, knownDay, tailSelection, cappedOptions, false);
        const auto cappedFuture = QueryObservationSearchIndex(cappedPath,
            AddDays(knownDay, 1), futureDay, tailSelection, cappedOptions, false);
        cutoffOkay = cappedKnown.complete && cappedKnown.stations.size() == 1
            && cappedKnown.stations[0].availableDays == 1
            && cappedKnown.stations[0].unverifiedDays == 0
            && cappedFuture.complete && cappedFuture.stations.empty()
            && ObservationSearchOnlineSnapshots(cappedPath, tailSelection, futureDay).empty();
    }
    if (!cutoffOkay) std::cerr << "Published data cutoff regression failed.\n";

    const bool igsHighRateOkay = CheckIgsHighRateImport(directory);
    const bool dailyQualityOkay = CheckDailyObservationCompleteness(directory / "daily-quality-test");
    std::filesystem::remove_all(directory, error);
    if (!queriesOkay || !intervalAliasesOkay || !masksOkay
        || !hydrationOkay || !stationWindowsOkay || !igsHighRateOkay || !tailOkay
        || !dailyQualityOkay || !cutoffOkay) {
        std::cerr << "Identity regression checks failed.\n";
        return 1;
    }
    std::cout << "Observation identity collision regression passed.\n";
    return 0;
}
