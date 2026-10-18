#include "DcbAvailability.h"

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
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <string_view>

namespace {

constexpr int kSourceCount = 4;

struct SourceDefinition {
    const char* key;
    int priority;
};

constexpr std::array kSources{
    SourceDefinition{"CDDIS", 1},
    SourceDefinition{"IGN", 2},
    SourceDefinition{"CAS", 3},
    SourceDefinition{"CODE", 4},
};

struct ListingDefinition {
    std::wstring listingUrl;
    std::wstring downloadDirectory;
    bool ftp = false;
    bool codeArchive = false;
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
    const std::chrono::year_month_day first{
        std::chrono::year{start.year}
        / std::chrono::month{static_cast<unsigned>(start.month)}
        / std::chrono::day{static_cast<unsigned>(start.day)}};
    if (unit == 'L') {
        const auto after = std::chrono::sys_days{first + std::chrono::months{count}};
        return FromSysDays(after - std::chrono::days{1});
    }
    if (unit == 'Y') {
        const auto after = std::chrono::sys_days{first + std::chrono::years{count}};
        return FromSysDays(after - std::chrono::days{1});
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
        || code == 52 || code == 55 || code == 56;
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

std::string NormalizedSeries(const std::string& raw) {
    if (raw.size() != 10) return Upper(raw);
    return Upper(raw.substr(0, 4) + "OPS" + raw.substr(7, 3));
}

std::string LogicalKey(const DcbArchiveFile& file) {
    return file.series + "|" + file.variant + "|"
        + DateText(file.coverageStart) + "|" + DateText(file.coverageEnd)
        + "|" + file.interval + "|" + file.signalPair + "|" + file.scope;
}

bool ParseLongName(
    const std::string& name, const int expectedYear, DcbArchiveFile& file) {
    static const std::regex pattern(
        R"(^([A-Z0-9]{10})_([0-9]{4})([0-9]{3})([0-9]{2})([0-9]{2})_([0-9]{2}[DHMSUWLY])_([0-9]{2}[DHMSUWLY])_DCB\.(BIA|BSX)(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) return false;
    const int year = std::stoi(match[2].str());
    const int doy = std::stoi(match[3].str());
    if (year != expectedYear || doy < 1
        || doy > (IsLeapYear(year) ? 366 : 365)) return false;
    const std::string rawSeries = Upper(match[1].str());
    const std::string center = rawSeries.substr(0, 3);
    if (center != "CAS" && center != "DLR" && center != "GFZ") return false;
    file.coverageStart = DateFromDoy(year, doy);
    file.duration = Upper(match[6].str());
    file.coverageEnd = EndForDuration(file.coverageStart, file.duration);
    file.interval = Upper(match[7].str());
    file.series = NormalizedSeries(rawSeries);
    file.center = center;
    file.variant = rawSeries.substr(0, 4);
    file.latency = rawSeries.substr(7, 3) == "FIN" ? "FINAL" : "RAPID";
    file.signalPair = "MULTI";
    file.format = Upper(match[8].str()) == "BIA"
        ? "BIAS_SINEX_BIA" : "BIAS_SINEX_BSX";
    if (center == "GFZ") {
        file.constellation = "GECJ";
        file.scope = "SATELLITE_STATION";
    } else if (center == "DLR") {
        file.constellation = "GRECJ";
        file.scope = file.interval == "07D"
            ? "SATELLITE" : "SATELLITE_STATION";
    } else {
        file.constellation = "GRECJ";
        file.scope = "SATELLITE_STATION";
    }
    file.logicalKey = LogicalKey(file);
    return true;
}

bool ParseCodeName(
    const std::string& name, const int expectedYear, DcbArchiveFile& file) {
    static const std::regex pattern(
        R"(^(P1C1|P1P2|P2C2)([0-9]{2})([0-9]{2})(?:_(ALL|RINEX|GPS))?\.DCB(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) return false;
    const int year = 2000 + std::stoi(match[2].str());
    const int adjustedYear = year > expectedYear + 50 ? year - 100 : year;
    const int month = std::stoi(match[3].str());
    if (adjustedYear != expectedYear || month < 1 || month > 12) return false;
    const std::string pair = Upper(match[1].str());
    const std::string qualifier = match[4].matched ? Upper(match[4].str()) : "";
    file.coverageStart = MakeQueryDate(adjustedYear, month, 1);
    file.duration = "01L";
    file.coverageEnd = EndForDuration(file.coverageStart, file.duration);
    file.interval = "01M";
    file.series = "COD0LEGFIN";
    file.center = "COD";
    file.variant = qualifier.empty() ? pair : pair + "_" + qualifier;
    file.latency = "FINAL";
    file.constellation = pair == "P1C1" && qualifier.empty() ? "G" : "GR";
    file.signalPair = pair;
    file.scope = qualifier == "ALL" || qualifier == "RINEX"
        ? "SATELLITE_STATION" : "SATELLITE";
    file.format = "BERNESE_DCB";
    file.logicalKey = LogicalKey(file);
    return true;
}

std::vector<ListingDefinition> ListingsFor(
    const SourceDefinition& source, const int year) {
    const std::wstring yearText = std::to_wstring(year);
    if (std::string_view(source.key) == "CDDIS") {
        return {
            {L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/products/bias/"
                + yearText + L"/", L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/products/bias/"
                + yearText + L"/", true, false},
            {L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/products/mgex/dcb/"
                + yearText + L"/", L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/products/mgex/dcb/"
                + yearText + L"/", true, false}
        };
    }
    if (std::string_view(source.key) == "IGN") {
        return {
            {L"ftp://igs.ign.fr/pub/igs/products/bias/" + yearText + L"/",
                L"ftp://igs.ign.fr/pub/igs/products/bias/" + yearText + L"/",
                true, false},
            {L"ftp://igs.ign.fr/pub/igs/products/mgex/dcb/" + yearText + L"/",
                L"ftp://igs.ign.fr/pub/igs/products/mgex/dcb/" + yearText + L"/",
                true, false}
        };
    }
    if (std::string_view(source.key) == "CAS") {
        const std::wstring directory =
            L"https://data.bdsmart.cn/pub/product/bias/" + yearText + L"/";
        return {{directory, directory, false, false}};
    }
    const std::wstring listing =
        L"https://code.aiub.unibe.ch/s3_script/aiub_s3_bucket_listing.php?path=CODE%2F"
        + yearText;
    const std::wstring downloads =
        L"https://www.aiub.unibe.ch/download/CODE/" + yearText + L"/";
    return {{listing, downloads, false, true}};
}

DcbSourceScan QuerySource(
    const SourceDefinition& source, const int year,
    const std::stop_token stopToken) {
    DcbSourceScan scan;
    scan.sourceKey = source.key;
    scan.sourcePriority = source.priority;
    bool fatal = false;
    for (const auto& definition : ListingsFor(source, year)) {
        scan.listingUrls.push_back(definition.listingUrl);
        const HttpTextResult response = definition.ftp
            ? HttpListFtpDirectory(definition.listingUrl, stopToken,
                64U * 1024U * 1024U)
            : HttpGetText(definition.listingUrl, stopToken,
                64U * 1024U * 1024U);
        if (response.ok()) {
            auto files = ParseDcbDirectoryListing(
                response.body, year, definition.downloadDirectory,
                definition.codeArchive);
            scan.files.insert(scan.files.end(),
                std::make_move_iterator(files.begin()),
                std::make_move_iterator(files.end()));
            continue;
        }
        if (MissingDirectory(response)) continue;
        fatal = true;
        scan.transportErrorCode = response.transportErrorCode;
        scan.networkFailure = scan.networkFailure || IsNetworkFailure(response);
        if (!scan.error.empty()) scan.error += L"；";
        scan.error += response.error.empty()
            ? std::format(L"服务器返回 HTTP {}", response.statusCode)
            : response.error;
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
        error = DbError(database, L"无法准备 DCB 知识库查询");
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
        error = L"无法创建 DCB 知识库目录：" + path.parent_path().wstring();
        return false;
    }
    sqlite3* raw = nullptr;
    const int status = sqlite3_open16(path.c_str(), &raw);
    database.reset(raw);
    if (status != SQLITE_OK || raw == nullptr) {
        error = DbError(raw, L"无法打开 DCB 知识库");
        return false;
    }
    sqlite3_busy_timeout(raw, 10000);
    const char* schema =
        "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS source_definitions(source_id INTEGER PRIMARY KEY,source_key TEXT NOT NULL UNIQUE,source_priority INTEGER NOT NULL UNIQUE);"
        "CREATE TABLE IF NOT EXISTS yearly_scans(archive_year INTEGER PRIMARY KEY,status TEXT NOT NULL CHECK(status IN('complete','partial')),source_success_count INTEGER NOT NULL,source_expected_count INTEGER NOT NULL,product_count INTEGER NOT NULL,is_provisional INTEGER NOT NULL,scanned_at_utc TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS yearly_source_scans(archive_year INTEGER NOT NULL,source_id INTEGER NOT NULL,status TEXT NOT NULL CHECK(status IN('complete','failed')),listing_urls TEXT NOT NULL,file_count INTEGER NOT NULL,error_message TEXT NOT NULL,PRIMARY KEY(archive_year,source_id),FOREIGN KEY(archive_year) REFERENCES yearly_scans(archive_year) ON DELETE CASCADE,FOREIGN KEY(source_id) REFERENCES source_definitions(source_id));"
        "CREATE TABLE IF NOT EXISTS products(logical_key TEXT PRIMARY KEY,archive_year INTEGER NOT NULL,coverage_start TEXT NOT NULL,coverage_end TEXT NOT NULL,series TEXT NOT NULL,center TEXT NOT NULL,variant TEXT NOT NULL,latency TEXT NOT NULL,duration TEXT NOT NULL,sample_interval TEXT NOT NULL,constellation TEXT NOT NULL,signal_pair TEXT NOT NULL,scope TEXT NOT NULL,format TEXT NOT NULL,FOREIGN KEY(archive_year) REFERENCES yearly_scans(archive_year) ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS products_coverage ON products(coverage_start,coverage_end,center,latency);"
        "CREATE TABLE IF NOT EXISTS product_sources(logical_key TEXT NOT NULL,source_id INTEGER NOT NULL,file_name TEXT NOT NULL,download_url TEXT NOT NULL,PRIMARY KEY(logical_key,source_id,file_name,download_url),FOREIGN KEY(logical_key) REFERENCES products(logical_key) ON DELETE CASCADE,FOREIGN KEY(source_id) REFERENCES source_definitions(source_id));"
        "INSERT OR IGNORE INTO source_definitions VALUES(1,'CDDIS',1),(2,'IGN',2),(3,'CAS',3),(4,'CODE',4);"
        "INSERT INTO metadata VALUES('schema_version','1') ON CONFLICT(key) DO UPDATE SET value='1';"
        "INSERT INTO metadata VALUES('product_type','dcb') ON CONFLICT(key) DO UPDATE SET value='dcb';"
        "INSERT INTO metadata VALUES('excludes_osb','1') ON CONFLICT(key) DO UPDATE SET value='1';";
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
        error = DbError(raw, L"无法以只读方式打开 DCB 知识库");
        return false;
    }
    sqlite3_busy_timeout(raw, 10000);
    return true;
}

bool ProvisionalYear(const int year) {
    const auto today = std::chrono::year_month_day{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    return year >= static_cast<int>(today.year());
}

} // namespace

std::vector<DcbArchiveFile> ParseDcbDirectoryListing(
    const std::string& listing, const int expectedYear,
    const std::wstring& directoryUrl, const bool codeArchive) {
    static const std::regex candidate(
        R"(([A-Za-z0-9][A-Za-z0-9_.-]*(?:_DCB\.(?:BIA|BSX)|\.DCB)(?:\.(?:gz|Z))?)(?=$|[^A-Za-z0-9_.-]))",
        std::regex::icase);
    std::set<std::string> seen;
    std::vector<DcbArchiveFile> result;
    for (std::sregex_iterator it(listing.begin(), listing.end(), candidate), end;
         it != end; ++it) {
        std::string name = (*it)[1].str();
        if (!seen.insert(Lower(name)).second) continue;
        DcbArchiveFile file;
        const bool parsed = codeArchive
            ? ParseCodeName(name, expectedYear, file)
            : ParseLongName(name, expectedYear, file);
        if (!parsed) continue;
        file.fileName = std::move(name);
        file.downloadUrl = JoinUrl(directoryUrl, file.fileName);
        result.push_back(std::move(file));
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        if (left.logicalKey != right.logicalKey)
            return left.logicalKey < right.logicalKey;
        return left.fileName < right.fileName;
    });
    return result;
}

std::vector<DcbSourceScan> QueryDcbYear(
    const int year, const std::stop_token stopToken) {
    std::vector<std::future<DcbSourceScan>> futures;
    futures.reserve(kSources.size());
    for (const auto& source : kSources) {
        futures.push_back(std::async(std::launch::async, [=, &stopToken]() {
            return QuerySource(source, year, stopToken);
        }));
    }
    std::vector<DcbSourceScan> scans;
    scans.reserve(kSources.size());
    for (auto& future : futures) scans.push_back(future.get());
    std::ranges::sort(scans, [](const auto& left, const auto& right) {
        return left.sourcePriority < right.sourcePriority;
    });
    return scans;
}

std::filesystem::path GetDcbAvailabilityDatabasePath() {
    const auto explicitPath = EnvironmentPath(L"GNSS_DOWNLOADER_DCB_DB_PATH");
    if (!explicitPath.empty()) return explicitPath;
    const auto directory = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
    if (!directory.empty()) {
        return ResolveDatabaseFilePath(directory / L"dcb_availability.sqlite");
    }
    return ResolveDatabaseFilePath(GetIgsAvailabilityDatabasePath().parent_path()
        / L"dcb_availability.sqlite");
}

bool StoreDcbYear(
    const int year, const std::vector<DcbSourceScan>& sources,
    std::wstring& error) {
    std::scoped_lock lock(gDatabaseMutex);
    const int successful = static_cast<int>(std::ranges::count_if(
        sources, [](const auto& source) { return source.success; }));
    if (successful == 0) {
        error = L"DCB 公开数据源均未读取成功";
        return false;
    }
    std::map<std::string, DcbArchiveFile> products;
    for (const auto& source : sources) {
        for (const auto& file : source.files)
            products.try_emplace(file.logicalKey, file);
    }
    DbPtr database;
    if (!OpenDatabase(GetDcbAvailabilityDatabasePath(), database, error))
        return false;
    int existing = -1;
    int existingExpected = -1;
    {
        auto statement = Prepare(database.get(),
            "SELECT source_success_count,source_expected_count FROM yearly_scans WHERE archive_year=?1;",
            error);
        if (!statement) return false;
        sqlite3_bind_int(statement.get(), 1, year);
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
            "DELETE FROM yearly_scans WHERE archive_year=?1;", error);
        if (!statement) { rollback(); return false; }
        sqlite3_bind_int(statement.get(), 1, year);
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法替换 DCB 年记录");
            rollback();
            return false;
        }
    }
    {
        auto statement = Prepare(database.get(),
            "INSERT INTO yearly_scans VALUES(?1,?2,?3,?4,?5,?6,strftime('%Y-%m-%dT%H:%M:%SZ','now'));",
            error);
        if (!statement) { rollback(); return false; }
        sqlite3_bind_int(statement.get(), 1, year);
        Bind(statement.get(), 2, successful == kSourceCount ? "complete" : "partial");
        sqlite3_bind_int(statement.get(), 3, successful);
        sqlite3_bind_int(statement.get(), 4, kSourceCount);
        sqlite3_bind_int64(statement.get(), 5,
            static_cast<sqlite3_int64>(products.size()));
        sqlite3_bind_int(statement.get(), 6, ProvisionalYear(year) ? 1 : 0);
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法写入 DCB 年记录");
            rollback();
            return false;
        }
    }
    auto sourceStatement = Prepare(database.get(),
        "INSERT INTO yearly_source_scans VALUES(?1,?2,?3,?4,?5,?6);", error);
    if (!sourceStatement) { rollback(); return false; }
    for (const auto& source : sources) {
        sqlite3_reset(sourceStatement.get());
        sqlite3_clear_bindings(sourceStatement.get());
        sqlite3_bind_int(sourceStatement.get(), 1, year);
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
            error = DbError(database.get(), L"无法写入 DCB 来源状态");
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
        sqlite3_bind_int(productStatement.get(), 2, year);
        Bind(productStatement.get(), 3, DateText(file.coverageStart));
        Bind(productStatement.get(), 4, DateText(file.coverageEnd));
        Bind(productStatement.get(), 5, file.series);
        Bind(productStatement.get(), 6, file.center);
        Bind(productStatement.get(), 7, file.variant);
        Bind(productStatement.get(), 8, file.latency);
        Bind(productStatement.get(), 9, file.duration);
        Bind(productStatement.get(), 10, file.interval);
        Bind(productStatement.get(), 11, file.constellation);
        Bind(productStatement.get(), 12, file.signalPair);
        Bind(productStatement.get(), 13, file.scope);
        Bind(productStatement.get(), 14, file.format);
        if (sqlite3_step(productStatement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法写入 DCB 产品");
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
                    error = DbError(database.get(), L"无法写入 DCB 下载来源");
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

DcbAvailabilityLookup LoadDcbAvailability(const QueryDate& date) {
    std::scoped_lock lock(gDatabaseMutex);
    DcbAvailabilityLookup result;
    result.archiveYear = date.year;
    result.sourcesExpected = kSourceCount;
    result.databasePath = GetDcbAvailabilityDatabasePath();
    DbPtr database;
    if (!OpenReadDatabase(result.databasePath, database, result.error)) return result;
    {
        auto statement = Prepare(database.get(),
            "SELECT status,source_success_count,source_expected_count,is_provisional FROM yearly_scans WHERE archive_year=?1;",
            result.error);
        if (!statement) return result;
        sqlite3_bind_int(statement.get(), 1, date.year);
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
    const std::string dateText = DateText(date);
    auto products = Prepare(database.get(),
        "SELECT logical_key,coverage_start,coverage_end,series,center,variant,latency,duration,sample_interval,constellation,signal_pair,scope,format FROM products WHERE coverage_start<=?1 AND coverage_end>=?1 ORDER BY center,variant,latency,sample_interval;",
        result.error);
    if (!products || !Bind(products.get(), 1, dateText)) return result;
    auto routes = Prepare(database.get(),
        "SELECT d.source_key,d.source_priority,s.file_name,s.download_url FROM product_sources s JOIN source_definitions d ON d.source_id=s.source_id WHERE s.logical_key=?1 ORDER BY d.source_priority,s.file_name;",
        result.error);
    if (!routes) return result;
    while (sqlite3_step(products.get()) == SQLITE_ROW) {
        DcbProductRecord record;
        record.logicalKey = ColumnText(products.get(), 0);
        const auto parseDate = [](const std::string& value) {
            int year = 0, month = 0, day = 0;
            if (sscanf_s(value.c_str(), "%d-%d-%d", &year, &month, &day) != 3)
                return QueryDate{};
            return MakeQueryDate(year, month, day);
        };
        record.coverageStart = parseDate(ColumnText(products.get(), 1));
        record.coverageEnd = parseDate(ColumnText(products.get(), 2));
        record.series = ColumnText(products.get(), 3);
        record.center = ColumnText(products.get(), 4);
        record.variant = ColumnText(products.get(), 5);
        record.latency = ColumnText(products.get(), 6);
        record.duration = ColumnText(products.get(), 7);
        record.interval = ColumnText(products.get(), 8);
        record.constellation = ColumnText(products.get(), 9);
        record.signalPair = ColumnText(products.get(), 10);
        record.scope = ColumnText(products.get(), 11);
        record.format = ColumnText(products.get(), 12);
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
                Utf8ToWide(ColumnText(routes.get(), 3))});
        }
        result.products.push_back(std::move(record));
    }
    return result;
}

bool IsDcbYearComplete(
    const int year, bool& complete, std::wstring& error) {
    std::scoped_lock lock(gDatabaseMutex);
    complete = false;
    DbPtr database;
    if (!OpenDatabase(GetDcbAvailabilityDatabasePath(), database, error))
        return false;
    auto statement = Prepare(database.get(),
        "SELECT status,is_provisional,source_expected_count FROM yearly_scans WHERE archive_year=?1;",
        error);
    if (!statement) return false;
    sqlite3_bind_int(statement.get(), 1, year);
    if (sqlite3_step(statement.get()) == SQLITE_ROW) {
        complete = ColumnText(statement.get(), 0) == "complete"
            && sqlite3_column_int(statement.get(), 1) == 0
            && sqlite3_column_int(statement.get(), 2) == kSourceCount;
    }
    return true;
}

DcbAvailabilityStats GetDcbAvailabilityStats() {
    std::scoped_lock lock(gDatabaseMutex);
    DcbAvailabilityStats result;
    result.databasePath = GetDcbAvailabilityDatabasePath();
    DbPtr database;
    if (!OpenDatabase(result.databasePath, database, result.error)) return result;
    auto statement = Prepare(database.get(),
        "SELECT SUM(status='complete'),SUM(status='partial'),MIN(archive_year),MAX(archive_year) FROM yearly_scans;",
        result.error);
    if (statement && sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.completeYears = sqlite3_column_int(statement.get(), 0);
        result.partialYears = sqlite3_column_int(statement.get(), 1);
        result.firstYear = sqlite3_column_int(statement.get(), 2);
        result.lastYear = sqlite3_column_int(statement.get(), 3);
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

int DcbExpectedSourceCount() {
    return kSourceCount;
}
