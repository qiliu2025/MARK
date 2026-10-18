#include "NetworkAvailabilityDb.h"

#include "IgsAvailabilityDb.h"

#include "sqlite3.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace {

std::mutex gDatabaseMutex;

struct DatabaseCloser {
    void operator()(sqlite3* database) const {
        if (database != nullptr) sqlite3_close(database);
    }
};
struct StatementCloser {
    void operator()(sqlite3_stmt* statement) const {
        if (statement != nullptr) sqlite3_finalize(statement);
    }
};
using DatabasePtr = std::unique_ptr<sqlite3, DatabaseCloser>;
using StatementPtr = std::unique_ptr<sqlite3_stmt, StatementCloser>;

std::string WideToUtf8(const std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::filesystem::path EnvironmentPath(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return {};
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0 || written >= required) return {};
    value.resize(written);
    return std::filesystem::path(value);
}

std::string DateText(const QueryDate& date) {
    return std::format("{:04}-{:02}-{:02}", date.year, date.month, date.day);
}

bool ToValidatedSysDays(
    const QueryDate& date,
    std::chrono::sys_days& days) {
    const std::chrono::year_month_day calendarDate{
        std::chrono::year{date.year}
        / std::chrono::month{static_cast<unsigned>(date.month)}
        / std::chrono::day{static_cast<unsigned>(date.day)}};
    if (!calendarDate.ok()) return false;
    days = std::chrono::sys_days{calendarDate};
    return true;
}

QueryDate QueryDateFromSysDays(const std::chrono::sys_days days) {
    const std::chrono::year_month_day calendarDate{days};
    QueryDate result;
    result.year = static_cast<int>(calendarDate.year());
    result.month = static_cast<int>(
        static_cast<unsigned>(calendarDate.month()));
    result.day = static_cast<int>(
        static_cast<unsigned>(calendarDate.day()));
    const auto yearStart = std::chrono::sys_days{
        calendarDate.year() / std::chrono::month{1} / std::chrono::day{1}};
    result.dayOfYear = static_cast<int>((days - yearStart).count()) + 1;
    return result;
}

std::string StationKey(const StationFile& station) {
    return station.stationId.size() == 9
        ? station.stationId
        : std::string("FOUR:") + station.fourCharId;
}

std::string FileKey(
    const StationFile& station,
    const bool includeDownloadRoute = false) {
    std::string value = StationKey(station) + "\x1f" + station.fileName;
    if (includeDownloadRoute) {
        value += "\x1f" + WideToUtf8(station.downloadUrl);
    }
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

int PreferenceScore(const StationFile& station) {
    int score = station.format == RinexFormat::Rinex34 ? 100 : 10;
    if (station.interval == "30S") score += 20;
    else if (station.interval == "15S") score += 15;
    else if (station.interval == "01S") score += 10;
    if (station.fileName.ends_with(".crx.gz")
        || station.fileName.ends_with(".d.gz")) score += 2;
    return score;
}

std::wstring CommonBaseUrl(const std::vector<StationFile>& files) {
    if (files.empty()) return {};
    std::wstring common = files.front().downloadUrl;
    for (const auto& file : files) {
        const std::size_t limit = std::min(common.size(), file.downloadUrl.size());
        std::size_t matched = 0;
        while (matched < limit && common[matched] == file.downloadUrl[matched]) {
            ++matched;
        }
        common.resize(matched);
        if (common.empty()) return {};
    }
    const auto slash = common.find_last_of(L'/');
    return slash == std::wstring::npos ? std::wstring{} : common.substr(0, slash + 1);
}

std::wstring SqliteError(sqlite3* database, const std::wstring_view action) {
    const char* message = database == nullptr ? "unknown" : sqlite3_errmsg(database);
    return std::wstring(action) + L": " + Utf8ToWide(message == nullptr ? "" : message);
}

bool Execute(sqlite3* database, const char* sql, std::wstring& error) {
    char* message = nullptr;
    const int status = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    if (status == SQLITE_OK) return true;
    error = L"SQLite execution failed: "
        + Utf8ToWide(message == nullptr ? "" : message);
    sqlite3_free(message);
    return false;
}

StatementPtr Prepare(sqlite3* database, const char* sql, std::wstring& error) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK) {
        error = SqliteError(database, L"Unable to prepare knowledge-base query");
        return {};
    }
    return StatementPtr(raw);
}

bool BindText(sqlite3_stmt* statement, const int index, const std::string_view text) {
    return sqlite3_bind_text(
        statement, index, text.data(), static_cast<int>(text.size()),
        SQLITE_TRANSIENT) == SQLITE_OK;
}

std::string ColumnText(sqlite3_stmt* statement, const int index) {
    const auto* value = sqlite3_column_text(statement, index);
    return value == nullptr ? std::string{} : reinterpret_cast<const char*>(value);
}

bool OpenDatabase(
    const std::filesystem::path& path,
    const bool create,
    DatabasePtr& database,
    std::wstring& error) {
    std::error_code filesystemError;
    if (!create && !std::filesystem::is_regular_file(path, filesystemError)) {
        return false;
    }
    if (create) {
        std::filesystem::create_directories(path.parent_path(), filesystemError);
        if (filesystemError) {
            error = L"Unable to create knowledge-base directory: "
                + path.parent_path().wstring();
            return false;
        }
    }
    sqlite3* raw = nullptr;
    const int flags = create
        ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX
        : SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX;
    const std::string utf8Path = WideToUtf8(path.wstring());
    if (sqlite3_open_v2(utf8Path.c_str(), &raw, flags, nullptr) != SQLITE_OK
        || raw == nullptr) {
        error = SqliteError(raw, L"Unable to open knowledge-base database");
        if (raw != nullptr) sqlite3_close(raw);
        return false;
    }
    database.reset(raw);
    sqlite3_busy_timeout(raw, 10000);
    if (!create) {
        // Range queries can walk millions of local rows.  The stock SQLite
        // cache is only a few megabytes, so a read-only connection otherwise
        // spends most of its time re-reading index pages and spilling ORDER
        // BY temporaries to disk.  Keep the tuning best-effort and scoped to
        // this connection; no database file or journal is modified.
        sqlite3_exec(
            raw,
            "PRAGMA query_only=ON;"
            "PRAGMA temp_store=MEMORY;"
            "PRAGMA cache_size=-65536;"
            "PRAGMA mmap_size=268435456;",
            nullptr,
            nullptr,
            nullptr);
    }
    return true;
}

