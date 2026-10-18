#include "IgsAvailabilityDb.h"

#include "DateUtil.h"

#include "sqlite3.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

std::mutex gAvailabilityDatabaseMutex;
// Increment whenever the high-rate directory parser/coverage contract changes.
// Rows written by older revisions are retained for diagnostics, but are not
// trusted as an online-cache hit until a fresh scan writes this revision.
constexpr int kHighrateScanRevision = 1;

// Confirmed continuous observation windows from the public GFZ/BKG
// directory audit.  Empty cache rows inside these windows are suspicious:
// the source should contain files, so they must be checked again after a
// parser, proxy, or transient listing failure.
bool HighrateSourceShouldContainFiles(
    const QueryDate& date, const std::string_view source) {
    if (source == "GFZ") {
        if (date.year == 2018) return date.dayOfYear == 121;
        if (date.year == 2019) return date.dayOfYear == 287;
        if (date.year >= 2020 && date.year <= 2025) return true;
        if (date.year == 2026) return date.dayOfYear <= 254;
        return false;
    }
    if (source == "BKG") {
        if (date.year == 2020) return date.dayOfYear >= 128
            && date.dayOfYear <= 250;
        if (date.year == 2021) return date.dayOfYear >= 24
            && date.dayOfYear <= 268;
        if (date.year == 2022) return date.dayOfYear >= 7;
        if (date.year >= 2023 && date.year <= 2025) return true;
        if (date.year == 2026) return date.dayOfYear <= 254;
        return false;
    }
    // The current IGN and ESA high-rate trees have daily directories for the
    // operational period covered by the index.  Empty rows in this period
    // were observed to be parser/transport artefacts, so let the next run
    // verify them instead of treating them as durable cache hits.
    if ((source == "IGN" || source == "ESA")
        && date.year >= 2020 && date.year <= 2026) {
        return true;
    }
    return false;
}

std::string WideToUtf8(const std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        size,
        nullptr,
        nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        size);
    return result;
}

std::wstring SqliteError(sqlite3* database, const std::wstring_view action) {
    const char* message = database == nullptr
        ? "unknown SQLite error"
        : sqlite3_errmsg(database);
    return std::wstring(action) + L"：" + Utf8ToWide(message == nullptr ? "" : message);
}

bool EnvironmentFlag(const wchar_t* name) {
    wchar_t value[16]{};
    const DWORD length = GetEnvironmentVariableW(
        name,
        value,
        static_cast<DWORD>(std::size(value)));
    return length > 0 && (value[0] == L'1' || value[0] == L'y'
        || value[0] == L'Y' || value[0] == L't' || value[0] == L'T');
}

std::filesystem::path EnvironmentPath(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return {};
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        name,
        value.data(),
        required);
    if (written == 0 || written >= required) {
        return {};
    }
    value.resize(written);
    return std::filesystem::path(value);
}

std::filesystem::path ExecutableDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD written = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size()) {
        return {};
    }
    buffer.resize(written);
    return std::filesystem::path(buffer).parent_path();
}

#ifdef GNSS_DOWNLOADER_DUCKDB_BACKEND
bool HasBundledMainDatabase(const std::filesystem::path& directory) {
    std::error_code error;
    if (std::filesystem::is_regular_file(
            directory / L"gnss_availability.duckdb", error)) {
        return true;
    }
    error.clear();
    return std::filesystem::is_regular_file(
        directory / L"gnss_availability.sqlite", error);
}

bool LooksLikeDuckDbFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(8, std::ios::beg);
    char magic[4]{};
    input.read(magic, sizeof(magic));
    return input.gcount() == static_cast<std::streamsize>(sizeof(magic))
        && std::equal(std::begin(magic), std::end(magic), "DUCK");
}

std::filesystem::path FindBundledKnowledgeBaseDirectory() {
    const auto executable = ExecutableDirectory();
    if (executable.empty()) return {};

    std::vector<std::filesystem::path> candidates;
    candidates.reserve(24);
    const auto add = [&candidates](const std::filesystem::path& value) {
        if (value.empty()) return;
        if (std::find(candidates.begin(), candidates.end(), value)
            == candidates.end()) {
            candidates.push_back(value);
        }
    };
    // Normal portable layout: release/software/MARK.exe.
    add(executable.parent_path() / L"knowledge-base");
    add(executable / L"knowledge-base");
    for (const auto& candidate : candidates) {
        if (HasBundledMainDatabase(candidate)) return candidate;
    }

    // Also support a developer launching out/build/Release directly.  Walk a
    // few ancestors and look for a sibling duckdb-release-* bundle; this does
    // not affect an installed package and avoids silently opening the old
    // SQLite profile during local testing.
    auto cursor = executable;
    for (int level = 0; level < 7 && !cursor.empty(); ++level) {
        add(cursor / L"knowledge-base");
        std::error_code directoryError;
        for (std::filesystem::directory_iterator it(
                 cursor, directoryError), end; !directoryError && it != end;
             it.increment(directoryError)) {
            if (!it->is_directory(directoryError)) continue;
            const auto name = it->path().filename().wstring();
            if (name.rfind(L"duckdb-release-", 0) == 0) {
                add(it->path() / L"knowledge-base");
            }
        }
        cursor = cursor.parent_path();
    }
    for (const auto& candidate : candidates) {
        if (HasBundledMainDatabase(candidate)) return candidate;
    }
    return {};
}
#endif

std::filesystem::path ResolveDatabasePath() {
    const auto explicitPath = EnvironmentPath(L"GNSS_DOWNLOADER_DB_PATH");
    if (!explicitPath.empty()) {
        std::error_code explicitError;
        const auto resolved = ResolveDatabaseFilePath(explicitPath);
        if (std::filesystem::is_regular_file(resolved, explicitError)) {
#ifdef GNSS_DOWNLOADER_DUCKDB_BACKEND
            if (LooksLikeDuckDbFile(resolved)) return resolved;
#else
            return resolved;
#endif
        }
    }
#ifdef GNSS_DOWNLOADER_DUCKDB_BACKEND
    // The packaged DuckDB build is self-contained: when launched by double
    // click (without run_duckdb.cmd), prefer its bundled knowledge base over
    // the user's legacy SQLite profile under %LOCALAPPDATA%.
    const auto bundledDirectory = FindBundledKnowledgeBaseDirectory();
    if (!bundledDirectory.empty()) {
        const auto bundled = bundledDirectory / L"gnss_availability.duckdb";
        std::error_code bundledError;
        if (std::filesystem::is_regular_file(bundled, bundledError)) {
            return bundled;
        }
        return ResolveDatabaseFilePath(
            bundledDirectory / L"gnss_availability.sqlite");
    }
#endif
    auto localAppData = EnvironmentPath(L"LOCALAPPDATA");
    if (localAppData.empty()) {
        localAppData = ExecutableDirectory();
    }
    const auto settingsPath = localAppData / L"GNSSDownloader" / L"settings.ini";
    wchar_t configuredPath[32768]{};
    const DWORD configuredLength = GetPrivateProfileStringW(
        L"storage",
        L"database_path",
        L"",
        configuredPath,
        static_cast<DWORD>(std::size(configuredPath)),
        settingsPath.c_str());
    if (configuredLength > 0
        && configuredLength < static_cast<DWORD>(std::size(configuredPath) - 1)) {
        const auto configured = std::filesystem::path(configuredPath);
#ifdef GNSS_DOWNLOADER_DUCKDB_BACKEND
        // Never feed the DuckDB compatibility layer the legacy SQLite path
        // from settings.ini.  If no portable bundle was found, return the
        // expected DuckDB location so the GUI can report it explicitly.
        if (configured.extension() == L".sqlite") {
            auto duckdb = configured;
            duckdb.replace_extension(L".duckdb");
            return duckdb;
        }
#endif
        return configured;
    }
#ifdef GNSS_DOWNLOADER_DUCKDB_BACKEND
    return localAppData / L"GNSSDownloader" / L"gnss_availability.duckdb";
#else
    return localAppData / L"GNSSDownloader" / L"gnss_availability.sqlite";
#endif
}

void WritePathDiagnostic(const std::filesystem::path& path) {
    const auto destination = EnvironmentPath(
        L"GNSS_DOWNLOADER_PATH_DIAGNOSTIC");
    if (destination.empty()) return;
    std::wofstream output(destination, std::ios::app);
    if (!output) return;
    output << L"IGS=" << path.wstring() << L"\n";
}

bool EnsureSeedDatabase(const std::filesystem::path& destination) {
    std::error_code error;
    if (std::filesystem::is_regular_file(destination, error)) {
        return true;
    }
    error.clear();
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        return false;
    }
    const auto seed = ExecutableDirectory()
        / L"data"
        / L"gnss_availability.sqlite";
    if (!std::filesystem::is_regular_file(seed, error)) {
        return true;
    }
    error.clear();
    std::filesystem::copy_file(
        seed,
        destination,
        std::filesystem::copy_options::overwrite_existing,
        error);
    return !error;
}

struct DatabaseCloser {
    void operator()(sqlite3* database) const {
        if (database != nullptr) {
            sqlite3_close(database);
        }
    }
};

struct StatementCloser {
    void operator()(sqlite3_stmt* statement) const {
        if (statement != nullptr) {
            sqlite3_finalize(statement);
        }
    }
};

using DatabasePtr = std::unique_ptr<sqlite3, DatabaseCloser>;
using StatementPtr = std::unique_ptr<sqlite3_stmt, StatementCloser>;

bool Execute(sqlite3* database, const char* sql, std::wstring& error) {
    char* sqliteMessage = nullptr;
    const int status = sqlite3_exec(
        database,
        sql,
        nullptr,
        nullptr,
        &sqliteMessage);
    if (status == SQLITE_OK) {
        return true;
    }
    error = L"SQLite：" + Utf8ToWide(
        sqliteMessage == nullptr ? sqlite3_errmsg(database) : sqliteMessage);
    sqlite3_free(sqliteMessage);
    return false;
}

std::string ExistingSchemaVersion(sqlite3* database) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(
            database,
            "SELECT value FROM metadata WHERE key='schema_version';",
            -1,
            &raw,
            nullptr) != SQLITE_OK) {
        return {};
    }
    StatementPtr statement(raw);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        return {};
    }
    const auto* value = sqlite3_column_text(statement.get(), 0);
    return value == nullptr
        ? std::string{}
        : std::string(reinterpret_cast<const char*>(value));
}

