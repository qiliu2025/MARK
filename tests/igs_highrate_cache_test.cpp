#include "DateUtil.h"
#include "IgsAvailabilityDb.h"

#include "sqlite3.h"

#include <Windows.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool Exec(sqlite3* db, const char* sql) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        std::cerr << (message == nullptr ? sqlite3_errmsg(db) : message)
                  << '\n';
        sqlite3_free(message);
        return false;
    }
    return true;
}

bool RunSql(const std::filesystem::path& path, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open16(path.c_str(), &db) != SQLITE_OK) return false;
    sqlite3_stmt* statement = nullptr;
    const bool okay = sqlite3_prepare_v2(db, sql, -1, &statement, nullptr)
        == SQLITE_OK && sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return okay;
}

int Scalar(const std::filesystem::path& path, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open16(path.c_str(), &db) != SQLITE_OK) return -1;
    sqlite3_stmt* statement = nullptr;
    int value = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK
        && sqlite3_step(statement) == SQLITE_ROW) {
        value = sqlite3_column_int(statement, 0);
    }
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return value;
}

IgsHighrateSourceScan Scan(
    const char* key,
    bool success,
    const char* station = "ABCD",
    int fileCount = 96) {
    IgsHighrateSourceScan result;
    result.sourceKey = key;
    result.success = success;
    result.listingUrl = L"https://example.invalid/highrate";
    if (!success) {
        result.networkFailure = true;
        result.error = L"transport failure";
        return result;
    }
    if (station == nullptr) return result;
    result.fileCount = fileCount;
    const std::string marker = station;
    result.stations.push_back(IgsHighrateStationScan{
        marker + "00XXX", marker, 2, fileCount});
    return result;
}

struct EnvironmentRestore {
    std::wstring db;
    std::wstring index;
    std::wstring force;
    std::wstring disabled;
    static std::wstring Read(const wchar_t* name) {
        const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
        if (length == 0) return {};
        std::wstring value(length, L'\0');
        GetEnvironmentVariableW(name, value.data(), length);
        value.resize(length - 1);
        return value;
    }
    EnvironmentRestore()
        : db(Read(L"GNSS_DOWNLOADER_DB_PATH")),
          index(Read(L"GNSS_DOWNLOADER_INDEX_DIR")),
          force(Read(L"GNSS_DOWNLOADER_FORCE_ONLINE_IGS")),
          disabled(Read(L"GNSS_DOWNLOADER_DISABLE_LOCAL_INDEX")) {}
    ~EnvironmentRestore() {
        SetEnvironmentVariableW(L"GNSS_DOWNLOADER_DB_PATH",
            db.empty() ? nullptr : db.c_str());
        SetEnvironmentVariableW(L"GNSS_DOWNLOADER_INDEX_DIR",
            index.empty() ? nullptr : index.c_str());
        SetEnvironmentVariableW(L"GNSS_DOWNLOADER_FORCE_ONLINE_IGS",
            force.empty() ? nullptr : force.c_str());
        SetEnvironmentVariableW(L"GNSS_DOWNLOADER_DISABLE_LOCAL_INDEX",
            disabled.empty() ? nullptr : disabled.c_str());
    }
};

} // namespace

