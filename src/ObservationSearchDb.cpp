#include "ObservationSearchDb.h"
#include "ObservationStationDates.h"

#include "DateUtil.h"
#include "IgsAvailabilityDb.h"
#include "SiteLogDb.h"
#include "SpaceWeather.h"

#include "sqlite3.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// Version 3 adds Site-Log coordinate repair to the derived station rows.  An
// older index is still structurally readable, but its station coordinates can
// be incomplete; force a rebuild instead of silently serving that snapshot.
// v4 rebuilds dated equipment segments after repairing parameterized DuckDB
// column decoding. Older indexes can silently contain current-only snapshots.
constexpr int kSchemaVersion = 4;
constexpr int kBitsPerBlock = 4096;
constexpr int kWordsPerBlock = kBitsPerBlock / 64;
constexpr std::size_t kExpectedBitmapBytes = kWordsPerBlock * sizeof(std::uint64_t);

struct SourceSpec {
    ObservationNetwork network;
    const wchar_t* fileName;
    const wchar_t* label;
    bool igs = false;
};

constexpr std::array kSourceSpecs{
    SourceSpec{ObservationNetwork::Igs, L"gnss_availability.sqlite", L"IGS/MGEX", true},
    SourceSpec{ObservationNetwork::PanamaCors, L"pan_cors_availability.sqlite", L"PAN-CORS"},
    SourceSpec{ObservationNetwork::Cut, L"cut_availability.sqlite", L"CUT"},
    SourceSpec{ObservationNetwork::Ngs, L"ngs_availability.sqlite", L"NGS"},
    SourceSpec{ObservationNetwork::Epn, L"epn_availability.sqlite", L"EPN"},
    SourceSpec{ObservationNetwork::GaGdc, L"ga_gdc_availability.sqlite", L"GA-GDC"},
    SourceSpec{ObservationNetwork::Rgp, L"rgp_availability.sqlite", L"RGP"},
    SourceSpec{ObservationNetwork::Rgp, L"rgp_highrate_availability.sqlite", L"RGP 1 Hz"},
    SourceSpec{ObservationNetwork::Sonel, L"sonel_availability.sqlite", L"SONEL"},
    SourceSpec{ObservationNetwork::Hk, L"hk_availability.sqlite", L"HK"},
    SourceSpec{ObservationNetwork::TrigNet, L"trignet_availability.sqlite", L"TRIGNET"},
    SourceSpec{ObservationNetwork::Rgna, L"rgna_availability.sqlite", L"RGNA"},
    SourceSpec{ObservationNetwork::Csn, L"csn_availability.sqlite", L"CSN"},
    SourceSpec{ObservationNetwork::EposIceland, L"epos_is_availability.sqlite", L"EPOS-IS"},
    SourceSpec{ObservationNetwork::Rbmc, L"ibge_rbmc_availability.sqlite", L"IBGE-RBMC"},
    SourceSpec{ObservationNetwork::Ramsac, L"ign_ramsac_availability.sqlite", L"IGN-RAMSAC"},
    SourceSpec{ObservationNetwork::RegnaRou, L"regna_rou_availability.sqlite", L"REGNA-ROU"},
    SourceSpec{ObservationNetwork::MagnaEco, L"magna_eco_availability.sqlite", L"MAGNA-ECO"},
    SourceSpec{ObservationNetwork::EposGnss, L"epos_gnss_availability.sqlite", L"EPOS-GNSS"},
    SourceSpec{ObservationNetwork::Gref, L"gref_availability.sqlite", L"GREF"},
    SourceSpec{ObservationNetwork::OsNet, L"os_net_availability.sqlite", L"OS-NET"},
    SourceSpec{ObservationNetwork::Ergnss, L"ergnss_availability.sqlite", L"ERGNSS"},
    SourceSpec{ObservationNetwork::Renep, L"renep_availability.sqlite", L"RENEP"},
    SourceSpec{ObservationNetwork::Cacs, L"cacs_nrcan_availability.sqlite", L"CACS-NRCAN"},
    SourceSpec{ObservationNetwork::GeoNetNz, L"geonet_nz_availability.sqlite", L"GEONET-NZ"},
    SourceSpec{ObservationNetwork::SopacGarner, L"sopac_garner_availability.sqlite", L"SOPAC-GARNER"},
    SourceSpec{ObservationNetwork::Ring, L"ring_availability.sqlite", L"RING"},
    SourceSpec{ObservationNetwork::Apos, L"apos_availability.sqlite", L"APOS"},
    SourceSpec{ObservationNetwork::RobBelgium, L"rob_belgium_availability.sqlite", L"ROB-BELGIUM"},
    SourceSpec{ObservationNetwork::Dpga, L"dpga_availability.sqlite", L"DPGA"},
    SourceSpec{ObservationNetwork::CatNet, L"catnet_availability.sqlite", L"CATNET"},
    SourceSpec{ObservationNetwork::SpsLux, L"spslux_availability.sqlite", L"SPSLUX"},
    SourceSpec{ObservationNetwork::KasiNgii, L"kasi_ngii_availability.sqlite", L"KASI-NGII"},
    SourceSpec{ObservationNetwork::Renag, L"renag_availability.sqlite", L"RENAG"},
    SourceSpec{ObservationNetwork::Nsgi, L"nsgi_availability.sqlite", L"NSGI"},
    SourceSpec{ObservationNetwork::NoaGnss, L"noa_gnss_availability.sqlite", L"NOA-GNSS"},
    SourceSpec{ObservationNetwork::Redgae, L"redgae_availability.sqlite", L"REDGAE"},
    SourceSpec{ObservationNetwork::GeoNetJapan, L"geonet_japan_availability.sqlite", L"GEONET-JP"},
    SourceSpec{ObservationNetwork::Dlab, L"dlab_availability.sqlite", L"DLAB"},
    SourceSpec{ObservationNetwork::Magnet, L"magnet_availability.sqlite", L"MAGNET"},
    SourceSpec{ObservationNetwork::PangaCwu, L"panga_cwu_availability.sqlite", L"PANGA-CWU"},
};

struct DatabaseCloser {
    void operator()(sqlite3* value) const noexcept {
        if (value != nullptr) sqlite3_close(value);
    }
};

struct StatementCloser {
    void operator()(sqlite3_stmt* value) const noexcept {
        if (value != nullptr) sqlite3_finalize(value);
    }
};

using DatabasePtr = std::unique_ptr<sqlite3, DatabaseCloser>;
using StatementPtr = std::unique_ptr<sqlite3_stmt, StatementCloser>;

std::string WideToUtf8(const std::wstring_view value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), required, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string_view value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), required);
    return result;
}

std::string PathToUtf8(const std::filesystem::path& path) {
    return WideToUtf8(path.wstring());
}

std::string ColumnText(sqlite3_stmt* statement, const int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value == nullptr
        ? std::string{}
        : std::string(reinterpret_cast<const char*>(value));
}

std::wstring SqliteError(sqlite3* database, const std::wstring_view action) {
    return std::wstring(action) + L"："
        + Utf8ToWide(database == nullptr ? "unknown" : sqlite3_errmsg(database));
}

bool Execute(sqlite3* database, const char* sql, std::wstring& error) {
    char* message = nullptr;
    if (sqlite3_exec(database, sql, nullptr, nullptr, &message) == SQLITE_OK) {
        return true;
    }
    error = L"SQLite：" + Utf8ToWide(
        message == nullptr ? sqlite3_errmsg(database) : message);
    sqlite3_free(message);
    return false;
}

StatementPtr Prepare(
    sqlite3* database,
    const char* sql,
    std::wstring& error) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK) {
        error = SqliteError(database, L"准备SQL失败");
        return {};
    }
    return StatementPtr(raw);
}

bool BindText(sqlite3_stmt* statement, const int index, const std::string_view value) {
    return sqlite3_bind_text(
        statement, index, value.data(), static_cast<int>(value.size()),
        SQLITE_TRANSIENT) == SQLITE_OK;
}

bool OpenReadOnly(
    const std::filesystem::path& path,
    DatabasePtr& database,
    std::wstring& error) {
    const auto resolvedPath = ResolveDatabaseFilePath(path);
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(resolvedPath, filesystemError)) {
        error = L"无法只读打开知识库：文件不存在：" + resolvedPath.wstring();
        return false;
    }
    sqlite3* raw = nullptr;
    const std::string utf8 = PathToUtf8(resolvedPath);
    if (utf8.empty()
        || sqlite3_open_v2(
            utf8.c_str(), &raw,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        error = SqliteError(raw, L"无法只读打开知识库");
        if (raw != nullptr) sqlite3_close(raw);
        return false;
    }
    database.reset(raw);
    sqlite3_busy_timeout(database.get(), 15000);
    std::wstring ignored;
    Execute(database.get(),
        "PRAGMA query_only=ON;PRAGMA temp_store=MEMORY;"
        "PRAGMA cache_size=-131072;PRAGMA mmap_size=536870912;",
        ignored);
    return true;
}

bool OpenOutput(
    const std::filesystem::path& path,
    DatabasePtr& database,
    std::wstring& error) {
    sqlite3* raw = nullptr;
    const std::string utf8 = PathToUtf8(path);
    if (utf8.empty()
        || sqlite3_open_v2(
            utf8.c_str(), &raw,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
            nullptr) != SQLITE_OK) {
        error = SqliteError(raw, L"无法创建高速搜索索引");
        if (raw != nullptr) sqlite3_close(raw);
        return false;
    }
    database.reset(raw);
    sqlite3_busy_timeout(database.get(), 15000);
#ifdef GNSS_DOWNLOADER_DUCKDB_BACKEND
    return Execute(database.get(), "SET threads=4;SET memory_limit='2GB';", error);
#else
    return Execute(database.get(),
        "PRAGMA page_size=32768;PRAGMA journal_mode=OFF;"
        "PRAGMA synchronous=OFF;PRAGMA temp_store=MEMORY;"
        "PRAGMA cache_size=-262144;",
        error);
#endif
}

bool TableExists(sqlite3* database, const std::string_view name) {
    std::wstring error;
    auto statement = Prepare(
        database,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1;",
        error);
    return statement && BindText(statement.get(), 1, name)
        && sqlite3_step(statement.get()) == SQLITE_ROW;
}

std::string UpperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
    return value;
}

std::string NormalizedInterval(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(),
        [](const unsigned char ch) { return std::isspace(ch) != 0; }), value.end());
    value = UpperAscii(std::move(value));
    if (value.empty()) return value;

    // The public archive/file names use a two-character seconds field
    // (01S, 05S, 15S...), while the UI deliberately uses friendlier labels
    // such as 1S and 5S.  Canonicalise both indexed values and query values so
    // they cannot become mutually exclusive merely because of zero padding.
    if (std::all_of(value.begin(), value.end(),
            [](const unsigned char ch) { return std::isdigit(ch) != 0; })) {
        value += 'S';
    } else if (value.ends_with("SEC")) {
        value.resize(value.size() - 3);
        value += 'S';
    } else if (value.ends_with("HZ")) {
        value.resize(value.size() - 2);
        value += 'S';
    }
    if (!value.ends_with('S') || value.size() <= 1) return value;

    std::string digits = value.substr(0, value.size() - 1);
    if (!std::all_of(digits.begin(), digits.end(),
            [](const unsigned char ch) { return std::isdigit(ch) != 0; })) {
        return value;
    }
    const auto firstSignificant = digits.find_first_not_of('0');
    if (firstSignificant == std::string::npos) {
        digits = "0";
    } else {
        digits.erase(0, firstSignificant);
    }
    if (digits.size() == 1) digits.insert(digits.begin(), '0');
    return digits + 'S';
}

bool QueryDateToOrdinal(const QueryDate& value, int& ordinal) {
    const auto ymd = std::chrono::year{value.year}
        / std::chrono::month{static_cast<unsigned>(value.month)}
        / std::chrono::day{static_cast<unsigned>(value.day)};
    if (!ymd.ok()) return false;
    const auto count = std::chrono::sys_days{ymd}.time_since_epoch().count();
    if (count < std::numeric_limits<int>::min()
        || count > std::numeric_limits<int>::max()) return false;
    ordinal = static_cast<int>(count);
    return true;
}

bool TextDateToOrdinal(const std::string_view value, int& ordinal) {
    if (value.size() < 10 || value[4] != '-' || value[7] != '-') return false;
    const auto digit = [&](const std::size_t index) -> int {
        const unsigned char ch = static_cast<unsigned char>(value[index]);
        return std::isdigit(ch) != 0 ? static_cast<int>(ch - '0') : -1;
    };
    int year = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        const int part = digit(index);
        if (part < 0) return false;
        year = year * 10 + part;
    }
    const int m0 = digit(5), m1 = digit(6), d0 = digit(8), d1 = digit(9);
    if (m0 < 0 || m1 < 0 || d0 < 0 || d1 < 0) return false;
    return QueryDateToOrdinal(
        MakeQueryDate(year, m0 * 10 + m1, d0 * 10 + d1), ordinal);
}

int BlockNumber(const int ordinal) {
    // Every currently supported observation archive starts after 1970.  Keep
    // floor division correct nevertheless so an imported older row is safe.
    if (ordinal >= 0) return ordinal / kBitsPerBlock;
    return -(((-ordinal) + kBitsPerBlock - 1) / kBitsPerBlock);
}

int OffsetInBlock(const int ordinal, const int block) {
    return ordinal - block * kBitsPerBlock;
}

struct BitmapBlock {
    int number = 0;
    std::array<std::uint64_t, kWordsPerBlock> words{};
};

BitmapBlock& FindOrAddBlock(std::vector<BitmapBlock>& blocks, const int number) {
    for (auto& block : blocks) {
        if (block.number == number) return block;
    }
    blocks.push_back(BitmapBlock{number});
    return blocks.back();
}

void SetOrdinal(std::vector<BitmapBlock>& blocks, const int ordinal) {
    const int blockNumber = BlockNumber(ordinal);
    const int offset = OffsetInBlock(ordinal, blockNumber);
    if (offset < 0 || offset >= kBitsPerBlock) return;
    auto& block = FindOrAddBlock(blocks, blockNumber);
    block.words[static_cast<std::size_t>(offset / 64)] |=
        std::uint64_t{1} << static_cast<unsigned>(offset % 64);
}

std::uint64_t CountBits(const std::vector<BitmapBlock>& blocks) {
    std::uint64_t count = 0;
    for (const auto& block : blocks) {
        for (const auto word : block.words) count += std::popcount(word);
    }
    return count;
}

double DistanceKm(
    const double latitude1,
    const double longitude1,
    const double latitude2,
    const double longitude2) {
    constexpr double radians = 3.14159265358979323846 / 180.0;
    constexpr double radiusKm = 6371.0088;
    const double lat1 = latitude1 * radians;
    const double lat2 = latitude2 * radians;
    const double dlat = (latitude2 - latitude1) * radians;
    const double dlon = (longitude2 - longitude1) * radians;
    const double a = std::sin(dlat / 2.0) * std::sin(dlat / 2.0)
        + std::cos(lat1) * std::cos(lat2)
        * std::sin(dlon / 2.0) * std::sin(dlon / 2.0);
    return radiusKm * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

struct FileStamp {
    std::uint64_t size = 0;
    std::int64_t modified = 0;
    std::uint64_t walSize = 0;
    std::int64_t walModified = 0;

    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};

FileStamp GetFileStamp(const std::filesystem::path& path) {
    FileStamp stamp;
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error)) {
        error.clear();
        stamp.size = std::filesystem::file_size(path, error);
        error.clear();
        stamp.modified = std::filesystem::last_write_time(path, error)
            .time_since_epoch().count();
    }
    const std::filesystem::path wal(path.wstring() + L"-wal");
    error.clear();
    if (std::filesystem::is_regular_file(wal, error)) {
        error.clear();
        stamp.walSize = std::filesystem::file_size(wal, error);
        // SQLite may create an empty -wal sidecar while opening a previously
        // checkpointed database.  That does not change the fact snapshot and
        // must not make the very first index build report a false race.
        if (stamp.walSize != 0) {
            error.clear();
            stamp.walModified = std::filesystem::last_write_time(wal, error)
                .time_since_epoch().count();
        }
    }
    return stamp;
}

std::filesystem::path EnvironmentPath(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return {};
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        name, value.data(), static_cast<DWORD>(value.size()));
    if (written == 0 || written >= value.size()) return {};
    value.resize(written);
    return std::filesystem::path(value);
}

std::filesystem::path DefaultKnowledgeBaseDirectory() {
#ifdef GNSS_DOWNLOADER_DUCKDB_BACKEND
    // Anchor every packaged index to the validated DuckDB main database.
    // This deliberately outranks stale GNSS_DOWNLOADER_INDEX_DIR values from
    // a previous SQLite session.
    {
        const auto mainDatabase = GetIgsAvailabilityDatabasePath();
        std::error_code mainError;
        if (std::filesystem::is_regular_file(mainDatabase, mainError)) {
            return mainDatabase.parent_path();
        }
    }
#endif
    auto directory = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
    if (!directory.empty()) {
#ifdef GNSS_DOWNLOADER_DUCKDB_BACKEND
        const auto candidate = ResolveDatabaseFilePath(
            directory / L"observation_search.sqlite");
        std::error_code indexError;
        if (std::filesystem::is_regular_file(candidate, indexError)) {
            return directory;
        }
#else
        return directory;
#endif
    }
    const auto explicitIgs = EnvironmentPath(L"GNSS_DOWNLOADER_DB_PATH");
    if (!explicitIgs.empty()) {
        std::error_code databaseError;
        if (std::filesystem::is_regular_file(
                ResolveDatabaseFilePath(explicitIgs), databaseError)) {
            return explicitIgs.parent_path();
        }
    }
#ifdef GNSS_DOWNLOADER_DUCKDB_BACKEND
    // Keep the portable DuckDB release usable when its EXE is double-clicked
    // instead of started through the environment-setting launcher.
    std::wstring moduleBuffer(32768, L'\0');
    const DWORD moduleLength = GetModuleFileNameW(
        nullptr, moduleBuffer.data(), static_cast<DWORD>(moduleBuffer.size()));
    if (moduleLength > 0 && moduleLength < moduleBuffer.size()) {
        moduleBuffer.resize(moduleLength);
        const auto bundled = std::filesystem::path(moduleBuffer).parent_path()
            .parent_path() / L"knowledge-base";
        std::error_code bundledError;
        if (std::filesystem::is_directory(bundled, bundledError)) {
            return bundled;
        }
    }
#endif
    auto local = EnvironmentPath(L"LOCALAPPDATA");
    if (local.empty()) local = std::filesystem::current_path();
    const auto settings = local / L"GNSSDownloader" / L"settings.ini";
    wchar_t configured[32768]{};
    const DWORD length = GetPrivateProfileStringW(
        L"storage", L"database_path", L"", configured,
        static_cast<DWORD>(std::size(configured)), settings.c_str());
    if (length > 0 && length < std::size(configured) - 1) {
        return std::filesystem::path(configured).parent_path();
    }
    return local / L"GNSSDownloader";
}

struct IdentityAggregate {
    ObservationNetworkMask networkMask = 0;
    std::string stationKey;
    std::string stationId;
    std::string fourCharId;
    std::string country;
    bool hasCoordinate = false;
    double latitude = 0.0;
    double longitude = 0.0;
    // Lower values have higher authority.  Daily-file coordinates are -1 and
    // therefore always win over a Site Log coordinate (native=0,
    // inherited=1, shared IGS/MGEX=2).  The value is an in-memory provenance
    // marker only; the derived SQLite schema stores the resulting LLH.
    int coordinateSourceRank = std::numeric_limits<int>::max();
};

struct ProfileKey {
    std::size_t identity = 0;
    ObservationNetworkMask networkMask = 0;
    int rinexFormat = 0;
    std::string interval;

    friend bool operator==(const ProfileKey&, const ProfileKey&) = default;
};

struct ProfileKeyHash {
    std::size_t operator()(const ProfileKey& value) const noexcept {
        std::size_t result = std::hash<std::size_t>{}(value.identity);
        result ^= std::hash<std::uint64_t>{}(value.networkMask)
            + 0x9e3779b9U + (result << 6U) + (result >> 2U);
        result ^= std::hash<int>{}(value.rinexFormat)
            + 0x9e3779b9U + (result << 6U) + (result >> 2U);
        result ^= std::hash<std::string>{}(value.interval)
            + 0x9e3779b9U + (result << 6U) + (result >> 2U);
        return result;
    }
};

struct ProfileAggregate {
    ProfileKey key;
    std::vector<BitmapBlock> blocks;
    int minimumDay = std::numeric_limits<int>::max();
    int maximumDay = std::numeric_limits<int>::min();
};

struct SourceState {
    ObservationNetworkMask networkMask = 0;
    std::filesystem::path path;
    FileStamp stamp;
    std::uint64_t rows = 0;
    std::string maximumDate;
};

// Site Log lookup precedence intentionally mirrors the existing GUI:
// every selected regional network's native record is considered first,
// followed by inherited IGS records in the regional database, and finally
// the shared IGS/MGEX record.  `applicabilityMask` describes which daily
// observation membership makes this record eligible.
struct PendingSiteRecord {
    std::size_t identity = 0;
    ObservationNetworkMask applicabilityMask = 0;
    int tier = 0; // 0 native, 1 regional inherited, 2 shared global
    int networkOrder = 0;
    bool exactStationId = false;
    SiteLogRecord record;
};

struct BuildMemory {
    std::vector<IdentityAggregate> identities;
    std::unordered_map<std::string, std::size_t> identityByKey;
    std::vector<ProfileAggregate> profiles;
    std::unordered_map<ProfileKey, std::size_t, ProfileKeyHash> profileByKey;
    std::unordered_map<ObservationNetworkMask, std::vector<BitmapBlock>> coverage;
    std::vector<SourceState> sources;
    std::vector<PendingSiteRecord> siteRecords;
    std::uint64_t rowsRead = 0;
};

constexpr std::array kSiteLogRegionalPrecedence{
    ObservationNetwork::Ngs, ObservationNetwork::Epn,
    ObservationNetwork::Hk, ObservationNetwork::GaGdc,
    ObservationNetwork::Rgna, ObservationNetwork::Sonel,
    ObservationNetwork::TrigNet, ObservationNetwork::Cut,
    ObservationNetwork::Csn, ObservationNetwork::EposIceland,
    ObservationNetwork::Rbmc, ObservationNetwork::Ramsac,
    ObservationNetwork::RegnaRou, ObservationNetwork::Cacs,
    ObservationNetwork::Rgp, ObservationNetwork::SopacGarner,
    ObservationNetwork::MagnaEco, ObservationNetwork::PanamaCors,
    ObservationNetwork::HondurasCors, ObservationNetwork::EposGnss,
    ObservationNetwork::Gref, ObservationNetwork::OsNet,
    ObservationNetwork::Ergnss, ObservationNetwork::Renep,
    ObservationNetwork::GeoNetNz, ObservationNetwork::Ring,
    ObservationNetwork::Apos, ObservationNetwork::RobBelgium,
    ObservationNetwork::Dpga, ObservationNetwork::CatNet,
    ObservationNetwork::SpsLux, ObservationNetwork::KasiNgii,
    ObservationNetwork::Renag, ObservationNetwork::Nsgi,
    ObservationNetwork::NoaGnss, ObservationNetwork::Redgae,
    ObservationNetwork::GeoNetJapan, ObservationNetwork::Dlab,
    ObservationNetwork::Magnet, ObservationNetwork::PangaCwu
};

int SiteLogNetworkOrder(const ObservationNetwork network) {
    const auto found = std::find(
        kSiteLogRegionalPrecedence.begin(),
        kSiteLogRegionalPrecedence.end(), network);
    return found == kSiteLogRegionalPrecedence.end()
        ? static_cast<int>(kSiteLogRegionalPrecedence.size())
        : static_cast<int>(found - kSiteLogRegionalPrecedence.begin());
}

std::string IdentityMapKey(
    const ObservationNetworkMask networkMask,
    const std::string_view stationKey) {
    return std::to_string(networkMask) + '\x1f' + std::string(stationKey);
}

std::size_t FindOrAddIdentity(
    BuildMemory& memory,
    const ObservationNetworkMask networkMask,
    std::string stationKey) {
    stationKey = UpperAscii(std::move(stationKey));
    const std::string key = IdentityMapKey(networkMask, stationKey);
    if (const auto found = memory.identityByKey.find(key);
        found != memory.identityByKey.end()) return found->second;
    const std::size_t index = memory.identities.size();
    IdentityAggregate identity;
    identity.networkMask = networkMask;
    identity.stationKey = std::move(stationKey);
    memory.identities.push_back(std::move(identity));
    memory.identityByKey.emplace(key, index);
    return index;
}

bool ValidLatitudeLongitude(const double latitude, const double longitude) {
    return std::isfinite(latitude) && std::isfinite(longitude)
        && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0;
}

struct ResolvedSiteLogCoordinate {
    double latitude = 0.0;
    double longitude = 0.0;
};

// Site Logs normally carry geodetic latitude/longitude, but a number of
// regional imports expose only the IGS-standard Cartesian XYZ position.  The
// shared SiteLogDb conversion is deliberately used here rather than parsing
// the raw log again; this keeps the derived-index path identical to the
// source-database parser and avoids subtly different polar/height handling.
std::optional<ResolvedSiteLogCoordinate> ResolveSiteLogCoordinate(
    const SiteLogRecord& record) {
    if (record.hasCoordinate
        && ValidLatitudeLongitude(record.latitude, record.longitude)) {
        return ResolvedSiteLogCoordinate{record.latitude, record.longitude};
    }
    if (!record.hasXyz
        || !std::isfinite(record.x) || !std::isfinite(record.y)
        || !std::isfinite(record.z)) {
        return std::nullopt;
    }

    double latitude = 0.0;
    double longitude = 0.0;
    double elevation = 0.0;
    if (!SiteLogEcefToGeodetic(
            record.x, record.y, record.z,
            latitude, longitude, elevation)
        || !ValidLatitudeLongitude(latitude, longitude)) {
        return std::nullopt;
    }
    return ResolvedSiteLogCoordinate{latitude, longitude};
}

void ApplyIdentityCoordinate(
    IdentityAggregate& identity,
    const ResolvedSiteLogCoordinate& coordinate,
    const int sourceRank) {
    if (!ValidLatitudeLongitude(coordinate.latitude, coordinate.longitude)) {
        return;
    }
    if (identity.hasCoordinate
        && sourceRank >= identity.coordinateSourceRank) {
        // A valid daily_files coordinate (rank -1), or an earlier Site Log
        // tier, must never be replaced by a lower-authority candidate.
        return;
    }
    identity.hasCoordinate = true;
    identity.latitude = coordinate.latitude;
    identity.longitude = coordinate.longitude;
    identity.coordinateSourceRank = sourceRank;
}

void UpdateIdentity(
    IdentityAggregate& identity,
    std::string stationId,
    std::string fourCharId,
    std::string country,
    const bool hasCoordinate,
    const double latitude,
    const double longitude) {
    stationId = UpperAscii(std::move(stationId));
    fourCharId = UpperAscii(std::move(fourCharId));
    country = UpperAscii(std::move(country));
    if (identity.stationId.empty()
        || (identity.stationId.size() != 9 && stationId.size() == 9)) {
        if (!stationId.empty()) identity.stationId = std::move(stationId);
    }
    if (identity.fourCharId.empty() && !fourCharId.empty()) {
        identity.fourCharId = std::move(fourCharId);
    }
    if (identity.country.empty() && !country.empty()) {
        identity.country = std::move(country);
    }
    const bool coordinateValid = hasCoordinate
        && ValidLatitudeLongitude(latitude, longitude);
    if (coordinateValid) {
        // Coordinates read from daily_files (including the IGS metadata
        // projection) are authoritative for the observation archive.  They
        // intentionally outrank any Site Log value attached later.
        ApplyIdentityCoordinate(
            identity, ResolvedSiteLogCoordinate{latitude, longitude}, -1);
    }
    if (identity.fourCharId.empty()) {
        if (identity.stationId.size() >= 4) {
            identity.fourCharId = identity.stationId.substr(0, 4);
        } else if (identity.stationKey.rfind("FOUR:", 0) == 0) {
            identity.fourCharId = identity.stationKey.substr(5);
        }
    }
    if (identity.stationId.empty() && identity.stationKey.rfind("FOUR:", 0) != 0) {
        identity.stationId = identity.stationKey;
    }
}

void AddProfileDay(
    BuildMemory& memory,
    const std::size_t identity,
    const ObservationNetworkMask networkMask,
    const int rinexFormat,
    std::string interval,
    const int ordinal) {
    interval = NormalizedInterval(std::move(interval));
    ProfileKey key{identity, networkMask, rinexFormat == 2 ? 2 : 3, interval};
    std::size_t profileIndex = 0;
    if (const auto found = memory.profileByKey.find(key);
        found != memory.profileByKey.end()) {
        profileIndex = found->second;
    } else {
        profileIndex = memory.profiles.size();
        ProfileAggregate aggregate;
        aggregate.key = key;
        memory.profiles.push_back(std::move(aggregate));
        memory.profileByKey.emplace(std::move(key), profileIndex);
    }
    auto& profile = memory.profiles[profileIndex];
    SetOrdinal(profile.blocks, ordinal);
    profile.minimumDay = std::min(profile.minimumDay, ordinal);
    profile.maximumDay = std::max(profile.maximumDay, ordinal);
}

void ReportProgress(
    const ObservationSearchBuildOptions& options,
    const SourceSpec& source,
    const std::filesystem::path& path,
    const BuildMemory& memory,
    const std::uint64_t sourceRows,
    const bool finished) {
    if (!options.progress) return;
    options.progress(ObservationSearchBuildProgress{
        source.network,
        source.label,
        path,
        sourceRows,
        static_cast<std::uint64_t>(memory.profiles.size()),
        finished});
}

bool ScanCoverageSingle(
    sqlite3* database,
    const ObservationNetworkMask networkMask,
    BuildMemory& memory,
    std::string& maximumDate,
    std::wstring& error) {
    auto statement = Prepare(database,
        "SELECT observation_date FROM daily_scans "
        "WHERE status='complete' AND COALESCE(is_provisional,0)=0;", error);
    if (!statement) return false;
    while (true) {
        const int status = sqlite3_step(statement.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            error = SqliteError(database, L"读取站网完整日期失败");
            return false;
        }
        const std::string date = ColumnText(statement.get(), 0);
        int ordinal = 0;
        if (!TextDateToOrdinal(date, ordinal)) continue;
        SetOrdinal(memory.coverage[networkMask], ordinal);
        if (date > maximumDate) maximumDate = date;
    }
    return true;
}

bool ScanCoverageIgs(
    sqlite3* database,
    BuildMemory& memory,
    std::string& maximumDate,
    std::wstring& error) {
    auto statement = Prepare(database,
        "SELECT d.observation_date FROM daily_scans d "
        "JOIN daily_source_scans s ON s.network=d.network "
        " AND s.observation_date=d.observation_date "
        "WHERE d.network='IGS' AND d.status='complete' "
        " AND COALESCE(d.is_provisional,0)=0 "
        "GROUP BY d.observation_date "
        "HAVING COUNT(*)=7 AND SUM(CASE WHEN s.status='complete' "
        " AND s.source_id BETWEEN 1 AND 7 THEN 1 ELSE 0 END)=7;", error);
    if (!statement) return false;
    while (true) {
        const int status = sqlite3_step(statement.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            error = SqliteError(database, L"读取IGS七源完整日期失败");
            return false;
        }
        const std::string date = ColumnText(statement.get(), 0);
        int ordinal = 0;
        if (!TextDateToOrdinal(date, ordinal)) continue;
        SetOrdinal(memory.coverage[NetworkBit(ObservationNetwork::Igs)], ordinal);
        SetOrdinal(memory.coverage[NetworkBit(ObservationNetwork::Mgex)], ordinal);
        if (date > maximumDate) maximumDate = date;
    }
    return true;
}

bool ScanSingleSource(
    sqlite3* database,
    const SourceSpec& source,
    const ObservationSearchBuildOptions& options,
    const std::filesystem::path& path,
    BuildMemory& memory,
    const std::stop_token stopToken,
    SourceState& sourceState,
    std::wstring& error) {
    const auto networkMask = NetworkBit(source.network);
    if (!ScanCoverageSingle(
            database, networkMask, memory, sourceState.maximumDate, error)) {
        return false;
    }
    auto rows = Prepare(database,
        "SELECT observation_date,station_key,station_id,four_char_id,country,"
        "rinex_format,sample_interval,has_coordinate,latitude,longitude "
        "FROM daily_files;", error);
    if (!rows) return false;
    std::uint64_t sourceRows = 0;
    while (true) {
        if ((sourceRows & 0x3fffU) == 0U && stopToken.stop_requested()) {
            error = L"高速索引构建已取消。";
            return false;
        }
        const int status = sqlite3_step(rows.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            error = SqliteError(database, L"读取站网观测事实失败");
            return false;
        }
        const std::string date = ColumnText(rows.get(), 0);
        int ordinal = 0;
        if (!TextDateToOrdinal(date, ordinal)) continue;
        std::string stationKey = ColumnText(rows.get(), 1);
        const std::string stationId = ColumnText(rows.get(), 2);
        const std::string fourChar = ColumnText(rows.get(), 3);
        if (stationKey.empty()) {
            stationKey = stationId.empty() ? "FOUR:" + fourChar : stationId;
        }
        if (stationKey.empty() || stationKey == "FOUR:") continue;
        const std::size_t identityIndex = FindOrAddIdentity(
            memory, networkMask, stationKey);
        const bool hasCoordinate = sqlite3_column_int(rows.get(), 7) != 0;
        UpdateIdentity(
            memory.identities[identityIndex],
            stationId, fourChar, ColumnText(rows.get(), 4),
            hasCoordinate,
            hasCoordinate ? sqlite3_column_double(rows.get(), 8) : 0.0,
            hasCoordinate ? sqlite3_column_double(rows.get(), 9) : 0.0);
        AddProfileDay(
            memory, identityIndex, networkMask,
            sqlite3_column_int(rows.get(), 5), ColumnText(rows.get(), 6), ordinal);
        ++sourceRows;
        ++memory.rowsRead;
        if (sourceRows % 1000000U == 0U) {
            ReportProgress(options, source, path, memory, sourceRows, false);
        }
    }

    sourceState.rows = sourceRows;
    ReportProgress(options, source, path, memory, sourceRows, true);
    return true;
}