bool OpenDatabase(
    const std::filesystem::path& path,
    DatabasePtr& database,
    std::wstring& error) {
    EnsureSeedDatabase(path);
    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(), directoryError);
    if (directoryError) {
        error = L"无法创建本地索引目录：" + path.parent_path().wstring();
        return false;
    }
    sqlite3* raw = nullptr;
    const int status = sqlite3_open16(path.c_str(), &raw);
    database.reset(raw);
    if (status != SQLITE_OK || raw == nullptr) {
        error = SqliteError(raw, L"无法打开本地可用性索引");
        return false;
    }
    sqlite3_busy_timeout(raw, 5000);
    const std::string existingVersion = ExistingSchemaVersion(raw);
    if (!existingVersion.empty()
        && existingVersion != "4"
        && existingVersion != "5") {
        error = L"The local GNSS index uses schema v"
            + Utf8ToWide(existingVersion)
            + L". Run the matching IGS database migration tool before opening it.";
        return false;
    }
    const char* schema =
        "PRAGMA journal_mode=DELETE;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS metadata("
        " key TEXT PRIMARY KEY,"
        " value TEXT NOT NULL"
        ") WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS source_definitions("
        " source_id INTEGER PRIMARY KEY,"
        " source_key TEXT NOT NULL UNIQUE,"
        " source_priority INTEGER NOT NULL UNIQUE"
        ");"
        "CREATE TABLE IF NOT EXISTS daily_scans("
        " network TEXT NOT NULL,"
        " observation_date TEXT NOT NULL,"
        " status TEXT NOT NULL CHECK(status IN ('complete','failed')),"
        " file_count INTEGER NOT NULL DEFAULT 0,"
        " is_provisional INTEGER NOT NULL DEFAULT 0,"
        " scanned_at_utc TEXT NOT NULL,"
        " PRIMARY KEY(network, observation_date)"
        ") WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS daily_files("
        " network TEXT NOT NULL,"
        " observation_date TEXT NOT NULL,"
        " station_key TEXT NOT NULL,"
        " station_id TEXT NOT NULL,"
        " four_char_id TEXT NOT NULL,"
        " country TEXT NOT NULL DEFAULT '',"
        " file_name TEXT NOT NULL,"
        " url_override TEXT NOT NULL DEFAULT '',"
        " rinex_format INTEGER NOT NULL,"
        " sample_interval TEXT NOT NULL DEFAULT '',"
        " preference_score INTEGER NOT NULL DEFAULT 0,"
        " available_networks INTEGER NOT NULL,"
        " has_coordinate INTEGER NOT NULL DEFAULT 0,"
        " latitude REAL,"
        " longitude REAL,"
        " preferred_source_id INTEGER NOT NULL DEFAULT 0,"
        " source_mask INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY(network, observation_date, station_key,file_name,url_override),"
        " FOREIGN KEY(network, observation_date)"
        " REFERENCES daily_scans(network, observation_date)"
        " ON DELETE CASCADE"
        ") WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS daily_source_scans("
        " network TEXT NOT NULL,"
        " observation_date TEXT NOT NULL,"
        " source_id INTEGER NOT NULL,"
        " status TEXT NOT NULL CHECK(status IN ('complete','failed')),"
        " data_url TEXT NOT NULL,"
        " file_count INTEGER NOT NULL DEFAULT 0,"
        " station_count INTEGER NOT NULL DEFAULT 0,"
        " error_message TEXT NOT NULL DEFAULT '',"
        " PRIMARY KEY(network,observation_date,source_id),"
        " FOREIGN KEY(network,observation_date)"
        " REFERENCES daily_scans(network,observation_date) ON DELETE CASCADE,"
        " FOREIGN KEY(source_id) REFERENCES source_definitions(source_id)"
        ") WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS daily_observation_files("
        " network TEXT NOT NULL,"
        " observation_date TEXT NOT NULL,"
        " observation_key TEXT NOT NULL,"
        " station_key TEXT NOT NULL,"
        " canonical_file_name TEXT NOT NULL,"
        " rinex_format INTEGER NOT NULL,"
        " sample_interval TEXT NOT NULL DEFAULT '',"
        " preferred_source_id INTEGER NOT NULL,"
        " source_mask INTEGER NOT NULL,"
        " PRIMARY KEY(network,observation_date,observation_key),"
        " FOREIGN KEY(network,observation_date)"
        " REFERENCES daily_scans(network,observation_date) ON DELETE CASCADE"
        ") WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS daily_observation_file_sources("
        " network TEXT NOT NULL,"
        " observation_date TEXT NOT NULL,"
        " observation_key TEXT NOT NULL,"
        " source_id INTEGER NOT NULL,"
        " file_name TEXT NOT NULL,"
        " url_override TEXT NOT NULL DEFAULT '',"
        " preference_score INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY(network,observation_date,observation_key,source_id,file_name,url_override),"
        " FOREIGN KEY(network,observation_date)"
        " REFERENCES daily_scans(network,observation_date) ON DELETE CASCADE,"
        " FOREIGN KEY(source_id) REFERENCES source_definitions(source_id)"
        ") WITHOUT ROWID;"
        // High-rate observations deliberately use a day-level manifest.  The
        // 1-second archive contains up to 96 fifteen-minute files per
        // station/day, so storing every physical file here would make the
        // IGS database grow by several gigabytes per year.  The actual
        // directory is enumerated when a high-rate download is hydrated.
        "CREATE TABLE IF NOT EXISTS highrate_daily_scans("
        " network TEXT NOT NULL," 
        " observation_date TEXT NOT NULL," 
        " status TEXT NOT NULL CHECK(status IN ('complete','failed'))," 
        " source_count INTEGER NOT NULL DEFAULT 0," 
        " file_count INTEGER NOT NULL DEFAULT 0," 
        " scanned_at_utc TEXT NOT NULL," 
        " PRIMARY KEY(network,observation_date)"
        ") WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS highrate_daily_observations("
        " network TEXT NOT NULL," 
        " observation_date TEXT NOT NULL," 
        " station_key TEXT NOT NULL," 
        " station_id TEXT NOT NULL," 
        " four_char_id TEXT NOT NULL," 
        " rinex_format_mask INTEGER NOT NULL DEFAULT 0," 
        " sample_interval TEXT NOT NULL DEFAULT '01S'," 
        " has_data INTEGER NOT NULL DEFAULT 1," 
        " file_count INTEGER NOT NULL DEFAULT 0," 
        " source_mask INTEGER NOT NULL DEFAULT 0," 
        " PRIMARY KEY(network,observation_date,station_key)," 
        " FOREIGN KEY(network,observation_date)"
        " REFERENCES highrate_daily_scans(network,observation_date)"
        " ON DELETE CASCADE"
        ") WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS highrate_daily_sources("
        " network TEXT NOT NULL," 
        " observation_date TEXT NOT NULL," 
        " station_key TEXT NOT NULL," 
        " source_id INTEGER NOT NULL," 
        " listing_url TEXT NOT NULL," 
        " file_count INTEGER NOT NULL DEFAULT 0," 
        " status TEXT NOT NULL CHECK(status IN ('complete','failed'))," 
        " error_message TEXT NOT NULL DEFAULT ''," 
        " PRIMARY KEY(network,observation_date,station_key,source_id)," 
        " FOREIGN KEY(network,observation_date)"
        " REFERENCES highrate_daily_scans(network,observation_date)"
        " ON DELETE CASCADE," 
        " FOREIGN KEY(source_id) REFERENCES source_definitions(source_id)"
        ") WITHOUT ROWID;"
        // Source/date level high-rate checkpoints.  This table is additive:
        // old manifests remain valid and are bootstrapped below from their
        // existing per-source rows.
        "CREATE TABLE IF NOT EXISTS highrate_daily_source_scans("
        " network TEXT NOT NULL,"
        " observation_date TEXT NOT NULL,"
        " source_id INTEGER NOT NULL,"
        " status TEXT NOT NULL CHECK(status IN ('complete','empty','failed')),"
        " listing_url TEXT NOT NULL DEFAULT '',"
        " file_count INTEGER NOT NULL DEFAULT 0,"
        " station_count INTEGER NOT NULL DEFAULT 0,"
        " error_message TEXT NOT NULL DEFAULT '',"
        " scan_revision INTEGER NOT NULL DEFAULT 0,"
        " scanned_at_utc TEXT NOT NULL,"
        " PRIMARY KEY(network,observation_date,source_id),"
        " FOREIGN KEY(source_id) REFERENCES source_definitions(source_id)"
        ") WITHOUT ROWID;"
        // Lightweight audit trail for rescans.  One row is written for the
        // aggregate (source_id=0) and one for each source, preserving the
        // counts that existed immediately before a replacement.
        "CREATE TABLE IF NOT EXISTS highrate_rescan_diffs("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " network TEXT NOT NULL,"
        " observation_date TEXT NOT NULL,"
        " source_id INTEGER NOT NULL DEFAULT 0,"
        " old_station_count INTEGER NOT NULL DEFAULT 0,"
        " new_station_count INTEGER NOT NULL DEFAULT 0,"
        " old_file_count INTEGER NOT NULL DEFAULT 0,"
        " new_file_count INTEGER NOT NULL DEFAULT 0,"
        " old_status TEXT NOT NULL DEFAULT '',"
        " new_status TEXT NOT NULL DEFAULT '',"
        " recorded_at_utc TEXT NOT NULL"
        ");"
        // Optional daily quality metrics imported from the public IGS
        // Network QC service.  This table is deliberately additive: the
        // availability/query paths do not depend on it, while the importer
        // and future UI/reporting code can safely share the same database.
        "CREATE TABLE IF NOT EXISTS daily_observation_quality("
        " network TEXT NOT NULL DEFAULT 'IGS',"
        " station_key TEXT NOT NULL,"
        " observation_date TEXT NOT NULL,"
        " observations_json TEXT,"
        " latency_json TEXT,"
        " position_json TEXT,"
        " residuals_json TEXT,"
        " multipath_json TEXT,"
        " cycle_slips_json TEXT,"
        " source_url TEXT NOT NULL,"
        " rinex_version_scope TEXT NOT NULL DEFAULT 'unknown',"
        " fetched_at_utc TEXT NOT NULL,"
        " PRIMARY KEY(network,station_key,observation_date)"
        ") WITHOUT ROWID;"
        "CREATE INDEX IF NOT EXISTS daily_observation_quality_date"
        " ON daily_observation_quality(observation_date,station_key);"
        "INSERT OR IGNORE INTO source_definitions VALUES"
        " (1,'CDDIS',1),(2,'BKG',2),(3,'WHU',3),(4,'KASI',4),"
        " (5,'IGN',5),(6,'ESA',6),(7,'SOPAC',7),(8,'GFZ',8);"
        "INSERT INTO metadata(key,value) VALUES('schema_version','5')"
        " ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
    if (!Execute(raw, schema, error)) {
        return false;
    }
    // Bootstrap source checkpoints once: OpenDatabase runs for every scan date.
    // Write the marker after success so interrupted migrations retry.
    bool checkpointBootstrapNeeded = true;
    {
        sqlite3_stmt* markerRaw = nullptr;
        if (sqlite3_prepare_v2(
                raw,
                "SELECT 1 FROM metadata "
                "WHERE key='highrate_source_checkpoint_bootstrap_v1' LIMIT 1;",
                -1,
                &markerRaw,
                nullptr) == SQLITE_OK) {
            StatementPtr marker(markerRaw);
            checkpointBootstrapNeeded = sqlite3_step(marker.get()) != SQLITE_ROW;
        }
    }
    if (checkpointBootstrapNeeded) {
        const char* checkpointBootstrap =
            "INSERT OR IGNORE INTO highrate_daily_source_scans"
            "(network,observation_date,source_id,status,listing_url,file_count,"
            " station_count,error_message,scanned_at_utc)"
            " SELECT network,observation_date,source_id,'complete',"
            " MIN(listing_url),SUM(file_count),COUNT(*),'',"
            " COALESCE((SELECT scanned_at_utc FROM highrate_daily_scans hs"
            "   WHERE hs.network=s.network AND hs.observation_date=s.observation_date),"
            "   strftime('%Y-%m-%dT%H:%M:%SZ','now'))"
            " FROM highrate_daily_sources s"
            " GROUP BY network,observation_date,source_id;";
        if (!Execute(raw, checkpointBootstrap, error)) {
            return false;
        }
        if (!Execute(raw,
                "INSERT OR REPLACE INTO metadata"
                "(key,value) VALUES"
                "('highrate_source_checkpoint_bootstrap_v1','completed');",
                error)) {
            return false;
        }
    }
    // The source-checkpoint table predates scan_revision in existing user
    // databases.  Migrate it additively so old rows remain available for
    // diagnosis but are treated as stale by the current cache reader.
    const int revisionColumn = sqlite3_table_column_metadata(
        raw, nullptr, "highrate_daily_source_scans", "scan_revision",
        nullptr, nullptr, nullptr, nullptr, nullptr);
    if (revisionColumn != SQLITE_OK) {
        if (!Execute(raw,
                "ALTER TABLE highrate_daily_source_scans "
                "ADD COLUMN scan_revision INTEGER NOT NULL DEFAULT 0;",
                error)) {
            return false;
        }
    }
    return true;
}

// Read-only query connections can coexist with availability-builder commits.
bool OpenReadOnlyDatabase(
    const std::filesystem::path& path,
    DatabasePtr& database,
    std::wstring& error) {
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(path, filesystemError)) {
        error = L"本地可用性知识库不存在：" + path.wstring();
        return false;
    }
    const std::string utf8Path = WideToUtf8(path.wstring());
    if (utf8Path.empty()) {
        error = L"无法编码本地可用性索引路径";
        return false;
    }
    sqlite3* raw = nullptr;
    const int status = sqlite3_open_v2(
        utf8Path.c_str(),
        &raw,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    database.reset(raw);
    if (status != SQLITE_OK || raw == nullptr) {
        error = SqliteError(raw, L"无法以只读方式打开本地可用性索引");
        return false;
    }
    // Cache and memory-backed sorts reduce I/O for large read-only range scans.
    // Pragmas affect this connection only; older SQLite may ignore mmap_size.
    sqlite3_exec(
        raw,
        "PRAGMA query_only=ON;"
        "PRAGMA temp_store=MEMORY;"
        "PRAGMA cache_size=-65536;"
        "PRAGMA mmap_size=268435456;",
        nullptr,
        nullptr,
        nullptr);
    sqlite3_busy_timeout(raw, 2'000);
    const std::string existingVersion = ExistingSchemaVersion(raw);
    if (!existingVersion.empty()
        && existingVersion != "4"
        && existingVersion != "5") {
        error = L"The local GNSS index uses schema v"
            + Utf8ToWide(existingVersion)
            + L". Run the matching IGS database migration tool before opening it.";
        return false;
    }
    return true;
}

StatementPtr Prepare(
    sqlite3* database,
    const char* sql,
    std::wstring& error) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK) {
        error = SqliteError(database, L"无法准备本地索引查询");
        return {};
    }
    return StatementPtr(raw);
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

std::string DateSourceKey(
    const std::string_view date,
    const int sourceId) {
    return std::string(date) + '\x1f' + std::to_string(sourceId);
}

bool IsProvisionalDate(const QueryDate& date) {
    const auto observationDay = std::chrono::sys_days{
        std::chrono::year{date.year}
        / std::chrono::month{static_cast<unsigned>(date.month)}
        / std::chrono::day{static_cast<unsigned>(date.day)}};
    const auto today = std::chrono::floor<std::chrono::days>(
        std::chrono::system_clock::now());
    return observationDay >= today - std::chrono::days{6};
}

std::string ColumnText(sqlite3_stmt* statement, const int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value == nullptr
        ? std::string{}
        : std::string(reinterpret_cast<const char*>(value));
}

bool BindText(
    sqlite3_stmt* statement,
    const int index,
    const std::string& value) {
    return sqlite3_bind_text(
        statement,
        index,
        value.c_str(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT) == SQLITE_OK;
}

struct SourceDefinition {
    int id;
    const char* key;
    int priority;
};

constexpr SourceDefinition kSources[]{
    {1, "CDDIS", 1},
    {2, "BKG", 2},
    {3, "WHU", 3},
    {4, "KASI", 4},
    {5, "IGN", 5},
    {6, "ESA", 6},
    {7, "SOPAC", 7},
    {8, "GFZ", 8},
};

int SourceId(const std::string_view key) {
    for (const auto& source : kSources) {
        if (key == source.key) {
            return source.id;
        }
    }
    return 0;
}

const char* SourceKey(const int id) {
    for (const auto& source : kSources) {
        if (id == source.id) {
            return source.key;
        }
    }
    return "";
}

int SourcePriorityForKey(const std::string_view source) {
    // Keep this order in sync with the normal single-day loader.  BKG and
    // SOPAC HTTPS mirrors are generally more proxy-friendly than CDDIS FTPS,
    // while CDDIS remains the final fallback.
    if (source == "BKG") return 1;
    if (source == "SOPAC") return 2;
    if (source == "WHU") return 3;
    if (source == "ESA") return 4;
    if (source == "IGN") return 5;
    if (source == "KASI") return 6;
    if (source == "CDDIS") return 7;
    if (source == "GFZ") return 8;
    return 99;
}

int SourceBit(const int sourceId) {
    return sourceId >= 1 && sourceId <= 31
        ? 1 << (sourceId - 1)
        : 0;
}

std::vector<std::string> SourceKeysFromMask(const int mask) {
    std::vector<std::string> values;
    for (const auto& source : kSources) {
        if ((mask & SourceBit(source.id)) != 0) {
            values.emplace_back(source.key);
        }
    }
    return values;
}

int SourceMask(const std::vector<std::string>& keys) {
    int mask = 0;
    for (const auto& key : keys) {
        mask |= SourceBit(SourceId(key));
    }
    return mask;
}

std::string UrlOverride(
    const std::wstring_view fullUrl,
    const std::wstring_view baseUrl,
    const std::string_view fileName) {
    std::wstring reconstructed(baseUrl);
    reconstructed.append(fileName.begin(), fileName.end());
    return reconstructed == fullUrl ? std::string{} : WideToUtf8(fullUrl);
}

std::string StationKey(const StationFile& station) {
    return station.stationId.size() == 9
        ? station.stationId
        : std::string("FOUR:") + station.fourCharId;
}

std::string ObservationFileKey(const StationFile& station) {
    std::string normalized = station.fileName;
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    if (normalized.ends_with(".gz")) {
        normalized.resize(normalized.size() - 3);
    } else if (normalized.ends_with(".z")) {
        normalized.resize(normalized.size() - 2);
    }
    return StationKey(station) + "\x1f" + normalized;
}

}  // namespace

