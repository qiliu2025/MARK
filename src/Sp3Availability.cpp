#include "Sp3Availability.h"
#include "AttAvailability.h"
#include "ClkAvailability.h"

#include "DateUtil.h"
#include "ArchiveLegacyNames.h"
#include "VerifiedLegacyOrbitHeaders.h"
#include "HttpClient.h"
#include "IgsAvailabilityDb.h"

#include "sqlite3.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <limits>
#include <regex>
#include <set>
#include <string_view>
#include <unordered_map>

namespace {

constexpr int kSourceCount = 7;
std::mutex gProductDatabaseMutex;

enum class ProductKind {
    Sp3,
    Clk,
    Att,
};

struct SourceDefinition {
    const char* key;
    int priority;
    const wchar_t* root;
    bool ftp;
};

constexpr SourceDefinition kSources[] {
    {"CDDIS", 1, L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/products/", true},
    {"BKG", 2, L"https://igs.bkg.bund.de/root_ftp/IGS/products/", false},
    {"WHU", 3, L"ftp://igs.gnsswhu.cn/pub/gps/products/", true},
    {"KASI", 4, L"ftp://nfs.kasi.re.kr/gps/products/", true},
    {"IGN", 5, L"ftp://igs.ign.fr/pub/igs/products/", true},
    {"ESA", 6, L"ftp://gssc.esa.int/gnss/products/", true},
    {"SOPAC", 7, L"http://garner.ucsd.edu/pub/products/", false},
};

struct DbCloser { void operator()(sqlite3* value) const { if (value) sqlite3_close(value); } };
struct StmtCloser { void operator()(sqlite3_stmt* value) const { if (value) sqlite3_finalize(value); } };
using DbPtr = std::unique_ptr<sqlite3, DbCloser>;
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtCloser>;

// The interactive product pane must not rebuild a product/date/route graph
// on every click.  Keep a compact process-wide summary cache per product
// database.  It contains only metadata and the logical key for each date;
// physical URLs are hydrated lazily when a download is actually started.
struct ProductFamilyDayRef {
    std::chrono::sys_days day{};
    QueryDate date{};
    std::string logicalKey;
};

struct ProductFamilyCacheEntry {
    std::string familyKey;
    Sp3ProductRecord representative;
    std::vector<ProductFamilyDayRef> days;
};

struct ProductFileStamp {
    std::uintmax_t size = 0;
    std::uintmax_t walSize = 0;
    std::filesystem::file_time_type modified{};
    std::filesystem::file_time_type walModified{};
    bool exists = false;
    bool walExists = false;
    bool operator==(const ProductFileStamp&) const = default;
};

struct ProductFamilyCache {
    std::filesystem::path databasePath;
    ProductFileStamp stamp;
    std::vector<ProductFamilyCacheEntry> families;
    std::unordered_map<std::string, std::size_t> familyIndex;
    std::wstring error;
};

std::wstring Utf8ToWide(const std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), size);
    return result;
}

std::string WideToUtf8(const std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), size, nullptr, nullptr);
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

std::string Lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string Upper(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

std::chrono::sys_days ToSysDays(const QueryDate& date) {
    return std::chrono::sys_days{
        std::chrono::year{date.year}
        / std::chrono::month{static_cast<unsigned>(date.month)}
        / std::chrono::day{static_cast<unsigned>(date.day)}};
}

QueryDate FromSysDays(const std::chrono::sys_days value) {
    const std::chrono::year_month_day date{value};
    return MakeQueryDate(static_cast<int>(date.year()),
        static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()));
}

std::string DateText(const QueryDate& date) {
    return std::format("{:04}-{:02}-{:02}", date.year, date.month, date.day);
}

bool SameDate(const QueryDate& left, const QueryDate& right) {
    return left.year == right.year && left.month == right.month && left.day == right.day;
}

bool MissingDirectory(const HttpTextResult& result) {
    // libcurl code 9 is CURLE_REMOTE_ACCESS_DENIED. Public FTP archives use
    // it for an absent optional directory (the same server reply commonly
    // appears as FTP 550), so it is an archive boundary, not a proxy outage.
    if (result.statusCode == 404 || result.statusCode == 410
        || result.transportErrorCode == 9) return true;
    const std::string message = Lower(WideToUtf8(result.error));
    return message.find("404") != std::string::npos
        || message.find("410") != std::string::npos
        || message.find("550") != std::string::npos
        || message.find("not found") != std::string::npos
        || message.find("no such file") != std::string::npos;
}

bool IsTransportFailure(const long code) {
    return code == 5 || code == 6 || code == 7 || code == 28 || code == 35
        || code == 52 || code == 55 || code == 56;
}

std::wstring JoinUrl(const std::wstring& directory, const std::string& file) {
    std::wstring result = directory;
    if (!result.ends_with(L'/')) result.push_back(L'/');
    result += Utf8ToWide(file);
    return result;
}

std::string ConstellationFor(const std::string& series) {
    const std::string upper = Upper(series);
    if (upper.starts_with("WUM") || upper.starts_with("IAC")) return "GRECJ";
    if (upper.starts_with("COD0MGX") || upper.starts_with("GFZ0MGX")) return "GRECJ";
    if (upper.starts_with("JAX")) return "GREJ";
    if (upper.starts_with("SHA")) return "GREC";
    if (upper.starts_with("HUS")) return "GREC";
    if (upper.starts_with("IGS2") || upper.starts_with("IGS3")) return "GREC";
    if (upper.starts_with("COD") || upper.starts_with("ESA")
        || upper.starts_with("GFZ") || upper.starts_with("JGX")) return "GRE";
    if (upper.starts_with("EMR") || upper.starts_with("GRG")) return "GR";
    if (upper.starts_with("JPL") || upper.starts_with("MIT")) return "GE";
    return "G";
}

std::string AttConstellationFor(const std::string& series) {
    const std::string upper = Upper(series);
    if (upper.starts_with("IGS2R03")) return "GRE";
    if (upper.starts_with("IGS0DEM") || upper.starts_with("WCC")) return "GRE";
    if (upper.starts_with("COD0MGX") || upper.starts_with("GFZ0MGX")
        || upper.starts_with("WUM")) return "GRECJ";
    if (upper.starts_with("GRG0MGX") || upper.starts_with("HUS")
        || upper.starts_with("SHA")) return "GREC";
    if (upper.starts_with("JAX")) return "GREJ";
    if (upper.starts_with("COD") || upper.starts_with("GRG")
        || upper.starts_with("JGX")) return "GRE";
    if (upper.starts_with("JPL")) return "GE";
    if (upper.starts_with("EMR")) return "G";
    return "MULTI";
}

std::string LatencyFor(const std::string& project, const std::string& solution) {
    const std::string p = Upper(project);
    const std::string s = Upper(solution);
    if (p.starts_with("R0")) return "REPROCESSING";
    if (s == "FIN") return "FINAL";
    if (s == "RAP") return "RAPID";
    // WHU's historical ULA stream contains the observed ultra-rapid arc.
    if (s == "ULT" || s == "ULA") return "ULTRA_RAPID";
    if (s == "RTS") return "REAL_TIME";
    if (s == "NRT") return "NEAR_REAL_TIME";
    return "OTHER";
}

struct LegacyDefinition {
    std::string series;
    std::string center;
    std::string project;
    std::string solution;
};

LegacyDefinition Legacy(const std::string& prefix) {
    static const std::unordered_map<std::string, LegacyDefinition> values{
        {"igs", {"IGS0OPSFIN", "IGS", "OPS", "FIN"}},
        {"igr", {"IGS0OPSRAP", "IGS", "OPS", "RAP"}},
        {"igu", {"IGS0OPSULT", "IGS", "OPS", "ULT"}},
        {"igl", {"IGS0OPSRAP", "IGS", "OPS", "RAP"}},
        {"igv", {"IGS0OPSULT", "IGS", "OPS", "ULT"}},
        {"igc", {"IGS2OPSRTS", "IGS", "OPS", "RTS"}},
        {"cod", {"COD0OPSFIN", "COD", "OPS", "FIN"}},
        {"utx", {"UTX0OPSFIN", "CSR", "OPS", "FIN"}},
        {"emr", {"EMR0OPSFIN", "EMR", "OPS", "FIN"}},
        {"esa", {"ESA0OPSFIN", "ESA", "OPS", "FIN"}},
        {"gfz", {"GFZ0OPSFIN", "GFZ", "OPS", "FIN"}},
        {"grg", {"GRG0OPSFIN", "GRG", "OPS", "FIN"}},
        {"jpl", {"JPL0OPSFIN", "JPL", "OPS", "FIN"}},
        {"mit", {"MIT0OPSFIN", "MIT", "OPS", "FIN"}},
        {"ngs", {"NGS0OPSFIN", "NGS", "OPS", "FIN"}},
        {"sio", {"SIO0OPSFIN", "SIO", "OPS", "FIN"}},
        {"usn", {"USN0OPSFIN", "USN", "OPS", "FIN"}},
        {"com", {"COD0MGXFIN", "COD", "MGX", "FIN"}},
        {"gbm", {"GFZ0MGXFIN", "GFZ", "MGX", "FIN"}},
        {"grm", {"GRG0MGXFIN", "GRG", "MGX", "FIN"}},
        {"wum", {"WUM0MGXFIN", "WUM", "MGX", "FIN"}},
        {"qzf", {"JAX0MGXFIN", "JAX", "MGX", "FIN"}},
        {"tum", {"TUM0MGXFIN", "TUM", "MGX", "FIN"}},
    };
    const auto found = values.find(prefix);
    if (found != values.end()) return found->second;
    const std::string center = Upper(prefix);
    return {center + "0LEGACY", center, "LEG", "UNK"};
}