bool ScanIgsSource(
    sqlite3* database,
    const SourceSpec& source,
    const ObservationSearchBuildOptions& options,
    const std::filesystem::path& path,
    BuildMemory& memory,
    const std::stop_token stopToken,
    SourceState& sourceState,
    std::wstring& error) {
    if (!ScanCoverageIgs(database, memory, sourceState.maximumDate, error)) {
        return false;
    }
    const ObservationNetworkMask sharedMask =
        NetworkBit(ObservationNetwork::Igs) | NetworkBit(ObservationNetwork::Mgex);

    // Metadata is read independently from the logical-file table.  Joining
    // daily_files here would multiply logical rows and would accidentally pull
    // the physical route layer back into this compact build.
    auto metadata = Prepare(database,
        "SELECT station_key,station_id,four_char_id,country,has_coordinate,"
        "latitude,longitude FROM daily_files WHERE network='IGS';", error);
    if (!metadata) return false;
    std::uint64_t metadataRows = 0;
    while (true) {
        if ((metadataRows & 0x3fffU) == 0U && stopToken.stop_requested()) {
            error = L"高速索引构建已取消。";
            return false;
        }
        const int status = sqlite3_step(metadata.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            error = SqliteError(database, L"读取IGS测站元数据失败");
            return false;
        }
        const std::string stationKey = ColumnText(metadata.get(), 0);
        if (stationKey.empty()) continue;
        const std::size_t identityIndex = FindOrAddIdentity(
            memory, sharedMask, stationKey);
        const bool hasCoordinate = sqlite3_column_int(metadata.get(), 4) != 0;
        UpdateIdentity(
            memory.identities[identityIndex],
            ColumnText(metadata.get(), 1), ColumnText(metadata.get(), 2),
            ColumnText(metadata.get(), 3), hasCoordinate,
            hasCoordinate ? sqlite3_column_double(metadata.get(), 5) : 0.0,
            hasCoordinate ? sqlite3_column_double(metadata.get(), 6) : 0.0);
        ++metadataRows;
    }

    auto rows = Prepare(database,
        "SELECT observation_date,station_key,rinex_format,sample_interval "
        "FROM daily_observation_files WHERE network='IGS';", error);
    if (!rows) return false;
    std::uint64_t sourceRows = 0;
    while (true) {
        if ((sourceRows & 0x3fffU) == 0U && stopToken.stop_requested()) {
            error = L"高速索引构建已取消。";
            return false;
        }
        const int status = sqlite3_step(rows.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            error = SqliteError(database, L"读取IGS逻辑观测事实失败");
            return false;
        }
        int ordinal = 0;
        if (!TextDateToOrdinal(ColumnText(rows.get(), 0), ordinal)) continue;
        const std::string stationKey = ColumnText(rows.get(), 1);
        if (stationKey.empty()) continue;
        const std::size_t identityIndex = FindOrAddIdentity(
            memory, sharedMask, stationKey);
        auto& identity = memory.identities[identityIndex];
        UpdateIdentity(identity, {}, {}, {}, false, 0.0, 0.0);
        const int format = sqlite3_column_int(rows.get(), 2) == 2 ? 2 : 3;
        const ObservationNetworkMask profileMask =
            NetworkBit(ObservationNetwork::Igs)
            | (format == 3 ? NetworkBit(ObservationNetwork::Mgex) : 0);
        AddProfileDay(
            memory, identityIndex, profileMask, format,
            ColumnText(rows.get(), 3), ordinal);
        ++sourceRows;
        ++memory.rowsRead;
        if (sourceRows % 1000000U == 0U) {
            ReportProgress(options, source, path, memory, sourceRows, false);
        }
    }

    // IGS high-rate observations are intentionally stored separately from
    // daily_observation_files.  They represent the 1 Hz (01S) archive at
    // station/day granularity and may use a FOUR:XXXX station key even when a
    // nine-character station ID is known.  Preserve that source alias while
    // carrying the full ID into the identity merger, so the high-rate profile
    // receives the country, coordinate and equipment metadata of the matching
    // daily IGS station instead of becoming a second anonymous station.
    if (TableExists(database, "highrate_daily_observations")) {
        auto highRateRows = Prepare(database,
            "SELECT observation_date,station_key,station_id,four_char_id,"
            "rinex_format_mask,sample_interval "
            "FROM highrate_daily_observations "
            "WHERE network='IGS' AND has_data<>0;", error);
        if (!highRateRows) return false;
        while (true) {
            if ((sourceRows & 0x3fffU) == 0U && stopToken.stop_requested()) {
                error = L"高速索引构建已取消。";
                return false;
            }
            const int status = sqlite3_step(highRateRows.get());
            if (status == SQLITE_DONE) break;
            if (status != SQLITE_ROW) {
                error = SqliteError(database, L"读取IGS高频观测事实失败");
                return false;
            }
            const std::string date = ColumnText(highRateRows.get(), 0);
            int ordinal = 0;
            if (!TextDateToOrdinal(date, ordinal)) continue;
            const std::string stationKey = ColumnText(highRateRows.get(), 1);
            if (stationKey.empty()) continue;
            const std::size_t identityIndex = FindOrAddIdentity(
                memory, sharedMask, stationKey);
            UpdateIdentity(
                memory.identities[identityIndex],
                ColumnText(highRateRows.get(), 2),
                ColumnText(highRateRows.get(), 3),
                {}, false, 0.0, 0.0);
            const auto formatMask = static_cast<RinexFormatMask>(
                sqlite3_column_int(highRateRows.get(), 4));
            const std::string interval = ColumnText(highRateRows.get(), 5);
            if ((formatMask & RinexFormatBit(RinexFormat::Rinex2)) != 0) {
                AddProfileDay(
                    memory, identityIndex,
                    NetworkBit(ObservationNetwork::Igs),
                    2, interval, ordinal);
            }
            if ((formatMask & RinexFormatBit(RinexFormat::Rinex34)) != 0) {
                AddProfileDay(
                    memory, identityIndex, sharedMask,
                    3, interval, ordinal);
            }
            if (date > sourceState.maximumDate) {
                sourceState.maximumDate = date;
            }
            ++sourceRows;
            ++memory.rowsRead;
            if (sourceRows % 1000000U == 0U) {
                ReportProgress(options, source, path, memory, sourceRows, false);
            }
        }
    }
    sourceState.rows = sourceRows;
    ReportProgress(options, source, path, memory, sourceRows, true);
    return true;
}

struct SiteLogLookup {
    // LoadSiteLogSummaryMap exposes every canonical record under both its
    // nine-character and four-character aliases.  Keep vectors here rather
    // than looking up that alias map directly, so a four-character collision
    // is detected instead of silently attaching the first record.
    std::unordered_map<std::string, std::vector<const SiteLogRecord*>> byStationId;
    std::unordered_map<std::string, std::vector<const SiteLogRecord*>> byFourChar;
};

// The summary API intentionally canonicalizes four-character aliases.  That
// is convenient for display metadata, but it cannot represent two physical
// stations which happen to reuse the same four-character marker.  The
// coordinate path therefore reads the original site_logs rows separately
// (read-only, and with no raw_text) and uses this lookup to make an exact or
// demonstrably unique coordinate decision.
bool LoadRawSiteLogCoordinateRecords(
    const std::filesystem::path& databasePath,
    const std::uint64_t preferredNetworkMask,
    std::vector<SiteLogRecord>& records,
    std::wstring& error) {
    records.clear();
    error.clear();
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(databasePath, filesystemError)) {
        return true;
    }
    DatabasePtr database;
    if (!OpenReadOnly(databasePath, database, error)) return false;
    if (!TableExists(database.get(), "site_logs")) return true;

    // Keep the projection deliberately narrow.  In particular, raw_text can
    // be hundreds of megabytes in SOPAC/GARNER and is not needed to resolve
    // a station position.  SiteLogDb::ReadRecord performs the same XYZ
    // fallback when loading a full row; ResolveSiteLogCoordinate below does
    // it for this compact projection.
    constexpr const char* sql =
        "SELECT source_url,station_id,four_char_id,country,"
        "x,y,z,has_xyz,latitude,longitude,has_coordinate,"
        "network_mask,revision_date,fetched_at_utc "
        "FROM site_logs WHERE (?1=0 OR (network_mask & ?1) != 0);";
    std::wstring prepareError;
    auto statement = Prepare(database.get(), sql, prepareError);
    if (!statement) {
        error = std::move(prepareError);
        return false;
    }
    if (sqlite3_bind_int64(
            statement.get(), 1,
            static_cast<sqlite3_int64>(preferredNetworkMask)) != SQLITE_OK) {
        error = SqliteError(database.get(), L"绑定 Site Log 坐标来源筛选失败");
        return false;
    }
    while (true) {
        const int status = sqlite3_step(statement.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            error = SqliteError(database.get(), L"读取 Site Log 原始坐标失败");
            records.clear();
            return false;
        }
        SiteLogRecord record;
        record.sourceUrl = ColumnText(statement.get(), 0);
        record.stationId = ColumnText(statement.get(), 1);
        record.fourCharId = ColumnText(statement.get(), 2);
        record.country = ColumnText(statement.get(), 3);
        record.x = sqlite3_column_double(statement.get(), 4);
        record.y = sqlite3_column_double(statement.get(), 5);
        record.z = sqlite3_column_double(statement.get(), 6);
        record.hasXyz = sqlite3_column_int(statement.get(), 7) != 0;
        record.latitude = sqlite3_column_double(statement.get(), 8);
        record.longitude = sqlite3_column_double(statement.get(), 9);
        record.hasCoordinate = sqlite3_column_int(statement.get(), 10) != 0;
        record.networkMask = static_cast<std::uint64_t>(
            sqlite3_column_int64(statement.get(), 11));
        record.revisionDate = ColumnText(statement.get(), 12);
        record.fetchedAtUtc = ColumnText(statement.get(), 13);
        records.push_back(std::move(record));
    }
    return true;
}

std::string SiteLogRecordIdentityKey(const SiteLogRecord& record) {
    // Aliased copies have the same provenance tuple.  Include identifiers and
    // revision/fetch times as a fallback for locally-created rows without a
    // source URL.
    std::string key = UpperAscii(record.sourceUrl);
    key += '\x1f';
    key += UpperAscii(record.stationId);
    key += '\x1f';
    key += UpperAscii(record.fourCharId);
    key += '\x1f';
    key += record.revisionDate;
    key += '\x1f';
    key += record.fetchedAtUtc;
    return key;
}

std::string SiteLogRecordFourChar(const SiteLogRecord& record) {
    if (!record.fourCharId.empty()) return UpperAscii(record.fourCharId);
    if (record.stationId.size() >= 4) {
        return UpperAscii(record.stationId.substr(0, 4));
    }
    return {};
}

void AppendUniqueSiteLogCandidate(
    std::vector<const SiteLogRecord*>& candidates,
    const SiteLogRecord* record) {
    if (record == nullptr) return;
    const auto sameRecord = [&](const SiteLogRecord* existing) {
        return SiteLogRecordIdentityKey(*existing)
            == SiteLogRecordIdentityKey(*record);
    };
    if (std::find_if(candidates.begin(), candidates.end(), sameRecord)
        == candidates.end()) {
        candidates.push_back(record);
    }
}

SiteLogLookup BuildSiteLogLookup(
    const std::unordered_map<std::string, SiteLogRecord>& records) {
    SiteLogLookup lookup;
    lookup.byStationId.reserve(records.size());
    lookup.byFourChar.reserve(records.size());
    std::unordered_set<std::string> seen;
    seen.reserve(records.size());
    for (const auto& [alias, record] : records) {
        (void)alias;
        if (!seen.insert(SiteLogRecordIdentityKey(record)).second) continue;
        if (!record.stationId.empty()) {
            AppendUniqueSiteLogCandidate(
                lookup.byStationId[UpperAscii(record.stationId)], &record);
        }
        const std::string four = SiteLogRecordFourChar(record);
        if (!four.empty()) {
            AppendUniqueSiteLogCandidate(lookup.byFourChar[four], &record);
        }
    }
    return lookup;
}

// Build the same collision-aware lookup over the uncanonicalized rows loaded
// directly from site_logs.  Keep this overload separate from the summary-map
// overload: pointers must refer to the raw vector for the lifetime of one
// AppendSiteLogMap call, and a summary map has already lost four-character
// aliases which collide across countries/networks.
SiteLogLookup BuildSiteLogLookup(
    const std::vector<SiteLogRecord>& records) {
    SiteLogLookup lookup;
    lookup.byStationId.reserve(records.size());
    lookup.byFourChar.reserve(records.size());
    std::unordered_set<std::string> seen;
    seen.reserve(records.size());
    for (const auto& record : records) {
        if (!seen.insert(SiteLogRecordIdentityKey(record)).second) continue;
        if (!record.stationId.empty()) {
            AppendUniqueSiteLogCandidate(
                lookup.byStationId[UpperAscii(record.stationId)], &record);
        }
        const std::string four = SiteLogRecordFourChar(record);
        if (!four.empty()) {
            AppendUniqueSiteLogCandidate(lookup.byFourChar[four], &record);
        }
    }
    return lookup;
}

// SOPAC/GARNER's historical Arizona station used the short code TONT.  A
// later Tonga Tide Gauge log deliberately states that TONT was decommissioned
// because of a code conflict and that the replacement code is TOGT.  The
// regional database necessarily contains both the old observation rows and the
// newer log, so this narrowly identified record must not be attached to the
// historical TONT identity during a central rebuild.  Keep this check on the
// parsed scalar fields (rather than scanning raw_text) so rebuilding remains
// fast even for the multi-gigabyte SOPAC archive.
bool IsKnownRetiredCodeReuse(const SiteLogRecord& record) {
    return record.stationId.empty()
        && UpperAscii(record.fourCharId) == "TONT"
        && UpperAscii(record.country) == "TONGA"
        && UpperAscii(record.sourceUrl).find("/TONT_") != std::string::npos;
}

int SiteLogCandidateQuality(const SiteLogRecord& record) {
    const int coordinate = ResolveSiteLogCoordinate(record).has_value() ? 32 : 0;
    // Local, bounded header histories represent the actual archived files.
    // Their equipment intervals must take precedence over a later Site Log
    // snapshot, including genuine omitted fields and observed changes.
    const int archivedHistory = record.sourceUrl.starts_with("header-interval://")
        && !record.equipmentHistory.empty() ? 64 : 0;
    return archivedHistory + coordinate
        + (!record.domes.empty() ? 8 : 0)
        + (!record.receiverType.empty() ? 4 : 0)
        + (!record.antennaType.empty() ? 2 : 0)
        + (!record.siteName.empty() ? 1 : 0);
}

bool SiteLogCandidateIsBetter(
    const SiteLogRecord& candidate,
    const SiteLogRecord& current) {
    const int candidateQuality = SiteLogCandidateQuality(candidate);
    const int currentQuality = SiteLogCandidateQuality(current);
    if (candidateQuality != currentQuality) {
        return candidateQuality > currentQuality;
    }
    return std::tie(candidate.revisionDate, candidate.fetchedAtUtc,
                    candidate.sourceUrl)
        > std::tie(current.revisionDate, current.fetchedAtUtc,
                   current.sourceUrl);
}

const SiteLogRecord* BestSiteLogCandidate(
    const std::vector<const SiteLogRecord*>& candidates) {
    const SiteLogRecord* best = nullptr;
    for (const auto* candidate : candidates) {
        if (candidate == nullptr) continue;
        if (best == nullptr || SiteLogCandidateIsBetter(*candidate, *best)) {
            best = candidate;
        }
    }
    return best;
}

bool SameSiteLogPhysicalCandidate(
    const SiteLogRecord& left,
    const SiteLogRecord& right) {
    const bool leftFullId = left.stationId.size() > 4;
    const bool rightFullId = right.stationId.size() > 4;
    if (leftFullId && rightFullId) {
        return UpperAscii(left.stationId) == UpperAscii(right.stationId);
    }
    if (!left.sourceUrl.empty() && !right.sourceUrl.empty()
        && UpperAscii(left.sourceUrl) == UpperAscii(right.sourceUrl)) {
        return true;
    }
    const auto leftCoordinate = ResolveSiteLogCoordinate(left);
    const auto rightCoordinate = ResolveSiteLogCoordinate(right);
    if (leftCoordinate.has_value() && rightCoordinate.has_value()) {
        return DistanceKm(
            leftCoordinate->latitude, leftCoordinate->longitude,
            rightCoordinate->latitude, rightCoordinate->longitude) <= 3.0;
    }
    return false;
}

// Observation archives do not use one country spelling consistently: NGS
// commonly stores ISO-3166 alpha-2 values ("US"), regional feeds use
// alpha-3 values ("DEU"), and Site Logs often contain English names
// ("Germany" or "United States of America (the)").  Compare canonical
// tokens so a safe four-character match is not discarded merely because the
// two sources chose different representations.  Unknown values are kept
// verbatim, preserving the conservative mismatch behaviour.
std::string CanonicalCountry(std::string value) {
    value = UpperAscii(std::move(value));
    const std::string original = value;
    value.erase(std::remove_if(value.begin(), value.end(),
        [](const unsigned char character) {
            return std::isalnum(character) == 0;
        }), value.end());
    // Do not turn non-ASCII/otherwise unknown country text into the same
    // empty token.  Keeping the original token preserves conservative
    // collision rejection for localized names such as Chinese/Japanese.
    if (value.empty()) return original;
    static const std::unordered_map<std::string_view, std::string_view> aliases{
        {"US", "USA"}, {"USA", "USA"},
        {"UNITEDSTATES", "USA"},
        {"UNITEDSTATESOFAMERICA", "USA"},
        {"UNITEDSTATESOFTHE", "USA"},
        {"UNITEDSTATESOFAMERICATHE", "USA"},
        {"CA", "CAN"}, {"CAN", "CAN"}, {"CANADA", "CAN"},
        {"MX", "MEX"}, {"MEX", "MEX"}, {"MEXICO", "MEX"},
        {"BR", "BRA"}, {"BRA", "BRA"}, {"BRAZIL", "BRA"},
        {"AR", "ARG"}, {"ARG", "ARG"}, {"ARGENTINA", "ARG"},
        {"CL", "CHL"}, {"CHL", "CHL"}, {"CHILE", "CHL"},
        {"DE", "DEU"}, {"DEU", "DEU"}, {"GERMANY", "DEU"},
        {"AT", "AUT"}, {"AUT", "AUT"}, {"AUSTRIA", "AUT"},
        {"CH", "CHE"}, {"CHE", "CHE"}, {"SWITZERLAND", "CHE"},
        {"FR", "FRA"}, {"FRA", "FRA"}, {"FRANCE", "FRA"},
        {"ES", "ESP"}, {"ESP", "ESP"}, {"SPAIN", "ESP"},
        {"PT", "PRT"}, {"PRT", "PRT"}, {"PORTUGAL", "PRT"},
        {"GB", "GBR"}, {"UK", "GBR"}, {"GBR", "GBR"},
        {"UNITEDKINGDOM", "GBR"},
        {"IE", "IRL"}, {"IRL", "IRL"}, {"IRELAND", "IRL"},
        {"NL", "NLD"}, {"NLD", "NLD"}, {"NETHERLANDS", "NLD"},
        {"BE", "BEL"}, {"BEL", "BEL"}, {"BELGIUM", "BEL"},
        {"DK", "DNK"}, {"DNK", "DNK"}, {"DENMARK", "DNK"},
        {"SE", "SWE"}, {"SWE", "SWE"}, {"SWEDEN", "SWE"},
        {"NO", "NOR"}, {"NOR", "NOR"}, {"NORWAY", "NOR"},
        {"FI", "FIN"}, {"FIN", "FIN"}, {"FINLAND", "FIN"},
        {"IS", "ISL"}, {"ISL", "ISL"}, {"ICELAND", "ISL"},
        {"PL", "POL"}, {"POL", "POL"}, {"POLAND", "POL"},
        {"CZ", "CZE"}, {"CZE", "CZE"},
        {"CZECHREPUBLIC", "CZE"}, {"CZECHIA", "CZE"},
        {"SK", "SVK"}, {"SVK", "SVK"}, {"SLOVAKIA", "SVK"},
        {"HU", "HUN"}, {"HUN", "HUN"}, {"HUNGARY", "HUN"},
        {"SI", "SVN"}, {"SVN", "SVN"}, {"SLOVENIA", "SVN"},
        {"HR", "HRV"}, {"HRV", "HRV"}, {"CROATIA", "HRV"},
        {"RO", "ROU"}, {"ROU", "ROU"}, {"ROMANIA", "ROU"},
        {"GR", "GRC"}, {"GRC", "GRC"}, {"GREECE", "GRC"},
        {"TR", "TUR"}, {"TUR", "TUR"}, {"TURKEY", "TUR"},
        {"UA", "UKR"}, {"UKR", "UKR"}, {"UKRAINE", "UKR"},
        {"RU", "RUS"}, {"RUS", "RUS"}, {"RUSSIA", "RUS"},
        {"IT", "ITA"}, {"ITA", "ITA"}, {"ITALY", "ITA"},
        {"AU", "AUS"}, {"AUS", "AUS"}, {"AUSTRALIA", "AUS"},
        {"NZ", "NZL"}, {"NZL", "NZL"}, {"NEWZEALAND", "NZL"},
        {"JP", "JPN"}, {"JPN", "JPN"}, {"JAPAN", "JPN"},
        {"KR", "KOR"}, {"KOR", "KOR"}, {"KOREA", "KOR"},
        {"SOUTHKOREA", "KOR"},
        {"CN", "CHN"}, {"CHN", "CHN"}, {"CHINA", "CHN"},
        {"TW", "TWN"}, {"TWN", "TWN"}, {"TAIWAN", "TWN"},
        {"IN", "IND"}, {"IND", "IND"}, {"INDIA", "IND"},
        {"ID", "IDN"}, {"IDN", "IDN"}, {"INDONESIA", "IDN"},
        {"ZA", "ZAF"}, {"ZAF", "ZAF"}, {"SOUTHAFRICA", "ZAF"},
        {"EG", "EGY"}, {"EGY", "EGY"}, {"EGYPT", "EGY"},
        {"IL", "ISR"}, {"ISR", "ISR"}, {"ISRAEL", "ISR"},
        {"MK", "MKD"}, {"MKD", "MKD"}, {"NORTHMACEDONIA", "MKD"},
        {"CY", "CYP"}, {"CYP", "CYP"}, {"CYPRUS", "CYP"}
    };
    if (const auto found = aliases.find(value); found != aliases.end()) {
        return std::string(found->second);
    }
    // Site Log country fields occasionally retain the parenthetical article
    // or a longer official name.  Prefix matching is restricted to names
    // whose canonical code is already unambiguous above.
    if (value.starts_with("UNITEDSTATESOFAMERICA")) return "USA";
    if (value.starts_with("UNITEDSTATES")) return "USA";
    return value;
}

std::string SiteLogIdCountrySuffix(const std::string& stationId) {
    if (stationId.size() < 3) return {};
    std::string suffix = UpperAscii(
        stationId.substr(stationId.size() - 3));
    if (suffix.size() != 3
        || !std::all_of(suffix.begin(), suffix.end(),
            [](const unsigned char character) {
                return std::isalpha(character) != 0;
            })) {
        return {};
    }
    return suffix;
}

bool CountriesEquivalent(
    const IdentityAggregate& identity,
    const SiteLogRecord& candidate) {
    if (identity.country.empty() || candidate.country.empty()) return true;
    const std::string left = CanonicalCountry(identity.country);
    const std::string right = CanonicalCountry(candidate.country);
    if (left == right) return true;
    // A full IGS-style ID ends in the alpha-3 country code even when the
    // textual country field uses an alpha-2 code or a name.
    const std::string candidateSuffix =
        SiteLogIdCountrySuffix(candidate.stationId);
    if (!candidateSuffix.empty() && left == candidateSuffix) return true;
    const std::string identitySuffix =
        SiteLogIdCountrySuffix(identity.stationId);
    if (!identitySuffix.empty() && right == identitySuffix) return true;
    return false;
}

const SiteLogRecord* FindSiteLogRecordForIdentity(
    const SiteLogLookup& lookup,
    const IdentityAggregate& identity,
    bool& exactStationId) {
    exactStationId = false;
    // A full station ID is authoritative.  Do not use a four-character alias
    // as an "exact" hit when the identity itself only has that prefix.
    if (identity.stationId.size() > 4) {
        const auto found = lookup.byStationId.find(
            UpperAscii(identity.stationId));
        if (found != lookup.byStationId.end() && !found->second.empty()) {
            exactStationId = true;
            return BestSiteLogCandidate(found->second);
        }
    }
    if (identity.fourCharId.empty()) return nullptr;
    const auto found = lookup.byFourChar.find(UpperAscii(identity.fourCharId));
    if (found == lookup.byFourChar.end()) return nullptr;

    std::vector<const SiteLogRecord*> countryFiltered;
    countryFiltered.reserve(found->second.size());
    for (const auto* candidate : found->second) {
        if (candidate == nullptr) continue;
        if (!CountriesEquivalent(identity, *candidate)) {
            continue;
        }
        countryFiltered.push_back(candidate);
    }
    if (countryFiltered.empty()) return nullptr;

    // Collapse revisions/aliases of one physical station, but reject a
    // reused four-character code when it resolves to multiple sites.
    std::vector<const SiteLogRecord*> physicalCandidates;
    for (const auto* candidate : countryFiltered) {
        const auto equivalent = std::find_if(
            physicalCandidates.begin(), physicalCandidates.end(),
            [&](const auto* existing) {
                return SameSiteLogPhysicalCandidate(*existing, *candidate);
            });
        if (equivalent == physicalCandidates.end()) {
            physicalCandidates.push_back(candidate);
        }
    }
    if (physicalCandidates.size() != 1) return nullptr;
    std::vector<const SiteLogRecord*> onePhysical;
    for (const auto* candidate : countryFiltered) {
        if (SameSiteLogPhysicalCandidate(
                *physicalCandidates.front(), *candidate)) {
            onePhysical.push_back(candidate);
        }
    }
    const auto* best = BestSiteLogCandidate(onePhysical);
    if (best == nullptr) return nullptr;
    // A unique four-character match is still rejected when it is demonstrably
    // far from an already-known daily coordinate.  Keep the same 3 km
    // physical-station tolerance used when collapsing raw candidates; this
    // protects against a reused prefix in a malformed/legacy archive while
    // retaining the useful fallback for rows whose daily position is absent.
    if (identity.hasCoordinate) {
        const auto candidateCoordinate = ResolveSiteLogCoordinate(*best);
        if (candidateCoordinate.has_value()
            && DistanceKm(identity.latitude, identity.longitude,
                          candidateCoordinate->latitude,
                          candidateCoordinate->longitude) > 3.0) {
            return nullptr;
        }
    }
    return best;
}

void AppendSiteLogMap(
    BuildMemory& memory,
    const ObservationNetworkMask identityMask,
    const ObservationNetworkMask applicabilityMask,
    const int tier,
    const int networkOrder,
    const std::unordered_map<std::string, SiteLogRecord>& records,
    const std::vector<SiteLogRecord>& coordinateRecords,
    const bool coordinateLookupAvailable) {
    if (records.empty() && coordinateRecords.empty()) return;
    const SiteLogLookup metadataLookup = BuildSiteLogLookup(records);
    const SiteLogLookup coordinateLookup =
        BuildSiteLogLookup(coordinateRecords);
    for (std::size_t index = 0; index < memory.identities.size(); ++index) {
        auto& identity = memory.identities[index];
        if (identity.networkMask != identityMask) continue;
        bool metadataExact = false;
        const auto* record = FindSiteLogRecordForIdentity(
            metadataLookup, identity, metadataExact);
        bool coordinateExact = false;
        const auto* coordinateRecord = FindSiteLogRecordForIdentity(
            coordinateLookup, identity, coordinateExact);
        (void)coordinateExact;
        if (record != nullptr && IsKnownRetiredCodeReuse(*record)) {
            record = nullptr;
        }
        if (coordinateRecord != nullptr
            && IsKnownRetiredCodeReuse(*coordinateRecord)) {
            coordinateRecord = nullptr;
        }
        if (record == nullptr && coordinateRecord == nullptr) continue;

        // Site Log identifiers/country are useful when a daily row only had a
        // four-character key.  UpdateIdentity never replaces a better full
        // daily identifier; coordinate provenance is applied separately so a
        // Site Log tier cannot outrank daily_files coordinates.
        if (record != nullptr) {
            UpdateIdentity(
                identity, record->stationId, record->fourCharId,
                record->country, false, 0.0, 0.0);
        }

        // Prefer the raw row for coordinates.  Unlike the summary map, the
        // raw lookup still contains every full station ID and therefore can
        // reject a reused four-character code instead of inheriting whichever
        // row happened to be canonicalized first.
        const SiteLogRecord* coordinateSource = coordinateRecord;
        std::optional<ResolvedSiteLogCoordinate> coordinate;
        if (coordinateSource != nullptr) {
            coordinate = ResolveSiteLogCoordinate(*coordinateSource);
        }
        if (!coordinate.has_value() && record != nullptr
            // A full-ID summary hit is unambiguous even when the raw row has
            // no usable position.  If the raw coordinate lookup failed,
            // retain the historical summary fallback for compatibility.
            && (metadataExact || !coordinateLookupAvailable)) {
            coordinate = ResolveSiteLogCoordinate(*record);
        }
        if (coordinate.has_value()) {
            ApplyIdentityCoordinate(identity, *coordinate, tier);
        }

        // A raw coordinate row can repair the identity even if no canonical
        // summary record exists.  It cannot, however, provide device/site
        // metadata for the materialized site-log table, so only emit a
        // PendingSiteRecord when the summary lookup found a row.
        if (record == nullptr) continue;
        PendingSiteRecord pending;
        pending.identity = index;
        pending.applicabilityMask = applicabilityMask;
        pending.tier = tier;
        pending.networkOrder = networkOrder;
        pending.exactStationId = metadataExact;
        pending.record = *record;
        if (coordinate.has_value()) {
            pending.record.latitude = coordinate->latitude;
            pending.record.longitude = coordinate->longitude;
            pending.record.hasCoordinate = true;
        } else if (coordinateLookupAvailable && !metadataExact) {
            // The summary map may have retained a coordinate from an
            // ambiguous four-character alias.  Once the raw lookup is
            // available, absence of a safe raw candidate is authoritative:
            // do not leak that canonicalized (possibly foreign) position
            // into site_records even though the scalar metadata is retained.
            pending.record.latitude = 0.0;
            pending.record.longitude = 0.0;
            pending.record.hasCoordinate = false;
        } else if (!ValidLatitudeLongitude(
                       pending.record.latitude, pending.record.longitude)) {
            // A malformed LLH flag should not make a Site Log look like it
            // supplied a usable coordinate in quality comparisons.
            pending.record.hasCoordinate = false;
        }
        // The summary loader intentionally keeps history but raw source text
        // is never useful to the range-search cache.
        pending.record.rawText.clear();
        memory.siteRecords.push_back(std::move(pending));
    }
}

void ScanSiteLogsForSource(
    const std::filesystem::path& path,
    const SourceSpec& source,
    BuildMemory& memory,
    std::vector<std::wstring>& warnings) {
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(path, filesystemError)) return;
    std::unordered_map<std::string, SiteLogRecord> records;
    std::vector<SiteLogRecord> coordinateRecords;
    std::wstring error;
    if (source.igs) {
        const auto sharedMask =
            NetworkBit(ObservationNetwork::Igs)
            | NetworkBit(ObservationNetwork::Mgex);
        std::wstring coordinateError;
        const bool coordinateLookupAvailable =
            LoadRawSiteLogCoordinateRecords(
                path, sharedMask, coordinateRecords, coordinateError);
        if (!coordinateLookupAvailable && !coordinateError.empty()) {
            warnings.push_back(L"IGS/MGEX Site Log 坐标读取：" + coordinateError);
        }
        if (LoadSiteLogSummaryMap(path, "", records, error, 0)) {
            AppendSiteLogMap(
                memory, sharedMask, sharedMask, 2,
                static_cast<int>(kSiteLogRegionalPrecedence.size()), records,
                coordinateRecords, coordinateLookupAvailable);
        } else if (!error.empty()) {
            warnings.push_back(L"IGS/MGEX Site Log：" + error);
        }
        return;
    }

    const auto networkMask = NetworkBit(source.network);
    std::wstring coordinateError;
    bool coordinateLookupAvailable = LoadRawSiteLogCoordinateRecords(
        path, networkMask, coordinateRecords, coordinateError);
    if (!coordinateLookupAvailable && !coordinateError.empty()) {
        warnings.push_back(std::wstring(source.label)
            + L" 原生 Site Log 坐标读取：" + coordinateError);
    }
    if (LoadSiteLogSummaryMap(path, "", records, error, networkMask)) {
        AppendSiteLogMap(
            memory, networkMask, networkMask, 0,
            SiteLogNetworkOrder(source.network), records, coordinateRecords,
            coordinateLookupAvailable);
    } else if (!error.empty()) {
        warnings.push_back(std::wstring(source.label)
            + L" 原生 Site Log：" + error);
    }
    records.clear();
    coordinateRecords.clear();
    error.clear();
    coordinateError.clear();
    const auto inheritedMask = NetworkBit(ObservationNetwork::Igs)
        | NetworkBit(ObservationNetwork::Mgex);
    coordinateLookupAvailable = LoadRawSiteLogCoordinateRecords(
        path, inheritedMask, coordinateRecords, coordinateError);
    if (!coordinateLookupAvailable && !coordinateError.empty()) {
        warnings.push_back(std::wstring(source.label)
            + L" 继承 Site Log 坐标读取：" + coordinateError);
    }
    if (LoadSiteLogSummaryMap(path, "", records, error, inheritedMask)) {
        AppendSiteLogMap(
            memory, networkMask, networkMask, 1,
            SiteLogNetworkOrder(source.network), records, coordinateRecords,
            coordinateLookupAvailable);
    } else if (!error.empty()) {
        warnings.push_back(std::wstring(source.label)
            + L" 继承 Site Log：" + error);
    }
}

struct DisjointSet {
    explicit DisjointSet(const std::size_t size) : parent(size), rank(size, 0) {
        for (std::size_t index = 0; index < size; ++index) parent[index] = index;
    }
    std::size_t Find(const std::size_t value) {
        if (parent[value] != value) parent[value] = Find(parent[value]);
        return parent[value];
    }
    void Unite(std::size_t left, std::size_t right) {
        left = Find(left); right = Find(right);
        if (left == right) return;
        if (rank[left] < rank[right]) std::swap(left, right);
        parent[right] = left;
        if (rank[left] == rank[right]) ++rank[left];
    }
    std::vector<std::size_t> parent;
    std::vector<unsigned char> rank;
};

struct UnifiedStation {
    IdentityAggregate representative;
    ObservationNetworkMask networks = 0;
    bool hasPreferredCoordinate = false;
    int preferredCoordinateRank = std::numeric_limits<int>::max();
    double preferredLatitude = 0.0;
    double preferredLongitude = 0.0;
};

// Four-character identifiers are not globally unique.  Regional archives
// routinely reuse a code for an unrelated physical site (for example OKAK in
// NGS versus SOPAC/GARNER, and STAR in GA-GDC versus SOPAC/PANGA).  Keep the
// cross-network merge deliberately conservative: a short code may only be
// merged when the two observations provide a reliable, nearby coordinate or
// when an explicit alias is listed below.  Same-network aliases remain
// mergeable because they originate from one archive's own identity layer.
bool IsShortStationId(const std::string& stationId) {
    return !stationId.empty() && stationId.size() <= 4;
}

bool HasReliableCoordinate(const IdentityAggregate& identity) {
    return identity.hasCoordinate
        && ValidLatitudeLongitude(identity.latitude, identity.longitude)
        && !(std::abs(identity.latitude) < 1.0e-12
            && std::abs(identity.longitude) < 1.0e-12);
}

bool CoordinatesIdentifySameSite(
    const IdentityAggregate& left,
    const IdentityAggregate& right) {
    return HasReliableCoordinate(left) && HasReliableCoordinate(right)
        && DistanceKm(
            left.latitude, left.longitude,
            right.latitude, right.longitude) <= 3.0;
}

bool IsExactNetworkMask(
    const ObservationNetworkMask value,
    const ObservationNetwork network) {
    return value == NetworkBit(network);
}