int wmain() {
    EnvironmentRestore restore;
    const auto tempRoot = std::filesystem::weakly_canonical(
        std::filesystem::temp_directory_path());
    const auto root = tempRoot / (L"igs_highrate_cache_test_"
        + std::to_wstring(GetCurrentProcessId()) + L"_"
        + std::to_wstring(GetTickCount64()));
    std::error_code ignored;
    if (!std::filesystem::create_directory(root, ignored)) {
        std::cerr << "FAIL create unique temporary directory\n";
        return 1;
    }
    const auto databasePath = root / L"legacy.sqlite";
    SetEnvironmentVariableW(L"GNSS_DOWNLOADER_DB_PATH", databasePath.c_str());
    SetEnvironmentVariableW(L"GNSS_DOWNLOADER_INDEX_DIR", root.c_str());
    SetEnvironmentVariableW(L"GNSS_DOWNLOADER_FORCE_ONLINE_IGS", nullptr);
    SetEnvironmentVariableW(L"GNSS_DOWNLOADER_DISABLE_LOCAL_INDEX", nullptr);

    // Simulate a pre-revision database. OpenDatabase must migrate it without
    // deleting the legacy checkpoint, while the reader must force a rescan.
    sqlite3* legacy = nullptr;
    if (sqlite3_open16(databasePath.c_str(), &legacy) != SQLITE_OK
        || !Exec(legacy,
            "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
            "INSERT INTO metadata VALUES('schema_version','5');"
            "CREATE TABLE highrate_daily_source_scans("
            "network TEXT,observation_date TEXT,source_id INTEGER,status TEXT,"
            "listing_url TEXT,file_count INTEGER,station_count INTEGER,"
            "error_message TEXT,scanned_at_utc TEXT,"
            "PRIMARY KEY(network,observation_date,source_id));"
            "INSERT INTO highrate_daily_source_scans VALUES("
            "'IGS','2020-01-01',2,'complete','legacy',96,1,'','now');")) {
        std::cerr << "FAIL create legacy database\n";
        if (legacy != nullptr) sqlite3_close(legacy);
        return 1;
    }
    sqlite3_close(legacy);
    const QueryDate legacyDate = MakeQueryDate(2020, 1, 1);
    if (LoadIgsHighrateSourceCache(legacyDate, "BKG").has_value()
        || IsIgsHighrateDateComplete(legacyDate)) {
        std::cerr << "FAIL stale legacy cache was trusted\n";
        return 2;
    }

    const QueryDate date = MakeQueryDate(2020, 1, 2);
    std::vector<IgsHighrateSourceScan> sources{
        Scan("BKG", true), Scan("GFZ", true, nullptr, 0),
        Scan("IGN", true, "EFGH", 48), Scan("ESA", true, nullptr, 0)};
    std::wstring error;
    if (!StoreIgsHighrateAvailability(date, sources, error)
        || !IsIgsHighrateDateComplete(date)
        || !LoadIgsHighrateSourceCache(date, "BKG").has_value()
        || !LoadIgsHighrateSourceCache(date, "GFZ").has_value()
        || !LoadIgsHighrateSourceCache(date, "ESA").has_value()) {
        std::wcerr << L"FAIL current revision cache: " << error << L'\n';
        return 3;
    }
    if (Scalar(databasePath,
            "SELECT scan_revision FROM highrate_daily_source_scans "
            "WHERE observation_date='2020-01-02' AND source_id=2;") != 1
        || Scalar(databasePath,
            "SELECT scan_revision FROM highrate_daily_source_scans "
            "WHERE observation_date='2020-01-01' AND source_id=2;") != 0) {
        std::cerr << "FAIL revision migration/storage\n";
        return 4;
    }

    SetEnvironmentVariableW(L"GNSS_DOWNLOADER_FORCE_ONLINE_IGS", L"1");
    if (LoadIgsHighrateSourceCache(date, "BKG").has_value()
        || IsIgsHighrateDateComplete(date)) {
        std::cerr << "FAIL forced-online cache bypass\n";
        return 5;
    }
    SetEnvironmentVariableW(L"GNSS_DOWNLOADER_FORCE_ONLINE_IGS", nullptr);

    // A failed refresh must invalidate completeness but retain the previous
    // positive station row until that source is successfully rescanned.
    if (!RunSql(databasePath,
            "UPDATE highrate_daily_source_scans SET scan_revision=0 "
            "WHERE observation_date='2020-01-02' AND source_id=2;")
        || IsIgsHighrateDateComplete(date)
        || LoadIgsHighrateSourceCache(date, "BKG").has_value()
        || Scalar(databasePath,
            "SELECT COUNT(*) FROM highrate_daily_observations "
            "WHERE observation_date='2020-01-02';") != 2) {
        std::cerr << "FAIL stale checkpoint invalidation\n";
        return 6;
    }

    sources = {Scan("BKG", false), Scan("GFZ", true, nullptr, 0),
        Scan("IGN", true, "EFGH", 48), Scan("ESA", true, nullptr, 0)};
    if (!StoreIgsHighrateAvailability(date, sources, error)
        || IsIgsHighrateDateComplete(date)
        || Scalar(databasePath,
            "SELECT COUNT(*) FROM highrate_daily_observations "
            "WHERE observation_date='2020-01-02';") != 2
        || Scalar(databasePath,
            "SELECT COUNT(*) FROM highrate_daily_sources "
            "WHERE observation_date='2020-01-02' AND source_id=2 "
            "AND station_key='FOUR:ABCD';") != 1) {
        std::wcerr << L"FAIL partial refresh retention: " << error << L'\n';
        return 7;
    }

    // Only remove the unique directory this test created beneath the
    // resolved system temporary directory.
    if (std::filesystem::weakly_canonical(root).parent_path() == tempRoot) {
        std::filesystem::remove_all(root, ignored);
    }
    std::cout << "IGS high-rate cache revision regression passed.\n";
    return 0;
}