int CoveredOrbitDays(const std::string& duration, const std::string& solution) {
    if (solution != "FIN" || duration.size() != 3 || duration[2] != 'D') return 1;
    if (!std::isdigit(static_cast<unsigned char>(duration[0])) || !std::isdigit(static_cast<unsigned char>(duration[1]))) return 1;
    return std::clamp(std::stoi(duration.substr(0,2)),1,7);
}

std::string LogicalKey(const Sp3ArchiveFile& file) {
    return file.series + "|" + DateText(file.nominalDate) + "|"
        + std::format("{:02}{:02}|{}|{}", file.startHour, file.startMinute,
            file.duration, file.interval);
}

bool ParseLongName(const std::string& name, const ProductKind kind,
                   Sp3ArchiveFile& file) {
    static const std::regex sp3Pattern(
        R"(^([A-Z0-9]{10})_([0-9]{4})([0-9]{3})([0-9]{2})([0-9]{2})_([A-Z0-9]{3})_([A-Z0-9]{3})_ORB\.(?:SP3|EPH)(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    static const std::regex clkPattern(
        R"(^([A-Z0-9]{10})_([0-9]{4})([0-9]{3})([0-9]{2})([0-9]{2})_([A-Z0-9]{3})_([A-Z0-9]{3})_CLK\.CLK(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    static const std::regex attPattern(
        R"(^([A-Z0-9]{10})_([0-9]{4})([0-9]{3})([0-9]{2})([0-9]{2})_([A-Z0-9]{3})_([A-Z0-9]{3})_ATT\.OBX(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    std::smatch match;
    const std::regex& pattern = kind == ProductKind::Clk ? clkPattern
        : kind == ProductKind::Att ? attPattern : sp3Pattern;
    if (!std::regex_match(name, match, pattern)) return false;
    const int year = std::stoi(match[2].str());
    const int doy = std::stoi(match[3].str());
    if (year < 1980 || doy < 1 || doy > (IsLeapYear(year) ? 366 : 365)) return false;
    file.nominalDate = FromSysDays(std::chrono::sys_days{
        std::chrono::year{year} / std::chrono::January / 1} + std::chrono::days{doy - 1});
    file.startHour = std::stoi(match[4].str());
    file.startMinute = std::stoi(match[5].str());
    file.series = Upper(match[1].str());
    file.center = file.series.substr(0, 3);
    file.project = file.series.substr(4, 3);
    file.solution = file.series.substr(7, 3);
    file.duration = Upper(match[6].str());
    file.interval = Upper(match[7].str());
    file.latency = LatencyFor(file.project, file.solution);
    file.constellation = kind == ProductKind::Att
        ? AttConstellationFor(file.series) : ConstellationFor(file.series);
    file.logicalKey = LogicalKey(file);
    return true;
}

bool ParseLegacyAttName(const std::string& name, const int week,
                        const std::string& archiveClass, Sp3ArchiveFile& file) {
    static const std::regex pattern(R"(^([A-Z0-9]{3})([0-9]{4})([0-6])\.OBX(?:\.(?:GZ|Z))?$)",std::regex::icase);
    std::smatch match;
    if (!std::regex_match(name,match,pattern) || std::stoi(match[2].str())!=week) return false;
    const auto legacy=Legacy(Lower(match[1].str()));
    file.nominalDate=FromSysDays(ToSysDays(GpsWeekStartDate(week))+std::chrono::days{std::stoi(match[3].str())});
    file.series=legacy.series; file.center=legacy.center;
    file.project=legacy.project; file.solution=legacy.solution;
    if (const auto project=ArchiveReprocessingProject(archiveClass); !project.empty()) {
        file.project=project; file.center=ArchiveReprocessingCenter(Upper(match[1].str()));
        file.solution="FIN"; file.series=Upper(match[1].str())+"0"+project+"FIN";
    }
    file.duration="01D"; file.interval="30S";
    file.latency=LatencyFor(file.project,file.solution);
    file.constellation=AttConstellationFor(ArchiveReprocessingProject(archiveClass).empty()?file.series:file.center+"0OPSFIN");
    file.logicalKey=LogicalKey(file);
    return true;
}

bool ParseLegacyName(const std::string& name, const int expectedGpsWeek,
                     const bool clock, const std::string& archiveClass, Sp3ArchiveFile& file) {
    static const std::regex sp3Pattern(
        R"(^([A-Z0-9]{3})([0-9]{4})([0-7])(?:_([0-9]{2}))?\.(?:SP3|EPH)(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    static const std::regex clkPattern(
        R"(^([A-Z0-9]{3})([0-9]{4})([0-6])(?:_([0-9]{2}))?(?:_V[23])?\.CLK(?:_?([0-9]{2})([SM])?)?(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(name, match, clock ? clkPattern : sp3Pattern)) return false;
    const int week = std::stoi(match[2].str());
    if (week != expectedGpsWeek) return false;
    const int day = std::stoi(match[3].str());
    const auto definition = Legacy(Lower(match[1].str()));
    file.nominalDate = FromSysDays(ToSysDays(GpsWeekStartDate(week)) + std::chrono::days{day == 7 ? 0 : day});
    file.startHour = match[4].matched ? std::stoi(match[4].str()) : 0;
    file.startMinute = 0;
    file.series = definition.series;
    file.center = definition.center;
    file.project = definition.project;
    file.solution = definition.solution;
    file.duration = day == 7 ? "07D" : definition.solution == "ULT" || definition.solution == "NRT"
        ? "02D" : "01D";
    if (clock) {
        if (match.size() > 5 && match[5].matched) {
            file.interval = Upper(match[5].str())
                + (match.size() > 6 && match[6].matched
                    ? Upper(match[6].str()) : "S");
        } else {
            file.interval = Lower(match[1].str()) == "igc" ? "10S" : "05M";
        }
    } else {
        file.interval = day == 7 ? "00U" : "15M";
    }
    if (const auto project = ArchiveReprocessingProject(archiveClass); !project.empty()) {
        file.project = project;
        file.center = ArchiveReprocessingCenter(Upper(match[1].str()));
        file.solution = "FIN";
        file.series = Upper(match[1].str()) + "0" + project + "FIN";
    }
    file.latency = LatencyFor(file.project, file.solution);
    file.constellation = ConstellationFor(ArchiveReprocessingProject(archiveClass).empty() ? file.series : file.center + "0OPSFIN");
    if (!clock && !ApplyVerifiedLegacyOrbitHeader(name,file)) return false;
    file.logicalKey = LogicalKey(file);
    return true;
}

std::wstring DbError(sqlite3* db, const wchar_t* action) {
    const char* raw = db ? sqlite3_errmsg(db) : "unknown SQLite error";
    return std::wstring(action) + L"：" + Utf8ToWide(raw ? raw : "");
}

bool Execute(sqlite3* db, const char* sql, std::wstring& error) {
    char* message = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &message) == SQLITE_OK) return true;
    error = L"SQLite：" + Utf8ToWide(message ? message : sqlite3_errmsg(db));
    sqlite3_free(message);
    return false;
}

StmtPtr Prepare(sqlite3* db, const char* sql, std::wstring& error) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) {
        error = DbError(db, L"无法准备 SP3 知识库查询");
        return {};
    }
    return StmtPtr(raw);
}