bool EnsureSchema(
    sqlite3* database,
    const ObservationNetwork network,
    std::wstring& error) {
    const char* schema =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS metadata("
        " key TEXT PRIMARY KEY,value TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS daily_scans("
        " observation_date TEXT PRIMARY KEY,"
        " status TEXT NOT NULL CHECK(status IN ('complete','failed')),"
        " is_provisional INTEGER NOT NULL DEFAULT 0,"
        " station_count INTEGER NOT NULL DEFAULT 0,"
        " file_count INTEGER NOT NULL DEFAULT 0,"
        " source_url TEXT NOT NULL DEFAULT '',"
        " error_message TEXT NOT NULL DEFAULT '',"
        " scanned_at_utc TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS daily_files("
        " observation_date TEXT NOT NULL,"
        " file_key TEXT NOT NULL,station_key TEXT NOT NULL,"
        " station_id TEXT NOT NULL,four_char_id TEXT NOT NULL,"
        " country TEXT NOT NULL DEFAULT '',file_name TEXT NOT NULL,"
        " url_override TEXT NOT NULL DEFAULT '',"
        " rinex_format INTEGER NOT NULL,sample_interval TEXT NOT NULL DEFAULT '',"
        " preference_score INTEGER NOT NULL DEFAULT 0,"
        " is_preferred INTEGER NOT NULL DEFAULT 0,"
        " available_networks INTEGER NOT NULL,has_coordinate INTEGER NOT NULL DEFAULT 0,"
        " latitude REAL,longitude REAL,"
        " PRIMARY KEY(observation_date,file_key),"
        " FOREIGN KEY(observation_date) REFERENCES daily_scans(observation_date)"
        " ON DELETE CASCADE) WITHOUT ROWID;"
        "CREATE INDEX IF NOT EXISTS daily_files_preferred"
        " ON daily_files(observation_date,is_preferred,station_key);";
    if (!Execute(database, schema, error)) return false;
    bool hasProvisional = false;
    {
        auto columns = Prepare(database, "PRAGMA table_info(daily_scans);", error);
        if (!columns) return false;
        while (sqlite3_step(columns.get()) == SQLITE_ROW) {
            if (ColumnText(columns.get(), 1) == "is_provisional") {
                hasProvisional = true;
                break;
            }
        }
    }
    if (!hasProvisional
        && !Execute(database,
            "ALTER TABLE daily_scans ADD COLUMN is_provisional INTEGER NOT NULL DEFAULT 0;",
            error)) return false;
    auto statement = Prepare(
        database,
        "INSERT INTO metadata(key,value) VALUES(?1,?2)"
        " ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
        error);
    if (!statement) return false;
    const std::string networkKey = KnowledgeBaseNetworkKey(network);
    if (!BindText(statement.get(), 1, "network")
        || !BindText(statement.get(), 2, networkKey)
        || sqlite3_step(statement.get()) != SQLITE_DONE) return false;
    sqlite3_reset(statement.get());
    sqlite3_clear_bindings(statement.get());
    return BindText(statement.get(), 1, "schema_version")
        && BindText(statement.get(), 2, "2")
        && sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool StoreScan(
    const QueryDate& date,
    const ObservationNetwork network,
    const std::vector<StationFile>* preferredStations,
    const std::vector<StationFile>* allFiles,
    const std::wstring& failure,
    std::wstring& error) {
    DatabasePtr database;
    const auto path = GetNetworkAvailabilityDatabasePath(network);
    if (!OpenDatabase(path, true, database, error)
        || !EnsureSchema(database.get(), network, error)
        || !Execute(database.get(), "BEGIN IMMEDIATE;", error)) return false;
    bool committed = false;
    const auto rollback = [&]() {
        if (!committed) {
            std::wstring ignored;
            Execute(database.get(), "ROLLBACK;", ignored);
        }
    };
    const std::string dateText = DateText(date);
    const bool success = preferredStations != nullptr;
    if (!success) {
        auto existing = Prepare(
            database.get(),
            "SELECT status FROM daily_scans WHERE observation_date=?1;",
            error);
        if (!existing || !BindText(existing.get(), 1, dateText)) {
            rollback();
            return false;
        }
        if (sqlite3_step(existing.get()) == SQLITE_ROW
            && ColumnText(existing.get(), 0) == "complete") {
            if (!Execute(database.get(), "ROLLBACK;", error)) return false;
            committed = true;
            return true;
        }
    }
    auto remove = Prepare(
        database.get(), "DELETE FROM daily_scans WHERE observation_date=?1;", error);
    if (!remove || !BindText(remove.get(), 1, dateText)
        || sqlite3_step(remove.get()) != SQLITE_DONE) {
        rollback(); return false;
    }

    const auto observationDay = std::chrono::sys_days{
        std::chrono::year{date.year}
        / std::chrono::month{static_cast<unsigned>(date.month)}
        / std::chrono::day{static_cast<unsigned>(date.day)}};
    const auto today = std::chrono::floor<std::chrono::days>(
        std::chrono::system_clock::now());
    const bool provisional = success
        && observationDay >= today - std::chrono::days{6};
    std::vector<StationFile> files = success && allFiles != nullptr && !allFiles->empty()
        ? *allFiles : (success ? *preferredStations : std::vector<StationFile>{});
    const bool keepEveryDownloadRoute =
        network == ObservationNetwork::EposGnss
        || network == ObservationNetwork::OsNet
        || network == ObservationNetwork::Ergnss
        || network == ObservationNetwork::Renep
        || network == ObservationNetwork::Cacs
        || network == ObservationNetwork::GeoNetNz
        || network == ObservationNetwork::SopacGarner
        || network == ObservationNetwork::Ring
        || network == ObservationNetwork::Apos
        || network == ObservationNetwork::RobBelgium
        || network == ObservationNetwork::Dpga
        || network == ObservationNetwork::CatNet
        || network == ObservationNetwork::SpsLux
        || network == ObservationNetwork::KasiNgii
        || network == ObservationNetwork::Renag
        || network == ObservationNetwork::Nsgi
        || network == ObservationNetwork::NoaGnss
        || network == ObservationNetwork::Redgae
        || network == ObservationNetwork::GeoNetJapan
        || network == ObservationNetwork::Dlab
        || network == ObservationNetwork::Magnet
        || network == ObservationNetwork::PangaCwu;
    if (success) {
        std::unordered_set<std::string> keys;
        keys.reserve(files.size());
        std::vector<StationFile> uniqueFiles;
        uniqueFiles.reserve(files.size());
        for (auto& file : files) {
            if (keys.insert(FileKey(file, keepEveryDownloadRoute)).second) {
                uniqueFiles.push_back(std::move(file));
            }
        }
        for (const auto& station : *preferredStations) {
            if (keys.insert(FileKey(station, keepEveryDownloadRoute)).second) {
                uniqueFiles.push_back(station);
            }
        }
        files = std::move(uniqueFiles);
    }
    const std::wstring baseUrl = CommonBaseUrl(files);
    auto scan = Prepare(
        database.get(),
        "INSERT INTO daily_scans(observation_date,status,is_provisional,station_count,file_count,"
        "source_url,error_message,scanned_at_utc) VALUES(?1,?2,?3,?4,?5,?6,?7,"
        "strftime('%Y-%m-%dT%H:%M:%SZ','now'));",
        error);
    if (!scan || !BindText(scan.get(), 1, dateText)
        || !BindText(scan.get(), 2, success ? "complete" : "failed")
        || sqlite3_bind_int(scan.get(), 3, provisional ? 1 : 0) != SQLITE_OK
        || sqlite3_bind_int64(scan.get(), 4,
            success ? static_cast<sqlite3_int64>(preferredStations->size()) : 0) != SQLITE_OK
        || sqlite3_bind_int64(scan.get(), 5,
            static_cast<sqlite3_int64>(files.size())) != SQLITE_OK
        || !BindText(scan.get(), 6, WideToUtf8(baseUrl))
        || !BindText(scan.get(), 7, WideToUtf8(failure))
        || sqlite3_step(scan.get()) != SQLITE_DONE) {
        rollback(); return false;
    }

    if (success) {
        std::unordered_set<std::string> preferredKeys;
        for (const auto& station : *preferredStations) {
            preferredKeys.insert(FileKey(station, keepEveryDownloadRoute));
        }
        auto insert = Prepare(
            database.get(),
            "INSERT OR REPLACE INTO daily_files("
            "observation_date,file_key,station_key,station_id,four_char_id,country,"
            "file_name,url_override,rinex_format,sample_interval,preference_score,"
            "is_preferred,available_networks,has_coordinate,latitude,longitude)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16);",
            error);
        if (!insert) { rollback(); return false; }
        for (const auto& file : files) {
            sqlite3_reset(insert.get());
            sqlite3_clear_bindings(insert.get());
            const std::string fileKey = FileKey(file, keepEveryDownloadRoute);
            std::string overrideUrl;
            if (baseUrl.empty()
                || !file.downloadUrl.starts_with(baseUrl)) {
                overrideUrl = WideToUtf8(file.downloadUrl);
            } else {
                const std::wstring relative = file.downloadUrl.substr(baseUrl.size());
                if (relative != Utf8ToWide(file.fileName)) {
                    overrideUrl = WideToUtf8(relative);
                }
            }
            const bool bound = BindText(insert.get(), 1, dateText)
                && BindText(insert.get(), 2, fileKey)
                && BindText(insert.get(), 3, StationKey(file))
                && BindText(insert.get(), 4, file.stationId)
                && BindText(insert.get(), 5, file.fourCharId)
                && BindText(insert.get(), 6, file.country)
                && BindText(insert.get(), 7, file.fileName)
                && BindText(insert.get(), 8, overrideUrl)
                && sqlite3_bind_int(insert.get(), 9,
                    file.format == RinexFormat::Rinex2 ? 2 : 3) == SQLITE_OK
                && BindText(insert.get(), 10, file.interval)
                && sqlite3_bind_int(insert.get(), 11, PreferenceScore(file)) == SQLITE_OK
                && sqlite3_bind_int(insert.get(), 12,
                    preferredKeys.contains(fileKey) ? 1 : 0) == SQLITE_OK
                && sqlite3_bind_int64(insert.get(), 13,
                    static_cast<sqlite3_int64>(file.availableNetworks)) == SQLITE_OK
                && sqlite3_bind_int(insert.get(), 14, file.hasCoordinate ? 1 : 0) == SQLITE_OK
                && (file.hasCoordinate
                    ? sqlite3_bind_double(insert.get(), 15, file.latitude) == SQLITE_OK
                        && sqlite3_bind_double(insert.get(), 16, file.longitude) == SQLITE_OK
                    : sqlite3_bind_null(insert.get(), 15) == SQLITE_OK
                        && sqlite3_bind_null(insert.get(), 16) == SQLITE_OK);
            if (!bound || sqlite3_step(insert.get()) != SQLITE_DONE) {
                error = SqliteError(database.get(), L"Unable to store daily station file");
                rollback(); return false;
            }
        }
    }
    if (!Execute(database.get(), "COMMIT;", error)) {
        rollback(); return false;
    }
    committed = true;
    return true;
}

}  // namespace

bool HasSingleSourceKnowledgeBase(const ObservationNetwork network) {
    return network == ObservationNetwork::PanamaCors
        || network == ObservationNetwork::Cut
        || network == ObservationNetwork::Ngs
        || network == ObservationNetwork::Epn
        || network == ObservationNetwork::GaGdc
        || network == ObservationNetwork::Rgp
        || network == ObservationNetwork::Sonel
        || network == ObservationNetwork::Hk
        || network == ObservationNetwork::TrigNet
        || network == ObservationNetwork::Rgna
        || network == ObservationNetwork::Csn
        || network == ObservationNetwork::EposIceland
        || network == ObservationNetwork::Rbmc
        || network == ObservationNetwork::Ramsac
        || network == ObservationNetwork::RegnaRou
        || network == ObservationNetwork::MagnaEco
        || network == ObservationNetwork::EposGnss
        || network == ObservationNetwork::Gref
        || network == ObservationNetwork::OsNet
        || network == ObservationNetwork::Ergnss
        || network == ObservationNetwork::Renep
        || network == ObservationNetwork::Cacs
        || network == ObservationNetwork::GeoNetNz
        || network == ObservationNetwork::SopacGarner
        || network == ObservationNetwork::Ring
        || network == ObservationNetwork::Apos
        || network == ObservationNetwork::RobBelgium
        || network == ObservationNetwork::Dpga
        || network == ObservationNetwork::CatNet
        || network == ObservationNetwork::SpsLux
        || network == ObservationNetwork::KasiNgii
        || network == ObservationNetwork::Renag
        || network == ObservationNetwork::Nsgi
        || network == ObservationNetwork::NoaGnss
        || network == ObservationNetwork::Redgae
        || network == ObservationNetwork::GeoNetJapan
        || network == ObservationNetwork::Dlab
        || network == ObservationNetwork::Magnet
        || network == ObservationNetwork::PangaCwu;
}

std::string KnowledgeBaseNetworkKey(const ObservationNetwork network) {
    switch (network) {
        case ObservationNetwork::PanamaCors: return "PAN-CORS";
        case ObservationNetwork::Cut: return "CUT";
        case ObservationNetwork::Ngs: return "NGS";
        case ObservationNetwork::Epn: return "EPN";
        case ObservationNetwork::GaGdc: return "GA-GDC";
        case ObservationNetwork::Rgp: return "RGP";
        case ObservationNetwork::Sonel: return "SONEL";
        case ObservationNetwork::Hk: return "HK";
        case ObservationNetwork::TrigNet: return "TRIGNET";
        case ObservationNetwork::Rgna: return "RGNA";
        case ObservationNetwork::Csn: return "CSN";
        case ObservationNetwork::EposIceland: return "EPOS-IS";
        case ObservationNetwork::Rbmc: return "IBGE-RBMC";
        case ObservationNetwork::Ramsac: return "IGN-RAMSAC";
        case ObservationNetwork::RegnaRou: return "REGNA-ROU";
        case ObservationNetwork::MagnaEco: return "MAGNA-ECO";
        case ObservationNetwork::EposGnss: return "EPOS-GNSS";
        case ObservationNetwork::Gref: return "GREF";
        case ObservationNetwork::OsNet: return "OS-NET";
        case ObservationNetwork::Ergnss: return "ERGNSS";
        case ObservationNetwork::Renep: return "RENEP";
        case ObservationNetwork::Cacs: return "CACS-NRCAN";
        case ObservationNetwork::GeoNetNz: return "GEONET-NZ";
        case ObservationNetwork::SopacGarner: return "SOPAC-GARNER";
        case ObservationNetwork::Ring: return "RING";
        case ObservationNetwork::Apos: return "APOS";
        case ObservationNetwork::RobBelgium: return "ROB-BELGIUM";
        case ObservationNetwork::Dpga: return "DPGA";
        case ObservationNetwork::CatNet: return "CATNET";
        case ObservationNetwork::SpsLux: return "SPSLUX";
        case ObservationNetwork::KasiNgii: return "KASI-NGII";
        case ObservationNetwork::Renag: return "RENAG";
        case ObservationNetwork::Nsgi: return "NSGI";
        case ObservationNetwork::NoaGnss: return "NOA-GNSS";
        case ObservationNetwork::Redgae: return "REDGAE";
        case ObservationNetwork::GeoNetJapan: return "GEONET-JP";
        case ObservationNetwork::Dlab: return "DLAB";
        case ObservationNetwork::Magnet: return "MAGNET";
        case ObservationNetwork::PangaCwu: return "PANGA-CWU";
        default: return {};
    }
}

std::wstring KnowledgeBaseNetworkLabel(const ObservationNetwork network) {
    const auto key = KnowledgeBaseNetworkKey(network);
    return std::wstring(key.begin(), key.end());
}

std::filesystem::path GetNetworkAvailabilityDatabasePath(
    const ObservationNetwork network) {
    auto directory = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
    if (directory.empty()) {
        directory = GetIgsAvailabilityDatabasePath().parent_path();
    }
    switch (network) {
        case ObservationNetwork::PanamaCors: return ResolveDatabaseFilePath(directory / L"pan_cors_availability.sqlite");
        case ObservationNetwork::Cut: return ResolveDatabaseFilePath(directory / L"cut_availability.sqlite");
        case ObservationNetwork::Ngs: return ResolveDatabaseFilePath(directory / L"ngs_availability.sqlite");
        case ObservationNetwork::Epn: return ResolveDatabaseFilePath(directory / L"epn_availability.sqlite");
        case ObservationNetwork::GaGdc: return ResolveDatabaseFilePath(directory / L"ga_gdc_availability.sqlite");
        case ObservationNetwork::Rgp: return ResolveDatabaseFilePath(directory / L"rgp_availability.sqlite");
        case ObservationNetwork::Sonel: return ResolveDatabaseFilePath(directory / L"sonel_availability.sqlite");
        case ObservationNetwork::Hk: return ResolveDatabaseFilePath(directory / L"hk_availability.sqlite");
        case ObservationNetwork::TrigNet:
            return ResolveDatabaseFilePath(directory / L"trignet_availability.sqlite");
        case ObservationNetwork::Rgna: return ResolveDatabaseFilePath(directory / L"rgna_availability.sqlite");
        case ObservationNetwork::Csn: return ResolveDatabaseFilePath(directory / L"csn_availability.sqlite");
        case ObservationNetwork::EposIceland: return ResolveDatabaseFilePath(directory / L"epos_is_availability.sqlite");
        case ObservationNetwork::Rbmc: return ResolveDatabaseFilePath(directory / L"ibge_rbmc_availability.sqlite");
        case ObservationNetwork::Ramsac: return ResolveDatabaseFilePath(directory / L"ign_ramsac_availability.sqlite");
        case ObservationNetwork::RegnaRou: return ResolveDatabaseFilePath(directory / L"regna_rou_availability.sqlite");
        case ObservationNetwork::MagnaEco: return ResolveDatabaseFilePath(directory / L"magna_eco_availability.sqlite");
        case ObservationNetwork::EposGnss: return ResolveDatabaseFilePath(directory / L"epos_gnss_availability.sqlite");
        case ObservationNetwork::Gref: return ResolveDatabaseFilePath(directory / L"gref_availability.sqlite");
        case ObservationNetwork::OsNet: return ResolveDatabaseFilePath(directory / L"os_net_availability.sqlite");
        case ObservationNetwork::Ergnss: return ResolveDatabaseFilePath(directory / L"ergnss_availability.sqlite");
        case ObservationNetwork::Renep: return ResolveDatabaseFilePath(directory / L"renep_availability.sqlite");
        case ObservationNetwork::Cacs: return ResolveDatabaseFilePath(directory / L"cacs_nrcan_availability.sqlite");
        case ObservationNetwork::GeoNetNz: return ResolveDatabaseFilePath(directory / L"geonet_nz_availability.sqlite");
        case ObservationNetwork::SopacGarner: return ResolveDatabaseFilePath(directory / L"sopac_garner_availability.sqlite");
        case ObservationNetwork::Ring: return ResolveDatabaseFilePath(directory / L"ring_availability.sqlite");
        case ObservationNetwork::Apos: return ResolveDatabaseFilePath(directory / L"apos_availability.sqlite");
        case ObservationNetwork::RobBelgium: return ResolveDatabaseFilePath(directory / L"rob_belgium_availability.sqlite");
        case ObservationNetwork::Dpga: return ResolveDatabaseFilePath(directory / L"dpga_availability.sqlite");
        case ObservationNetwork::CatNet: return ResolveDatabaseFilePath(directory / L"catnet_availability.sqlite");
        case ObservationNetwork::SpsLux: return ResolveDatabaseFilePath(directory / L"spslux_availability.sqlite");
        case ObservationNetwork::KasiNgii: return ResolveDatabaseFilePath(directory / L"kasi_ngii_availability.sqlite");
        case ObservationNetwork::Renag: return ResolveDatabaseFilePath(directory / L"renag_availability.sqlite");
        case ObservationNetwork::Nsgi: return ResolveDatabaseFilePath(directory / L"nsgi_availability.sqlite");
        case ObservationNetwork::NoaGnss: return ResolveDatabaseFilePath(directory / L"noa_gnss_availability.sqlite");
        case ObservationNetwork::Redgae: return ResolveDatabaseFilePath(directory / L"redgae_availability.sqlite");
        case ObservationNetwork::GeoNetJapan:
            return ResolveDatabaseFilePath(directory / L"geonet_japan_availability.sqlite");
        case ObservationNetwork::Dlab:
            return ResolveDatabaseFilePath(directory / L"dlab_availability.sqlite");
        case ObservationNetwork::Magnet:
            return ResolveDatabaseFilePath(directory / L"magnet_availability.sqlite");
        case ObservationNetwork::PangaCwu:
            return ResolveDatabaseFilePath(directory / L"panga_cwu_availability.sqlite");
        default: return {};
    }
}

std::filesystem::path GetRgpHighRateAvailabilityDatabasePath() {
    auto directory = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
    if (directory.empty()) {
        directory = GetIgsAvailabilityDatabasePath().parent_path();
    }
    return ResolveDatabaseFilePath(directory / L"rgp_highrate_availability.sqlite");
}

NetworkAvailabilityLookup LoadNetworkAvailability(
    const QueryDate& date,
    const ObservationNetwork network,
    const bool includeAllFiles) {
    NetworkAvailabilityLookup result;
    result.databasePath = GetNetworkAvailabilityDatabasePath(network);
    if (!HasSingleSourceKnowledgeBase(network)) return result;
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(result.databasePath, filesystemError)) {
        return result;
    }
    DatabasePtr database;
    // Interactive lookups are strictly read-only.  Running EnsureSchema here
    // used to take a write lock and update metadata on every click, which is
    // particularly expensive when several multi-gigabyte network databases
    // are queried together.  The builder owns schema creation/migration.
    if (!OpenDatabase(result.databasePath, false, database, result.error)) {
        return result;
    }
    const std::string dateText = DateText(date);
    auto scan = Prepare(database.get(),
        "SELECT status,is_provisional,source_url FROM daily_scans WHERE observation_date=?1;",
        result.error);
    if (!scan || !BindText(scan.get(), 1, dateText)) return result;
    if (sqlite3_step(scan.get()) != SQLITE_ROW || ColumnText(scan.get(), 0) != "complete") {
        result.error.clear();
        return result;
    }
    result.provisional = sqlite3_column_int(scan.get(), 1) != 0;
    const std::string baseUrl = ColumnText(scan.get(), 2);
    const char* filesSql = includeAllFiles
        ? "SELECT station_id,four_char_id,country,file_name,url_override,"
          "rinex_format,sample_interval,available_networks,has_coordinate,"
          "latitude,longitude,is_preferred FROM daily_files"
          " WHERE observation_date=?1 ORDER BY station_id,file_name;"
        : "SELECT station_id,four_char_id,country,file_name,url_override,"
          "rinex_format,sample_interval,available_networks,has_coordinate,"
          "latitude,longitude,is_preferred FROM daily_files"
          " WHERE observation_date=?1 AND is_preferred=1"
          " ORDER BY station_id,file_name;";
    auto files = Prepare(database.get(),
        filesSql,
        result.error);
    if (!files || !BindText(files.get(), 1, dateText)) return result;
    while (true) {
        const int status = sqlite3_step(files.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            result.error = SqliteError(database.get(), L"Unable to load daily stations");
            result.stations.clear();
            return result;
        }
        StationFile station;
        station.stationId = ColumnText(files.get(), 0);
        station.fourCharId = ColumnText(files.get(), 1);
        station.country = ColumnText(files.get(), 2);
        station.fileName = ColumnText(files.get(), 3);
        const std::string overrideUrl = ColumnText(files.get(), 4);
        if (overrideUrl.empty()) {
            station.downloadUrl = Utf8ToWide(baseUrl + station.fileName);
        } else if (overrideUrl.find("://") != std::string::npos) {
            station.downloadUrl = Utf8ToWide(overrideUrl);
        } else {
            station.downloadUrl = Utf8ToWide(baseUrl + overrideUrl);
        }
        if (network == ObservationNetwork::GeoNetJapan
            && station.fileName.ends_with("d.gz")) {
            // Terras expands compact RINEX on download and returns *.o.gz.
            // Keep the stored virtual route unchanged, but expose the actual
            // delivered filename to the GUI and destination-path builder.
            station.fileName[station.fileName.size() - 4] = 'o';
        }
        station.format = sqlite3_column_int(files.get(), 5) == 2
            ? RinexFormat::Rinex2 : RinexFormat::Rinex34;
        station.interval = ColumnText(files.get(), 6);
        station.availableNetworks = static_cast<ObservationNetworkMask>(
            sqlite3_column_int64(files.get(), 7));
        station.hasCoordinate = sqlite3_column_int(files.get(), 8) != 0;
        if (station.hasCoordinate) {
            station.latitude = sqlite3_column_double(files.get(), 9);
            station.longitude = sqlite3_column_double(files.get(), 10);
        }
        station.preferredSource = KnowledgeBaseNetworkKey(network);
        station.availabilitySources = {station.preferredSource};
        const bool preferred = sqlite3_column_int(files.get(), 11) != 0;
        if (includeAllFiles) {
            // Preserve the preferred view separately.  A copy is only made
            // for preferred rows; the raw row is moved into allFiles below.
            if (preferred) {
                result.stations.push_back(station);
            }
            result.allFiles.push_back(std::move(station));
        } else {
            result.stations.push_back(std::move(station));
        }
    }
    result.complete = true;
    return result;
}

