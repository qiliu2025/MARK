#include "SinexAvailability.h"

#include "DateUtil.h"
#include "HttpClient.h"
#include "IgsAvailabilityDb.h"
#include "Sp3Availability.h"

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
#include <regex>
#include <set>
#include <string_view>
#include <unordered_map>

namespace {

constexpr int kSourceCount = 6;
std::mutex gSinexDatabaseMutex;

struct SourceDefinition {
    const char* key;
    int priority;
    const wchar_t* root;
    bool ftp;
};

// KASI is intentionally not part of the strict set: its public FTP product
// tree has proved persistently unavailable.  Priorities keep their values
// compatible with the other product databases.
constexpr SourceDefinition kSources[] {
    {"CDDIS", 1, L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/products/", true},
    {"BKG", 2, L"https://igs.bkg.bund.de/root_ftp/IGS/products/", false},
    {"WHU", 3, L"ftp://igs.gnsswhu.cn/pub/gps/products/", true},
    {"IGN", 5, L"ftp://igs.ign.fr/pub/igs/products/", true},
    {"ESA", 6, L"ftp://gssc.esa.int/gnss/products/", true},
    {"SOPAC", 7, L"http://garner.ucsd.edu/pub/products/", false},
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

QueryDate DateFromDoy(const int year, const int dayOfYear) {
    return FromSysDays(std::chrono::sys_days{
        std::chrono::year{year} / std::chrono::January / 1}
        + std::chrono::days{dayOfYear - 1});
}

std::string DateText(const QueryDate& date) {
    return std::format("{:04}-{:02}-{:02}",
        date.year, date.month, date.day);
}

bool ValidDoy(const int year, const int dayOfYear) {
    return year >= 1980 && dayOfYear >= 1
        && dayOfYear <= (IsLeapYear(year) ? 366 : 365);
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

bool IsTransportFailure(const long code) {
    return code == 5 || code == 6 || code == 7 || code == 28 || code == 35
        || code == 52 || code == 55 || code == 56;
}

std::wstring JoinUrl(const std::wstring& directory,
                     const std::string& fileName) {
    std::wstring result = directory;
    if (!result.ends_with(L'/')) result.push_back(L'/');
    result += Utf8ToWide(fileName);
    return result;
}

std::string ArchiveProject(const std::string& archiveClass) {
    const std::string value = Upper(archiveClass);
    if (value == "REPRO1") return "R01";
    if (value == "REPRO2") return "R02";
    if (value == "REPRO3") return "R03";
    if (value == "MGEX") return "MGX";
    return "OPS";
}

std::string LatencyFor(const std::string& project,
                       const std::string& solution) {
    const std::string p = Upper(project);
    const std::string s = Upper(solution);
    if (p.starts_with('R')) return "FINAL";
    if (s == "RAP") return "RAPID";
    if (s == "ULT") return "ULTRA_RAPID";
    if (s == "NRT") return "NEAR_REAL_TIME";
    if (s == "RTS") return "REAL_TIME";
    // SNX denotes an official combined SINEX stream rather than latency;
    // these archived combinations are final products.
    if (s == "FIN" || s == "SNX" || s == "G--") return "FINAL";
    return "OTHER";
}

int DurationMinutes(const std::string& duration) {
    if (duration.size() != 3
        || !std::isdigit(static_cast<unsigned char>(duration[0]))
        || !std::isdigit(static_cast<unsigned char>(duration[1]))) return 0;
    const int value = (duration[0] - '0') * 10 + duration[1] - '0';
    switch (static_cast<char>(std::toupper(
        static_cast<unsigned char>(duration[2])))) {
    case 'D': return value * 24 * 60;
    case 'H': return value * 60;
    case 'M': return value;
    default: return 0;
    }
}

QueryDate CoverageEnd(const QueryDate& start, const int hour,
                      const int minute, const std::string& duration) {
    const int length = DurationMinutes(duration);
    if (length <= 0) return start;
    const int inclusiveDayOffset =
        (hour * 60 + minute + length - 1) / (24 * 60);
    return FromSysDays(ToSysDays(start)
        + std::chrono::days{inclusiveDayOffset});
}

std::string LogicalKey(std::string fileName, const int expectedGpsWeek,
                       const bool weekless) {
    std::string lower = Lower(std::move(fileName));
    if (lower.ends_with(".gz")) lower.resize(lower.size() - 3);
    else if (lower.ends_with(".z")) lower.resize(lower.size() - 2);
    return weekless ? std::format("{}|{}", expectedGpsWeek, lower) : lower;
}

bool ParseModern(const std::string& name, const int expectedGpsWeek,
                 SinexArchiveFile& file) {
    static const std::regex regular(
        R"(^([A-Z0-9]{10})_([0-9]{4})([0-9]{3})([0-9]{2})([0-9]{2})_([0-9A-Z]{3})_([0-9A-Z]{3})_(SOL|CRD)\.SNX(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    static const std::regex cumulative(
        R"(^([A-Z0-9]{10})_([0-9]{4})([0-9]{3})_([0-9]{4})([0-9]{3})_00U_(SOL|CRD)\.SNX(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    std::smatch match;
    if (std::regex_match(name, match, regular)) {
        const int year = std::stoi(match[2].str());
        const int dayOfYear = std::stoi(match[3].str());
        if (!ValidDoy(year, dayOfYear)) return false;
        file.nominalDate = DateFromDoy(year, dayOfYear);
        if (CalculateGpsWeek(file.nominalDate) != expectedGpsWeek) return false;
        file.coverageStart = file.nominalDate;
        file.startHour = std::stoi(match[4].str());
        file.startMinute = std::stoi(match[5].str());
        file.series = Upper(match[1].str());
        file.streamCode = file.series;
        file.center = file.series.substr(0, 3);
        file.project = file.series.substr(4, 3);
        file.solution = file.series.substr(7, 3);
        file.duration = Upper(match[6].str());
        file.interval = Upper(match[7].str());
        file.contentType = Upper(match[8].str());
        file.format = "SINEX";
        file.productClass = file.duration == "01D"
            ? "DAILY" : (file.duration == "07D" ? "WEEKLY" : "MULTI_DAY");
        file.coverageEnd = CoverageEnd(file.coverageStart, file.startHour,
            file.startMinute, file.duration);
        file.latency = LatencyFor(file.project, file.solution);
        file.logicalKey = LogicalKey(name, expectedGpsWeek, false);
        return true;
    }
    if (!std::regex_match(name, match, cumulative)) return false;
    const int startYear = std::stoi(match[2].str());
    const int startDoy = std::stoi(match[3].str());
    const int endYear = std::stoi(match[4].str());
    const int endDoy = std::stoi(match[5].str());
    if (!ValidDoy(startYear, startDoy) || !ValidDoy(endYear, endDoy))
        return false;
    file.coverageStart = DateFromDoy(startYear, startDoy);
    file.coverageEnd = DateFromDoy(endYear, endDoy);
    file.nominalDate = file.coverageEnd;
    if (CalculateGpsWeek(file.nominalDate) != expectedGpsWeek) return false;
    file.series = Upper(match[1].str());
    file.streamCode = file.series;
    file.center = file.series.substr(0, 3);
    file.project = file.series.substr(4, 3);
    file.solution = file.series.substr(7, 3);
    file.duration = "00U";
    file.interval = "00U";
    file.contentType = Upper(match[6].str());
    file.format = "SINEX";
    file.productClass = "CUMULATIVE";
    file.latency = LatencyFor(file.project, file.solution);
    file.logicalKey = LogicalKey(name, expectedGpsWeek, false);
    return true;
}

std::string LegacyCenter(const std::string& rawPrefix) {
    const std::string p = Lower(rawPrefix);
    const std::array<std::pair<std::string_view, std::string_view>, 19> map{{
        {"igs", "IGS"}, {"eur", "EUR"}, {"cod", "COD"},
        {"co", "COD"}, {"em", "EMR"}, {"es", "ESA"},
        {"gf", "GFZ"}, {"gr", "GRG"}, {"jp", "JPL"},
        {"mi", "MIT"}, {"nc", "NCL"}, {"ng", "NGS"},
        {"si", "SIO"}, {"ul", "ULR"}, {"pd", "PDR"},
        {"tu", "TUG"}, {"wh", "WHU"}, {"gsi", "GSI"},
        {"sir", "SIR"},
    }};
    for (const auto& [prefix, center] : map) {
        if (p.starts_with(prefix)) return std::string(center);
    }
    std::string letters;
    for (const unsigned char c : p) {
        if (!std::isalpha(c)) break;
        letters.push_back(static_cast<char>(c));
        if (letters.size() == 3) break;
    }
    if (letters.empty()) return "UNK";
    return Upper(letters);
}

bool ParseLegacy(const std::string& name, const int expectedGpsWeek,
                 const std::string& archiveClass, SinexArchiveFile& file) {
    static const std::regex legacyCandidate(
        R"(^([A-Z0-9_.-]+)\.(SNX|SSC)(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(name, match, legacyCandidate)) return false;
    const std::string base = match[1].str();
    const std::string lowerBase = Lower(base);
    const std::string weekText = std::format("{:04}", expectedGpsWeek);
    const std::size_t weekPosition = lowerBase.find(weekText);
    bool weekless = false;
    int day = 7;
    std::string rawPrefix;
    if (weekPosition != std::string::npos) {
        rawPrefix = base.substr(0, weekPosition);
        const std::size_t suffixPosition = weekPosition + weekText.size();
        if (suffixPosition < base.size()
            && base[suffixPosition] >= '0' && base[suffixPosition] <= '7') {
            day = base[suffixPosition] - '0';
        }
    } else {
        static const std::regex oldCumulative(
            R"(^IGS[0-9]{2}P[0-9]{2}(?:_ALL)?$)", std::regex::icase);
        if (!std::regex_match(base, oldCumulative)) return false;
        rawPrefix = "IGS";
        weekless = true;
    }
    while (!rawPrefix.empty()
        && (rawPrefix.back() == '_' || rawPrefix.back() == '-')) {
        rawPrefix.pop_back();
    }
    file.streamCode = Upper(rawPrefix.empty() ? base : rawPrefix);
    file.center = LegacyCenter(file.streamCode);
    file.project = ArchiveProject(archiveClass);
    file.solution = file.center == "IGS" || file.center == "EUR"
        ? "SNX" : "FIN";
    file.series = file.center + "0" + file.project + file.solution;
    file.startHour = 0;
    file.startMinute = 0;
    const QueryDate weekStart = GpsWeekStartDate(expectedGpsWeek);
    if (day >= 0 && day <= 6) {
        file.nominalDate = FromSysDays(ToSysDays(weekStart)
            + std::chrono::days{day});
        file.coverageStart = file.nominalDate;
        file.coverageEnd = file.nominalDate;
        file.duration = "01D";
        file.productClass = "DAILY";
    } else {
        file.nominalDate = weekStart;
        file.coverageStart = weekStart;
        file.coverageEnd = FromSysDays(ToSysDays(weekStart)
            + std::chrono::days{6});
        file.duration = weekless ? "00U" : "07D";
        file.productClass = weekless ? "CUMULATIVE_LEGACY" : "WEEKLY";
    }
    file.interval = "01D";
    file.format = Upper(match[2].str()) == "SSC" ? "SSC" : "SINEX";
    file.contentType = file.format == "SSC" ? "CRD" : "SOL";
    file.latency = "FINAL";
    file.logicalKey = LogicalKey(name, expectedGpsWeek, weekless);
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
        error = DbError(database, L"无法准备 SINEX 知识库查询");
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
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    if (code) {
        error = L"无法创建 SINEX 知识库目录："
            + path.parent_path().wstring();
        return false;
    }
    sqlite3* raw = nullptr;
    const int status = sqlite3_open16(path.c_str(), &raw);
    database.reset(raw);
    if (status != SQLITE_OK || !raw) {
        error = DbError(raw, L"无法打开 SINEX 知识库");
        return false;
    }
    sqlite3_busy_timeout(raw, 10000);
    const char* schema =
        "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS source_definitions(source_id INTEGER PRIMARY KEY,source_key TEXT NOT NULL UNIQUE,source_priority INTEGER NOT NULL UNIQUE);"
        "CREATE TABLE IF NOT EXISTS weekly_scans(gps_week INTEGER PRIMARY KEY,status TEXT NOT NULL CHECK(status IN('complete','partial')),source_success_count INTEGER NOT NULL,source_expected_count INTEGER NOT NULL,product_count INTEGER NOT NULL,is_provisional INTEGER NOT NULL,scanned_at_utc TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS weekly_source_scans(gps_week INTEGER NOT NULL,source_id INTEGER NOT NULL,status TEXT NOT NULL CHECK(status IN('complete','failed')),listing_urls TEXT NOT NULL,file_count INTEGER NOT NULL,error_message TEXT NOT NULL,PRIMARY KEY(gps_week,source_id),FOREIGN KEY(gps_week) REFERENCES weekly_scans(gps_week) ON DELETE CASCADE,FOREIGN KEY(source_id) REFERENCES source_definitions(source_id));"
        "CREATE TABLE IF NOT EXISTS products(logical_key TEXT PRIMARY KEY,gps_week INTEGER NOT NULL,nominal_date TEXT NOT NULL,coverage_start TEXT NOT NULL,coverage_end TEXT NOT NULL,start_hour INTEGER NOT NULL,start_minute INTEGER NOT NULL,series TEXT NOT NULL,stream_code TEXT NOT NULL,center TEXT NOT NULL,project TEXT NOT NULL,solution TEXT NOT NULL,latency TEXT NOT NULL,duration TEXT NOT NULL,sample_interval TEXT NOT NULL,content_type TEXT NOT NULL,format TEXT NOT NULL,product_class TEXT NOT NULL,FOREIGN KEY(gps_week) REFERENCES weekly_scans(gps_week) ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS products_week_coverage ON products(gps_week,coverage_start,coverage_end,center,content_type);"
        "CREATE TABLE IF NOT EXISTS product_sources(logical_key TEXT NOT NULL,source_id INTEGER NOT NULL,file_name TEXT NOT NULL,download_url TEXT NOT NULL,archive_class TEXT NOT NULL,PRIMARY KEY(logical_key,source_id,file_name,download_url),FOREIGN KEY(logical_key) REFERENCES products(logical_key) ON DELETE CASCADE,FOREIGN KEY(source_id) REFERENCES source_definitions(source_id));"
        "INSERT OR IGNORE INTO source_definitions VALUES(1,'CDDIS',1),(2,'BKG',2),(3,'WHU',3),(5,'IGN',5),(6,'ESA',6),(7,'SOPAC',7);"
        "INSERT INTO metadata VALUES('schema_version','1') ON CONFLICT(key) DO UPDATE SET value='1';"
        "INSERT INTO metadata VALUES('product_type','SINEX') ON CONFLICT(key) DO UPDATE SET value='SINEX';";
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
        error = DbError(raw, L"无法以只读方式打开 SINEX 知识库");
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

} // namespace

int SinexExpectedSourceCount() { return kSourceCount; }

std::vector<SinexArchiveFile> ParseSinexDirectoryListing(
    const std::string& listing, const int expectedGpsWeek,
    const std::wstring& directoryUrl, const std::string& archiveClass) {
    static const std::regex candidate(
        R"(([A-Za-z0-9][A-Za-z0-9_.-]*\.(?:snx|ssc)(?:\.(?:gz|Z))?)(?=$|[^A-Za-z0-9_.-]))",
        std::regex::icase);
    std::set<std::string> seen;
    std::vector<SinexArchiveFile> result;
    for (std::sregex_iterator iterator(listing.begin(), listing.end(), candidate),
         end; iterator != end; ++iterator) {
        std::string name = (*iterator)[1].str();
        if (!seen.insert(Lower(name)).second) continue;
        SinexArchiveFile file;
        if (!ParseModern(name, expectedGpsWeek, file)
            && !ParseLegacy(name, expectedGpsWeek, archiveClass, file)) {
            continue;
        }
        file.fileName = std::move(name);
        file.downloadUrl = JoinUrl(directoryUrl, file.fileName);
        file.archiveClass = archiveClass;
        result.push_back(std::move(file));
    }
    return result;
}

std::vector<SinexSourceScan> QuerySinexWeek(
    const int gpsWeek, const std::stop_token stopToken) {
    std::vector<SinexSourceScan> scans;
    scans.reserve(kSourceCount);
    for (const auto& source : kSources) {
        SinexSourceScan scan;
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
                auto files = ParseSinexDirectoryListing(
                    response.body, gpsWeek, url, archiveClass);
                scan.files.insert(scan.files.end(),
                    std::make_move_iterator(files.begin()),
                    std::make_move_iterator(files.end()));
                continue;
            }
            if (MissingDirectory(response)) continue;
            fatal = true;
            scan.transportErrorCode = response.transportErrorCode;
            scan.networkFailure = IsTransportFailure(
                response.transportErrorCode);
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

std::filesystem::path GetSinexAvailabilityDatabasePath() {
    const auto explicitPath = EnvironmentPath(
        L"GNSS_DOWNLOADER_SINEX_DB_PATH");
    if (!explicitPath.empty()) return explicitPath;
    const auto directory = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
    if (!directory.empty()) {
        return ResolveDatabaseFilePath(directory / L"sinex_availability.sqlite");
    }
    return ResolveDatabaseFilePath(GetIgsAvailabilityDatabasePath().parent_path()
        / L"sinex_availability.sqlite");
}

bool StoreSinexWeek(const int gpsWeek,
                    const std::vector<SinexSourceScan>& sources,
                    std::wstring& error) {
    std::scoped_lock lock(gSinexDatabaseMutex);
    const int successful = static_cast<int>(std::ranges::count_if(
        sources, [](const auto& source) { return source.success; }));
    if (successful == 0) {
        error = L"六个 SINEX 数据中心均未读取成功";
        return false;
    }
    std::map<std::string, SinexArchiveFile> products;
    for (const auto& source : sources) if (source.success) {
        for (const auto& file : source.files)
            products.try_emplace(file.logicalKey, file);
    }
    DbPtr database;
    if (!OpenDatabase(GetSinexAvailabilityDatabasePath(), database, error))
        return false;
    int existing = -1;
    {
        auto statement = Prepare(database.get(),
            "SELECT source_success_count FROM weekly_scans WHERE gps_week=?1;",
            error);
        if (!statement) return false;
        sqlite3_bind_int(statement.get(), 1, gpsWeek);
        if (sqlite3_step(statement.get()) == SQLITE_ROW)
            existing = sqlite3_column_int(statement.get(), 0);
    }
    if (existing > successful
        || (existing == successful && successful < kSourceCount)) return true;
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
            error = DbError(database.get(), L"无法替换 SINEX 周记录");
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
            error = DbError(database.get(), L"无法写入 SINEX 周记录");
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
            error = DbError(database.get(), L"无法写入 SINEX 来源状态");
            rollback();
            return false;
        }
    }
    auto productStatement = Prepare(database.get(),
        "INSERT INTO products VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18);",
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
        Bind(productStatement.get(), 9, file.streamCode);
        Bind(productStatement.get(), 10, file.center);
        Bind(productStatement.get(), 11, file.project);
        Bind(productStatement.get(), 12, file.solution);
        Bind(productStatement.get(), 13, file.latency);
        Bind(productStatement.get(), 14, file.duration);
        Bind(productStatement.get(), 15, file.interval);
        Bind(productStatement.get(), 16, file.contentType);
        Bind(productStatement.get(), 17, file.format);
        Bind(productStatement.get(), 18, file.productClass);
        if (sqlite3_step(productStatement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法写入 SINEX 产品");
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
                        L"无法写入 SINEX 下载来源");
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

SinexAvailabilityLookup LoadSinexAvailability(const QueryDate& date) {
    std::scoped_lock lock(gSinexDatabaseMutex);
    SinexAvailabilityLookup result;
    result.gpsWeek = CalculateGpsWeek(date);
    result.sourcesExpected = kSourceCount;
    result.databasePath = GetSinexAvailabilityDatabasePath();
    DbPtr database;
    if (!OpenReadDatabase(result.databasePath, database, result.error))
        return result;
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
        result.complete = ColumnText(statement.get(), 0) == "complete";
        result.sourcesSuccessful = sqlite3_column_int(statement.get(), 1);
        result.sourcesExpected = sqlite3_column_int(statement.get(), 2);
        result.provisional = sqlite3_column_int(statement.get(), 3) != 0;
    }
    auto products = Prepare(database.get(),
        "SELECT logical_key,nominal_date,coverage_start,coverage_end,start_hour,start_minute,series,stream_code,center,project,solution,latency,duration,sample_interval,content_type,format,product_class FROM products WHERE gps_week=?1 AND coverage_start<=?2 AND coverage_end>=?2 ORDER BY latency,center,content_type,series,start_hour;",
        result.error);
    if (!products) return result;
    sqlite3_bind_int(products.get(), 1, result.gpsWeek);
    if (!Bind(products.get(), 2, DateText(date))) return result;
    auto routes = Prepare(database.get(),
        "SELECT d.source_key,d.source_priority,s.file_name,s.download_url,s.archive_class FROM product_sources s JOIN source_definitions d ON d.source_id=s.source_id WHERE s.logical_key=?1 ORDER BY d.source_priority,s.file_name;",
        result.error);
    if (!routes) return result;
    while (sqlite3_step(products.get()) == SQLITE_ROW) {
        SinexProductRecord record;
        record.logicalKey = ColumnText(products.get(), 0);
        const auto parseDate = [](const std::string& text) {
            int year = 0;
            int month = 0;
            int day = 0;
            if (sscanf_s(text.c_str(), "%d-%d-%d",
                    &year, &month, &day) != 3) return QueryDate{};
            return MakeQueryDate(year, month, day);
        };
        record.nominalDate = parseDate(ColumnText(products.get(), 1));
        record.coverageStart = parseDate(ColumnText(products.get(), 2));
        record.coverageEnd = parseDate(ColumnText(products.get(), 3));
        record.startHour = sqlite3_column_int(products.get(), 4);
        record.startMinute = sqlite3_column_int(products.get(), 5);
        record.series = ColumnText(products.get(), 6);
        record.streamCode = ColumnText(products.get(), 7);
        record.center = ColumnText(products.get(), 8);
        record.project = ColumnText(products.get(), 9);
        record.solution = ColumnText(products.get(), 10);
        record.latency = ColumnText(products.get(), 11);
        record.duration = ColumnText(products.get(), 12);
        record.interval = ColumnText(products.get(), 13);
        record.contentType = ColumnText(products.get(), 14);
        record.format = ColumnText(products.get(), 15);
        record.productClass = ColumnText(products.get(), 16);
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

bool IsSinexWeekComplete(const int gpsWeek, bool& complete,
                         std::wstring& error) {
    std::scoped_lock lock(gSinexDatabaseMutex);
    complete = false;
    DbPtr database;
    if (!OpenDatabase(GetSinexAvailabilityDatabasePath(), database, error))
        return false;
    auto statement = Prepare(database.get(),
        "SELECT status,is_provisional FROM weekly_scans WHERE gps_week=?1;",
        error);
    if (!statement) return false;
    sqlite3_bind_int(statement.get(), 1, gpsWeek);
    if (sqlite3_step(statement.get()) == SQLITE_ROW) {
        complete = ColumnText(statement.get(), 0) == "complete"
            && sqlite3_column_int(statement.get(), 1) == 0;
    }
    return true;
}

SinexAvailabilityStats GetSinexAvailabilityStats() {
    std::scoped_lock lock(gSinexDatabaseMutex);
    SinexAvailabilityStats result;
    result.databasePath = GetSinexAvailabilityDatabasePath();
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
        "SELECT (SELECT COUNT(*) FROM products),(SELECT COUNT(*) FROM product_sources);",
        result.error);
    if (counts && sqlite3_step(counts.get()) == SQLITE_ROW) {
        result.products = sqlite3_column_int64(counts.get(), 0);
        result.sourceRoutes = sqlite3_column_int64(counts.get(), 1);
    }
    return result;
}