bool Bind(sqlite3_stmt* statement, int index, const std::string& value) {
    return sqlite3_bind_text(statement, index, value.c_str(),
        static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

std::string ColumnText(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char*>(value) : "";
}

bool OpenDatabase(const std::filesystem::path& path, DbPtr& db, std::wstring& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = L"无法创建 SP3 知识库目录：" + path.parent_path().wstring();
        return false;
    }
    sqlite3* raw = nullptr;
    const int status = sqlite3_open16(path.c_str(), &raw);
    db.reset(raw);
    if (status != SQLITE_OK || !raw) {
        error = DbError(raw, L"无法打开 SP3 知识库");
        return false;
    }
    sqlite3_busy_timeout(raw, 10000);
    const char* schema =
        "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS source_definitions(source_id INTEGER PRIMARY KEY,source_key TEXT NOT NULL UNIQUE,source_priority INTEGER NOT NULL UNIQUE);"
        "CREATE TABLE IF NOT EXISTS weekly_scans(gps_week INTEGER PRIMARY KEY,status TEXT NOT NULL CHECK(status IN('complete','partial')),source_success_count INTEGER NOT NULL,product_count INTEGER NOT NULL,is_provisional INTEGER NOT NULL,scanned_at_utc TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS weekly_source_scans(gps_week INTEGER NOT NULL,source_id INTEGER NOT NULL,status TEXT NOT NULL CHECK(status IN('complete','failed')),listing_urls TEXT NOT NULL,file_count INTEGER NOT NULL,error_message TEXT NOT NULL,PRIMARY KEY(gps_week,source_id),FOREIGN KEY(gps_week) REFERENCES weekly_scans(gps_week) ON DELETE CASCADE,FOREIGN KEY(source_id) REFERENCES source_definitions(source_id));"
        "CREATE TABLE IF NOT EXISTS products(logical_key TEXT PRIMARY KEY,gps_week INTEGER NOT NULL,nominal_date TEXT NOT NULL,start_hour INTEGER NOT NULL,start_minute INTEGER NOT NULL,series TEXT NOT NULL,center TEXT NOT NULL,project TEXT NOT NULL,solution TEXT NOT NULL,latency TEXT NOT NULL,duration TEXT NOT NULL,sample_interval TEXT NOT NULL,constellation TEXT NOT NULL,FOREIGN KEY(gps_week) REFERENCES weekly_scans(gps_week) ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS products_date ON products(nominal_date,latency,center);"
        "CREATE TABLE IF NOT EXISTS product_sources(logical_key TEXT NOT NULL,source_id INTEGER NOT NULL,file_name TEXT NOT NULL,download_url TEXT NOT NULL,archive_class TEXT NOT NULL,PRIMARY KEY(logical_key,source_id,file_name,download_url),FOREIGN KEY(logical_key) REFERENCES products(logical_key) ON DELETE CASCADE,FOREIGN KEY(source_id) REFERENCES source_definitions(source_id));"
        "INSERT OR IGNORE INTO source_definitions VALUES(1,'CDDIS',1),(2,'BKG',2),(3,'WHU',3),(4,'KASI',4),(5,'IGN',5),(6,'ESA',6),(7,'SOPAC',7);"
        "INSERT INTO metadata VALUES('schema_version','1') ON CONFLICT(key) DO UPDATE SET value='1';";
    return Execute(raw, schema, error);
}

// Interactive range queries are strictly read-only.  Opening a multi-GB
// product index through OpenDatabase would execute the complete builder
// schema (including WAL/CREATE statements) for every requested day.  Keep a
// lightweight reader path for lookups so the schema is never touched.
bool OpenReadDatabase(const std::filesystem::path& path, DbPtr& db,
                      std::wstring& error) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    sqlite3* raw = nullptr;
    const auto utf8PathValue = path.u8string();
    const std::string utf8Path(
        reinterpret_cast<const char*>(utf8PathValue.data()),
        utf8PathValue.size());
    const int status = sqlite3_open_v2(
        utf8Path.c_str(), &raw,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    db.reset(raw);
    if (status != SQLITE_OK || raw == nullptr) {
        error = DbError(raw, L"无法以只读方式打开精密产品知识库");
        return false;
    }
    sqlite3_busy_timeout(raw, 10000);
    return true;
}

ProductFileStamp GetProductFileStamp(const std::filesystem::path& path) {
    ProductFileStamp stamp;
    std::error_code ec;
    stamp.exists = std::filesystem::is_regular_file(path, ec);
    if (stamp.exists) {
        ec.clear();
        stamp.size = std::filesystem::file_size(path, ec);
        ec.clear();
        stamp.modified = std::filesystem::last_write_time(path, ec);
    }
    const std::filesystem::path wal(path.wstring() + L"-wal");
    ec.clear();
    stamp.walExists = std::filesystem::is_regular_file(wal, ec);
    if (stamp.walExists) {
        ec.clear();
        stamp.walSize = std::filesystem::file_size(wal, ec);
        ec.clear();
        stamp.walModified = std::filesystem::last_write_time(wal, ec);
    }
    return stamp;
}

std::string ProductFamilyKey(const Sp3ProductRecord& record) {
    // This deliberately matches ProductLogicalKey in the GUI: product IDs
    // differ by date in some archives, while the stable fields below identify
    // the logical product family across the requested period.  Include the
    // project/solution dimensions as well: some archive scans contain the
    // same series and sampling level under more than one product line, and
    // collapsing those rows would copy the representative metadata onto the
    // wrong day's route during lazy hydration.
    return record.series + "\x1f" + record.center + "\x1f"
        + record.project + "\x1f" + record.solution + "\x1f"
        + record.constellation + "\x1f" + record.interval + "\x1f"
        + record.latency + "\x1f" + record.duration + "\x1f"
        + std::to_string(record.startHour) + ":"
        + std::to_string(record.startMinute);
}

bool ParseDateText(const std::string& text, QueryDate& date) {
    int year = 0, month = 0, day = 0;
    if (sscanf_s(text.c_str(), "%d-%d-%d", &year, &month, &day) != 3) {
        return false;
    }
    date = MakeQueryDate(year, month, day);
    return date.dayOfYear > 0;
}

Sp3ProductRecord ProductRecordFromStatement(sqlite3_stmt* statement) {
    Sp3ProductRecord record;
    record.logicalKey = ColumnText(statement, 0);
    ParseDateText(ColumnText(statement, 1), record.nominalDate);
    record.startHour = sqlite3_column_int(statement, 2);
    record.startMinute = sqlite3_column_int(statement, 3);
    record.series = ColumnText(statement, 4);
    record.center = ColumnText(statement, 5);
    record.project = ColumnText(statement, 6);
    record.solution = ColumnText(statement, 7);
    record.latency = ColumnText(statement, 8);
    record.duration = ColumnText(statement, 9);
    record.interval = ColumnText(statement, 10);
    record.constellation = ColumnText(statement, 11);
    return record;
}

// Read only the product rows needed by one range (and, for route hydration,
// one family).  The former implementation populated a process-wide cache by
// scanning every row in `products` while holding a mutex.  That was tolerable
// for a small SQLite test database, but it made a fallback query over the
// multi-million-row DuckDB compatibility database allocate a complete
// product/date graph before the UI could show its first result.  The product
// table has a nominal_date index, so keeping the range predicate in SQL gives
// us the same family/date representation without materialising unrelated
// years.
std::shared_ptr<ProductFamilyCache> LoadProductFamilyCache(
    const std::filesystem::path& databasePath,
    const QueryDate& startDate,
    const QueryDate& endDate,
    const std::string* requestedFamily = nullptr) {
    const auto absolute = std::filesystem::absolute(databasePath);
    const ProductFileStamp currentStamp = GetProductFileStamp(absolute);

    auto cache = std::make_shared<ProductFamilyCache>();
    cache->databasePath = absolute;
    cache->stamp = currentStamp;
    if (!currentStamp.exists) {
        cache->error = L"精密产品知识库文件不存在：" + absolute.wstring();
        return cache;
    }
    DbPtr db;
    if (!OpenReadDatabase(absolute, db, cache->error)) {
        return cache;
    }

    // ProductFamilyKey has nine fields separated by ASCII US: the eight
    // stable product dimensions followed by HH:MM.  Supplying these fields
    // to SQL lets the date/latency/center indexes discard almost all rows for
    // a route hydration.  If an old database contains a malformed key, keep
    // the safe date-only query and let the family-key comparison below decide
    // whether a row belongs to the requested family.
    std::array<std::string, 9> dimensions{};
    bool hasFamilyPredicate = false;
    if (requestedFamily != nullptr) {
        std::size_t begin = 0;
        int field = 0;
        while (field < static_cast<int>(dimensions.size())) {
            const auto end = requestedFamily->find('\x1f', begin);
            if (end == std::string::npos) {
                dimensions[static_cast<std::size_t>(field++)]
                    = requestedFamily->substr(begin);
                break;
            }
            dimensions[static_cast<std::size_t>(field++)]
                = requestedFamily->substr(begin, end - begin);
            begin = end + 1;
        }
        hasFamilyPredicate = field == static_cast<int>(dimensions.size())
            && requestedFamily->find('\x1f', begin) == std::string::npos;
        if (hasFamilyPredicate) {
            const auto colon = dimensions[8].find(':');
            int hour = -1;
            int minute = -1;
            if (colon != std::string::npos) {
                try {
                    hour = std::stoi(dimensions[8].substr(0, colon));
                    minute = std::stoi(dimensions[8].substr(colon + 1));
                } catch (...) {
                    hasFamilyPredicate = false;
                }
            } else {
                hasFamilyPredicate = false;
            }
            if (hasFamilyPredicate && (hour < 0 || hour > 23
                    || minute < 0 || minute > 59)) {
                hasFamilyPredicate = false;
            }
        }
    }

    std::string sql =
        "SELECT logical_key,nominal_date,start_hour,start_minute,series,"
        "center,project,solution,latency,duration,sample_interval,"
        "constellation FROM products WHERE nominal_date BETWEEN ?1 AND ?2";
    if (hasFamilyPredicate) {
        sql += " AND series=?3 AND center=?4 AND project=?5 AND solution=?6"
            " AND constellation=?7 AND sample_interval=?8 AND latency=?9"
            " AND duration=?10 AND start_hour=?11 AND start_minute=?12";
    }
    sql += ";";
    auto statement = Prepare(db.get(), sql.c_str(), cache->error);
    if (!statement) {
        return cache;
    }
    const std::string firstText = DateText(FromSysDays(ToSysDays(startDate) - std::chrono::days{6}));
    const std::string lastText = DateText(endDate);
    if (!Bind(statement.get(), 1, firstText)
        || !Bind(statement.get(), 2, lastText)) {
        cache->error = L"绑定精密产品日期范围失败";
        return cache;
    }
    if (hasFamilyPredicate) {
        for (int index = 0; index < 8; ++index) {
            if (!Bind(statement.get(), index + 3,
                      dimensions[static_cast<std::size_t>(index)])) {
                cache->error = L"绑定精密产品族筛选条件失败";
                return cache;
            }
        }
        const auto colon = dimensions[8].find(':');
        const int hour = std::stoi(dimensions[8].substr(0, colon));
        const int minute = std::stoi(dimensions[8].substr(colon + 1));
        if (sqlite3_bind_int(statement.get(), 11, hour) != SQLITE_OK
            || sqlite3_bind_int(statement.get(), 12, minute) != SQLITE_OK) {
            cache->error = L"绑定精密产品族时间条件失败";
            return cache;
        }
    }
    while (true) {
        const int status = sqlite3_step(statement.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            cache->error = DbError(db.get(), L"读取精密产品高速索引失败");
            cache->families.clear();
            cache->familyIndex.clear();
            return cache;
        }
        Sp3ProductRecord record = ProductRecordFromStatement(statement.get());
        if (record.logicalKey.empty() || record.nominalDate.dayOfYear <= 0) {
            continue;
        }
        // The date-only fallback above is intentionally conservative for old
        // or malformed family keys.  Never return a neighbouring family.
        if (requestedFamily != nullptr
            && ProductFamilyKey(record) != *requestedFamily) {
            continue;
        }
        const std::string familyKey = ProductFamilyKey(record);
        auto [found, inserted] = cache->familyIndex.emplace(
            familyKey, cache->families.size());
        if (inserted) {
            ProductFamilyCacheEntry family;
            family.familyKey = familyKey;
            family.representative = record;
            family.representative.routes.clear();
            cache->families.push_back(std::move(family));
        }
        auto& family = cache->families[found->second];
        const auto nominal = ToSysDays(record.nominalDate);
        for (int offset = 0; offset < CoveredOrbitDays(record.duration,record.solution); ++offset) {
            const auto day = nominal + std::chrono::days{offset};
            if (day >= ToSysDays(startDate) && day <= ToSysDays(endDate))
                family.days.push_back({day, FromSysDays(day), record.logicalKey});
        }
    }
    for (auto& family : cache->families) {
        std::ranges::sort(family.days,
            [](const ProductFamilyDayRef& left,
               const ProductFamilyDayRef& right) {
                return left.day < right.day;
            });
    }
    // If a builder committed while the snapshot was being read, retain this
    // coherent snapshot but force a refresh on the next query.
    if (!(GetProductFileStamp(absolute) == currentStamp)) {
        cache->stamp = {};
    }
    return cache;
}

const ProductFamilyCacheEntry* FindFamily(
    const ProductFamilyCache& cache, const std::string& familyKey) {
    const auto found = cache.familyIndex.find(familyKey);
    if (found == cache.familyIndex.end() || found->second >= cache.families.size()) {
        return nullptr;
    }
    return &cache.families[found->second];
}

bool ValidProductRange(
    const QueryDate& startDate, const QueryDate& endDate,
    int& expectedDays, std::chrono::sys_days& first,
    std::chrono::sys_days& last, std::wstring& error) {
    first = ToSysDays(startDate);
    last = ToSysDays(endDate);
    const auto span = (last - first).count();
    if (span < 0 || span > static_cast<long long>(
            std::numeric_limits<int>::max() - 1)) {
        error = L"精密产品日期范围无效";
        return false;
    }
    expectedDays = static_cast<int>(span) + 1;
    return true;
}

Sp3ProductFamilyRangeLookup LoadProductFamilyRange(
    const std::filesystem::path& databasePath,
    const QueryDate& startDate,
    const QueryDate& endDate) {
    std::scoped_lock lock(gProductDatabaseMutex);
    Sp3ProductFamilyRangeLookup result;
    result.databasePath = databasePath;
    std::chrono::sys_days first, last;
    if (!ValidProductRange(startDate, endDate, result.expectedDays,
                           first, last, result.error)) {
        return result;
    }
    const auto cache = LoadProductFamilyCache(
        databasePath, startDate, endDate);
    if (!cache || !cache->error.empty()) {
        result.error = cache && !cache->error.empty()
            ? cache->error : L"无法加载精密产品高速索引";
        return result;
    }
    if (!cache->stamp.exists || cache->stamp.size == 0) {
        result.error = L"精密产品知识库文件不存在";
        return result;
    }

    DbPtr db;
    if (!OpenReadDatabase(databasePath, db, result.error)) return result;
    const int firstWeek = CalculateGpsWeek(startDate);
    const int lastWeek = CalculateGpsWeek(endDate);
    if (firstWeek < 0 || lastWeek < firstWeek) {
        result.error = L"精密产品日期范围早于 GPS 周起点";
        return result;
    }
    std::unordered_map<int, bool> indexedWeeks;
    std::unordered_map<int, bool> completeWeeks;
    auto scans = Prepare(db.get(),
        "SELECT gps_week,status,is_provisional FROM weekly_scans "
        "WHERE gps_week BETWEEN ?1 AND ?2;", result.error);
    if (!scans) return result;
    sqlite3_bind_int(scans.get(), 1, firstWeek);
    sqlite3_bind_int(scans.get(), 2, lastWeek);
    while (true) {
        const int status = sqlite3_step(scans.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            result.error = DbError(db.get(), L"读取精密产品日期索引失败");
            return result;
        }
        const int week = sqlite3_column_int(scans.get(), 0);
        indexedWeeks[week] = true;
        completeWeeks[week] = ColumnText(scans.get(), 1) == "complete"
            && sqlite3_column_int(scans.get(), 2) == 0;
    }
    for (int offset = 0; offset < result.expectedDays; ++offset) {
        const QueryDate day = FromSysDays(first + std::chrono::days{offset});
        const int week = CalculateGpsWeek(day);
        if (indexedWeeks.contains(week)) ++result.indexedDays;
        if (!indexedWeeks.contains(week) || !completeWeeks[week]) {
            result.incompleteDates.push_back(day);
        }
    }
    result.complete = result.incompleteDates.empty();

    for (const auto& family : cache->families) {
        const auto begin = std::lower_bound(family.days.begin(), family.days.end(),
            first, [](const ProductFamilyDayRef& ref,
                      const std::chrono::sys_days value) {
                return ref.day < value;
            });
        const auto end = std::upper_bound(family.days.begin(), family.days.end(),
            last, [](const std::chrono::sys_days value,
                     const ProductFamilyDayRef& ref) {
                return value < ref.day;
            });
        if (begin == end) continue;
        Sp3ProductFamilyRecord record;
        record.familyKey = family.familyKey;
        record.representative = family.representative;
        record.representative.nominalDate = begin->date;
        record.representative.routes.clear();
        // A malformed/legacy index can contain more than one logical key for
        // the same family/date.  Report dates, rather than rows, so the
        // compact summary cannot overstate coverage and downstream planners
        // can use the list as a precise lazy-hydration hint.
        for (auto it = begin; it != end; ++it) {
            if (!record.availableDates.empty()
                && record.availableDates.back().year == it->date.year
                && record.availableDates.back().month == it->date.month
                && record.availableDates.back().day == it->date.day) {
                continue;
            }
            record.availableDates.push_back(it->date);
        }
        record.availableDays = static_cast<int>(record.availableDates.size());
        result.families.push_back(std::move(record));
    }
    std::ranges::sort(result.families,
        [](const Sp3ProductFamilyRecord& left,
           const Sp3ProductFamilyRecord& right) {
            const auto& a = left.representative;
            const auto& b = right.representative;
            if (a.latency != b.latency) return a.latency < b.latency;
            if (a.center != b.center) return a.center < b.center;
            if (a.series != b.series) return a.series < b.series;
            if (a.startHour != b.startHour) return a.startHour < b.startHour;
            return a.logicalKey < b.logicalKey;
        });
    return result;
}

Sp3ProductFamilyRoutesLookup LoadProductFamilyRoutes(
    const std::filesystem::path& databasePath,
    const std::string& familyKey,
    const QueryDate& startDate,
    const QueryDate& endDate,
    const std::stop_token stopToken,
    const Sp3ProductRouteProgress& progress) {
    std::scoped_lock lock(gProductDatabaseMutex);
    Sp3ProductFamilyRoutesLookup result;
    result.databasePath = databasePath;
    std::chrono::sys_days first, last;
    int expectedDays = 0;
    if (!ValidProductRange(startDate, endDate, expectedDays, first, last,
                           result.error)) {
        return result;
    }
    const auto cache = LoadProductFamilyCache(
        databasePath, startDate, endDate, &familyKey);
    if (!cache || !cache->error.empty()) {
        result.error = cache && !cache->error.empty()
            ? cache->error : L"无法加载精密产品高速索引";
        return result;
    }
    const auto* family = FindFamily(*cache, familyKey);
    if (family == nullptr) {
        result.error = L"高速产品索引中找不到所选产品族";
        return result;
    }
    DbPtr db;
    if (!OpenReadDatabase(databasePath, db, result.error)) return result;
    const auto begin = std::lower_bound(family->days.begin(), family->days.end(),
        first, [](const ProductFamilyDayRef& ref,
                  const std::chrono::sys_days value) {
            return ref.day < value;
        });
    const auto end = std::upper_bound(family->days.begin(), family->days.end(),
        last, [](const std::chrono::sys_days value,
                 const ProductFamilyDayRef& ref) {
            return value < ref.day;
        });
    const auto total = static_cast<std::size_t>(end - begin);
    result.records.reserve(total);
    if (progress) progress(0, total);
    std::unordered_map<std::string, std::vector<std::size_t>> recordsByKey;
    std::vector<std::string> keys;
    keys.reserve(total);
    for (auto it = begin; it != end; ++it) {
        if (stopToken.stop_requested()) {
            result.error = L"精密产品下载路线读取已取消";
            result.records.clear();
            return result;
        }
        Sp3ProductRecord record = family->representative;
        record.nominalDate = it->date;
        record.logicalKey = it->logicalKey;
        const auto recordIndex = result.records.size();
        const auto [found, inserted] = recordsByKey.emplace(
            record.logicalKey, std::vector<std::size_t>{});
        if (inserted) {
            keys.push_back(record.logicalKey);
        }
        found->second.push_back(recordIndex);
        result.records.push_back({record.nominalDate, std::move(record)});
    }

    // A bounded IN-list keeps this fallback path fast with both SQLite and
    // DuckDB.  The latter otherwise turns every reset/step of the old
    // point-lookup statement into a complete columnar scan.
    constexpr std::size_t kRouteBatchSize = 512;
    for (std::size_t offset = 0; offset < keys.size();
         offset += kRouteBatchSize) {
        if (stopToken.stop_requested()) {
            result.error = L"精密产品下载路线读取已取消";
            result.records.clear();
            return result;
        }
        const auto count = std::min(kRouteBatchSize, keys.size() - offset);
        std::string sql =
            "SELECT s.logical_key,d.source_key,d.source_priority,s.file_name,"
            "s.download_url,s.archive_class FROM product_sources s "
            "JOIN source_definitions d ON d.source_id=s.source_id "
            "WHERE s.logical_key IN (";
        for (std::size_t index = 0; index < count; ++index) {
            if (index != 0) sql += ',';
            sql += '?';
            sql += std::to_string(index + 1);
        }
        sql += ") ORDER BY s.logical_key,d.source_priority,s.file_name;";
        auto routes = Prepare(db.get(), sql.c_str(), result.error);
        if (!routes) return result;
        for (std::size_t index = 0; index < count; ++index) {
            if (!Bind(routes.get(), static_cast<int>(index + 1),
                      keys[offset + index])) {
                result.error = L"绑定精密产品下载路线失败";
                return result;
            }
        }
        while (true) {
            const int status = sqlite3_step(routes.get());
            if (status == SQLITE_DONE) break;
            if (status != SQLITE_ROW) {
                result.error = DbError(db.get(), L"读取精密产品下载路线失败");
                result.records.clear();
                return result;
            }
            const auto key = ColumnText(routes.get(), 0);
            const auto found = recordsByKey.find(key);
            if (found == recordsByKey.end()) continue;
            const Sp3ProductRoute route{ColumnText(routes.get(), 1),
                sqlite3_column_int(routes.get(), 2),
                ColumnText(routes.get(), 3),
                Utf8ToWide(ColumnText(routes.get(), 4)),
                ColumnText(routes.get(), 5)};
            if (route.downloadUrl.empty()) continue;
            for (const auto recordIndex : found->second) {
                auto& record = result.records[recordIndex].record;
                const auto duplicate = std::ranges::find_if(record.routes,
                    [&](const Sp3ProductRoute& existing) {
                        return existing.downloadUrl == route.downloadUrl;
                    });
                if (duplicate == record.routes.end()) record.routes.push_back(route);
            }
        }
        if (progress) {
            progress(std::min(offset + count, total), total);
        }
    }
    result.records.erase(std::remove_if(result.records.begin(), result.records.end(),
        [](const DatedSp3ProductRecord& item) {
            return item.record.routes.empty();
        }), result.records.end());
    return result;
}

bool ProvisionalWeek(int gpsWeek) {
    const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    return ToSysDays(GpsWeekStartDate(gpsWeek)) >= today - std::chrono::days{35};
}

} // namespace