std::filesystem::path GetIgsAvailabilityDatabasePath() {
    const auto path = ResolveDatabasePath();
    WritePathDiagnostic(path);
    return path;
}

IgsAvailabilityLookup LoadIgsAvailability(const QueryDate& date) {
    std::scoped_lock databaseLock(gAvailabilityDatabaseMutex);
    IgsAvailabilityLookup result;
    result.databasePath = ResolveDatabasePath();
    if (EnvironmentFlag(L"GNSS_DOWNLOADER_DISABLE_LOCAL_INDEX")) {
        return result;
    }
    DatabasePtr database;
    if (!OpenReadOnlyDatabase(result.databasePath, database, result.error)) {
        return result;
    }
    const std::string dateText = DateText(date);
    auto scan = Prepare(
        database.get(),
        "SELECT status,is_provisional FROM daily_scans"
        " WHERE network='IGS' AND observation_date=?1;",
        result.error);
    if (!scan || !BindText(scan.get(), 1, dateText)) {
        if (result.error.empty()) {
            result.error = SqliteError(database.get(), L"无法读取本地索引日期");
        }
        return result;
    }
    if (sqlite3_step(scan.get()) != SQLITE_ROW
        || ColumnText(scan.get(), 0) != "complete") {
        result.error.clear();
        return result;
    }
    result.provisional = sqlite3_column_int(scan.get(), 1) != 0;
    auto sourceCompleteness = Prepare(
        database.get(),
        "SELECT COUNT(*),"
        " SUM(CASE WHEN status='complete' THEN 1 ELSE 0 END)"
        " FROM daily_source_scans"
        " WHERE network='IGS' AND observation_date=?1;",
        result.error);
    if (!sourceCompleteness
        || !BindText(sourceCompleteness.get(), 1, dateText)
        || sqlite3_step(sourceCompleteness.get()) != SQLITE_ROW) {
        if (result.error.empty()) {
            result.error = SqliteError(
                database.get(),
                L"Unable to check IGS source completeness");
        }
        return result;
    }
    const int configuredSources =
        sqlite3_column_int(sourceCompleteness.get(), 0);
    const int completedSources =
        sqlite3_column_int(sourceCompleteness.get(), 1);
    // Version-1/BKG-only rows have no per-source scans. Treat them as a
    // usable fallback, but force one online seven-source refresh.
    result.provisional = result.provisional
        || configuredSources < 7
        || completedSources < 7;

    auto files = Prepare(
        database.get(),
        "SELECT f.station_id,f.four_char_id,f.country,f.file_name,"
        " CASE WHEN f.url_override<>'' THEN f.url_override"
        "      ELSE COALESCE(s.data_url,'')||f.file_name END,"
        " f.rinex_format,f.sample_interval,f.preference_score,"
        " f.available_networks,f.has_coordinate,f.latitude,f.longitude,"
        " f.preferred_source_id,f.source_mask"
        " FROM daily_files f"
        " LEFT JOIN daily_source_scans s"
        "   ON s.network=f.network"
        "  AND s.observation_date=f.observation_date"
        "  AND s.source_id=f.preferred_source_id"
        " WHERE f.network='IGS' AND f.observation_date=?1"
        " ORDER BY f.station_id,f.preference_score DESC,f.file_name;",
        result.error);
    if (!files || !BindText(files.get(), 1, dateText)) {
        if (result.error.empty()) {
            result.error = SqliteError(database.get(), L"无法读取本地索引文件");
        }
        return result;
    }
    while (true) {
        const int status = sqlite3_step(files.get());
        if (status == SQLITE_DONE) {
            break;
        }
        if (status != SQLITE_ROW) {
            result.error = SqliteError(database.get(), L"读取本地索引文件失败");
            result.records.clear();
            return result;
        }
        IndexedIgsAvailability record;
        record.station.stationId = ColumnText(files.get(), 0);
        record.station.fourCharId = ColumnText(files.get(), 1);
        record.station.country = ColumnText(files.get(), 2);
        record.station.fileName = ColumnText(files.get(), 3);
        record.station.downloadUrl = Utf8ToWide(ColumnText(files.get(), 4));
        record.station.format = sqlite3_column_int(files.get(), 5) == 2
            ? RinexFormat::Rinex2
            : RinexFormat::Rinex34;
        record.station.interval = ColumnText(files.get(), 6);
        record.preferenceScore = sqlite3_column_int(files.get(), 7);
        record.station.availableNetworks = static_cast<ObservationNetworkMask>(
            sqlite3_column_int64(files.get(), 8));
        record.station.hasCoordinate = sqlite3_column_int(files.get(), 9) != 0;
        if (record.station.hasCoordinate) {
            record.station.latitude = sqlite3_column_double(files.get(), 10);
            record.station.longitude = sqlite3_column_double(files.get(), 11);
        }
        record.station.preferredSource = SourceKey(
            sqlite3_column_int(files.get(), 12));
        record.station.availabilitySources = SourceKeysFromMask(
            sqlite3_column_int(files.get(), 13));
        record.sourceKey = record.station.preferredSource;
        result.records.push_back(std::move(record));
    }

    // Keep every exact route for the same logical observation file.  CDDIS
    // is the most complete archive, but its FTPS/TLS path is not necessarily
    // usable on every user's proxy.  Prefer anonymous HTTPS mirrors for the
    // interactive downloader and retain CDDIS as the final fallback.
    auto routeRows = Prepare(
        database.get(),
        "SELECT r.observation_key,d.source_key,r.file_name,"
        " CASE WHEN r.url_override<>'' THEN r.url_override"
        "      ELSE COALESCE(s.data_url,'')||r.file_name END"
        " FROM daily_observation_file_sources r"
        " JOIN source_definitions d ON d.source_id=r.source_id"
        " LEFT JOIN daily_source_scans s"
        "   ON s.network=r.network"
        "  AND s.observation_date=r.observation_date"
        "  AND s.source_id=r.source_id"
        " WHERE r.network='IGS' AND r.observation_date=?1"
        " ORDER BY r.observation_key,"
        " CASE d.source_key"
        "   WHEN 'BKG' THEN 1"
        "   WHEN 'SOPAC' THEN 2"
        "   WHEN 'WHU' THEN 3"
        "   WHEN 'ESA' THEN 4"
        "   WHEN 'IGN' THEN 5"
        "   WHEN 'KASI' THEN 6"
        "   WHEN 'CDDIS' THEN 7 ELSE 99 END,"
        " r.preference_score DESC;",
        result.error);
    if (!routeRows || !BindText(routeRows.get(), 1, dateText)) {
        if (result.error.empty()) {
            result.error = SqliteError(
                database.get(), L"无法读取 IGS 多数据中心下载路线");
        }
        result.records.clear();
        return result;
    }
    std::unordered_map<std::string, std::vector<DownloadRoute>> routesByFile;
    while (true) {
        const int status = sqlite3_step(routeRows.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            result.error = SqliteError(
                database.get(), L"读取 IGS 多数据中心下载路线失败");
            result.records.clear();
            return result;
        }
        const std::string observationKey = ColumnText(routeRows.get(), 0);
        DownloadRoute route;
        route.source = ColumnText(routeRows.get(), 1);
        route.fileName = ColumnText(routeRows.get(), 2);
        route.url = Utf8ToWide(ColumnText(routeRows.get(), 3));
        if (route.url.empty() || route.fileName.empty()) continue;
        auto& routes = routesByFile[observationKey];
        const bool duplicate = std::any_of(
            routes.begin(), routes.end(), [&](const DownloadRoute& existing) {
                return existing.url == route.url;
            });
        if (!duplicate) routes.push_back(std::move(route));
    }
    for (auto& record : result.records) {
        const auto routes = routesByFile.find(
            ObservationFileKey(record.station));
        if (routes == routesByFile.end() || routes->second.empty()) continue;
        record.station.downloadRoutes = routes->second;
        record.station.downloadUrl = routes->second.front().url;
        record.station.fileName = routes->second.front().fileName;
        record.station.preferredSource = routes->second.front().source;
    }
    result.complete = !result.provisional;
    return result;
}

IgsAvailabilityLookup LoadIgsAvailabilityAllFiles(const QueryDate& date) {
    // Keep the established loader as the source of completeness semantics
    // and the preferred physical-station metadata.  The additional queries
    // below only replace the file view; ordinary daily queries therefore keep
    // exactly the same behaviour and cost as before.
    IgsAvailabilityLookup preferred = LoadIgsAvailability(date);
    if (EnvironmentFlag(L"GNSS_DOWNLOADER_DISABLE_LOCAL_INDEX")
        || !preferred.error.empty()
        || (!preferred.complete && preferred.records.empty())) {
        return preferred;
    }

    std::scoped_lock databaseLock(gAvailabilityDatabaseMutex);
    IgsAvailabilityLookup result = preferred;
    DatabasePtr database;
    if (!OpenReadOnlyDatabase(result.databasePath, database, result.error)) {
        // A concurrent writer can briefly make a second read-only open fail.
        // The preferred view is still valid, so do not turn an otherwise
        // usable daily index into a hard range-query failure.
        result = preferred;
        return result;
    }
    const std::string dateText = DateText(date);

    // Build a station metadata lookup from the preferred daily rows.  The
    // logical-file table stores only station_key; coordinates, country and
    // nine-character aliases remain in daily_files.
    std::unordered_map<std::string, StationFile> metadataByStation;
    metadataByStation.reserve(preferred.records.size() * 2U + 1U);
    for (const auto& record : preferred.records) {
        const auto key = StationKey(record.station);
        const auto found = metadataByStation.find(key);
        if (found == metadataByStation.end()
            || (!found->second.hasCoordinate
                && record.station.hasCoordinate)) {
            metadataByStation.insert_or_assign(key, record.station);
        }
    }

    // Read every concrete route first.  The ordering is the same as the
    // normal loader: BKG/SOPAC/WHU/ESA/IGN/KASI/CDDIS, then preference score.
    // This ordering is deliberately independent of the preferred_source_id
    // persisted by the builder, so a mirror that is temporarily unreachable
    // can be tried first by the downloader.
    auto routeRows = Prepare(
        database.get(),
        "SELECT r.observation_key,d.source_key,r.file_name,"
        " CASE WHEN r.url_override<>'' THEN r.url_override"
        "      ELSE COALESCE(s.data_url,'')||r.file_name END"
        " FROM daily_observation_file_sources r"
        " JOIN source_definitions d ON d.source_id=r.source_id"
        " LEFT JOIN daily_source_scans s"
        "   ON s.network=r.network"
        "  AND s.observation_date=r.observation_date"
        "  AND s.source_id=r.source_id"
        " WHERE r.network='IGS' AND r.observation_date=?1"
        " ORDER BY r.observation_key,"
        " CASE d.source_key"
        "   WHEN 'BKG' THEN 1"
        "   WHEN 'SOPAC' THEN 2"
        "   WHEN 'WHU' THEN 3"
        "   WHEN 'ESA' THEN 4"
        "   WHEN 'IGN' THEN 5"
        "   WHEN 'KASI' THEN 6"
        "   WHEN 'CDDIS' THEN 7 ELSE 99 END,"
        " r.preference_score DESC;",
        result.error);
    if (!routeRows || !BindText(routeRows.get(), 1, dateText)) {
        // v4 databases created by an older migration may not have the route
        // table populated yet.  Falling back to preferred rows is safer than
        // reporting a false missing-date error to the GUI.
        result = preferred;
        result.error.clear();
        return result;
    }
    std::unordered_map<std::string, std::vector<DownloadRoute>> routesByObservation;
    routesByObservation.reserve(preferred.records.size() * 2U + 1U);
    while (true) {
        const int status = sqlite3_step(routeRows.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            result = preferred;
            result.error.clear();
            return result;
        }
        const std::string observationKey = ColumnText(routeRows.get(), 0);
        DownloadRoute route;
        route.source = ColumnText(routeRows.get(), 1);
        route.fileName = ColumnText(routeRows.get(), 2);
        route.url = Utf8ToWide(ColumnText(routeRows.get(), 3));
        if (observationKey.empty() || route.source.empty()
            || route.fileName.empty() || route.url.empty()) {
            continue;
        }
        auto& routes = routesByObservation[observationKey];
        const bool duplicate = std::any_of(
            routes.begin(), routes.end(), [&](const DownloadRoute& existing) {
                return existing.source == route.source
                    && existing.fileName == route.fileName
                    && existing.url == route.url;
            });
        if (!duplicate) routes.push_back(std::move(route));
    }

    // Some early v4 rows contain the source mask but no source route (for
    // example a failed retry).  Keep source data URLs available for a safe
    // fallback URL in that case.
    std::unordered_map<int, std::wstring> sourceBaseUrls;
    auto sourceBases = Prepare(
        database.get(),
        "SELECT source_id,data_url FROM daily_source_scans"
        " WHERE network='IGS' AND observation_date=?1;",
        result.error);
    if (sourceBases && BindText(sourceBases.get(), 1, dateText)) {
        while (sqlite3_step(sourceBases.get()) == SQLITE_ROW) {
            sourceBaseUrls.emplace(
                sqlite3_column_int(sourceBases.get(), 0),
                Utf8ToWide(ColumnText(sourceBases.get(), 1)));
        }
    }

    auto logicalFiles = Prepare(
        database.get(),
        "SELECT observation_key,station_key,canonical_file_name,"
        " rinex_format,sample_interval,preferred_source_id,source_mask"
        " FROM daily_observation_files"
        " WHERE network='IGS' AND observation_date=?1"
        " ORDER BY observation_key;",
        result.error);
    if (!logicalFiles || !BindText(logicalFiles.get(), 1, dateText)) {
        result = preferred;
        result.error.clear();
        return result;
    }

    std::vector<IndexedIgsAvailability> allRecords;
    allRecords.reserve(preferred.records.size() * 2U + 1U);
    const auto sourcePriorityFor = [](const std::string_view source) {
        if (source == "CDDIS") return 1;
        if (source == "BKG") return 2;
        if (source == "WHU") return 3;
        if (source == "KASI") return 4;
        if (source == "IGN") return 5;
        if (source == "ESA") return 6;
        if (source == "SOPAC") return 7;
        return 99;
    };
    const auto appendUnique = [](std::vector<std::string>& values,
                                 const std::string& value) {
        if (value.empty()) return;
        if (std::find(values.begin(), values.end(), value) == values.end()) {
            values.push_back(value);
        }
    };

    while (true) {
        const int status = sqlite3_step(logicalFiles.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            result = preferred;
            result.error.clear();
            return result;
        }
        const std::string observationKey = ColumnText(logicalFiles.get(), 0);
        const std::string stationKey = ColumnText(logicalFiles.get(), 1);
        const std::string canonicalName = ColumnText(logicalFiles.get(), 2);
        if (observationKey.empty() || canonicalName.empty()) continue;
        StationFile station;
        const auto metadata = metadataByStation.find(stationKey);
        if (metadata != metadataByStation.end()) {
            station = metadata->second;
        } else if (stationKey.rfind("FOUR:", 0) == 0) {
            station.fourCharId = stationKey.substr(5);
        } else {
            station.stationId = stationKey;
        }
        station.fileName = canonicalName;
        station.downloadUrl.clear();
        station.downloadRoutes.clear();
        station.format = sqlite3_column_int(logicalFiles.get(), 3) == 2
            ? RinexFormat::Rinex2
            : RinexFormat::Rinex34;
        station.interval = ColumnText(logicalFiles.get(), 4);
        const int preferredSourceId = sqlite3_column_int(logicalFiles.get(), 5);
        const int sourceMask = sqlite3_column_int(logicalFiles.get(), 6);
        station.availableNetworks = static_cast<ObservationNetworkMask>(
            (station.availableNetworks
                | NetworkBit(ObservationNetwork::Igs))
            & ~NetworkBit(ObservationNetwork::Mgex));
        if (station.format == RinexFormat::Rinex34) {
            station.availableNetworks = static_cast<ObservationNetworkMask>(
                station.availableNetworks
                | NetworkBit(ObservationNetwork::Mgex));
        }
        station.preferredSource = SourceKey(preferredSourceId);
        station.availabilitySources = SourceKeysFromMask(sourceMask);
        IndexedIgsAvailability record;
        record.station = std::move(station);
        record.sourceKey = record.station.preferredSource;
        record.sourcePriority = sourcePriorityFor(record.sourceKey);
        auto routes = routesByObservation.find(observationKey);
        if (routes != routesByObservation.end()) {
            record.station.downloadRoutes = routes->second;
            if (!routes->second.empty()) {
                record.station.downloadUrl = routes->second.front().url;
                record.station.fileName = routes->second.front().fileName;
                record.station.preferredSource = routes->second.front().source;
                record.sourceKey = record.station.preferredSource;
                record.sourcePriority = sourcePriorityFor(record.sourceKey);
                record.preferenceScore = 0;
                for (const auto& route : routes->second) {
                    appendUnique(record.station.availabilitySources, route.source);
                }
            }
        }
        if (record.station.downloadUrl.empty()
            && preferredSourceId > 0) {
            const auto base = sourceBaseUrls.find(preferredSourceId);
            if (base != sourceBaseUrls.end() && !base->second.empty()) {
                record.station.downloadUrl = base->second +
                    Utf8ToWide(record.station.fileName);
            }
        }
        allRecords.push_back(std::move(record));
    }

    // If the logical union is absent (legacy rows or a partially migrated
    // date), preserve the known preferred station view.  Otherwise expose the
    // complete logical-file list to the caller.
    if (allRecords.empty()) {
        result = preferred;
        result.error.clear();
        return result;
    }
    result.records = std::move(allRecords);
    result.error.clear();
    result.complete = preferred.complete;
    return result;
}