NetworkAvailabilityRangeStream StreamNetworkAvailabilityRange(
    const QueryDate& startDate,
    const QueryDate& endDate,
    const ObservationNetwork network,
    const bool includeAllFiles,
    const NetworkAvailabilityRangeVisitor& visitor,
    const int rinexVersion,
    const std::string_view sampleInterval,
    const bool allowIncompleteDates,
    const std::span<const std::string> stationFourCharIds,
    const NetworkAvailabilityRangeProgress& progress) {
    NetworkAvailabilityRangeStream result;
    result.databasePath = GetNetworkAvailabilityDatabasePath(network);

    if (!HasSingleSourceKnowledgeBase(network)) {
        result.error = KnowledgeBaseNetworkLabel(network)
            + L"：不是单源站网，不能使用单源日期范围读取。";
        return result;
    }

    std::chrono::sys_days startDays;
    std::chrono::sys_days endDays;
    if (!ToValidatedSysDays(startDate, startDays)
        || !ToValidatedSysDays(endDate, endDays)
        || endDays < startDays) {
        result.error = L"日期范围无效。";
        return result;
    }
    const auto dayCount = (endDays - startDays).count() + 1;
    if (dayCount <= 0
        || dayCount > static_cast<long long>(std::numeric_limits<int>::max())) {
        result.error = L"日期范围过大。";
        return result;
    }
    result.expectedDays = static_cast<int>(dayCount);

    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(
            result.databasePath, filesystemError)) {
        result.error = KnowledgeBaseNetworkLabel(network)
            + L"：本地知识库文件不存在。";
        return result;
    }

    DatabasePtr database;
    // This function is intentionally read-only.  In particular, do not call
    // EnsureSchema here: an interactive range query must not acquire a write
    // lock or mutate a database while another builder is running.
    if (!OpenDatabase(result.databasePath, false, database, result.error)) {
        return result;
    }

    const std::string startText = DateText(QueryDateFromSysDays(startDays));
    const std::string endText = DateText(QueryDateFromSysDays(endDays));

    // First read the compact day table.  Besides supplying the base URL for
    // each file row, this pass makes the strict completeness contract
    // explicit: every requested day must be present, complete, and
    // non-provisional.  A BETWEEN query alone would silently accept a hole.
    auto scans = Prepare(
        database.get(),
        "SELECT observation_date,status,is_provisional,source_url,"
        "station_count,file_count FROM daily_scans "
        "WHERE observation_date BETWEEN ?1 AND ?2 "
        "ORDER BY observation_date;",
        result.error);
    if (!scans
        || !BindText(scans.get(), 1, startText)
        || !BindText(scans.get(), 2, endText)) {
        return result;
    }

    std::unordered_map<std::string, QueryDate> dates;
    std::unordered_map<std::string, std::string> baseUrls;
    dates.reserve(static_cast<std::size_t>(result.expectedDays));
    baseUrls.reserve(static_cast<std::size_t>(result.expectedDays));
    std::int64_t estimatedRows = 0;
    auto expected = startDays;
    int scanStatus = sqlite3_step(scans.get());
    for (int index = 0; index < result.expectedDays; ++index,
             expected += std::chrono::days{1}) {
        const QueryDate expectedDate = QueryDateFromSysDays(expected);
        const std::string expectedText = DateText(expectedDate);
        if (scanStatus != SQLITE_ROW && scanStatus != SQLITE_DONE) {
            result.error = SqliteError(
                database.get(), L"读取本地日期索引失败");
            return result;
        }

        const bool hasRow = scanStatus == SQLITE_ROW;
        const std::string rowDate = hasRow
            ? ColumnText(scans.get(), 0) : std::string{};
        if (hasRow && rowDate < expectedText) {
            result.error = KnowledgeBaseNetworkLabel(network)
                + L"：本地日期索引包含无序或重复记录 "
                + Utf8ToWide(rowDate) + L"。";
            return result;
        }

        // A later row means the current calendar day is absent.  Do not
        // consume that later row: the next loop iteration must still see it.
        if (!hasRow || rowDate > expectedText) {
            result.incompleteDates.push_back(expectedDate);
            if (!allowIncompleteDates) {
                result.error = KnowledgeBaseNetworkLabel(network)
                    + L"：本地知识库缺少日期 "
                    + Utf8ToWide(expectedText) + L"。";
                return result;
            }
            continue;
        }

        const std::string statusText = ColumnText(scans.get(), 1);
        const bool provisional = sqlite3_column_int(scans.get(), 2) != 0;
        const bool usable = statusText == "complete" && !provisional;
        if (!usable) {
            result.incompleteDates.push_back(expectedDate);
            if (!allowIncompleteDates) {
                result.error = KnowledgeBaseNetworkLabel(network)
                    + L"：日期 " + Utf8ToWide(expectedText)
                    + (statusText != "complete"
                        ? L" 的索引状态不是 complete。"
                        : L" 仍是暂定记录。");
                return result;
            }
        } else {
            dates.emplace(rowDate, expectedDate);
            baseUrls.emplace(rowDate, ColumnText(scans.get(), 3));
            const int countColumn = includeAllFiles ? 5 : 4;
            const auto count = sqlite3_column_int64(
                scans.get(), countColumn);
            if (count > 0) estimatedRows += count;
        }
        scanStatus = sqlite3_step(scans.get());
    }
    // There must not be an additional row in the bounded interval.  This is
    // normally guaranteed by the date loop above, but checking the cursor
    // catches malformed/non-ISO date keys instead of returning a partial set.
    if (scanStatus != SQLITE_DONE) {
        result.error = KnowledgeBaseNetworkLabel(network)
            + L"：本地日期索引存在无法解析的额外记录。";
        return result;
    }

    const bool filterVersion = rinexVersion == 2
        || rinexVersion == 3
        || rinexVersion == 4;
    const bool filterInterval = !sampleInterval.empty();
    // Decode a statement with the same twelve-column projection used by both
    // the historical whole-range path and the selected-station seek path.
    const auto consumeFiles = [&](sqlite3_stmt* files) {
        while (true) {
            const int status = sqlite3_step(files);
            if (status == SQLITE_DONE) break;
            if (status != SQLITE_ROW) {
                result.error = SqliteError(
                    database.get(), L"读取本地观测文件索引失败");
                return false;
            }
            const std::string rowDate = ColumnText(files, 0);
            const auto dateIt = dates.find(rowDate);
            const auto baseIt = baseUrls.find(rowDate);
            // A foreign-key violation or a manually edited database can leave
            // a file row outside the validated scan set.  Do not expose it as
            // if the requested range were complete.
            if (dateIt == dates.end() || baseIt == baseUrls.end()) {
                if (allowIncompleteDates) continue;
                result.error = KnowledgeBaseNetworkLabel(network)
                    + L"：观测文件索引包含未验证日期 "
                    + Utf8ToWide(rowDate) + L"。";
                return false;
            }

            StationFile station;
            station.stationId = ColumnText(files, 1);
            station.fourCharId = ColumnText(files, 2);
            station.country = ColumnText(files, 3);
            station.fileName = ColumnText(files, 4);
            const std::string overrideUrl = ColumnText(files, 5);
            if (overrideUrl.empty()) {
                station.downloadUrl = Utf8ToWide(
                    baseIt->second + station.fileName);
            } else if (overrideUrl.find("://") != std::string::npos) {
                station.downloadUrl = Utf8ToWide(overrideUrl);
            } else {
                station.downloadUrl = Utf8ToWide(
                    baseIt->second + overrideUrl);
            }
            if (network == ObservationNetwork::GeoNetJapan
                && station.fileName.ends_with("d.gz")) {
                station.fileName[station.fileName.size() - 4] = 'o';
            }
            station.format = sqlite3_column_int(files, 6) == 2
                ? RinexFormat::Rinex2 : RinexFormat::Rinex34;
            station.interval = ColumnText(files, 7);
            station.availableNetworks = static_cast<ObservationNetworkMask>(
                sqlite3_column_int64(files, 8));
            station.hasCoordinate = sqlite3_column_int(files, 9) != 0;
            if (station.hasCoordinate) {
                station.latitude = sqlite3_column_double(files, 10);
                station.longitude = sqlite3_column_double(files, 11);
            }
            station.preferredSource = KnowledgeBaseNetworkKey(network);
            station.availabilitySources = {station.preferredSource};

            DatedStationFile dated;
            dated.date = dateIt->second;
            dated.station = std::move(station);
            if (visitor && !visitor(std::move(dated))) {
                result.stopped = true;
                return false;
            }
        }
        return true;
    };

    if (stationFourCharIds.empty()) {
        // Preserve the original strict/default whole-range statement exactly.
        std::string filesSql = includeAllFiles
            ? "SELECT observation_date,station_id,four_char_id,country,file_name,"
              "url_override,rinex_format,sample_interval,available_networks,"
              "has_coordinate,latitude,longitude FROM daily_files "
              "WHERE observation_date BETWEEN ?1 AND ?2"
            : "SELECT observation_date,station_id,four_char_id,country,file_name,"
              "url_override,rinex_format,sample_interval,available_networks,"
              "has_coordinate,latitude,longitude FROM daily_files "
              "WHERE observation_date BETWEEN ?1 AND ?2 AND is_preferred=1";
        // DuckDB binds by ordinal, so optional filters must keep indices consecutive.
        int nextFilterParameter = 3;
        const int versionParameter = filterVersion ? nextFilterParameter++ : 0;
        const int intervalParameter = filterInterval ? nextFilterParameter++ : 0;
        if (filterVersion) {
            filesSql += " AND rinex_format=?" + std::to_string(versionParameter);
        }
        if (filterInterval) {
            filesSql += " AND sample_interval=?" + std::to_string(intervalParameter);
        }
        filesSql += " ORDER BY observation_date,file_key;";
        auto files = Prepare(database.get(), filesSql.c_str(), result.error);
        if (!files
            || !BindText(files.get(), 1, startText)
            || !BindText(files.get(), 2, endText)
            || (filterVersion
                && sqlite3_bind_int(files.get(), versionParameter,
                    rinexVersion == 2 ? 2 : 3) != SQLITE_OK)
            || (filterInterval
                && !BindText(files.get(), intervalParameter, sampleInterval))) {
            return result;
        }
        if (!consumeFiles(files.get())) return result;
    } else {
        // file_key is lower-case and begins with either the nine-character
        // station ID or "four:" plus the four-character ID.  With an exact
        // date, these ranges seek the existing (observation_date,file_key)
        // primary key instead of scanning the full multi-year table.
        std::vector<std::string> fourChars;
        std::unordered_set<std::string> seen;
        fourChars.reserve(stationFourCharIds.size());
        for (auto four : stationFourCharIds) {
            if (four.size() != 4U) continue;
            std::transform(four.begin(), four.end(), four.begin(),
                [](const unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });
            if (seen.insert(four).second) fourChars.push_back(std::move(four));
        }
        std::sort(fourChars.begin(), fourChars.end());
        struct KeyRange { std::string low; std::string high; };
        std::vector<KeyRange> ranges;
        ranges.reserve(fourChars.size() * 2U);
        const auto addPrefix = [&](std::string prefix) {
            std::string high = prefix;
            ++high.back();
            ranges.push_back({std::move(prefix), std::move(high)});
        };
        for (const auto& four : fourChars) {
            addPrefix(four);
            addPrefix("four:" + four);
        }

        constexpr std::size_t kRangesPerStatement = 240U;
        struct FilteredStatement {
            StatementPtr statement;
            int dateParameter = 0;
        };
        const auto sqlLiteral = [](const std::string_view value) {
            std::string quoted{"'"};
            for (const char character : value) {
                if (character == '\'') quoted += '\'';
                quoted += character;
            }
            quoted += '\'';
            return quoted;
        };
        std::vector<FilteredStatement> statements;
        for (std::size_t first = 0; first < ranges.size();
             first += kRangesPerStatement) {
            const std::size_t count = std::min(
                kRangesPerStatement, ranges.size() - first);
            std::string sql = "WITH wanted(lo,hi) AS (VALUES ";
            for (std::size_t index = 0; index < count; ++index) {
                if (index != 0) sql += ',';
                const auto& range = ranges[first + index];
                sql += '(' + sqlLiteral(range.low) + ','
                    + sqlLiteral(range.high) + ')';
            }
            // Bound VALUES in this CTE produce an empty range join through
            // DuckDB's C API.  The prefixes are SQL-escaped here; date and
            // user-selected filters remain bound parameters.
            const int dateParameter = 1;
            sql += ") SELECT f.observation_date,f.station_id,f.four_char_id,"
                "f.country,f.file_name,f.url_override,f.rinex_format,"
                "f.sample_interval,f.available_networks,f.has_coordinate,"
                "f.latitude,f.longitude FROM wanted w JOIN daily_files f ON "
                "f.observation_date=?" + std::to_string(dateParameter)
                + " AND f.file_key>=w.lo AND f.file_key<w.hi WHERE 1=1";
            if (!includeAllFiles) sql += " AND f.is_preferred=1";
            int nextParameter = dateParameter + 1;
            if (filterVersion) {
                sql += " AND f.rinex_format=?" + std::to_string(nextParameter++);
            }
            if (filterInterval) {
                sql += " AND f.sample_interval=?" + std::to_string(nextParameter++);
            }
            // The CTE is already emitted in sorted, non-overlapping prefix
            // blocks and every inner lookup walks the primary key in file-key
            // order.  An explicit ORDER BY would make SQLite build one temp
            // B-tree per UTC day (several seconds over a year) without
            // changing the range aggregate's result.
            sql += ';';
            auto statement = Prepare(database.get(), sql.c_str(), result.error);
            if (!statement) return result;
            int staticParameter = dateParameter + 1;
            if (filterVersion
                && sqlite3_bind_int(statement.get(), staticParameter++,
                    rinexVersion == 2 ? 2 : 3) != SQLITE_OK) {
                return result;
            }
            if (filterInterval
                && !BindText(statement.get(), staticParameter, sampleInterval)) {
                return result;
            }
            statements.push_back({std::move(statement), dateParameter});
        }

        const std::size_t totalUnits = static_cast<std::size_t>(
            result.expectedDays) * statements.size();
        std::size_t completedUnits = 0;
        for (auto day = startDays; day <= endDays;
             day += std::chrono::days{1}) {
            const std::string dateText = DateText(QueryDateFromSysDays(day));
            for (auto& filtered : statements) {
                if (dates.contains(dateText)) {
                    sqlite3_reset(filtered.statement.get());
                    if (!BindText(filtered.statement.get(),
                            filtered.dateParameter, dateText)
                        || !consumeFiles(filtered.statement.get())) {
                        return result;
                    }
                }
                ++completedUnits;
                if (progress) progress(completedUnits, totalUnits);
            }
        }
    }

    result.complete = true;
    return result;
}