int CalculateGpsWeek(const QueryDate& date) {
    const auto epoch = std::chrono::sys_days{std::chrono::year{1980} / std::chrono::January / 6};
    const auto days = (ToSysDays(date) - epoch).count();
    return days < 0 ? -1 : static_cast<int>(days / 7);
}

QueryDate GpsWeekStartDate(const int gpsWeek) {
    const auto epoch = std::chrono::sys_days{std::chrono::year{1980} / std::chrono::January / 6};
    return FromSysDays(epoch + std::chrono::days{gpsWeek * 7LL});
}

std::vector<Sp3ArchiveFile> ParseProductDirectoryListing(
    const std::string& listing, const int expectedGpsWeek,
    const std::wstring& directoryUrl, const std::string& archiveClass,
    const ProductKind kind) {
    static const std::regex sp3Candidate(
        R"(([A-Za-z0-9][A-Za-z0-9_.-]*\.(?:sp3|eph)(?:\.(?:gz|Z))?)(?=$|[^A-Za-z0-9_.-]))",
        std::regex::icase);
    static const std::regex clkCandidate(
        R"(([A-Za-z0-9][A-Za-z0-9_.-]*(?:_CLK\.CLK|\.clk(?:_?[0-9]{2}[sSmM]?)?)(?:\.(?:gz|Z))?)(?=$|[^A-Za-z0-9_.-]))",
        std::regex::icase);
    static const std::regex attCandidate(
        R"(([A-Za-z0-9][A-Za-z0-9_.-]*(?:_ATT\.OBX|\.obx)(?:\.(?:gz|Z))?)(?=$|[^A-Za-z0-9_.-]))",
        std::regex::icase);
    const std::regex& candidate = kind == ProductKind::Clk ? clkCandidate
        : kind == ProductKind::Att ? attCandidate : sp3Candidate;
    std::set<std::string> seen;
    std::vector<Sp3ArchiveFile> result;
    for (std::sregex_iterator it(listing.begin(), listing.end(), candidate), end;
         it != end; ++it) {
        std::string name = (*it)[1].str();
        const std::string key = Lower(name);
        if (!seen.insert(key).second) continue;
        Sp3ArchiveFile file;
        const bool parsedLong = ParseLongName(name, kind, file);
        const bool parsedAtt = kind == ProductKind::Att && ParseLegacyAttName(name, expectedGpsWeek, archiveClass, file);
        const bool parsedLegacy = kind != ProductKind::Att
            && ParseLegacyName(name, expectedGpsWeek,
                kind == ProductKind::Clk, archiveClass, file);
        if (!parsedLong && !parsedLegacy && !parsedAtt) continue;
        if (CalculateGpsWeek(file.nominalDate) != expectedGpsWeek) continue;
        file.fileName = std::move(name);
        file.downloadUrl = JoinUrl(directoryUrl, file.fileName);
        file.archiveClass = archiveClass;
        result.push_back(std::move(file));
    }
    return result;
}