IgsAvailabilityRangeStream StreamIgsAvailabilityRange(
    const QueryDate& startDate,
    const QueryDate& endDate,
    const IgsAvailabilityRangeVisitor& visitor,
    const int rinexVersion,
    const std::string_view sampleInterval,
    const bool allowIncompleteDates,
    const std::span<const std::string> stationFourCharIds,
    const IgsAvailabilityRangeProgress& progress) {
    std::scoped_lock databaseLock(gAvailabilityDatabaseMutex);
    IgsAvailabilityRangeStream result;
    result.databasePath = ResolveDatabasePath();
    if (EnvironmentFlag(L"GNSS_DOWNLOADER_DISABLE_LOCAL_INDEX")) {
        result.error = L"IGS 本地知识库已被环境变量禁用。";
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

    DatabasePtr database;
    if (!OpenReadOnlyDatabase(result.databasePath, database, result.error)) {
        if (result.error.empty()) {
            result.error = L"IGS 本地知识库文件不存在。";
        }
        return result;
    }

    const std::string startText = DateText(QueryDateFromSysDays(startDays));
    const std::string endText = DateText(QueryDateFromSysDays(endDays));

    struct DayState {
        QueryDate date;
        std::string text;
        bool scanComplete = false;
        bool provisional = false;
        int completeSourceMask = 0;
        int sourceCount = 0;
        int fileCount = 0;
        bool usable = false;
    };
    std::vector<DayState> days;
    days.reserve(static_cast<std::size_t>(result.expectedDays));
    std::unordered_map<std::string, std::size_t> dayIndexByText;
    dayIndexByText.reserve(static_cast<std::size_t>(result.expectedDays) * 2U);
    auto cursor = startDays;
    for (int index = 0; index < result.expectedDays; ++index,
             cursor += std::chrono::days{1}) {
        const QueryDate date = QueryDateFromSysDays(cursor);
        const std::string text = DateText(date);
        dayIndexByText.emplace(text, days.size());
        days.push_back(DayState{date, text});
    }

    // Validate the compact daily scan rows first.  The seven source rows are
    // checked in a second pass below so old v1/BKG-only rows cannot be
    // mistaken for a complete seven-centre date.
    auto scanRows = Prepare(
        database.get(),
        "SELECT observation_date,status,is_provisional,file_count"
        " FROM daily_scans WHERE network='IGS'"
        " AND observation_date BETWEEN ?1 AND ?2"
        " ORDER BY observation_date;",
        result.error);
    if (!scanRows
        || !BindText(scanRows.get(), 1, startText)
        || !BindText(scanRows.get(), 2, endText)) {
        return result;
    }
    while (true) {
        const int status = sqlite3_step(scanRows.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            result.error = SqliteError(
                database.get(), L"读取 IGS 日期索引失败");
            return result;
        }
        const std::string text = ColumnText(scanRows.get(), 0);
        const auto found = dayIndexByText.find(text);
        if (found == dayIndexByText.end()) continue;
        auto& day = days[found->second];
        day.scanComplete = ColumnText(scanRows.get(), 1) == "complete";
        day.provisional = sqlite3_column_int(scanRows.get(), 2) != 0;
        day.fileCount = std::max(0, sqlite3_column_int(scanRows.get(), 3));
    }

    auto sourceRows = Prepare(
        database.get(),
        "SELECT observation_date,source_id,status"
        " FROM daily_source_scans WHERE network='IGS'"
        " AND observation_date BETWEEN ?1 AND ?2"
        " ORDER BY observation_date,source_id;",
        result.error);
    if (!sourceRows
        || !BindText(sourceRows.get(), 1, startText)
        || !BindText(sourceRows.get(), 2, endText)) {
        return result;
    }
    while (true) {
        const int status = sqlite3_step(sourceRows.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            result.error = SqliteError(
                database.get(), L"读取 IGS 数据中心完整性失败");
            return result;
        }
        const std::string text = ColumnText(sourceRows.get(), 0);
        const auto found = dayIndexByText.find(text);
        if (found == dayIndexByText.end()) continue;
        auto& day = days[found->second];
        ++day.sourceCount;
        const int sourceId = sqlite3_column_int(sourceRows.get(), 1);
        if (ColumnText(sourceRows.get(), 2) == "complete"
            && sourceId >= 1 && sourceId <= 7) {
            day.completeSourceMask |= SourceBit(sourceId);
        }
    }

    std::int64_t estimatedRecords = 0;
    for (auto& day : days) {
        const bool complete = day.scanComplete
            && !day.provisional
            && day.sourceCount == 7
            && day.completeSourceMask == 0x7f;
        if (!complete) result.incompleteDates.push_back(day.date);
        // StoreIgsAvailability writes a complete daily union whenever at
        // least one archive was read successfully.  A 6/7 day is therefore
        // useful for downloading: its logical rows and routes come only from
        // the six successful sources.  Missing/failed daily rows remain
        // unusable even in tolerant mode.
        day.usable = day.scanComplete && day.completeSourceMask != 0;
        if (day.usable && day.fileCount > 0) {
            estimatedRecords += day.fileCount;
        }
    }
    if (!result.incompleteDates.empty() && !allowIncompleteDates) {
        result.error = L"IGS 本地知识库在请求范围内缺少完整七源日期：";
        const std::size_t shown = std::min<std::size_t>(
            result.incompleteDates.size(), 8U);
        for (std::size_t index = 0; index < shown; ++index) {
            if (index != 0) result.error += L"、";
            result.error += FormatDate(result.incompleteDates[index]);
        }
        if (shown < result.incompleteDates.size()) {
            result.error += std::format(
                L" 等 {} 天", result.incompleteDates.size());
        }
        return result;
    }

    // Keep source data URLs available for logical rows that have no route
    // (early v4 migrations can contain a source mask but an empty route
    // table).  This is tiny compared with the file rows: at most seven per
    // day.
    std::unordered_map<std::string, std::wstring> sourceBases;
    sourceBases.reserve(days.size() * 8U + 1U);
    auto bases = Prepare(
        database.get(),
        "SELECT observation_date,source_id,data_url"
        " FROM daily_source_scans WHERE network='IGS'"
        " AND observation_date BETWEEN ?1 AND ?2"
        " ORDER BY observation_date,source_id;",
        result.error);
    if (!bases
        || !BindText(bases.get(), 1, startText)
        || !BindText(bases.get(), 2, endText)) {
        return result;
    }
    while (true) {
        const int status = sqlite3_step(bases.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            result.error = SqliteError(
                database.get(), L"读取 IGS 数据中心下载地址失败");
            return result;
        }
        sourceBases.emplace(
            DateSourceKey(
                ColumnText(bases.get(), 0),
                sqlite3_column_int(bases.get(), 1)),
            Utf8ToWide(ColumnText(bases.get(), 2)));
    }

    const bool filterVersion = rinexVersion == 2
        || rinexVersion == 3
        || rinexVersion == 4;
    const bool filterInterval = !sampleInterval.empty();
    std::string currentDate;
    std::string currentObservation;
    IndexedIgsAvailability current;
    bool haveCurrent = false;
    auto flushCurrent = [&]() -> bool {
        if (!haveCurrent) return true;
        if (current.station.downloadUrl.empty()
            && !current.station.fileName.empty()
            && !current.station.preferredSource.empty()) {
            const int sourceId = SourceId(current.station.preferredSource);
            const auto base = sourceBases.find(
                DateSourceKey(currentDate, sourceId));
            if (base != sourceBases.end() && !base->second.empty()) {
                current.station.downloadUrl = base->second
                    + Utf8ToWide(current.station.fileName);
            }
        }
        current.sourceKey = current.station.preferredSource;
        current.sourcePriority = SourcePriorityForKey(current.sourceKey);
        if (visitor && !visitor(DatedIndexedIgsAvailability{
                days[dayIndexByText.at(currentDate)].date,
                std::move(current)})) {
            current = {};
            haveCurrent = false;
            result.stopped = true;
            return false;
        }
        current = {};
        haveCurrent = false;
        return true;
    };
    const auto consumeLogicalRows = [&](sqlite3_stmt* logicalRows) {
        while (true) {
            const int status = sqlite3_step(logicalRows);
            if (status == SQLITE_DONE) break;
            if (status != SQLITE_ROW) {
                result.error = SqliteError(
                    database.get(), L"读取 IGS 日期范围逻辑文件失败");
                return false;
            }
            const std::string rowDate = ColumnText(logicalRows, 0);
            const auto dayFound = dayIndexByText.find(rowDate);
            if (dayFound == dayIndexByText.end()
                || !days[dayFound->second].usable) {
                continue;
            }
            const std::string observationKey = ColumnText(logicalRows, 1);
            if (observationKey.empty()) continue;
            if (!haveCurrent
                || rowDate != currentDate
                || observationKey != currentObservation) {
                if (!flushCurrent()) return false;
                currentDate = rowDate;
                currentObservation = observationKey;
                current = {};
                haveCurrent = true;

                const std::string stationKey = ColumnText(logicalRows, 2);
                current.station.fileName = ColumnText(logicalRows, 3);
                current.station.format = sqlite3_column_int(logicalRows, 4) == 2
                    ? RinexFormat::Rinex2 : RinexFormat::Rinex34;
                current.station.interval = ColumnText(logicalRows, 5);
                const int preferredSourceId = sqlite3_column_int(
                    logicalRows, 6);
                const int sourceMask = sqlite3_column_int(logicalRows, 7);

                const auto stationId = ColumnText(logicalRows, 8);
                const auto fourCharId = ColumnText(logicalRows, 9);
                if (!stationId.empty() || !fourCharId.empty()) {
                    current.station.stationId = stationId;
                    current.station.fourCharId = fourCharId;
                    current.station.country = ColumnText(logicalRows, 10);
                    current.station.availableNetworks =
                        static_cast<ObservationNetworkMask>(
                            sqlite3_column_int64(logicalRows, 11));
                    current.station.hasCoordinate = sqlite3_column_int(
                        logicalRows, 12) != 0;
                    if (current.station.hasCoordinate) {
                        current.station.latitude = sqlite3_column_double(
                            logicalRows, 13);
                        current.station.longitude = sqlite3_column_double(
                            logicalRows, 14);
                    }
                } else if (stationKey.rfind("FOUR:", 0) == 0) {
                    current.station.fourCharId = stationKey.substr(5);
                } else {
                    current.station.stationId = stationKey;
                }
                current.station.availableNetworks =
                    static_cast<ObservationNetworkMask>(
                        (current.station.availableNetworks
                            | NetworkBit(ObservationNetwork::Igs))
                        & ~NetworkBit(ObservationNetwork::Mgex));
                if (current.station.format == RinexFormat::Rinex34) {
                    current.station.availableNetworks =
                        static_cast<ObservationNetworkMask>(
                            current.station.availableNetworks
                            | NetworkBit(ObservationNetwork::Mgex));
                }
                current.station.preferredSource = SourceKey(preferredSourceId);
                current.station.availabilitySources =
                    SourceKeysFromMask(sourceMask);
                current.preferenceScore = 0;
            }

            const int sourceId = sqlite3_column_type(logicalRows, 15)
                == SQLITE_NULL ? 0 : sqlite3_column_int(logicalRows, 15);
            if (sourceId <= 0) continue;
            DownloadRoute route;
            route.source = ColumnText(logicalRows, 16);
            route.fileName = ColumnText(logicalRows, 17);
            const std::string overrideUrl = ColumnText(logicalRows, 18);
            const std::string dataUrl = ColumnText(logicalRows, 20);
            route.url = Utf8ToWide(overrideUrl.empty()
                ? dataUrl + route.fileName : overrideUrl);
            if (route.source.empty() || route.fileName.empty()
                || route.url.empty()) {
                continue;
            }
            const bool duplicate = std::any_of(
                current.station.downloadRoutes.begin(),
                current.station.downloadRoutes.end(),
                [&](const DownloadRoute& existing) {
                    return existing.source == route.source
                        && existing.fileName == route.fileName
                        && existing.url == route.url;
                });
            if (duplicate) continue;
            const int score = sqlite3_column_int(logicalRows, 19);
            current.preferenceScore = std::max(current.preferenceScore, score);
            current.station.downloadRoutes.push_back(std::move(route));
            const auto& selected = current.station.downloadRoutes.back();
            if (current.station.downloadUrl.empty()) {
                current.station.downloadUrl = selected.url;
                current.station.fileName = selected.fileName;
                current.station.preferredSource = selected.source;
            }
            if (std::find(current.station.availabilitySources.begin(),
                    current.station.availabilitySources.end(), selected.source)
                == current.station.availabilitySources.end()) {
                current.station.availabilitySources.push_back(selected.source);
            }
        }
        return true;
    };

    const std::string projection =
        "SELECT o.observation_date,o.observation_key,o.station_key,"
        " o.canonical_file_name,o.rinex_format,o.sample_interval,"
        " o.preferred_source_id,o.source_mask,"
        " m.station_id,m.four_char_id,m.country,m.available_networks,"
        " m.has_coordinate,m.latitude,m.longitude,"
        " r.source_id,d.source_key,r.file_name,r.url_override,"
        " r.preference_score,COALESCE(s.data_url,'')";
    const std::string joins =
        " LEFT JOIN daily_files m ON m.network=o.network"
        "  AND m.observation_date=o.observation_date"
        "  AND m.station_key=o.station_key"
        " LEFT JOIN daily_observation_file_sources r"
        "  ON r.network=o.network"
        " AND r.observation_date=o.observation_date"
        " AND r.observation_key=o.observation_key"
        " LEFT JOIN source_definitions d ON d.source_id=r.source_id"
        " LEFT JOIN daily_source_scans s ON s.network=r.network"
        "  AND s.observation_date=r.observation_date"
        "  AND s.source_id=r.source_id";
    const std::string order =
        " ORDER BY o.observation_date,o.observation_key,"
        " CASE d.source_key WHEN 'BKG' THEN 1 WHEN 'SOPAC' THEN 2"
        " WHEN 'WHU' THEN 3 WHEN 'ESA' THEN 4 WHEN 'IGN' THEN 5"
        " WHEN 'KASI' THEN 6 WHEN 'CDDIS' THEN 7 ELSE 99 END,"
        " r.preference_score DESC,r.file_name;";

    if (stationFourCharIds.empty()) {
        // Preserve the original strict/default whole-range statement.
        std::string logicalSql = projection
            + " FROM daily_observation_files o" + joins
            + " WHERE o.network='IGS'"
              " AND o.observation_date BETWEEN ?1 AND ?2";
        // DuckDB binds by ordinal, so optional filters must keep indices consecutive.
        int nextFilterParameter = 3;
        const int versionParameter = filterVersion ? nextFilterParameter++ : 0;
        const int intervalParameter = filterInterval ? nextFilterParameter++ : 0;
        if (filterVersion) {
            logicalSql += " AND o.rinex_format=?" + std::to_string(versionParameter);
        }
        if (filterInterval) {
            logicalSql += " AND o.sample_interval=?" + std::to_string(intervalParameter);
        }
        logicalSql += order;
        auto logicalRows = Prepare(
            database.get(), logicalSql.c_str(), result.error);
        if (!logicalRows
            || !BindText(logicalRows.get(), 1, startText)
            || !BindText(logicalRows.get(), 2, endText)
            || (filterVersion
                && sqlite3_bind_int(logicalRows.get(), versionParameter,
                    rinexVersion == 2 ? 2 : 3) != SQLITE_OK)
            || (filterInterval
                && !BindText(logicalRows.get(), intervalParameter,
                    std::string(sampleInterval)))) {
            return result;
        }
        if (!consumeLogicalRows(logicalRows.get())) return result;
    } else {
        // The logical observation key starts with StationKey.  Seek the
        // existing (network,date,observation_key) primary key one UTC day at
        // a time; this avoids expanding every source route in a long range.
        struct KeyRange { std::string low; std::string high; };
        std::vector<KeyRange> ranges;
        std::unordered_set<std::string> seenPrefixes;
        const auto addPrefix = [&](std::string prefix) {
            if (!seenPrefixes.insert(prefix).second) return;
            std::string high = prefix;
            ++high.back();
            ranges.push_back({std::move(prefix), std::move(high)});
        };
        for (auto four : stationFourCharIds) {
            if (four.size() != 4U) continue;
            std::transform(four.begin(), four.end(), four.begin(),
                [](const unsigned char value) {
                    return static_cast<char>(std::toupper(value));
                });
            addPrefix(four);
            addPrefix("FOUR:" + four);
        }
        std::sort(ranges.begin(), ranges.end(),
            [](const KeyRange& left, const KeyRange& right) {
                return left.low < right.low;
            });

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
            std::string cte = "WITH wanted(lo,hi) AS (VALUES ";
            for (std::size_t index = 0; index < count; ++index) {
                if (index != 0) cte += ',';
                const auto& range = ranges[first + index];
                cte += '(' + sqlLiteral(range.low) + ','
                    + sqlLiteral(range.high) + ')';
            }
            // DuckDB's C API returns no rows for bound VALUES in this CTE
            // joined to a range predicate.  Quote the validated station
            // prefixes here; keep the date and user filters parameterized.
            const int dateParameter = 1;
            std::string sql = cte + ") " + projection
                + " FROM wanted w JOIN daily_observation_files o ON"
                  " o.network='IGS' AND o.observation_date=?"
                + std::to_string(dateParameter)
                + " AND o.observation_key>=w.lo"
                  " AND o.observation_key<w.hi"
                + joins + " WHERE 1=1";
            int nextParameter = dateParameter + 1;
            if (filterVersion) {
                sql += " AND o.rinex_format=?" + std::to_string(nextParameter++);
            }
            if (filterInterval) {
                sql += " AND o.sample_interval=?" + std::to_string(nextParameter++);
            }
            sql += order;
            auto statement = Prepare(database.get(), sql.c_str(), result.error);
            if (!statement) return result;
            int staticParameter = dateParameter + 1;
            if (filterVersion
                && sqlite3_bind_int(statement.get(), staticParameter++,
                    rinexVersion == 2 ? 2 : 3) != SQLITE_OK) {
                return result;
            }
            if (filterInterval
                && !BindText(statement.get(), staticParameter,
                    std::string(sampleInterval))) {
                return result;
            }
            statements.push_back({std::move(statement), dateParameter});
        }

        const std::size_t totalUnits = days.size() * statements.size();
        std::size_t completedUnits = 0;
        for (const auto& day : days) {
            for (auto& filtered : statements) {
                if (day.usable) {
                    sqlite3_reset(filtered.statement.get());
                    if (!BindText(filtered.statement.get(),
                            filtered.dateParameter, day.text)
                        || !consumeLogicalRows(filtered.statement.get())) {
                        return result;
                    }
                }
                ++completedUnits;
                if (progress) progress(completedUnits, totalUnits);
            }
        }
    }
    if (!flushCurrent()) return result;
    result.complete = true;
    return result;
}