bool IsExplicitPhysicalAlias(
    const IdentityAggregate& left,
    const IdentityAggregate& right) {
    const std::string leftFour = UpperAscii(left.fourCharId);
    const std::string rightFour = UpperAscii(right.fourCharId);
    if (leftFour != "STAR" || rightFour != "STAR") return false;

    // SOPAC/GARNER and PANGA/CWU publish the same St. Andrews Rock station
    // under the reused four-character code STAR.  This explicit pair is
    // intentionally narrow; GA-GDC STAR is a different Australian site.
    const bool leftSopac = IsExactNetworkMask(
        left.networkMask, ObservationNetwork::SopacGarner);
    const bool rightSopac = IsExactNetworkMask(
        right.networkMask, ObservationNetwork::SopacGarner);
    const bool leftPanga = IsExactNetworkMask(
        left.networkMask, ObservationNetwork::PangaCwu);
    const bool rightPanga = IsExactNetworkMask(
        right.networkMask, ObservationNetwork::PangaCwu);
    return (leftSopac && rightPanga) || (leftPanga && rightSopac);
}

bool CanUniteIdentityPair(
    const IdentityAggregate& left,
    const IdentityAggregate& right) {
    if (left.networkMask == right.networkMask) {
        // FindOrAddIdentity normally already collapses this case.  Keep the
        // rule for source rows that expose both a full and a FOUR alias.
        if (!left.stationId.empty() && !right.stationId.empty()
            && UpperAscii(left.stationId) == UpperAscii(right.stationId)) {
            return true;
        }
        // Preserve the historical within-network coordinate alias rule for
        // rows whose archive changed the suffix/alias over time.  The strict
        // short-code guard below applies only when the network masks differ.
        const std::string leftFour = UpperAscii(left.fourCharId);
        const std::string rightFour = UpperAscii(right.fourCharId);
        return !leftFour.empty() && leftFour == rightFour
            && CoordinatesIdentifySameSite(left, right);
    }

    const bool shortIdentifier = IsShortStationId(left.stationId)
        || IsShortStationId(right.stationId)
        || left.stationId.empty() || right.stationId.empty();
    if (shortIdentifier) {
        const std::string leftFour = UpperAscii(left.fourCharId);
        const std::string rightFour = UpperAscii(right.fourCharId);
        if (leftFour.empty() || leftFour != rightFour) return false;
        // A short/empty identifier is not globally unique.  A reliable
        // nearby coordinate is the normal proof that two archive rows refer
        // to one monument; a narrowly curated explicit mapping covers the
        // rare case where one of those rows has no usable position.
        return CoordinatesIdentifySameSite(left, right)
            || IsExplicitPhysicalAlias(left, right);
    }

    // Full (nine-character) IGS-style identifiers are designed to be global
    // station identities.  Preserve the established exact-ID merge rule.
    return !left.stationId.empty() && !right.stationId.empty()
        && UpperAscii(left.stationId) == UpperAscii(right.stationId);
}

void ConsiderPreferredCoordinate(
    UnifiedStation& station,
    const IdentityAggregate& identity) {
    if (!identity.hasCoordinate
        || !ValidLatitudeLongitude(identity.latitude, identity.longitude)) {
        return;
    }
    if (station.hasPreferredCoordinate
        && identity.coordinateSourceRank >= station.preferredCoordinateRank) {
        return;
    }
    station.hasPreferredCoordinate = true;
    station.preferredCoordinateRank = identity.coordinateSourceRank;
    station.preferredLatitude = identity.latitude;
    station.preferredLongitude = identity.longitude;
}

void ApplyPreferredCoordinate(UnifiedStation& station) {
    if (!station.hasPreferredCoordinate) return;
    station.representative.hasCoordinate = true;
    station.representative.latitude = station.preferredLatitude;
    station.representative.longitude = station.preferredLongitude;
    station.representative.coordinateSourceRank =
        station.preferredCoordinateRank;
}

int IdentityQuality(const IdentityAggregate& identity) {
    return (identity.hasCoordinate ? 8 : 0)
        + (identity.stationId.size() == 9 ? 4 : 0)
        + (!identity.country.empty() ? 2 : 0)
        + (!identity.fourCharId.empty() ? 1 : 0);
}

struct UnifiedProfileKey {
    std::size_t station = 0;
    ObservationNetworkMask networkMask = 0;
    int rinexFormat = 0;
    std::string interval;
    friend bool operator==(const UnifiedProfileKey&, const UnifiedProfileKey&) = default;
};

struct UnifiedProfileKeyHash {
    std::size_t operator()(const UnifiedProfileKey& value) const noexcept {
        std::size_t result = std::hash<std::size_t>{}(value.station);
        result ^= std::hash<std::uint64_t>{}(value.networkMask) + (result << 6U);
        result ^= std::hash<int>{}(value.rinexFormat) + (result << 6U);
        result ^= std::hash<std::string>{}(value.interval) + (result << 6U);
        return result;
    }
};

struct UnifiedProfile {
    UnifiedProfileKey key;
    std::vector<BitmapBlock> blocks;
    int minimumDay = std::numeric_limits<int>::max();
    int maximumDay = std::numeric_limits<int>::min();
};

struct MaterializedSiteSegment {
    int kind = 0; // 0 receiver, 1 antenna
    int firstDay = 0;
    int lastDayExclusive = 0;
    std::string type;
    std::string systems;
    std::string serial;
    std::string firmware;
    std::string radome;
    std::string referencePoint;
    std::string installedDate;
};

struct MaterializedSiteRecord {
    std::size_t station = 0;
    ObservationNetworkMask applicabilityMask = 0;
    int tier = 0;
    int networkOrder = 0;
    bool exactStationId = false;
    SiteLogRecord source;
    std::vector<MaterializedSiteSegment> segments;
};

struct MaterializedSiteKey {
    std::size_t station = 0;
    ObservationNetworkMask applicabilityMask = 0;
    int tier = 0;
    friend bool operator==(const MaterializedSiteKey&, const MaterializedSiteKey&) = default;
};

struct MaterializedSiteKeyHash {
    std::size_t operator()(const MaterializedSiteKey& value) const noexcept {
        std::size_t hash = std::hash<std::size_t>{}(value.station);
        hash ^= std::hash<std::uint64_t>{}(value.applicabilityMask)
            + (hash << 6U);
        hash ^= std::hash<int>{}(value.tier) + (hash << 6U);
        return hash;
    }
};

std::string OrdinalDateText(const int ordinal) {
    const auto day = std::chrono::sys_days{
        std::chrono::days{static_cast<long long>(ordinal)}};
    const std::chrono::year_month_day ymd{day};
    if (!ymd.ok()) return {};
    return std::format(
        "{:04}-{:02}-{:02}",
        static_cast<int>(ymd.year()),
        static_cast<unsigned>(ymd.month()),
        static_cast<unsigned>(ymd.day()));
}

bool SegmentSameValue(
    const MaterializedSiteSegment& left,
    const MaterializedSiteSegment& right) {
    return left.kind == right.kind && left.type == right.type
        && left.systems == right.systems && left.serial == right.serial
        && left.firmware == right.firmware && left.radome == right.radome
        && left.referencePoint == right.referencePoint
        && left.installedDate == right.installedDate;
}

void AppendMaterializedSegment(
    std::vector<MaterializedSiteSegment>& segments,
    MaterializedSiteSegment segment) {
    if (segment.firstDay >= segment.lastDayExclusive) return;
    if (segment.type.empty() && segment.systems.empty()
        && segment.serial.empty() && segment.firmware.empty()
        && segment.radome.empty() && segment.referencePoint.empty()) {
        return;
    }
    if (!segments.empty()
        && segments.back().lastDayExclusive == segment.firstDay
        && SegmentSameValue(segments.back(), segment)) {
        segments.back().lastDayExclusive = segment.lastDayExclusive;
        return;
    }
    segments.push_back(std::move(segment));
}

std::vector<MaterializedSiteSegment> MaterializeSiteSegments(
    const SiteLogRecord& record,
    const int minimumDay,
    const int maximumDay) {
    std::vector<MaterializedSiteSegment> segments;
    if (maximumDay < minimumDay) return segments;
    const int lastExclusive = maximumDay == std::numeric_limits<int>::max()
        ? maximumDay : maximumDay + 1;
    std::vector<int> boundaries{minimumDay, lastExclusive};
    for (const auto& epoch : record.equipmentHistory) {
        int value = 0;
        if (!epoch.dateInstalled.empty()
            && TextDateToOrdinal(epoch.dateInstalled, value)
            && value > minimumDay && value < lastExclusive) {
            boundaries.push_back(value);
        }
        if (!epoch.dateRemoved.empty()
            && TextDateToOrdinal(epoch.dateRemoved, value)
            && value > minimumDay && value < lastExclusive) {
            boundaries.push_back(value);
        }
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                     boundaries.end());
    for (std::size_t index = 0; index + 1 < boundaries.size(); ++index) {
        const int first = boundaries[index];
        const int last = boundaries[index + 1];
        SiteLogRecord snapshot;
        ApplySiteLogEquipmentAsOf(record, OrdinalDateText(first), snapshot);
        MaterializedSiteSegment receiver;
        receiver.kind = 0;
        receiver.firstDay = first;
        receiver.lastDayExclusive = last;
        receiver.type = snapshot.receiverType;
        receiver.systems = snapshot.receiverSystems;
        receiver.serial = snapshot.receiverSerial;
        receiver.firmware = snapshot.receiverFirmware;
        receiver.installedDate = snapshot.receiverDateInstalled;
        AppendMaterializedSegment(segments, std::move(receiver));

        MaterializedSiteSegment antenna;
        antenna.kind = 1;
        antenna.firstDay = first;
        antenna.lastDayExclusive = last;
        antenna.type = snapshot.antennaType;
        antenna.serial = snapshot.antennaSerial;
        antenna.radome = snapshot.antennaRadome;
        antenna.referencePoint = snapshot.antennaReferencePoint;
        antenna.installedDate = snapshot.antennaDateInstalled;
        AppendMaterializedSegment(segments, std::move(antenna));
    }
    return segments;
}

int SiteRecordQuality(const MaterializedSiteRecord& record) {
    const auto& value = record.source;
    return (record.exactStationId ? 1000 : 0)
        + (!value.domes.empty() ? 8 : 0)
        + (value.hasCoordinate ? 4 : 0)
        + (!value.receiverType.empty() ? 2 : 0)
        + (!value.antennaType.empty() ? 1 : 0);
}

void MergeBlocks(
    std::vector<BitmapBlock>& destination,
    const std::vector<BitmapBlock>& source) {
    for (const auto& sourceBlock : source) {
        auto& target = FindOrAddBlock(destination, sourceBlock.number);
        for (std::size_t word = 0; word < target.words.size(); ++word) {
            target.words[word] |= sourceBlock.words[word];
        }
    }
}

bool EnsureOutputSchema(sqlite3* database, std::wstring& error) {
    std::string schema =
        "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE stations("
        " station_uid INTEGER PRIMARY KEY,station_id TEXT NOT NULL DEFAULT '',"
        " four_char_id TEXT NOT NULL DEFAULT '',country TEXT NOT NULL DEFAULT '',"
        " has_coordinate INTEGER NOT NULL,latitude REAL,longitude REAL,"
        " available_networks INTEGER NOT NULL);"
        "CREATE TABLE station_aliases("
        " network_mask INTEGER NOT NULL,station_key TEXT NOT NULL,"
        " station_uid INTEGER NOT NULL,station_id TEXT NOT NULL DEFAULT '',"
        " four_char_id TEXT NOT NULL DEFAULT '',"
        " PRIMARY KEY(network_mask,station_key)) WITHOUT ROWID;"
        "CREATE TABLE profiles("
        " profile_id INTEGER PRIMARY KEY,station_uid INTEGER NOT NULL,"
        " network_mask INTEGER NOT NULL,rinex_format INTEGER NOT NULL,"
        " sample_interval TEXT NOT NULL,minimum_day INTEGER NOT NULL,"
        " maximum_day INTEGER NOT NULL,available_days INTEGER NOT NULL);"
        "CREATE INDEX profiles_station ON profiles(station_uid);"
        "CREATE INDEX profiles_filter ON profiles(network_mask,rinex_format,sample_interval);"
        "CREATE TABLE profile_blocks("
        " profile_id INTEGER NOT NULL,block_number INTEGER NOT NULL,bits BLOB NOT NULL,"
        " PRIMARY KEY(profile_id,block_number)) WITHOUT ROWID;"
        "CREATE TABLE coverage_blocks("
        " network_mask INTEGER NOT NULL,block_number INTEGER NOT NULL,bits BLOB NOT NULL,"
        " PRIMARY KEY(network_mask,block_number)) WITHOUT ROWID;"
        "CREATE TABLE source_state("
        " network_mask INTEGER NOT NULL,source_path TEXT NOT NULL,"
        " file_size INTEGER NOT NULL,file_mtime INTEGER NOT NULL,"
        " wal_size INTEGER NOT NULL,wal_mtime INTEGER NOT NULL,"
        " fact_rows INTEGER NOT NULL,maximum_date TEXT NOT NULL DEFAULT '',"
        " PRIMARY KEY(network_mask,source_path)) WITHOUT ROWID;"
        "CREATE TABLE site_records("
        " record_id INTEGER PRIMARY KEY,station_uid INTEGER NOT NULL,"
        " applicability_mask INTEGER NOT NULL,tier INTEGER NOT NULL,"
        " network_order INTEGER NOT NULL,domes TEXT NOT NULL DEFAULT '',"
        " site_name TEXT NOT NULL DEFAULT '',city TEXT NOT NULL DEFAULT '',"
        " country TEXT NOT NULL DEFAULT '',agency TEXT NOT NULL DEFAULT '',"
        " operator_name TEXT NOT NULL DEFAULT '',revision_date TEXT NOT NULL DEFAULT '',"
        " source_directory TEXT NOT NULL DEFAULT '',source_url TEXT NOT NULL DEFAULT '');"
        "CREATE INDEX site_records_station ON site_records(station_uid,tier,network_order);"
        "CREATE TABLE site_segments("
        " record_id INTEGER NOT NULL,kind INTEGER NOT NULL,first_day INTEGER NOT NULL,"
        " last_day_exclusive INTEGER NOT NULL,equipment_type TEXT NOT NULL DEFAULT '',"
        " systems TEXT NOT NULL DEFAULT '',serial TEXT NOT NULL DEFAULT '',"
        " firmware TEXT NOT NULL DEFAULT '',radome TEXT NOT NULL DEFAULT '',"
        " reference_point TEXT NOT NULL DEFAULT '',installed_date TEXT NOT NULL DEFAULT '',"
        " PRIMARY KEY(record_id,kind,first_day,last_day_exclusive)) WITHOUT ROWID;"
        "CREATE INDEX site_segments_dates ON site_segments(first_day,last_day_exclusive);";
#ifdef GNSS_DOWNLOADER_DUCKDB_BACKEND
    for (const auto& [from, to] : {std::pair<std::string_view, std::string_view>{" INTEGER", " BIGINT"},
                                 {" REAL", " DOUBLE"}, {" WITHOUT ROWID", ""}}) {
        std::size_t offset = 0;
        while ((offset = schema.find(from, offset)) != std::string::npos) {
            schema.replace(offset, from.size(), to);
            offset += to.size();
        }
    }
#endif
    return Execute(database, schema.c_str(), error);
}

bool StepDone(sqlite3* database, sqlite3_stmt* statement, std::wstring& error) {
    if (sqlite3_step(statement) == SQLITE_DONE) return true;
    error = SqliteError(database, L"写入高速搜索索引失败");
    return false;
}

bool WriteOutput(
    sqlite3* database,
    BuildMemory& memory,
    ObservationSearchBuildResult& result,
    std::wstring& error) {
    DisjointSet groups(memory.identities.size());
    std::unordered_map<std::string, std::vector<std::size_t>> byStationId;
    std::unordered_map<std::string, std::vector<std::size_t>> byFourChar;
    for (std::size_t index = 0; index < memory.identities.size(); ++index) {
        const auto& identity = memory.identities[index];
        if (!identity.stationId.empty()) {
            const std::string id = UpperAscii(identity.stationId);
            byStationId[id].push_back(index);
        }
        if (!identity.fourCharId.empty()) {
            byFourChar[UpperAscii(identity.fourCharId)].push_back(index);
        }
    }
    const auto uniteBuckets = [&](const auto& buckets) {
        for (const auto& [key, candidates] : buckets) {
            (void)key;
            for (std::size_t left = 0; left < candidates.size(); ++left) {
                for (std::size_t right = left + 1;
                     right < candidates.size(); ++right) {
                    const auto& a = memory.identities[candidates[left]];
                    const auto& b = memory.identities[candidates[right]];
                    if (CanUniteIdentityPair(a, b)) {
                        groups.Unite(candidates[left], candidates[right]);
                    }
                }
            }
        }
    };
    // The station-ID buckets catch full-ID aliases as well as short IDs.  The
    // four-character buckets are needed when one source stores only FOUR:XXXX
    // while another stores a full station ID or an explicit regional alias.
    uniteBuckets(byStationId);
    uniteBuckets(byFourChar);

    std::unordered_map<std::size_t, std::size_t> stationByRoot;
    std::vector<std::size_t> stationByIdentity(memory.identities.size());
    std::vector<UnifiedStation> stations;
    for (std::size_t index = 0; index < memory.identities.size(); ++index) {
        const std::size_t root = groups.Find(index);
        std::size_t stationIndex = 0;
        if (const auto found = stationByRoot.find(root); found != stationByRoot.end()) {
            stationIndex = found->second;
        } else {
            stationIndex = stations.size();
            stationByRoot.emplace(root, stationIndex);
            UnifiedStation unified;
            unified.representative = memory.identities[index];
            unified.networks = 0;
            ConsiderPreferredCoordinate(unified, memory.identities[index]);
            ApplyPreferredCoordinate(unified);
            stations.push_back(std::move(unified));
        }
        stationByIdentity[index] = stationIndex;
        auto& unified = stations[stationIndex];
        const auto& identity = memory.identities[index];
        unified.networks |= identity.networkMask;
        ConsiderPreferredCoordinate(unified, identity);
        if (IdentityQuality(identity) > IdentityQuality(unified.representative)) {
            unified.representative = identity;
        }
        auto& representative = unified.representative;
        if (representative.stationId.empty()) representative.stationId = identity.stationId;
        if (representative.fourCharId.empty()) representative.fourCharId = identity.fourCharId;
        if (representative.country.empty()) representative.country = identity.country;
        // Keep the best coordinate independently of the metadata
        // representative.  A full-ID Site Log row can win IdentityQuality,
        // but a valid daily_files position must still remain authoritative.
        ApplyPreferredCoordinate(unified);
    }

    std::vector<UnifiedProfile> profiles;
    std::unordered_map<UnifiedProfileKey, std::size_t, UnifiedProfileKeyHash> profileMap;
    for (const auto& original : memory.profiles) {
        UnifiedProfileKey key{
            stationByIdentity[original.key.identity], original.key.networkMask,
            original.key.rinexFormat, original.key.interval};
        std::size_t index = 0;
        if (const auto found = profileMap.find(key); found != profileMap.end()) {
            index = found->second;
        } else {
            index = profiles.size();
            profiles.push_back(UnifiedProfile{key});
            profileMap.emplace(std::move(key), index);
        }
        auto& profile = profiles[index];
        MergeBlocks(profile.blocks, original.blocks);
        profile.minimumDay = std::min(profile.minimumDay, original.minimumDay);
        profile.maximumDay = std::max(profile.maximumDay, original.maximumDay);
    }

    int observationMinimum = std::numeric_limits<int>::max();
    int observationMaximum = std::numeric_limits<int>::min();
    for (const auto& profile : profiles) {
        observationMinimum = std::min(observationMinimum, profile.minimumDay);
        observationMaximum = std::max(observationMaximum, profile.maximumDay);
    }
    std::vector<MaterializedSiteRecord> siteRecords;
    std::unordered_map<MaterializedSiteKey, std::size_t,
        MaterializedSiteKeyHash> siteRecordByKey;
    for (const auto& pending : memory.siteRecords) {
        if (pending.identity >= stationByIdentity.size()) continue;
        MaterializedSiteRecord candidate;
        candidate.station = stationByIdentity[pending.identity];
        candidate.applicabilityMask = pending.applicabilityMask;
        candidate.tier = pending.tier;
        candidate.networkOrder = pending.networkOrder;
        candidate.exactStationId = pending.exactStationId;
        candidate.source = pending.record;
        const MaterializedSiteKey key{
            candidate.station, candidate.applicabilityMask, candidate.tier};
        const auto found = siteRecordByKey.find(key);
        if (found == siteRecordByKey.end()) {
            siteRecordByKey.emplace(key, siteRecords.size());
            siteRecords.push_back(std::move(candidate));
            continue;
        }
        auto& current = siteRecords[found->second];
        const int candidateQuality = SiteRecordQuality(candidate);
        const int currentQuality = SiteRecordQuality(current);
        if (candidateQuality > currentQuality
            || (candidateQuality == currentQuality
                && std::tie(candidate.source.revisionDate,
                            candidate.source.fetchedAtUtc,
                            candidate.source.sourceUrl)
                    > std::tie(current.source.revisionDate,
                               current.source.fetchedAtUtc,
                               current.source.sourceUrl))) {
            current = std::move(candidate);
        }
    }
    std::sort(siteRecords.begin(), siteRecords.end(),
        [](const MaterializedSiteRecord& left,
           const MaterializedSiteRecord& right) {
            return std::tie(left.station, left.tier, left.networkOrder,
                            left.applicabilityMask)
                < std::tie(right.station, right.tier, right.networkOrder,
                           right.applicabilityMask);
        });
    if (observationMinimum <= observationMaximum) {
        for (auto& record : siteRecords) {
            record.segments = MaterializeSiteSegments(
                record.source, observationMinimum, observationMaximum);
        }
    }

    if (!Execute(database, "BEGIN;", error)) return false;
    const auto rollback = [&]() {
        std::wstring ignored;
        Execute(database, "ROLLBACK;", ignored);
    };
    auto metadata = Prepare(database,
        "INSERT INTO metadata(key,value) VALUES(?1,?2);", error);
    auto stationStatement = Prepare(database,
        "INSERT INTO stations(station_uid,station_id,four_char_id,country,"
        "has_coordinate,latitude,longitude,available_networks)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8);", error);
    auto aliasStatement = Prepare(database,
        "INSERT INTO station_aliases(network_mask,station_key,station_uid,station_id,four_char_id)"
        " VALUES(?1,?2,?3,?4,?5);", error);
    auto profileStatement = Prepare(database,
        "INSERT INTO profiles(profile_id,station_uid,network_mask,rinex_format,"
        "sample_interval,minimum_day,maximum_day,available_days)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8);", error);
    auto blockStatement = Prepare(database,
        "INSERT INTO profile_blocks(profile_id,block_number,bits) VALUES(?1,?2,?3);", error);
    auto coverageStatement = Prepare(database,
        "INSERT INTO coverage_blocks(network_mask,block_number,bits) VALUES(?1,?2,?3);", error);
    auto sourceStatement = Prepare(database,
        "INSERT INTO source_state(network_mask,source_path,file_size,file_mtime,"
        "wal_size,wal_mtime,fact_rows,maximum_date) VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
        error);
    auto siteRecordStatement = Prepare(database,
        "INSERT INTO site_records(record_id,station_uid,applicability_mask,tier,"
        "network_order,domes,site_name,city,country,agency,operator_name,"
        "revision_date,source_directory,source_url)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14);", error);
    auto siteSegmentStatement = Prepare(database,
        "INSERT INTO site_segments(record_id,kind,first_day,last_day_exclusive,"
        "equipment_type,systems,serial,firmware,radome,reference_point,installed_date)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11);", error);
    if (!metadata || !stationStatement || !aliasStatement || !profileStatement
        || !blockStatement || !coverageStatement || !sourceStatement
        || !siteRecordStatement || !siteSegmentStatement) {
        rollback(); return false;
    }
    const auto writeMetadata = [&](const std::string_view key, const std::string& value) {
        sqlite3_reset(metadata.get()); sqlite3_clear_bindings(metadata.get());
        return BindText(metadata.get(), 1, key) && BindText(metadata.get(), 2, value)
            && StepDone(database, metadata.get(), error);
    };
    if (!writeMetadata("schema_version", std::to_string(kSchemaVersion))
        || !writeMetadata("indexed_networks", std::to_string(result.indexedNetworks))
        || !writeMetadata("station_count", std::to_string(stations.size()))
        || !writeMetadata("profile_count", std::to_string(profiles.size()))) {
        rollback(); return false;
    }

    for (std::size_t index = 0; index < stations.size(); ++index) {
        const auto& station = stations[index];
        const auto& identity = station.representative;
        sqlite3_reset(stationStatement.get()); sqlite3_clear_bindings(stationStatement.get());
        sqlite3_bind_int64(stationStatement.get(), 1, static_cast<sqlite3_int64>(index + 1));
        BindText(stationStatement.get(), 2, identity.stationId);
        BindText(stationStatement.get(), 3, identity.fourCharId);
        BindText(stationStatement.get(), 4, identity.country);
        sqlite3_bind_int(stationStatement.get(), 5, identity.hasCoordinate ? 1 : 0);
        if (identity.hasCoordinate) {
            sqlite3_bind_double(stationStatement.get(), 6, identity.latitude);
            sqlite3_bind_double(stationStatement.get(), 7, identity.longitude);
        } else {
            sqlite3_bind_null(stationStatement.get(), 6);
            sqlite3_bind_null(stationStatement.get(), 7);
        }
        sqlite3_bind_int64(stationStatement.get(), 8,
            static_cast<sqlite3_int64>(station.networks));
        if (!StepDone(database, stationStatement.get(), error)) {
            rollback(); return false;
        }
    }
    for (std::size_t index = 0; index < memory.identities.size(); ++index) {
        const auto& identity = memory.identities[index];
        sqlite3_reset(aliasStatement.get()); sqlite3_clear_bindings(aliasStatement.get());
        sqlite3_bind_int64(aliasStatement.get(), 1,
            static_cast<sqlite3_int64>(identity.networkMask));
        BindText(aliasStatement.get(), 2, identity.stationKey);
        sqlite3_bind_int64(aliasStatement.get(), 3,
            static_cast<sqlite3_int64>(stationByIdentity[index] + 1));
        BindText(aliasStatement.get(), 4, identity.stationId);
        BindText(aliasStatement.get(), 5, identity.fourCharId);
        if (!StepDone(database, aliasStatement.get(), error)) {
            rollback(); return false;
        }
    }

    std::uint64_t blockCount = 0;
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        auto& profile = profiles[index];
        std::sort(profile.blocks.begin(), profile.blocks.end(),
            [](const auto& left, const auto& right) { return left.number < right.number; });
        sqlite3_reset(profileStatement.get()); sqlite3_clear_bindings(profileStatement.get());
        sqlite3_bind_int64(profileStatement.get(), 1, static_cast<sqlite3_int64>(index + 1));
        sqlite3_bind_int64(profileStatement.get(), 2,
            static_cast<sqlite3_int64>(profile.key.station + 1));
        sqlite3_bind_int64(profileStatement.get(), 3,
            static_cast<sqlite3_int64>(profile.key.networkMask));
        sqlite3_bind_int(profileStatement.get(), 4, profile.key.rinexFormat);
        BindText(profileStatement.get(), 5, profile.key.interval);
        sqlite3_bind_int(profileStatement.get(), 6, profile.minimumDay);
        sqlite3_bind_int(profileStatement.get(), 7, profile.maximumDay);
        sqlite3_bind_int64(profileStatement.get(), 8,
            static_cast<sqlite3_int64>(CountBits(profile.blocks)));
        if (!StepDone(database, profileStatement.get(), error)) {
            rollback(); return false;
        }
        for (const auto& block : profile.blocks) {
            sqlite3_reset(blockStatement.get()); sqlite3_clear_bindings(blockStatement.get());
            sqlite3_bind_int64(blockStatement.get(), 1, static_cast<sqlite3_int64>(index + 1));
            sqlite3_bind_int(blockStatement.get(), 2, block.number);
            sqlite3_bind_blob(blockStatement.get(), 3, block.words.data(),
                static_cast<int>(kExpectedBitmapBytes), SQLITE_TRANSIENT);
            if (!StepDone(database, blockStatement.get(), error)) {
                rollback(); return false;
            }
            ++blockCount;
        }
    }
    for (auto& [networkMask, blocks] : memory.coverage) {
        std::sort(blocks.begin(), blocks.end(),
            [](const auto& left, const auto& right) { return left.number < right.number; });
        for (const auto& block : blocks) {
            sqlite3_reset(coverageStatement.get());
            sqlite3_clear_bindings(coverageStatement.get());
            sqlite3_bind_int64(coverageStatement.get(), 1,
                static_cast<sqlite3_int64>(networkMask));
            sqlite3_bind_int(coverageStatement.get(), 2, block.number);
            sqlite3_bind_blob(coverageStatement.get(), 3, block.words.data(),
                static_cast<int>(kExpectedBitmapBytes), SQLITE_TRANSIENT);
            if (!StepDone(database, coverageStatement.get(), error)) {
                rollback(); return false;
            }
        }
    }
    for (const auto& source : memory.sources) {
        sqlite3_reset(sourceStatement.get()); sqlite3_clear_bindings(sourceStatement.get());
        sqlite3_bind_int64(sourceStatement.get(), 1,
            static_cast<sqlite3_int64>(source.networkMask));
        BindText(sourceStatement.get(), 2, PathToUtf8(source.path));
        sqlite3_bind_int64(sourceStatement.get(), 3,
            static_cast<sqlite3_int64>(source.stamp.size));
        sqlite3_bind_int64(sourceStatement.get(), 4, source.stamp.modified);
        sqlite3_bind_int64(sourceStatement.get(), 5,
            static_cast<sqlite3_int64>(source.stamp.walSize));
        sqlite3_bind_int64(sourceStatement.get(), 6, source.stamp.walModified);
        sqlite3_bind_int64(sourceStatement.get(), 7,
            static_cast<sqlite3_int64>(source.rows));
        BindText(sourceStatement.get(), 8, source.maximumDate);
        if (!StepDone(database, sourceStatement.get(), error)) {
            rollback(); return false;
        }
    }
    for (std::size_t index = 0; index < siteRecords.size(); ++index) {
        const auto& item = siteRecords[index];
        const auto& source = item.source;
        sqlite3_reset(siteRecordStatement.get());
        sqlite3_clear_bindings(siteRecordStatement.get());
        sqlite3_bind_int64(siteRecordStatement.get(), 1,
            static_cast<sqlite3_int64>(index + 1));
        sqlite3_bind_int64(siteRecordStatement.get(), 2,
            static_cast<sqlite3_int64>(item.station + 1));
        sqlite3_bind_int64(siteRecordStatement.get(), 3,
            static_cast<sqlite3_int64>(item.applicabilityMask));
        sqlite3_bind_int(siteRecordStatement.get(), 4, item.tier);
        sqlite3_bind_int(siteRecordStatement.get(), 5, item.networkOrder);
        BindText(siteRecordStatement.get(), 6, source.domes);
        BindText(siteRecordStatement.get(), 7, source.siteName);
        BindText(siteRecordStatement.get(), 8, source.city);
        BindText(siteRecordStatement.get(), 9, source.country);
        BindText(siteRecordStatement.get(), 10, source.agency);
        BindText(siteRecordStatement.get(), 11, source.operatorName);
        BindText(siteRecordStatement.get(), 12, source.revisionDate);
        BindText(siteRecordStatement.get(), 13, source.sourceDirectory);
        BindText(siteRecordStatement.get(), 14, source.sourceUrl);
        if (!StepDone(database, siteRecordStatement.get(), error)) {
            rollback(); return false;
        }
        for (const auto& segment : item.segments) {
            sqlite3_reset(siteSegmentStatement.get());
            sqlite3_clear_bindings(siteSegmentStatement.get());
            sqlite3_bind_int64(siteSegmentStatement.get(), 1,
                static_cast<sqlite3_int64>(index + 1));
            sqlite3_bind_int(siteSegmentStatement.get(), 2, segment.kind);
            sqlite3_bind_int(siteSegmentStatement.get(), 3, segment.firstDay);
            sqlite3_bind_int(siteSegmentStatement.get(), 4,
                segment.lastDayExclusive);
            BindText(siteSegmentStatement.get(), 5, segment.type);
            BindText(siteSegmentStatement.get(), 6, segment.systems);
            BindText(siteSegmentStatement.get(), 7, segment.serial);
            BindText(siteSegmentStatement.get(), 8, segment.firmware);
            BindText(siteSegmentStatement.get(), 9, segment.radome);
            BindText(siteSegmentStatement.get(), 10, segment.referencePoint);
            BindText(siteSegmentStatement.get(), 11, segment.installedDate);
            if (!StepDone(database, siteSegmentStatement.get(), error)) {
                rollback(); return false;
            }
        }
    }
    if (!Execute(database, "COMMIT;", error)) {
        rollback(); return false;
    }
    result.unifiedStations = stations.size();
    result.profiles = profiles.size();
    result.bitmapBlocks = blockCount;
    return true;
}

struct CachedProfile {
    std::size_t stationIndex = 0;
    ObservationNetworkMask networkMask = 0;
    int rinexFormat = 0;
    std::string interval;
    std::vector<BitmapBlock> blocks;
};

struct CachedSiteSegment {
    int kind = 0;
    int firstDay = 0;
    int lastDayExclusive = 0;
    std::string type;
    std::string systems;
    std::string serial;
    std::string firmware;
    std::string radome;
    std::string referencePoint;
    std::string installedDate;
};

struct CachedSiteRecord {
    std::size_t stationIndex = 0;
    ObservationNetworkMask applicabilityMask = 0;
    int tier = 0;
    int networkOrder = 0;
    std::string domes;
    std::string siteName;
    std::string city;
    std::string country;
    std::string agency;
    std::string operatorName;
    std::string revisionDate;
    std::string sourceDirectory;
    std::string sourceUrl;
    std::vector<CachedSiteSegment> segments;
};

struct CachedSourceState {
    ObservationNetworkMask networkMask = 0;
    std::filesystem::path path;
    FileStamp stamp;
};

struct CachedStationAlias {
    ObservationNetworkMask networkMask = 0;
    std::size_t stationIndex = 0;
};

// A dated, system-qualified observation-code capability imported from the
// standalone Site Log Plus database.  Both endpoints are inclusive calendar
// ordinals, matching signal_capability_intervals.interval_start/end.
struct CachedSignalInterval {
    int firstDay = 0;
    int lastDay = 0;
    std::string system;
    std::string code;
    // RINEX 2 and RINEX 3/4 can coexist on a station/day with different
    // observation-code declarations. Zero is the legacy, unqualified schema.
    int rinexFamily = 0;
};