std::vector<Sp3ArchiveFile> ParseSp3DirectoryListing(
    const std::string& listing, const int expectedGpsWeek,
    const std::wstring& directoryUrl, const std::string& archiveClass) {
    return ParseProductDirectoryListing(
        listing, expectedGpsWeek, directoryUrl, archiveClass, ProductKind::Sp3);
}

std::vector<ClkArchiveFile> ParseClkDirectoryListing(
    const std::string& listing, const int expectedGpsWeek,
    const std::wstring& directoryUrl, const std::string& archiveClass) {
    return ParseProductDirectoryListing(
        listing, expectedGpsWeek, directoryUrl, archiveClass, ProductKind::Clk);
}

std::vector<AttArchiveFile> ParseAttDirectoryListing(
    const std::string& listing, const int expectedGpsWeek,
    const std::wstring& directoryUrl, const std::string& archiveClass) {
    return ParseProductDirectoryListing(
        listing, expectedGpsWeek, directoryUrl, archiveClass, ProductKind::Att);
}

std::vector<Sp3SourceScan> QueryProductWeek(
    const int gpsWeek, const std::stop_token stopToken,
    const ProductKind kind) {
    std::vector<Sp3SourceScan> scans;
    scans.reserve(kind == ProductKind::Att ? 5 : kSourceCount);
    for (const auto& source : kSources) {
        // BKG did not publish ATT/OBX in the checked OPS/MGEX archive, while
        // KASI's legacy FTP is not a dependable ATT route.  Do not make every
        // ATT week permanently partial by treating non-providers as required.
        if (kind == ProductKind::Att
            && (std::string_view(source.key) == "BKG"
                || std::string_view(source.key) == "KASI")) continue;
        Sp3SourceScan scan;
        scan.sourceKey = source.key;
        scan.sourcePriority = source.priority;
        const std::array<std::pair<std::wstring, std::string>, 5> directories{{
            {std::wstring(source.root) + std::format(L"{:04}", gpsWeek) + L"/", "OPS"},
            {std::wstring(source.root) + L"mgex/" + std::format(L"{:04}", gpsWeek) + L"/", "MGEX"},
            {std::wstring(source.root) + L"repro1/" + std::format(L"{:04}", gpsWeek) + L"/", "REPRO1"},
            {std::wstring(source.root) + L"repro2/" + std::format(L"{:04}", gpsWeek) + L"/", "REPRO2"},
            {std::wstring(source.root) + L"repro3/" + std::format(L"{:04}", gpsWeek) + L"/", "REPRO3"},
        }};
        bool fatal = false;
        for (const auto& [url, archiveClass] : directories) {
            if (stopToken.stop_requested()) {
                scan.error = L"查询已取消";
                fatal = true;
                break;
            }
            scan.listingUrls.push_back(url);
            const HttpTextResult response = source.ftp
                ? HttpListFtpDirectory(url, stopToken, 32U * 1024U * 1024U)
                : HttpGetText(url, stopToken, 32U * 1024U * 1024U);
            if (response.ok()) {
                auto files = ParseProductDirectoryListing(
                    response.body, gpsWeek, url, archiveClass, kind);
                scan.files.insert(scan.files.end(),
                    std::make_move_iterator(files.begin()),
                    std::make_move_iterator(files.end()));
                continue;
            }
            if (MissingDirectory(response)) continue;
            fatal = true;
            scan.transportErrorCode = response.transportErrorCode;
            scan.networkFailure = IsTransportFailure(response.transportErrorCode);
            scan.error = response.error.empty()
                ? std::format(L"服务器返回 HTTP {}", response.statusCode)
                : response.error;
            break;
        }
        scan.success = !fatal;
        std::ranges::sort(scan.files, [](const auto& a, const auto& b) {
            if (a.logicalKey != b.logicalKey) return a.logicalKey < b.logicalKey;
            return a.downloadUrl < b.downloadUrl;
        });
        scan.files.erase(std::unique(scan.files.begin(), scan.files.end(),
            [](const auto& a, const auto& b) { return a.downloadUrl == b.downloadUrl; }),
            scan.files.end());
        scans.push_back(std::move(scan));
    }
    return scans;
}