IgsAvailabilityRangeLookup LoadIgsAvailabilityRange(
    const QueryDate& startDate,
    const QueryDate& endDate,
    const int rinexVersion,
    const std::string_view sampleInterval) {
    IgsAvailabilityRangeLookup lookup;
    lookup.databasePath = ResolveDatabasePath();
    const auto stream = StreamIgsAvailabilityRange(
        startDate,
        endDate,
        [&](DatedIndexedIgsAvailability&& dated) {
            lookup.records.push_back(std::move(dated));
            return true;
        },
        rinexVersion,
        sampleInterval);
    lookup.complete = stream.complete;
    lookup.expectedDays = stream.expectedDays;
    lookup.incompleteDates = stream.incompleteDates;
    lookup.databasePath = stream.databasePath;
    lookup.error = stream.error;
    return lookup;
}

bool StoreIgsAvailability(
    const QueryDate& date,
    const std::vector<IgsAvailabilitySourceScan>& sources,
    std::wstring& error) {
    std::scoped_lock databaseLock(gAvailabilityDatabaseMutex);
    error.clear();
    if (EnvironmentFlag(L"GNSS_DOWNLOADER_DISABLE_LOCAL_INDEX")) {
        return true;
    }

    struct Route {
        IndexedIgsAvailability record;
        std::string stationKey;
    };
    struct ObservationFileRoute {
        IndexedIgsAvailability record;
        std::string stationKey;
        std::string observationKey;
    };
    std::unordered_map<std::string, Route> bestByStationAndSource;
    std::vector<ObservationFileRoute> observationFileRoutes;
    int successfulSources = 0;
    for (const auto& source : sources) {
        if (!source.success) {
            continue;
        }
        ++successfulSources;
        for (auto record : source.records) {
            record.sourceKey = source.sourceKey;
            record.sourcePriority = source.sourcePriority;
            const std::string stationKey = StationKey(record.station);
            const std::string routeKey =
                stationKey + "\x1f" + source.sourceKey;
            const auto existing = bestByStationAndSource.find(routeKey);
            if (existing == bestByStationAndSource.end()
                || record.preferenceScore
                    > existing->second.record.preferenceScore) {
                bestByStationAndSource.insert_or_assign(
                    routeKey,
                    Route{std::move(record), stationKey});
            }
        }
        const auto& sourceFiles = source.files.empty()
            ? source.records
            : source.files;
        for (auto record : sourceFiles) {
            record.sourceKey = source.sourceKey;
            record.sourcePriority = source.sourcePriority;
            const std::string stationKey = StationKey(record.station);
            const std::string observationKey =
                ObservationFileKey(record.station);
            observationFileRoutes.push_back(
                ObservationFileRoute{
                    std::move(record),
                    stationKey,
                    observationKey});
        }
    }
    if (successfulSources == 0) {
        error = L"No IGS archive directory was read successfully.";
        return false;
    }

    std::vector<Route> routes;
    routes.reserve(bestByStationAndSource.size());
    for (auto& [routeKey, route] : bestByStationAndSource) {
        routes.push_back(std::move(route));
    }
    std::sort(
        routes.begin(),
        routes.end(),
        [](const Route& left, const Route& right) {
            if (left.stationKey != right.stationKey) {
                return left.stationKey < right.stationKey;
            }
            if (left.record.sourcePriority != right.record.sourcePriority) {
                return left.record.sourcePriority < right.record.sourcePriority;
            }
            return left.record.preferenceScore > right.record.preferenceScore;
        });

    struct PreferredRoute {
        IndexedIgsAvailability record;
        std::vector<std::string> sourceKeys;
        std::string stationKey;
    };
    std::vector<PreferredRoute> preferred;
    for (const auto& route : routes) {
        if (preferred.empty()
            || preferred.back().stationKey != route.stationKey) {
            preferred.push_back(
                PreferredRoute{
                    route.record,
                    {route.record.sourceKey},
                    route.stationKey});
            continue;
        }
        auto& item = preferred.back();
        item.sourceKeys.push_back(route.record.sourceKey);
        item.record.station.availableNetworks =
            static_cast<ObservationNetworkMask>(
                item.record.station.availableNetworks
                | route.record.station.availableNetworks);
        if (!item.record.station.hasCoordinate
            && route.record.station.hasCoordinate) {
            item.record.station.country = route.record.station.country;
            item.record.station.latitude = route.record.station.latitude;
            item.record.station.longitude = route.record.station.longitude;
            item.record.station.hasCoordinate = true;
        }
    }

    std::sort(
        observationFileRoutes.begin(),
        observationFileRoutes.end(),
        [](const ObservationFileRoute& left,
           const ObservationFileRoute& right) {
            if (left.observationKey != right.observationKey) {
                return left.observationKey < right.observationKey;
            }
            if (left.record.sourcePriority != right.record.sourcePriority) {
                return left.record.sourcePriority < right.record.sourcePriority;
            }
            return left.record.preferenceScore
                > right.record.preferenceScore;
        });
    struct PreferredObservationFile {
        IndexedIgsAvailability record;
        std::string stationKey;
        std::string observationKey;
        std::vector<std::string> sourceKeys;
    };
    std::vector<PreferredObservationFile> preferredObservationFiles;
    for (const auto& route : observationFileRoutes) {
        if (preferredObservationFiles.empty()
            || preferredObservationFiles.back().observationKey
                != route.observationKey) {
            preferredObservationFiles.push_back(
                PreferredObservationFile{
                    route.record,
                    route.stationKey,
                    route.observationKey,
                    {route.record.sourceKey}});
            continue;
        }
        auto& item = preferredObservationFiles.back();
        if (std::find(
                item.sourceKeys.begin(),
                item.sourceKeys.end(),
                route.record.sourceKey) == item.sourceKeys.end()) {
            item.sourceKeys.push_back(route.record.sourceKey);
        }
    }

    DatabasePtr database;
    const auto path = ResolveDatabasePath();
    if (!OpenDatabase(path, database, error)) {
        return false;
    }
    if (!Execute(database.get(), "BEGIN IMMEDIATE;", error)) {
        return false;
    }
    bool committed = false;
    const auto rollback = [&]() {
        if (!committed) {
            std::wstring ignored;
            Execute(database.get(), "ROLLBACK;", ignored);
        }
    };
    const std::string dateText = DateText(date);
    const bool provisional = IsProvisionalDate(date);
    // Never let a transient outage replace a more complete daily union.  A
    // partial retry is allowed to replace an older row only when it has more
    // successfully checked archives.  A full 7/7 refresh may replace 7/7.
    int existingSuccessfulSources = -1;
    {
        auto existing = Prepare(
            database.get(),
            "SELECT COUNT(*) FROM daily_source_scans"
            " WHERE network='IGS' AND observation_date=?1"
            " AND status='complete';",
            error);
        if (!existing || !BindText(existing.get(), 1, dateText)
            || sqlite3_step(existing.get()) != SQLITE_ROW) {
            if (error.empty()) {
                error = SqliteError(
                    database.get(),
                    L"Unable to compare IGS scan completeness");
            }
            rollback();
            return false;
        }
        existingSuccessfulSources = sqlite3_column_int(existing.get(), 0);
    }
    if (existingSuccessfulSources > successfulSources
        || (existingSuccessfulSources == successfulSources
            && successfulSources < 7)) {
        if (!Execute(database.get(), "ROLLBACK;", error)) {
            return false;
        }
        committed = true;
        return true;
    }
    for (const char* table :
         {"daily_files",
          "daily_observation_files",
          "daily_observation_file_sources",
          "daily_source_scans"}) {
        const std::string sql = std::string("DELETE FROM ") + table
            + " WHERE network='IGS' AND observation_date=?1;";
        auto statement = Prepare(database.get(), sql.c_str(), error);
        if (!statement || !BindText(statement.get(), 1, dateText)
            || sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (error.empty()) {
                error = SqliteError(
                    database.get(),
                    L"Unable to replace IGS source routes");
            }
            rollback();
            return false;
        }
    }

    auto scan = Prepare(
        database.get(),
        "INSERT INTO daily_scans(network,observation_date,status,"
        " file_count,is_provisional,scanned_at_utc)"
        " VALUES('IGS',?1,'complete',?2,?3,"
        " strftime('%Y-%m-%dT%H:%M:%SZ','now'))"
        " ON CONFLICT(network,observation_date) DO UPDATE SET"
        " status='complete',file_count=excluded.file_count,"
        " is_provisional=excluded.is_provisional,"
        " scanned_at_utc=excluded.scanned_at_utc;",
        error);
    if (!scan || !BindText(scan.get(), 1, dateText)
        || sqlite3_bind_int64(
               scan.get(),
               2,
               static_cast<sqlite3_int64>(preferred.size()))
            != SQLITE_OK
        || sqlite3_bind_int(scan.get(), 3, provisional ? 1 : 0) != SQLITE_OK
        || sqlite3_step(scan.get()) != SQLITE_DONE) {
        if (error.empty()) {
            error = SqliteError(database.get(), L"无法写入本地索引日期");
        }
        rollback();
        return false;
    }

    auto sourceScanInsert = Prepare(
        database.get(),
        "INSERT INTO daily_source_scans("
        " network,observation_date,source_id,status,data_url,file_count,"
        " station_count,error_message)"
        " VALUES('IGS',?1,?2,?3,?4,?5,?6,?7);",
        error);
    if (!sourceScanInsert) {
        rollback();
        return false;
    }
    for (const auto& source : sources) {
        sqlite3_reset(sourceScanInsert.get());
        sqlite3_clear_bindings(sourceScanInsert.get());
        const int sourceId = SourceId(source.sourceKey);
        const bool bound =
            BindText(sourceScanInsert.get(), 1, dateText)
            && sqlite3_bind_int(
                sourceScanInsert.get(),
                2,
                sourceId) == SQLITE_OK
            && BindText(
                sourceScanInsert.get(),
                3,
                source.success ? "complete" : "failed")
            && BindText(
                sourceScanInsert.get(),
                4,
                WideToUtf8(source.dataUrl))
            && sqlite3_bind_int64(
                sourceScanInsert.get(),
                5,
                static_cast<sqlite3_int64>(
                    source.files.empty()
                        ? source.records.size()
                        : source.files.size())) == SQLITE_OK
            && sqlite3_bind_int64(
                sourceScanInsert.get(),
                6,
                static_cast<sqlite3_int64>(source.records.size())) == SQLITE_OK
            && BindText(
                sourceScanInsert.get(),
                7,
                WideToUtf8(source.error));
        if (sourceId == 0 || !bound
            || sqlite3_step(sourceScanInsert.get()) != SQLITE_DONE) {
            error = SqliteError(
                database.get(),
                L"Unable to store IGS source scan");
            rollback();
            return false;
        }
    }
    std::unordered_map<std::string, std::wstring> sourceBaseUrls;
    for (const auto& source : sources) {
        sourceBaseUrls.insert_or_assign(source.sourceKey, source.dataUrl);
    }

    auto observationRouteInsert = Prepare(
        database.get(),
        "INSERT INTO daily_observation_file_sources("
        " network,observation_date,observation_key,source_id,file_name,"
        " url_override,preference_score)"
        " VALUES('IGS',?1,?2,?3,?4,?5,?6);",
        error);
    if (!observationRouteInsert) {
        rollback();
        return false;
    }
    for (const auto& route : observationFileRoutes) {
        sqlite3_reset(observationRouteInsert.get());
        sqlite3_clear_bindings(observationRouteInsert.get());
        const auto& station = route.record.station;
        const int sourceId = SourceId(route.record.sourceKey);
        const auto base = sourceBaseUrls.find(route.record.sourceKey);
        const std::string overrideUrl = UrlOverride(
            station.downloadUrl,
            base == sourceBaseUrls.end() ? std::wstring_view{} : base->second,
            station.fileName);
        const bool bound =
            BindText(observationRouteInsert.get(), 1, dateText)
            && BindText(
                observationRouteInsert.get(),
                2,
                route.observationKey)
            && sqlite3_bind_int(
                observationRouteInsert.get(),
                3,
                sourceId) == SQLITE_OK
            && BindText(observationRouteInsert.get(), 4, station.fileName)
            && BindText(observationRouteInsert.get(), 5, overrideUrl)
            && sqlite3_bind_int(
                observationRouteInsert.get(),
                6,
                route.record.preferenceScore) == SQLITE_OK;
        if (sourceId == 0 || !bound
            || sqlite3_step(observationRouteInsert.get()) != SQLITE_DONE) {
            error = SqliteError(
                database.get(),
                L"Unable to store IGS observation-file source route");
            rollback();
            return false;
        }
    }

    auto observationInsert = Prepare(
        database.get(),
        "INSERT INTO daily_observation_files("
        " network,observation_date,observation_key,station_key,"
        " canonical_file_name,rinex_format,sample_interval,"
        " preferred_source_id,source_mask)"
        " VALUES('IGS',?1,?2,?3,?4,?5,?6,?7,?8);",
        error);
    if (!observationInsert) {
        rollback();
        return false;
    }
    for (const auto& item : preferredObservationFiles) {
        sqlite3_reset(observationInsert.get());
        sqlite3_clear_bindings(observationInsert.get());
        const auto& station = item.record.station;
        const int preferredSourceId = SourceId(item.record.sourceKey);
        const bool bound =
            BindText(observationInsert.get(), 1, dateText)
            && BindText(observationInsert.get(), 2, item.observationKey)
            && BindText(observationInsert.get(), 3, item.stationKey)
            && BindText(observationInsert.get(), 4, station.fileName)
            && sqlite3_bind_int(
                observationInsert.get(),
                5,
                station.format == RinexFormat::Rinex2 ? 2 : 3) == SQLITE_OK
            && BindText(observationInsert.get(), 6, station.interval)
            && sqlite3_bind_int(
                observationInsert.get(),
                7,
                preferredSourceId) == SQLITE_OK
            && sqlite3_bind_int(
                observationInsert.get(),
                8,
                SourceMask(item.sourceKeys)) == SQLITE_OK;
        if (preferredSourceId == 0 || !bound
            || sqlite3_step(observationInsert.get()) != SQLITE_DONE) {
            error = SqliteError(
                database.get(),
                L"Unable to store IGS observation-file union");
            rollback();
            return false;
        }
    }

    auto insert = Prepare(
        database.get(),
        "INSERT OR REPLACE INTO daily_files("
        " network,observation_date,station_key,station_id,four_char_id,"
        " country,file_name,url_override,rinex_format,sample_interval,preference_score,"
        " available_networks,has_coordinate,latitude,longitude,"
        " preferred_source_id,source_mask)"
        " VALUES('IGS',?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,"
        " ?14,?15,?16);",
        error);
    if (!insert) {
        rollback();
        return false;
    }
    for (const auto& item : preferred) {
        sqlite3_reset(insert.get());
        sqlite3_clear_bindings(insert.get());
        const auto& record = item.record;
        const auto& station = record.station;
        const int preferredSourceId = SourceId(record.sourceKey);
        const auto base = sourceBaseUrls.find(record.sourceKey);
        const std::string overrideUrl = UrlOverride(
            station.downloadUrl,
            base == sourceBaseUrls.end() ? std::wstring_view{} : base->second,
            station.fileName);
        const bool bound =
            BindText(insert.get(), 1, dateText)
            && BindText(insert.get(), 2, item.stationKey)
            && BindText(insert.get(), 3, station.stationId)
            && BindText(insert.get(), 4, station.fourCharId)
            && BindText(insert.get(), 5, station.country)
            && BindText(insert.get(), 6, station.fileName)
            && BindText(insert.get(), 7, overrideUrl)
            && sqlite3_bind_int(
                insert.get(),
                8,
                station.format == RinexFormat::Rinex2 ? 2 : 3) == SQLITE_OK
            && BindText(insert.get(), 9, station.interval)
            && sqlite3_bind_int(insert.get(), 10, record.preferenceScore) == SQLITE_OK
            && sqlite3_bind_int64(
                insert.get(),
                11,
                static_cast<sqlite3_int64>(station.availableNetworks)) == SQLITE_OK
            && sqlite3_bind_int(
                insert.get(),
                12,
                station.hasCoordinate ? 1 : 0) == SQLITE_OK
            && (station.hasCoordinate
                ? sqlite3_bind_double(insert.get(), 13, station.latitude) == SQLITE_OK
                    && sqlite3_bind_double(insert.get(), 14, station.longitude) == SQLITE_OK
                : sqlite3_bind_null(insert.get(), 13) == SQLITE_OK
                    && sqlite3_bind_null(insert.get(), 14) == SQLITE_OK)
            && sqlite3_bind_int(
                insert.get(),
                15,
                preferredSourceId) == SQLITE_OK
            && sqlite3_bind_int(
                insert.get(),
                16,
                SourceMask(item.sourceKeys)) == SQLITE_OK;
        if (preferredSourceId == 0 || !bound
            || sqlite3_step(insert.get()) != SQLITE_DONE) {
            error = SqliteError(database.get(), L"无法写入本地索引文件");
            rollback();
            return false;
        }
    }
    if (!Execute(database.get(), "COMMIT;", error)) {
        rollback();
        return false;
    }
    committed = true;
    return true;
}