struct CachedIndex {
    std::filesystem::path path;
    FileStamp ownStamp;
    // Optional read-only coordinate overlay.  It is kept outside the central
    // SQLite snapshot so a long-running source builder (notably GEONET-JP)
    // never has to reopen or rewrite observation_search.sqlite.
    FileStamp coordinateOverrideStamp;
    FileStamp siteLogPlusStamp;
    FileStamp trustedEquipmentStamp;
    // Site Log data is relatively large compared with the compact station /
    // bitmap core.  Ordinary availability queries do not need the dated
    // equipment segments synchronously because MainWindow can enrich the
    // visible rows on its worker afterwards.  Keep track of which optional
    // portions were hydrated so a later metadata-filter query can promote the
    // same immutable snapshot to a complete cache without confusing a
    // deliberately core-only cache hit for a full one.
    bool siteLogMetadataLoaded = false;
    bool siteLogPlusLoaded = false;
    bool siteLogPlusAvailable = false;
    std::optional<int> stationDetailLastDay;
    std::optional<int> dataCutoffDay;
    std::wstring siteLogPlusWarning;
    std::size_t coordinateOverridesApplied = 0;
    std::wstring coordinateOverrideWarning;
    ObservationNetworkMask indexedNetworks = 0;
    std::vector<StationFile> stations;
    std::vector<CachedProfile> profiles;
    std::vector<CachedSiteRecord> siteRecords;
    std::vector<std::vector<std::size_t>> siteRecordsByStation;
    // station_aliases is keyed by (network_mask, station_key) in SQLite.
    // Keep a reverse in-memory map so dated rows can resolve a reused
    // four-character code to the station belonging to their selected
    // network instead of the first global stationId match.
    std::unordered_map<std::string, std::vector<CachedStationAlias>>
        stationAliasesByKey;
    std::unordered_map<std::string, std::size_t> stationById;
    std::unordered_map<std::string, std::size_t> stationByFourCountry;
    std::unordered_map<std::string, std::size_t> uniqueStationByFour;
    std::vector<std::vector<CachedSignalInterval>> signalIntervalsByStation;
    // Fast lookup for download-list rechecks.  The key is SYSTEM + '\x1f' +
    // OBS_CODE and the ranges are sorted, so a large dated manifest does not
    // rescan every capability row for each file.
    std::vector<std::unordered_map<
        std::string, std::vector<std::pair<int, int>>>> signalRangesByStation;
    bool signalKnowledgeAvailable = false;
    bool signalFileAvailabilityRestricted = false;
    std::vector<std::unordered_map<std::string, std::vector<std::pair<int, int>>>>
        signalFileAvailabilityByStation;
    std::vector<std::unordered_map<int, std::vector<std::pair<int, int>>>>
        signalKnowledgeRangesByStation;
    std::unordered_map<ObservationNetworkMask, std::vector<BitmapBlock>> coverage;
    std::vector<CachedSourceState> sources;
    std::wstring error;
};

// Coordinate repairs which are discovered outside the central rebuild are
// kept in a tiny, optional JSON sidecar.  The sidecar is deliberately
// read-only from this library; its default location is next to the selected
// observation_search.sqlite file (normally runtime-data/).
constexpr std::size_t kMaxCoordinateOverrideBytes = 16U * 1024U * 1024U;

std::filesystem::path CoordinateOverridePath(
    const std::filesystem::path& indexPath) {
    return indexPath.parent_path() / L"observation_coordinate_overrides.json";
}

std::filesystem::path SiteLogPlusDatabasePath(
    const std::filesystem::path& indexPath) {
    // Keep the location configurable for packaged installations, while the
    // normal layout places the active Site Log Plus database beside the
    // compact observation index (runtime-data/).
    const auto explicitPath = EnvironmentPath(
        L"GNSS_DOWNLOADER_SITE_LOG_PLUS_DB_PATH");
    if (!explicitPath.empty()) return explicitPath;
    return ResolveDatabaseFilePath(
        indexPath.parent_path() / L"site_log_plus_equipment.sqlite");
}


std::optional<std::size_t> FindCachedStationIndex(
    const CachedIndex& index, const StationFile& station,
    const ObservationNetworkMask requestedMask);

std::filesystem::path TrustedEquipmentDatabasePath(
    const std::filesystem::path& indexPath) {
    const auto explicitPath = EnvironmentPath(
        L"GNSS_DOWNLOADER_TRUSTED_EQUIPMENT_DB_PATH");
    if (!explicitPath.empty()) return explicitPath;
    return ResolveDatabaseFilePath(
        indexPath.parent_path() / L"observation_trusted_equipment.sqlite");
}

bool ApplyTrustedEquipmentOverlay(CachedIndex& index,
    const std::filesystem::path& path, std::wstring& error);

struct CoordinateOverride {
    std::int64_t stationUid = 0;
    std::string stationId;
    std::string fourCharId;
    std::string country;
    std::string source;
    std::uint64_t networkMask = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    bool hasLatitude = false;
    bool hasLongitude = false;
    bool geonet = false;
};

// This small JSON reader intentionally accepts only the data types needed by
// coordinate overrides.  It avoids introducing a third-party parser into the
// index/query binaries and treats malformed/oversized sidecars as optional
// warnings rather than as a reason to reject an otherwise valid index.
class JsonCursor {
public:
    explicit JsonCursor(const std::string_view text) : text_(text) {}

    [[nodiscard]] std::size_t position() const { return position_; }
    void setPosition(const std::size_t value) { position_ = value; }

    void skipWhitespace() {
        while (position_ < text_.size()
            && std::isspace(static_cast<unsigned char>(text_[position_])) != 0) {
            ++position_;
        }
    }

    [[nodiscard]] char peek() {
        skipWhitespace();
        return position_ < text_.size() ? text_[position_] : '\0';
    }

    bool consume(const char expected) {
        skipWhitespace();
        if (position_ >= text_.size() || text_[position_] != expected) return false;
        ++position_;
        return true;
    }