std::vector<Sp3SourceScan> QuerySp3Week(
    const int gpsWeek, const std::stop_token stopToken) {
    return QueryProductWeek(gpsWeek, stopToken, ProductKind::Sp3);
}

std::vector<ClkSourceScan> QueryClkWeek(
    const int gpsWeek, const std::stop_token stopToken) {
    return QueryProductWeek(gpsWeek, stopToken, ProductKind::Clk);
}

std::vector<AttSourceScan> QueryAttWeek(
    const int gpsWeek, const std::stop_token stopToken) {
    return QueryProductWeek(gpsWeek, stopToken, ProductKind::Att);
}

std::filesystem::path GetSp3AvailabilityDatabasePath() {
    const auto explicitPath = EnvironmentPath(L"GNSS_DOWNLOADER_SP3_DB_PATH");
    if (!explicitPath.empty()) return explicitPath;
    const auto directory = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
    if (!directory.empty()) {
        return ResolveDatabaseFilePath(directory / L"sp3_availability.sqlite");
    }
    return ResolveDatabaseFilePath(
        GetIgsAvailabilityDatabasePath().parent_path() / L"sp3_availability.sqlite");
}

std::filesystem::path GetClkAvailabilityDatabasePath() {
    const auto explicitPath = EnvironmentPath(L"GNSS_DOWNLOADER_CLK_DB_PATH");
    if (!explicitPath.empty()) return explicitPath;
    const auto directory = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
    if (!directory.empty()) {
        return ResolveDatabaseFilePath(directory / L"clk_availability.sqlite");
    }
    return ResolveDatabaseFilePath(
        GetIgsAvailabilityDatabasePath().parent_path() / L"clk_availability.sqlite");
}

std::filesystem::path GetAttAvailabilityDatabasePath() {
    const auto explicitPath = EnvironmentPath(L"GNSS_DOWNLOADER_ATT_DB_PATH");
    if (!explicitPath.empty()) return explicitPath;
    const auto directory = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
    if (!directory.empty()) {
        return ResolveDatabaseFilePath(directory / L"att_availability.sqlite");
    }
    return ResolveDatabaseFilePath(
        GetIgsAvailabilityDatabasePath().parent_path() / L"att_availability.sqlite");
}

bool StoreAvailabilityWeek(const std::filesystem::path& databasePath,
                           const int gpsWeek,
                           const std::vector<Sp3SourceScan>& sources,
                           const int expectedSourceCount,
                           std::wstring& error) {
    std::scoped_lock lock(gProductDatabaseMutex);
    const int successful = static_cast<int>(std::ranges::count_if(sources,
        [](const auto& source) { return source.success; }));
    if (successful == 0) {
        error = L"七个 SP3 数据中心均未读取成功";
        return false;
    }
    std::map<std::string, Sp3ArchiveFile> products;
    for (const auto& source : sources) if (source.success) {
        for (const auto& file : source.files) products.try_emplace(file.logicalKey, file);
    }
    DbPtr db;
    if (!OpenDatabase(databasePath, db, error)) return false;
    int existing = -1;
    {
        auto statement = Prepare(db.get(), "SELECT source_success_count FROM weekly_scans WHERE gps_week=?1;", error);
        if (!statement) return false;
        sqlite3_bind_int(statement.get(), 1, gpsWeek);
        if (sqlite3_step(statement.get()) == SQLITE_ROW) existing = sqlite3_column_int(statement.get(), 0);
    }
    if (existing > successful
        || (existing == successful && successful < expectedSourceCount)) return true;
    if (!Execute(db.get(), "BEGIN IMMEDIATE;", error)) return false;
    bool committed = false;
    const auto rollback = [&]() { if (!committed) { std::wstring ignored; Execute(db.get(), "ROLLBACK;", ignored); } };
    {
        auto statement = Prepare(db.get(), "DELETE FROM weekly_scans WHERE gps_week=?1;", error);
        if (!statement) { rollback(); return false; }
        sqlite3_bind_int(statement.get(), 1, gpsWeek);
        if (sqlite3_step(statement.get()) != SQLITE_DONE) { error = DbError(db.get(), L"无法替换 SP3 周记录"); rollback(); return false; }
    }
    {
        auto statement = Prepare(db.get(), "INSERT INTO weekly_scans VALUES(?1,?2,?3,?4,?5,strftime('%Y-%m-%dT%H:%M:%SZ','now'));", error);
        if (!statement) { rollback(); return false; }
        sqlite3_bind_int(statement.get(), 1, gpsWeek);
        Bind(statement.get(), 2,
            successful == expectedSourceCount ? "complete" : "partial");
        sqlite3_bind_int(statement.get(), 3, successful);
        sqlite3_bind_int64(statement.get(), 4, static_cast<sqlite3_int64>(products.size()));
        sqlite3_bind_int(statement.get(), 5, ProvisionalWeek(gpsWeek) ? 1 : 0);
        if (sqlite3_step(statement.get()) != SQLITE_DONE) { error = DbError(db.get(), L"无法写入 SP3 周记录"); rollback(); return false; }
    }
    auto sourceStatement = Prepare(db.get(), "INSERT INTO weekly_source_scans VALUES(?1,?2,?3,?4,?5,?6);", error);
    if (!sourceStatement) { rollback(); return false; }
    for (const auto& source : sources) {
        std::string urls;
        for (const auto& url : source.listingUrls) { if (!urls.empty()) urls.push_back('\n'); urls += WideToUtf8(url); }
        sqlite3_reset(sourceStatement.get()); sqlite3_clear_bindings(sourceStatement.get());
        sqlite3_bind_int(sourceStatement.get(), 1, gpsWeek);
        sqlite3_bind_int(sourceStatement.get(), 2, source.sourcePriority);
        Bind(sourceStatement.get(), 3, source.success ? "complete" : "failed");
        Bind(sourceStatement.get(), 4, urls);
        sqlite3_bind_int64(sourceStatement.get(), 5, static_cast<sqlite3_int64>(source.files.size()));
        Bind(sourceStatement.get(), 6, WideToUtf8(source.error));
        if (sqlite3_step(sourceStatement.get()) != SQLITE_DONE) { error = DbError(db.get(), L"无法写入 SP3 来源状态"); rollback(); return false; }
    }
    auto productStatement = Prepare(db.get(), "INSERT INTO products VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13);", error);
    auto routeStatement = Prepare(db.get(), "INSERT OR IGNORE INTO product_sources VALUES(?1,?2,?3,?4,?5);", error);
    if (!productStatement || !routeStatement) { rollback(); return false; }
    for (const auto& [logicalKey, file] : products) {
        sqlite3_reset(productStatement.get()); sqlite3_clear_bindings(productStatement.get());
        Bind(productStatement.get(), 1, logicalKey); sqlite3_bind_int(productStatement.get(), 2, gpsWeek);
        Bind(productStatement.get(), 3, DateText(file.nominalDate));
        sqlite3_bind_int(productStatement.get(), 4, file.startHour); sqlite3_bind_int(productStatement.get(), 5, file.startMinute);
        Bind(productStatement.get(), 6, file.series); Bind(productStatement.get(), 7, file.center);
        Bind(productStatement.get(), 8, file.project); Bind(productStatement.get(), 9, file.solution);
        Bind(productStatement.get(), 10, file.latency); Bind(productStatement.get(), 11, file.duration);
        Bind(productStatement.get(), 12, file.interval); Bind(productStatement.get(), 13, file.constellation);
        if (sqlite3_step(productStatement.get()) != SQLITE_DONE) { error = DbError(db.get(), L"无法写入 SP3 产品"); rollback(); return false; }
        for (const auto& source : sources) if (source.success) for (const auto& route : source.files) {
            if (route.logicalKey != logicalKey) continue;
            sqlite3_reset(routeStatement.get()); sqlite3_clear_bindings(routeStatement.get());
            Bind(routeStatement.get(), 1, logicalKey); sqlite3_bind_int(routeStatement.get(), 2, source.sourcePriority);
            Bind(routeStatement.get(), 3, route.fileName); Bind(routeStatement.get(), 4, WideToUtf8(route.downloadUrl));
            Bind(routeStatement.get(), 5, route.archiveClass);
            if (sqlite3_step(routeStatement.get()) != SQLITE_DONE) { error = DbError(db.get(), L"无法写入 SP3 下载来源"); rollback(); return false; }
        }
    }
    if (!Execute(db.get(), "COMMIT;", error)) { rollback(); return false; }
    committed = true;
    return true;
}