bool RemoveIgsAvailability(const QueryDate& date, std::wstring& error) {
    std::scoped_lock databaseLock(gAvailabilityDatabaseMutex);
    error.clear();
    DatabasePtr database;
    if (!OpenDatabase(ResolveDatabasePath(), database, error)) {
        return false;
    }
    auto statement = Prepare(
        database.get(),
        "DELETE FROM daily_scans WHERE network='IGS' AND observation_date=?1;",
        error);
    const std::string dateText = DateText(date);
    if (!statement || !BindText(statement.get(), 1, dateText)
        || sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (error.empty()) {
            error = SqliteError(database.get(), L"无法删除本地索引日期");
        }
        return false;
    }
    auto highrate = Prepare(
        database.get(),
        "DELETE FROM highrate_daily_scans"
        " WHERE network='IGS' AND observation_date=?1;",
        error);
    if (!highrate || !BindText(highrate.get(), 1, dateText)
        || sqlite3_step(highrate.get()) != SQLITE_DONE) {
        if (error.empty()) {
            error = SqliteError(database.get(), L"无法删除本地高频索引日期");
        }
        return false;
    }
    return true;
}

std::optional<IgsHighrateSourceScan> LoadIgsHighrateSourceCache(
    const QueryDate& date,
    const std::string_view sourceKey) {
    std::scoped_lock databaseLock(gAvailabilityDatabaseMutex);
    if (EnvironmentFlag(L"GNSS_DOWNLOADER_DISABLE_LOCAL_INDEX")) {
        return std::nullopt;
    }
    // The index builder's --refresh switch is used after parser or source
    // layout fixes.  Do not let an older cached empty/complete row hide data
    // that the new scanner can now recognize.
    if (EnvironmentFlag(L"GNSS_DOWNLOADER_FORCE_ONLINE_IGS")) {
        return std::nullopt;
    }
    const int sourceId = SourceId(sourceKey);
    if (sourceId == 0) return std::nullopt;
    DatabasePtr database;
    std::wstring ignored;
    if (!OpenReadOnlyDatabase(ResolveDatabasePath(), database, ignored)) {
        return std::nullopt;
    }
    const std::string dateText = DateText(date);
    auto state = Prepare(
        database.get(),
        "SELECT status,listing_url,file_count,error_message,scan_revision,station_count"
        " FROM highrate_daily_source_scans"
        " WHERE network='IGS' AND observation_date=?1 AND source_id=?2;",
        ignored);
    if (!state || !BindText(state.get(), 1, dateText)
        || sqlite3_bind_int(state.get(), 2, sourceId) != SQLITE_OK
        || sqlite3_step(state.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    const std::string status = ColumnText(state.get(), 0);
    if (status == "empty"
        && HighrateSourceShouldContainFiles(date, sourceKey)) {
        // A source-level empty result in a confirmed continuous window is
        // never treated as a durable cache hit.  Re-read the directory so a
        // transient network response or parser mismatch can be repaired.
        return std::nullopt;
    }
    // Entries from the pre-revision scanner are normally stale.  Preserve
    // the useful part of an existing database, however: when the aggregate
    // checkpoint explicitly records all four primary sources (including
    // legitimate empty/out-of-coverage sources), each corresponding legacy
    // row is still a trustworthy completed day and need not be rescanned.
    const int scanRevision = sqlite3_column_int(state.get(), 4);
    if (scanRevision != kHighrateScanRevision) {
        // Source coverage is date dependent: IGN/ESA are the only primary
        // mirrors expected before 2018, GFZ joins from 2018, and BKG from
        // 2020.  A legacy day is reusable when every source applicable to
        // that period has a completed checkpoint.
        const int expectedSources = date.year < 2018 ? 2
            : (date.year < 2020 ? 3 : 4);
        const std::string sourceIds = date.year < 2018 ? "5,6"
            : (date.year < 2020 ? "5,6,8" : "2,5,6,8");
        const std::string legacySql =
            "SELECT 1 FROM highrate_daily_scans ds "
            "WHERE ds.network='IGS' AND ds.observation_date=?1 "
            "  AND ds.status='complete' AND ds.source_count>="
                + std::to_string(expectedSources) +
            "  AND (SELECT COUNT(*) FROM highrate_daily_source_scans ss "
            "       WHERE ss.network='IGS' AND ss.observation_date=?1 "
            "         AND ss.source_id IN (" + sourceIds + ") "
            "         AND ss.status IN ('complete','empty'))>="
                + std::to_string(expectedSources) + " LIMIT 1;";
        auto legacy = Prepare(database.get(), legacySql.c_str(), ignored);
        if (!legacy || !BindText(legacy.get(), 1, dateText)
            || sqlite3_step(legacy.get()) != SQLITE_ROW) {
            return std::nullopt;
        }
    }
    if (status != "complete" && status != "empty") {
        return std::nullopt;
    }
    IgsHighrateSourceScan result;
    result.sourceKey = std::string(sourceKey);
    result.sourcePriority = sourceId;
    result.listingUrl = Utf8ToWide(ColumnText(state.get(), 1));
    result.fileCount = std::max(0, sqlite3_column_int(state.get(), 2));
    result.success = true;
    result.error = Utf8ToWide(ColumnText(state.get(), 3));
    result.outsideCoverage = result.error == L"OUTSIDE_COVERAGE";
    result.networkFailure = false;
    const int expectedStations = sqlite3_column_int(state.get(), 5);
    if (status == "empty") {
        return (result.fileCount == 0 && expectedStations == 0)
            ? std::optional<IgsHighrateSourceScan>{std::move(result)}
            : std::nullopt;
    }

    // Reconstruct the source view from the source-specific manifest rows.  The
    // compact observation row stores the maximum count across mirrors, so
    // reading its file_count here would inflate cached source totals.
    auto observations = Prepare(
        database.get(),
        "SELECT o.station_id,o.four_char_id,o.rinex_format_mask,s.file_count"
        " FROM highrate_daily_sources s"
        " JOIN highrate_daily_observations o"
        "   ON o.network=s.network"
        "  AND o.observation_date=s.observation_date"
        "  AND o.station_key=s.station_key"
        " WHERE s.network='IGS' AND s.observation_date=?1"
        " AND s.source_id=?2 AND s.status='complete';",
        ignored);
    if (!observations || !BindText(observations.get(), 1, dateText)
        || sqlite3_bind_int(observations.get(), 2, sourceId) != SQLITE_OK) {
        return std::nullopt;
    }
    int readStatus = SQLITE_OK;
    while ((readStatus = sqlite3_step(observations.get())) == SQLITE_ROW) {
        IgsHighrateStationScan station;
        station.stationId = ColumnText(observations.get(), 0);
        station.fourCharId = ColumnText(observations.get(), 1);
        station.rinexFormatMask = sqlite3_column_int(observations.get(), 2);
        station.fileCount = std::max(0, sqlite3_column_int(observations.get(), 3));
        result.stations.push_back(std::move(station));
    }
    // Older high-rate builds stored the aggregate observation rows but did
    // not preserve matching rows in highrate_daily_sources.  Recover the
    // source view from the observation source mask so a valid completed day
    // is not needlessly downloaded again after an upgrade.
    if (readStatus == SQLITE_DONE && result.stations.empty()) {
        auto fallback = Prepare(
            database.get(),
            "SELECT station_id,four_char_id,rinex_format_mask,file_count"
            " FROM highrate_daily_observations"
            " WHERE network='IGS' AND observation_date=?1"
            "   AND (source_mask & ?2)<>0;",
            ignored);
        if (fallback && BindText(fallback.get(), 1, dateText)
            && sqlite3_bind_int(fallback.get(), 2, SourceBit(sourceId)) == SQLITE_OK) {
            while (sqlite3_step(fallback.get()) == SQLITE_ROW) {
                IgsHighrateStationScan station;
                station.stationId = ColumnText(fallback.get(), 0);
                station.fourCharId = ColumnText(fallback.get(), 1);
                station.rinexFormatMask = sqlite3_column_int(fallback.get(), 2);
                station.fileCount = std::max(0, sqlite3_column_int(fallback.get(), 3));
                result.stations.push_back(std::move(station));
            }
        }
    }
    // A complete state without any station rows is indistinguishable from an
    // incomplete legacy write.  The stored station_count can legitimately
    // differ after catalogue/parser updates, however; requiring an exact
    // match would turn otherwise valid four-source days into network scans on
    // every run.  The source checkpoint and non-empty manifest are sufficient
    // evidence for cache reuse.
    if (readStatus != SQLITE_DONE || result.stations.empty()) {
        return std::nullopt;
    }
    return result;
}

bool StoreIgsHighrateAvailability(
    const QueryDate& date,
    const std::vector<IgsHighrateSourceScan>& sources,
    std::wstring& error) {
    std::scoped_lock databaseLock(gAvailabilityDatabaseMutex);
    error.clear();
    if (EnvironmentFlag(L"GNSS_DOWNLOADER_DISABLE_LOCAL_INDEX")) {
        return true;
    }

    struct Aggregate {
        std::string stationId;
        std::string fourCharId;
        int rinexFormatMask = 0;
        int fileCount = 0;
        int sourceMask = 0;
    };
    struct SourceRow {
        std::string stationKey;
        int sourceId = 0;
        std::wstring listingUrl;
        int fileCount = 0;
    };
    std::unordered_map<std::string, Aggregate> aggregates;
    std::vector<SourceRow> sourceRows;
    int successfulSources = 0;
    int totalFiles = 0;
    for (const auto& source : sources) {
        if (!source.success) continue;
        const int sourceId = SourceId(source.sourceKey);
        if (sourceId == 0) continue;
        ++successfulSources;
        totalFiles += source.fileCount;
        for (const auto& station : source.stations) {
            // RINEX 2 high-rate names carry only a four-character ID while
            // the corresponding RINEX 3 files carry a nine-character ID.
            // Use the four-character monument key so both formats become one
            // station/day summary instead of two false stations.
            const std::string stationKey =
                std::string("FOUR:") + station.fourCharId;
            auto& aggregate = aggregates[stationKey];
            if (aggregate.stationId.empty() || station.stationId.size() == 9) {
                aggregate.stationId = station.stationId;
            }
            aggregate.fourCharId = station.fourCharId;
            aggregate.rinexFormatMask |= station.rinexFormatMask;
            // The same physical 15-minute files are usually mirrored by
            // several archives.  Keep the largest per-source count as a
            // useful estimate instead of multiplying it by the number of
            // mirrors.
            aggregate.fileCount = std::max(
                aggregate.fileCount,
                station.fileCount);
            aggregate.sourceMask |= SourceBit(sourceId);
            sourceRows.push_back(SourceRow{
                stationKey,
                sourceId,
                source.listingUrl,
                station.fileCount});
        }
    }
    if (successfulSources == 0) {
        error = L"No IGS high-rate archive directory was read successfully.";
        return false;
    }

    DatabasePtr database;
    const auto path = ResolveDatabasePath();
    if (!OpenDatabase(path, database, error)) return false;
    if (!Execute(database.get(), "BEGIN IMMEDIATE;", error)) return false;
    bool committed = false;
    const auto rollback = [&]() {
        if (!committed) {
            std::wstring ignored;
            Execute(database.get(), "ROLLBACK;", ignored);
        }
    };
    const std::string dateText = DateText(date);
    // Capture previous high-rate counts before replacing this date.
    int oldStationCount = 0;
    int oldFileCount = 0;
    struct OldSource { int stations = 0; int files = 0; std::string status; };
    std::unordered_map<int, OldSource> oldSources;
    {
        auto previous = Prepare(database.get(),
            "SELECT COUNT(*),COALESCE(SUM(file_count),0) FROM highrate_daily_observations"
            " WHERE network='IGS' AND observation_date=?1;", error);
        if (!previous || !BindText(previous.get(), 1, dateText)
            || sqlite3_step(previous.get()) != SQLITE_ROW) {
            if (error.empty()) error = SqliteError(database.get(), L"Unable to read previous high-rate manifest");
            rollback();
            return false;
        }
        oldStationCount = sqlite3_column_int(previous.get(), 0);
        oldFileCount = sqlite3_column_int(previous.get(), 1);
        auto previousSources = Prepare(database.get(),
            "SELECT source_id,station_count,file_count,status FROM highrate_daily_source_scans"
            " WHERE network='IGS' AND observation_date=?1;", error);
        if (!previousSources || !BindText(previousSources.get(), 1, dateText)) { rollback(); return false; }
        while (sqlite3_step(previousSources.get()) == SQLITE_ROW) {
            OldSource row;
            row.stations = sqlite3_column_int(previousSources.get(), 1);
            row.files = sqlite3_column_int(previousSources.get(), 2);
            row.status = ColumnText(previousSources.get(), 3);
            oldSources.emplace(sqlite3_column_int(previousSources.get(), 0), std::move(row));
        }
    }
    const bool completeSourceSet = sources.size() >= 4
        && std::all_of(sources.begin(), sources.end(),
            [](const auto& source) { return source.success; });
    if (completeSourceSet) {
        for (const char* table : {
                 "highrate_daily_observations",
                 "highrate_daily_sources",
                 "highrate_daily_source_scans"}) {
            const std::string sql = std::string("DELETE FROM ") + table
                + " WHERE network='IGS' AND observation_date=?1;";
            auto statement = Prepare(database.get(), sql.c_str(), error);
            if (!statement || !BindText(statement.get(), 1, dateText)
                || sqlite3_step(statement.get()) != SQLITE_DONE) {
                if (error.empty()) {
                    error = SqliteError(
                        database.get(),
                        L"Unable to replace IGS high-rate availability");
                }
                rollback();
                return false;
            }
        }
    } else {
        // Refresh successful mirrors independently. Failed mirrors retain
        // their previous routes and can be retried on the next run.
        for (const auto& source : sources) {
            if (!source.success) continue;
            const int sourceId = SourceId(source.sourceKey);
            const int sourceBit = SourceBit(sourceId);
            auto clear = Prepare(database.get(),
                "UPDATE highrate_daily_observations"
                " SET source_mask=source_mask & ?2"
                " WHERE network='IGS' AND observation_date=?1;", error);
            auto remove = Prepare(database.get(),
                "DELETE FROM highrate_daily_sources"
                " WHERE network='IGS' AND observation_date=?1 AND source_id=?2;", error);
            if (!clear || !remove || !BindText(clear.get(), 1, dateText)
                || sqlite3_bind_int(clear.get(), 2, ~sourceBit) != SQLITE_OK
                || sqlite3_step(clear.get()) != SQLITE_DONE
                || !BindText(remove.get(), 1, dateText)
                || sqlite3_bind_int(remove.get(), 2, sourceId) != SQLITE_OK
                || sqlite3_step(remove.get()) != SQLITE_DONE) {
                if (error.empty()) error = SqliteError(
                    database.get(), L"Unable to refresh IGS high-rate source");
                rollback();
                return false;
            }
        }
    }

    auto scan = Prepare(
        database.get(),
        "INSERT INTO highrate_daily_scans(network,observation_date,status,"
        "source_count,file_count,scanned_at_utc)"
        " VALUES('IGS',?1,'complete',?2,?3,"
        "strftime('%Y-%m-%dT%H:%M:%SZ','now'))"
        " ON CONFLICT(network,observation_date) DO UPDATE SET"
        " status='complete',source_count=excluded.source_count,"
        " file_count=excluded.file_count,"
        " scanned_at_utc=excluded.scanned_at_utc;",
        error);
    if (!scan
        || !BindText(scan.get(), 1, dateText)
        || sqlite3_bind_int(scan.get(), 2, successfulSources) != SQLITE_OK
        || sqlite3_bind_int(scan.get(), 3, totalFiles) != SQLITE_OK
        || sqlite3_step(scan.get()) != SQLITE_DONE) {
        if (error.empty()) {
            error = SqliteError(
                database.get(),
                L"Unable to store IGS high-rate scan");
        }
        rollback();
        return false;
    }

    auto sourceInsert = Prepare(
        database.get(),
        "INSERT OR REPLACE INTO highrate_daily_sources("
        "network,observation_date,station_key,source_id,listing_url,"
        "file_count,status,error_message)"
        " VALUES('IGS',?1,?2,?3,?4,?5,'complete','');",
        error);
    if (!sourceInsert) {
        rollback();
        return false;
    }
    for (const auto& row : sourceRows) {
        sqlite3_reset(sourceInsert.get());
        sqlite3_clear_bindings(sourceInsert.get());
        if (!BindText(sourceInsert.get(), 1, dateText)
            || !BindText(sourceInsert.get(), 2, row.stationKey)
            || sqlite3_bind_int(sourceInsert.get(), 3, row.sourceId) != SQLITE_OK
            || !BindText(sourceInsert.get(), 4, WideToUtf8(row.listingUrl))
            || sqlite3_bind_int(sourceInsert.get(), 5, row.fileCount) != SQLITE_OK
            || sqlite3_step(sourceInsert.get()) != SQLITE_DONE) {
            error = SqliteError(
                database.get(),
                L"Unable to store IGS high-rate source route");
            rollback();
            return false;
        }
    }

    auto stateInsert = Prepare(
        database.get(),
        "INSERT INTO highrate_daily_source_scans("
        "network,observation_date,source_id,status,listing_url,file_count,"
        "station_count,error_message,scan_revision,scanned_at_utc)"
        " VALUES('IGS',?1,?2,?3,?4,?5,?6,?7,?8,"
        "strftime('%Y-%m-%dT%H:%M:%SZ','now'))"
        " ON CONFLICT(network,observation_date,source_id) DO UPDATE SET"
        " status=excluded.status,listing_url=excluded.listing_url,"
        " file_count=excluded.file_count,station_count=excluded.station_count,"
        " error_message=excluded.error_message,scan_revision=excluded.scan_revision,"
        " scanned_at_utc=excluded.scanned_at_utc;",
        error);
    if (!stateInsert) {
        rollback();
        return false;
    }
    for (const auto& source : sources) {
        const int sourceId = SourceId(source.sourceKey);
        if (sourceId == 0) continue;
        const char* status = source.success
            ? (source.stations.empty() ? "empty" : "complete")
            : "failed";
        sqlite3_reset(stateInsert.get());
        sqlite3_clear_bindings(stateInsert.get());
        if (!BindText(stateInsert.get(), 1, dateText)
            || sqlite3_bind_int(stateInsert.get(), 2, sourceId) != SQLITE_OK
            || !BindText(stateInsert.get(), 3, status)
            || !BindText(stateInsert.get(), 4, WideToUtf8(source.listingUrl))
            || sqlite3_bind_int(stateInsert.get(), 5, source.fileCount) != SQLITE_OK
            || sqlite3_bind_int(stateInsert.get(), 6,
                static_cast<int>(source.stations.size())) != SQLITE_OK
            || !BindText(stateInsert.get(), 7, WideToUtf8(source.error))
            || sqlite3_bind_int(stateInsert.get(), 8, kHighrateScanRevision) != SQLITE_OK
            || sqlite3_step(stateInsert.get()) != SQLITE_DONE) {
            error = SqliteError(database.get(),
                L"Unable to store IGS high-rate source checkpoint");
            rollback();
            return false;
        }
    }

    auto observationInsert = Prepare(
        database.get(),
        "INSERT INTO highrate_daily_observations("
        "network,observation_date,station_key,station_id,four_char_id,"
        "rinex_format_mask,sample_interval,has_data,file_count,source_mask)"
        " VALUES('IGS',?1,?2,?3,?4,?5,'01S',1,?6,?7)"
        " ON CONFLICT(network,observation_date,station_key) DO UPDATE SET"
        " station_id=CASE WHEN excluded.station_id<>'' THEN excluded.station_id"
        " ELSE highrate_daily_observations.station_id END,"
        " four_char_id=CASE WHEN excluded.four_char_id<>'' THEN excluded.four_char_id"
        " ELSE highrate_daily_observations.four_char_id END,"
        " rinex_format_mask=highrate_daily_observations.rinex_format_mask"
        " | excluded.rinex_format_mask,"
        " file_count=MAX(highrate_daily_observations.file_count,excluded.file_count),"
        " source_mask=highrate_daily_observations.source_mask | excluded.source_mask;",
        error);
    if (!observationInsert) {
        rollback();
        return false;
    }
    std::vector<std::string> keys;
    keys.reserve(aggregates.size());
    for (const auto& [key, unused] : aggregates) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys) {
        const auto& row = aggregates.at(key);
        sqlite3_reset(observationInsert.get());
        sqlite3_clear_bindings(observationInsert.get());
        if (!BindText(observationInsert.get(), 1, dateText)
            || !BindText(observationInsert.get(), 2, key)
            || !BindText(observationInsert.get(), 3, row.stationId)
            || !BindText(observationInsert.get(), 4, row.fourCharId)
            || sqlite3_bind_int(
                   observationInsert.get(), 5, row.rinexFormatMask) != SQLITE_OK
            || sqlite3_bind_int(observationInsert.get(), 6, row.fileCount) != SQLITE_OK
            || sqlite3_bind_int(observationInsert.get(), 7, row.sourceMask) != SQLITE_OK
            || sqlite3_step(observationInsert.get()) != SQLITE_DONE) {
            error = SqliteError(
                database.get(),
                L"Unable to store IGS high-rate station summary");
            rollback();
            return false;
        }
    }
    if (!completeSourceSet) {
        auto cleanup = Prepare(database.get(),
            "DELETE FROM highrate_daily_observations"
            " WHERE network='IGS' AND observation_date=?1 AND source_mask=0;",
            error);
        if (!cleanup || !BindText(cleanup.get(), 1, dateText)
            || sqlite3_step(cleanup.get()) != SQLITE_DONE) {
            if (error.empty()) error = SqliteError(
                database.get(), L"Unable to clean stale high-rate rows");
            rollback();
            return false;
        }
    }
    // Keep a compact before/after audit row so a small corrective rescan can
    // be compared with the values that were present before replacement.
    auto diff = Prepare(database.get(),
        "INSERT INTO highrate_rescan_diffs(network,observation_date,source_id,"
        "old_station_count,new_station_count,old_file_count,new_file_count,"
        "old_status,new_status,recorded_at_utc) VALUES('IGS',?1,0,?2,?3,?4,?5,'',?6,"
        "strftime('%Y-%m-%dT%H:%M:%SZ','now'));", error);
    if (!diff
        || !BindText(diff.get(), 1, dateText)
        || sqlite3_bind_int(diff.get(), 2, oldStationCount) != SQLITE_OK
        || sqlite3_bind_int(diff.get(), 3, static_cast<int>(aggregates.size())) != SQLITE_OK
        || sqlite3_bind_int(diff.get(), 4, oldFileCount) != SQLITE_OK
        || sqlite3_bind_int(diff.get(), 5, totalFiles) != SQLITE_OK
        || !BindText(diff.get(), 6, completeSourceSet ? "complete" : "partial")
        || sqlite3_step(diff.get()) != SQLITE_DONE) {
        if (error.empty()) error = SqliteError(
            database.get(), L"Unable to store high-rate rescan comparison");
        rollback();
        return false;
    }
    if (!Execute(database.get(), "COMMIT;", error)) {
        rollback();
        return false;
    }
    committed = true;
    return true;
}

bool IsIgsHighrateDateComplete(const QueryDate& date) {
    std::scoped_lock databaseLock(gAvailabilityDatabaseMutex);
    if (EnvironmentFlag(L"GNSS_DOWNLOADER_DISABLE_LOCAL_INDEX")
        || EnvironmentFlag(L"GNSS_DOWNLOADER_FORCE_ONLINE_IGS")) {
        return false;
    }
    DatabasePtr database;
    std::wstring ignored;
    if (!OpenReadOnlyDatabase(ResolveDatabasePath(), database, ignored)) {
        return false;
    }
    const int expectedSources = date.year < 2018 ? 2
        : (date.year < 2020 ? 3 : 4);
    const std::string sourceIds = date.year < 2018 ? "5,6"
        : (date.year < 2020 ? "5,6,8" : "2,5,6,8");
    const std::string completenessSql =
        "SELECT 1 FROM highrate_daily_scans "
        "WHERE network='IGS' AND observation_date=?1 "
        "AND status='complete' AND source_count>="
            + std::to_string(expectedSources) +
        "AND (SELECT COUNT(*) FROM highrate_daily_source_scans ss "
        " WHERE ss.network='IGS' AND ss.observation_date=?1 "
        "   AND ss.source_id IN (" + sourceIds + ") "
        "   AND ss.status IN ('complete','empty'))>="
            + std::to_string(expectedSources) + " LIMIT 1;";
    auto statement = Prepare(database.get(), completenessSql.c_str(), ignored);
    if (!statement || !BindText(statement.get(), 1, DateText(date))) {
        return false;
    }
    return sqlite3_step(statement.get()) == SQLITE_ROW;
}

IgsAvailabilityStats GetIgsAvailabilityStats() {
    std::scoped_lock databaseLock(gAvailabilityDatabaseMutex);
    IgsAvailabilityStats result;
    result.databasePath = ResolveDatabasePath();
    DatabasePtr database;
    if (!OpenReadOnlyDatabase(result.databasePath, database, result.error)) {
        return result;
    }
    auto statement = Prepare(
        database.get(),
        "SELECT SUM(CASE WHEN is_provisional=0 AND"
        " (SELECT COUNT(*) FROM daily_source_scans ss"
        "   WHERE ss.network=daily_scans.network"
        "     AND ss.observation_date=daily_scans.observation_date"
        "     AND ss.status='complete')=7 THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN is_provisional<>0 OR"
        " (SELECT COUNT(*) FROM daily_source_scans ss"
        "   WHERE ss.network=daily_scans.network"
        "     AND ss.observation_date=daily_scans.observation_date"
        "     AND ss.status='complete')<>7 THEN 1 ELSE 0 END),"
        " COALESCE(SUM(file_count),0),"
        " COALESCE(MIN(observation_date),''),COALESCE(MAX(observation_date),'')"
        " FROM daily_scans WHERE network='IGS' AND status='complete';",
        result.error);
    if (!statement || sqlite3_step(statement.get()) != SQLITE_ROW) {
        if (result.error.empty()) {
            result.error = SqliteError(database.get(), L"无法统计本地索引");
        }
        return result;
    }
    result.completeDays = sqlite3_column_int(statement.get(), 0);
    result.provisionalDays = sqlite3_column_int(statement.get(), 1);
    result.files = sqlite3_column_int64(statement.get(), 2);
    result.earliestDate = ColumnText(statement.get(), 3);
    result.latestDate = ColumnText(statement.get(), 4);
    return result;
}

IgsAvailabilityDateStats GetIgsAvailabilityDateStats(
    const QueryDate& date) {
    std::scoped_lock databaseLock(gAvailabilityDatabaseMutex);
    IgsAvailabilityDateStats result;
    result.databasePath = ResolveDatabasePath();
    DatabasePtr database;
    if (!OpenReadOnlyDatabase(result.databasePath, database, result.error)) {
        return result;
    }
    const std::string dateText = DateText(date);
    auto totals = Prepare(
        database.get(),
        "SELECT COUNT(*),"
        " COALESCE(SUM(CASE WHEN source_mask<>0"
        "  AND (source_mask & (source_mask-1))<>0 THEN 1 ELSE 0 END),0)"
        " FROM (SELECT station_key,MAX(source_mask) AS source_mask"
        "       FROM daily_files"
        "       WHERE network='IGS' AND observation_date=?1"
        "       GROUP BY station_key);",
        result.error);
    if (!totals || !BindText(totals.get(), 1, dateText)
        || sqlite3_step(totals.get()) != SQLITE_ROW) {
        if (result.error.empty()) {
            result.error = SqliteError(
                database.get(),
                L"Unable to summarize the IGS daily union");
        }
        return result;
    }
    result.uniqueStations = sqlite3_column_int(totals.get(), 0);
    result.multiSourceStations = sqlite3_column_int(totals.get(), 1);

    auto routes = Prepare(
        database.get(),
        "SELECT COALESCE(SUM("
        " ((source_mask & 1)<>0)+((source_mask & 2)<>0)+"
        " ((source_mask & 4)<>0)+((source_mask & 8)<>0)+"
        " ((source_mask & 16)<>0)+((source_mask & 32)<>0)+"
        " ((source_mask & 64)<>0)),0)"
        " FROM (SELECT station_key,MAX(source_mask) AS source_mask"
        "       FROM daily_files"
        "       WHERE network='IGS' AND observation_date=?1"
        "       GROUP BY station_key);",
        result.error);
    if (!routes || !BindText(routes.get(), 1, dateText)
        || sqlite3_step(routes.get()) != SQLITE_ROW) {
        if (result.error.empty()) {
            result.error = SqliteError(
                database.get(),
                L"Unable to summarize IGS source routes");
        }
        return result;
    }
    result.sourceRoutes = sqlite3_column_int(routes.get(), 0);

    auto observationFiles = Prepare(
        database.get(),
        "SELECT COUNT(*),COALESCE(SUM("
        " ((source_mask & 1)<>0)+((source_mask & 2)<>0)+"
        " ((source_mask & 4)<>0)+((source_mask & 8)<>0)+"
        " ((source_mask & 16)<>0)+((source_mask & 32)<>0)+"
        " ((source_mask & 64)<>0)),0),"
        " COALESCE(SUM(CASE WHEN source_mask<>0"
        "  AND (source_mask & (source_mask-1))<>0 THEN 1 ELSE 0 END),0),"
        " (SELECT COUNT(*) FROM daily_observation_file_sources r"
        "   WHERE r.network='IGS' AND r.observation_date=?1)"
        " FROM daily_observation_files"
        " WHERE network='IGS' AND observation_date=?1;",
        result.error);
    if (!observationFiles
        || !BindText(observationFiles.get(), 1, dateText)
        || sqlite3_step(observationFiles.get()) != SQLITE_ROW) {
        if (result.error.empty()) {
            result.error = SqliteError(
                database.get(),
                L"Unable to summarize IGS observation-file union");
        }
        return result;
    }
    result.logicalObservationFiles =
        sqlite3_column_int(observationFiles.get(), 0);
    result.observationSourceRoutes =
        sqlite3_column_int(observationFiles.get(), 1);
    result.multiSourceObservationFiles =
        sqlite3_column_int(observationFiles.get(), 2);
    result.physicalFileRoutes =
        sqlite3_column_int(observationFiles.get(), 3);

    auto sourceRows = Prepare(
        database.get(),
        "SELECT d.source_key,d.source_priority,s.status,s.file_count,"
        " s.station_count,s.data_url,s.error_message,"
        " (SELECT COUNT(DISTINCT f.station_key) FROM daily_files f"
        "   WHERE f.network=s.network"
        "     AND f.observation_date=s.observation_date"
        "     AND f.preferred_source_id=s.source_id),"
        " (SELECT COUNT(DISTINCT f.station_key) FROM daily_files f"
        "   WHERE f.network=s.network"
        "     AND f.observation_date=s.observation_date"
        "     AND f.preferred_source_id=s.source_id"
        "     AND f.source_mask=(1 << (s.source_id-1)))"
        " FROM daily_source_scans s"
        " JOIN source_definitions d ON d.source_id=s.source_id"
        " WHERE s.network='IGS' AND s.observation_date=?1"
        " ORDER BY d.source_priority,d.source_key;",
        result.error);
    if (!sourceRows || !BindText(sourceRows.get(), 1, dateText)) {
        if (result.error.empty()) {
            result.error = SqliteError(
                database.get(),
                L"Unable to read IGS source summaries");
        }
        return result;
    }
    while (true) {
        const int status = sqlite3_step(sourceRows.get());
        if (status == SQLITE_DONE) {
            break;
        }
        if (status != SQLITE_ROW) {
            result.error = SqliteError(
                database.get(),
                L"Unable to read IGS source summary row");
            result.sources.clear();
            return result;
        }
        IgsAvailabilitySourceStats source;
        source.sourceKey = ColumnText(sourceRows.get(), 0);
        source.sourcePriority = sqlite3_column_int(sourceRows.get(), 1);
        source.complete = ColumnText(sourceRows.get(), 2) == "complete";
        source.files = sqlite3_column_int(sourceRows.get(), 3);
        source.stations = sqlite3_column_int(sourceRows.get(), 4);
        source.listingUrl = Utf8ToWide(ColumnText(sourceRows.get(), 5));
        source.error = Utf8ToWide(ColumnText(sourceRows.get(), 6));
        source.preferredStations = sqlite3_column_int(sourceRows.get(), 7);
        source.exclusiveStations = sqlite3_column_int(sourceRows.get(), 8);
        result.sources.push_back(std::move(source));
    }
    return result;
}