    bool parseString(std::string& value) {
        value.clear();
        skipWhitespace();
        if (position_ >= text_.size() || text_[position_] != '"') return false;
        ++position_;
        while (position_ < text_.size()) {
            const unsigned char character =
                static_cast<unsigned char>(text_[position_++]);
            if (character == '"') return true;
            if (character < 0x20U) return false;
            if (character != '\\') {
                value.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ >= text_.size()) return false;
            const char escaped = text_[position_++];
            switch (escaped) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'u': {
                // Station IDs/country tokens are ASCII.  Preserve a Unicode
                // escape as '?' rather than accepting malformed UTF-16.
                unsigned code = 0;
                for (int digit = 0; digit < 4; ++digit) {
                    if (position_ >= text_.size()) return false;
                    const char hex = text_[position_++];
                    unsigned nibble = 0;
                    if (hex >= '0' && hex <= '9') nibble = static_cast<unsigned>(hex - '0');
                    else if (hex >= 'a' && hex <= 'f') nibble = static_cast<unsigned>(hex - 'a' + 10);
                    else if (hex >= 'A' && hex <= 'F') nibble = static_cast<unsigned>(hex - 'A' + 10);
                    else return false;
                    code = (code << 4U) | nibble;
                }
                value.push_back(code <= 0x7fU ? static_cast<char>(code) : '?');
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool parseNumber(double& value) {
        skipWhitespace();
        const std::size_t start = position_;
        while (position_ < text_.size()) {
            const char character = text_[position_];
            if ((character >= '0' && character <= '9')
                || character == '-' || character == '+' || character == '.'
                || character == 'e' || character == 'E') {
                ++position_;
            } else {
                break;
            }
        }
        if (start == position_) return false;
        const std::string token(text_.substr(start, position_ - start));
        char* end = nullptr;
        value = std::strtod(token.c_str(), &end);
        return end != token.c_str() && end == token.c_str() + token.size()
            && std::isfinite(value);
    }

    bool parseUnsigned(std::uint64_t& value) {
        double number = 0.0;
        if (!parseNumber(number) || number < 0.0
            || number > static_cast<double>(std::numeric_limits<std::uint64_t>::max())
            || std::floor(number) != number) return false;
        value = static_cast<std::uint64_t>(number);
        return true;
    }

    bool parseBoolean(bool& value) {
        skipWhitespace();
        if (text_.substr(position_, 4) == "true") {
            position_ += 4;
            value = true;
            return true;
        }
        if (text_.substr(position_, 5) == "false") {
            position_ += 5;
            value = false;
            return true;
        }
        return false;
    }

    bool skipValue() {
        skipWhitespace();
        const char character = peek();
        if (character == '"') {
            std::string ignored;
            return parseString(ignored);
        }
        if (character == '{') {
            if (!consume('{')) return false;
            if (consume('}')) return true;
            while (true) {
                std::string key;
                if (!parseString(key) || !consume(':') || !skipValue()) return false;
                if (consume('}')) return true;
                if (!consume(',')) return false;
            }
        }
        if (character == '[') {
            if (!consume('[')) return false;
            if (consume(']')) return true;
            while (true) {
                if (!skipValue()) return false;
                if (consume(']')) return true;
                if (!consume(',')) return false;
            }
        }
        if (text_.substr(position_, 4) == "true") {
            position_ += 4;
            return true;
        }
        if (text_.substr(position_, 5) == "false") {
            position_ += 5;
            return true;
        }
        if (text_.substr(position_, 4) == "null") {
            position_ += 4;
            return true;
        }
        double ignored = 0.0;
        return parseNumber(ignored);
    }

private:
    std::string_view text_;
    std::size_t position_ = 0;
};

bool ParseCoordinateOverrideObject(
    JsonCursor& cursor,
    CoordinateOverride& result) {
    if (!cursor.consume('{')) return false;
    if (cursor.consume('}')) return true;
    while (true) {
        std::string key;
        if (!cursor.parseString(key) || !cursor.consume(':')) return false;
        std::transform(key.begin(), key.end(), key.begin(),
            [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
        if (key == "station_uid" || key == "stationuid" || key == "uid") {
            std::uint64_t value = 0;
            if (!cursor.parseUnsigned(value)
                || value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return false;
            }
            result.stationUid = static_cast<std::int64_t>(value);
        } else if (key == "station_id" || key == "stationid"
                   || key == "full_id" || key == "fullid" || key == "id") {
            if (!cursor.parseString(result.stationId)) return false;
        } else if (key == "four_char_id" || key == "fourcharid"
                   || key == "four" || key == "site") {
            if (!cursor.parseString(result.fourCharId)) return false;
        } else if (key == "country") {
            if (!cursor.parseString(result.country)) return false;
        } else if (key == "latitude" || key == "lat") {
            if (!cursor.parseNumber(result.latitude)) return false;
            result.hasLatitude = true;
        } else if (key == "longitude" || key == "lon" || key == "lng") {
            if (!cursor.parseNumber(result.longitude)) return false;
            result.hasLongitude = true;
        } else if (key == "network_mask" || key == "networkmask"
                   || key == "source_mask" || key == "sourcemask"
                   || key == "available_networks" || key == "availablenetworks") {
            if (!cursor.parseUnsigned(result.networkMask)) return false;
        } else if (key == "geonet" || key == "geonet_jp" || key == "include_geonet") {
            if (!cursor.parseBoolean(result.geonet)) return false;
        } else if (key == "source" || key == "method" || key == "provenance") {
            if (!cursor.parseString(result.source)) return false;
        } else if (!cursor.skipValue()) {
            return false;
        }
        if (cursor.consume('}')) return true;
        if (!cursor.consume(',')) return false;
    }
}

bool ParseCoordinateOverrideArray(
    JsonCursor& cursor,
    std::vector<CoordinateOverride>& result) {
    if (!cursor.consume('[')) return false;
    if (cursor.consume(']')) return true;
    while (true) {
        if (cursor.peek() == '{') {
            CoordinateOverride item;
            if (!ParseCoordinateOverrideObject(cursor, item)) return false;
            result.push_back(std::move(item));
        } else if (!cursor.skipValue()) {
            return false;
        }
        if (cursor.consume(']')) return true;
        if (!cursor.consume(',')) return false;
    }
}

void ApplyCoordinateOverrideMapKey(
    CoordinateOverride& item,
    const std::string_view key) {
    // Numeric keys in the repair utility are UIDs; do not turn them into a
    // synthetic station_id and then reject an otherwise valid UID match.
    // Keep non-numeric map keys as a station-id fallback for hand-authored
    // {"ABCD00USA": {"lat": ..., "lon": ...}} sidecars.
    const bool numeric = !key.empty()
        && std::all_of(key.begin(), key.end(), [](const unsigned char value) {
            return std::isdigit(value) != 0;
        });
    if (item.stationId.empty() && (!numeric || item.stationUid <= 0)) {
        item.stationId = std::string(key);
    }
}

// The RINEX repair utility persists overrides as an object keyed by UID,
// while hand-authored sidecars commonly use an array.  Accept both forms in
// one container helper so the C++ reader can consume the utility's output
// without a conversion step.
bool ParseCoordinateOverrideMap(
    JsonCursor& cursor,
    std::vector<CoordinateOverride>& result) {
    if (!cursor.consume('{')) return false;
    if (cursor.consume('}')) return true;
    while (true) {
        std::string key;
        if (!cursor.parseString(key) || !cursor.consume(':')) return false;
        if (cursor.peek() == '{') {
            CoordinateOverride item;
            if (!ParseCoordinateOverrideObject(cursor, item)) return false;
            // Map keys are a fallback identity only.  Explicit station_id or
            // station_uid fields inside the value remain authoritative.
            ApplyCoordinateOverrideMapKey(item, key);
            result.push_back(std::move(item));
        } else if (!cursor.skipValue()) {
            return false;
        }
        if (cursor.consume('}')) return true;
        if (!cursor.consume(',')) return false;
    }
}

bool ParseCoordinateOverrideJson(
    const std::string& text,
    std::vector<CoordinateOverride>& result) {
    result.clear();
    // Accept the UTF-8 BOM emitted by some Windows editors/PowerShell
    // versions.  It is metadata, not part of the JSON token stream.
    std::string_view payload(text);
    if (payload.size() >= 3
        && static_cast<unsigned char>(payload[0]) == 0xEFU
        && static_cast<unsigned char>(payload[1]) == 0xBBU
        && static_cast<unsigned char>(payload[2]) == 0xBFU) {
        payload.remove_prefix(3);
    }
    JsonCursor cursor(payload);
    if (cursor.peek() == '[') {
        if (!ParseCoordinateOverrideArray(cursor, result)) return false;
        return cursor.peek() == '\0';
    }
    if (cursor.peek() != '{') return false;

    // Also accept a single override object as the root value.
    const auto rootPosition = cursor.position();
    CoordinateOverride single;
    if (ParseCoordinateOverrideObject(cursor, single)
        && single.hasLatitude && single.hasLongitude) {
        cursor.skipWhitespace();
        if (cursor.peek() == '\0') {
            result.push_back(std::move(single));
            return true;
        }
    }
    cursor.setPosition(rootPosition);
    if (!cursor.consume('{')) return false;
    if (cursor.consume('}')) return true;
    while (true) {
        std::string key;
        if (!cursor.parseString(key) || !cursor.consume(':')) return false;
        std::string lowerKey = key;
        std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(),
            [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
        if (lowerKey == "overrides" || lowerKey == "coordinates"
            || lowerKey == "safe_candidates" || lowerKey == "items"
            || lowerKey == "updates") {
            if (cursor.peek() == '[') {
                if (!ParseCoordinateOverrideArray(cursor, result)) return false;
            } else if (cursor.peek() == '{') {
                if (!ParseCoordinateOverrideMap(cursor, result)) return false;
            } else {
                return false;
            }
        } else if (cursor.peek() == '{') {
            // Map-style JSON ("ABCD00USA": {"lat":...,"lon":...}) is
            // accepted as a convenience, while explicit station_id wins.
            CoordinateOverride item;
            if (!ParseCoordinateOverrideObject(cursor, item)) return false;
            ApplyCoordinateOverrideMapKey(item, key);
            result.push_back(std::move(item));
        } else if (!cursor.skipValue()) {
            return false;
        }
        if (cursor.consume('}')) break;
        if (!cursor.consume(',')) return false;
    }
    return cursor.peek() == '\0';
}

bool ValidCoordinateOverride(const CoordinateOverride& value) {
    return value.hasLatitude && value.hasLongitude
        && ValidLatitudeLongitude(value.latitude, value.longitude)
        // Reject the Null-Island placeholder used by a few legacy imports.
        && !(std::abs(value.latitude) < 1.0e-12
            && std::abs(value.longitude) < 1.0e-12)
        && value.stationUid >= 0;
}

std::string OverrideFourChar(const CoordinateOverride& value) {
    std::string four = UpperAscii(value.fourCharId);
    if (!four.empty()) return four.substr(0, std::min<std::size_t>(4, four.size()));
    const std::string stationId = UpperAscii(value.stationId);
    if (stationId.size() < 4) return {};
    return stationId.substr(0, 4);
}

bool OverrideCountryMatches(
    const StationFile& station,
    const CoordinateOverride& value) {
    if (station.country.empty() || value.country.empty()) return true;
    return CanonicalCountry(station.country)
        == CanonicalCountry(value.country);
}

bool OverrideIdentityMatchesUid(
    const StationFile& station,
    const CoordinateOverride& value,
    const std::size_t stationIndex) {
    if (value.stationUid <= 0
        || value.stationUid != static_cast<std::int64_t>(stationIndex + 1)) {
        return false;
    }
    const std::string overrideId = UpperAscii(value.stationId);
    const std::string stationId = UpperAscii(station.stationId);
    if (!overrideId.empty() && !stationId.empty() && overrideId != stationId) {
        return false;
    }
    const std::string overrideFour = OverrideFourChar(value);
    const std::string stationFour = UpperAscii(station.fourCharId);
    if (!overrideFour.empty() && !stationFour.empty()
        && overrideFour != stationFour.substr(0, std::min<std::size_t>(4, stationFour.size()))) {
        return false;
    }
    return OverrideCountryMatches(station, value);
}

const CoordinateOverride* SelectUniqueOverride(
    const StationFile& station,
    const std::vector<const CoordinateOverride*>& candidates,
    const bool exactIdentity) {
    if (candidates.empty()) return nullptr;
    std::vector<const CoordinateOverride*> filtered;
    filtered.reserve(candidates.size());
    for (const auto* candidate : candidates) {
        if (candidate == nullptr || !ValidCoordinateOverride(*candidate)
            || candidate->geonet
            || (candidate->networkMask
                & NetworkBit(ObservationNetwork::GeoNetJapan)) != 0
            // A network-scoped sidecar entry must belong to at least one
            // network represented by this station.  Entries with a zero
            // mask are intentionally network-neutral (for UID/full-ID
            // repairs generated by older tools).
            || (candidate->networkMask != 0
                && (candidate->networkMask & station.availableNetworks) == 0)
            || !OverrideCountryMatches(station, *candidate)) {
            continue;
        }
        filtered.push_back(candidate);
    }
    if (filtered.empty()) return nullptr;
    if (!exactIdentity && station.stationId.size() > 4) {
        // Do not attach a bare four-character repair to a central full ID;
        // reused prefixes are common across regional archives.
        return nullptr;
    }
    const auto first = filtered.front();
    for (std::size_t index = 1; index < filtered.size(); ++index) {
        if (DistanceKm(
                first->latitude, first->longitude,
                filtered[index]->latitude, filtered[index]->longitude) > 3.0) {
            return nullptr;
        }
    }
    // If no country/full-ID discriminator exists, require one physical
    // coordinate cluster.  The distance test above collapses harmless
    // revisions while rejecting a reused four-character code.
    return first;
}

void ApplyCoordinateOverrides(
    CachedIndex& index,
    const std::filesystem::path& sidecarPath) {
    index.coordinateOverridesApplied = 0;
    index.coordinateOverrideWarning.clear();
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(sidecarPath, filesystemError)) return;
    const auto size = std::filesystem::file_size(sidecarPath, filesystemError);
    if (filesystemError || size == 0) return;
    if (size > kMaxCoordinateOverrideBytes) {
        index.coordinateOverrideWarning =
            L"坐标覆盖文件过大，已忽略：" + sidecarPath.wstring();
        return;
    }
    std::ifstream stream(sidecarPath, std::ios::binary);
    if (!stream) {
        index.coordinateOverrideWarning =
            L"无法读取坐标覆盖文件，已忽略：" + sidecarPath.wstring();
        return;
    }
    const std::string text(
        (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    std::vector<CoordinateOverride> parsed;
    if (!ParseCoordinateOverrideJson(text, parsed)) {
        index.coordinateOverrideWarning =
            L"坐标覆盖文件 JSON 无效，已忽略：" + sidecarPath.wstring();
        return;
    }

    std::vector<CoordinateOverride> valid;
    valid.reserve(parsed.size());
    for (auto& value : parsed) {
        value.stationId = UpperAscii(std::move(value.stationId));
        value.fourCharId = UpperAscii(std::move(value.fourCharId));
        value.country = UpperAscii(std::move(value.country));
        if (ValidCoordinateOverride(value)) valid.push_back(std::move(value));
    }
    if (valid.empty()) return;

    std::unordered_map<std::int64_t, std::vector<const CoordinateOverride*>> byUid;
    std::unordered_map<std::string, std::vector<const CoordinateOverride*>> byId;
    std::unordered_map<std::string, std::vector<const CoordinateOverride*>> byFourCountry;
    std::unordered_map<std::string, std::vector<const CoordinateOverride*>> byFour;
    for (const auto& value : valid) {
        if (value.stationUid > 0) byUid[value.stationUid].push_back(&value);
        if (!value.stationId.empty() && value.stationId.size() > 4) {
            byId[value.stationId].push_back(&value);
        }
        const std::string four = OverrideFourChar(value);
        if (four.empty()) continue;
        if (!value.country.empty()) {
            byFourCountry[four + '\x1f' + CanonicalCountry(value.country)]
                .push_back(&value);
        }
        byFour[four].push_back(&value);
    }

    for (std::size_t stationIndex = 0;
         stationIndex < index.stations.size(); ++stationIndex) {
        auto& station = index.stations[stationIndex];
        // GEONET-JP is intentionally left to its active SFTP builder.  A
        // sidecar entry can never opt this network into a partial snapshot.
        if (HasNetwork(station.availableNetworks, ObservationNetwork::GeoNetJapan)) {
            continue;
        }
        if (station.hasCoordinate
            && ValidLatitudeLongitude(station.latitude, station.longitude)
            && !(std::abs(station.latitude) < 1.0e-12
                && std::abs(station.longitude) < 1.0e-12)) {
            continue;
        }

        const CoordinateOverride* selected = nullptr;
        if (const auto found = byUid.find(static_cast<std::int64_t>(stationIndex + 1));
            found != byUid.end()) {
            std::vector<const CoordinateOverride*> uidCandidates;
            for (const auto* candidate : found->second) {
                if (OverrideIdentityMatchesUid(station, *candidate, stationIndex)) {
                    uidCandidates.push_back(candidate);
                }
            }
            selected = SelectUniqueOverride(station, uidCandidates, true);
        }
        if (selected == nullptr && station.stationId.size() > 4) {
            if (const auto found = byId.find(UpperAscii(station.stationId));
                found != byId.end()) {
                selected = SelectUniqueOverride(station, found->second, true);
            }
        }
        if (selected == nullptr && station.stationId.size() <= 4) {
            const std::string four = UpperAscii(station.fourCharId).empty()
                ? UpperAscii(station.stationId)
                : UpperAscii(station.fourCharId);
            std::vector<const CoordinateOverride*> candidates;
            if (!station.country.empty()) {
                const auto found = byFourCountry.find(
                    four.substr(0, std::min<std::size_t>(4, four.size()))
                    + '\x1f' + CanonicalCountry(station.country));
                if (found != byFourCountry.end()) candidates = found->second;
            }
            if (candidates.empty()) {
                const auto found = byFour.find(
                    four.substr(0, std::min<std::size_t>(4, four.size())));
                if (found != byFour.end()) candidates = found->second;
            }
            selected = SelectUniqueOverride(station, candidates, false);
        }
        if (selected == nullptr) continue;
        station.latitude = selected->latitude;
        station.longitude = selected->longitude;
        station.hasCoordinate = true;
        ++index.coordinateOverridesApplied;
    }
}

std::mutex gCacheMutex;
std::condition_variable gCacheCondition;
std::shared_ptr<const CachedIndex> gCachedIndex;
// Loading the compact index is deliberately done outside gCacheMutex because
// it hydrates hundreds of thousands of rows.  Keep a single in-flight load,
// however: the background preload and the first interactive query can arrive
// at the same time, and allowing both to hydrate the same snapshot doubles
// peak memory and disk work.
bool gCacheLoading = false;

// Forward declaration used by the cache loader's optional Site Log decision.
// The definition lives with the public metadata-filter preparation helpers
// below, after the cache-loading code.
bool AnyMetadataFilter(const ObservationRangeOptions& options);

class CacheLoadGuard {
public:
    CacheLoadGuard() = default;
    CacheLoadGuard(const CacheLoadGuard&) = delete;
    CacheLoadGuard& operator=(const CacheLoadGuard&) = delete;

    ~CacheLoadGuard() {
        if (!active_) return;
        {
            std::scoped_lock lock(gCacheMutex);
            gCacheLoading = false;
        }
        gCacheCondition.notify_all();
    }

    void activate() { active_ = true; }

private:
    bool active_ = false;
};

std::optional<std::uint64_t> MetadataUnsigned(
    sqlite3* database,
    const std::string_view key) {
    std::wstring error;
    auto statement = Prepare(database,
        "SELECT value FROM metadata WHERE key=?1;", error);
    if (!statement || !BindText(statement.get(), 1, key)
        || sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    try {
        return std::stoull(ColumnText(statement.get(), 0));
    } catch (...) {
        return std::nullopt;
    }
}

std::string NormalizeSignalSystem(std::string value) {
    value = UpperAscii(std::move(value));
    while (!value.empty()
        && (value.front() == '(' || value.front() == '['
            || value.front() == '{' || value.front() == ':')) {
        value.erase(value.begin());
    }
    while (!value.empty()
        && (value.back() == ')' || value.back() == ']'
            || value.back() == '}' || value.back() == ':')) {
        value.pop_back();
    }
    if (value == "GPS" || value == "NAVSTAR") return "G";
    if (value == "GLONASS" || value == "GLONAS" || value == "GLO") return "R";
    if (value == "GALILEO" || value == "GAL") return "E";
    if (value == "BDS" || value == "BEIDOU" || value == "BEI"
        || value == "COMPASS") return "C";
    if (value == "QZSS" || value == "QZS") return "J";
    if (value == "SBAS" || value == "SBS" || value == "WAAS") return "S";
    if (value == "IRNSS" || value == "IRNS" || value == "NAVIC"
        || value == "IRN") return "I";
    if (value.size() == 1U
        && std::string_view("GRECJSI?").find(value) != std::string_view::npos) {
        return value;
    }
    return value;
}

std::string SignalRangeKey(
    const std::string& system,
    const std::string& code,
    const int rinexFamily = 0) {
    std::string key = system;
    key.push_back('\x1f');
    key += code;
    key.push_back('\x1f');
    key += std::to_string(rinexFamily);
    return key;
}

void LoadSiteLogPlusSignals(
    CachedIndex& index,
    const std::filesystem::path& plusPath) {
    index.signalIntervalsByStation.resize(index.stations.size());
    index.signalRangesByStation.resize(index.stations.size());
    index.signalKnowledgeRangesByStation.resize(index.stations.size());
    index.signalFileAvailabilityByStation.resize(index.stations.size());
    index.siteLogPlusStamp = GetFileStamp(plusPath);
    if (index.siteLogPlusStamp.size == 0) {
        index.siteLogPlusWarning =
            L"未找到 Site Log Plus 频点知识库（site_log_plus_equipment.sqlite）。";
        return;
    }
    DatabasePtr database;
    std::wstring error;
    if (!OpenReadOnly(plusPath, database, error)) {
        index.siteLogPlusWarning = L"无法读取 Site Log Plus 频点知识库：" + error;
        return;
    }
    // Equipment/code metadata has its own snapshot cutoff. High-rate file
    // coverage can extend further and must not extend this history implicitly.
    if (TableExists(database.get(), "metadata")) {
        auto range = Prepare(database.get(),
            "SELECT value FROM metadata WHERE key='range_end';", error);
        int day = 0;
        if (range && sqlite3_step(range.get()) == SQLITE_ROW
            && TextDateToOrdinal(ColumnText(range.get(), 0), day))
            index.stationDetailLastDay = day;
    }
    if (!TableExists(database.get(), "signal_capability_intervals")) {
        index.siteLogPlusWarning =
            L"Site Log Plus 知识库缺少 signal_capability_intervals 表。";
        return;
    }
    bool hasRinexFamily = false;
    auto columns = Prepare(database.get(),
        "PRAGMA table_info(signal_capability_intervals);", error);
    while (columns && sqlite3_step(columns.get()) == SQLITE_ROW) {
        if (ColumnText(columns.get(), 1) == "rinex_family") {
            hasRinexFamily = true;
            break;
        }
    }
    const std::string signalSql =
        "SELECT network,station_key,four_char_id,system,obs_code,"
        "interval_start,interval_end,"
        + std::string(hasRinexFamily ? "CAST(rinex_family AS INTEGER)" : "0")
        + " FROM signal_capability_intervals WHERE network='IGS' "
          "ORDER BY station_key,interval_start,system,obs_code;";
    auto rows = Prepare(
        database.get(),
        signalSql.c_str(),
        error);
    if (!rows) {
        index.siteLogPlusWarning = L"读取 Site Log Plus 频点区间失败：" + error;
        return;
    }
    std::uint64_t loadedRows = 0;
    while (true) {
        const int status = sqlite3_step(rows.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            index.siteLogPlusWarning =
                L"读取 Site Log Plus 频点区间时发生 SQLite 错误："
                + SqliteError(database.get(), L"");
            return;
        }
        const std::string stationKey = UpperAscii(ColumnText(rows.get(), 1));
        const std::string fourChar = UpperAscii(ColumnText(rows.get(), 2));
        std::optional<std::size_t> stationIndex;
        if (!stationKey.empty()) {
            const auto found = index.stationById.find(stationKey);
            if (found != index.stationById.end()) stationIndex = found->second;
        }
        if (!stationIndex.has_value() && !fourChar.empty()) {
            const auto found = index.uniqueStationByFour.find(fourChar);
            if (found != index.uniqueStationByFour.end()) stationIndex = found->second;
        }
        if (!stationIndex.has_value() || *stationIndex >= index.stations.size()) {
            continue;
        }
        int firstDay = 0, lastDay = 0;
        if (!TextDateToOrdinal(ColumnText(rows.get(), 5), firstDay)
            || !TextDateToOrdinal(ColumnText(rows.get(), 6), lastDay)
            || lastDay < firstDay) {
            continue;
        }
        const std::string system = NormalizeSignalSystem(
            ColumnText(rows.get(), 3));
        std::string code = UpperAscii(ColumnText(rows.get(), 4));
        code.erase(std::remove_if(code.begin(), code.end(),
            [](const unsigned char value) { return std::isspace(value) != 0; }),
            code.end());
        if (system.empty() || code.empty()) continue;
        const int rinexFamily = sqlite3_column_int(rows.get(), 7);
        if (hasRinexFamily && rinexFamily != 2 && rinexFamily != 34) continue;
        index.signalIntervalsByStation[*stationIndex].push_back(
            CachedSignalInterval{firstDay, lastDay, system, std::move(code), rinexFamily});
        ++loadedRows;
    }
    for (auto& intervals : index.signalIntervalsByStation) {
        std::sort(intervals.begin(), intervals.end(),
            [](const CachedSignalInterval& left, const CachedSignalInterval& right) {
                return std::tie(left.firstDay, left.lastDay, left.system, left.code, left.rinexFamily)
                    < std::tie(right.firstDay, right.lastDay, right.system, right.code, right.rinexFamily);
            });
        intervals.erase(std::unique(intervals.begin(), intervals.end(),
            [](const CachedSignalInterval& left, const CachedSignalInterval& right) {
                return left.firstDay == right.firstDay
                    && left.lastDay == right.lastDay
                    && left.system == right.system
                    && left.code == right.code
                    && left.rinexFamily == right.rinexFamily;
            }), intervals.end());
    }
    for (std::size_t stationIndex = 0;
         stationIndex < index.signalIntervalsByStation.size();
         ++stationIndex) {
        auto& ranges = index.signalRangesByStation[stationIndex];
        for (const auto& interval : index.signalIntervalsByStation[stationIndex]) {
            ranges[SignalRangeKey(interval.system, interval.code, interval.rinexFamily)].push_back(
                {interval.firstDay, interval.lastDay});
        }
        for (auto& [key, values] : ranges) {
            (void)key;
            std::sort(values.begin(), values.end());
            std::vector<std::pair<int, int>> merged;
            merged.reserve(values.size());
            for (const auto& range : values) {
                if (merged.empty()
                    || range.first > merged.back().second
                        && (merged.back().second
                            == std::numeric_limits<int>::max()
                            || range.first > merged.back().second + 1)) {
                    merged.push_back(range);
                } else {
                    merged.back().second = std::max(
                        merged.back().second, range.second);
                }
            }
            values = std::move(merged);
        }
    }
    if (TableExists(database.get(), "signal_history_known_intervals")) {
        auto known = Prepare(database.get(),
            "SELECT station_key,CAST(rinex_family AS INTEGER),interval_start,interval_end "
            "FROM signal_history_known_intervals ORDER BY station_key,rinex_family,interval_start;", error);
        if (!known) {
            index.siteLogPlusWarning = L"读取频点已知时段失败：" + error;
            return;
        }
        while (sqlite3_step(known.get()) == SQLITE_ROW) {
            const std::string key = UpperAscii(ColumnText(known.get(), 0));
            std::optional<std::size_t> station;
            const auto exact = index.stationById.find(key);
            if (exact != index.stationById.end()) station = exact->second;
            if (!station.has_value() && key.size() >= 4) {
                const auto four = index.uniqueStationByFour.find(key.substr(0, 4));
                if (four != index.uniqueStationByFour.end()) station = four->second;
            }
            int first = 0, last = 0;
            const int family = sqlite3_column_int(known.get(), 1);
            if (!station.has_value() || (family != 2 && family != 34)
                || !TextDateToOrdinal(ColumnText(known.get(), 2), first)
                || !TextDateToOrdinal(ColumnText(known.get(), 3), last)
                || last < first) continue;
            index.signalKnowledgeRangesByStation[*station][family].push_back({first, last});
        }
        index.signalKnowledgeAvailable = true;
    }
    if (TableExists(database.get(), "signal_history_file_availability")) {
        auto files = Prepare(database.get(),
            "SELECT station_key,CAST(rinex_family AS INTEGER),sample_interval,"
            "interval_start,interval_end FROM signal_history_file_availability "
            "ORDER BY station_key,rinex_family,sample_interval,interval_start;", error);
        if (!files) {
            index.siteLogPlusWarning = L"读取频点对应的日文件可用性失败：" + error;
            return;
        }
        while (sqlite3_step(files.get()) == SQLITE_ROW) {
            const std::string key = UpperAscii(ColumnText(files.get(), 0));
            auto exact = index.stationById.find(key);
            std::optional<std::size_t> station;
            if (exact != index.stationById.end()) station = exact->second;
            if (!station.has_value() && key.size() >= 4) {
                const auto four = index.uniqueStationByFour.find(key.substr(0, 4));
                if (four != index.uniqueStationByFour.end()) station = four->second;
            }
            int first = 0, last = 0;
            const int family = sqlite3_column_int(files.get(), 1);
            if (!station.has_value() || (family != 2 && family != 34)
                || !TextDateToOrdinal(ColumnText(files.get(), 3), first)
                || !TextDateToOrdinal(ColumnText(files.get(), 4), last) || last < first) continue;
            const std::string interval = NormalizedInterval(ColumnText(files.get(), 2));
            index.signalFileAvailabilityByStation[*station][
                SignalRangeKey("", interval, family)].push_back({first, last});
        }
        index.signalFileAvailabilityRestricted = true;
    }
    index.siteLogPlusAvailable = true;
    if (loadedRows == 0) {
        index.siteLogPlusWarning =
            L"Site Log Plus 频点知识库可读取，但没有可匹配到高速索引测站的区间。";
    }
}

std::shared_ptr<const CachedIndex> LoadCachedIndex(
    const std::filesystem::path& path,
    const bool requireSiteLogMetadata = true,
    const bool requireSiteLogPlus = true) {
    const auto coordinateOverridePath = CoordinateOverridePath(path);
    const auto siteLogPlusPath = SiteLogPlusDatabasePath(path);
    const auto trustedEquipmentPath = TrustedEquipmentDatabasePath(path);
    FileStamp currentStamp;
    FileStamp coordinateOverrideStamp;
    FileStamp siteLogPlusStamp;
    FileStamp trustedEquipmentStamp;
    CacheLoadGuard loadGuard;
    for (;;) {
        // Re-read the stamps after waiting.  A builder may replace the index
        // while the preload is in progress; accepting the stamp captured
        // before the wait would incorrectly publish/return an older snapshot.
        currentStamp = GetFileStamp(path);
        coordinateOverrideStamp = GetFileStamp(coordinateOverridePath);
        siteLogPlusStamp = GetFileStamp(siteLogPlusPath);
        trustedEquipmentStamp = GetFileStamp(trustedEquipmentPath);
        std::unique_lock lock(gCacheMutex);
        if (gCachedIndex && gCachedIndex->path == path
            && gCachedIndex->ownStamp == currentStamp
            && gCachedIndex->coordinateOverrideStamp
                == coordinateOverrideStamp
            && gCachedIndex->siteLogPlusStamp == siteLogPlusStamp
            && gCachedIndex->trustedEquipmentStamp == trustedEquipmentStamp
            && (!requireSiteLogMetadata
                || gCachedIndex->siteLogMetadataLoaded)
            && (!requireSiteLogPlus || gCachedIndex->siteLogPlusLoaded)) {
            return gCachedIndex;
        }
        if (!gCacheLoading) {
            gCacheLoading = true;
            loadGuard.activate();
            break;
        }
        gCacheCondition.wait(lock, [] { return !gCacheLoading; });
    }
    auto loaded = std::make_shared<CachedIndex>();
    loaded->path = path;
    loaded->ownStamp = currentStamp;
    loaded->coordinateOverrideStamp = coordinateOverrideStamp;
    loaded->siteLogPlusStamp = siteLogPlusStamp;
    loaded->trustedEquipmentStamp = trustedEquipmentStamp;
    if (currentStamp.size == 0) {
        loaded->error = L"中央高速观测索引不存在：" + path.wstring();
    } else {
        DatabasePtr database;
        if (!OpenReadOnly(path, database, loaded->error)) {
            // OpenReadOnly supplied the error.
        } else if (!TableExists(database.get(), "metadata")
            || !TableExists(database.get(), "profile_blocks")) {
            loaded->error = L"中央高速观测索引结构不完整，请重新安装官方知识库后重试。";
        } else {
            const auto version = MetadataUnsigned(database.get(), "schema_version");
            const auto indexed = MetadataUnsigned(database.get(), "indexed_networks");
            if (!version || *version != kSchemaVersion || !indexed) {
                loaded->error = L"中央高速观测索引版本不兼容，请重新安装官方知识库后重试。";
            } else {
                loaded->indexedNetworks = static_cast<ObservationNetworkMask>(*indexed);
                std::wstring error;
                auto cutoff = Prepare(database.get(),
                    "SELECT value FROM metadata WHERE key='data_cutoff_date';", error);
                int cutoffDay = 0;
                if (cutoff && sqlite3_step(cutoff.get()) == SQLITE_ROW
                    && TextDateToOrdinal(ColumnText(cutoff.get(), 0), cutoffDay)) {
                    loaded->dataCutoffDay = cutoffDay;
                }
                auto stations = Prepare(database.get(),
                    "SELECT station_uid,station_id,four_char_id,country,has_coordinate,"
                    "latitude,longitude,available_networks FROM stations ORDER BY station_uid;",
                    error);
                if (!stations) loaded->error = std::move(error);
                while (stations && loaded->error.empty()) {
                    const int status = sqlite3_step(stations.get());
                    if (status == SQLITE_DONE) break;
                    if (status != SQLITE_ROW) {
                        loaded->error = SqliteError(database.get(), L"读取高速索引测站失败");
                        break;
                    }
                    const auto uid = sqlite3_column_int64(stations.get(), 0);
                    if (uid != static_cast<sqlite3_int64>(loaded->stations.size() + 1)) {
                        loaded->error = L"高速索引测站编号不连续，请重新安装官方知识库后重试。";
                        break;
                    }
                    StationFile station;
                    station.stationId = ColumnText(stations.get(), 1);
                    station.fourCharId = ColumnText(stations.get(), 2);
                    station.country = ColumnText(stations.get(), 3);
                    station.hasCoordinate = sqlite3_column_int(stations.get(), 4) != 0;
                    if (station.hasCoordinate) {
                        station.latitude = sqlite3_column_double(stations.get(), 5);
                        station.longitude = sqlite3_column_double(stations.get(), 6);
                    }
                    station.availableNetworks = static_cast<ObservationNetworkMask>(
                        sqlite3_column_int64(stations.get(), 7));
                    loaded->stations.push_back(std::move(station));
                }
                std::unordered_map<std::string, int> fourCounts;
                for (std::size_t index = 0; index < loaded->stations.size(); ++index) {
                    const auto& station = loaded->stations[index];
                    if (!station.stationId.empty()) {
                        loaded->stationById.try_emplace(
                            UpperAscii(station.stationId), index);
                    }
                    if (!station.fourCharId.empty()) {
                        const std::string four = UpperAscii(station.fourCharId);
                        loaded->stationByFourCountry.try_emplace(
                            four + '\x1f' + UpperAscii(station.country), index);
                        ++fourCounts[four];
                    }
                }
                for (std::size_t index = 0; index < loaded->stations.size(); ++index) {
                    const auto& station = loaded->stations[index];
                    const std::string four = UpperAscii(station.fourCharId);
                    if (!four.empty() && fourCounts[four] == 1) {
                        loaded->uniqueStationByFour.emplace(four, index);
                    }
                }
                // Read the network-scoped alias table when present.  Current
                // schema-3 indexes always emit it; retaining an empty map for
                // an older hand-built snapshot keeps the legacy ID fallback
                // usable while allowing the loader to report no false match.
                if (TableExists(database.get(), "station_aliases")) {
                    auto aliases = Prepare(
                        database.get(),
                        "SELECT network_mask,station_key,station_uid "
                        "FROM station_aliases ORDER BY network_mask,station_key;",
                        error);
                    if (!aliases && loaded->error.empty()) {
                        loaded->error = error;
                    }
                    while (aliases && loaded->error.empty()) {
                        const int status = sqlite3_step(aliases.get());
                        if (status == SQLITE_DONE) break;
                        if (status != SQLITE_ROW) {
                            loaded->error = SqliteError(
                                database.get(), L"读取高速索引测站别名失败");
                            break;
                        }
                        const auto stationUid = sqlite3_column_int64(aliases.get(), 2);
                        if (stationUid <= 0
                            || stationUid > static_cast<sqlite3_int64>(
                                loaded->stations.size())) {
                            loaded->error = L"高速索引测站别名编号无效，请重新安装官方知识库后重试。";
                            break;
                        }
                        const std::string stationKey = UpperAscii(
                            ColumnText(aliases.get(), 1));
                        if (stationKey.empty()) continue;
                        loaded->stationAliasesByKey[stationKey].push_back(
                            CachedStationAlias{
                                static_cast<ObservationNetworkMask>(
                                    sqlite3_column_int64(aliases.get(), 0)),
                                static_cast<std::size_t>(stationUid - 1)});
                    }
                }
                // Site Log Plus is an optional sibling database.  Loading it
                // never invalidates the compact observation snapshot: when
                // it is absent or temporarily locked, ordinary queries keep
                // working and a frequency-filtered query returns no false
                // positives (with a visible warning).
                if (requireSiteLogPlus) {
                    LoadSiteLogPlusSignals(*loaded, siteLogPlusPath);
                    loaded->siteLogPlusLoaded = true;
                }
                // Apply optional coordinates only after the central station
                // rows have been validated, and before publishing the cache.
                // This keeps all public query paths (including the GUI map)
                // on the same in-memory StationFile values without mutating
                // observation_search.sqlite.
                ApplyCoordinateOverrides(*loaded, coordinateOverridePath);
                auto profiles = Prepare(database.get(),
                    "SELECT profile_id,station_uid,network_mask,rinex_format,sample_interval "
                    "FROM profiles ORDER BY profile_id;", error);
                if (!profiles && loaded->error.empty()) loaded->error = error;
                while (profiles && loaded->error.empty()) {
                    const int status = sqlite3_step(profiles.get());
                    if (status == SQLITE_DONE) break;
                    if (status != SQLITE_ROW) {
                        loaded->error = SqliteError(database.get(), L"读取高速索引档案失败");
                        break;
                    }
                    const auto profileId = sqlite3_column_int64(profiles.get(), 0);
                    const auto stationUid = sqlite3_column_int64(profiles.get(), 1);
                    if (profileId != static_cast<sqlite3_int64>(loaded->profiles.size() + 1)
                        || stationUid <= 0
                        || stationUid > static_cast<sqlite3_int64>(loaded->stations.size())) {
                        loaded->error = L"高速索引档案编号无效，请重新安装官方知识库后重试。";
                        break;
                    }
                    loaded->profiles.push_back(CachedProfile{
                        static_cast<std::size_t>(stationUid - 1),
                        static_cast<ObservationNetworkMask>(
                            sqlite3_column_int64(profiles.get(), 2)),
                        sqlite3_column_int(profiles.get(), 3),
                        ColumnText(profiles.get(), 4), {}});
                }
                auto blocks = Prepare(database.get(),
                    "SELECT profile_id,block_number,bits FROM profile_blocks "
                    "ORDER BY profile_id,block_number;", error);
                if (!blocks && loaded->error.empty()) loaded->error = error;
                while (blocks && loaded->error.empty()) {
                    const int status = sqlite3_step(blocks.get());
                    if (status == SQLITE_DONE) break;
                    if (status != SQLITE_ROW) {
                        loaded->error = SqliteError(database.get(), L"读取高速索引位图失败");
                        break;
                    }
                    const auto profileId = sqlite3_column_int64(blocks.get(), 0);
                    const int bytes = sqlite3_column_bytes(blocks.get(), 2);
                    const void* blob = sqlite3_column_blob(blocks.get(), 2);
                    if (profileId <= 0
                        || profileId > static_cast<sqlite3_int64>(loaded->profiles.size())
                        || bytes != static_cast<int>(kExpectedBitmapBytes)
                        || blob == nullptr) {
                        loaded->error = L"高速索引位图损坏，请重新安装官方知识库后重试。";
                        break;
                    }
                    BitmapBlock block;
                    block.number = sqlite3_column_int(blocks.get(), 1);
                    std::memcpy(block.words.data(), blob, kExpectedBitmapBytes);
                    loaded->profiles[static_cast<std::size_t>(profileId - 1)]
                        .blocks.push_back(std::move(block));
                }
                auto coverage = Prepare(database.get(),
                    "SELECT network_mask,block_number,bits FROM coverage_blocks;", error);
                if (!coverage && loaded->error.empty()) loaded->error = error;
                while (coverage && loaded->error.empty()) {
                    const int status = sqlite3_step(coverage.get());
                    if (status == SQLITE_DONE) break;
                    if (status != SQLITE_ROW) {
                        loaded->error = SqliteError(database.get(), L"读取完整日期位图失败");
                        break;
                    }
                    const int bytes = sqlite3_column_bytes(coverage.get(), 2);
                    const void* blob = sqlite3_column_blob(coverage.get(), 2);
                    if (bytes != static_cast<int>(kExpectedBitmapBytes) || blob == nullptr) {
                        loaded->error = L"完整日期位图损坏，请重新安装官方知识库后重试。";
                        break;
                    }
                    BitmapBlock block;
                    block.number = sqlite3_column_int(coverage.get(), 1);
                    std::memcpy(block.words.data(), blob, kExpectedBitmapBytes);
                    loaded->coverage[static_cast<ObservationNetworkMask>(
                        sqlite3_column_int64(coverage.get(), 0))]
                        .push_back(std::move(block));
                }
                auto sources = Prepare(database.get(),
                    "SELECT network_mask,source_path,file_size,file_mtime,wal_size,wal_mtime "
                    "FROM source_state;", error);
                if (!sources && loaded->error.empty()) loaded->error = error;
                while (sources && loaded->error.empty()) {
                    const int status = sqlite3_step(sources.get());
                    if (status == SQLITE_DONE) break;
                    if (status != SQLITE_ROW) {
                        loaded->error = SqliteError(database.get(), L"读取索引源指纹失败");
                        break;
                    }
                    CachedSourceState source;
                    source.networkMask = static_cast<ObservationNetworkMask>(
                        sqlite3_column_int64(sources.get(), 0));
                    source.path = std::filesystem::path(Utf8ToWide(ColumnText(sources.get(), 1)));
                    // A converted release may have been built on another
                    // machine.  Re-anchor an unavailable absolute source
                    // path to the database directory by filename before the
                    // freshness check; this keeps a read-only portable copy
                    // usable without rewriting the index.
                    std::error_code sourceError;
                    if (!std::filesystem::is_regular_file(
                            source.path, sourceError)) {
                        const auto relocated = ResolveDatabaseFilePath(
                            path.parent_path() / source.path.filename());
                        sourceError.clear();
                        if (std::filesystem::is_regular_file(
                                relocated, sourceError)) {
                            source.path = relocated;
                        }
                    }
                    source.stamp.size = static_cast<std::uint64_t>(
                        sqlite3_column_int64(sources.get(), 2));
                    source.stamp.modified = sqlite3_column_int64(sources.get(), 3);
                    source.stamp.walSize = static_cast<std::uint64_t>(
                        sqlite3_column_int64(sources.get(), 4));
                    source.stamp.walModified = sqlite3_column_int64(sources.get(), 5);
                    loaded->sources.push_back(std::move(source));
                }
                if (requireSiteLogMetadata) {
                auto siteRecords = Prepare(database.get(),
                    "SELECT record_id,station_uid,applicability_mask,tier,network_order,"
                    "domes,site_name,city,country,agency,operator_name,revision_date,"
                    "source_directory,source_url FROM site_records ORDER BY record_id;",
                    error);
                if (!siteRecords && loaded->error.empty()) loaded->error = error;
                while (siteRecords && loaded->error.empty()) {
                    const int status = sqlite3_step(siteRecords.get());
                    if (status == SQLITE_DONE) break;
                    if (status != SQLITE_ROW) {
                        loaded->error = SqliteError(
                            database.get(), L"读取时间化 Site Log 来源失败");
                        break;
                    }
                    const auto recordId = sqlite3_column_int64(siteRecords.get(), 0);
                    const auto stationUid = sqlite3_column_int64(siteRecords.get(), 1);
                    if (recordId != static_cast<sqlite3_int64>(
                            loaded->siteRecords.size() + 1)
                        || stationUid <= 0
                        || stationUid > static_cast<sqlite3_int64>(
                            loaded->stations.size())) {
                        loaded->error = L"高速索引 Site Log 编号无效，请重新安装官方知识库后重试。";
                        break;
                    }
                    CachedSiteRecord record;
                    record.stationIndex = static_cast<std::size_t>(stationUid - 1);
                    record.applicabilityMask = static_cast<ObservationNetworkMask>(
                        sqlite3_column_int64(siteRecords.get(), 2));
                    record.tier = sqlite3_column_int(siteRecords.get(), 3);
                    record.networkOrder = sqlite3_column_int(siteRecords.get(), 4);
                    record.domes = ColumnText(siteRecords.get(), 5);
                    record.siteName = ColumnText(siteRecords.get(), 6);
                    record.city = ColumnText(siteRecords.get(), 7);
                    record.country = ColumnText(siteRecords.get(), 8);
                    record.agency = ColumnText(siteRecords.get(), 9);
                    record.operatorName = ColumnText(siteRecords.get(), 10);
                    record.revisionDate = ColumnText(siteRecords.get(), 11);
                    record.sourceDirectory = ColumnText(siteRecords.get(), 12);
                    record.sourceUrl = ColumnText(siteRecords.get(), 13);
                    loaded->siteRecords.push_back(std::move(record));
                }
                auto siteSegments = Prepare(database.get(),
                    "SELECT record_id,kind,first_day,last_day_exclusive,equipment_type,"
                    "systems,serial,firmware,radome,reference_point,installed_date "
                    "FROM site_segments ORDER BY record_id,kind,first_day;", error);
                if (!siteSegments && loaded->error.empty()) loaded->error = error;
                while (siteSegments && loaded->error.empty()) {
                    const int status = sqlite3_step(siteSegments.get());
                    if (status == SQLITE_DONE) break;
                    if (status != SQLITE_ROW) {
                        loaded->error = SqliteError(
                            database.get(), L"读取时间化 Site Log 设备段失败");
                        break;
                    }
                    const auto recordId = sqlite3_column_int64(siteSegments.get(), 0);
                    if (recordId <= 0
                        || recordId > static_cast<sqlite3_int64>(
                            loaded->siteRecords.size())) {
                        loaded->error = L"高速索引 Site Log 设备段编号无效。";
                        break;
                    }
                    CachedSiteSegment segment;
                    segment.kind = sqlite3_column_int(siteSegments.get(), 1);
                    segment.firstDay = sqlite3_column_int(siteSegments.get(), 2);
                    segment.lastDayExclusive = sqlite3_column_int(siteSegments.get(), 3);
                    segment.type = ColumnText(siteSegments.get(), 4);
                    segment.systems = ColumnText(siteSegments.get(), 5);
                    segment.serial = ColumnText(siteSegments.get(), 6);
                    segment.firmware = ColumnText(siteSegments.get(), 7);
                    segment.radome = ColumnText(siteSegments.get(), 8);
                    segment.referencePoint = ColumnText(siteSegments.get(), 9);
                    segment.installedDate = ColumnText(siteSegments.get(), 10);
                    loaded->siteRecords[static_cast<std::size_t>(recordId - 1)]
                        .segments.push_back(std::move(segment));
                }
                loaded->siteRecordsByStation.resize(loaded->stations.size());
                for (std::size_t index = 0;
                     index < loaded->siteRecords.size(); ++index) {
                    const auto stationIndex = loaded->siteRecords[index].stationIndex;
                    if (stationIndex < loaded->siteRecordsByStation.size()) {
                        loaded->siteRecordsByStation[stationIndex].push_back(index);
                    }
                }
                if (loaded->error.empty()) {
                    ApplyTrustedEquipmentOverlay(*loaded, trustedEquipmentPath, loaded->error);
                }
                loaded->siteLogMetadataLoaded = loaded->error.empty();
                }
            }
        }
    }
    std::scoped_lock lock(gCacheMutex);
    gCachedIndex = loaded;
    return loaded;
}

bool AnyMetadataFilter(const ObservationRangeOptions& options) {
    return !options.systemsInclude.empty() || !options.systemsExclude.empty()
        || !options.receiverInclude.empty() || !options.receiverExclude.empty()
        || !options.antennaInclude.empty() || !options.antennaExclude.empty()
        || !options.includeSystems.empty() || !options.excludeSystems.empty()
        || !options.includeReceiver.empty() || !options.excludeReceiver.empty()
        || !options.includeAntenna.empty() || !options.excludeAntenna.empty()
        || !options.signalCodesInclude.empty()
        || !options.signalCodesExclude.empty();
}

std::uint64_t WordRangeMask(
    const int wordStartOrdinal,
    const int firstOrdinal,
    const int lastOrdinal) {
    const int low = std::max(0, firstOrdinal - wordStartOrdinal);
    const int high = std::min(63, lastOrdinal - wordStartOrdinal);
    if (low > high) return 0;
    const std::uint64_t lower = low == 0
        ? ~std::uint64_t{0} : (~std::uint64_t{0} << low);
    const std::uint64_t upper = high == 63
        ? ~std::uint64_t{0} : ((std::uint64_t{1} << (high + 1)) - 1);
    return lower & upper;
}

bool BlocksHaveAnyDayInRange(
    const std::vector<BitmapBlock>& blocks,
    const int firstOrdinal,
    const int lastOrdinal) {
    for (const auto& block : blocks) {
        const int blockFirst = block.number * kBitsPerBlock;
        const int blockLast = blockFirst + kBitsPerBlock - 1;
        if (blockLast < firstOrdinal || blockFirst > lastOrdinal) continue;
        for (int word = 0; word < kWordsPerBlock; ++word) {
            const int wordStart = blockFirst + word * 64;
            const auto withinRange = WordRangeMask(
                wordStart, firstOrdinal, lastOrdinal);
            if ((block.words[static_cast<std::size_t>(word)] & withinRange)
                != 0) {
                return true;
            }
        }
    }
    return false;
}

bool CoverageContainsRange(
    const std::vector<BitmapBlock>& blocks,
    const int firstOrdinal,
    const int lastOrdinal) {
    for (int ordinal = firstOrdinal; ordinal <= lastOrdinal;) {
        const int blockNumber = BlockNumber(ordinal);
        const int blockEnd = std::min(
            lastOrdinal, (blockNumber + 1) * kBitsPerBlock - 1);
        const auto found = std::find_if(blocks.begin(), blocks.end(),
            [&](const BitmapBlock& block) { return block.number == blockNumber; });
        if (found == blocks.end()) return false;
        for (int word = 0; word < kWordsPerBlock; ++word) {
            const int wordStart = blockNumber * kBitsPerBlock + word * 64;
            const std::uint64_t required = WordRangeMask(
                wordStart, ordinal, blockEnd);
            if (required != 0
                && (found->words[static_cast<std::size_t>(word)] & required) != required) {
                return false;
            }
        }
        if (blockEnd == std::numeric_limits<int>::max()) break;
        ordinal = blockEnd + 1;
    }
    return true;
}

bool ProfileMatches(
    const CachedProfile& profile,
    const NetworkSelection selection,
    const ObservationRangeOptions& options,
    const std::vector<std::string>& intervals) {
    if ((profile.networkMask & selection.mask) == 0) return false;
    if (options.rinexVersion == 2 && profile.rinexFormat != 2) return false;
    if ((options.rinexVersion == 3 || options.rinexVersion == 4)
        && profile.rinexFormat != 3) return false;
    if (options.rinexVersion == 0) {
        if (profile.rinexFormat == 2 && !options.includeRinex2) return false;
        if (profile.rinexFormat != 2 && !options.includeRinex34) return false;
    }
    return intervals.empty()
        || std::find(intervals.begin(), intervals.end(), profile.interval) != intervals.end();
}

struct QueryStationAccumulator {
    struct NetworkAvailability {
        ObservationNetworkMask bit = 0;
        std::vector<BitmapBlock> blocks;
    };
    bool touched = false;
    ObservationNetworkMask networks = 0;
    int representativeFormat = 0;
    RinexFormatMask availableFormats = 0;
    std::string representativeInterval;
    std::vector<BitmapBlock> blocks;
    std::vector<NetworkAvailability> byNetwork;
};

const std::string& EffectiveFilterField(
    const std::string& primary,
    const std::string& compatibility) {
    return primary.empty() ? compatibility : primary;
}

std::vector<std::string> SplitMetadataTokens(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;
    const auto flush = [&]() {
        if (current.empty()) return;
        std::transform(current.begin(), current.end(), current.begin(),
            [](const unsigned char value) {
                return static_cast<char>(std::toupper(value));
            });
        tokens.push_back(current);
        current.clear();
    };
    for (const unsigned char value : text) {
        if (value == ',' || value == ';' || value == '/'
            || value == '|' || value == '+' || std::isspace(value)) {
            flush();
        } else {
            current.push_back(static_cast<char>(value));
        }
    }
    flush();
    return tokens;
}

std::optional<std::string> CanonicalSatelliteSystemTokenInternal(
    std::string value) {
    // Site Log exports mix RINEX one-letter identifiers with full names and
    // a few common abbreviations.  Strip punctuation left by values such as
    // "(GPS/GLONASS)" before applying the alias table.
    value = UpperAscii(std::move(value));
    while (!value.empty()
        && (value.front() == '(' || value.front() == '['
            || value.front() == '{' || value.front() == ':')) {
        value.erase(value.begin());
    }
    while (!value.empty()
        && (value.back() == ')' || value.back() == ']'
            || value.back() == '}' || value.back() == ':')) {
        value.pop_back();
    }
    if (value == "G" || value == "GPS" || value == "NAVSTAR") return "GPS";
    if (value == "R" || value == "GLO" || value == "GLONASS"
        || value == "GLONAS") return "GLO";
    if (value == "E" || value == "GAL" || value == "GALILEO"
        || value == "GA") return "GAL";
    if (value == "C" || value == "BDS" || value == "BEI"
        || value == "BEIDOU" || value == "COMPASS") return "BDS";
    if (value == "J" || value == "QZS" || value == "QZSS") return "QZS";
    if (value == "I" || value == "IRN" || value == "IRNS"
        || value == "IRNSS" || value == "NAVIC") return "IRN";
    if (value == "S" || value == "SBS" || value == "SBAS"
        || value == "WAAS") return "SBS";
    return std::nullopt;
}

std::vector<std::string> CanonicalSatelliteSystemTokensInternal(
    const std::string& text) {
    std::vector<std::string> result;
    for (const auto& token : SplitMetadataTokens(text)) {
        const auto canonical = CanonicalSatelliteSystemTokenInternal(token);
        if (canonical.has_value()
            && std::find(result.begin(), result.end(), *canonical)
                == result.end()) {
            result.push_back(*canonical);
        }
    }
    return result;
}

std::vector<std::string> CanonicalOrRawSystemFilterTokens(
    const std::string& text) {
    std::vector<std::string> result;
    for (const auto& token : SplitMetadataTokens(text)) {
        const auto canonical = CanonicalSatelliteSystemTokenInternal(token);
        const std::string value = canonical.has_value()
            ? *canonical : token;
        if (std::find(result.begin(), result.end(), value) == result.end()) {
            result.push_back(value);
        }
    }
    return result;
}

std::vector<std::string> SplitMetadataChoices(const std::string& text) {
    std::vector<std::string> choices;
    std::string current;
    const auto flush = [&]() {
        const auto first = current.find_first_not_of(" \t\r\n");
        const auto last = current.find_last_not_of(" \t\r\n");
        if (first != std::string::npos) {
            std::string value = current.substr(first, last - first + 1);
            std::transform(value.begin(), value.end(), value.begin(),
                [](const unsigned char character) {
                    return static_cast<char>(std::toupper(character));
                });
            choices.push_back(std::move(value));
        }
        current.clear();
    };
    for (const char value : text) {
        if (value == ',' || value == ';' || value == '|') flush();
        else current.push_back(value);
    }
    flush();
    return choices;
}

struct PreparedSignalToken {
    std::string system;
    std::string code;
};

std::vector<PreparedSignalToken> SplitSignalTokens(const std::string& text) {
    std::vector<PreparedSignalToken> result;
    for (const auto& raw : SplitMetadataChoices(text)) {
        const auto separator = raw.find(':');
        if (separator == std::string::npos || separator == 0U
            || separator + 1U >= raw.size()) {
            // A bare observation code is intentionally rejected: the same
            // code (for example C1C/P1) can belong to multiple constellations.
            continue;
        }
        const std::string system = NormalizeSignalSystem(
            raw.substr(0, separator));
        std::string code = UpperAscii(raw.substr(separator + 1U));
        code.erase(std::remove_if(code.begin(), code.end(),
            [](const unsigned char value) { return std::isspace(value) != 0; }),
            code.end());
        if (system.empty() || code.empty()) continue;
        const bool duplicate = std::any_of(result.begin(), result.end(),
            [&](const PreparedSignalToken& item) {
                return item.system == system && item.code == code;
            });
        if (!duplicate) result.push_back({system, std::move(code)});
    }
    return result;
}

struct PreparedMetadataFilter {
    std::vector<std::string> systemsInclude;
    std::vector<std::string> systemsExclude;
    std::vector<std::string> receiverInclude;
    std::vector<std::string> receiverExclude;
    std::vector<std::string> antennaInclude;
    std::vector<std::string> antennaExclude;
    std::vector<PreparedSignalToken> signalInclude;
    std::vector<PreparedSignalToken> signalExclude;
    bool signalInputRequested = false;
    bool invalidSignalInput = false;

    bool HasReceiverRestriction() const {
        return !systemsInclude.empty() || !systemsExclude.empty()
            || !receiverInclude.empty() || !receiverExclude.empty();
    }
    bool HasAntennaRestriction() const {
        return !antennaInclude.empty() || !antennaExclude.empty();
    }
    bool HasSignalRestriction() const {
        return signalInputRequested
            || !signalInclude.empty() || !signalExclude.empty();
    }
    bool HasSiteLogRestriction() const {
        return HasReceiverRestriction() || HasAntennaRestriction();
    }
};

PreparedMetadataFilter PrepareMetadataFilter(
    const ObservationRangeOptions& options) {
    PreparedMetadataFilter prepared;
    prepared.systemsInclude = CanonicalOrRawSystemFilterTokens(
        EffectiveFilterField(options.systemsInclude, options.includeSystems));
    prepared.systemsExclude = CanonicalOrRawSystemFilterTokens(
        EffectiveFilterField(options.systemsExclude, options.excludeSystems));
    prepared.receiverInclude = SplitMetadataChoices(EffectiveFilterField(
        options.receiverInclude, options.includeReceiver));
    prepared.receiverExclude = SplitMetadataChoices(EffectiveFilterField(
        options.receiverExclude, options.excludeReceiver));
    prepared.antennaInclude = SplitMetadataChoices(EffectiveFilterField(
        options.antennaInclude, options.includeAntenna));
    prepared.antennaExclude = SplitMetadataChoices(EffectiveFilterField(
        options.antennaExclude, options.excludeAntenna));
    prepared.signalInputRequested =
        !options.signalCodesInclude.empty()
        || !options.signalCodesExclude.empty();
    prepared.signalInclude = SplitSignalTokens(options.signalCodesInclude);
    prepared.signalExclude = SplitSignalTokens(options.signalCodesExclude);
    if (prepared.signalInputRequested
        && prepared.signalInclude.empty()
        && prepared.signalExclude.empty()) {
        prepared.invalidSignalInput = true;
    }
    const auto hasMalformedToken = [](const std::string& text) {
        for (const auto& token : SplitMetadataChoices(text)) {
            const auto separator = token.find(':');
            if (separator == std::string::npos || separator == 0U
                || separator + 1U >= token.size()) {
                return true;
            }
            const std::string system = NormalizeSignalSystem(
                token.substr(0, separator));
            if (system.size() != 1U
                || std::string_view("GRECJSI").find(system)
                    == std::string_view::npos) {
                return true;
            }
        }
        return !text.empty() && SplitMetadataChoices(text).empty();
    };
    prepared.invalidSignalInput = prepared.invalidSignalInput
        || hasMalformedToken(options.signalCodesInclude)
        || hasMalformedToken(options.signalCodesExclude);
    return prepared;
}

struct GimStationFilter {
    enum class Mode { Ignore, Keep, Exclude };

    Mode mode = Mode::Ignore;
    std::unordered_set<std::string> stationCodes;
    std::wstring warning;

    bool active() const {
        return mode != Mode::Ignore;
    }

    bool Matches(const StationFile& station) const {
        if (!active()) return true;
        std::string code = station.fourCharId.empty()
            ? station.stationId : station.fourCharId;
        code = UpperAscii(std::move(code));
        code.erase(std::remove_if(code.begin(), code.end(),
            [](const unsigned char value) { return std::isspace(value) != 0; }),
            code.end());
        if (code.size() > 4U) code.resize(4U);
        const bool found = !code.empty() && stationCodes.contains(code);
        return mode == Mode::Keep ? found : !found;
    }
};

std::mutex gGimStationCacheMutex;
std::unordered_map<
    std::string,
    std::shared_ptr<const std::unordered_set<std::string>>>
    gGimStationCache;

std::filesystem::path DefaultGimStationDatabasePath() {
    if (const auto explicitPath = EnvironmentPath(
            L"GNSS_DOWNLOADER_GIM_STATION_DB_PATH");
        !explicitPath.empty()) {
        return explicitPath;
    }
    std::vector<std::filesystem::path> candidates;
    if (const auto directory = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
        !directory.empty()) {
        candidates.push_back(ResolveDatabaseFilePath(
            directory / L"gim_station.sqlite"));
    }
#ifdef GNSS_DOWNLOADER_DUCKDB_BACKEND
    {
        std::wstring moduleBuffer(32768, L'\0');
        const DWORD moduleLength = GetModuleFileNameW(
            nullptr, moduleBuffer.data(), static_cast<DWORD>(moduleBuffer.size()));
        if (moduleLength > 0 && moduleLength < moduleBuffer.size()) {
            moduleBuffer.resize(moduleLength);
            candidates.push_back(ResolveDatabaseFilePath(
                std::filesystem::path(moduleBuffer).parent_path().parent_path()
                / L"knowledge-base" / L"gim_station.sqlite"));
        }
    }
#endif
    const auto local = ResolveDatabaseFilePath(
        DefaultKnowledgeBaseDirectory() / L"gim_station.sqlite");
    candidates.push_back(local);
    candidates.push_back(
        std::filesystem::current_path() / L"runtime-data" / L"gim_station.sqlite");
    wchar_t moduleBuffer[32768]{};
    const DWORD moduleLength = GetModuleFileNameW(
        nullptr, moduleBuffer, static_cast<DWORD>(std::size(moduleBuffer)));
    if (moduleLength > 0 && moduleLength < std::size(moduleBuffer)) {
        auto directory = std::filesystem::path(
            std::wstring(moduleBuffer, moduleLength)).parent_path();
        for (int level = 0; level < 5 && !directory.empty(); ++level) {
            candidates.push_back(directory / L"runtime-data" / L"gim_station.sqlite");
            directory = directory.parent_path();
        }
    }
    std::error_code error;
    for (const auto& candidate : candidates) {
        error.clear();
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;
    }
    return local;
}

std::vector<std::string> ParseGimProviders(const std::string& text) {
    static constexpr std::array<std::string_view, 8> known{
        "IGS", "CODE", "ESA", "JPL", "UPC", "CAS", "EMR", "WHU"};
    std::vector<std::string> result;
    std::string current;
    const auto flush = [&]() {
        if (current.empty()) return;
        current = UpperAscii(std::move(current));
        if (std::find(known.begin(), known.end(), current) != known.end()
            && std::find(result.begin(), result.end(), current) == result.end()) {
            result.push_back(current);
        }
        current.clear();
    };
    for (const unsigned char value : text) {
        if (value == ',' || value == ';' || value == '|'
            || std::isspace(value) != 0) flush();
        else current.push_back(static_cast<char>(value));
    }
    flush();
    return result;
}

std::vector<std::string> ParseGimProductGrades(
    const std::string& text, const std::vector<std::string>& providers) {
    static constexpr std::array<std::string_view, 5> grades{
        "FINAL", "RAPID", "PREDICTED", "REAL_TIME", "OTHER"};
    std::vector<std::string> result;
    std::string token;
    const auto flush = [&]() {
        token = UpperAscii(std::move(token));
        const auto colon = token.find(':');
        if (colon != std::string::npos) {
            const auto provider = token.substr(0, colon);
            const auto grade = token.substr(colon + 1);
            if (std::find(providers.begin(), providers.end(), provider) != providers.end()
                && std::find(grades.begin(), grades.end(), grade) != grades.end()
                && std::find(result.begin(), result.end(), token) == result.end()) {
                result.push_back(token);
            }
        }
        token.clear();
    };
    for (const unsigned char value : text) {
        if (value == ',' || value == ';' || value == '|' || std::isspace(value)) flush();
        else token.push_back(static_cast<char>(value));
    }
    flush();
    std::sort(result.begin(), result.end());
    return result;
}

GimStationFilter LoadGimStationFilter(
    const QueryDate& startDate,
    const QueryDate& endDate,
    const ObservationRangeOptions& options,
    const std::stop_token stopToken) {
    GimStationFilter result;
    const std::string mode = UpperAscii(options.gimStationMode);
    if (mode == "KEEP") result.mode = GimStationFilter::Mode::Keep;
    else if (mode == "EXCLUDE") result.mode = GimStationFilter::Mode::Exclude;
    else result.mode = GimStationFilter::Mode::Ignore;
    if (!result.active()) return result;
    const auto providers = ParseGimProviders(options.gimProviders);
    if (providers.empty()) {
        result.warning = L"GIM 测站筛选已启用，但没有选择机构；当前按零个 GIM 测站处理。";
        return result;
    }
    if (stopToken.stop_requested()) {
        result.warning = L"GIM 测站筛选已取消。";
        return result;
    }
    const auto grades = ParseGimProductGrades(options.gimProductGrades, providers);
    if (!options.gimProductGrades.empty() && grades.empty()) {
        result.warning = L"GIM 产品等级没有有效的机构与等级组合；当前按零个 GIM 测站处理。";
        return result;
    }
    const auto databasePath = DefaultGimStationDatabasePath();
    const auto fileStamp = GetFileStamp(databasePath);
    std::vector<std::string> cacheProviders = providers;
    std::sort(cacheProviders.begin(), cacheProviders.end());
    std::string cacheKey = PathToUtf8(databasePath) + "|"
        + std::format("{}:{}:{}:{}", fileStamp.size, fileStamp.modified,
            fileStamp.walSize, fileStamp.walModified)
        + "|" + std::format("{:04}-{:02}-{:02}", startDate.year,
            startDate.month, startDate.day)
        + "|" + std::format("{:04}-{:02}-{:02}", endDate.year,
            endDate.month, endDate.day) + "|";
    for (const auto& provider : cacheProviders) cacheKey += provider + ',';
    cacheKey += '|';
    for (const auto& grade : grades) cacheKey += grade + ',';
    {
        std::scoped_lock lock(gGimStationCacheMutex);
        const auto cached = gGimStationCache.find(cacheKey);
        if (cached != gGimStationCache.end()) {
            result.stationCodes = *cached->second;
            return result;
        }
    }
    DatabasePtr database;
    std::wstring error;
    if (!OpenReadOnly(databasePath, database, error)) {
        result.warning = L"无法读取 GIM 测站知识库（"
            + databasePath.wstring() + L"）；"
            + (result.mode == GimStationFilter::Mode::Keep
                ? L"仅保留模式按零个测站处理。"
                : L"排除模式暂不排除任何测站。")
            + L" 原因：" + error;
        return result;
    }
    std::string sql =
        "SELECT DISTINCT ps.station_id FROM product_stations ps "
        "JOIN products p ON p.product_id=ps.product_id "
        "WHERE p.content_type='GIM' AND p.nominal_date BETWEEN ?1 AND ?2 "
        "AND p.provider_code IN (";
    for (std::size_t index = 0; index < providers.size(); ++index) {
        if (index != 0) sql += ',';
        sql += '?' + std::to_string(static_cast<int>(index + 3));
    }
    sql += ")";
    if (!grades.empty()) {
        sql += " AND (";
        for (std::size_t index = 0; index < grades.size(); ++index) {
            if (index != 0) sql += " OR ";
            const auto parameter = static_cast<int>(providers.size() + 3 + 2 * index);
            sql += "(p.provider_code=?" + std::to_string(parameter)
                + " AND p.latency=?" + std::to_string(parameter + 1) + ")";
        }
        sql += ")";
    }
    sql += ";";
    auto statement = Prepare(database.get(), sql.c_str(), error);
    if (!statement
        || !BindText(statement.get(), 1,
            std::format("{:04}-{:02}-{:02}", startDate.year,
                startDate.month, startDate.day))
        || !BindText(statement.get(), 2,
            std::format("{:04}-{:02}-{:02}", endDate.year,
                endDate.month, endDate.day))) {
        result.warning = L"无法准备 GIM 测站筛选查询；当前未改变观测结果。"
            + (error.empty() ? L"" : L" 原因：" + error);
        return result;
    }
    for (std::size_t index = 0; index < providers.size(); ++index) {
        if (!BindText(statement.get(), static_cast<int>(index + 3),
                providers[index])) {
            result.warning = L"无法绑定 GIM 机构筛选条件；当前未改变观测结果。";
            result.stationCodes.clear();
            return result;
        }
    }
    for (std::size_t index = 0; index < grades.size(); ++index) {
        const auto colon = grades[index].find(':');
        const auto parameter = static_cast<int>(providers.size() + 3 + 2 * index);
        if (!BindText(statement.get(), parameter, grades[index].substr(0, colon))
            || !BindText(statement.get(), parameter + 1, grades[index].substr(colon + 1))) {
            result.warning = L"无法绑定 GIM 产品等级筛选条件。";
            return result;
        }
    }
    while (true) {
        if (stopToken.stop_requested()) {
            result.warning = L"GIM 测站筛选已取消。";
            result.stationCodes.clear();
            return result;
        }
        const int status = sqlite3_step(statement.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            result.warning = SqliteError(database.get(), L"读取 GIM 测站列表失败");
            result.stationCodes.clear();
            return result;
        }
        std::string station = UpperAscii(ColumnText(statement.get(), 0));
        station.erase(std::remove_if(station.begin(), station.end(),
            [](const unsigned char value) { return std::isspace(value) != 0; }),
            station.end());
        if (station.size() > 4U) station.resize(4U);
        if (!station.empty()) result.stationCodes.insert(std::move(station));
    }
    {
        auto cached = std::make_shared<const std::unordered_set<std::string>>(
            result.stationCodes);
        std::scoped_lock lock(gGimStationCacheMutex);
        if (gGimStationCache.size() >= 32U) {
            gGimStationCache.erase(gGimStationCache.begin());
        }
        gGimStationCache[std::move(cacheKey)] = std::move(cached);
    }
    return result;
}

bool MetadataValueMatches(
    const std::string& value,
    const std::vector<std::string>& include,
    const std::vector<std::string>& exclude) {
    const auto canonical = CanonicalSatelliteSystemTokensInternal(value);
    if (!canonical.empty()) {
        for (const auto& token : include) {
            if (std::find(canonical.begin(), canonical.end(), token)
                == canonical.end()) {
                return false;
            }
        }
        for (const auto& token : exclude) {
            if (std::find(canonical.begin(), canonical.end(), token)
                != canonical.end()) {
                return false;
            }
        }
        return true;
    }
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    if ((!include.empty() || !exclude.empty()) && normalized.empty()) {
        return false;
    }
    for (const auto& token : include) {
        if (normalized.find(token) == std::string::npos) return false;
    }
    for (const auto& token : exclude) {
        if (!token.empty() && normalized.find(token) != std::string::npos) {
            return false;
        }
    }
    return true;
}

bool MetadataModelMatches(
    const std::string& value,
    const std::vector<std::string>& include,
    const std::vector<std::string>& exclude) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    if ((!include.empty() || !exclude.empty()) && normalized.empty()) {
        return false;
    }
    if (!include.empty()
        && std::find(include.begin(), include.end(), normalized) == include.end()) {
        return false;
    }
    return std::find(exclude.begin(), exclude.end(), normalized) == exclude.end();
}

void OrBlocks(
    std::vector<BitmapBlock>& target,
    const std::vector<BitmapBlock>& source,
    const int firstOrdinal,
    const int lastOrdinal) {
    for (const auto& sourceBlock : source) {
        const int blockFirst = sourceBlock.number * kBitsPerBlock;
        const int blockLast = blockFirst + kBitsPerBlock - 1;
        if (blockLast < firstOrdinal || blockFirst > lastOrdinal) continue;
        auto& destination = FindOrAddBlock(target, sourceBlock.number);
        for (int word = 0; word < kWordsPerBlock; ++word) {
            const int wordStart = blockFirst + word * 64;
            destination.words[static_cast<std::size_t>(word)] |=
                sourceBlock.words[static_cast<std::size_t>(word)]
                & WordRangeMask(wordStart, firstOrdinal, lastOrdinal);
        }
    }
}

std::vector<BitmapBlock> FullRangeBlocks(
    const int firstOrdinal,
    const int lastOrdinal) {
    std::vector<BitmapBlock> result;
    for (int ordinal = firstOrdinal; ordinal <= lastOrdinal;) {
        const int blockNumber = BlockNumber(ordinal);
        const int blockFirst = blockNumber * kBitsPerBlock;
        const int blockLast = std::min(
            lastOrdinal, blockFirst + kBitsPerBlock - 1);
        BitmapBlock block;
        block.number = blockNumber;
        for (int word = 0; word < kWordsPerBlock; ++word) {
            block.words[static_cast<std::size_t>(word)] = WordRangeMask(
                blockFirst + word * 64, ordinal, blockLast);
        }
        result.push_back(std::move(block));
        ordinal = blockLast + 1;
    }
    return result;
}

void OrOrdinalRange(
    std::vector<BitmapBlock>& target,
    const int firstOrdinal,
    const int lastOrdinal) {
    if (lastOrdinal < firstOrdinal) return;
    const auto source = FullRangeBlocks(firstOrdinal, lastOrdinal);
    OrBlocks(target, source, firstOrdinal, lastOrdinal);
}

std::vector<BitmapBlock> IntersectBlocks(
    const std::vector<BitmapBlock>& left,
    const std::vector<BitmapBlock>& right) {
    std::vector<BitmapBlock> result;
    std::size_t leftIndex = 0, rightIndex = 0;
    while (leftIndex < left.size() && rightIndex < right.size()) {
        if (left[leftIndex].number < right[rightIndex].number) {
            ++leftIndex;
            continue;
        }
        if (right[rightIndex].number < left[leftIndex].number) {
            ++rightIndex;
            continue;
        }
        BitmapBlock block;
        block.number = left[leftIndex].number;
        bool nonzero = false;
        for (int word = 0; word < kWordsPerBlock; ++word) {
            block.words[static_cast<std::size_t>(word)] =
                left[leftIndex].words[static_cast<std::size_t>(word)]
                & right[rightIndex].words[static_cast<std::size_t>(word)];
            nonzero = nonzero
                || block.words[static_cast<std::size_t>(word)] != 0;
        }
        if (nonzero) result.push_back(std::move(block));
        ++leftIndex;
        ++rightIndex;
    }
    return result;
}

std::vector<BitmapBlock> SubtractBlocks(
    const std::vector<BitmapBlock>& left,
    const std::vector<BitmapBlock>& right) {
    std::vector<BitmapBlock> result;
    std::size_t rightIndex = 0;
    for (const auto& source : left) {
        while (rightIndex < right.size()
            && right[rightIndex].number < source.number) ++rightIndex;
        BitmapBlock block = source;
        if (rightIndex < right.size()
            && right[rightIndex].number == source.number) {
            for (int word = 0; word < kWordsPerBlock; ++word) {
                block.words[static_cast<std::size_t>(word)] &=
                    ~right[rightIndex].words[static_cast<std::size_t>(word)];
            }
        }
        if (std::any_of(block.words.begin(), block.words.end(),
                [](const std::uint64_t value) { return value != 0; })) {
            result.push_back(std::move(block));
        }
    }
    return result;
}

void SortBitmapBlocks(std::vector<BitmapBlock>& blocks) {
    std::sort(blocks.begin(), blocks.end(),
        [](const BitmapBlock& left, const BitmapBlock& right) {
            return left.number < right.number;
        });
}

std::vector<BitmapBlock> SignalDaysForToken(
    const CachedIndex& index,
    const std::size_t stationIndex,
    const PreparedSignalToken& token,
    const int firstOrdinal,
    const int lastOrdinal,
    const int rinexFamily = 0) {
    std::vector<BitmapBlock> result;
    if (stationIndex >= index.signalRangesByStation.size()) return result;
    const auto& rangesByToken = index.signalRangesByStation[stationIndex];
    for (const int family : {0, rinexFamily == 0 ? 2 : rinexFamily,
                            rinexFamily == 0 ? 34 : -1}) {
        if (family < 0) continue;
        const auto found = rangesByToken.find(
            SignalRangeKey(token.system, token.code, family));
        if (found == rangesByToken.end()) continue;
        for (const auto& [intervalFirst, intervalLast] : found->second) {
            const int first = std::max(firstOrdinal, intervalFirst);
            const int last = std::min(lastOrdinal, intervalLast);
            if (last >= first) OrOrdinalRange(result, first, last);
        }
    }
    SortBitmapBlocks(result);
    return result;
}

std::vector<BitmapBlock> SignalKnowledgeDaysForStation(
    const CachedIndex& index, const std::size_t stationIndex,
    const int firstOrdinal, const int lastOrdinal, const int rinexFamily) {
    if (!index.signalKnowledgeAvailable) return FullRangeBlocks(firstOrdinal, lastOrdinal);
    std::vector<BitmapBlock> result;
    if (stationIndex >= index.signalKnowledgeRangesByStation.size()) return result;
    const auto& ranges = index.signalKnowledgeRangesByStation[stationIndex];
    for (const int family : {rinexFamily == 0 ? 2 : rinexFamily,
                            rinexFamily == 0 ? 34 : -1}) {
        if (family < 0) continue;
        const auto found = ranges.find(family);
        if (found == ranges.end()) continue;
        for (const auto& [a, b] : found->second) {
            const int first = std::max(a, firstOrdinal), last = std::min(b, lastOrdinal);
            if (last >= first) OrOrdinalRange(result, first, last);
        }
    }
    SortBitmapBlocks(result);
    return result;
}

std::vector<BitmapBlock> SignalDaysForStation(
    const CachedIndex& index,
    const std::size_t stationIndex,
    const PreparedMetadataFilter& filter,
    const int firstOrdinal,
    const int lastOrdinal,
    const int rinexFamily = 0) {
    if (!filter.HasSignalRestriction()) {
        return FullRangeBlocks(firstOrdinal, lastOrdinal);
    }
    if (filter.invalidSignalInput
        || !index.siteLogPlusAvailable
        || stationIndex >= index.signalIntervalsByStation.size()) {
        return {};
    }
    // Unknown does not establish absence. A negative code predicate requires
    // a qualified signature on that file family/date before subtraction.
    std::vector<BitmapBlock> result = filter.signalExclude.empty()
        ? FullRangeBlocks(firstOrdinal, lastOrdinal)
        : SignalKnowledgeDaysForStation(index, stationIndex, firstOrdinal,
            lastOrdinal, rinexFamily);
    for (const auto& token : filter.signalInclude) {
        result = IntersectBlocks(
            result,
            SignalDaysForToken(index, stationIndex, token,
                firstOrdinal, lastOrdinal, rinexFamily));
        if (result.empty()) return result;
    }
    if (!filter.signalExclude.empty()) {
        std::vector<BitmapBlock> excluded;
        for (const auto& token : filter.signalExclude) {
            const auto days = SignalDaysForToken(
                index, stationIndex, token, firstOrdinal, lastOrdinal, rinexFamily);
            OrBlocks(excluded, days, firstOrdinal, lastOrdinal);
        }
        SortBitmapBlocks(excluded);
        result = SubtractBlocks(result, excluded);
    }
    return result;
}

std::vector<BitmapBlock> SignalFileDaysForStation(
    const CachedIndex& index, const std::size_t stationIndex,
    const int firstOrdinal, const int lastOrdinal,
    const int rinexFamily, const std::string& interval) {
    if (!index.signalFileAvailabilityRestricted) return FullRangeBlocks(firstOrdinal, lastOrdinal);
    std::vector<BitmapBlock> result;
    if (stationIndex >= index.signalFileAvailabilityByStation.size()) return result;
    const auto& profiles = index.signalFileAvailabilityByStation[stationIndex];
    const auto found = profiles.find(SignalRangeKey("", NormalizedInterval(interval), rinexFamily));
    if (found == profiles.end()) return result;
    for (const auto& [a, b] : found->second) {
        const int first = std::max(a, firstOrdinal), last = std::min(b, lastOrdinal);
        if (last >= first) OrOrdinalRange(result, first, last);
    }
    SortBitmapBlocks(result);
    return result;
}

bool SignalMatchesDay(
    const CachedIndex& index,
    const std::size_t stationIndex,
    const PreparedMetadataFilter& filter,
    const int ordinal,
    const int rinexFamily = 0,
    const std::string& interval = "30S") {
    if (!filter.HasSignalRestriction()) return true;
    if (filter.invalidSignalInput
        || !index.siteLogPlusAvailable
        || stationIndex >= index.signalRangesByStation.size()) return false;
    if (index.signalFileAvailabilityRestricted) {
        const auto fileDays = SignalFileDaysForStation(
            index, stationIndex, ordinal, ordinal, rinexFamily, interval);
        if (!BlocksHaveAnyDayInRange(fileDays, ordinal, ordinal)) return false;
    }
    if (!filter.signalExclude.empty() && index.signalKnowledgeAvailable) {
        const auto known = SignalKnowledgeDaysForStation(
            index, stationIndex, ordinal, ordinal, rinexFamily);
        if (!BlocksHaveAnyDayInRange(known, ordinal, ordinal)) return false;
    }
    const auto contains = [&](const PreparedSignalToken& token) {
        const auto& rangesByToken = index.signalRangesByStation[stationIndex];
        for (const int family : {0, rinexFamily == 0 ? 2 : rinexFamily,
                                rinexFamily == 0 ? 34 : -1}) {
            if (family < 0) continue;
            const auto found = rangesByToken.find(
                SignalRangeKey(token.system, token.code, family));
            if (found == rangesByToken.end()) continue;
            if (std::any_of(found->second.begin(), found->second.end(),
                [&](const auto& range) {
                    return range.first <= ordinal && ordinal <= range.second;
                })) return true;
        }
        return false;
    };
    for (const auto& token : filter.signalInclude) {
        if (!contains(token)) return false;
    }
    for (const auto& token : filter.signalExclude) {
        if (contains(token)) return false;
    }
    return true;
}

std::vector<BitmapBlock> AvailabilityForMask(
    const QueryStationAccumulator& accumulator,
    const ObservationNetworkMask mask,
    const int firstOrdinal,
    const int lastOrdinal) {
    std::vector<BitmapBlock> result;
    for (const auto& item : accumulator.byNetwork) {
        if ((item.bit & mask) != 0) {
            OrBlocks(result, item.blocks, firstOrdinal, lastOrdinal);
        }
    }
    SortBitmapBlocks(result);
    return result;
}

std::vector<BitmapBlock> MetadataDaysForRecord(
    const CachedSiteRecord& record,
    const PreparedMetadataFilter& filter,
    const int firstOrdinal,
    const int lastOrdinal) {
    auto receiverDays = filter.HasReceiverRestriction()
        ? std::vector<BitmapBlock>{}
        : FullRangeBlocks(firstOrdinal, lastOrdinal);
    auto antennaDays = filter.HasAntennaRestriction()
        ? std::vector<BitmapBlock>{}
        : FullRangeBlocks(firstOrdinal, lastOrdinal);
    for (const auto& segment : record.segments) {
        const int first = std::max(firstOrdinal, segment.firstDay);
        const int last = std::min(lastOrdinal, segment.lastDayExclusive - 1);
        if (last < first) continue;
        if (segment.kind == 0 && filter.HasReceiverRestriction()
            && MetadataValueMatches(segment.systems,
                filter.systemsInclude, filter.systemsExclude)
            && MetadataModelMatches(segment.type,
                filter.receiverInclude, filter.receiverExclude)) {
            OrOrdinalRange(receiverDays, first, last);
        } else if (segment.kind == 1 && filter.HasAntennaRestriction()
            && MetadataModelMatches(segment.type,
                filter.antennaInclude, filter.antennaExclude)) {
            OrOrdinalRange(antennaDays, first, last);
        }
    }
    return IntersectBlocks(receiverDays, antennaDays);
}

const CachedSiteSegment* FindEffectiveSegment(
    const CachedSiteRecord& record,
    const int kind,
    const int ordinal) {
    const auto found = std::find_if(record.segments.begin(), record.segments.end(),
        [&](const CachedSiteSegment& segment) {
            return segment.kind == kind && segment.firstDay <= ordinal
                && ordinal < segment.lastDayExclusive;
        });
    return found == record.segments.end() ? nullptr : &*found;
}

void ApplyCachedSiteRecord(
    StationFile& station,
    const CachedSiteRecord& record,
    const int ordinal) {
    // A source row can carry the current scalar equipment values. Clear
    // them before applying the dated record so a missing historical segment
    // cannot inherit a future receiver, antenna or serial number.
    station.siteLogReceiver.clear();
    station.siteLogSystems.clear();
    station.siteLogReceiverSerial.clear();
    station.siteLogFirmware.clear();
    station.siteLogReceiverDateInstalled.clear();
    station.siteLogAntenna.clear();
    station.siteLogRadome.clear();
    station.siteLogAntennaSerial.clear();
    station.siteLogAntennaReferencePoint.clear();
    station.siteLogAntennaDateInstalled.clear();
    station.hasSiteLog = true;
    station.siteLogDomes = record.domes;
    station.siteLogName = record.siteName;
    station.siteLogCity = record.city;
    station.siteLogCountry = record.country;
    station.siteLogAgency = record.agency;
    station.siteLogOperator = record.operatorName;
    station.siteLogRevisionDate = record.revisionDate;
    station.siteLogSourceDirectory = record.sourceDirectory;
    station.siteLogUrl = record.sourceUrl;
    if (const auto* receiver = FindEffectiveSegment(record, 0, ordinal)) {
        station.siteLogReceiver = receiver->type;
        const auto systems = CanonicalSatelliteSystemTokensInternal(
            receiver->systems);
        station.siteLogSystems.clear();
        for (const auto& system : systems) {
            if (!station.siteLogSystems.empty()) station.siteLogSystems += '+';
            station.siteLogSystems += system;
        }
        station.siteLogReceiverSerial = receiver->serial;
        station.siteLogFirmware = receiver->firmware;
        station.siteLogReceiverDateInstalled = receiver->installedDate;
    }
    if (const auto* antenna = FindEffectiveSegment(record, 1, ordinal)) {
        station.siteLogAntenna = antenna->type;
        station.siteLogRadome = antenna->radome;
        station.siteLogAntennaSerial = antenna->serial;
        station.siteLogAntennaReferencePoint = antenna->referencePoint;
        station.siteLogAntennaDateInstalled = antenna->installedDate;
    }
}

std::optional<int> FirstSetOrdinal(const std::vector<BitmapBlock>& blocks) {
    for (const auto& block : blocks) {
        for (int word = 0; word < kWordsPerBlock; ++word) {
            const auto bits = block.words[static_cast<std::size_t>(word)];
            if (bits == 0) continue;
            return block.number * kBitsPerBlock + word * 64
                + static_cast<int>(std::countr_zero(bits));
        }
    }
    return std::nullopt;
}

std::vector<std::string> CachedStationAliasKeys(const StationFile& station) {
    std::vector<std::string> keys;
    keys.reserve(3);
    const auto add = [&](std::string value) {
        value = UpperAscii(std::move(value));
        if (value.empty()
            || std::find(keys.begin(), keys.end(), value) != keys.end()) {
            return;
        }
        keys.push_back(std::move(value));
    };
    if (station.stationId.size() == 9) add(station.stationId);
    std::string four = UpperAscii(station.fourCharId);
    if (four.empty() && station.stationId.size() >= 4) {
        four = UpperAscii(station.stationId.substr(0, 4));
    }
    if (!four.empty()) {
        four.resize(std::min<std::size_t>(4, four.size()));
        add("FOUR:" + four);
    }
    // A few legacy source rows persist a non-standard raw key (for example a
    // bare four-character value).  Keep it as a final alias fallback.
    if (!station.stationId.empty()) add(station.stationId);
    return keys;
}

std::optional<std::size_t> FindCachedStationIndex(
    const CachedIndex& index,
    const StationFile& station,
    const ObservationNetworkMask requestedMask = 0) {
    const ObservationNetworkMask mask = requestedMask != 0
        ? requestedMask : station.availableNetworks;

    // A unique complete station identifier is stronger than a legacy
    // four-character alias. Do not mix the two candidate sets: the short
    // alias can still point to an older row for the same source network.
    if (station.stationId.size() == 9) {
        const auto full = index.stationAliasesByKey.find(UpperAscii(station.stationId));
        std::vector<std::size_t> exact;
        if (full != index.stationAliasesByKey.end()) {
            for (const auto& alias : full->second) {
                if (alias.stationIndex >= index.stations.size()) continue;
                if (mask != 0 && alias.networkMask != 0 && !(alias.networkMask & mask)) continue;
                if (station.availableNetworks != 0 && alias.networkMask != 0
                    && !(alias.networkMask & station.availableNetworks)) continue;
                if (std::find(exact.begin(), exact.end(), alias.stationIndex) == exact.end())
                    exact.push_back(alias.stationIndex);
            }
        }
        if (exact.size() == 1) return exact.front();
    }

    // Resolve through the network-scoped alias layer first.  This is
    // essential after a reused four-character code is split into separate
    // physical stations: a global stationId map cannot distinguish OKAK/NGS
    // from OKAK/SOPAC or GA STAR from SOPAC/PANGA STAR.
    std::vector<std::size_t> aliasCandidates;
    std::unordered_map<std::size_t, ObservationNetworkMask> aliasMasks;
    for (const auto& key : CachedStationAliasKeys(station)) {
        const auto found = index.stationAliasesByKey.find(key);
        if (found == index.stationAliasesByKey.end()) continue;
        for (const auto& alias : found->second) {
            if (alias.stationIndex >= index.stations.size()) continue;
            if (mask != 0 && alias.networkMask != 0
                && (alias.networkMask & mask) == 0) continue;
            if (station.availableNetworks != 0
                && alias.networkMask != 0
                && (alias.networkMask & station.availableNetworks) == 0) continue;
            if (std::find(aliasCandidates.begin(), aliasCandidates.end(),
                          alias.stationIndex) == aliasCandidates.end()) {
                aliasCandidates.push_back(alias.stationIndex);
            }
            aliasMasks[alias.stationIndex] |= alias.networkMask;
        }
    }
    if (aliasCandidates.size() == 1) return aliasCandidates.front();
    if (aliasCandidates.size() > 1) {
        // A dated row may carry coordinates even when its source key is only
        // four characters.  Use a unique nearby candidate as a second,
        // independent discriminator; never choose an arbitrary first row.
        if (station.hasCoordinate
            && ValidLatitudeLongitude(station.latitude, station.longitude)) {
            std::size_t nearest = 0;
            double nearestDistance = std::numeric_limits<double>::max();
            bool tie = false;
            for (const auto candidate : aliasCandidates) {
                const auto& value = index.stations[candidate];
                if (!value.hasCoordinate
                    || !ValidLatitudeLongitude(value.latitude, value.longitude)) {
                    continue;
                }
                const double distance = DistanceKm(
                    station.latitude, station.longitude,
                    value.latitude, value.longitude);
                if (distance < nearestDistance - 1.0e-9) {
                    nearest = candidate;
                    nearestDistance = distance;
                    tie = false;
                } else if (std::abs(distance - nearestDistance) <= 1.0e-9) {
                    tie = true;
                }
            }
            if (!tie && nearestDistance <= 3.0) return nearest;
        }
        // If the source row explicitly names one network, a candidate whose
        // alias mask overlaps that network is safe to prefer.  This matters
        // for a unified SOPAC+PANGA station: its central available mask is
        // the union, while a dated PANGA row carries only the PANGA bit.  For
        // a genuinely multi-network ambiguous row we retain the null result.
        if (station.availableNetworks != 0) {
            std::vector<std::size_t> exact;
            for (const auto candidate : aliasCandidates) {
                const auto foundMask = aliasMasks.find(candidate);
                if (foundMask != aliasMasks.end()
                    && (foundMask->second & station.availableNetworks) != 0) {
                    exact.push_back(candidate);
                }
            }
            if (exact.size() == 1) return exact.front();
        }
        return std::nullopt;
    }

    if (!station.stationId.empty()) {
        const auto found = index.stationById.find(UpperAscii(station.stationId));
        if (found != index.stationById.end()
            && (mask == 0
                || (index.stations[found->second].availableNetworks & mask) != 0)) {
            return found->second;
        }
    }
    if (!station.fourCharId.empty()) {
        const std::string four = UpperAscii(station.fourCharId);
        const auto country = index.stationByFourCountry.find(
            four + '\x1f' + UpperAscii(station.country));
        if (country != index.stationByFourCountry.end()
            && (mask == 0
                || (index.stations[country->second].availableNetworks & mask) != 0)) {
            return country->second;
        }
        const auto unique = index.uniqueStationByFour.find(four);
        if (unique != index.uniqueStationByFour.end()
            && (mask == 0
                || (index.stations[unique->second].availableNetworks & mask) != 0)) {
            return unique->second;
        }
    }
    return std::nullopt;
}


// Resolve station identities against the current central index, rather than
// persisting central row numbers across later index rebuilds. Equipment
// fields and serials were resolved independently by the maintenance builder.
bool ApplyTrustedEquipmentOverlay(CachedIndex& index,
    const std::filesystem::path& path, std::wstring& error) {
    if (GetFileStamp(path).size == 0) return true;
    DatabasePtr database;
    if (!OpenReadOnly(path, database, error)) return false;
    if (!TableExists(database.get(), "stations")
        || !TableExists(database.get(), "site_records")
        || !TableExists(database.get(), "site_segments")
        || MetadataUnsigned(database.get(), "schema_version") != 1) {
        error = L"可信设备时序知识库结构或版本不兼容：" + path.wstring();
        return false;
    }
    std::unordered_map<sqlite3_int64, std::size_t> identities;
    auto stations = Prepare(database.get(),
        "SELECT station_uid,station_id,four_char_id,country,has_coordinate,"
        "latitude,longitude,available_networks FROM stations;", error);
    if (!stations) return false;
    for (;;) {
        const int status = sqlite3_step(stations.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            error = SqliteError(database.get(), L"读取可信时序测站身份失败");
            return false;
        }
        StationFile station;
        station.stationId = ColumnText(stations.get(), 1);
        station.fourCharId = ColumnText(stations.get(), 2);
        station.country = ColumnText(stations.get(), 3);
        station.hasCoordinate = sqlite3_column_int(stations.get(), 4) != 0;
        station.latitude = sqlite3_column_double(stations.get(), 5);
        station.longitude = sqlite3_column_double(stations.get(), 6);
        station.availableNetworks = static_cast<ObservationNetworkMask>(
            sqlite3_column_int64(stations.get(), 7));
        const auto identity = FindCachedStationIndex(
            index, station, station.availableNetworks);
        if (identity) identities.emplace(
            sqlite3_column_int64(stations.get(), 0), *identity);
    }
    std::vector<CachedSiteRecord> overlays;
    std::unordered_map<sqlite3_int64, std::size_t> recordIds;
    std::unordered_map<std::size_t, ObservationNetworkMask> replaced;
    auto records = Prepare(database.get(),
        "SELECT record_id,station_uid,applicability_mask,tier,network_order,"
        "domes,site_name,city,country,agency,operator_name,revision_date,"
        "source_directory,source_url FROM site_records ORDER BY record_id;", error);
    if (!records) return false;
    for (;;) {
        const int status = sqlite3_step(records.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            error = SqliteError(database.get(), L"读取可信时序来源失败");
            return false;
        }
        const auto identity = identities.find(sqlite3_column_int64(records.get(), 1));
        if (identity == identities.end()) continue;
        CachedSiteRecord record;
        record.stationIndex = identity->second;
        record.applicabilityMask = static_cast<ObservationNetworkMask>(
            sqlite3_column_int64(records.get(), 2))
            & index.stations[identity->second].availableNetworks;
        if (record.applicabilityMask == 0) continue;
        record.tier = sqlite3_column_int(records.get(), 3);
        record.networkOrder = sqlite3_column_int(records.get(), 4);
        record.domes = ColumnText(records.get(), 5);
        record.siteName = ColumnText(records.get(), 6);
        record.city = ColumnText(records.get(), 7);
        record.country = ColumnText(records.get(), 8);
        record.agency = ColumnText(records.get(), 9);
        record.operatorName = ColumnText(records.get(), 10);
        record.revisionDate = ColumnText(records.get(), 11);
        record.sourceDirectory = ColumnText(records.get(), 12);
        record.sourceUrl = ColumnText(records.get(), 13);
        replaced[record.stationIndex] |= record.applicabilityMask;
        recordIds.emplace(sqlite3_column_int64(records.get(), 0), overlays.size());
        overlays.push_back(std::move(record));
    }
    auto segments = Prepare(database.get(),
        "SELECT record_id,kind,first_day,last_day_exclusive,equipment_type,"
        "systems,serial,firmware,radome,reference_point,installed_date "
        "FROM site_segments ORDER BY record_id,kind,first_day;", error);
    if (!segments) return false;
    for (;;) {
        const int status = sqlite3_step(segments.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            error = SqliteError(database.get(), L"读取可信设备段失败");
            return false;
        }
        const auto record = recordIds.find(sqlite3_column_int64(segments.get(), 0));
        if (record == recordIds.end()) continue;
        CachedSiteSegment segment;
        segment.kind = sqlite3_column_int(segments.get(), 1);
        segment.firstDay = sqlite3_column_int(segments.get(), 2);
        segment.lastDayExclusive = sqlite3_column_int(segments.get(), 3);
        if ((segment.kind != 0 && segment.kind != 1)
            || segment.firstDay >= segment.lastDayExclusive) {
            error = L"可信设备时序包含无效日期段。";
            return false;
        }
        segment.type = ColumnText(segments.get(), 4);
        segment.systems = ColumnText(segments.get(), 5);
        segment.serial = ColumnText(segments.get(), 6);
        segment.firmware = ColumnText(segments.get(), 7);
        segment.radome = ColumnText(segments.get(), 8);
        segment.referencePoint = ColumnText(segments.get(), 9);
        segment.installedDate = ColumnText(segments.get(), 10);
        overlays[record->second].segments.push_back(std::move(segment));
    }
    // Replace older metadata for these source masks. Missing trusted fields
    // stay missing instead of falling through to undated/stale scalars.
    std::vector<CachedSiteRecord> combined;
    combined.reserve(index.siteRecords.size() + overlays.size());
    for (auto& record : index.siteRecords) {
        const auto mask = replaced.find(record.stationIndex);
        if (mask != replaced.end()) record.applicabilityMask &= ~mask->second;
        if (record.applicabilityMask != 0) combined.push_back(std::move(record));
    }
    for (auto& record : overlays) combined.push_back(std::move(record));
    std::stable_sort(combined.begin(), combined.end(),
        [](const CachedSiteRecord& a, const CachedSiteRecord& b) {
            return std::tie(a.stationIndex,a.tier,a.networkOrder)
                < std::tie(b.stationIndex,b.tier,b.networkOrder);
        });
    index.siteRecords = std::move(combined);
    index.siteRecordsByStation.assign(index.stations.size(), {});
    for (std::size_t i = 0; i < index.siteRecords.size(); ++i) {
        index.siteRecordsByStation[index.siteRecords[i].stationIndex].push_back(i);
    }
    return true;
}

bool MetadataRecordMatchesDay(
    const CachedSiteRecord& record,
    const PreparedMetadataFilter& filter,
    const int ordinal) {
    if (filter.HasReceiverRestriction()) {
        const auto* receiver = FindEffectiveSegment(record, 0, ordinal);
        if (receiver == nullptr
            || !MetadataValueMatches(receiver->systems,
                filter.systemsInclude, filter.systemsExclude)
            || !MetadataModelMatches(receiver->type,
                filter.receiverInclude, filter.receiverExclude)) {
            return false;
        }
    }
    if (filter.HasAntennaRestriction()) {
        const auto* antenna = FindEffectiveSegment(record, 1, ordinal);
        if (antenna == nullptr
            || !MetadataModelMatches(antenna->type,
                filter.antennaInclude, filter.antennaExclude)) {
            return false;
        }
    }
    return true;
}

std::wstring FreshnessError(
    const CachedIndex& index,
    const ObservationNetworkMask requested) {
    for (const auto& source : index.sources) {
        if ((source.networkMask & requested) == 0) continue;
        if (GetFileStamp(source.path) != source.stamp) {
            return L"中央高速观测索引已过期（源库已变化）："
                + source.path.filename().wstring()
                + L"。请重新安装官方知识库后重试。";
        }
    }
    return {};
}

}  // namespace

GimProductGradeChoices QueryGimProductGradeChoices(
    const QueryDate& startDate, const QueryDate& endDate) {
    GimProductGradeChoices result;
    DatabasePtr database;
    if (!OpenReadOnly(DefaultGimStationDatabasePath(), database, result.error)) return result;
    auto statement = Prepare(database.get(),
        "SELECT DISTINCT provider_code, latency FROM products "
        "WHERE content_type='GIM' AND nominal_date BETWEEN ?1 AND ?2 "
        "ORDER BY provider_code, latency;", result.error);
    if (!statement || !BindText(statement.get(), 1,
            std::format("{:04}-{:02}-{:02}", startDate.year, startDate.month, startDate.day))
        || !BindText(statement.get(), 2,
            std::format("{:04}-{:02}-{:02}", endDate.year, endDate.month, endDate.day))) return result;
    const auto providers = ParseGimProviders("IGS,CODE,ESA,JPL,UPC,CAS,EMR,WHU");
    int status;
    while ((status = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const auto tokens = ParseGimProductGrades(
            ColumnText(statement.get(), 0) + ":" + ColumnText(statement.get(), 1), providers);
        result.values.insert(result.values.end(), tokens.begin(), tokens.end());
    }
    if (status != SQLITE_DONE) {
        result.values.clear();
        result.error = SqliteError(database.get(), L"读取 GIM 产品等级失败");
    }
    return result;
}

std::vector<std::string> CanonicalSatelliteSystemTokens(
    const std::string& value) {
    return CanonicalSatelliteSystemTokensInternal(value);
}

ObservationNetworkMask ObservationSearchSupportedNetworkMask() {
    ObservationNetworkMask mask = NetworkBit(ObservationNetwork::Mgex);
    for (const auto& source : kSourceSpecs) mask |= NetworkBit(source.network);
    return mask;
}

std::filesystem::path DefaultObservationSearchIndexPath(
    const std::filesystem::path& knowledgeBaseDirectory) {
    const auto directory = knowledgeBaseDirectory.empty()
        ? DefaultKnowledgeBaseDirectory() : knowledgeBaseDirectory;
    return ResolveDatabaseFilePath(directory / L"observation_search.sqlite");
}

ObservationSearchBuildResult BuildObservationSearchIndex(
    const ObservationSearchBuildOptions& options,
    const std::stop_token stopToken) {
    ObservationSearchBuildResult result;
    const auto directory = options.sourceDirectory.empty()
        ? DefaultKnowledgeBaseDirectory() : options.sourceDirectory;
    result.outputPath = options.outputPath.empty()
        ? DefaultObservationSearchIndexPath(directory) : options.outputPath;
    const ObservationNetworkMask requested = options.networkMask == 0
        ? ObservationSearchSupportedNetworkMask() : options.networkMask;
    std::error_code filesystemError;
    std::filesystem::create_directories(
        result.outputPath.parent_path(), filesystemError);
    if (filesystemError) {
        result.error = L"无法创建高速索引目录："
            + result.outputPath.parent_path().wstring();
        return result;
    }
    const std::filesystem::path temporary(
        result.outputPath.wstring() + L".building");
    filesystemError.clear();
    std::filesystem::remove(temporary, filesystemError);

    BuildMemory memory;
    memory.identities.reserve(32768);
    memory.identityByKey.reserve(32768);
    memory.profiles.reserve(65536);
    memory.profileByKey.reserve(65536);

    for (const auto& source : kSourceSpecs) {
        const ObservationNetworkMask sourceSelection = source.igs
            ? NetworkBit(ObservationNetwork::Igs) | NetworkBit(ObservationNetwork::Mgex)
            : NetworkBit(source.network);
        if ((requested & sourceSelection) == 0) continue;
        if (stopToken.stop_requested()) {
            result.error = L"高速索引构建已取消。";
            return result;
        }
        const auto path = ResolveDatabaseFilePath(directory / source.fileName);
        filesystemError.clear();
        if (!std::filesystem::is_regular_file(path, filesystemError)) {
            result.warnings.push_back(
                std::wstring(source.label) + L"：知识库不存在，已跳过。");
            continue;
        }
        const FileStamp before = GetFileStamp(path);
        DatabasePtr database;
        std::wstring error;
        if (!OpenReadOnly(path, database, error)) {
            result.warnings.push_back(
                std::wstring(source.label) + L"：" + error);
            continue;
        }
        const bool schemaOkay = TableExists(database.get(), "daily_files")
            && TableExists(database.get(), "daily_scans")
            && (!source.igs
                || (TableExists(database.get(), "daily_observation_files")
                    && TableExists(database.get(), "daily_source_scans")));
        if (!schemaOkay) {
            result.warnings.push_back(
                std::wstring(source.label) + L"：知识库结构不兼容，已跳过。");
            continue;
        }
        if (!Execute(database.get(), "BEGIN;", error)) {
            result.error = std::wstring(source.label) + L"：" + error;
            return result;
        }
        SourceState state;
        state.networkMask = sourceSelection;
        state.path = std::filesystem::absolute(path);
        const bool scanned = source.igs
            ? ScanIgsSource(
                database.get(), source, options, path, memory,
                stopToken, state, error)
            : ScanSingleSource(
                database.get(), source, options, path, memory,
                stopToken, state, error);
        std::wstring ignored;
        Execute(database.get(), scanned ? "COMMIT;" : "ROLLBACK;", ignored);
        database.reset();
        if (!scanned) {
            result.error = std::wstring(source.label) + L"：" + error;
            return result;
        }
        // Site Log rows live in the same source SQLite file.  Read the
        // already-normalized summaries and complete equipment histories once
        // during this offline rebuild; no network access is performed.
        ScanSiteLogsForSource(path, source, memory, result.warnings);
        const FileStamp after = GetFileStamp(path);
        if (after != before) {
            result.warnings.push_back(std::wstring(source.label)
                + L"：源知识库在本次一致性快照之后继续写入；已提交可用但立即标记为过期的快照。"
                  L"源库构建结束后再次运行即可补入新增日期。");
        }
        // BEGIN pins all availability reads to one SQLite snapshot. Store the
        // stamp observed immediately before opening it. If a long-running
        // builder appends rows concurrently, freshness validation marks this
        // derived index stale without discarding a consistent snapshot.
        state.stamp = before;
        memory.sources.push_back(std::move(state));
        result.indexedNetworks |= sourceSelection;
    }
    if (memory.sources.empty()) {
        result.error = L"没有找到可用于构建高速索引的观测知识库。";
        return result;
    }
    result.sourceRowsRead = memory.rowsRead;

    DatabasePtr output;
    if (!OpenOutput(temporary, output, result.error)
        || !EnsureOutputSchema(output.get(), result.error)
        || !WriteOutput(output.get(), memory, result, result.error)) {
        output.reset();
        filesystemError.clear();
        std::filesystem::remove(temporary, filesystemError);
        return result;
    }
    std::wstring ignored;
    Execute(output.get(), "PRAGMA optimize;", ignored);
    output.reset();

    // Keep the previous valid index until the complete temporary database is
    // closed.  Windows cannot atomically replace an existing file with rename,
    // so move it to a short-lived backup and restore it on failure.
    const std::filesystem::path backup(result.outputPath.wstring() + L".previous");
    filesystemError.clear();
    std::filesystem::remove(backup, filesystemError);
    filesystemError.clear();
    if (std::filesystem::exists(result.outputPath, filesystemError)) {
        filesystemError.clear();
        std::filesystem::rename(result.outputPath, backup, filesystemError);
        if (filesystemError) {
            result.error = L"无法替换正在使用的高速索引；请关闭下载器后重试。";
            return result;
        }
    }
    filesystemError.clear();
    std::filesystem::rename(temporary, result.outputPath, filesystemError);
    if (filesystemError) {
        std::error_code restoreError;
        if (std::filesystem::exists(backup, restoreError)) {
            restoreError.clear();
            std::filesystem::rename(backup, result.outputPath, restoreError);
        }
        result.error = L"无法提交新的高速搜索索引。";
        return result;
    }
    filesystemError.clear();
    std::filesystem::remove(backup, filesystemError);
    result.complete = true;
    return result;
}

namespace {

struct DailyObservationQuality {
    std::optional<double> observationCompleteness;
    std::optional<double> latencyHours;
    std::optional<double> meanAbsoluteDetrendedPositionMillimeters;
    std::optional<double> meanAbsoluteResidualMillimeters;
    std::optional<double> meanMultipathCentimeters;
    std::optional<double> totalCycleSlips;
};

std::string QualitySystemToken(const std::string_view key) {
    const auto token = key.substr(0, key.find(' '));
    const auto systems = CanonicalSatelliteSystemTokens(std::string(token));
    return systems.size() == 1 ? systems.front() : std::string{};
}


struct StationQualitySummary {
    // No metric is borrowed from another day. An absent metric makes that
    // date unqualified only when the corresponding filter is enabled.
    std::map<int, DailyObservationQuality> daily;
};

struct ObservationQualityCatalogue {
    std::unordered_map<std::string, StationQualitySummary> byStation;
    std::unordered_map<std::string, std::string> uniqueStationByFourChar;
    std::wstring error;
};

bool HasObservationQualityFilter(const ObservationRangeOptions& options) {
    return options.observationCompletenessEnabled
        || options.latencyEnabled
        || options.positionTimeSeriesEnabled
        || options.residualsEnabled
        || options.meanMultipathEnabled
        || options.cycleSlipsEnabled;
}

// The six IGS Network QC columns are stored as small, flat JSON objects.  A
// tiny number iterator is enough for the six official categories and avoids
// making the interactive query depend on SQLite JSON extension settings.
template <typename Callback>
void ForEachFlatJsonNumber(const std::string_view json, Callback callback) {
    std::size_t cursor = 0;
    while (cursor < json.size()) {
        const auto keyStart = json.find('"', cursor);
        if (keyStart == std::string_view::npos) break;
        const auto keyEnd = json.find('"', keyStart + 1);
        if (keyEnd == std::string_view::npos) break;
        const auto colon = json.find(':', keyEnd + 1);
        if (colon == std::string_view::npos) break;
        std::size_t valueStart = colon + 1;
        while (valueStart < json.size()
               && std::isspace(static_cast<unsigned char>(json[valueStart]))) {
            ++valueStart;
        }
        const char* begin = json.data() + valueStart;
        char* end = nullptr;
        const double value = std::strtod(begin, &end);
        if (end != begin && std::isfinite(value)) {
            callback(json.substr(keyStart + 1, keyEnd - keyStart - 1), value);
            cursor = static_cast<std::size_t>(end - json.data());
        } else {
            cursor = colon + 1;
        }
    }
}

std::string QueryDateText(const QueryDate& value) {
    return std::format("{:04}-{:02}-{:02}",
        value.year, value.month, value.day);
}

ObservationQualityCatalogue LoadObservationQualityCatalogue(
    const std::filesystem::path& databasePath,
    const QueryDate& startDate,
    const QueryDate& endDate,
    std::vector<std::string> stationKeys,
    const bool weatherActive,
    const std::vector<BitmapBlock>& weatherBlocks,
    const bool dailyCoverageActive,
    const std::vector<BitmapBlock>& dailyCoverageBlocks,
    const ObservationRangeOptions& options,
    const std::stop_token stopToken) {
    ObservationQualityCatalogue result;
    DatabasePtr database;
    if (!OpenReadOnly(databasePath, database, result.error)) return result;
    if (!TableExists(database.get(), "daily_observation_quality")) {
        result.error = L"质量知识库缺少 daily_observation_quality 表。";
        return result;
    }
    std::wstring prepareError;
    auto rows = Prepare(database.get(),
        "SELECT station_key,observation_date,observations_json,"
        "latency_json,position_json,residuals_json,"
        "multipath_json,cycle_slips_json "
        "FROM daily_observation_quality "
        "WHERE network='IGS' AND station_key=?1 "
        "AND observation_date BETWEEN ?2 AND ?3 "
        "ORDER BY observation_date;",
        prepareError);
    if (!rows) {
        result.error = std::move(prepareError);
        return result;
    }
    std::sort(stationKeys.begin(), stationKeys.end());
    stationKeys.erase(
        std::unique(stationKeys.begin(), stationKeys.end()), stationKeys.end());
    const std::string firstDate = QueryDateText(startDate);
    const std::string lastDate = QueryDateText(endDate);
    const auto selectedSystem = [](const std::string& value) {
        if (value.empty()) return std::string{};
        const auto token = QualitySystemToken(value);
        return token.empty() ? std::string("unsupported") : token;
    };
    const auto completenessSystem = selectedSystem(options.observationCompletenessSystem);
    const auto multipathSystem = selectedSystem(options.multipathSystem);
    const auto cycleSlipsSystem = selectedSystem(options.cycleSlipsSystem);
    std::size_t rowIndex = 0;
    for (const auto& requestedStation : stationKeys) {
        if (stopToken.stop_requested()) {
            result.error = L"查询已取消。";
            return result;
        }
        sqlite3_reset(rows.get());
        sqlite3_clear_bindings(rows.get());
        if (!BindText(rows.get(), 1, requestedStation)
            || !BindText(rows.get(), 2, firstDate)
            || !BindText(rows.get(), 3, lastDate)) {
            result.error = SqliteError(database.get(), L"绑定质量筛选条件失败");
            return result;
        }
        int status = SQLITE_ROW;
        while ((status = sqlite3_step(rows.get())) == SQLITE_ROW) {
            if ((rowIndex++ & 0x3ffU) == 0U && stopToken.stop_requested()) {
                result.error = L"查询已取消。";
                return result;
            }
            int ordinal = 0;
            if (!TextDateToOrdinal(ColumnText(rows.get(), 1), ordinal)) continue;
            if (weatherActive
                && !BlocksHaveAnyDayInRange(
                    weatherBlocks, ordinal, ordinal)) continue;
            if (dailyCoverageActive
                && !BlocksHaveAnyDayInRange(
                    dailyCoverageBlocks, ordinal, ordinal)) continue;
            const std::string stationKey = UpperAscii(ColumnText(rows.get(), 0));
            if (stationKey.empty()) continue;
            auto& aggregate = result.byStation[stationKey];
            DailyObservationQuality daily;
            double dailyExpected = 0.0;
            double dailyActual = 0.0;
            bool hasExpected = false;
            bool hasActual = false;
            ForEachFlatJsonNumber(ColumnText(rows.get(), 2),
                [&](const std::string_view key, const double value) {
                    if (value < 0.0) return;
                    const bool actual = key.size() >= 4
                        && key.substr(key.size() - 4) == " Obs";
                    if (!completenessSystem.empty()
                        && QualitySystemToken(key) != completenessSystem) return;
                    if (actual) {
                        dailyActual += value;
                        hasActual = true;
                    } else {
                        dailyExpected += value;
                        hasExpected = true;
                    }
                });
            if (hasExpected && hasActual
                && dailyExpected > 0.0 && std::isfinite(dailyExpected)
                && std::isfinite(dailyActual)) {
                daily.observationCompleteness = std::clamp(
                    100.0 * dailyActual / dailyExpected, 0.0, 100.0);
            }
            ForEachFlatJsonNumber(ColumnText(rows.get(), 3),
                [&](const std::string_view key, const double value) {
                    if (key != "Latency" || value < 0.0) return;
                    daily.latencyHours = value;
                });
            double positionSum = 0.0;
            int positionValues = 0;
            ForEachFlatJsonNumber(ColumnText(rows.get(), 4),
                [&](const std::string_view key, const double value) {
                    if (key != "ENV Detrended Time Series"
                        && key != "North Detrended"
                        && key != "Vertical Detrended") return;
                    positionSum += std::abs(value);
                    ++positionValues;
                });
            if (positionValues > 0 && std::isfinite(positionSum))
                daily.meanAbsoluteDetrendedPositionMillimeters =
                    positionSum / positionValues;
            double residualSum = 0.0;
            int residualValues = 0;
            ForEachFlatJsonNumber(ColumnText(rows.get(), 5),
                [&](const std::string_view key, const double value) {
                    if (key != "East" && key != "North"
                        && key != "Vertical") return;
                    residualSum += std::abs(value);
                    ++residualValues;
                });
            if (residualValues > 0 && std::isfinite(residualSum))
                daily.meanAbsoluteResidualMillimeters = residualSum / residualValues;
            double multipathSum = 0.0;
            int multipathValues = 0;
            ForEachFlatJsonNumber(ColumnText(rows.get(), 6),
                [&](const std::string_view key, const double value) {
                    // Zero denotes an unavailable constellation/metric in the
                    // upstream payload and must not make a station look better.
                    if (value <= 0.0 || (!multipathSystem.empty()
                        && QualitySystemToken(key) != multipathSystem)) return;
                    multipathSum += value;
                    ++multipathValues;
                });
            if (multipathValues > 0 && std::isfinite(multipathSum))
                daily.meanMultipathCentimeters = multipathSum / multipathValues;
            double dailyCycleSlips = 0.0;
            bool hasCycleSlipValue = false;
            ForEachFlatJsonNumber(ColumnText(rows.get(), 7),
                [&](const std::string_view key, const double value) {
                    if (value < 0.0 || (!cycleSlipsSystem.empty()
                        && QualitySystemToken(key) != cycleSlipsSystem)) return;
                    dailyCycleSlips += value;
                    hasCycleSlipValue = true;
                });
            if (hasCycleSlipValue && std::isfinite(dailyCycleSlips))
                daily.totalCycleSlips = dailyCycleSlips;
            aggregate.daily[ordinal] = std::move(daily);
        }
        if (status != SQLITE_DONE) {
            result.error = SqliteError(database.get(), L"读取观测质量记录失败");
            return result;
        }
    }

    std::unordered_set<std::string> duplicateFourChar;
    for (const auto& [stationKey, summary] : result.byStation) {
        (void)summary;
        if (stationKey.size() < 4) continue;
        const std::string four = stationKey.substr(0, 4);
        const auto [it, inserted] =
            result.uniqueStationByFourChar.try_emplace(four, stationKey);
        if (!inserted && it->second != stationKey) duplicateFourChar.insert(four);
    }
    for (const auto& four : duplicateFourChar) {
        result.uniqueStationByFourChar.erase(four);
    }
    return result;
}

const StationQualitySummary* FindObservationQuality(
    const ObservationQualityCatalogue& catalogue,
    const StationFile& station) {
    std::string full = UpperAscii(station.stationId);
    if (const auto found = catalogue.byStation.find(full);
        found != catalogue.byStation.end()) return &found->second;
    if (full.size() >= 9) {
        full.resize(9);
        if (const auto found = catalogue.byStation.find(full);
            found != catalogue.byStation.end()) return &found->second;
    }
    std::string four = UpperAscii(station.fourCharId);
    if (four.empty() && full.size() >= 4) four = full.substr(0, 4);
    if (four.size() > 4) four.resize(4);
    const auto alias = catalogue.uniqueStationByFourChar.find(four);
    if (alias == catalogue.uniqueStationByFourChar.end()) return nullptr;
    const auto found = catalogue.byStation.find(alias->second);
    return found == catalogue.byStation.end() ? nullptr : &found->second;
}

bool MatchesObservationQuality(
    const DailyObservationQuality& quality,
    const ObservationRangeOptions& options) {
    const auto atMost = [](const std::optional<double>& value,
                           const double configuredMaximum) {
        const double maximum = std::isfinite(configuredMaximum)
            ? std::max(0.0, configuredMaximum) : 0.0;
        return value.has_value() && std::isfinite(*value)
            && *value <= maximum + 1e-9;
    };
    if (options.observationCompletenessEnabled) {
        const double threshold = std::isfinite(
                options.minimumObservationCompletenessPercent)
            ? std::clamp(
                options.minimumObservationCompletenessPercent, 0.0, 100.0)
            : 0.0;
        if (!quality.observationCompleteness.has_value()
            || *quality.observationCompleteness + 1e-9 < threshold) return false;
    }
    if (options.latencyEnabled
        && !atMost(quality.latencyHours, options.maximumMeanLatencyHours)) return false;
    if (options.positionTimeSeriesEnabled
        && !atMost(quality.meanAbsoluteDetrendedPositionMillimeters,
                   options.maximumMeanAbsoluteDetrendedPositionMillimeters)) return false;
    if (options.residualsEnabled
        && !atMost(quality.meanAbsoluteResidualMillimeters,
                   options.maximumMeanAbsoluteResidualMillimeters)) return false;
    if (options.meanMultipathEnabled
        && !atMost(quality.meanMultipathCentimeters,
                   options.maximumMeanMultipathCentimeters)) return false;
    if (options.cycleSlipsEnabled
        && !atMost(quality.totalCycleSlips,
                   options.maximumMeanDailyCycleSlips)) return false;
    return true;
}

std::vector<BitmapBlock> ObservationQualityDays(
    const StationFile& station,
    const StationQualitySummary* quality,
    const int firstOrdinal, const int lastOrdinal,
    const ObservationRangeOptions& options) {
    const ObservationNetworkMask igsQualityMask =
        NetworkBit(ObservationNetwork::Igs) | NetworkBit(ObservationNetwork::Mgex);
    std::vector<BitmapBlock> qualified;
    if ((station.availableNetworks & igsQualityMask) == 0 || !quality) return qualified;
    for (const auto& [ordinal, daily] : quality->daily) {
        if (ordinal >= firstOrdinal && ordinal <= lastOrdinal
            && MatchesObservationQuality(daily, options)) SetOrdinal(qualified, ordinal);
    }
    SortBitmapBlocks(qualified);
    return qualified;
}

}  // namespace

std::vector<ObservationSnapshotWindow> ObservationSearchOnlineSnapshots(
    const std::filesystem::path& indexPath,
    const NetworkSelection selection, const QueryDate& endDate) {
    std::vector<ObservationSnapshotWindow> result;
    const auto index = LoadCachedIndex(indexPath, false, false);
    int end = 0;
    if (!index || !index->error.empty() || !QueryDateToOrdinal(endDate, end)) return result;
    if (index->dataCutoffDay.has_value()) return result;
    for (const auto& [network, blocks] : index->coverage) {
        if ((selection.mask & network) == 0) continue;
        int last = std::numeric_limits<int>::min();
        for (const auto& block : blocks) {
            for (int word = 0; word < kWordsPerBlock; ++word) {
                const auto bits = block.words[word];
                if (bits) last = std::max(last, block.number * kBitsPerBlock
                    + word * 64 + 63 - static_cast<int>(std::countl_zero(bits)));
            }
        }
        if (last == std::numeric_limits<int>::min() || end <= last) continue;
        const std::chrono::year_month_day day{std::chrono::sys_days{std::chrono::days{last}}};
        result.push_back({network, MakeQueryDate(int(day.year()), unsigned(day.month()), unsigned(day.day()))});
    }
    return result;
}

ObservationSearchQueryResult QueryObservationSearchIndex(
    const std::filesystem::path& indexPath,
    const QueryDate& startDate,
    const QueryDate& endDate,
    const NetworkSelection selection,
    const ObservationRangeOptions& options,
    const bool validateSourceFreshness,
    const std::stop_token stopToken,
    const bool includeDisplayMetadata) {
    ObservationSearchQueryResult result;
    int firstOrdinal = 0, lastOrdinal = 0;
    if (!QueryDateToOrdinal(startDate, firstOrdinal)
        || !QueryDateToOrdinal(endDate, lastOrdinal)
        || lastOrdinal < firstOrdinal) {
        result.error = L"日期范围无效。";
        return result;
    }
    if (!selection.Any()) {
        result.error = L"请至少选择一个站网。";
        return result;
    }
    const long long expected = static_cast<long long>(lastOrdinal)
        - static_cast<long long>(firstOrdinal) + 1;
    if (expected <= 0 || expected > std::numeric_limits<int>::max()) {
        result.error = L"日期范围过大。";
        return result;
    }
    result.expectedDays = static_cast<int>(expected);
    // Space-weather predicates are date-level constraints.  Resolve them
    // once against the local compact index, then use the resulting ordinal
    // bitmap in exactly the same way as observation availability bitmaps.
    // This keeps the query O(number of profiles + selected days) and makes
    // the denominator for completeness the filtered date count.
    const auto weather = SelectSpaceWeatherDates(
        DefaultSpaceWeatherDatabasePath(), startDate, endDate, options,
        stopToken);
    if (!weather.error.empty()) {
        result.error = weather.error;
        return result;
    }
    std::vector<BitmapBlock> weatherBlocks;
    if (weather.active) {
        weatherBlocks.reserve(weather.ordinals.size() / 4096U + 1U);
        for (const int ordinal : weather.ordinals) {
            SetOrdinal(weatherBlocks, ordinal);
        }
        result.expectedDays = weather.matchedDays;
        result.warning = weather.warning;
    }
    // GIM provider membership is resolved from the separate GIM station
    // Selection validates all requested dates, except explicit weather
    // exclusions. Never derive this calendar from stations with data.
    for (int ordinal = firstOrdinal; ordinal <= lastOrdinal; ++ordinal) {
        if (!weather.active || BlocksHaveAnyDayInRange(weatherBlocks, ordinal, ordinal))
            result.selectionEligibleOrdinals.push_back(ordinal);
    }
    // The GUI needs equipment for display even when no equipment filter is
    // active. Load it from this index so the completed rows are ready to use.
    const bool requireSiteLog = includeDisplayMetadata || AnyMetadataFilter(options);
    // Site Log Plus is only needed for system+signal-code predicates.  A
    // receiver/antenna/system-only query can use the dated Site Log records
    // without scanning the separate signal interval catalogue.  Keeping the
    // two optional portions independent avoids an unnecessary second archive
    // read on the common equipment-filter path while preserving synchronous
    // loading whenever a signal predicate is active.
    const bool requireSiteLogPlus = !options.signalCodesInclude.empty()
        || !options.signalCodesExclude.empty();
    const auto index = LoadCachedIndex(
        indexPath, requireSiteLog, requireSiteLogPlus);
    if (!index || !index->error.empty()) {
        result.error = index ? index->error : L"无法加载中央高速观测索引。";
        return result;
    }
    result.indexedNetworks = index->indexedNetworks;
    result.siteLogMetadataIncluded = index->siteLogMetadataLoaded;
    if (!index->coordinateOverrideWarning.empty()) {
        if (!result.warning.empty()) result.warning += L"；";
        result.warning += index->coordinateOverrideWarning;
    }
    const auto missing = selection.mask & ~index->indexedNetworks;
    if (missing != 0) {
        result.error = std::format(
            L"中央高速索引未覆盖所选站网掩码 0x{:X}，请调整所选站网。",
            missing);
        return result;
    }
    if (validateSourceFreshness) {
        const std::wstring freshness = FreshnessError(*index, selection.mask);
        if (!freshness.empty()) {
            result.stale = true;
            // The derived database is an immutable, transactionally built
            // snapshot.  A knowledge-base builder may legitimately keep
            // appending older days for hours; making every such commit fall
            // back to a multi-minute full scan defeats the purpose of the
            // interactive index.  Keep serving the last complete snapshot
            // and make the lag visible so the user can rebuild it when the
            // source job finishes.  We never mix partially rebuilt rows.
            if (!result.warning.empty()) result.warning += L" ";
            result.warning += freshness
                + L" 当前查询继续使用上一次完整索引；请重新安装官方知识库后重试。";
        }
    }
    ObservationNetworkMask incompleteCoverageMask = 0;
    for (unsigned bit = 0; bit < 64; ++bit) {
        const ObservationNetworkMask networkMask = std::uint64_t{1} << bit;
        if ((selection.mask & networkMask) == 0) continue;
        const auto found = index->coverage.find(networkMask);
        int checkedLast = lastOrdinal;
        for (const auto& snapshot : options.onlineSnapshots) {
            int last = 0;
            if ((snapshot.networks & networkMask) != 0
                && QueryDateToOrdinal(snapshot.lastKnownDate, last)) checkedLast = std::min(checkedLast, last);
        }
        if (checkedLast >= firstOrdinal && (found == index->coverage.end()
            || !CoverageContainsRange(found->second, firstOrdinal, checkedLast))) {
            incompleteCoverageMask |= networkMask;
        }
    }
    if (incompleteCoverageMask != 0) {
        if (!result.warning.empty()) result.warning += L"；";
        result.warning += std::format(
            L"部分所选站网（掩码0x{:X}）尚未完整扫描这一日期范围；"
            L"未收录日期按缺测保守计算，不影响其他站网的查询结果",
            incompleteCoverageMask);
    }

    const auto extendSnapshotDays = [&](std::vector<BitmapBlock> blocks,
                                        const ObservationNetworkMask networks, const bool freeze = false) {
        if (index->dataCutoffDay.has_value()) {
            return IntersectBlocks(blocks, FullRangeBlocks(
                firstOrdinal, std::min(lastOrdinal, *index->dataCutoffDay)));
        }
        for (const auto& snapshot : options.onlineSnapshots) {
            int last = 0;
            if ((snapshot.networks & networks) == 0
                || !QueryDateToOrdinal(snapshot.lastKnownDate, last)
                || lastOrdinal <= last) continue;
            const bool matched = BlocksHaveAnyDayInRange(blocks, last, last);
            if (freeze) blocks = SubtractBlocks(blocks, FullRangeBlocks(last + 1, lastOrdinal));
            if (!matched) continue;
            OrBlocks(blocks, FullRangeBlocks(std::max(firstOrdinal, last + 1), lastOrdinal),
                     firstOrdinal, lastOrdinal);
            SortBitmapBlocks(blocks);
        }
        return blocks;
    };
    const auto metadataFirst = [&]() {
        int first = firstOrdinal;
        for (const auto& snapshot : options.onlineSnapshots) {
            int last = 0;
            if (QueryDateToOrdinal(snapshot.lastKnownDate, last)) first = std::min(first, last);
        }
        return first;
    }();
    if (!options.onlineSnapshots.empty() && !index->dataCutoffDay.has_value()) {
        result.warning += L"；超出知识库日期的候选沿用最后已知日信息，完整性为预估；下载时在线确认实际文件。";
    }

    std::vector<std::string> intervals;
    if (!options.sampleIntervals.empty()) {
        intervals.reserve(options.sampleIntervals.size());
        for (const auto& value : options.sampleIntervals) {
            const auto normalized = NormalizedInterval(value);
            if (!normalized.empty()
                && std::find(intervals.begin(), intervals.end(), normalized)
                    == intervals.end()) intervals.push_back(normalized);
        }
    } else if (!options.sampleInterval.empty()) {
        intervals.push_back(NormalizedInterval(options.sampleInterval));
    }

    const GimStationFilter gimStationFilter = LoadGimStationFilter(
        startDate, endDate, options, stopToken);
    if (!gimStationFilter.warning.empty()) {
        if (!result.warning.empty()) result.warning += L"；";
        result.warning += gimStationFilter.warning;
    }

    std::vector<QueryStationAccumulator> accumulators(index->stations.size());
    const ObservationStationDateFilter frameDateFilter(options);
    if (frameDateFilter.active() && !frameDateFilter.hasEntries()) {
        if (!result.warning.empty()) result.warning += L"；";
        result.warning += L"参考框架日期范围没有可用的内置测站名册，结果按零个测站处理。";
    }
    std::vector<std::vector<BitmapBlock>> frameEligibleBlocks(
        frameDateFilter.active() ? index->stations.size() : 0U);
    if (frameDateFilter.active()) {
        for (std::size_t stationIndex = 0;
             stationIndex < index->stations.size(); ++stationIndex) {
            for (const auto& range : frameDateFilter.Ranges(
                    index->stations[stationIndex], firstOrdinal, lastOrdinal)) {
                OrBlocks(
                    frameEligibleBlocks[stationIndex],
                    FullRangeBlocks(range.first, range.second),
                    firstOrdinal,
                    lastOrdinal);
            }
            SortBitmapBlocks(frameEligibleBlocks[stationIndex]);
        }
    }
    std::vector<std::size_t> touched;
    touched.reserve(index->stations.size());
    const PreparedMetadataFilter metadataFilter = PrepareMetadataFilter(options);
    const bool signalFilterRequested = metadataFilter.HasSignalRestriction();
    const ObservationNetworkMask signalNetworkMask =
        NetworkBit(ObservationNetwork::Igs)
        | NetworkBit(ObservationNetwork::Mgex);
    std::size_t profileOrdinal = 0;
    for (const auto& profile : index->profiles) {
        if ((profileOrdinal++ & 0x3ffU) == 0U && stopToken.stop_requested()) {
            result.error = L"查询已取消。";
            return result;
        }
        if (!ProfileMatches(profile, selection, options, intervals)) {
            continue;
        }
        // Site Log Plus currently describes the shared IGS/MGEX archive.  A
        // frequency-qualified query must therefore use only that archive's
        // availability, instead of accidentally attributing IGS capabilities
        // to an unrelated regional source sharing the same station code.
        const auto selectedProfileNetworks = profile.networkMask & selection.mask
            & (signalFilterRequested
                ? signalNetworkMask
                : std::numeric_limits<ObservationNetworkMask>::max());
        if (selectedProfileNetworks == 0) continue;
        const auto projectedBlocks = extendSnapshotDays(profile.blocks, selectedProfileNetworks);
        const auto profileBlocks = weather.active
            ? IntersectBlocks(projectedBlocks, weatherBlocks)
            : projectedBlocks;
        auto eligibleProfileBlocks = profileBlocks;
        if (signalFilterRequested) {
            // Qualify the actual file family before station aggregation.
            // A RINEX3 BDS code must never authorize a RINEX2 file on that day.
            eligibleProfileBlocks = IntersectBlocks(eligibleProfileBlocks,
                SignalDaysForStation(*index, profile.stationIndex, metadataFilter,
                    firstOrdinal, lastOrdinal, profile.rinexFormat == 2 ? 2 : 34));
            eligibleProfileBlocks = IntersectBlocks(eligibleProfileBlocks,
                SignalFileDaysForStation(*index, profile.stationIndex, firstOrdinal,
                    lastOrdinal, profile.rinexFormat == 2 ? 2 : 34, profile.interval));
        }
        if (frameDateFilter.active()) {
            if (profile.stationIndex >= frameEligibleBlocks.size()
                || frameEligibleBlocks[profile.stationIndex].empty()) continue;
            eligibleProfileBlocks = IntersectBlocks(
                eligibleProfileBlocks,
                frameEligibleBlocks[profile.stationIndex]);
        }
        if (!BlocksHaveAnyDayInRange(
            eligibleProfileBlocks, firstOrdinal, lastOrdinal)) continue;
        auto& accumulator = accumulators[profile.stationIndex];
        if (!accumulator.touched) {
            accumulator.touched = true;
            touched.push_back(profile.stationIndex);
            accumulator.representativeFormat = profile.rinexFormat;
            accumulator.representativeInterval = profile.interval;
        } else if (profile.rinexFormat == 3) {
            accumulator.representativeFormat = 3;
            accumulator.representativeInterval = profile.interval;
        }
        accumulator.availableFormats = static_cast<RinexFormatMask>(
            accumulator.availableFormats
            | RinexFormatBit(profile.rinexFormat == 2
                ? RinexFormat::Rinex2
                : RinexFormat::Rinex34));
        accumulator.networks |= selectedProfileNetworks;
        OrBlocks(accumulator.blocks, eligibleProfileBlocks, firstOrdinal, lastOrdinal);
        for (unsigned bitIndex = 0; bitIndex < 64; ++bitIndex) {
            const ObservationNetworkMask bit = std::uint64_t{1} << bitIndex;
            if ((selectedProfileNetworks & bit) == 0) continue;
            auto found = std::find_if(
                accumulator.byNetwork.begin(), accumulator.byNetwork.end(),
                [&](const auto& item) { return item.bit == bit; });
            if (found == accumulator.byNetwork.end()) {
                accumulator.byNetwork.push_back({bit, {}});
                found = std::prev(accumulator.byNetwork.end());
            }
            OrBlocks(found->blocks, eligibleProfileBlocks, firstOrdinal, lastOrdinal);
        }
    }
    for (const auto stationIndex : touched) {
        SortBitmapBlocks(accumulators[stationIndex].blocks);
        for (auto& item : accumulators[stationIndex].byNetwork) {
            SortBitmapBlocks(item.blocks);
        }
    }

    const bool filterSiteLogMetadata = metadataFilter.HasSiteLogRestriction();
    if (metadataFilter.HasSignalRestriction()) {
        if (metadataFilter.invalidSignalInput) {
            if (!result.warning.empty()) result.warning += L"；";
            result.warning += L"频点条件格式无效；请使用‘GPS:C1C’、‘BDS:C2I’这类‘系统:观测码’写法。"
                L"无效条件按不匹配处理。";
        }
        if (!index->siteLogPlusAvailable) {
            if (!result.warning.empty()) result.warning += L"；";
            result.warning += index->siteLogPlusWarning.empty()
                ? L"频点筛选需要可用的 Site Log Plus 频点知识库；当前没有可匹配数据。"
                : index->siteLogPlusWarning;
        }
        const ObservationNetworkMask igsMask =
            NetworkBit(ObservationNetwork::Igs)
            | NetworkBit(ObservationNetwork::Mgex);
        if ((selection.mask & ~igsMask) != 0) {
            if (!result.warning.empty()) result.warning += L"；";
            result.warning += L"频点筛选目前只对 IGS/MGEX 的 Site Log Plus 记录生效；"
                L"其他站网没有可验证的系统+观测码区间。";
        }
    }
    std::vector<BitmapBlock> dailyCoverageBlocks;
    if (options.dailyCoverageMode != DailyCoverageMode::Ignore) {
        const int stationCount = static_cast<int>(std::count_if(
            touched.begin(), touched.end(),
            [&](const std::size_t stationIndex) {
                return stationIndex < index->stations.size()
                    && gimStationFilter.Matches(index->stations[stationIndex]);
            }));
        const double requestedPercent = std::isfinite(options.minimumDailyStationPercent)
            ? std::clamp(options.minimumDailyStationPercent, 0.0, 100.0) : 0.0;
        const int requestedStations = std::max(0, options.minimumDailyStations);
        for (int ordinal = firstOrdinal;; ++ordinal) {
            int availableStations = 0;
            for (const auto stationIndex : touched) {
                if (stationIndex >= index->stations.size()
                    || !gimStationFilter.Matches(index->stations[stationIndex])) {
                    continue;
                }
                if (BlocksHaveAnyDayInRange(
                        accumulators[stationIndex].blocks, ordinal, ordinal)) {
                    ++availableStations;
                }
            }
            bool keep = false;
            if (options.dailyCoverageMode == DailyCoverageMode::StationsAtLeast) {
                keep = availableStations >= requestedStations;
            } else {
                const double percent = stationCount <= 0 ? 0.0
                    : 100.0 * static_cast<double>(availableStations)
                        / static_cast<double>(stationCount);
                keep = percent + 1e-9 >= requestedPercent;
            }
            if (keep) SetOrdinal(dailyCoverageBlocks, ordinal);
            if (ordinal == lastOrdinal) break;
        }
        SortBitmapBlocks(dailyCoverageBlocks);
        result.expectedDays = static_cast<int>(CountBits(dailyCoverageBlocks));
        if (result.expectedDays == 0) {
            if (!result.warning.empty()) result.warning += L"；";
            result.warning += L"每日覆盖完整性条件未匹配任何日期。";
        }
    }
    std::optional<ObservationQualityCatalogue> qualityCatalogue;
    if (HasObservationQualityFilter(options)) {
        const ObservationNetworkMask igsQualityMask =
            NetworkBit(ObservationNetwork::Igs)
            | NetworkBit(ObservationNetwork::Mgex);
        std::vector<std::string> qualityStationKeys;
        qualityStationKeys.reserve(touched.size());
        for (const auto stationIndex : touched) {
            if ((accumulators[stationIndex].networks & igsQualityMask) == 0) {
                continue;
            }
            std::string key = UpperAscii(index->stations[stationIndex].stationId);
            if (key.size() >= 9) key.resize(9);
            if (key.empty()) {
                key = UpperAscii(index->stations[stationIndex].fourCharId);
            }
            if (!key.empty()) qualityStationKeys.push_back(std::move(key));
        }
        qualityCatalogue.emplace(LoadObservationQualityCatalogue(
            ResolveDatabaseFilePath(
                indexPath.parent_path() / L"gnss_availability.sqlite"),
            startDate,
            endDate,
            qualityStationKeys,
            weather.active,
            weatherBlocks,
            options.dailyCoverageMode != DailyCoverageMode::Ignore,
            dailyCoverageBlocks,
            options,
            stopToken));
        if (!qualityCatalogue->error.empty()) {
            result.error = L"观测质量筛选失败：" + qualityCatalogue->error;
            return result;
        }
        // A latest-day snapshot cannot supply quality evidence for later dates.
        if ((selection.mask & ~igsQualityMask) != 0) {
            if (!result.warning.empty()) result.warning += L"；";
            result.warning += L"观测质量记录来自共享的 IGS/MGEX 日 QC 知识库；"
                L"启用质量条件后，没有可匹配 QC 记录的其他站网测站会被排除";
        }
    }
    const double percentThreshold = std::isfinite(options.minimumCompletenessPercent)
        ? std::clamp(options.minimumCompletenessPercent, 0.0, 100.0) : 0.0;
    const int daysThreshold = std::max(0, options.minimumAvailableDays);
    std::size_t qualityAffected = 0;
    result.stations.reserve(touched.size());
    for (const auto stationIndex : touched) {
        if (stationIndex >= index->stations.size()
            || !gimStationFilter.Matches(index->stations[stationIndex])) {
            continue;
        }
        const auto& accumulator = accumulators[stationIndex];
        std::vector<BitmapBlock> accepted = accumulator.blocks;
        if (filterSiteLogMetadata) {
            accepted.clear();
            std::vector<BitmapBlock> claimed;
            if (stationIndex < index->siteRecordsByStation.size()) {
                for (const auto recordIndex :
                     index->siteRecordsByStation[stationIndex]) {
                    const auto& record = index->siteRecords[recordIndex];
                    const auto applicable = record.applicabilityMask
                        & selection.mask;
                    if (applicable == 0) continue;
                    auto recordAvailability = AvailabilityForMask(
                        accumulator, applicable, firstOrdinal, lastOrdinal);
                    auto unclaimed = SubtractBlocks(recordAvailability, claimed);
                    if (unclaimed.empty()) continue;
                    const auto metadataDays = extendSnapshotDays(MetadataDaysForRecord(
                        record, metadataFilter, metadataFirst, lastOrdinal), applicable, true);
                    const auto matches = IntersectBlocks(unclaimed, metadataDays);
                    OrBlocks(accepted, matches, firstOrdinal, lastOrdinal);
                    OrBlocks(claimed, unclaimed, firstOrdinal, lastOrdinal);
                    SortBitmapBlocks(claimed);
                }
            }
        }
        SortBitmapBlocks(accepted);
        // Signal predicates were applied to individual file-family profiles.
        if (options.dailyCoverageMode != DailyCoverageMode::Ignore) {
            accepted = IntersectBlocks(
                accepted, dailyCoverageBlocks);
        }
        if (qualityCatalogue.has_value()) {
            auto qualityStation = index->stations[stationIndex];
            qualityStation.availableNetworks = accumulator.networks;
            const auto qualified = ObservationQualityDays(
                qualityStation,
                FindObservationQuality(*qualityCatalogue, qualityStation),
                firstOrdinal, lastOrdinal, options);
            const auto beforeQuality = CountBits(accepted);
            accepted = IntersectBlocks(accepted, qualified);
            if (CountBits(accepted) < beforeQuality) ++qualityAffected;
        }
        const auto available = static_cast<int>(CountBits(accepted));
        if (available <= 0) continue;
        int stationExpectedDays = result.expectedDays;
        if (frameDateFilter.active()) {
            auto eligibleDays = frameEligibleBlocks[stationIndex];
            if (weather.active) {
                eligibleDays = IntersectBlocks(eligibleDays, weatherBlocks);
            }
            stationExpectedDays = static_cast<int>(CountBits(eligibleDays));
        }
        if (options.dailyCoverageMode != DailyCoverageMode::Ignore) {
            stationExpectedDays = static_cast<int>(CountBits(
                IntersectBlocks(
                    frameDateFilter.active()
                        ? frameEligibleBlocks[stationIndex]
                        : FullRangeBlocks(firstOrdinal, lastOrdinal),
                    dailyCoverageBlocks)));
        }
        if (stationExpectedDays <= 0) continue;
        const double completeness = 100.0 * static_cast<double>(available)
            / static_cast<double>(stationExpectedDays);
        if (options.completenessMode == CompletenessMode::RatioAtLeast
            && completeness + 1e-9 < percentThreshold) continue;
        if (options.completenessMode == CompletenessMode::DaysAtLeast
            && available < daysThreshold) continue;
        StationFile station = index->stations[stationIndex];
        station.availableNetworks = accumulator.networks;
        station.format = accumulator.representativeFormat == 2
            ? RinexFormat::Rinex2 : RinexFormat::Rinex34;
        station.availableRinexFormats = accumulator.availableFormats;
        station.interval = accumulator.representativeInterval;
        station.availableDays = available;
        station.expectedDays = stationExpectedDays;
        station.completenessPercent = completeness;
        std::vector<BitmapBlock> unverified;
        for (const auto& snapshot : options.onlineSnapshots) {
            int last = 0;
            if ((snapshot.networks & accumulator.networks) == 0
                || !QueryDateToOrdinal(snapshot.lastKnownDate, last)) continue;
            OrBlocks(unverified, accepted, std::max(firstOrdinal, last + 1), lastOrdinal);
        }
        station.unverifiedDays = static_cast<int>(CountBits(unverified));
        auto coverage = std::make_shared<CalendarCoverage>();
        coverage->firstOrdinal = firstOrdinal;
        coverage->lastOrdinal = lastOrdinal;
        const auto copyBlocks = [](const auto& input, auto& output) {
            for (const auto& block : input)
                output.push_back({block.number * kBitsPerBlock,
                    {block.words.begin(), block.words.end()}});
        };
        copyBlocks(accepted, coverage->available);
        copyBlocks(unverified, coverage->unverified);
        coverage->restrictEligibleDates = weather.active || frameDateFilter.active()
            || options.dailyCoverageMode != DailyCoverageMode::Ignore;
        if (coverage->restrictEligibleDates) {
            auto eligible = frameDateFilter.active() ? frameEligibleBlocks[stationIndex]
                : FullRangeBlocks(firstOrdinal, lastOrdinal);
            if (weather.active) eligible = IntersectBlocks(eligible, weatherBlocks);
            if (options.dailyCoverageMode != DailyCoverageMode::Ignore)
                eligible = IntersectBlocks(eligible, dailyCoverageBlocks);
            copyBlocks(eligible, coverage->eligible);
        }
        station.calendarCoverage = std::move(coverage);

        station.selected = false;
        station.inSelectionScope = true;
        if (const auto representativeDay = FirstSetOrdinal(accepted);
            representativeDay.has_value()
            && stationIndex < index->siteRecordsByStation.size()) {
            for (const auto recordIndex : index->siteRecordsByStation[stationIndex]) {
                const auto& record = index->siteRecords[recordIndex];
                const auto applicable = record.applicabilityMask & selection.mask;
                if (applicable == 0) continue;
                const auto recordAvailability = AvailabilityForMask(
                    accumulator, applicable, *representativeDay, *representativeDay);
                if (CountBits(recordAvailability) == 0) continue;
                int metadataDay = *representativeDay;
                for (const auto& snapshot : options.onlineSnapshots) {
                    int last = 0;
                    if ((snapshot.networks & applicable) != 0
                        && QueryDateToOrdinal(snapshot.lastKnownDate, last))
                        metadataDay = std::min(metadataDay, last);
                }
                ApplyCachedSiteRecord(station, record, metadataDay);
                break;
            }
        }
        // fileName/downloadUrl/downloadRoutes intentionally remain empty.
        result.stations.push_back(std::move(station));
    }
    if (qualityCatalogue.has_value()) {
        if (!result.warning.empty()) result.warning += L"；";
        result.warning += std::format(
            L"已逐日检查 IGS Network 质量记录，"
            L"{} 个测站的合格日期因质量条件或缺少记录而减少",
            qualityAffected);
    }
    std::sort(result.stations.begin(), result.stations.end(),
        [](const StationFile& left, const StationFile& right) {
            const std::string& leftId = left.stationId.empty()
                ? left.fourCharId : left.stationId;
            const std::string& rightId = right.stationId.empty()
                ? right.fourCharId : right.stationId;
            return leftId < rightId;
        });
    result.complete = true;
    if (result.stations.empty()) {
        if (!result.warning.empty()) result.warning += L"；";
        result.warning += L"日期范围内没有测站满足当前筛选条件。";
    }
    return result;
}

ObservationSearchMetadataChoices QueryObservationSearchMetadataChoices(
    const std::filesystem::path& indexPath,
    const QueryDate& startDate,
    const QueryDate& endDate,
    const NetworkSelection selection,
    const bool validateSourceFreshness) {
    ObservationSearchMetadataChoices result;
    int firstOrdinal = 0, lastOrdinal = 0;
    if (!QueryDateToOrdinal(startDate, firstOrdinal)
        || !QueryDateToOrdinal(endDate, lastOrdinal)
        || lastOrdinal < firstOrdinal) {
        result.error = L"日期范围无效。";
        return result;
    }
    if (!selection.Any()) {
        result.error = L"请至少选择一个站网。";
        return result;
    }
    const auto index = LoadCachedIndex(indexPath, true, true);
    if (!index || !index->error.empty()) {
        result.error = index ? index->error : L"无法加载中央高速观测索引。";
        return result;
    }
    const auto missing = selection.mask & ~index->indexedNetworks;
    if (missing != 0) {
        result.error = std::format(
            L"中央高速索引未覆盖所选站网掩码 0x{:X}。", missing);
        return result;
    }
    if (validateSourceFreshness) {
        // The immutable snapshot remains internally consistent while a source
        // builder is running.  Keep option enumeration usable; the range query
        // reports staleness as a visible warning using the same policy.
        (void)FreshnessError(*index, selection.mask);
    }

    // Keep advanced-filter choices available for wholly out-of-index dates.
    for (const auto& snapshot : ObservationSearchOnlineSnapshots(indexPath, selection, endDate)) {
        int last = 0;
        if (QueryDateToOrdinal(snapshot.lastKnownDate, last)) firstOrdinal = std::min(firstOrdinal, last);
    }

    std::map<std::string, std::string> systems;
    std::map<std::string, std::string> receivers;
    std::map<std::string, std::string> antennas;
    std::map<std::string, std::string> signals;
    const auto remember = [](std::map<std::string, std::string>& values,
                             const std::string& display) {
        if (display.empty()) return;
        std::string key = display;
        std::transform(key.begin(), key.end(), key.begin(),
            [](const unsigned char value) {
                return static_cast<char>(std::toupper(value));
            });
        values.try_emplace(std::move(key), display);
    };
    for (const auto& record : index->siteRecords) {
        if ((record.applicabilityMask & selection.mask) == 0
            || record.stationIndex >= index->stations.size()
            || (index->stations[record.stationIndex].availableNetworks
                & selection.mask) == 0) continue;
        for (const auto& segment : record.segments) {
            if (segment.lastDayExclusive <= firstOrdinal
                || segment.firstDay > lastOrdinal) continue;
            if (segment.kind == 0) {
                remember(receivers, segment.type);
                for (const auto& token :
                    CanonicalSatelliteSystemTokensInternal(segment.systems)) {
                    remember(systems, token);
                }
            } else if (segment.kind == 1) {
                remember(antennas, segment.type);
            }
        }
    }

    const auto append = [](const std::map<std::string, std::string>& source,
                           std::vector<std::string>& destination) {
        destination.reserve(source.size());
        for (const auto& [key, display] : source) {
            (void)key;
            destination.push_back(display);
        }
    };
    append(systems, result.systems);
    append(receivers, result.receivers);
    append(antennas, result.antennas);
    const ObservationNetworkMask igsMask =
        NetworkBit(ObservationNetwork::Igs)
        | NetworkBit(ObservationNetwork::Mgex);
    if ((selection.mask & igsMask) != 0) {
        for (std::size_t stationIndex = 0;
             stationIndex < index->signalIntervalsByStation.size();
             ++stationIndex) {
            if (stationIndex >= index->stations.size()
                || (index->stations[stationIndex].availableNetworks
                    & selection.mask & igsMask) == 0) continue;
            for (const auto& interval : index->signalIntervalsByStation[stationIndex]) {
                if (interval.lastDay < firstOrdinal
                    || interval.firstDay > lastOrdinal
                    || interval.system == "?") continue;
                const std::string value = interval.system + ":" + interval.code;
                remember(signals, value);
            }
        }
    }
    append(signals, result.signals);
    return result;
}

ObservationSearchDatedFilterResult FilterObservationSearchDatedFiles(
    const std::filesystem::path& indexPath,
    const NetworkSelection selection,
    const ObservationRangeOptions& options,
    std::vector<DatedStationFile>& files,
    const bool validateSourceFreshness,
    const std::stop_token stopToken,
    ObservationSearchDatedFilterProgressCallback progress) {
    ObservationSearchDatedFilterResult result;
    result.inputFiles = files.size();
    if (!selection.Any()) {
        result.error = L"请至少选择一个站网。";
        return result;
    }
    const bool requireSiteLog = AnyMetadataFilter(options);
    const bool requireSiteLogPlus = !options.signalCodesInclude.empty()
        || !options.signalCodesExclude.empty();
    const auto index = LoadCachedIndex(
        indexPath, requireSiteLog, requireSiteLogPlus);
    if (!index || !index->error.empty()) {
        result.error = index ? index->error : L"无法加载中央高速观测索引。";
        return result;
    }
    const auto missing = selection.mask & ~index->indexedNetworks;
    if (missing != 0) {
        result.error = std::format(
            L"中央高速索引未覆盖所选站网掩码 0x{:X}。", missing);
        return result;
    }
    if (validateSourceFreshness) {
        const auto freshness = FreshnessError(*index, selection.mask);
        if (!freshness.empty()) {
            result.stale = true;
            result.warning = freshness
                + L" 当前下载清单按上一次完整高速快照复核。";
        }
    }

    SpaceWeatherDateSelection weather;
    if (HasSpaceWeatherFilter(options) && !files.empty()) {
        QueryDate firstDate{}, lastDate{};
        int firstOrdinal = std::numeric_limits<int>::max();
        int lastOrdinal = std::numeric_limits<int>::min();
        for (const auto& file : files) {
            int ordinal = 0;
            if (!QueryDateToOrdinal(file.date, ordinal)) continue;
            if (ordinal < firstOrdinal) {
                firstOrdinal = ordinal;
                firstDate = file.date;
            }
            if (ordinal > lastOrdinal) {
                lastOrdinal = ordinal;
                lastDate = file.date;
            }
        }
        if (firstOrdinal <= lastOrdinal) {
            weather = SelectSpaceWeatherDates(
                DefaultSpaceWeatherDatabasePath(), firstDate, lastDate,
                options, stopToken);
            if (!weather.error.empty()) {
                result.error = weather.error;
                return result;
            }
            if (!weather.warning.empty()) {
                if (!result.warning.empty()) result.warning += L"；";
                result.warning += weather.warning;
            }
        }
    }

    GimStationFilter gimStationFilter;
    if (options.gimStationMode != "ignore" && !files.empty()) {
        QueryDate firstDate{};
        QueryDate lastDate{};
        int firstOrdinal = std::numeric_limits<int>::max();
        int lastOrdinal = std::numeric_limits<int>::min();
        for (const auto& file : files) {
            int ordinal = 0;
            if (!QueryDateToOrdinal(file.date, ordinal)) continue;
            if (ordinal < firstOrdinal) {
                firstOrdinal = ordinal;
                firstDate = file.date;
            }
            if (ordinal > lastOrdinal) {
                lastOrdinal = ordinal;
                lastDate = file.date;
            }
        }
        if (firstOrdinal <= lastOrdinal) {
            gimStationFilter = LoadGimStationFilter(
                firstDate, lastDate, options, stopToken);
            if (!gimStationFilter.warning.empty()) {
                if (!result.warning.empty()) result.warning += L"；";
                result.warning += gimStationFilter.warning;
            }
        }
    }

    std::optional<ObservationQualityCatalogue> qualityCatalogue;
    if (HasObservationQualityFilter(options) && !files.empty()) {
        QueryDate firstDate{}, lastDate{};
        int firstOrdinal = std::numeric_limits<int>::max();
        int lastOrdinal = std::numeric_limits<int>::min();
        std::vector<std::string> stationKeys;
        for (const auto& file : files) {
            int ordinal = 0;
            if (!QueryDateToOrdinal(file.date, ordinal)) continue;
            if (ordinal < firstOrdinal) { firstOrdinal = ordinal; firstDate = file.date; }
            if (ordinal > lastOrdinal) { lastOrdinal = ordinal; lastDate = file.date; }
            const auto stationIndex = FindCachedStationIndex(
                *index, file.station, selection.mask);
            if (!stationIndex.has_value()) continue;
            auto key = UpperAscii(index->stations[*stationIndex].stationId);
            if (key.size() >= 9) key.resize(9);
            if (key.empty()) key = UpperAscii(index->stations[*stationIndex].fourCharId);
            if (!key.empty()) stationKeys.push_back(std::move(key));
        }
        if (firstOrdinal <= lastOrdinal) {
            qualityCatalogue.emplace(LoadObservationQualityCatalogue(
                ResolveDatabaseFilePath(indexPath.parent_path() / L"gnss_availability.sqlite"),
                firstDate, lastDate, std::move(stationKeys), false, {}, false, {}, options, stopToken));
            if (!qualityCatalogue->error.empty()) {
                result.error = qualityCatalogue->error;
                return result;
            }
        }
    }

    const bool restrictMetadata = AnyMetadataFilter(options);
    const auto filter = PrepareMetadataFilter(options);
    const bool restrictSiteLogMetadata = filter.HasSiteLogRestriction();
    const bool restrictSignals = filter.HasSignalRestriction();
    if (filter.invalidSignalInput) {
        result.warning = L"频点条件格式无效；请使用‘GPS:C1C’、‘BDS:C2I’这类‘系统:观测码’写法。"
            L"无效条件按不匹配处理。";
    }
    if (restrictSignals && !index->siteLogPlusAvailable) {
        if (!result.warning.empty()) result.warning += L"；";
        result.warning += index->siteLogPlusWarning.empty()
            ? L"频点筛选需要可用的 Site Log Plus 频点知识库；相关文件将被排除。"
            : index->siteLogPlusWarning;
    } else if (restrictSignals && !index->siteLogPlusWarning.empty()) {
        // Keep a readable diagnostic for a present-but-partially-matchable
        // Plus database (for example after a station-key migration).  The
        // filter remains fail-closed for dates without an explicit capability
        // interval, but the user should not have to infer that from an empty
        // download list.
        result.warning = index->siteLogPlusWarning;
    }
    std::vector<DatedStationFile> retained;
    retained.reserve(files.size());
    if (progress) progress(0, files.size());
    for (std::size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
        if ((fileIndex & 0x3ffU) == 0U && stopToken.stop_requested()) {
            result.error = L"下载清单 Site Log 复核已取消。";
            return result;
        }
        if (progress && (fileIndex & 0x1ffU) == 0U) {
            progress(fileIndex, files.size());
        }
        auto& dated = files[fileIndex];
        int ordinal = 0;
        if (!QueryDateToOrdinal(dated.date, ordinal)) continue;
        if (!gimStationFilter.Matches(dated.station)) continue;
        if (weather.active && !weather.Contains(ordinal)) continue;
        const ObservationNetworkMask dayNetworks =
            dated.station.availableNetworks & selection.mask;
        const ObservationNetworkMask signalNetworkMask =
            NetworkBit(ObservationNetwork::Igs)
            | NetworkBit(ObservationNetwork::Mgex);
        const ObservationNetworkMask effectiveSignalNetworks =
            dayNetworks == 0 ? selection.mask : dayNetworks;
        if (restrictSignals
            && (effectiveSignalNetworks & signalNetworkMask) == 0) {
            // The active Site Log Plus database covers IGS/MGEX only; do not
            // reuse its capabilities for a regional-only file row.
            continue;
        }
        const int signalObservationOrdinal = ordinal;
        for (const auto& snapshot : options.onlineSnapshots) {
            int last = 0;
            if ((snapshot.networks & effectiveSignalNetworks) != 0
                && QueryDateToOrdinal(snapshot.lastKnownDate, last)) ordinal = std::min(ordinal, last);
        }
        const auto stationIndex = FindCachedStationIndex(
            *index, dated.station,
            dayNetworks == 0 ? selection.mask : dayNetworks);
        if (!stationIndex.has_value()) {
            // A signal-qualified row cannot be proven without a Site Log Plus
            // station identity.  Rows with no metadata restrictions retain
            // the historical permissive behaviour.
            if (!restrictMetadata && !qualityCatalogue.has_value())
                retained.push_back(std::move(dated));
            continue;
        }
        if (qualityCatalogue.has_value()) {
            auto qualityStation = index->stations[*stationIndex];
            qualityStation.availableNetworks = effectiveSignalNetworks;
            const auto* quality = FindObservationQuality(*qualityCatalogue, qualityStation);
            if (!quality) continue;
            const auto day = quality->daily.find(signalObservationOrdinal);
            if (day == quality->daily.end()
                || (qualityStation.availableNetworks & signalNetworkMask) == 0
                || !MatchesObservationQuality(day->second, options)) continue;
        }
        if (restrictSignals
            && !SignalMatchesDay(*index, *stationIndex, filter, signalObservationOrdinal,
                dated.station.format == RinexFormat::Rinex2 ? 2 : 34,
                dated.station.interval)) {
            continue;
        }
        if (restrictSignals && index->signalFileAvailabilityRestricted) {
            const auto& name = dated.station.fileName;
            // Daily header evidence does not establish codes in an hourly or
            // 15-minute high-rate segment, even on the same station/date.
            if (name.size() > 27 && name[9] == '_' && name[11] == '_'
                && name[23] == '_' && name.substr(24, 3) != "01D") continue;
            if (name.size() > 8 && name[8] == '.' && name[7] != '0') continue;
        }
        if (!restrictSiteLogMetadata) {
            retained.push_back(std::move(dated));
            continue;
        }
        if (*stationIndex >= index->siteRecordsByStation.size()) continue;
        ObservationNetworkMask effectiveDayNetworks = dayNetworks;
        if (effectiveDayNetworks == 0) effectiveDayNetworks = selection.mask;
        const CachedSiteRecord* claimed = nullptr;
        for (const auto recordIndex : index->siteRecordsByStation[*stationIndex]) {
            const auto& candidate = index->siteRecords[recordIndex];
            if ((candidate.applicabilityMask & effectiveDayNetworks) == 0) continue;
            claimed = &candidate;
            break;
        }
        if (claimed == nullptr) {
            continue;
        }
        ApplyCachedSiteRecord(dated.station, *claimed, ordinal);
        if (!restrictMetadata
            || MetadataRecordMatchesDay(*claimed, filter, ordinal)) {
            retained.push_back(std::move(dated));
        }
    }
    files = std::move(retained);
    if (progress) progress(result.inputFiles, result.inputFiles);
    result.retainedFiles = files.size();
    result.complete = true;
    return result;
}

ObservationStationDetail LoadObservationStationDetail(
    const std::filesystem::path& path, const StationFile& station,
    const QueryDate& start, const QueryDate& end) {
    ObservationStationDetail result;
    result.station = station;
    int queryFirst = 0, queryLast = 0;
    if (!QueryDateToOrdinal(start, queryFirst) || !QueryDateToOrdinal(end, queryLast)) return result;
    const auto index = LoadCachedIndex(path, true, true);
    if (!index || !index->error.empty()) {
        result.error = index ? index->error : L"无法读取测站详情";
        return result;
    }
    const auto identity = FindCachedStationIndex(*index, station);
    if (!identity) return result;

    // Some historical importers stored UTF-8 bytes as literal \xHH text.
    // Decode only that syntax, and retain the original if it is not valid UTF-8.
    const auto clean = [](const std::string& value) {
        std::string decoded;
        const auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (std::size_t i = 0; i < value.size();) {
            std::size_t marker = i;
            while (marker < value.size() && value[marker] == '\\') ++marker;
            if (marker > i && marker + 2 < value.size() && value[marker] == 'x'
                && hex(value[marker + 1]) >= 0 && hex(value[marker + 2]) >= 0) {
                const auto byte = static_cast<char>(hex(value[marker + 1]) * 16 + hex(value[marker + 2]));
                if (byte == '\0') return value;
                decoded.push_back(byte); i = marker + 3;
            } else decoded.push_back(value[i++]);
        }
        if (!decoded.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            decoded.data(), static_cast<int>(decoded.size()), nullptr, 0) == 0) return value;
        return decoded;
    };

    // History spans the station's indexed archive, independently of the current query.
    int first = std::numeric_limits<int>::max(), last = std::numeric_limits<int>::min();
    for (const auto& profile : index->profiles) {
        if (profile.stationIndex != *identity) continue;
        if (station.availableNetworks && !(profile.networkMask & station.availableNetworks)) continue;
        if (const auto begin = FirstSetOrdinal(profile.blocks)) first = std::min(first, *begin);
        for (auto block = profile.blocks.rbegin(); block != profile.blocks.rend(); ++block) {
            bool found = false;
            for (int word = kWordsPerBlock - 1; word >= 0; --word) {
                const auto bits = block->words[static_cast<std::size_t>(word)];
                if (!bits) continue;
                last = std::max(last, block->number * kBitsPerBlock + word * 64
                    + 63 - static_cast<int>(std::countl_zero(bits)));
                found = true; break;
            }
            if (found) break;
        }
    }
    if (first > last) { first = queryFirst; last = queryLast; }
    // Signal evidence has its own interval bounds; its snapshot date must
    // not truncate equipment history from newer indexed observation files.
    if (first > last) return result;

    const CachedSiteRecord* selected = nullptr;
    if (*identity < index->siteRecordsByStation.size()) {
        for (const auto recordId : index->siteRecordsByStation[*identity]) {
            const auto& record = index->siteRecords[recordId];
            if (station.availableNetworks && !(record.applicabilityMask & station.availableNetworks)) continue;
            selected = &record; break;
        }
    }
    std::vector<ObservationEquipmentEpoch> equipment;
    if (selected) {
        ApplyCachedSiteRecord(result.station, *selected, queryFirst);
        for (const auto& item : selected->segments) {
            auto model = clean(item.type);
            const auto radome = clean(item.radome);
            // Site Logs sometimes append the separately recorded radome to antenna type.
            if (item.kind == 1 && !radome.empty() && model.size() > radome.size()
                && model.ends_with(radome)
                && std::isspace(static_cast<unsigned char>(model[model.size() - radome.size() - 1]))) {
                model.resize(model.size() - radome.size());
                while (!model.empty() && std::isspace(static_cast<unsigned char>(model.back()))) model.pop_back();
            }
            equipment.push_back({item.kind, item.firstDay, item.lastDayExclusive, model,
                clean(item.serial), clean(item.firmware), radome, clean(item.referencePoint)});
        }
    }
    auto& s = result.station;
    s.siteLogName = clean(s.siteLogName); s.siteLogDomes = clean(s.siteLogDomes);
    s.siteLogCity = clean(s.siteLogCity); s.siteLogCountry = clean(s.siteLogCountry);
    s.siteLogAgency = clean(s.siteLogAgency); s.siteLogOperator = clean(s.siteLogOperator);
    // Historical logs annotate a DOMES ID or put spaces/hyphens inside it.
    // Normalize only a verified identifier shape; never fabricate one.
    const std::regex domesPattern(R"((\d{5})[\s-]*([MS])[\s-]*(\d{3}))", std::regex::icase);
    std::smatch domesMatch;
    if (std::regex_search(s.siteLogDomes, domesMatch, domesPattern)) {
        s.siteLogDomes = domesMatch[1].str()+domesMatch[2].str()+domesMatch[3].str();
        std::ranges::transform(s.siteLogDomes,s.siteLogDomes.begin(),
            [](unsigned char c){return static_cast<char>(std::toupper(c));});
    }

    std::vector<ObservationCodeEpoch> codes;
    if (*identity < index->signalIntervalsByStation.size()) {
        for (const auto& item : index->signalIntervalsByStation[*identity])
            if (item.system != "?") codes.push_back({item.firstDay, item.lastDay + 1, item.system + ":" + item.code});
    }
    result.periods = BuildObservationEquipmentHistory(equipment, codes, first, last);
    return result;
}

ObservationSearchIndexStatus GetObservationSearchIndexStatus(
    const std::filesystem::path& indexPath,
    const bool validateSourceFreshness,
    const bool includeDisplayMetadata) {
    ObservationSearchIndexStatus result;
    std::error_code filesystemError;
    result.exists = std::filesystem::is_regular_file(indexPath, filesystemError);
    if (!result.exists) {
        result.error = L"中央高速观测索引不存在：" + indexPath.wstring();
        return result;
    }
    const auto index = LoadCachedIndex(indexPath, includeDisplayMetadata, false);
    if (!index || !index->error.empty()) {
        result.error = index ? index->error : L"无法加载中央高速观测索引。";
        return result;
    }
    result.compatible = true;
    result.indexedNetworks = index->indexedNetworks;
    result.unifiedStations = index->stations.size();
    result.profiles = index->profiles.size();
    for (const auto& profile : index->profiles) {
        result.bitmapBlocks += profile.blocks.size();
    }
    if (validateSourceFreshness) {
        result.error = FreshnessError(*index, index->indexedNetworks);
        result.stale = !result.error.empty();
    }
    return result;
}