bool StoreSp3Week(const int gpsWeek, const std::vector<Sp3SourceScan>& sources,
                  std::wstring& error) {
    return StoreAvailabilityWeek(
        GetSp3AvailabilityDatabasePath(), gpsWeek, sources, kSourceCount, error);
}

bool StoreClkWeek(const int gpsWeek, const std::vector<ClkSourceScan>& sources,
                  std::wstring& error) {
    return StoreAvailabilityWeek(
        GetClkAvailabilityDatabasePath(), gpsWeek, sources, kSourceCount, error);
}

bool StoreAttWeek(const int gpsWeek, const std::vector<AttSourceScan>& sources,
                  std::wstring& error) {
    return StoreAvailabilityWeek(
        GetAttAvailabilityDatabasePath(), gpsWeek, sources, 5, error);
}

Sp3AvailabilityLookup LoadProductAvailability(
    const std::filesystem::path& databasePath, const QueryDate& date) {
    std::scoped_lock lock(gProductDatabaseMutex);
    Sp3AvailabilityLookup result;
    result.gpsWeek = CalculateGpsWeek(date);
    result.databasePath = databasePath;
    DbPtr db;
    if (!OpenReadDatabase(result.databasePath, db, result.error)) return result;
    {
        auto statement = Prepare(db.get(), "SELECT status,source_success_count,is_provisional FROM weekly_scans WHERE gps_week=?1;", result.error);
        if (!statement) return result;
        sqlite3_bind_int(statement.get(), 1, result.gpsWeek);
        if (sqlite3_step(statement.get()) != SQLITE_ROW) { result.error.clear(); return result; }
        result.indexed = true;
        // A provisional scan is an intentionally incomplete snapshot even
        // when all configured listing sources happened to respond.  Keep the
        // single-day and range readers consistent with Is*WeekComplete so a
        // bundle planner cannot treat a provisional week as download-ready.
        result.complete = ColumnText(statement.get(), 0) == "complete"
            && sqlite3_column_int(statement.get(), 2) == 0;
        result.sourcesSuccessful = sqlite3_column_int(statement.get(), 1);
        result.provisional = sqlite3_column_int(statement.get(), 2) != 0;
    }
    auto products = Prepare(db.get(), "SELECT logical_key,start_hour,start_minute,series,center,project,solution,latency,duration,sample_interval,constellation FROM products WHERE nominal_date=?1 OR (solution='FIN' AND duration IN ('02D','03D','04D','05D','06D','07D') AND nominal_date BETWEEN ?2 AND ?1) ORDER BY latency,center,series,start_hour;", result.error);
    if (!products || !Bind(products.get(), 1, DateText(date))
        || !Bind(products.get(), 2, DateText(FromSysDays(ToSysDays(date) - std::chrono::days{6})))) return result;
    auto routes = Prepare(db.get(), "SELECT d.source_key,d.source_priority,s.file_name,s.download_url,s.archive_class FROM product_sources s JOIN source_definitions d ON d.source_id=s.source_id WHERE s.logical_key=?1 ORDER BY d.source_priority,s.file_name;", result.error);
    if (!routes) return result;
    while (sqlite3_step(products.get()) == SQLITE_ROW) {
        Sp3ProductRecord record;
        record.logicalKey = ColumnText(products.get(), 0); record.nominalDate = date;
        record.startHour = sqlite3_column_int(products.get(), 1); record.startMinute = sqlite3_column_int(products.get(), 2);
        record.series = ColumnText(products.get(), 3); record.center = ColumnText(products.get(), 4);
        record.project = ColumnText(products.get(), 5); record.solution = ColumnText(products.get(), 6);
        record.latency = ColumnText(products.get(), 7); record.duration = ColumnText(products.get(), 8);
        record.interval = ColumnText(products.get(), 9); record.constellation = ColumnText(products.get(), 10);
        sqlite3_reset(routes.get());
        sqlite3_clear_bindings(routes.get());
        if (!Bind(routes.get(), 1, record.logicalKey)) { result.products.clear(); return result; }
        while (sqlite3_step(routes.get()) == SQLITE_ROW) {
            record.routes.push_back({ColumnText(routes.get(), 0), sqlite3_column_int(routes.get(), 1),
                ColumnText(routes.get(), 2), Utf8ToWide(ColumnText(routes.get(), 3)), ColumnText(routes.get(), 4)});
        }
        result.products.push_back(std::move(record));
    }
    return result;
}

Sp3AvailabilityRangeLookup LoadProductAvailabilityRange(
    const std::filesystem::path& databasePath,
    const QueryDate& startDate,
    const QueryDate& endDate) {
    std::scoped_lock lock(gProductDatabaseMutex);
    Sp3AvailabilityRangeLookup result;
    result.databasePath = databasePath;
    const auto first = ToSysDays(startDate);
    const auto last = ToSysDays(endDate);
    const auto span = (last - first).count();
    if (span < 0 || span > static_cast<long long>(
            std::numeric_limits<int>::max() - 1)) {
        result.error = L"精密产品日期范围无效";
        return result;
    }
    result.expectedDays = static_cast<int>(span) + 1;

    DbPtr db;
    if (!OpenReadDatabase(databasePath, db, result.error)) return result;

    // Weekly scans cover SP3, CLK and ATT alike.  Read their status in one
    // pass, then expand it in memory to the requested UTC dates.
    const int firstWeek = CalculateGpsWeek(startDate);
    const int lastWeek = CalculateGpsWeek(endDate);
    if (firstWeek < 0 || lastWeek < firstWeek) {
        result.error = L"精密产品日期范围早于 GPS 周起点";
        return result;
    }
    std::unordered_map<int, bool> indexedWeeks;
    std::unordered_map<int, bool> completeWeeks;
    auto scans = Prepare(db.get(),
        "SELECT gps_week,status,is_provisional FROM weekly_scans "
        "WHERE gps_week BETWEEN ?1 AND ?2;", result.error);
    if (!scans) return result;
    sqlite3_bind_int(scans.get(), 1, firstWeek);
    sqlite3_bind_int(scans.get(), 2, lastWeek);
    while (sqlite3_step(scans.get()) == SQLITE_ROW) {
        const int week = sqlite3_column_int(scans.get(), 0);
        indexedWeeks[week] = true;
        // ``status=complete`` only means every source was contacted.  The
        // index builder marks provisional weeks separately (for example the
        // current ultra-rapid week); those dates must remain unavailable to a
        // strict precise-product bundle until a non-provisional scan lands.
        completeWeeks[week] = ColumnText(scans.get(), 1) == "complete"
            && sqlite3_column_int(scans.get(), 2) == 0;
    }
    for (int offset = 0; offset < result.expectedDays; ++offset) {
        const QueryDate day = FromSysDays(first + std::chrono::days{offset});
        const int week = CalculateGpsWeek(day);
        if (indexedWeeks.contains(week)) ++result.indexedDays;
        if (!indexedWeeks.contains(week) || !completeWeeks[week]) {
            result.incompleteDates.push_back(day);
        }
    }
    result.complete = result.incompleteDates.empty();

    // The LEFT JOIN returns one row per physical route, but product metadata
    // is decoded only once per (date,logical_key).  This removes the old
    // per-day/per-product route statement loop for multi-decade queries.
    auto products = Prepare(db.get(),
        "SELECT p.nominal_date,p.logical_key,p.start_hour,p.start_minute,"
        "p.series,p.center,p.project,p.solution,p.latency,p.duration,"
        "p.sample_interval,p.constellation,d.source_key,d.source_priority,"
        "s.file_name,s.download_url,s.archive_class "
        "FROM products p "
        "LEFT JOIN product_sources s ON s.logical_key=p.logical_key "
        "LEFT JOIN source_definitions d ON d.source_id=s.source_id "
        "WHERE p.nominal_date BETWEEN ?1 AND ?2 "
        // Do not ask SQLite to sort the full product/source join.  For a
        // multi-decade database this can materialize a very large temporary
        // B-tree on the UI thread and make the downloader look hung.  The
        // compact product records are sorted once below after aggregation.
        ";", result.error);
    if (!products) return result;
    const std::string firstText = DateText(FromSysDays(first - std::chrono::days{6}));
    const std::string lastText = DateText(endDate);
    if (!Bind(products.get(), 1, firstText)
        || !Bind(products.get(), 2, lastText)) return result;
    std::unordered_map<std::string, std::size_t> byProduct;
    const auto parseDate = [](const std::string& text) {
        int year = 0, month = 0, day = 0;
        if (sscanf_s(text.c_str(), "%d-%d-%d", &year, &month, &day) != 3) {
            return QueryDate{};
        }
        return MakeQueryDate(year, month, day);
    };
    while (sqlite3_step(products.get()) == SQLITE_ROW) {
        const std::string dateText = ColumnText(products.get(), 0);
        const std::string logicalKey = ColumnText(products.get(), 1);
        const std::string mapKey = dateText + "\x1f" + logicalKey;
        auto [it, inserted] = byProduct.emplace(mapKey, result.records.size());
        if (inserted) {
            Sp3ProductRecord record;
            record.logicalKey = logicalKey;
            record.nominalDate = parseDate(dateText);
            record.startHour = sqlite3_column_int(products.get(), 2);
            record.startMinute = sqlite3_column_int(products.get(), 3);
            record.series = ColumnText(products.get(), 4);
            record.center = ColumnText(products.get(), 5);
            record.project = ColumnText(products.get(), 6);
            record.solution = ColumnText(products.get(), 7);
            record.latency = ColumnText(products.get(), 8);
            record.duration = ColumnText(products.get(), 9);
            record.interval = ColumnText(products.get(), 10);
            record.constellation = ColumnText(products.get(), 11);
            result.records.push_back({record.nominalDate, std::move(record)});
        }
        auto& record = result.records[it->second].record;
        const std::string source = ColumnText(products.get(), 12);
        const std::string fileName = ColumnText(products.get(), 14);
        const std::string url = ColumnText(products.get(), 15);
        if (source.empty() || fileName.empty() || url.empty()) continue;
        const auto duplicate = std::ranges::find_if(record.routes,
            [&](const Sp3ProductRoute& route) {
                return WideToUtf8(route.downloadUrl) == url;
            });
        if (duplicate == record.routes.end()) {
            record.routes.push_back({source, sqlite3_column_int(products.get(), 13),
                fileName, Utf8ToWide(url), ColumnText(products.get(), 16)});
        }
    }
    std::vector<DatedSp3ProductRecord> expanded;
    for (auto& dated : result.records) {
        const auto nominal = ToSysDays(dated.date);
        for (int offset = 0; offset < CoveredOrbitDays(dated.record.duration,dated.record.solution); ++offset) {
            const auto day = nominal + std::chrono::days{offset};
            if (day < first || day > last) continue;
            auto copy = dated;
            copy.date = FromSysDays(day);
            expanded.push_back(std::move(copy));
        }
    }
    result.records = std::move(expanded);
    // Keep the result deterministic for the range consumer while avoiding a
    // database-wide sort over every physical mirror route.  Routes retain
    // their source priority order within each logical product below.
    std::ranges::sort(result.records, [](const DatedSp3ProductRecord& left,
                                        const DatedSp3ProductRecord& right) {
        const auto leftDay = ToSysDays(left.date);
        const auto rightDay = ToSysDays(right.date);
        if (leftDay != rightDay) return leftDay < rightDay;
        const auto& a = left.record;
        const auto& b = right.record;
        if (a.latency != b.latency) return a.latency < b.latency;
        if (a.center != b.center) return a.center < b.center;
        if (a.series != b.series) return a.series < b.series;
        if (a.startHour != b.startHour) return a.startHour < b.startHour;
        return a.logicalKey < b.logicalKey;
    });
    for (auto& dated : result.records) {
        std::ranges::sort(dated.record.routes,
            [](const Sp3ProductRoute& left, const Sp3ProductRoute& right) {
                if (left.sourcePriority != right.sourcePriority) {
                    return left.sourcePriority < right.sourcePriority;
                }
                return left.fileName < right.fileName;
            });
    }
    return result;
}

