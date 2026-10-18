#include "IonexAvailability.h"

#include "DateUtil.h"
#include "HttpClient.h"
#include "IgsAvailabilityDb.h"

#include "sqlite3.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <format>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <string_view>
#include <unordered_map>

namespace {

struct SourceDefinition {
    const char* key;
    int priority;
    const wchar_t* root;
    bool ftp;
    // Some mirrors publish a center's own legacy products in a separate
    // annual tree.  The alternate route is folded into the same logical
    // archive source so source completeness does not count WHU twice.
    const wchar_t* alternateRoot = nullptr;
};

// These are independent archive routes, not product producers.  A CODE, UPC or
// JPL GIM mirrored by two archives remains one logical product with two routes.
constexpr std::array kSources{
    SourceDefinition{"CDDIS", 1,
        L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/products/ionex/", true},
    SourceDefinition{"BKG", 2,
        L"https://igs.bkg.bund.de/root_ftp/IGS/IONEX/", false},
    SourceDefinition{"IGN", 3,
        L"ftp://igs.ign.fr/pub/igs/products/ionosphere/", true},
    SourceDefinition{"WHU", 4,
        L"ftp://igs.gnsswhu.cn/pub/gps/products/ionex/", true,
        L"ftp://igs.gnsswhu.cn/pub/whu/MGEX/ionosphere/"},
    SourceDefinition{"CAS", 5,
        L"https://data.bdsmart.cn/pub/product/iono/ionex/", false},
};
constexpr int kSourceCount = static_cast<int>(kSources.size());

struct DbCloser { void operator()(sqlite3* value) const { if (value) sqlite3_close(value); } };
struct StmtCloser { void operator()(sqlite3_stmt* value) const { if (value) sqlite3_finalize(value); } };
using DbPtr = std::unique_ptr<sqlite3, DbCloser>;
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtCloser>;

std::mutex gDatabaseMutex;

struct IonexFamilyDayRef {
    std::chrono::sys_days day{};
    QueryDate date{};
};

struct IonexProductFamilyCacheEntry {
    std::string familyKey;
    IonexProductRecord representative;
    std::vector<IonexFamilyDayRef> days;
};

struct IonexScanDayRef {
    std::chrono::sys_days day{};
    QueryDate date{};
    bool complete = false;
};

struct IonexFileStamp {
    std::uintmax_t size = 0;
    std::uintmax_t walSize = 0;
    std::filesystem::file_time_type modified{};
    std::filesystem::file_time_type walModified{};
    bool exists = false;
    bool walExists = false;
    bool operator==(const IonexFileStamp&) const = default;
};

struct IonexProductFamilyCache {
    std::filesystem::path databasePath;
    IonexFileStamp stamp;
    std::vector<IonexProductFamilyCacheEntry> families;
    std::unordered_map<std::string, std::size_t> familyIndex;
    std::vector<IonexScanDayRef> scans;
    std::wstring error;
};

std::mutex gIonexFamilyCacheMutex;
std::unordered_map<std::wstring, std::shared_ptr<IonexProductFamilyCache>>
    gIonexFamilyCaches;

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

std::string Upper(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

std::string Lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::chrono::sys_days ToSysDays(const QueryDate& date) {
    return std::chrono::sys_days{std::chrono::year{date.year}
        / std::chrono::month{static_cast<unsigned>(date.month)}
        / std::chrono::day{static_cast<unsigned>(date.day)}};
}

QueryDate FromSysDays(const std::chrono::sys_days value) {
    const std::chrono::year_month_day date{value};
    return MakeQueryDate(static_cast<int>(date.year()),
        static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()));
}

QueryDate DateFromDoy(const int year, const int doy) {
    return FromSysDays(std::chrono::sys_days{
        std::chrono::year{year} / std::chrono::January / 1}
        + std::chrono::days{doy - 1});
}

std::string DateText(const QueryDate& date) {
    return std::format("{:04}-{:02}-{:02}", date.year, date.month, date.day);
}

bool SameDate(const QueryDate& left, const QueryDate& right) {
    return left.year == right.year && left.month == right.month
        && left.day == right.day;
}

std::wstring JoinUrl(const std::wstring& directory, const std::string& file) {
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

std::string LatencyFor(const std::string& project, const std::string& solution) {
    const std::string p = Upper(project);
    const std::string s = Upper(solution);
    if (p.starts_with("R0")) return "REPROCESSING";
    if (s == "FIN") return "FINAL";
    if (s == "RAP") return "RAPID";
    if (s == "ULT") return "ULTRA_RAPID";
    if (s == "NRT") return "NEAR_REAL_TIME";
    if (s == "RTS") return "REAL_TIME";
    if (s == "P0D" || s == "P1D" || s == "P2D"
        || s == "1DP" || s == "2DP") return "PREDICTED";
    return "OTHER";
}

std::string LogicalKey(const IonexArchiveFile& file) {
    return file.series + "|" + DateText(file.nominalDate) + "|"
        + std::format("{:02}{:02}|{}|{}|{}", file.startHour,
            file.startMinute, file.duration, file.interval, file.contentType);
}

void FillLegacyDefinition(IonexArchiveFile& file, const std::string& prefix) {
    const std::string code = Lower(prefix);
    const bool oneHourCode = file.nominalDate.year >= 2014;
    file.duration = "01D";
    file.interval = "02H";
    file.contentType = "GIM";
    file.originalContent = "GIM";
    file.format = "IONEX_LEGACY";
    file.center = Upper(prefix.substr(0, 3));
    file.project = "OPS";
    file.solution = "FIN";
    file.latency = "FINAL";
    file.series = Upper(prefix);

    if (code == "igsg") { file.series = "IGS0OPSFIN"; file.center = "IGS"; }
    else if (code == "igrg") { file.series = "IGS0OPSRAP"; file.center = "IGS"; file.solution = "RAP"; file.latency = "RAPID"; }
    else if (code == "codg") { file.series = "COD0OPSFIN"; file.center = "COD"; file.interval = oneHourCode ? "01H" : "02H"; }
    else if (code == "corg") { file.series = "COD0OPSRAP"; file.center = "COD"; file.solution = "RAP"; file.latency = "RAPID"; file.interval = oneHourCode ? "01H" : "02H"; }
    else if (code == "c1pg") { file.series = "COD0OPS1DP"; file.center = "COD"; file.solution = "1DP"; file.latency = "PREDICTED"; file.interval = oneHourCode ? "01H" : "02H"; }
    else if (code == "c2pg") { file.series = "COD0OPS2DP"; file.center = "COD"; file.solution = "2DP"; file.latency = "PREDICTED"; file.interval = oneHourCode ? "01H" : "02H"; }
    else if (code == "esag") { file.series = "ESA0OPSFIN"; file.center = "ESA"; file.project = "OPS"; file.solution = "FIN"; file.latency = "FINAL"; }
    else if (code == "ehrg" || code == "esrg") { file.series = "ESA0OPSRAP"; file.center = "ESA"; file.project = "OPS"; file.solution = "RAP"; file.latency = "RAPID"; file.interval = "01H"; }
    else if (code == "jplg") { file.series = "JPL0OPSFIN"; file.center = "JPL"; }
    else if (code == "jprg") { file.series = "JPL0OPSRAP"; file.center = "JPL"; file.solution = "RAP"; file.latency = "RAPID"; }
    else if (code == "upcg") { file.series = "UPC0OPSFIN"; file.center = "UPC"; }
    else if (code == "uprg") { file.series = "UPC0OPSRAP"; file.center = "UPC"; file.solution = "RAP"; file.latency = "RAPID"; }
    else if (code == "uqrg") { file.series = "UPC0OPSR15"; file.center = "UPC"; file.solution = "RAP"; file.latency = "RAPID"; file.interval = "15M"; }
    else if (code == "uhrg") { file.series = "UPC0OPSR01"; file.center = "UPC"; file.solution = "RAP"; file.latency = "RAPID"; file.interval = "01H"; }
    else if (code == "u2pg") { file.series = "UPC0OPS2DP"; file.center = "UPC"; file.solution = "2DP"; file.latency = "PREDICTED"; }
    else if (code == "urtg") { file.series = "UPC0OPSRTS"; file.center = "UPC"; file.solution = "RTS"; file.latency = "REAL_TIME"; }
    else if (code == "irtg") { file.series = "IGS0OPSRTS"; file.center = "IGS"; file.project = "OPS"; file.solution = "RTS"; file.latency = "REAL_TIME"; file.interval = "20M"; }
    else if (code == "crtg") { file.series = "CAS0OPSRTS"; file.center = "CAS"; file.project = "OPS"; file.solution = "RTS"; file.latency = "REAL_TIME"; file.interval = "05M"; }
    else if (code == "wrtg") { file.series = "WHU0OPSRTS"; file.center = "WHU"; file.project = "OPS"; file.solution = "RTS"; file.latency = "REAL_TIME"; file.interval = "05M"; }
    else if (code == "casg") { file.series = "CAS0OPSFIN"; file.center = "CAS"; }
    else if (code == "carg") { file.series = "CAS0OPSRAP"; file.center = "CAS"; file.solution = "RAP"; file.latency = "RAPID"; }
    else if (code == "whug") { file.series = "WHU0OPSFIN"; file.center = "WHU"; }
    else if (code == "whrg") { file.series = "WHU0OPSRAP"; file.center = "WHU"; file.solution = "RAP"; file.latency = "RAPID"; }
    else if (code == "emrg") { file.series = "EMR0OPSRAP"; file.center = "EMR"; file.solution = "RAP"; file.latency = "RAPID"; file.interval = "01H"; }
    else if (code == "othg") { file.series = "OPT0OPSFIN"; file.center = "OPT"; file.interval = "10M"; }
    else if (code == "otlg") { file.series = "OPT0OPSOTL"; file.center = "OPT"; file.interval = "10M"; }
    else if (code == "roti") {
        file.series = "IGS0OPSFIN";
        file.center = "IGS";
        file.contentType = "ROT";
        file.originalContent = "ROT";
        file.interval = "01D";
    }
    static const std::set<std::string> known{
        "igsg", "igrg", "codg", "corg", "c1pg", "c2pg", "esag",
        "ehrg", "esrg", "jplg", "jprg", "upcg", "uprg", "uqrg",
        "uhrg", "u2pg", "urtg", "irtg", "crtg", "wrtg", "casg",
        "carg", "whug", "whrg", "emrg", "othg", "otlg", "roti"};
    if (!known.contains(code)) {
        // Preserve unknown public IONEX series without pretending it is final.
        file.series = Upper(prefix);
        file.project = "LEG";
        file.solution = "UNK";
        file.latency = "OTHER";
    }
}

bool ParseLongName(
    const std::string& name, const QueryDate& expected, IonexArchiveFile& file) {
    static const std::regex pattern(
        R"(^([A-Z0-9]{10})_([0-9]{4})([0-9]{3})([0-9]{2})([0-9]{2})_([0-9]{2}[DHMSUWLY])_([0-9]{2}[DHMSUWLY])_(GIM|ION|ROT)\.(INX|IOX)(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) return false;
    const int year = std::stoi(match[2].str());
    const int doy = std::stoi(match[3].str());
    if (year != expected.year || doy != expected.dayOfYear
        || doy < 1 || doy > (IsLeapYear(year) ? 366 : 365)) return false;
    file.series = Upper(match[1].str());
    file.center = file.series.substr(0, 3);
    file.project = file.series.substr(4, 3);
    file.solution = file.series.substr(7, 3);
    file.latency = LatencyFor(file.project, file.solution);
    file.nominalDate = DateFromDoy(year, doy);
    file.startHour = std::stoi(match[4].str());
    file.startMinute = std::stoi(match[5].str());
    file.duration = Upper(match[6].str());
    file.interval = Upper(match[7].str());
    file.originalContent = Upper(match[8].str());
    file.contentType = file.originalContent == "ROT" ? "ROT" : "GIM";
    file.format = "IONEX_" + Upper(match[9].str());
    file.logicalKey = LogicalKey(file);
    return true;
}

bool ParseShortName(
    const std::string& name, const QueryDate& expected, IonexArchiveFile& file) {
    static const std::regex pattern(
        R"(^([A-Z0-9]{4})([0-9]{3})([0-9])\.([0-9]{2})([IF])(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) return false;
    if (std::stoi(match[2].str()) != expected.dayOfYear
        || std::stoi(match[4].str()) != expected.year % 100) return false;
    file.nominalDate = expected;
    FillLegacyDefinition(file, match[1].str());
    const std::string suffix = Upper(match[5].str());
    if (suffix == "F") {
        if (Lower(match[1].str()) != "roti") return false;
        file.contentType = "ROT";
        file.originalContent = "ROT";
        file.interval = "01D";
    } else if (file.contentType == "ROT") {
        return false;
    }
    file.logicalKey = LogicalKey(file);
    return true;
}

std::wstring DailyUrl(const SourceDefinition& source, const QueryDate& date) {
    return std::wstring(source.root) + std::to_wstring(date.year) + L"/"
        + std::format(L"{:03}/", date.dayOfYear);
}

std::wstring AlternateUrl(
    const SourceDefinition& source, const QueryDate& date) {
    return std::wstring(source.alternateRoot) + std::to_wstring(date.year)
        + L"/";
}

IonexSourceScan QuerySource(
    const SourceDefinition& source, const QueryDate& date,
    const std::stop_token stopToken) {
    IonexSourceScan scan;
    scan.sourceKey = source.key;
    scan.sourcePriority = source.priority;
    scan.listingUrl = DailyUrl(source, date);
    const auto readDirectory = [&](const std::wstring& url) {
        return source.ftp
            ? HttpListFtpDirectory(url, stopToken, 32U * 1024U * 1024U)
            : HttpGetText(url, stopToken, 32U * 1024U * 1024U);
    };
    const auto consume = [&](const std::wstring& url,
                             const HttpTextResult& response) {
        if (response.ok()) {
            auto files = ParseIonexDirectoryListing(response.body, date, url);
            scan.files.insert(scan.files.end(), files.begin(), files.end());
            scan.success = true;
            return;
        }
        if (MissingDirectory(response)) {
            // A source is considered reachable when it explicitly reports
            // that this date is not present; this is not a transport failure.
            scan.success = true;
            return;
        }
        scan.transportErrorCode = response.transportErrorCode;
        scan.networkFailure = scan.networkFailure
            || IsNetworkFailure(response);
        const std::wstring message = response.error.empty()
            ? std::format(L"服务器返回 HTTP {}", response.statusCode)
            : response.error;
        if (!scan.error.empty()) scan.error += L"；";
        scan.error += url + L"：" + message;
    };
    const auto primaryResponse = readDirectory(scan.listingUrl);
    consume(scan.listingUrl, primaryResponse);
    if (source.alternateRoot != nullptr) {
        const auto alternateUrl = AlternateUrl(source, date);
        const auto alternateResponse = readDirectory(alternateUrl);
        consume(alternateUrl, alternateResponse);
        if (alternateResponse.ok()) {
            scan.listingUrl += L";" + alternateUrl;
        }
    }
    std::ranges::sort(scan.files, [](const auto& left, const auto& right) {
        if (left.logicalKey != right.logicalKey)
            return left.logicalKey < right.logicalKey;
        return left.downloadUrl < right.downloadUrl;
    });
    scan.files.erase(std::unique(scan.files.begin(), scan.files.end(),
        [](const auto& left, const auto& right) {
            return left.logicalKey == right.logicalKey
                && left.downloadUrl == right.downloadUrl;
        }), scan.files.end());
    if (scan.success) {
        scan.error.clear();
        scan.networkFailure = false;
    }
    return scan;
}

std::wstring DbError(sqlite3* database, const wchar_t* action) {
    const char* raw = database ? sqlite3_errmsg(database) : "unknown SQLite error";
    return std::wstring(action) + L"：" + Utf8ToWide(raw ? raw : "");
}

bool Execute(sqlite3* database, const char* sql, std::wstring& error) {
    char* message = nullptr;
    if (sqlite3_exec(database, sql, nullptr, nullptr, &message) == SQLITE_OK)
        return true;
    error = L"SQLite：" + Utf8ToWide(message ? message : sqlite3_errmsg(database));
    sqlite3_free(message);
    return false;
}

StmtPtr Prepare(sqlite3* database, const char* sql, std::wstring& error) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK) {
        error = DbError(database, L"无法准备 IONEX 知识库查询");
        return {};
    }
    return StmtPtr(raw);
}

bool Bind(sqlite3_stmt* statement, const int index, const std::string& value) {
    return sqlite3_bind_text(statement, index, value.c_str(),
        static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

std::string ColumnText(sqlite3_stmt* statement, const int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char*>(value) : "";
}

bool OpenDatabase(
    const std::filesystem::path& path, DbPtr& database, std::wstring& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = L"无法创建 IONEX 知识库目录：" + path.parent_path().wstring();
        return false;
    }
    sqlite3* raw = nullptr;
    const int status = sqlite3_open16(path.c_str(), &raw);
    database.reset(raw);
    if (status != SQLITE_OK || raw == nullptr) {
        error = DbError(raw, L"无法打开 IONEX 知识库");
        return false;
    }
    sqlite3_busy_timeout(raw, 10000);
    const char* schema =
        "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS source_definitions(source_id INTEGER PRIMARY KEY,source_key TEXT NOT NULL UNIQUE,source_priority INTEGER NOT NULL UNIQUE);"
        "CREATE TABLE IF NOT EXISTS daily_scans(date TEXT PRIMARY KEY,status TEXT NOT NULL CHECK(status IN('complete','partial')),source_success_count INTEGER NOT NULL,source_expected_count INTEGER NOT NULL,product_count INTEGER NOT NULL,is_provisional INTEGER NOT NULL,scanned_at_utc TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS daily_source_scans(date TEXT NOT NULL,source_id INTEGER NOT NULL,status TEXT NOT NULL CHECK(status IN('complete','failed')),listing_url TEXT NOT NULL,file_count INTEGER NOT NULL,error_message TEXT NOT NULL,PRIMARY KEY(date,source_id),FOREIGN KEY(date) REFERENCES daily_scans(date) ON DELETE CASCADE,FOREIGN KEY(source_id) REFERENCES source_definitions(source_id));"
        "CREATE TABLE IF NOT EXISTS products(logical_key TEXT PRIMARY KEY,date TEXT NOT NULL,start_hour INTEGER NOT NULL,start_minute INTEGER NOT NULL,series TEXT NOT NULL,center TEXT NOT NULL,project TEXT NOT NULL,solution TEXT NOT NULL,latency TEXT NOT NULL,duration TEXT NOT NULL,sample_interval TEXT NOT NULL,content_type TEXT NOT NULL,original_content TEXT NOT NULL,format TEXT NOT NULL,FOREIGN KEY(date) REFERENCES daily_scans(date) ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS products_date ON products(date,content_type,latency,center,series);"
        "CREATE TABLE IF NOT EXISTS product_sources(logical_key TEXT NOT NULL,source_id INTEGER NOT NULL,file_name TEXT NOT NULL,download_url TEXT NOT NULL,PRIMARY KEY(logical_key,source_id,file_name,download_url),FOREIGN KEY(logical_key) REFERENCES products(logical_key) ON DELETE CASCADE,FOREIGN KEY(source_id) REFERENCES source_definitions(source_id));"
        "INSERT OR IGNORE INTO source_definitions VALUES"
        "(1,'CDDIS',1),(2,'BKG',2),(3,'IGN',3),(4,'WHU',4),(5,'CAS',5);"
        "INSERT INTO metadata VALUES('schema_version','1') ON CONFLICT(key) DO UPDATE SET value='1';"
        "INSERT INTO metadata VALUES('product_type','ionex_gim') ON CONFLICT(key) DO UPDATE SET value='ionex_gim';";
    return Execute(raw, schema, error);
}

bool OpenReadDatabase(
    const std::filesystem::path& path, DbPtr& database, std::wstring& error) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    sqlite3* raw = nullptr;
    const int status = sqlite3_open_v2(
        path.string().c_str(), &raw,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    database.reset(raw);
    if (status != SQLITE_OK || raw == nullptr) {
        error = DbError(raw, L"无法以只读方式打开 IONEX 知识库");
        return false;
    }
    sqlite3_busy_timeout(raw, 10000);
    return true;
}

bool ParseDateText(const std::string& text, QueryDate& date) {
    int year = 0, month = 0, day = 0;
    if (sscanf_s(text.c_str(), "%d-%d-%d", &year, &month, &day) != 3) {
        return false;
    }
    date = MakeQueryDate(year, month, day);
    return date.dayOfYear > 0;
}

std::string IonexProductFamilyKey(const IonexProductRecord& record) {
    return record.series + "\x1f" + record.center + "\x1f"
        + record.project + "\x1f" + record.solution + "\x1f"
        + record.latency + "\x1f" + record.duration + "\x1f"
        + record.interval + "\x1f" + record.contentType + "\x1f"
        + std::to_string(record.startHour) + ":"
        + std::to_string(record.startMinute);
}

IonexProductRecord IonexProductFromStatement(sqlite3_stmt* statement) {
    IonexProductRecord record;
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
    record.contentType = ColumnText(statement, 11);
    record.originalContent = ColumnText(statement, 12);
    record.format = ColumnText(statement, 13);
    return record;
}

IonexFileStamp GetIonexFileStamp(const std::filesystem::path& path) {
    IonexFileStamp stamp;
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

std::shared_ptr<IonexProductFamilyCache> LoadIonexProductFamilyCache(
    const std::filesystem::path& databasePath) {
    const auto absolute = std::filesystem::absolute(databasePath);
    const std::wstring cacheKey = absolute.wstring();
    const IonexFileStamp currentStamp = GetIonexFileStamp(absolute);
    std::scoped_lock cacheLock(gIonexFamilyCacheMutex);
    if (const auto found = gIonexFamilyCaches.find(cacheKey);
        found != gIonexFamilyCaches.end()
        && found->second != nullptr
        && found->second->stamp == currentStamp) {
        return found->second;
    }

    auto cache = std::make_shared<IonexProductFamilyCache>();
    cache->databasePath = absolute;
    cache->stamp = currentStamp;
    if (!currentStamp.exists) {
        cache->error = L"IONEX 知识库文件不存在：" + absolute.wstring();
        gIonexFamilyCaches[cacheKey] = cache;
        return cache;
    }
    DbPtr database;
    if (!OpenReadDatabase(absolute, database, cache->error)) {
        gIonexFamilyCaches[cacheKey] = cache;
        return cache;
    }

    auto scans = Prepare(database.get(),
        "SELECT date,status,source_expected_count FROM daily_scans;",
        cache->error);
    if (!scans) {
        gIonexFamilyCaches[cacheKey] = cache;
        return cache;
    }
    while (true) {
        const int status = sqlite3_step(scans.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            cache->error = DbError(database.get(), L"读取 IONEX 日期高速索引失败");
            gIonexFamilyCaches[cacheKey] = cache;
            return cache;
        }
        QueryDate date;
        if (!ParseDateText(ColumnText(scans.get(), 0), date)) continue;
        cache->scans.push_back({ToSysDays(date), date,
            ColumnText(scans.get(), 1) == "complete"
                && sqlite3_column_int(scans.get(), 2) == kSourceCount});
    }

    auto products = Prepare(database.get(),
        "SELECT logical_key,date,start_hour,start_minute,series,center,"
        "project,solution,latency,duration,sample_interval,content_type,"
        "original_content,format FROM products;", cache->error);
    if (!products) {
        gIonexFamilyCaches[cacheKey] = cache;
        return cache;
    }
    while (true) {
        const int status = sqlite3_step(products.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            cache->error = DbError(database.get(), L"读取 IONEX 产品高速索引失败");
            cache->families.clear();
            cache->familyIndex.clear();
            gIonexFamilyCaches[cacheKey] = cache;
            return cache;
        }
        IonexProductRecord record = IonexProductFromStatement(products.get());
        if (record.logicalKey.empty() || record.nominalDate.dayOfYear <= 0) continue;
        const std::string familyKey = IonexProductFamilyKey(record);
        auto [found, inserted] = cache->familyIndex.emplace(
            familyKey, cache->families.size());
        if (inserted) {
            IonexProductFamilyCacheEntry family;
            family.familyKey = familyKey;
            family.representative = record;
            family.representative.routes.clear();
            cache->families.push_back(std::move(family));
        }
        cache->families[found->second].days.push_back(
            {ToSysDays(record.nominalDate), record.nominalDate});
    }
    std::ranges::sort(cache->scans,
        [](const IonexScanDayRef& left, const IonexScanDayRef& right) {
            return left.day < right.day;
        });
    for (auto& family : cache->families) {
        std::ranges::sort(family.days,
            [](const IonexFamilyDayRef& left, const IonexFamilyDayRef& right) {
                return left.day < right.day;
            });
        family.days.erase(std::unique(family.days.begin(), family.days.end(),
            [](const IonexFamilyDayRef& left, const IonexFamilyDayRef& right) {
                return left.day == right.day;
            }), family.days.end());
    }
    if (!(GetIonexFileStamp(absolute) == currentStamp)) cache->stamp = {};
    gIonexFamilyCaches[cacheKey] = cache;
    return cache;
}

const IonexProductFamilyCacheEntry* FindIonexFamily(
    const IonexProductFamilyCache& cache, const std::string& familyKey) {
    const auto found = cache.familyIndex.find(familyKey);
    if (found == cache.familyIndex.end()
        || found->second >= cache.families.size()) return nullptr;
    return &cache.families[found->second];
}

bool ValidIonexRange(
    const QueryDate& startDate, const QueryDate& endDate,
    int& expectedDays, std::chrono::sys_days& first,
    std::chrono::sys_days& last, std::wstring& error) {
    first = ToSysDays(startDate);
    last = ToSysDays(endDate);
    const auto span = (last - first).count();
    if (span < 0 || span > static_cast<long long>(
            std::numeric_limits<int>::max() - 1)) {
        error = L"IONEX 日期范围无效";
        return false;
    }
    expectedDays = static_cast<int>(span) + 1;
    return true;
}

bool ProvisionalDate(const QueryDate& date) {
    const auto today = std::chrono::floor<std::chrono::days>(
        std::chrono::system_clock::now());
    return ToSysDays(date) >= today - std::chrono::days{35};
}

} // namespace

std::vector<IonexArchiveFile> ParseIonexDirectoryListing(
    const std::string& listing, const QueryDate& expectedDate,
    const std::wstring& directoryUrl) {
    static const std::regex candidate(
        R"(([A-Za-z0-9]{10}_[0-9]{11}_[0-9]{2}[dDhHmMsSuUwWlLyY]_[0-9]{2}[dDhHmMsSuUwWlLyY]_(?:GIM|ION|ROT)\.(?:INX|IOX)(?:\.(?:gz|Z))?|[A-Za-z0-9]{4}[0-9]{4}\.[0-9]{2}[iIfF](?:\.(?:gz|Z))?)(?=$|[^A-Za-z0-9_.-]))",
        std::regex::icase);
    std::set<std::string> seen;
    std::vector<IonexArchiveFile> result;
    for (std::sregex_iterator it(listing.begin(), listing.end(), candidate), end;
         it != end; ++it) {
        std::string name = (*it)[1].str();
        if (!seen.insert(Lower(name)).second) continue;
        IonexArchiveFile file;
        if (!ParseLongName(name, expectedDate, file)
            && !ParseShortName(name, expectedDate, file)) continue;
        file.fileName = std::move(name);
        file.downloadUrl = JoinUrl(directoryUrl, file.fileName);
        result.push_back(std::move(file));
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        if (left.logicalKey != right.logicalKey) return left.logicalKey < right.logicalKey;
        return left.fileName < right.fileName;
    });
    return result;
}

std::vector<IonexSourceScan> QueryIonexDate(
    const QueryDate& date, const std::stop_token stopToken) {
    std::vector<std::future<IonexSourceScan>> futures;
    futures.reserve(kSources.size());
    for (const auto& source : kSources) {
        futures.push_back(std::async(std::launch::async, [&, source]() {
            return QuerySource(source, date, stopToken);
        }));
    }
    std::vector<IonexSourceScan> scans;
    scans.reserve(kSources.size());
    for (auto& future : futures) scans.push_back(future.get());
    std::ranges::sort(scans, [](const auto& left, const auto& right) {
        return left.sourcePriority < right.sourcePriority;
    });
    return scans;
}

std::filesystem::path GetIonexAvailabilityDatabasePath() {
    const auto explicitPath = EnvironmentPath(L"GNSS_DOWNLOADER_IONEX_DB_PATH");
    if (!explicitPath.empty()) return explicitPath;
    const auto directory = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
    if (!directory.empty()) {
        return ResolveDatabaseFilePath(directory / L"ionex_availability.sqlite");
    }
    return ResolveDatabaseFilePath(GetIgsAvailabilityDatabasePath().parent_path()
        / L"ionex_availability.sqlite");
}

bool StoreIonexDate(
    const QueryDate& date, const std::vector<IonexSourceScan>& sources,
    std::wstring& error) {
    std::scoped_lock lock(gDatabaseMutex);
    const int successful = static_cast<int>(std::ranges::count_if(
        sources, [](const auto& source) { return source.success; }));
    if (successful == 0) {
        error = L"IONEX 公开数据源均未读取成功";
        return false;
    }
    std::map<std::string, IonexArchiveFile> products;
    for (const auto& source : sources) {
        for (const auto& file : source.files)
            products.try_emplace(file.logicalKey, file);
    }
    DbPtr database;
    if (!OpenDatabase(GetIonexAvailabilityDatabasePath(), database, error))
        return false;
    const std::string dateText = DateText(date);
    int existing = -1;
    int existingExpected = -1;
    {
        auto statement = Prepare(database.get(),
            "SELECT source_success_count,source_expected_count FROM daily_scans WHERE date=?1;", error);
        if (!statement || !Bind(statement.get(), 1, dateText)) return false;
        if (sqlite3_step(statement.get()) == SQLITE_ROW) {
            existing = sqlite3_column_int(statement.get(), 0);
            existingExpected = sqlite3_column_int(statement.get(), 1);
        }
    }
    if (existingExpected == kSourceCount
        && (existing > successful
            || (existing == successful && successful < kSourceCount))) return true;
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
            "DELETE FROM daily_scans WHERE date=?1;", error);
        if (!statement || !Bind(statement.get(), 1, dateText)
            || sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (error.empty()) error = DbError(database.get(), L"无法替换 IONEX 日期记录");
            rollback();
            return false;
        }
    }
    {
        auto statement = Prepare(database.get(),
            "INSERT INTO daily_scans VALUES(?1,?2,?3,?4,?5,?6,strftime('%Y-%m-%dT%H:%M:%SZ','now'));",
            error);
        if (!statement) { rollback(); return false; }
        Bind(statement.get(), 1, dateText);
        Bind(statement.get(), 2, successful == kSourceCount ? "complete" : "partial");
        sqlite3_bind_int(statement.get(), 3, successful);
        sqlite3_bind_int(statement.get(), 4, kSourceCount);
        sqlite3_bind_int64(statement.get(), 5,
            static_cast<sqlite3_int64>(products.size()));
        sqlite3_bind_int(statement.get(), 6, ProvisionalDate(date) ? 1 : 0);
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法写入 IONEX 日期记录");
            rollback();
            return false;
        }
    }
    auto sourceStatement = Prepare(database.get(),
        "INSERT INTO daily_source_scans VALUES(?1,?2,?3,?4,?5,?6);", error);
    if (!sourceStatement) { rollback(); return false; }
    for (const auto& source : sources) {
        sqlite3_reset(sourceStatement.get());
        sqlite3_clear_bindings(sourceStatement.get());
        Bind(sourceStatement.get(), 1, dateText);
        sqlite3_bind_int(sourceStatement.get(), 2, source.sourcePriority);
        Bind(sourceStatement.get(), 3, source.success ? "complete" : "failed");
        Bind(sourceStatement.get(), 4, WideToUtf8(source.listingUrl));
        sqlite3_bind_int64(sourceStatement.get(), 5,
            static_cast<sqlite3_int64>(source.files.size()));
        Bind(sourceStatement.get(), 6, WideToUtf8(source.error));
        if (sqlite3_step(sourceStatement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法写入 IONEX 来源状态");
            rollback();
            return false;
        }
    }
    auto productStatement = Prepare(database.get(),
        "INSERT INTO products VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14);",
        error);
    auto routeStatement = Prepare(database.get(),
        "INSERT OR IGNORE INTO product_sources VALUES(?1,?2,?3,?4);", error);
    if (!productStatement || !routeStatement) { rollback(); return false; }
    for (const auto& [logicalKey, file] : products) {
        sqlite3_reset(productStatement.get());
        sqlite3_clear_bindings(productStatement.get());
        Bind(productStatement.get(), 1, logicalKey);
        Bind(productStatement.get(), 2, dateText);
        sqlite3_bind_int(productStatement.get(), 3, file.startHour);
        sqlite3_bind_int(productStatement.get(), 4, file.startMinute);
        Bind(productStatement.get(), 5, file.series);
        Bind(productStatement.get(), 6, file.center);
        Bind(productStatement.get(), 7, file.project);
        Bind(productStatement.get(), 8, file.solution);
        Bind(productStatement.get(), 9, file.latency);
        Bind(productStatement.get(), 10, file.duration);
        Bind(productStatement.get(), 11, file.interval);
        Bind(productStatement.get(), 12, file.contentType);
        Bind(productStatement.get(), 13, file.originalContent);
        Bind(productStatement.get(), 14, file.format);
        if (sqlite3_step(productStatement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法写入 IONEX 产品");
            rollback();
            return false;
        }
        for (const auto& source : sources) {
            for (const auto& route : source.files) {
                if (route.logicalKey != logicalKey) continue;
                sqlite3_reset(routeStatement.get());
                sqlite3_clear_bindings(routeStatement.get());
                Bind(routeStatement.get(), 1, logicalKey);
                sqlite3_bind_int(routeStatement.get(), 2, source.sourcePriority);
                Bind(routeStatement.get(), 3, route.fileName);
                Bind(routeStatement.get(), 4, WideToUtf8(route.downloadUrl));
                if (sqlite3_step(routeStatement.get()) != SQLITE_DONE) {
                    error = DbError(database.get(), L"无法写入 IONEX 下载来源");
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

IonexAvailabilityLookup LoadIonexAvailability(const QueryDate& date) {
    std::scoped_lock lock(gDatabaseMutex);
    IonexAvailabilityLookup result;
    result.sourcesExpected = kSourceCount;
    result.databasePath = GetIonexAvailabilityDatabasePath();
    DbPtr database;
    if (!OpenReadDatabase(result.databasePath, database, result.error)) return result;
    const std::string dateText = DateText(date);
    {
        auto statement = Prepare(database.get(),
            "SELECT status,source_success_count,source_expected_count,is_provisional FROM daily_scans WHERE date=?1;",
            result.error);
        if (!statement || !Bind(statement.get(), 1, dateText)) return result;
        if (sqlite3_step(statement.get()) != SQLITE_ROW) {
            result.error.clear();
            return result;
        }
        result.indexed = true;
        result.sourcesSuccessful = sqlite3_column_int(statement.get(), 1);
        result.sourcesExpected = sqlite3_column_int(statement.get(), 2);
        result.complete = ColumnText(statement.get(), 0) == "complete"
            && result.sourcesExpected == kSourceCount;
        result.provisional = sqlite3_column_int(statement.get(), 3) != 0;
    }
    auto products = Prepare(database.get(),
        "SELECT logical_key,start_hour,start_minute,series,center,project,solution,latency,duration,sample_interval,content_type,original_content,format FROM products WHERE date=?1 ORDER BY content_type,latency,center,series,start_hour,start_minute;",
        result.error);
    if (!products || !Bind(products.get(), 1, dateText)) return result;
    auto routes = Prepare(database.get(),
        "SELECT d.source_key,d.source_priority,s.file_name,s.download_url FROM product_sources s JOIN source_definitions d ON d.source_id=s.source_id WHERE s.logical_key=?1 ORDER BY d.source_priority,s.file_name;",
        result.error);
    if (!routes) return result;
    while (sqlite3_step(products.get()) == SQLITE_ROW) {
        IonexProductRecord record;
        record.logicalKey = ColumnText(products.get(), 0);
        record.nominalDate = date;
        record.startHour = sqlite3_column_int(products.get(), 1);
        record.startMinute = sqlite3_column_int(products.get(), 2);
        record.series = ColumnText(products.get(), 3);
        record.center = ColumnText(products.get(), 4);
        record.project = ColumnText(products.get(), 5);
        record.solution = ColumnText(products.get(), 6);
        record.latency = ColumnText(products.get(), 7);
        record.duration = ColumnText(products.get(), 8);
        record.interval = ColumnText(products.get(), 9);
        record.contentType = ColumnText(products.get(), 10);
        record.originalContent = ColumnText(products.get(), 11);
        record.format = ColumnText(products.get(), 12);
        sqlite3_reset(routes.get());
        sqlite3_clear_bindings(routes.get());
        if (!Bind(routes.get(), 1, record.logicalKey)) {
            result.products.clear();
            return result;
        }
        while (sqlite3_step(routes.get()) == SQLITE_ROW) {
            record.routes.push_back({ColumnText(routes.get(), 0),
                sqlite3_column_int(routes.get(), 1), ColumnText(routes.get(), 2),
                Utf8ToWide(ColumnText(routes.get(), 3))});
        }
        result.products.push_back(std::move(record));
    }
    return result;
}

IonexProductFamilyRangeLookup LoadIonexProductFamilyRange(
    const QueryDate& startDate, const QueryDate& endDate) {
    std::scoped_lock lock(gDatabaseMutex);
    IonexProductFamilyRangeLookup result;
    result.databasePath = GetIonexAvailabilityDatabasePath();
    std::chrono::sys_days first, last;
    if (!ValidIonexRange(startDate, endDate, result.expectedDays,
                         first, last, result.error)) return result;
    const auto cache = LoadIonexProductFamilyCache(result.databasePath);
    if (!cache || !cache->error.empty()) {
        result.error = cache && !cache->error.empty()
            ? cache->error : L"无法加载 IONEX 高速索引";
        return result;
    }
    if (!cache->stamp.exists) {
        result.error = L"IONEX 知识库文件不存在";
        return result;
    }

    const auto scanBegin = std::lower_bound(cache->scans.begin(),
        cache->scans.end(), first,
        [](const IonexScanDayRef& ref, const std::chrono::sys_days value) {
            return ref.day < value;
        });
    const auto scanEnd = std::upper_bound(cache->scans.begin(),
        cache->scans.end(), last,
        [](const std::chrono::sys_days value, const IonexScanDayRef& ref) {
            return value < ref.day;
        });
    result.indexedDays = static_cast<int>(scanEnd - scanBegin);
    auto scan = scanBegin;
    for (int offset = 0; offset < result.expectedDays; ++offset) {
        const auto day = first + std::chrono::days{offset};
        if (scan == scanEnd || scan->day != day) {
            result.incompleteDates.push_back(FromSysDays(day));
            continue;
        }
        if (!scan->complete) result.incompleteDates.push_back(scan->date);
        ++scan;
    }
    result.complete = result.incompleteDates.empty();

    for (const auto& family : cache->families) {
        const auto begin = std::lower_bound(family.days.begin(),
            family.days.end(), first,
            [](const IonexFamilyDayRef& ref,
               const std::chrono::sys_days value) {
                return ref.day < value;
            });
        const auto end = std::upper_bound(family.days.begin(),
            family.days.end(), last,
            [](const std::chrono::sys_days value,
               const IonexFamilyDayRef& ref) {
                return value < ref.day;
            });
        if (begin == end) continue;
        IonexProductFamilyRecord record;
        record.familyKey = family.familyKey;
        record.representative = family.representative;
        record.representative.nominalDate = begin->date;
        record.representative.routes.clear();
        record.availableDays = static_cast<int>(end - begin);
        result.families.push_back(std::move(record));
    }
    std::ranges::sort(result.families,
        [](const IonexProductFamilyRecord& left,
           const IonexProductFamilyRecord& right) {
            const auto& a = left.representative;
            const auto& b = right.representative;
            if (a.latency != b.latency) return a.latency < b.latency;
            if (a.center != b.center) return a.center < b.center;
            if (a.series != b.series) return a.series < b.series;
            if (a.startHour != b.startHour)
                return a.startHour < b.startHour;
            return a.startMinute < b.startMinute;
        });
    return result;
}

IonexProductFamilyRoutesLookup LoadIonexProductFamilyRoutes(
    const std::string& familyKey, const QueryDate& startDate,
    const QueryDate& endDate, const std::stop_token stopToken,
    const IonexProductRouteProgress progress) {
    std::scoped_lock lock(gDatabaseMutex);
    IonexProductFamilyRoutesLookup result;
    result.databasePath = GetIonexAvailabilityDatabasePath();
    int expectedDays = 0;
    std::chrono::sys_days first, last;
    if (!ValidIonexRange(startDate, endDate, expectedDays, first, last,
                         result.error)) return result;
    const auto cache = LoadIonexProductFamilyCache(result.databasePath);
    if (!cache || !cache->error.empty()) {
        result.error = cache && !cache->error.empty()
            ? cache->error : L"无法加载 IONEX 高速索引";
        return result;
    }
    const auto* family = FindIonexFamily(*cache, familyKey);
    if (family == nullptr) {
        result.error = L"高速 IONEX 索引中找不到所选产品族";
        return result;
    }
    const auto begin = std::lower_bound(family->days.begin(), family->days.end(),
        first, [](const IonexFamilyDayRef& ref,
                  const std::chrono::sys_days value) {
            return ref.day < value;
        });
    const auto end = std::upper_bound(family->days.begin(), family->days.end(),
        last, [](const std::chrono::sys_days value,
                 const IonexFamilyDayRef& ref) {
            return value < ref.day;
        });
    const auto total = static_cast<std::size_t>(end - begin);
    if (progress) progress(0, total);

    DbPtr database;
    if (!OpenReadDatabase(result.databasePath, database, result.error)) return result;
    auto statement = Prepare(database.get(),
        "SELECT p.logical_key,p.date,p.start_hour,p.start_minute,p.series,"
        "p.center,p.project,p.solution,p.latency,p.duration,p.sample_interval,"
        "p.content_type,p.original_content,p.format,d.source_key,"
        "d.source_priority,s.file_name,s.download_url "
        "FROM products p LEFT JOIN product_sources s "
        "ON s.logical_key=p.logical_key LEFT JOIN source_definitions d "
        "ON d.source_id=s.source_id WHERE p.date BETWEEN ?1 AND ?2 "
        "AND p.series=?3 AND p.center=?4 AND p.project=?5 "
        "AND p.solution=?6 AND p.latency=?7 AND p.duration=?8 "
        "AND p.sample_interval=?9 AND p.content_type=?10 "
        "AND p.start_hour=?11 AND p.start_minute=?12 "
        "ORDER BY p.date,p.logical_key,d.source_priority,s.file_name,"
        "s.download_url;", result.error);
    if (!statement) return result;
    if (!Bind(statement.get(), 1, DateText(startDate))
        || !Bind(statement.get(), 2, DateText(endDate))
        || !Bind(statement.get(), 3, family->representative.series)
        || !Bind(statement.get(), 4, family->representative.center)
        || !Bind(statement.get(), 5, family->representative.project)
        || !Bind(statement.get(), 6, family->representative.solution)
        || !Bind(statement.get(), 7, family->representative.latency)
        || !Bind(statement.get(), 8, family->representative.duration)
        || !Bind(statement.get(), 9, family->representative.interval)
        || !Bind(statement.get(), 10, family->representative.contentType)) {
        result.error = L"无法绑定 IONEX 高速索引查询条件";
        return result;
    }
    sqlite3_bind_int(statement.get(), 11, family->representative.startHour);
    sqlite3_bind_int(statement.get(), 12, family->representative.startMinute);

    std::string currentKey;
    std::size_t completed = 0;
    while (!stopToken.stop_requested()) {
        const int status = sqlite3_step(statement.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            result.error = DbError(database.get(), L"读取 IONEX 下载路线失败");
            return result;
        }
        const std::string logicalKey = ColumnText(statement.get(), 0);
        if (logicalKey != currentKey) {
            if (!currentKey.empty()) ++completed;
            IonexProductRecord record = IonexProductFromStatement(statement.get());
            result.records.push_back({record.nominalDate, std::move(record)});
            currentKey = logicalKey;
        }
        auto& record = result.records.back().record;
        const std::string source = ColumnText(statement.get(), 14);
        const std::string fileName = ColumnText(statement.get(), 16);
        if (!source.empty() && !fileName.empty()) {
            record.routes.push_back({source, sqlite3_column_int(statement.get(), 15),
                fileName, Utf8ToWide(ColumnText(statement.get(), 17))});
        }
    }
    if (stopToken.stop_requested()) {
        result.error = L"IONEX 下载路线读取已取消";
        return result;
    }
    if (!currentKey.empty()) ++completed;
    if (progress) progress(completed, total);
    return result;
}

bool IsIonexDateComplete(
    const QueryDate& date, bool& complete, std::wstring& error) {
    std::scoped_lock lock(gDatabaseMutex);
    complete = false;
    DbPtr database;
    if (!OpenDatabase(GetIonexAvailabilityDatabasePath(), database, error))
        return false;
    auto statement = Prepare(database.get(),
        "SELECT status,is_provisional,source_expected_count FROM daily_scans WHERE date=?1;",
        error);
    if (!statement || !Bind(statement.get(), 1, DateText(date))) return false;
    if (sqlite3_step(statement.get()) == SQLITE_ROW) {
        complete = ColumnText(statement.get(), 0) == "complete"
            && sqlite3_column_int(statement.get(), 1) == 0
            && sqlite3_column_int(statement.get(), 2) == kSourceCount;
    }
    return true;
}

IonexAvailabilityStats GetIonexAvailabilityStats() {
    std::scoped_lock lock(gDatabaseMutex);
    IonexAvailabilityStats result;
    result.databasePath = GetIonexAvailabilityDatabasePath();
    DbPtr database;
    if (!OpenDatabase(result.databasePath, database, result.error)) return result;
    auto statement = Prepare(database.get(),
        "SELECT SUM(status='complete'),SUM(status='partial'),MIN(date),MAX(date) FROM daily_scans;",
        result.error);
    if (statement && sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.completeDays = sqlite3_column_int(statement.get(), 0);
        result.partialDays = sqlite3_column_int(statement.get(), 1);
        result.firstDate = ColumnText(statement.get(), 2);
        result.lastDate = ColumnText(statement.get(), 3);
    }
    auto counts = Prepare(database.get(),
        "SELECT (SELECT COUNT(*) FROM products),(SELECT COUNT(*) FROM product_sources);",
        result.error);
    if (counts && sqlite3_step(counts.get()) == SQLITE_ROW) {
        result.products = sqlite3_column_int64(counts.get(), 0);
        result.sourceRoutes = sqlite3_column_int64(counts.get(), 1);
    }
    return result;
}

int IonexExpectedSourceCount() {
    return kSourceCount;
}
