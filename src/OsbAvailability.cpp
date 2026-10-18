#include "OsbAvailability.h"

#include "DateUtil.h"
#include "ArchiveLegacyNames.h"
#include "HttpClient.h"
#include "IgsAvailabilityDb.h"

#include "sqlite3.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <format>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <string_view>
#include <unordered_map>

namespace {

constexpr int kSourceCount = 5;

enum class SourceKind { WeeklyMirror, CasAnnual, WccWeekly };

struct SourceDefinition {
    const char* key;
    int priority;
    SourceKind kind;
    const wchar_t* root;
    const wchar_t* annualBiasRoot;
    bool ftp;
};

constexpr std::array kSources{
    SourceDefinition{"CDDIS", 1, SourceKind::WeeklyMirror,
        L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/products/",
        L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/products/bias/", true},
    SourceDefinition{"IGN", 2, SourceKind::WeeklyMirror,
        L"ftp://igs.ign.fr/pub/igs/products/",
        L"ftp://igs.ign.fr/pub/igs/products/bias/", true},
    SourceDefinition{"WHU", 3, SourceKind::WeeklyMirror,
        L"ftp://igs.gnsswhu.cn/pub/gps/products/", L"", true},
    SourceDefinition{"CAS", 4, SourceKind::CasAnnual,
        L"https://data.bdsmart.cn/pub/product/bias/", L"", false},
    SourceDefinition{"WCC", 5, SourceKind::WccWeekly,
        L"ftps://bdspride.com/wcc/", L"", true},
};

struct DbCloser {
    void operator()(sqlite3* value) const { if (value) sqlite3_close(value); }
};
struct StmtCloser {
    void operator()(sqlite3_stmt* value) const { if (value) sqlite3_finalize(value); }
};
using DbPtr = std::unique_ptr<sqlite3, DbCloser>;
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtCloser>;

std::mutex gDatabaseMutex;
std::mutex gAnnualCacheMutex;
std::map<std::wstring, std::shared_future<HttpTextResult>> gAnnualCache;

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

QueryDate DateFromDoy(const int year, const int doy) {
    return FromSysDays(std::chrono::sys_days{
        std::chrono::year{year} / std::chrono::January / 1}
        + std::chrono::days{doy - 1});
}

std::string DateText(const QueryDate& date) {
    return std::format("{:04}-{:02}-{:02}", date.year, date.month, date.day);
}

QueryDate EndForDuration(const QueryDate& start, const std::string& duration) {
    if (duration.size() != 3) return start;
    int count = 0;
    try { count = std::stoi(duration.substr(0, 2)); }
    catch (...) { return start; }
    if (count < 1) return start;
    const char unit = static_cast<char>(std::toupper(
        static_cast<unsigned char>(duration[2])));
    if (unit == 'D') {
        return FromSysDays(ToSysDays(start) + std::chrono::days{count - 1});
    }
    return start;
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
    if (p.starts_with("R0") || p == "R03") return "REPROCESSING";
    if (s == "FIN") return "FINAL";
    if (s == "RAP") return "RAPID";
    if (s == "ULT") return "ULTRA_RAPID";
    if (s == "NRT") return "NEAR_REAL_TIME";
    if (s == "RTS") return "REAL_TIME";
    return "OTHER";
}

std::string ConstellationFor(const std::string& series) {
    const std::string upper = Upper(series);
    if (upper.starts_with("CAS")) return "GRECJ";
    if (upper.starts_with("WCC")) return "GEC";
    if (upper.starts_with("WUM")) return "GECJ";
    if (upper.starts_with("HUS") || upper.starts_with("GFZ0MGX")
        || upper.starts_with("GBM0MGX") || upper.starts_with("GRG0MGX"))
        return "GEC";
    if (upper.starts_with("COD0MGX")) return "GEJ";
    if (upper.starts_with("COD") || upper.starts_with("GRG")) return "GE";
    if (upper.starts_with("EMR")) return "G";
    if (upper.starts_with("TUG") || upper.starts_with("TUM")) return "GE";
    return "MULTI";
}

std::string BiasClassFor(const std::string& series) {
    const std::string upper = Upper(series);
    if (upper.starts_with("CAS")) return "CODE_ONLY";
    if (upper.starts_with("WCC")) return "COMBINED_CODE_PHASE";
    return "CODE_PHASE";
}

std::string LogicalKey(const OsbArchiveFile& file) {
    return file.series + "|" + DateText(file.coverageStart) + "|"
        + std::format("{:02}{:02}|{}|{}|{}", file.startHour,
            file.startMinute, file.duration, file.interval, file.biasClass);
}

bool ParseLongName(const std::string& name, OsbArchiveFile& file) {
    static const std::regex pattern(
        R"(^([A-Z0-9]{10})_([0-9]{4})([0-9]{3})([0-9]{2})([0-9]{2})_([0-9]{2}[DHMSUWLY])_([0-9]{2}[DHMSUWLY])_OSB\.BIA(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) return false;
    const int year = std::stoi(match[2].str());
    const int doy = std::stoi(match[3].str());
    if (year < 1980 || doy < 1
        || doy > (IsLeapYear(year) ? 366 : 365)) return false;
    file.series = Upper(match[1].str());
    file.center = file.series.substr(0, 3);
    file.project = file.series.substr(4, 3);
    file.solution = file.series.substr(7, 3);
    file.coverageStart = DateFromDoy(year, doy);
    file.startHour = std::stoi(match[4].str());
    file.startMinute = std::stoi(match[5].str());
    file.duration = Upper(match[6].str());
    file.interval = Upper(match[7].str());
    file.coverageEnd = EndForDuration(file.coverageStart, file.duration);
    file.latency = LatencyFor(file.project, file.solution);
    file.constellation = ConstellationFor(file.series);
    file.biasClass = BiasClassFor(file.series);
    file.format = "BIAS_SINEX_BIA";
    file.logicalKey = LogicalKey(file);
    return true;
}

bool ParseLegacyName(
    const std::string& name, const int expectedGpsWeek, OsbArchiveFile& file) {
    static const std::regex pattern(
        R"(^(COM|COD)([0-9]{4})([0-6])\.BIA(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) return false;
    const int week = std::stoi(match[2].str());
    if (week != expectedGpsWeek) return false;
    const int day = std::stoi(match[3].str());
    const bool mgex = Upper(match[1].str()) == "COM";
    file.series = mgex ? "COD0MGXFIN" : "COD0OPSFIN";
    file.center = "COD";
    file.project = mgex ? "MGX" : "OPS";
    file.solution = "FIN";
    file.coverageStart = FromSysDays(
        ToSysDays(OsbGpsWeekStartDate(week)) + std::chrono::days{day});
    file.coverageEnd = file.coverageStart;
    file.duration = "01D";
    file.interval = "01D";
    file.latency = "FINAL";
    file.constellation = "GE";
    file.biasClass = "CODE_PHASE";
    file.format = "BIAS_SINEX_BIA";
    file.logicalKey = LogicalKey(file);
    return true;
}

std::shared_future<HttpTextResult> AnnualResponse(
    const std::wstring& url, const bool ftp) {
    std::scoped_lock lock(gAnnualCacheMutex);
    const auto found = gAnnualCache.find(url);
    if (found != gAnnualCache.end()) return found->second;
    auto future = std::async(std::launch::async, [url, ftp]() {
        return ftp
            ? HttpListFtpDirectory(url, {}, 64U * 1024U * 1024U)
            : HttpGetText(url, {}, 64U * 1024U * 1024U);
    }).share();
    gAnnualCache.emplace(url, future);
    return future;
}

void ForgetFailedAnnualResponse(const std::wstring& url) {
    std::scoped_lock lock(gAnnualCacheMutex);
    gAnnualCache.erase(url);
}

void AddAnnualListing(
    OsbSourceScan& scan, const std::wstring& url, const bool ftp,
    const int expectedWeek, const std::string& archiveClass, bool& fatal) {
    scan.listingUrls.push_back(url);
    const HttpTextResult response = AnnualResponse(url, ftp).get();
    if (response.ok()) {
        auto files = ParseOsbDirectoryListing(
            response.body, expectedWeek, url, archiveClass);
        scan.files.insert(scan.files.end(),
            std::make_move_iterator(files.begin()),
            std::make_move_iterator(files.end()));
        return;
    }
    if (MissingDirectory(response)) return;
    ForgetFailedAnnualResponse(url);
    fatal = true;
    scan.transportErrorCode = response.transportErrorCode;
    scan.networkFailure = scan.networkFailure || IsNetworkFailure(response);
    if (!scan.error.empty()) scan.error += L"；";
    scan.error += response.error.empty()
        ? std::format(L"服务器返回 HTTP {}", response.statusCode)
        : response.error;
}

void AddWeeklyListing(
    OsbSourceScan& scan, const std::wstring& url, const bool ftp,
    const int expectedWeek, const std::string& archiveClass, bool& fatal,
    const std::stop_token stopToken) {
    if (stopToken.stop_requested()) {
        fatal = true;
        scan.error = L"查询已取消";
        return;
    }
    scan.listingUrls.push_back(url);
    const HttpTextResult response = ftp
        ? HttpListFtpDirectory(url, stopToken, 64U * 1024U * 1024U)
        : HttpGetText(url, stopToken, 64U * 1024U * 1024U);
    if (response.ok()) {
        auto files = ParseOsbDirectoryListing(
            response.body, expectedWeek, url, archiveClass);
        scan.files.insert(scan.files.end(),
            std::make_move_iterator(files.begin()),
            std::make_move_iterator(files.end()));
        return;
    }
    if (MissingDirectory(response)) return;
    fatal = true;
    scan.transportErrorCode = response.transportErrorCode;
    scan.networkFailure = scan.networkFailure || IsNetworkFailure(response);
    if (!scan.error.empty()) scan.error += L"；";
    scan.error += response.error.empty()
        ? std::format(L"服务器返回 HTTP {}", response.statusCode)
        : response.error;
}

OsbSourceScan QuerySource(
    const SourceDefinition& source, const int gpsWeek,
    const std::stop_token stopToken) {
    OsbSourceScan scan;
    scan.sourceKey = source.key;
    scan.sourcePriority = source.priority;
    bool fatal = false;
    if (source.kind == SourceKind::WeeklyMirror) {
        const std::wstring week = std::format(L"{:04}", gpsWeek);
        AddWeeklyListing(scan, std::wstring(source.root) + week + L"/",
            source.ftp, gpsWeek, "OPS", fatal, stopToken);
        if (gpsWeek >= 1680) {
            AddWeeklyListing(scan,
                std::wstring(source.root) + L"mgex/" + week + L"/",
                source.ftp, gpsWeek, "MGEX", fatal, stopToken);
        }
        if (gpsWeek >= 730 && gpsWeek <= 2240) {
            AddWeeklyListing(scan,
                std::wstring(source.root) + L"repro3/" + week + L"/",
                source.ftp, gpsWeek, "REPRO3", fatal, stopToken);
        }
        if (source.annualBiasRoot[0] != L'\0') {
            const auto start = OsbGpsWeekStartDate(gpsWeek);
            const auto end = FromSysDays(ToSysDays(start) + std::chrono::days{6});
            std::set<int> years{start.year, end.year};
            for (const int year : years) {
                const std::wstring url = std::wstring(source.annualBiasRoot)
                    + std::to_wstring(year) + L"/";
                AddAnnualListing(scan, url, source.ftp, gpsWeek,
                    "BIAS_ARCHIVE", fatal);
            }
        }
    } else if (source.kind == SourceKind::CasAnnual) {
        const auto start = OsbGpsWeekStartDate(gpsWeek);
        const auto end = FromSysDays(ToSysDays(start) + std::chrono::days{6});
        std::set<int> years{start.year, end.year};
        for (const int year : years) {
            const std::wstring url = std::wstring(source.root)
                + std::to_wstring(year) + L"/";
            AddAnnualListing(scan, url, false, gpsWeek, "CAS_BIAS", fatal);
        }
    } else {
        const std::wstring url = std::wstring(source.root)
            + std::format(L"{:04}", gpsWeek) + L"/";
        AddWeeklyListing(scan, url, true, gpsWeek, "WCC_COMBINED",
            fatal, stopToken);
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
        error = DbError(database, L"无法准备 OSB 知识库查询");
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
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = L"无法创建 OSB 知识库目录：" + path.parent_path().wstring();
        return false;
    }
    sqlite3* raw = nullptr;
    const int status = sqlite3_open16(path.c_str(), &raw);
    database.reset(raw);
    if (status != SQLITE_OK || raw == nullptr) {
        error = DbError(raw, L"无法打开 OSB 知识库");
        return false;
    }
    sqlite3_busy_timeout(raw, 10000);
    const char* schema =
        "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS source_definitions(source_id INTEGER PRIMARY KEY,source_key TEXT NOT NULL UNIQUE,source_priority INTEGER NOT NULL UNIQUE);"
        "CREATE TABLE IF NOT EXISTS weekly_scans(gps_week INTEGER PRIMARY KEY,status TEXT NOT NULL CHECK(status IN('complete','partial')),source_success_count INTEGER NOT NULL,source_expected_count INTEGER NOT NULL,product_count INTEGER NOT NULL,is_provisional INTEGER NOT NULL,scanned_at_utc TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS weekly_source_scans(gps_week INTEGER NOT NULL,source_id INTEGER NOT NULL,status TEXT NOT NULL CHECK(status IN('complete','failed')),listing_urls TEXT NOT NULL,file_count INTEGER NOT NULL,error_message TEXT NOT NULL,PRIMARY KEY(gps_week,source_id),FOREIGN KEY(gps_week) REFERENCES weekly_scans(gps_week) ON DELETE CASCADE,FOREIGN KEY(source_id) REFERENCES source_definitions(source_id));"
        "CREATE TABLE IF NOT EXISTS products(logical_key TEXT PRIMARY KEY,gps_week INTEGER NOT NULL,coverage_start TEXT NOT NULL,coverage_end TEXT NOT NULL,start_hour INTEGER NOT NULL,start_minute INTEGER NOT NULL,series TEXT NOT NULL,center TEXT NOT NULL,project TEXT NOT NULL,solution TEXT NOT NULL,latency TEXT NOT NULL,duration TEXT NOT NULL,sample_interval TEXT NOT NULL,constellation TEXT NOT NULL,bias_class TEXT NOT NULL,format TEXT NOT NULL,FOREIGN KEY(gps_week) REFERENCES weekly_scans(gps_week) ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS products_coverage ON products(coverage_start,coverage_end,center,latency);"
        "CREATE TABLE IF NOT EXISTS product_sources(logical_key TEXT NOT NULL,source_id INTEGER NOT NULL,file_name TEXT NOT NULL,download_url TEXT NOT NULL,archive_class TEXT NOT NULL,PRIMARY KEY(logical_key,source_id,file_name,download_url),FOREIGN KEY(logical_key) REFERENCES products(logical_key) ON DELETE CASCADE,FOREIGN KEY(source_id) REFERENCES source_definitions(source_id));"
        "INSERT OR IGNORE INTO source_definitions VALUES(1,'CDDIS',1),(2,'IGN',2),(3,'WHU',3),(4,'CAS',4),(5,'WCC',5);"
        "INSERT INTO metadata VALUES('schema_version','1') ON CONFLICT(key) DO UPDATE SET value='1';"
        "INSERT INTO metadata VALUES('product_type','osb') ON CONFLICT(key) DO UPDATE SET value='osb';"
        "INSERT INTO metadata VALUES('excludes_dcb_dsb_abs','1') ON CONFLICT(key) DO UPDATE SET value='1';";
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
        error = DbError(raw, L"无法以只读方式打开 OSB 知识库");
        return false;
    }
    sqlite3_busy_timeout(raw, 10000);
    return true;
}

bool ProvisionalWeek(const int gpsWeek) {
    const auto today = std::chrono::floor<std::chrono::days>(
        std::chrono::system_clock::now());
    return ToSysDays(OsbGpsWeekStartDate(gpsWeek))
        >= today - std::chrono::days{35};
}

QueryDate ParseDateText(const std::string& value) {
    int year = 0, month = 0, day = 0;
    if (sscanf_s(value.c_str(), "%d-%d-%d", &year, &month, &day) != 3)
        return {};
    return MakeQueryDate(year, month, day);
}

} // namespace

int CalculateOsbGpsWeek(const QueryDate& date) {
    const auto epoch = std::chrono::sys_days{
        std::chrono::year{1980} / std::chrono::January / 6};
    const auto days = (ToSysDays(date) - epoch).count();
    return days < 0 ? -1 : static_cast<int>(days / 7);
}

QueryDate OsbGpsWeekStartDate(const int gpsWeek) {
    const auto epoch = std::chrono::sys_days{
        std::chrono::year{1980} / std::chrono::January / 6};
    return FromSysDays(epoch + std::chrono::days{gpsWeek * 7LL});
}

std::vector<OsbArchiveFile> ParseOsbDirectoryListing(
    const std::string& listing, const int expectedGpsWeek,
    const std::wstring& directoryUrl, const std::string& archiveClass) {
    static const std::regex candidate(
        R"(([A-Za-z0-9][A-Za-z0-9_.-]*(?:_OSB\.BIA|\.bia)(?:\.(?:gz|Z))?)(?=$|[^A-Za-z0-9_.-]))",
        std::regex::icase);
    std::set<std::string> seen;
    std::vector<OsbArchiveFile> result;
    for (std::sregex_iterator it(listing.begin(), listing.end(), candidate), end;
         it != end; ++it) {
        std::string name = (*it)[1].str();
        if (!seen.insert(Lower(name)).second) continue;
        OsbArchiveFile file;
        if (!ParseLongName(name, file)
            && !ParseLegacyName(name, expectedGpsWeek, file)) continue;
        if (CalculateOsbGpsWeek(file.coverageStart) != expectedGpsWeek) continue;
        file.fileName = std::move(name);
        file.downloadUrl = JoinUrl(directoryUrl, file.fileName);
        file.archiveClass = archiveClass;
        result.push_back(std::move(file));
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        if (left.logicalKey != right.logicalKey)
            return left.logicalKey < right.logicalKey;
        return left.fileName < right.fileName;
    });
    return result;
}

std::vector<OsbSourceScan> QueryOsbWeek(
    const int gpsWeek, const std::stop_token stopToken) {
    std::vector<std::future<OsbSourceScan>> futures;
    futures.reserve(kSources.size());
    for (const auto& source : kSources) {
        futures.push_back(std::async(std::launch::async,
            [source, gpsWeek, stopToken]() {
                return QuerySource(source, gpsWeek, stopToken);
            }));
    }
    std::vector<OsbSourceScan> scans;
    scans.reserve(kSources.size());
    for (auto& future : futures) scans.push_back(future.get());
    std::ranges::sort(scans, [](const auto& left, const auto& right) {
        return left.sourcePriority < right.sourcePriority;
    });
    return scans;
}

std::filesystem::path GetOsbAvailabilityDatabasePath() {
    const auto explicitPath = EnvironmentPath(L"GNSS_DOWNLOADER_OSB_DB_PATH");
    if (!explicitPath.empty()) return explicitPath;
    const auto directory = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
    if (!directory.empty()) {
        return ResolveDatabaseFilePath(directory / L"osb_availability.sqlite");
    }
    return ResolveDatabaseFilePath(GetIgsAvailabilityDatabasePath().parent_path()
        / L"osb_availability.sqlite");
}

bool StoreOsbWeek(
    const int gpsWeek, const std::vector<OsbSourceScan>& sources,
    std::wstring& error) {
    std::scoped_lock lock(gDatabaseMutex);
    const int successful = static_cast<int>(std::ranges::count_if(
        sources, [](const auto& source) { return source.success; }));
    if (successful == 0) {
        error = L"OSB 公开数据源均未读取成功";
        return false;
    }
    std::map<std::string, OsbArchiveFile> products;
    for (const auto& source : sources) {
        for (const auto& file : source.files)
            products.try_emplace(file.logicalKey, file);
    }
    DbPtr database;
    if (!OpenDatabase(GetOsbAvailabilityDatabasePath(), database, error))
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
            "DELETE FROM weekly_scans WHERE gps_week=?1;", error);
        if (!statement) { rollback(); return false; }
        sqlite3_bind_int(statement.get(), 1, gpsWeek);
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法替换 OSB 周记录");
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
        sqlite3_bind_int(statement.get(), 6, ProvisionalWeek(gpsWeek) ? 1 : 0);
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法写入 OSB 周记录");
            rollback();
            return false;
        }
    }
    auto sourceStatement = Prepare(database.get(),
        "INSERT INTO weekly_source_scans VALUES(?1,?2,?3,?4,?5,?6);", error);
    if (!sourceStatement) { rollback(); return false; }
    for (const auto& source : sources) {
        sqlite3_reset(sourceStatement.get());
        sqlite3_clear_bindings(sourceStatement.get());
        sqlite3_bind_int(sourceStatement.get(), 1, gpsWeek);
        sqlite3_bind_int(sourceStatement.get(), 2, source.sourcePriority);
        Bind(sourceStatement.get(), 3, source.success ? "complete" : "failed");
        std::wstring urls;
        for (const auto& url : source.listingUrls) {
            if (!urls.empty()) urls += L";";
            urls += url;
        }
        Bind(sourceStatement.get(), 4, WideToUtf8(urls));
        sqlite3_bind_int64(sourceStatement.get(), 5,
            static_cast<sqlite3_int64>(source.files.size()));
        Bind(sourceStatement.get(), 6, WideToUtf8(source.error));
        if (sqlite3_step(sourceStatement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法写入 OSB 来源状态");
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
    if (!productStatement || !routeStatement) { rollback(); return false; }
    for (const auto& [logicalKey, file] : products) {
        sqlite3_reset(productStatement.get());
        sqlite3_clear_bindings(productStatement.get());
        Bind(productStatement.get(), 1, logicalKey);
        sqlite3_bind_int(productStatement.get(), 2, gpsWeek);
        Bind(productStatement.get(), 3, DateText(file.coverageStart));
        Bind(productStatement.get(), 4, DateText(file.coverageEnd));
        sqlite3_bind_int(productStatement.get(), 5, file.startHour);
        sqlite3_bind_int(productStatement.get(), 6, file.startMinute);
        Bind(productStatement.get(), 7, file.series);
        Bind(productStatement.get(), 8, file.center);
        Bind(productStatement.get(), 9, file.project);
        Bind(productStatement.get(), 10, file.solution);
        Bind(productStatement.get(), 11, file.latency);
        Bind(productStatement.get(), 12, file.duration);
        Bind(productStatement.get(), 13, file.interval);
        Bind(productStatement.get(), 14, file.constellation);
        Bind(productStatement.get(), 15, file.biasClass);
        Bind(productStatement.get(), 16, file.format);
        if (sqlite3_step(productStatement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法写入 OSB 产品");
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
                Bind(routeStatement.get(), 5, route.archiveClass);
                if (sqlite3_step(routeStatement.get()) != SQLITE_DONE) {
                    error = DbError(database.get(), L"无法写入 OSB 下载来源");
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

OsbAvailabilityLookup LoadOsbAvailability(const QueryDate& date) {
    std::scoped_lock lock(gDatabaseMutex);
    OsbAvailabilityLookup result;
    result.gpsWeek = CalculateOsbGpsWeek(date);
    result.sourcesExpected = kSourceCount;
    result.databasePath = GetOsbAvailabilityDatabasePath();
    DbPtr database;
    if (!OpenReadDatabase(result.databasePath, database, result.error)) return result;
    {
        auto statement = Prepare(database.get(),
            "SELECT status,source_success_count,source_expected_count,is_provisional FROM weekly_scans WHERE gps_week=?1;",
            result.error);
        if (!statement) return result;
        sqlite3_bind_int(statement.get(), 1, result.gpsWeek);
        if (sqlite3_step(statement.get()) != SQLITE_ROW) {
            result.error.clear();
            return result;
        }
        result.indexed = true;
        result.sourcesSuccessful = sqlite3_column_int(statement.get(), 1);
        result.sourcesExpected = sqlite3_column_int(statement.get(), 2);
        // A provisional weekly snapshot is intentionally not a stable
        // availability assertion.  Keep the single-day reader aligned with
        // the range reader and IsOsbWeekComplete so a bundle planner cannot
        // select routes from an in-flight week merely because all source
        // counters happened to match.
        result.complete = ColumnText(statement.get(), 0) == "complete"
            && result.sourcesExpected == kSourceCount
            && sqlite3_column_int(statement.get(), 3) == 0;
        result.provisional = sqlite3_column_int(statement.get(), 3) != 0;
    }
    const std::string dateText = DateText(date);
    auto products = Prepare(database.get(),
        "SELECT logical_key,coverage_start,coverage_end,start_hour,start_minute,series,center,project,solution,latency,duration,sample_interval,constellation,bias_class,format FROM products WHERE coverage_start<=?1 AND coverage_end>=?1 ORDER BY center,series,latency,sample_interval;",
        result.error);
    if (!products || !Bind(products.get(), 1, dateText)) return result;
    auto routes = Prepare(database.get(),
        "SELECT d.source_key,d.source_priority,s.file_name,s.download_url,s.archive_class FROM product_sources s JOIN source_definitions d ON d.source_id=s.source_id WHERE s.logical_key=?1 ORDER BY d.source_priority,s.file_name;",
        result.error);
    if (!routes) return result;
    while (sqlite3_step(products.get()) == SQLITE_ROW) {
        OsbProductRecord record;
        record.logicalKey = ColumnText(products.get(), 0);
        record.coverageStart = ParseDateText(ColumnText(products.get(), 1));
        record.coverageEnd = ParseDateText(ColumnText(products.get(), 2));
        record.startHour = sqlite3_column_int(products.get(), 3);
        record.startMinute = sqlite3_column_int(products.get(), 4);
        record.series = ColumnText(products.get(), 5);
        record.center = ColumnText(products.get(), 6);
        record.project = ColumnText(products.get(), 7);
        record.solution = ColumnText(products.get(), 8);
        record.latency = ColumnText(products.get(), 9);
        record.duration = ColumnText(products.get(), 10);
        record.interval = ColumnText(products.get(), 11);
        record.constellation = ColumnText(products.get(), 12);
        record.biasClass = ColumnText(products.get(), 13);
        record.format = ColumnText(products.get(), 14);
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
    return result;
}

OsbAvailabilityRangeLookup LoadOsbAvailabilityRange(
    const QueryDate& startDate, const QueryDate& endDate) {
    std::scoped_lock lock(gDatabaseMutex);
    OsbAvailabilityRangeLookup result;
    result.databasePath = GetOsbAvailabilityDatabasePath();
    const auto start = ToSysDays(startDate);
    const auto end = ToSysDays(endDate);
    result.expectedDays = start <= end ? static_cast<int>((end - start).count()) + 1 : 0;
    if (result.expectedDays <= 0) { result.error = L"OSB 日期范围无效"; return result; }
    DbPtr database;
    if (!OpenReadDatabase(result.databasePath, database, result.error)) return result;
    result.indexed = true;
    const int firstWeek = CalculateOsbGpsWeek(startDate), lastWeek = CalculateOsbGpsWeek(endDate);
    std::unordered_map<int, bool> completeWeeks;
    auto scans = Prepare(database.get(),
        "SELECT gps_week,status,source_success_count,source_expected_count,is_provisional FROM weekly_scans WHERE gps_week>=?1 AND gps_week<=?2;",
        result.error);
    if (!scans) return result;
    sqlite3_bind_int(scans.get(), 1, firstWeek); sqlite3_bind_int(scans.get(), 2, lastWeek);
    while (sqlite3_step(scans.get()) == SQLITE_ROW) {
        completeWeeks[sqlite3_column_int(scans.get(), 0)] = ColumnText(scans.get(), 1) == "complete"
            && sqlite3_column_int(scans.get(), 2) == kSourceCount
            && sqlite3_column_int(scans.get(), 3) == kSourceCount
            && sqlite3_column_int(scans.get(), 4) == 0;
    }
    for (int i = 0; i < result.expectedDays; ++i) {
        const auto day = FromSysDays(start + std::chrono::days{i});
        const auto it = completeWeeks.find(CalculateOsbGpsWeek(day));
        if (it != completeWeeks.end() && it->second) ++result.indexedDays;
        else result.incompleteDates.push_back(day);
    }
    auto products = Prepare(database.get(),
        "SELECT logical_key,coverage_start,coverage_end,start_hour,start_minute,series,center,project,solution,latency,duration,sample_interval,constellation,bias_class,format FROM products WHERE coverage_end>=?1 AND coverage_start<=?2 ORDER BY coverage_start,center,series,latency,sample_interval;",
        result.error);
    if (!products || !Bind(products.get(), 1, DateText(startDate)) || !Bind(products.get(), 2, DateText(endDate))) return result;
    auto routes = Prepare(database.get(),
        "SELECT d.source_key,d.source_priority,s.file_name,s.download_url,s.archive_class FROM product_sources s JOIN source_definitions d ON d.source_id=s.source_id WHERE s.logical_key=?1 ORDER BY d.source_priority,s.file_name;",
        result.error);
    if (!routes) return result;
    while (sqlite3_step(products.get()) == SQLITE_ROW) {
        OsbProductRecord record;
        record.logicalKey = ColumnText(products.get(), 0);
        record.coverageStart = ParseDateText(ColumnText(products.get(), 1)); record.coverageEnd = ParseDateText(ColumnText(products.get(), 2));
        record.startHour = sqlite3_column_int(products.get(), 3); record.startMinute = sqlite3_column_int(products.get(), 4);
        record.series = ColumnText(products.get(), 5); record.center = ColumnText(products.get(), 6); record.project = ColumnText(products.get(), 7); record.solution = ColumnText(products.get(), 8);
        record.latency = ColumnText(products.get(), 9); record.duration = ColumnText(products.get(), 10); record.interval = ColumnText(products.get(), 11); record.constellation = ColumnText(products.get(), 12); record.biasClass = ColumnText(products.get(), 13); record.format = ColumnText(products.get(), 14);
        sqlite3_reset(routes.get()); sqlite3_clear_bindings(routes.get());
        if (!Bind(routes.get(), 1, record.logicalKey)) { result.products.clear(); return result; }
        while (sqlite3_step(routes.get()) == SQLITE_ROW) record.routes.push_back({ColumnText(routes.get(),0), sqlite3_column_int(routes.get(),1), ColumnText(routes.get(),2), Utf8ToWide(ColumnText(routes.get(),3)), ColumnText(routes.get(),4)});
        result.products.push_back(std::move(record));
    }
    result.complete = result.indexedDays == result.expectedDays && result.incompleteDates.empty();
    return result;
}

OsbAvailabilityStats GetOsbAvailabilityStats() {
    std::scoped_lock lock(gDatabaseMutex);
    OsbAvailabilityStats result;
    result.databasePath = GetOsbAvailabilityDatabasePath();
    DbPtr database;
    if (!OpenDatabase(result.databasePath, database, result.error)) return result;
    auto statement = Prepare(database.get(),
        "SELECT SUM(status='complete'),SUM(status='partial'),MIN(gps_week),MAX(gps_week) FROM weekly_scans;",
        result.error);
    if (!statement) return result;
    if (sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.completeWeeks = sqlite3_column_int(statement.get(), 0);
        result.partialWeeks = sqlite3_column_int(statement.get(), 1);
        if (sqlite3_column_type(statement.get(), 2) != SQLITE_NULL) {
            result.firstGpsWeek = sqlite3_column_int(statement.get(), 2);
            result.lastGpsWeek = sqlite3_column_int(statement.get(), 3);
        }
    }
    auto count = Prepare(database.get(),
        "SELECT (SELECT COUNT(*) FROM products),(SELECT COUNT(*) FROM product_sources);",
        result.error);
    if (!count) return result;
    if (sqlite3_step(count.get()) == SQLITE_ROW) {
        result.products = sqlite3_column_int64(count.get(), 0);
        result.sourceRoutes = sqlite3_column_int64(count.get(), 1);
    }
    return result;
}

bool IsOsbWeekComplete(
    const int gpsWeek, bool& complete, std::wstring& error) {
    std::scoped_lock lock(gDatabaseMutex);
    complete = false;
    DbPtr database;
    if (!OpenDatabase(GetOsbAvailabilityDatabasePath(), database, error))
        return false;
    auto statement = Prepare(database.get(),
        "SELECT status,source_success_count,source_expected_count,is_provisional FROM weekly_scans WHERE gps_week=?1;",
        error);
    if (!statement) return false;
    sqlite3_bind_int(statement.get(), 1, gpsWeek);
    if (sqlite3_step(statement.get()) == SQLITE_ROW) {
        complete = ColumnText(statement.get(), 0) == "complete"
            && sqlite3_column_int(statement.get(), 1) == kSourceCount
            && sqlite3_column_int(statement.get(), 2) == kSourceCount
            && sqlite3_column_int(statement.get(), 3) == 0;
    }
    return true;
}

int OsbExpectedSourceCount() { return kSourceCount; }