NetworkAvailabilityRangeLookup LoadNetworkAvailabilityRange(
    const QueryDate& startDate,
    const QueryDate& endDate,
    const ObservationNetwork network,
    const bool includeAllFiles,
    const int rinexVersion,
    const std::string_view sampleInterval) {
    NetworkAvailabilityRangeLookup lookup;
    lookup.databasePath = GetNetworkAvailabilityDatabasePath(network);
    const auto stream = StreamNetworkAvailabilityRange(
        startDate,
        endDate,
        network,
        includeAllFiles,
        [&](DatedStationFile&& dated) {
            lookup.files.push_back(std::move(dated));
            return true;
        },
        rinexVersion,
        sampleInterval);
    lookup.complete = stream.complete;
    lookup.expectedDays = stream.expectedDays;
    lookup.databasePath = stream.databasePath;
    lookup.error = stream.error;
    return lookup;
}

bool StoreNetworkAvailability(
    const QueryDate& date,
    const ObservationNetwork network,
    const std::vector<StationFile>& preferredStations,
    const std::vector<StationFile>& allFiles,
    std::wstring& error) {
    std::scoped_lock lock(gDatabaseMutex);
    return StoreScan(date, network, &preferredStations, &allFiles, {}, error);
}

bool StoreNetworkAvailabilityFailure(
    const QueryDate& date,
    const ObservationNetwork network,
    const std::wstring& message,
    std::wstring& error) {
    std::scoped_lock lock(gDatabaseMutex);
    return StoreScan(date, network, nullptr, nullptr, message, error);
}

