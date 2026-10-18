#include "ErpAvailability.h"

#include "DateUtil.h"
#include "ArchiveLegacyNames.h"
#include "HttpClient.h"
#include "IgsAvailabilityDb.h"
#include "Sp3Availability.h"

#include "sqlite3.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace {

struct SourceDefinition {
    const char* key;
    int priority;
    const wchar_t* root;
    bool ftp;
};

constexpr std::array kSources{
    SourceDefinition{"CDDIS", 1,
        L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/products/", true},
    SourceDefinition{"BKG", 2,
        L"https://igs.bkg.bund.de/root_ftp/IGS/products/", false},
    SourceDefinition{"WHU", 3,
        L"ftp://igs.gnsswhu.cn/pub/gps/products/", true},
    // KASI's legacy nfs.kasi.re.kr FTP endpoint currently accepts a TCP/SOCKS
    // connection but closes it before sending the FTP greeting.  Keep source
    // id 4 reserved in existing databases, but do not scan it as an active
    // ERP archive.
    SourceDefinition{"IGN", 5,
        L"ftp://igs.ign.fr/pub/igs/products/", true},
    SourceDefinition{"ESA", 6,
        L"ftp://gssc.esa.int/gnss/products/", true},
    SourceDefinition{"SOPAC", 7,
        L"http://garner.ucsd.edu/pub/products/", false},
};
constexpr int kSourceCount = static_cast<int>(kSources.size());

struct IersDefinition {
    const char* series;
    const wchar_t* fileName;
    const char* latency;
    const char* interval;
    const char* standard;
    const char* notes;
};

constexpr wchar_t kIersRoot[] =
    L"https://datacenter.iers.org/data/latestVersion/";

// These are the operational IERS EOP series useful to GNSS software.  C01,
// Bulletin C and Bulletin D are deliberately excluded: C01 is a historical
// astronomy series, while C/D are leap-second and DUT1 announcements rather
// than day-addressable GNSS ERP/EOP files.
constexpr std::array kIersSeries{
    IersDefinition{"IERS_C04_00_DXDY",
        L"EOP_20u24_C04_one_file_1962-now.txt", "FINAL", "01D",
        "IAU2000A_DX_DY_00H", "EOP 20u24 C04, 0 h UTC, dX/dY"},
    IersDefinition{"IERS_C04_12_DXDY",
        L"EOP_20u24_C04_one_file_12h_1984-now.txt", "FINAL", "01D",
        "IAU2000A_DX_DY_12H", "EOP 20u24 C04, 12 h UTC, dX/dY"},
    IersDefinition{"IERS_C04_00_DPSIDEPS",
        L"EOP_20u24_C04_0h_dPsi_dEps_1962-now.txt", "FINAL", "01D",
        "IAU2000A_DPSI_DEPS_00H", "EOP 20u24 C04, 0 h UTC, dPsi/dEps"},
    IersDefinition{"IERS_C04_12_DPSIDEPS",
        L"EOP_20u24_C04_12h_dPsi_dEps_1984-now.txt", "FINAL", "01D",
        "IAU2000A_DPSI_DEPS_12H", "EOP 20u24 C04, 12 h UTC, dPsi/dEps"},
    IersDefinition{"IERS_FINALS_ALL_IAU2000",
        L"finals.all.iau2000.txt", "RAPID_PREDICTED", "01D",
        "IAU2000A_FINALS_ALL", "Bulletin A standard EOP, full IAU2000 series"},
    IersDefinition{"IERS_FINALS_DAILY_IAU2000",
        L"finals.daily.iau2000.txt", "RAPID_PREDICTED", "01D",
        "IAU2000A_FINALS_DAILY", "Bulletin A recent daily IAU2000 subset"},
    IersDefinition{"IERS_FINALS_ALL_IAU1980",
        L"finals.all.iau1980.txt", "RAPID_PREDICTED", "01D",
        "IAU1980_FINALS_ALL", "Bulletin A legacy IAU1980 compatibility series"},
    IersDefinition{"IERS_FINALS_DAILY_IAU1980",
        L"finals.daily.iau1980.txt", "RAPID_PREDICTED", "01D",
        "IAU1980_FINALS_DAILY", "Bulletin A recent daily IAU1980 subset"},
    IersDefinition{"IERS_GPSRAPID_OUT",
        L"gpsrapid.out.txt", "RAPID_PREDICTED", "01D",
        "IERS_GPS_RAPID", "IERS GPS rapid EOP full series"},
    IersDefinition{"IERS_GPSRAPID_DAILY",
        L"gpsrapid.daily.txt", "RAPID_PREDICTED", "01D",
        "IERS_GPS_RAPID_DAILY", "IERS GPS rapid recent daily subset"},
    IersDefinition{"IERS_BULLETIN_B",
        L"bulletinB.txt", "FINAL", "01D",
        "IERS_BULLETIN_B", "Current monthly Bulletin B final EOP issue"},
};

struct DbCloser {
    void operator()(sqlite3* value) const { if (value) sqlite3_close(value); }
};
struct StmtCloser {
    void operator()(sqlite3_stmt* value) const {
        if (value) sqlite3_finalize(value);
    }
};
using DbPtr = std::unique_ptr<sqlite3, DbCloser>;
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtCloser>;

std::mutex gDatabaseMutex;

std::wstring Utf8ToWide(const std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string WideToUtf8(const std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::filesystem::path EnvironmentPath(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return {};
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        name, value.data(), required);
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
        static_cast<unsigned>(date.month()),
        static_cast<unsigned>(date.day()));
}

std::string DateText(const QueryDate& date) {
    return std::format("{:04}-{:02}-{:02}",
        date.year, date.month, date.day);
}

bool IsValidRange(const QueryDate& start, const QueryDate& end) {
    return ToSysDays(start) <= ToSysDays(end);
}

int InclusiveDays(const QueryDate& start, const QueryDate& end) {
    if (!IsValidRange(start, end)) return 0;
    return static_cast<int>((ToSysDays(end) - ToSysDays(start)).count()) + 1;
}

QueryDate ParseDateText(const std::string& value) {
    if (value.size() < 10) return {};
    try {
        return MakeQueryDate(std::stoi(value.substr(0, 4)),
            std::stoi(value.substr(5, 2)), std::stoi(value.substr(8, 2)));
    } catch (...) { return {}; }
}

QueryDate DateFromDoy(const int year, const int doy) {
    return FromSysDays(std::chrono::sys_days{
        std::chrono::year{year} / std::chrono::January / 1}
        + std::chrono::days{doy - 1});
}

std::wstring JoinUrl(const std::wstring& directory,
                     const std::string& file) {
    std::wstring result = directory;
    if (!result.ends_with(L'/')) result.push_back(L'/');
    result += Utf8ToWide(file);
    return result;
}

bool IsTransportFailure(const long code) {
    return code == 5 || code == 6 || code == 7 || code == 28 || code == 35
        || code == 52 || code == 55 || code == 56 || code == 60;
}

bool IsNetworkFailure(const HttpTextResult& result) {
    return IsTransportFailure(result.transportErrorCode)
        || result.statusCode == 408 || result.statusCode == 429
        || result.statusCode == 500 || result.statusCode == 502
        || result.statusCode == 503 || result.statusCode == 504;
}

bool MissingDirectory(const HttpTextResult& result) {
    if (result.statusCode == 404 || result.statusCode == 410
        || result.transportErrorCode == 9) return true;
    const std::string message = Lower(WideToUtf8(result.error));
    return message.find("404") != std::string::npos
        || message.find("410") != std::string::npos
        || message.find("550") != std::string::npos
        || message.find("not found") != std::string::npos
        || message.find("no such file") != std::string::npos;
}

std::string LatencyFor(const std::string& project,
                       const std::string& solution) {
    const std::string p = Upper(project);
    const std::string s = Upper(solution);
    if (p.starts_with("R0")) return "REPROCESSING";
    if (s == "FIN" || s == "SNX") return "FINAL";
    if (s == "RAP") return "RAPID";
    if (s == "ULT" || s == "ULA") return "ULTRA_RAPID";
    if (s == "NRT") return "NEAR_REAL_TIME";
    if (s == "RTS") return "REAL_TIME";
    return "OTHER";
}

std::string LogicalKey(const ErpArchiveFile& file) {
    return file.productFamily + "|" + file.series + "|"
        + DateText(file.nominalDate) + "|"
        + std::format("{:02}{:02}|{}|{}|{}", file.startHour,
            file.startMinute, file.duration, file.interval, file.standard);
}

std::chrono::seconds DurationSeconds(const std::string& duration) {
    static const std::regex pattern(R"(^([0-9]{2})([DHMSUW])$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(duration, match, pattern)) return {};
    const long long value = std::stoll(match[1].str());
    const char unit = static_cast<char>(
        std::toupper(static_cast<unsigned char>(match[2].str()[0])));
    switch (unit) {
    case 'S': return std::chrono::seconds{value};
    case 'M': return std::chrono::minutes{value};
    case 'H': return std::chrono::hours{value};
    case 'D': return std::chrono::hours{24 * value};
    case 'W': return std::chrono::hours{24 * 7 * value};
    default: return {};
    }
}

QueryDate CoverageEnd(const ErpArchiveFile& file) {
    const auto duration = DurationSeconds(file.duration);
    if (duration <= std::chrono::seconds::zero()) return file.nominalDate;
    const auto start = std::chrono::sys_seconds{ToSysDays(file.nominalDate)}
        + std::chrono::hours{file.startHour}
        + std::chrono::minutes{file.startMinute};
    return FromSysDays(std::chrono::floor<std::chrono::days>(
        start + duration - std::chrono::seconds{1}));
}

std::string CenterForLegacy(const std::string& rawPrefix) {
    static const std::unordered_map<std::string, std::string> centers{
        {"igs", "IGS"}, {"igr", "IGS"}, {"igu", "IGS"},
        {"coa", "COD"}, {"cou", "COD"}, {"cod", "COD"},
        {"emu", "EMR"}, {"emr", "EMR"},
        {"esu", "ESA"}, {"esa", "ESA"},
        {"gfu", "GFZ"}, {"gfz", "GFZ"},
        {"grg", "GRG"}, {"jpu", "JPL"}, {"jpl", "JPL"},
        {"mit", "MIT"}, {"ngu", "NGS"}, {"ngs", "NGS"},
        {"siu", "SIO"}, {"sio", "SIO"},
        {"usu", "USN"}, {"usn", "USN"},
        {"whu", "WHU"}, {"wum", "WUM"}, {"jgx", "JGX"},
        {"hus", "HUS"},
    };
    const auto found = centers.find(Lower(rawPrefix));
    return found == centers.end() ? Upper(rawPrefix) : found->second;
}

bool ParseLongName(const std::string& name, ErpArchiveFile& file) {
    static const std::regex pattern(
        R"(^([A-Z0-9]{10})_([0-9]{4})([0-9]{3})([0-9]{2})([0-9]{2})_([A-Z0-9]{3})_([A-Z0-9]{3})_ERP\.ERP(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) return false;
    const int year = std::stoi(match[2].str());
    const int doy = std::stoi(match[3].str());
    if (year < 1980 || doy < 1
        || doy > (IsLeapYear(year) ? 366 : 365)) return false;
    file.nominalDate = DateFromDoy(year, doy);
    file.coverageStart = file.nominalDate;
    file.startHour = std::stoi(match[4].str());
    file.startMinute = std::stoi(match[5].str());
    file.series = Upper(match[1].str());
    file.center = file.series.substr(0, 3);
    file.project = file.series.substr(4, 3);
    file.solution = file.series.substr(7, 3);
    file.latency = LatencyFor(file.project, file.solution);
    file.duration = Upper(match[6].str());
    file.interval = Upper(match[7].str());
    file.standard = "IGS_ERP_LONG";
    file.coverageEnd = CoverageEnd(file);
    file.logicalKey = LogicalKey(file);
    return true;
}

bool ParseLegacyName(const std::string& name, const int expectedGpsWeek,
                     const std::string& archiveClass,
                     ErpArchiveFile& file) {
    static const std::regex pattern(
        R"(^([A-Z0-9]{3})([0-9]{4})([0-7])(?:_([0-9]{2}))?\.ERP(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) return false;
    if (std::stoi(match[2].str()) != expectedGpsWeek) return false;
    const std::string prefix = Lower(match[1].str());
    const int day = std::stoi(match[3].str());
    const bool hasHour = match[4].matched;
    const bool ultraPrefix = prefix == "igu" || prefix == "cou"
        || prefix == "emu" || prefix == "esu" || prefix == "gfu"
        || prefix == "jpu" || prefix == "ngu" || prefix == "siu"
        || prefix == "usu";
    std::string solution;
    if (hasHour || ultraPrefix) solution = "ULT";
    else if (day == 7 || prefix == "igs") solution = "FIN";
    else solution = "RAP";
    if (prefix == "igr") solution = "RAP";

    file.center = CenterForLegacy(prefix);
    file.project = archiveClass == "MGEX" ? "MGX"
        : archiveClass == "REPRO3" ? "R03" : "OPS";
    if (const auto project = ArchiveReprocessingProject(archiveClass); !project.empty()) {
        file.project = project;
        file.center = ArchiveReprocessingCenter(Upper(prefix));
        solution = "FIN";
    }
    file.solution = solution;
    file.latency = LatencyFor(file.project, file.solution);
    file.series = (ArchiveReprocessingProject(archiveClass).empty() ? file.center : Upper(prefix)) + "0" + file.project + file.solution;
    file.nominalDate = day == 7
        ? GpsWeekStartDate(expectedGpsWeek)
        : FromSysDays(ToSysDays(GpsWeekStartDate(expectedGpsWeek))
            + std::chrono::days{day});
    file.coverageStart = file.nominalDate;
    file.startHour = hasHour ? std::stoi(match[4].str()) : 0;
    file.startMinute = 0;
    file.duration = solution == "FIN" ? "07D"
        : solution == "ULT" ? "02D" : "01D";
    if (!ArchiveReprocessingProject(archiveClass).empty() && day < 7) file.duration = "01D";
    file.interval = "01D";
    file.standard = "IGS_ERP_LEGACY";
    file.coverageEnd = CoverageEnd(file);
    file.logicalKey = LogicalKey(file);
    return true;
}

std::wstring DbError(sqlite3* database, const wchar_t* action) {
    const char* raw = database ? sqlite3_errmsg(database)
        : "unknown SQLite error";
    return std::wstring(action) + L"：" + Utf8ToWide(raw ? raw : "");
}

bool Execute(sqlite3* database, const char* sql, std::wstring& error) {
    char* message = nullptr;
    if (sqlite3_exec(database, sql, nullptr, nullptr, &message) == SQLITE_OK)
        return true;
    error = L"SQLite：" + Utf8ToWide(
        message ? message : sqlite3_errmsg(database));
    sqlite3_free(message);
    return false;
}

StmtPtr Prepare(sqlite3* database, const char* sql, std::wstring& error) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK) {
        error = DbError(database, L"无法准备 ERP/EOP 知识库查询");
        return {};
    }
    return StmtPtr(raw);
}

bool Bind(sqlite3_stmt* statement, const int index,
          const std::string& value) {
    return sqlite3_bind_text(statement, index, value.c_str(),
        static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

std::string ColumnText(sqlite3_stmt* statement, const int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char*>(value) : "";
}

bool OpenDatabase(const std::filesystem::path& path, DbPtr& database,
                  std::wstring& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = L"无法创建 ERP/EOP 知识库目录："
            + path.parent_path().wstring();
        return false;
    }
    sqlite3* raw = nullptr;
    const int status = sqlite3_open16(path.c_str(), &raw);
    database.reset(raw);
    if (status != SQLITE_OK || raw == nullptr) {
        error = DbError(raw, L"无法打开 ERP/EOP 知识库");
        return false;
    }
    sqlite3_busy_timeout(raw, 10000);
    const char* schema =
        "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS source_definitions(source_id INTEGER PRIMARY KEY,source_key TEXT NOT NULL UNIQUE,source_priority INTEGER NOT NULL UNIQUE);"
        "CREATE TABLE IF NOT EXISTS weekly_scans(gps_week INTEGER PRIMARY KEY,status TEXT NOT NULL CHECK(status IN('complete','partial')),source_success_count INTEGER NOT NULL,source_expected_count INTEGER NOT NULL,product_count INTEGER NOT NULL,is_provisional INTEGER NOT NULL,scanned_at_utc TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS weekly_source_scans(gps_week INTEGER NOT NULL,source_id INTEGER NOT NULL,status TEXT NOT NULL CHECK(status IN('complete','failed')),listing_urls TEXT NOT NULL,file_count INTEGER NOT NULL,error_message TEXT NOT NULL,PRIMARY KEY(gps_week,source_id),FOREIGN KEY(gps_week) REFERENCES weekly_scans(gps_week) ON DELETE CASCADE,FOREIGN KEY(source_id) REFERENCES source_definitions(source_id));"
        "CREATE TABLE IF NOT EXISTS products(logical_key TEXT PRIMARY KEY,gps_week INTEGER NOT NULL,nominal_date TEXT NOT NULL,coverage_start TEXT NOT NULL,coverage_end TEXT NOT NULL,start_hour INTEGER NOT NULL,start_minute INTEGER NOT NULL,series TEXT NOT NULL,center TEXT NOT NULL,project TEXT NOT NULL,solution TEXT NOT NULL,latency TEXT NOT NULL,duration TEXT NOT NULL,sample_interval TEXT NOT NULL,product_family TEXT NOT NULL,standard TEXT NOT NULL,FOREIGN KEY(gps_week) REFERENCES weekly_scans(gps_week) ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS products_coverage ON products(coverage_start,coverage_end,latency,center,series);"
        "CREATE TABLE IF NOT EXISTS product_sources(logical_key TEXT NOT NULL,source_id INTEGER NOT NULL,file_name TEXT NOT NULL,download_url TEXT NOT NULL,archive_class TEXT NOT NULL,PRIMARY KEY(logical_key,source_id,file_name,download_url),FOREIGN KEY(logical_key) REFERENCES products(logical_key) ON DELETE CASCADE,FOREIGN KEY(source_id) REFERENCES source_definitions(source_id));"
        "CREATE TABLE IF NOT EXISTS eop_series(logical_key TEXT PRIMARY KEY,series TEXT NOT NULL,coverage_start TEXT NOT NULL,coverage_end TEXT NOT NULL,latency TEXT NOT NULL,sample_interval TEXT NOT NULL,standard TEXT NOT NULL,notes TEXT NOT NULL,refreshed_at_utc TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE INDEX IF NOT EXISTS eop_series_coverage ON eop_series(coverage_start,coverage_end,latency,series);"
        "CREATE TABLE IF NOT EXISTS eop_series_sources(logical_key TEXT NOT NULL,source_key TEXT NOT NULL,source_priority INTEGER NOT NULL,file_name TEXT NOT NULL,download_url TEXT NOT NULL,PRIMARY KEY(logical_key,source_key,file_name,download_url),FOREIGN KEY(logical_key) REFERENCES eop_series(logical_key) ON DELETE CASCADE);"
        "INSERT OR IGNORE INTO source_definitions VALUES"
        "(1,'CDDIS',1),(2,'BKG',2),(3,'WHU',3),(4,'KASI',4),(5,'IGN',5),(6,'ESA',6),(7,'SOPAC',7);"
        // Upgrade legacy 7-source weeks in place.  If KASI had succeeded, its
        // contribution is subtracted from the active-source count; valid old
        // KASI routes remain available as historical mirrors.
        "UPDATE weekly_scans SET source_success_count=source_success_count-CASE WHEN EXISTS(SELECT 1 FROM weekly_source_scans w JOIN source_definitions d ON d.source_id=w.source_id WHERE w.gps_week=weekly_scans.gps_week AND d.source_key='KASI' AND w.status='complete') THEN 1 ELSE 0 END,source_expected_count=6 WHERE source_expected_count=7;"
        "UPDATE weekly_scans SET status=CASE WHEN source_success_count>=source_expected_count THEN 'complete' ELSE 'partial' END WHERE source_expected_count=6;"
        "INSERT INTO metadata VALUES('schema_version','2') ON CONFLICT(key) DO UPDATE SET value='2';"
        "INSERT INTO metadata VALUES('product_type','erp_eop') ON CONFLICT(key) DO UPDATE SET value='erp_eop';";
    return Execute(raw, schema, error);
}

bool OpenReadDatabase(const std::filesystem::path& path, DbPtr& database,
                      std::wstring& error) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    sqlite3* raw = nullptr;
    const int status = sqlite3_open_v2(
        path.string().c_str(), &raw,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    database.reset(raw);
    if (status != SQLITE_OK || raw == nullptr) {
        error = DbError(raw, L"无法以只读方式打开 ERP/EOP 知识库");
        return false;
    }
    sqlite3_busy_timeout(raw, 10000);
    return true;
}

bool ProvisionalWeek(const int gpsWeek) {
    const auto today = std::chrono::floor<std::chrono::days>(
        std::chrono::system_clock::now());
    return ToSysDays(GpsWeekStartDate(gpsWeek))
        >= today - std::chrono::days{35};
}

QueryDate DateFromMjd(const double mjd) {
    const auto unixEpoch = std::chrono::sys_days{
        std::chrono::year{1970} / std::chrono::January / 1};
    return FromSysDays(unixEpoch
        + std::chrono::days{static_cast<long long>(std::floor(mjd - 40587.0))});
}

bool CoverageFromIersBody(const std::string& body, QueryDate& first,
                          QueryDate& last) {
    static const std::regex mjdPattern(
        R"((?:^|\s)([3-9][0-9]{4}(?:\.[0-9]+)?)(?=\s|$))");
    double minimum = 1000000.0;
    double maximum = -1.0;
    for (std::sregex_iterator it(body.begin(), body.end(), mjdPattern), end;
         it != end; ++it) {
        try {
            const double value = std::stod((*it)[1].str());
            if (value < 30000.0 || value > 100000.0) continue;
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        } catch (...) {
        }
    }
    if (maximum < minimum) return false;
    first = DateFromMjd(minimum);
    last = DateFromMjd(maximum);
    return true;
}

ErpProductRecord RecordFromColumns(sqlite3_stmt* statement,
                                   const QueryDate& requestedDate) {
    ErpProductRecord record;
    record.logicalKey = ColumnText(statement, 0);
    record.nominalDate = MakeQueryDate(
        std::stoi(ColumnText(statement, 1).substr(0, 4)),
        std::stoi(ColumnText(statement, 1).substr(5, 2)),
        std::stoi(ColumnText(statement, 1).substr(8, 2)));
    const auto parseDate = [](const std::string& text) {
        return MakeQueryDate(std::stoi(text.substr(0, 4)),
            std::stoi(text.substr(5, 2)), std::stoi(text.substr(8, 2)));
    };
    record.coverageStart = parseDate(ColumnText(statement, 2));
    record.coverageEnd = parseDate(ColumnText(statement, 3));
    record.startHour = sqlite3_column_int(statement, 4);
    record.startMinute = sqlite3_column_int(statement, 5);
    record.series = ColumnText(statement, 6);
    record.center = ColumnText(statement, 7);
    record.project = ColumnText(statement, 8);
    record.solution = ColumnText(statement, 9);
    record.latency = ColumnText(statement, 10);
    record.duration = ColumnText(statement, 11);
    record.interval = ColumnText(statement, 12);
    record.productFamily = ColumnText(statement, 13);
    record.standard = ColumnText(statement, 14);
    (void)requestedDate;
    return record;
}

} // namespace

std::vector<ErpArchiveFile> ParseErpDirectoryListing(
    const std::string& listing, const int expectedGpsWeek,
    const std::wstring& directoryUrl, const std::string& archiveClass) {
    static const std::regex candidate(
        R"(([A-Za-z0-9][A-Za-z0-9_.-]*(?:_ERP\.ERP|\.erp)(?:\.(?:gz|Z))?)(?=$|[^A-Za-z0-9_.-]))",
        std::regex::icase);
    std::set<std::string> seen;
    std::vector<ErpArchiveFile> result;
    for (std::sregex_iterator it(listing.begin(), listing.end(), candidate), end;
         it != end; ++it) {
        std::string name = (*it)[1].str();
        if (!seen.insert(Lower(name)).second) continue;
        ErpArchiveFile file;
        if (!ParseLongName(name, file)
            && !ParseLegacyName(name, expectedGpsWeek,
                archiveClass, file)) continue;
        if (CalculateGpsWeek(file.nominalDate) != expectedGpsWeek) continue;
        file.fileName = std::move(name);
        file.downloadUrl = JoinUrl(directoryUrl, file.fileName);
        file.archiveClass = archiveClass;
        result.push_back(std::move(file));
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        if (left.logicalKey != right.logicalKey)
            return left.logicalKey < right.logicalKey;
        return left.downloadUrl < right.downloadUrl;
    });
    return result;
}

std::vector<ErpSourceScan> QueryErpWeek(
    const int gpsWeek, const std::stop_token stopToken) {
    std::vector<ErpSourceScan> scans;
    scans.reserve(kSources.size());
    for (const auto& source : kSources) {
        ErpSourceScan scan;
        scan.sourceKey = source.key;
        scan.sourcePriority = source.priority;
        const std::array<std::pair<std::wstring, std::string>, 5> directories{{
            {std::wstring(source.root) + std::format(L"{:04}", gpsWeek) + L"/", "OPS"},
            {std::wstring(source.root) + L"mgex/"
                + std::format(L"{:04}", gpsWeek) + L"/", "MGEX"},
            {std::wstring(source.root) + L"repro1/" + std::format(L"{:04}", gpsWeek) + L"/", "REPRO1"},
            {std::wstring(source.root) + L"repro2/" + std::format(L"{:04}", gpsWeek) + L"/", "REPRO2"},
            {std::wstring(source.root) + L"repro3/"
                + std::format(L"{:04}", gpsWeek) + L"/", "REPRO3"},
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
                auto files = ParseErpDirectoryListing(
                    response.body, gpsWeek, url, archiveClass);
                scan.files.insert(scan.files.end(),
                    std::make_move_iterator(files.begin()),
                    std::make_move_iterator(files.end()));
                continue;
            }
            if (MissingDirectory(response)) continue;
            fatal = true;
            scan.transportErrorCode = response.transportErrorCode;
            scan.networkFailure = IsNetworkFailure(response);
            scan.error = response.error.empty()
                ? std::format(L"服务器返回 HTTP {}", response.statusCode)
                : response.error;
            break;
        }
        scan.success = !fatal;
        std::ranges::sort(scan.files, [](const auto& left, const auto& right) {
            if (left.logicalKey != right.logicalKey)
                return left.logicalKey < right.logicalKey;
            return left.downloadUrl < right.downloadUrl;
        });
        scan.files.erase(std::unique(scan.files.begin(), scan.files.end(),
            [](const auto& left, const auto& right) {
                return left.downloadUrl == right.downloadUrl;
            }), scan.files.end());
        scans.push_back(std::move(scan));
    }
    return scans;
}

std::filesystem::path GetErpAvailabilityDatabasePath() {
    const auto explicitPath = EnvironmentPath(
        L"GNSS_DOWNLOADER_ERP_EOP_DB_PATH");
    if (!explicitPath.empty()) return explicitPath;
    const auto directory = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
    if (!directory.empty()) {
        return ResolveDatabaseFilePath(directory / L"erp_eop_availability.sqlite");
    }
    return ResolveDatabaseFilePath(GetIgsAvailabilityDatabasePath().parent_path()
        / L"erp_eop_availability.sqlite");
}

bool StoreErpWeek(const int gpsWeek,
                  const std::vector<ErpSourceScan>& sources,
                  std::wstring& error) {
    std::scoped_lock lock(gDatabaseMutex);
    const int successful = static_cast<int>(std::ranges::count_if(
        sources, [](const auto& source) { return source.success; }));
    if (successful == 0) {
        error = L"六个 ERP 数据中心均未读取成功";
        return false;
    }
    std::map<std::string, ErpArchiveFile> products;
    for (const auto& source : sources) if (source.success) {
        for (const auto& file : source.files)
            products.try_emplace(file.logicalKey, file);
    }
    DbPtr database;
    if (!OpenDatabase(GetErpAvailabilityDatabasePath(), database, error))
        return false;
    int existing = -1;
    int existingExpected = -1;
    {
        auto statement = Prepare(database.get(),
            "SELECT source_success_count,source_expected_count FROM weekly_scans WHERE gps_week=?1;",
            error);
        if (!statement) return false;
        sqlite3_bind_int(statement.get(), 1, gpsWeek);
        if (sqlite3_step(statement.get()) == SQLITE_ROW) {
            existing = sqlite3_column_int(statement.get(), 0);
            existingExpected = sqlite3_column_int(statement.get(), 1);
        }
    }
    if (existingExpected == kSourceCount
        && (existing > successful
            || (existing == successful && successful < kSourceCount)))
        return true;
    if (!Execute(database.get(), "BEGIN IMMEDIATE;", error)) return false;
    bool committed = false;
    const auto rollback = [&]() {
        if (!committed) {
            std::wstring ignored;
            Execute(database.get(), "ROLLBACK;", ignored);
        }
    };
    {
        auto statement = Prepare(database.get(),
            "DELETE FROM weekly_scans WHERE gps_week=?1;", error);
        if (!statement) { rollback(); return false; }
        sqlite3_bind_int(statement.get(), 1, gpsWeek);
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法替换 ERP 周记录");
            rollback();
            return false;
        }
    }
    {
        auto statement = Prepare(database.get(),
            "INSERT INTO weekly_scans VALUES(?1,?2,?3,?4,?5,?6,strftime('%Y-%m-%dT%H:%M:%SZ','now'));",
            error);
        if (!statement) { rollback(); return false; }
        sqlite3_bind_int(statement.get(), 1, gpsWeek);
        Bind(statement.get(), 2,
            successful == kSourceCount ? "complete" : "partial");
        sqlite3_bind_int(statement.get(), 3, successful);
        sqlite3_bind_int(statement.get(), 4, kSourceCount);
        sqlite3_bind_int64(statement.get(), 5,
            static_cast<sqlite3_int64>(products.size()));
        sqlite3_bind_int(statement.get(), 6,
            ProvisionalWeek(gpsWeek) ? 1 : 0);
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法写入 ERP 周记录");
            rollback();
            return false;
        }
    }
    auto sourceStatement = Prepare(database.get(),
        "INSERT INTO weekly_source_scans VALUES(?1,?2,?3,?4,?5,?6);",
        error);
    if (!sourceStatement) { rollback(); return false; }
    for (const auto& source : sources) {
        std::string urls;
        for (const auto& url : source.listingUrls) {
            if (!urls.empty()) urls.push_back('\n');
            urls += WideToUtf8(url);
        }
        sqlite3_reset(sourceStatement.get());
        sqlite3_clear_bindings(sourceStatement.get());
        sqlite3_bind_int(sourceStatement.get(), 1, gpsWeek);
        sqlite3_bind_int(sourceStatement.get(), 2, source.sourcePriority);
        Bind(sourceStatement.get(), 3,
            source.success ? "complete" : "failed");
        Bind(sourceStatement.get(), 4, urls);
        sqlite3_bind_int64(sourceStatement.get(), 5,
            static_cast<sqlite3_int64>(source.files.size()));
        Bind(sourceStatement.get(), 6, WideToUtf8(source.error));
        if (sqlite3_step(sourceStatement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法写入 ERP 来源状态");
            rollback();
            return false;
        }
    }
    auto productStatement = Prepare(database.get(),
        "INSERT INTO products VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16);",
        error);
    auto routeStatement = Prepare(database.get(),
        "INSERT OR IGNORE INTO product_sources VALUES(?1,?2,?3,?4,?5);",
        error);
    if (!productStatement || !routeStatement) {
        rollback();
        return false;
    }
    for (const auto& [logicalKey, file] : products) {
        sqlite3_reset(productStatement.get());
        sqlite3_clear_bindings(productStatement.get());
        Bind(productStatement.get(), 1, logicalKey);
        sqlite3_bind_int(productStatement.get(), 2, gpsWeek);
        Bind(productStatement.get(), 3, DateText(file.nominalDate));
        Bind(productStatement.get(), 4, DateText(file.coverageStart));
        Bind(productStatement.get(), 5, DateText(file.coverageEnd));
        sqlite3_bind_int(productStatement.get(), 6, file.startHour);
        sqlite3_bind_int(productStatement.get(), 7, file.startMinute);
        Bind(productStatement.get(), 8, file.series);
        Bind(productStatement.get(), 9, file.center);
        Bind(productStatement.get(), 10, file.project);
        Bind(productStatement.get(), 11, file.solution);
        Bind(productStatement.get(), 12, file.latency);
        Bind(productStatement.get(), 13, file.duration);
        Bind(productStatement.get(), 14, file.interval);
        Bind(productStatement.get(), 15, file.productFamily);
        Bind(productStatement.get(), 16, file.standard);
        if (sqlite3_step(productStatement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法写入 ERP 产品");
            rollback();
            return false;
        }
        for (const auto& source : sources) if (source.success) {
            for (const auto& route : source.files) {
                if (route.logicalKey != logicalKey) continue;
                sqlite3_reset(routeStatement.get());
                sqlite3_clear_bindings(routeStatement.get());
                Bind(routeStatement.get(), 1, logicalKey);
                sqlite3_bind_int(routeStatement.get(), 2,
                    source.sourcePriority);
                Bind(routeStatement.get(), 3, route.fileName);
                Bind(routeStatement.get(), 4,
                    WideToUtf8(route.downloadUrl));
                Bind(routeStatement.get(), 5, route.archiveClass);
                if (sqlite3_step(routeStatement.get()) != SQLITE_DONE) {
                    error = DbError(database.get(),
                        L"无法写入 ERP 下载来源");
                    rollback();
                    return false;
                }
            }
        }
    }
    if (!Execute(database.get(), "COMMIT;", error)) {
        rollback();
        return false;
    }
    committed = true;
    return true;
}

std::vector<IersEopScan> QueryIersEopSeries(
    const std::stop_token stopToken) {
    std::vector<IersEopScan> scans(kIersSeries.size());
    std::atomic<std::size_t> next{0};
    const auto worker = [&]() {
        while (!stopToken.stop_requested()) {
            const std::size_t index = next.fetch_add(1);
            if (index >= kIersSeries.size()) return;
            const auto definition = kIersSeries[index];
            IersEopScan scan;
            scan.series = definition.series;
            scan.fileName = WideToUtf8(definition.fileName);
            scan.downloadUrl = std::wstring(kIersRoot)
                + definition.fileName;
            scan.latency = definition.latency;
            scan.interval = definition.interval;
            scan.standard = definition.standard;
            scan.notes = definition.notes;
            const auto response = HttpGetText(scan.downloadUrl,
                stopToken, 64U * 1024U * 1024U);
            if (!response.ok()) {
                scan.transportErrorCode = response.transportErrorCode;
                scan.networkFailure = IsNetworkFailure(response);
                scan.error = response.error.empty()
                    ? std::format(L"服务器返回 HTTP {}",
                        response.statusCode)
                    : response.error;
                scans[index] = std::move(scan);
                continue;
            }
            if (!CoverageFromIersBody(response.body,
                    scan.coverageStart, scan.coverageEnd)) {
                scan.error = L"已下载，但没有识别出有效 MJD 日期范围";
                scans[index] = std::move(scan);
                continue;
            }
            scan.success = true;
            scans[index] = std::move(scan);
        }
    };
    // The IERS server/Windows Schannel combination can reject simultaneous
    // TLS handshakes with curl error 35.  These rolling files are refreshed
    // only once per week, so reliability is more important than parallelism.
    {
        std::array<std::jthread, 1> workers{std::jthread(worker)};
    }
    return scans;
}

bool StoreIersEopSeries(const std::vector<IersEopScan>& scans,
                        std::wstring& error) {
    std::scoped_lock lock(gDatabaseMutex);
    if (std::ranges::none_of(scans,
            [](const auto& scan) { return scan.success; })) {
        error = L"IERS EOP 序列均未读取成功";
        return false;
    }
    DbPtr database;
    if (!OpenDatabase(GetErpAvailabilityDatabasePath(), database, error))
        return false;
    if (!Execute(database.get(), "BEGIN IMMEDIATE;", error)) return false;
    bool committed = false;
    const auto rollback = [&]() {
        if (!committed) {
            std::wstring ignored;
            Execute(database.get(), "ROLLBACK;", ignored);
        }
    };
    auto deleteStatement = Prepare(database.get(),
        "DELETE FROM eop_series WHERE logical_key=?1;", error);
    auto seriesStatement = Prepare(database.get(),
        "INSERT INTO eop_series VALUES(?1,?2,?3,?4,?5,?6,?7,?8,strftime('%Y-%m-%dT%H:%M:%SZ','now'));",
        error);
    auto sourceStatement = Prepare(database.get(),
        "INSERT INTO eop_series_sources VALUES(?1,'IERS',1,?2,?3);",
        error);
    if (!deleteStatement || !seriesStatement || !sourceStatement) {
        rollback();
        return false;
    }
    for (const auto& scan : scans) {
        if (!scan.success) continue;
        const std::string logicalKey = "EOP|" + scan.series;
        sqlite3_reset(deleteStatement.get());
        sqlite3_clear_bindings(deleteStatement.get());
        Bind(deleteStatement.get(), 1, logicalKey);
        if (sqlite3_step(deleteStatement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法替换 IERS EOP 序列");
            rollback();
            return false;
        }
        sqlite3_reset(seriesStatement.get());
        sqlite3_clear_bindings(seriesStatement.get());
        Bind(seriesStatement.get(), 1, logicalKey);
        Bind(seriesStatement.get(), 2, scan.series);
        Bind(seriesStatement.get(), 3, DateText(scan.coverageStart));
        Bind(seriesStatement.get(), 4, DateText(scan.coverageEnd));
        Bind(seriesStatement.get(), 5, scan.latency);
        Bind(seriesStatement.get(), 6, scan.interval);
        Bind(seriesStatement.get(), 7, scan.standard);
        Bind(seriesStatement.get(), 8, scan.notes);
        if (sqlite3_step(seriesStatement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法写入 IERS EOP 序列");
            rollback();
            return false;
        }
        sqlite3_reset(sourceStatement.get());
        sqlite3_clear_bindings(sourceStatement.get());
        Bind(sourceStatement.get(), 1, logicalKey);
        Bind(sourceStatement.get(), 2, scan.fileName);
        Bind(sourceStatement.get(), 3, WideToUtf8(scan.downloadUrl));
        if (sqlite3_step(sourceStatement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法写入 IERS EOP 下载来源");
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

bool IersEopSeriesNeedRefresh(bool& needRefresh, std::wstring& error) {
    std::scoped_lock lock(gDatabaseMutex);
    needRefresh = true;
    DbPtr database;
    if (!OpenDatabase(GetErpAvailabilityDatabasePath(), database, error))
        return false;
    auto statement = Prepare(database.get(),
        "SELECT COUNT(*),COALESCE(MAX(refreshed_at_utc),''),"
        "CASE WHEN MAX(refreshed_at_utc) IS NULL THEN 1 "
        "ELSE julianday('now')-julianday(MAX(refreshed_at_utc))>7 END "
        "FROM eop_series;", error);
    if (!statement) return false;
    if (sqlite3_step(statement.get()) == SQLITE_ROW) {
        const int count = sqlite3_column_int(statement.get(), 0);
        const bool stale = sqlite3_column_int(statement.get(), 2) != 0;
        needRefresh = count < static_cast<int>(kIersSeries.size()) || stale;
    }
    return true;
}

ErpAvailabilityLookup LoadErpAvailability(const QueryDate& date) {
    std::scoped_lock lock(gDatabaseMutex);
    ErpAvailabilityLookup result;
    result.gpsWeek = CalculateGpsWeek(date);
    result.sourcesExpected = kSourceCount;
    result.databasePath = GetErpAvailabilityDatabasePath();
    DbPtr database;
    if (!OpenReadDatabase(result.databasePath, database, result.error))
        return result;
    {
        auto statement = Prepare(database.get(),
            "SELECT status,source_success_count,source_expected_count,is_provisional FROM weekly_scans WHERE gps_week=?1;",
            result.error);
        if (!statement) return result;
        sqlite3_bind_int(statement.get(), 1, result.gpsWeek);
        if (sqlite3_step(statement.get()) == SQLITE_ROW) {
            result.indexed = true;
            result.sourcesSuccessful = sqlite3_column_int(statement.get(), 1);
            result.sourcesExpected = sqlite3_column_int(statement.get(), 2);
            // Provisional weekly rows are not download-ready, even when all
            // configured sources responded.  This mirrors the range reader
            // and IsErpWeekComplete contract.
            result.complete = ColumnText(statement.get(), 0) == "complete"
                && result.sourcesExpected == kSourceCount
                && sqlite3_column_int(statement.get(), 3) == 0;
            result.provisional = sqlite3_column_int(statement.get(), 3) != 0;
        }
    }
    const std::string requested = DateText(date);
    auto products = Prepare(database.get(),
        "SELECT logical_key,nominal_date,coverage_start,coverage_end,start_hour,start_minute,series,center,project,solution,latency,duration,sample_interval,product_family,standard FROM products WHERE coverage_start<=?1 AND coverage_end>=?1 ORDER BY latency,center,series,start_hour,start_minute;",
        result.error);
    if (!products || !Bind(products.get(), 1, requested)) return result;
    auto routes = Prepare(database.get(),
        "SELECT d.source_key,d.source_priority,s.file_name,s.download_url,s.archive_class FROM product_sources s JOIN source_definitions d ON d.source_id=s.source_id WHERE s.logical_key=?1 ORDER BY d.source_priority,s.file_name;",
        result.error);
    if (!routes) return result;
    while (sqlite3_step(products.get()) == SQLITE_ROW) {
        auto record = RecordFromColumns(products.get(), date);
        sqlite3_reset(routes.get());
        sqlite3_clear_bindings(routes.get());
        if (!Bind(routes.get(), 1, record.logicalKey)) {
            result.products.clear();
            return result;
        }
        while (sqlite3_step(routes.get()) == SQLITE_ROW) {
            record.routes.push_back({ColumnText(routes.get(), 0),
                sqlite3_column_int(routes.get(), 1),
                ColumnText(routes.get(), 2),
                Utf8ToWide(ColumnText(routes.get(), 3)),
                ColumnText(routes.get(), 4)});
        }
        result.products.push_back(std::move(record));
    }
    auto eop = Prepare(database.get(),
        "SELECT logical_key,series,coverage_start,coverage_end,latency,sample_interval,standard,notes FROM eop_series WHERE coverage_start<=?1 AND coverage_end>=?1 ORDER BY latency,series;",
        result.error);
    if (!eop || !Bind(eop.get(), 1, requested)) return result;
    const auto parseDate = [](const std::string& text) {
        return MakeQueryDate(std::stoi(text.substr(0, 4)),
            std::stoi(text.substr(5, 2)), std::stoi(text.substr(8, 2)));
    };
    auto eopRoutes = Prepare(database.get(),
        "SELECT source_key,source_priority,file_name,download_url FROM eop_series_sources WHERE logical_key=?1 ORDER BY source_priority,file_name;",
        result.error);
    if (!eopRoutes) return result;
    while (sqlite3_step(eop.get()) == SQLITE_ROW) {
        ErpProductRecord record;
        record.logicalKey = ColumnText(eop.get(), 0);
        record.series = ColumnText(eop.get(), 1);
        record.coverageStart = parseDate(ColumnText(eop.get(), 2));
        record.coverageEnd = parseDate(ColumnText(eop.get(), 3));
        record.nominalDate = record.coverageStart;
        record.center = "IERS";
        record.project = "EOP";
        record.solution = "SERIES";
        record.latency = ColumnText(eop.get(), 4);
        record.duration = "00U";
        record.interval = ColumnText(eop.get(), 5);
        record.productFamily = "EOP";
        record.standard = ColumnText(eop.get(), 6);
        sqlite3_reset(eopRoutes.get());
        sqlite3_clear_bindings(eopRoutes.get());
        if (!Bind(eopRoutes.get(), 1, record.logicalKey)) {
            result.products.clear();
            return result;
        }
        while (sqlite3_step(eopRoutes.get()) == SQLITE_ROW) {
            record.routes.push_back({ColumnText(eopRoutes.get(), 0),
                sqlite3_column_int(eopRoutes.get(), 1),
                ColumnText(eopRoutes.get(), 2),
                Utf8ToWide(ColumnText(eopRoutes.get(), 3)), "IERS"});
        }
        result.products.push_back(std::move(record));
    }
    if (!result.products.empty()) result.indexed = true;
    return result;
}

ErpAvailabilityRangeLookup LoadErpAvailabilityRange(
    const QueryDate& startDate, const QueryDate& endDate) {
    std::scoped_lock lock(gDatabaseMutex);
    ErpAvailabilityRangeLookup result;
    result.databasePath = GetErpAvailabilityDatabasePath();
    result.expectedDays = InclusiveDays(startDate, endDate);
    if (result.expectedDays <= 0) {
        result.error = L"ERP 日期范围无效";
        return result;
    }
    DbPtr database;
    if (!OpenReadDatabase(result.databasePath, database, result.error)) return result;
    result.indexed = true;
    const int firstWeek = CalculateGpsWeek(startDate);
    const int lastWeek = CalculateGpsWeek(endDate);
    std::unordered_map<int, bool> completeWeeks;
    auto scans = Prepare(database.get(),
        "SELECT gps_week,status,is_provisional,source_expected_count FROM weekly_scans WHERE gps_week>=?1 AND gps_week<=?2;",
        result.error);
    if (!scans) return result;
    sqlite3_bind_int(scans.get(), 1, firstWeek);
    sqlite3_bind_int(scans.get(), 2, lastWeek);
    while (sqlite3_step(scans.get()) == SQLITE_ROW) {
        const int week = sqlite3_column_int(scans.get(), 0);
        completeWeeks[week] = ColumnText(scans.get(), 1) == "complete"
            && sqlite3_column_int(scans.get(), 2) == 0
            && sqlite3_column_int(scans.get(), 3) == kSourceCount;
    }
    for (int offset = 0; offset < result.expectedDays; ++offset) {
        const auto day = FromSysDays(ToSysDays(startDate) + std::chrono::days{offset});
        const auto it = completeWeeks.find(CalculateGpsWeek(day));
        if (it != completeWeeks.end() && it->second) ++result.indexedDays;
        else result.incompleteDates.push_back(day);
    }
    const auto first = DateText(startDate);
    const auto last = DateText(endDate);
    auto products = Prepare(database.get(),
        "SELECT logical_key,nominal_date,coverage_start,coverage_end,start_hour,start_minute,series,center,project,solution,latency,duration,sample_interval,product_family,standard FROM products WHERE coverage_end>=?1 AND coverage_start<=?2 ORDER BY coverage_start,latency,center,series,start_hour,start_minute;",
        result.error);
    if (!products || !Bind(products.get(), 1, first) || !Bind(products.get(), 2, last)) return result;
    auto routes = Prepare(database.get(),
        "SELECT d.source_key,d.source_priority,s.file_name,s.download_url,s.archive_class FROM product_sources s JOIN source_definitions d ON d.source_id=s.source_id WHERE s.logical_key=?1 ORDER BY d.source_priority,s.file_name;",
        result.error);
    if (!routes) return result;
    while (sqlite3_step(products.get()) == SQLITE_ROW) {
        auto record = RecordFromColumns(products.get(), startDate);
        sqlite3_reset(routes.get());
        sqlite3_clear_bindings(routes.get());
        if (!Bind(routes.get(), 1, record.logicalKey)) { result.products.clear(); return result; }
        while (sqlite3_step(routes.get()) == SQLITE_ROW) {
            record.routes.push_back({ColumnText(routes.get(), 0), sqlite3_column_int(routes.get(), 1),
                ColumnText(routes.get(), 2), Utf8ToWide(ColumnText(routes.get(), 3)), ColumnText(routes.get(), 4)});
        }
        result.products.push_back(std::move(record));
    }
    // IERS EOP series live in the same database but are not tied to GPS weeks;
    // include any series whose validity intersects the requested interval.
    auto eop = Prepare(database.get(),
        "SELECT logical_key,series,coverage_start,coverage_end,latency,sample_interval,standard,notes FROM eop_series WHERE coverage_end>=?1 AND coverage_start<=?2 ORDER BY latency,series;",
        result.error);
    if (eop && Bind(eop.get(), 1, first) && Bind(eop.get(), 2, last)) {
        auto eopRoutes = Prepare(database.get(),
            "SELECT source_key,source_priority,file_name,download_url FROM eop_series_sources WHERE logical_key=?1 ORDER BY source_priority,file_name;",
            result.error);
        if (!eopRoutes) return result;
        while (sqlite3_step(eop.get()) == SQLITE_ROW) {
            ErpProductRecord record;
            record.logicalKey = ColumnText(eop.get(), 0); record.series = ColumnText(eop.get(), 1);
            record.coverageStart = ParseDateText(ColumnText(eop.get(), 2)); record.coverageEnd = ParseDateText(ColumnText(eop.get(), 3));
            record.nominalDate = record.coverageStart; record.center = "IERS"; record.project = "EOP"; record.solution = "SERIES"; record.latency = ColumnText(eop.get(), 4); record.duration = "00U"; record.interval = ColumnText(eop.get(), 5); record.productFamily = "EOP"; record.standard = ColumnText(eop.get(), 6);
            sqlite3_reset(eopRoutes.get()); sqlite3_clear_bindings(eopRoutes.get()); Bind(eopRoutes.get(), 1, record.logicalKey);
            while (sqlite3_step(eopRoutes.get()) == SQLITE_ROW) record.routes.push_back({ColumnText(eopRoutes.get(),0), sqlite3_column_int(eopRoutes.get(),1), ColumnText(eopRoutes.get(),2), Utf8ToWide(ColumnText(eopRoutes.get(),3)), "IERS"});
            result.products.push_back(std::move(record));
        }
    }
    result.complete = result.indexedDays == result.expectedDays && result.incompleteDates.empty();
    return result;
}

bool IsErpWeekComplete(const int gpsWeek, bool& complete,
                       std::wstring& error) {
    std::scoped_lock lock(gDatabaseMutex);
    complete = false;
    DbPtr database;
    if (!OpenDatabase(GetErpAvailabilityDatabasePath(), database, error))
        return false;
    auto statement = Prepare(database.get(),
        "SELECT status,is_provisional,source_expected_count FROM weekly_scans WHERE gps_week=?1;",
        error);
    if (!statement) return false;
    sqlite3_bind_int(statement.get(), 1, gpsWeek);
    if (sqlite3_step(statement.get()) == SQLITE_ROW) {
        complete = ColumnText(statement.get(), 0) == "complete"
            && sqlite3_column_int(statement.get(), 1) == 0
            && sqlite3_column_int(statement.get(), 2) == kSourceCount;
    }
    return true;
}

ErpAvailabilityStats GetErpAvailabilityStats() {
    std::scoped_lock lock(gDatabaseMutex);
    ErpAvailabilityStats result;
    result.databasePath = GetErpAvailabilityDatabasePath();
    DbPtr database;
    if (!OpenDatabase(result.databasePath, database, result.error))
        return result;
    auto statement = Prepare(database.get(),
        "SELECT SUM(status='complete'),SUM(status='partial'),MIN(gps_week),MAX(gps_week) FROM weekly_scans;",
        result.error);
    if (statement && sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.completeWeeks = sqlite3_column_int(statement.get(), 0);
        result.partialWeeks = sqlite3_column_int(statement.get(), 1);
        result.firstGpsWeek = sqlite3_column_type(statement.get(), 2)
            == SQLITE_NULL ? -1 : sqlite3_column_int(statement.get(), 2);
        result.lastGpsWeek = sqlite3_column_type(statement.get(), 3)
            == SQLITE_NULL ? -1 : sqlite3_column_int(statement.get(), 3);
    }
    auto counts = Prepare(database.get(),
        "SELECT (SELECT COUNT(*) FROM products),"
        "(SELECT COUNT(*) FROM product_sources),"
        "(SELECT COUNT(*) FROM eop_series);", result.error);
    if (counts && sqlite3_step(counts.get()) == SQLITE_ROW) {
        result.products = sqlite3_column_int64(counts.get(), 0);
        result.sourceRoutes = sqlite3_column_int64(counts.get(), 1);
        result.iersSeries = sqlite3_column_int(counts.get(), 2);
    }
    return result;
}

int ErpExpectedSourceCount() {
    return kSourceCount;
}

int IersExpectedSeriesCount() {
    return static_cast<int>(kIersSeries.size());
}