Sp3AvailabilityLookup LoadSp3Availability(const QueryDate& date) {
    return LoadProductAvailability(GetSp3AvailabilityDatabasePath(), date);
}

Sp3AvailabilityRangeLookup LoadSp3AvailabilityRange(
    const QueryDate& startDate, const QueryDate& endDate) {
    return LoadProductAvailabilityRange(
        GetSp3AvailabilityDatabasePath(), startDate, endDate);
}

Sp3AvailabilityRangeLookup LoadClkAvailabilityRange(
    const QueryDate& startDate, const QueryDate& endDate) {
    return LoadProductAvailabilityRange(
        GetClkAvailabilityDatabasePath(), startDate, endDate);
}

Sp3AvailabilityRangeLookup LoadAttAvailabilityRange(
    const QueryDate& startDate, const QueryDate& endDate) {
    return LoadProductAvailabilityRange(
        GetAttAvailabilityDatabasePath(), startDate, endDate);
}

Sp3ProductFamilyRangeLookup LoadSp3ProductFamilyRange(
    const QueryDate& startDate, const QueryDate& endDate) {
    return LoadProductFamilyRange(
        GetSp3AvailabilityDatabasePath(), startDate, endDate);
}

Sp3ProductFamilyRangeLookup LoadClkProductFamilyRange(
    const QueryDate& startDate, const QueryDate& endDate) {
    return LoadProductFamilyRange(
        GetClkAvailabilityDatabasePath(), startDate, endDate);
}

Sp3ProductFamilyRangeLookup LoadAttProductFamilyRange(
    const QueryDate& startDate, const QueryDate& endDate) {
    return LoadProductFamilyRange(
        GetAttAvailabilityDatabasePath(), startDate, endDate);
}

Sp3ProductFamilyRoutesLookup LoadSp3ProductFamilyRoutes(
    const std::string& familyKey,
    const QueryDate& startDate,
    const QueryDate& endDate,
    const std::stop_token stopToken,
    Sp3ProductRouteProgress progress) {
    return LoadProductFamilyRoutes(
        GetSp3AvailabilityDatabasePath(), familyKey,
        startDate, endDate, stopToken, progress);
}

Sp3ProductFamilyRoutesLookup LoadClkProductFamilyRoutes(
    const std::string& familyKey,
    const QueryDate& startDate,
    const QueryDate& endDate,
    const std::stop_token stopToken,
    Sp3ProductRouteProgress progress) {
    return LoadProductFamilyRoutes(
        GetClkAvailabilityDatabasePath(), familyKey,
        startDate, endDate, stopToken, progress);
}

Sp3ProductFamilyRoutesLookup LoadAttProductFamilyRoutes(
    const std::string& familyKey,
    const QueryDate& startDate,
    const QueryDate& endDate,
    const std::stop_token stopToken,
    Sp3ProductRouteProgress progress) {
    return LoadProductFamilyRoutes(
        GetAttAvailabilityDatabasePath(), familyKey,
        startDate, endDate, stopToken, progress);
}

ClkAvailabilityLookup LoadClkAvailability(const QueryDate& date) {
    return LoadProductAvailability(GetClkAvailabilityDatabasePath(), date);
}

AttAvailabilityLookup LoadAttAvailability(const QueryDate& date) {
    return LoadProductAvailability(GetAttAvailabilityDatabasePath(), date);
}

bool IsProductWeekComplete(const std::filesystem::path& databasePath,
                           const int gpsWeek, bool& complete,
                           std::wstring& error) {
    std::scoped_lock lock(gProductDatabaseMutex);
    complete = false;
    DbPtr db;
    if (!OpenDatabase(databasePath, db, error)) return false;
    auto statement = Prepare(db.get(), "SELECT status,is_provisional FROM weekly_scans WHERE gps_week=?1;", error);
    if (!statement) return false;
    sqlite3_bind_int(statement.get(), 1, gpsWeek);
    if (sqlite3_step(statement.get()) == SQLITE_ROW) complete = ColumnText(statement.get(), 0) == "complete" && sqlite3_column_int(statement.get(), 1) == 0;
    return true;
}

bool IsSp3WeekComplete(const int gpsWeek, bool& complete, std::wstring& error) {
    return IsProductWeekComplete(
        GetSp3AvailabilityDatabasePath(), gpsWeek, complete, error);
}

bool IsClkWeekComplete(const int gpsWeek, bool& complete, std::wstring& error) {
    return IsProductWeekComplete(
        GetClkAvailabilityDatabasePath(), gpsWeek, complete, error);
}

bool IsAttWeekComplete(const int gpsWeek, bool& complete, std::wstring& error) {
    return IsProductWeekComplete(
        GetAttAvailabilityDatabasePath(), gpsWeek, complete, error);
}

Sp3AvailabilityStats GetProductAvailabilityStats(
    const std::filesystem::path& databasePath) {
    std::scoped_lock lock(gProductDatabaseMutex);
    Sp3AvailabilityStats result;
    result.databasePath = databasePath;
    DbPtr db;
    if (!OpenDatabase(result.databasePath, db, result.error)) return result;
    auto statement = Prepare(db.get(), "SELECT SUM(status='complete'),SUM(status='partial'),MIN(gps_week),MAX(gps_week) FROM weekly_scans;", result.error);
    if (statement && sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.completeWeeks = sqlite3_column_int(statement.get(), 0); result.partialWeeks = sqlite3_column_int(statement.get(), 1);
        result.firstGpsWeek = sqlite3_column_type(statement.get(), 2) == SQLITE_NULL ? -1 : sqlite3_column_int(statement.get(), 2);
        result.lastGpsWeek = sqlite3_column_type(statement.get(), 3) == SQLITE_NULL ? -1 : sqlite3_column_int(statement.get(), 3);
    }
    auto counts = Prepare(db.get(), "SELECT (SELECT COUNT(*) FROM products),(SELECT COUNT(*) FROM product_sources);", result.error);
    if (counts && sqlite3_step(counts.get()) == SQLITE_ROW) {
        result.products = sqlite3_column_int64(counts.get(), 0); result.sourceRoutes = sqlite3_column_int64(counts.get(), 1);
    }
    return result;
}

Sp3AvailabilityStats GetSp3AvailabilityStats() {
    return GetProductAvailabilityStats(GetSp3AvailabilityDatabasePath());
}

ClkAvailabilityStats GetClkAvailabilityStats() {
    return GetProductAvailabilityStats(GetClkAvailabilityDatabasePath());
}

AttAvailabilityStats GetAttAvailabilityStats() {
    return GetProductAvailabilityStats(GetAttAvailabilityDatabasePath());
}