NetworkAvailabilityStats GetNetworkAvailabilityStats(
    const ObservationNetwork network) {
    NetworkAvailabilityStats result;
    result.databasePath = GetNetworkAvailabilityDatabasePath(network);
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(result.databasePath, filesystemError)) {
        return result;
    }
    DatabasePtr database;
    if (!OpenDatabase(result.databasePath, false, database, result.error)) {
        return result;
    }
    auto statement = Prepare(database.get(),
        "SELECT SUM(status='complete' AND is_provisional=0),"
        "SUM(status='complete' AND is_provisional<>0),SUM(status='failed'),"
        "COALESCE(SUM(station_count),0),COALESCE(SUM(file_count),0),"
        "COALESCE(MIN(observation_date),''),COALESCE(MAX(observation_date),'')"
        " FROM daily_scans;", result.error);
    if (!statement || sqlite3_step(statement.get()) != SQLITE_ROW) return result;
    result.completeDays = sqlite3_column_int(statement.get(), 0);
    result.provisionalDays = sqlite3_column_int(statement.get(), 1);
    result.failedDays = sqlite3_column_int(statement.get(), 2);
    result.stations = sqlite3_column_int64(statement.get(), 3);
    result.files = sqlite3_column_int64(statement.get(), 4);
    result.earliestDate = ColumnText(statement.get(), 5);
    result.latestDate = ColumnText(statement.get(), 6);
    return result;
}
