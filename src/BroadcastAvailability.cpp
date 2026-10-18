#include "BroadcastAvailability.h"

#include "DateUtil.h"
#include "HttpClient.h"
#include "IgsAvailabilityDb.h"

#include "sqlite3.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
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

enum class DirectoryMode {
    AnnualFtp,
    DailyFtp,
    DailyHttps,
    DailyHttp,
    SplitDailyFtp,
    WhuHybrid
};

struct SourceDefinition {
    const char* key;
    int priority;
    const wchar_t* root;
    DirectoryMode mode;
};

constexpr std::array kSources{
    SourceDefinition{"CDDIS", 1,
        L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/data/daily/",
        DirectoryMode::AnnualFtp},
    SourceDefinition{"BKG", 2,
        L"https://igs.bkg.bund.de/root_ftp/IGS/BRDC/",
        DirectoryMode::DailyHttps},
    SourceDefinition{"WHU", 3,
        L"ftp://igs.gnsswhu.cn/pub/gps/data/daily/",
        DirectoryMode::WhuHybrid},
    SourceDefinition{"KASI", 4,
        L"ftp://nfs.kasi.re.kr/gps/data/daily/",
        DirectoryMode::SplitDailyFtp},
    SourceDefinition{"IGN", 5,
        L"ftp://igs.ign.fr/pub/igs/data/",
        DirectoryMode::DailyFtp},
    SourceDefinition{"ESA", 6,
        L"ftp://gssc.esa.int/gnss/data/daily/",
        DirectoryMode::AnnualFtp},
    SourceDefinition{"SOPAC", 7,
        L"http://garner.ucsd.edu/pub/rinex/",
        DirectoryMode::DailyHttp},
    SourceDefinition{"CAS", 8,
        L"https://data.bdsmart.cn/pub/product/rts/brdc/",
        DirectoryMode::DailyHttps},
};

constexpr int kSourceCount = static_cast<int>(kSources.size());

struct DbCloser { void operator()(sqlite3* value) const { if (value) sqlite3_close(value); } };
struct StmtCloser { void operator()(sqlite3_stmt* value) const { if (value) sqlite3_finalize(value); } };
using DbPtr = std::unique_ptr<sqlite3, DbCloser>;
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtCloser>;

std::mutex gDatabaseMutex;

struct AnnualCacheEntry {
    std::mutex mutex;
    std::condition_variable condition;
    bool loading = false;
    bool ready = false;
    std::chrono::steady_clock::time_point lastFailure{};
    long transportErrorCode = 0;
    bool networkFailure = false;
    std::wstring error;
    std::vector<BroadcastArchiveFile> files;
};

std::mutex gAnnualCacheMutex;
std::map<std::pair<int, int>, std::shared_ptr<AnnualCacheEntry>> gAnnualCache;

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
    return left.year == right.year && left.month == right.month
        && left.day == right.day;
}

QueryDate DateFromDoy(const int year, const int doy) {
    return FromSysDays(std::chrono::sys_days{
        std::chrono::year{year} / std::chrono::January / 1}
        + std::chrono::days{doy - 1});
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

std::wstring JoinUrl(const std::wstring& directory, const std::string& file) {
    std::wstring result = directory;
    if (!result.ends_with(L'/')) result.push_back(L'/');
    result += Utf8ToWide(file);
    return result;
}

std::string ProducerFor(const std::string& series) {
    if (series == "BRDC00IGS") return "IGS";
    if (series == "BRDC00WRD") return "BKG";
    if (series == "BRDC00IGN") return "IGN";
    if (series == "BRDM00DLR" || series == "BRD400DLR"
        || series == "BRDX00DLR") return "DLR";
    if (series == "BRDM00CAS") return "CAS";
    return series.size() >= 3 ? series.substr(series.size() - 3) : series;
}

std::string ConstellationFor(
    const std::string& series, const std::string& navigationType) {
    if (navigationType == "GN") return "G";
    if (navigationType == "RN") return "R";
    if (navigationType == "EN") return "E";
    if (navigationType == "CN") return "C";
    if (navigationType == "JN") return "J";
    if (navigationType == "IN") return "I";
    if (navigationType == "SN") return "S";
    if (navigationType == "CNAV") return "GJ";
    if (series == "BRDM00DLR" || series == "BRD400DLR") return "GRECJIS";
    if (series == "BRDC00IGN") return "GRE";
    if (series == "BRDM00CAS") return "GRECJ";
    return "GRECJS";
}

std::string LogicalKey(const BroadcastArchiveFile& file) {
    return file.series + "|" + DateText(file.nominalDate) + "|"
        + std::format("{:02}{:02}|{}|{}|{}|{}", file.startHour,
            file.startMinute, file.duration, file.navigationType,
            file.rinexVersion, file.sourceFlag);
}

bool ParseLongName(
    const std::string& name, const int expectedYear,
    BroadcastArchiveFile& file) {
    static const std::regex pattern(
        R"(^([A-Z0-9]{9})_([A-Z])_([0-9]{4})([0-9]{3})([0-9]{2})([0-9]{2})_([0-9]{2}[DHMSU])_([A-Z0-9]{2})\.RNX(?:\.(?:GZ|Z|BZ2))?$)",
        std::regex::icase);
    static const std::set<std::string> supported{
        "BRDC00IGS", "BRDC00WRD", "BRDC00IGN", "BRDM00DLR",
        "BRD400DLR", "BRDM00CAS"};
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) return false;
    const std::string series = Upper(match[1].str());
    if (!supported.contains(series)) return false;
    const std::string sourceFlag = Upper(match[2].str());
    const std::string navigationType = Upper(match[8].str());
    const bool validCombination =
        (series == "BRDC00IGS" && sourceFlag == "R"
            && navigationType == "MN")
        || (series == "BRDC00WRD"
            && ((sourceFlag == "S" && navigationType == "MN")
                || (sourceFlag == "R"
                    && (navigationType == "MN" || navigationType == "GN"
                        || navigationType == "RN" || navigationType == "EN"
                        || navigationType == "CN" || navigationType == "JN"
                        || navigationType == "IN" || navigationType == "SN"))))
        || (series == "BRDC00IGN" && sourceFlag == "R"
            && (navigationType == "MN" || navigationType == "GN"
                || navigationType == "RN" || navigationType == "EN"))
        || ((series == "BRDM00DLR" || series == "BRD400DLR")
            && sourceFlag == "S" && navigationType == "MN")
        || (series == "BRDM00CAS" && sourceFlag == "S"
            && navigationType == "MN");
    if (!validCombination) return false;
    const int year = std::stoi(match[3].str());
    const int doy = std::stoi(match[4].str());
    if (year != expectedYear || doy < 1
        || doy > (IsLeapYear(year) ? 366 : 365)) return false;
    file.series = series;
    file.sourceFlag = sourceFlag;
    file.nominalDate = DateFromDoy(year, doy);
    file.startHour = std::stoi(match[5].str());
    file.startMinute = std::stoi(match[6].str());
    if (file.startHour > 23 || file.startMinute > 59) return false;
    file.duration = Upper(match[7].str());
    file.navigationType = navigationType;
    file.rinexVersion = series == "BRD400DLR" ? "RINEX 4" : "RINEX 3";
    file.producer = ProducerFor(series);
    file.constellation = ConstellationFor(series, file.navigationType);
    file.logicalKey = LogicalKey(file);
    return true;
}

bool ParseShortName(
    const std::string& name, const int expectedYear,
    BroadcastArchiveFile& file) {
    static const std::regex pattern(
        R"(^((?:BRDC|BRDM|BRDX|AUTO))([0-9]{3})0\.([0-9]{2})([A-Z])(?:\.(?:GZ|Z|BZ2))?$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) return false;
    const int doy = std::stoi(match[2].str());
    const int shortYear = std::stoi(match[3].str());
    if (shortYear != expectedYear % 100 || doy < 1
        || doy > (IsLeapYear(expectedYear) ? 366 : 365)) return false;
    const std::string prefix = Upper(match[1].str());
    const std::string suffix = Upper(match[4].str());
    if (prefix == "BRDC" || prefix == "AUTO") {
        if ((prefix == "AUTO" && suffix != "N")
            || (prefix == "BRDC" && suffix != "N" && suffix != "G"))
            return false;
        file.series = "BRDC00IGS";
        file.rinexVersion = "RINEX 2";
        file.navigationType = suffix == "G" ? "RN" : "GN";
    } else if (prefix == "BRDM") {
        if (suffix != "P") return false;
        file.series = "BRDM00DLR";
        file.rinexVersion = "RINEX 3";
        file.navigationType = "MN";
    } else {
        if (suffix != "X") return false;
        file.series = "BRDX00DLR";
        file.rinexVersion = "CNAV";
        file.navigationType = "CNAV";
    }
    file.sourceFlag = (prefix == "BRDM" || prefix == "BRDX") ? "S" : "R";
    file.nominalDate = DateFromDoy(expectedYear, doy);
    file.duration = "01D";
    file.producer = ProducerFor(file.series);
    file.constellation = ConstellationFor(file.series, file.navigationType);
    file.logicalKey = LogicalKey(file);
    return true;
}

std::wstring AnnualUrl(const SourceDefinition& source, const int year) {
    return std::wstring(source.root) + std::to_wstring(year) + L"/brdc/";
}

std::wstring DailyUrl(const SourceDefinition& source, const QueryDate& date) {
    return std::wstring(source.root) + std::to_wstring(date.year) + L"/"
        + std::format(L"{:03}/", date.dayOfYear);
}

std::vector<std::wstring> SplitDailyUrls(
    const SourceDefinition& source, const QueryDate& date) {
    const std::wstring base = DailyUrl(source, date);
    const std::wstring shortYear = std::format(L"{:02}", date.year % 100);
    return {base + shortYear + L"p/", base + shortYear + L"n/",
        base + shortYear + L"g/"};
}

std::shared_ptr<AnnualCacheEntry> CacheEntry(
    const int sourcePriority, const int year) {
    std::scoped_lock lock(gAnnualCacheMutex);
    const auto key = std::pair{sourcePriority, year};
    auto& entry = gAnnualCache[key];
    if (!entry) entry = std::make_shared<AnnualCacheEntry>();
    return entry;
}

BroadcastSourceScan QueryAnnualSource(
    const SourceDefinition& source, const QueryDate& date,
    const std::stop_token stopToken) {
    BroadcastSourceScan scan;
    scan.sourceKey = source.key;
    scan.sourcePriority = source.priority;
    scan.listingUrl = AnnualUrl(source, date.year);
    const auto entry = CacheEntry(source.priority, date.year);
    bool fetch = false;
    {
        std::unique_lock lock(entry->mutex);
        if (entry->loading) {
            entry->condition.wait(lock, [&]() { return !entry->loading; });
        }
        if (entry->ready) {
            scan.success = true;
        } else if (!entry->error.empty()
            && std::chrono::steady_clock::now() - entry->lastFailure
                < std::chrono::seconds{10}) {
            scan.error = entry->error;
            scan.transportErrorCode = entry->transportErrorCode;
            scan.networkFailure = entry->networkFailure;
            return scan;
        } else {
            entry->loading = true;
            fetch = true;
        }
    }
    if (fetch) {
        const HttpTextResult response = HttpListFtpDirectory(
            scan.listingUrl, stopToken, 64U * 1024U * 1024U);
        std::vector<BroadcastArchiveFile> parsed;
        bool success = response.ok() || MissingDirectory(response);
        if (response.ok()) {
            parsed = ParseBroadcastDirectoryListing(
                response.body, date.year, scan.listingUrl);
        }
        {
            std::scoped_lock lock(entry->mutex);
            entry->loading = false;
            entry->ready = success;
            if (success) {
                entry->files = std::move(parsed);
                entry->error.clear();
                entry->transportErrorCode = 0;
                entry->networkFailure = false;
            } else {
                entry->lastFailure = std::chrono::steady_clock::now();
                entry->error = response.error.empty()
                    ? std::format(L"服务器返回 HTTP {}", response.statusCode)
                    : response.error;
                entry->transportErrorCode = response.transportErrorCode;
                entry->networkFailure = IsNetworkFailure(response);
            }
        }
        entry->condition.notify_all();
    }
    {
        std::scoped_lock lock(entry->mutex);
        scan.success = entry->ready;
        scan.error = entry->error;
        scan.transportErrorCode = entry->transportErrorCode;
        scan.networkFailure = entry->networkFailure;
        if (scan.success) {
            for (const auto& file : entry->files) {
                if (SameDate(file.nominalDate, date)) scan.files.push_back(file);
            }
        }
    }
    return scan;
}

BroadcastSourceScan QueryDailySource(
    const SourceDefinition& source, const QueryDate& date,
    const std::stop_token stopToken) {
    BroadcastSourceScan scan;
    scan.sourceKey = source.key;
    scan.sourcePriority = source.priority;
    std::vector<std::wstring> urls{DailyUrl(source, date)};
    if (std::string_view(source.key) == "BKG") {
        const auto append = [&](const wchar_t* root) {
            urls.push_back(std::wstring(root) + std::to_wstring(date.year)
                + std::format(L"/{:03}/", date.dayOfYear));
        };
        if (date.year == 2012 || date.year == 2013)
            append(L"https://igs.bkg.bund.de/root_ftp/IGS/BRDC_v3/");
        if (date.year >= 2014 && date.year <= 2020)
            append(L"https://igs.bkg.bund.de/root_ftp/MGEX/BRDC_v3/");
        if (date.year >= 2016)
            append(L"https://igs.bkg.bund.de/root_ftp/MGEX/BRDC/");
    }
    bool allDirectoriesQueried = true;
    for (const auto& url : urls) {
        if (!scan.listingUrl.empty()) scan.listingUrl += L";";
        scan.listingUrl += url;
        const HttpTextResult response = source.mode == DirectoryMode::DailyFtp
            ? HttpListFtpDirectory(url, stopToken, 64U * 1024U * 1024U)
            : HttpGetText(url, stopToken, 64U * 1024U * 1024U);
        if (response.ok()) {
            auto parsed = ParseBroadcastDirectoryListing(response.body, date.year, url);
            for (auto& file : parsed)
                if (SameDate(file.nominalDate, date)) scan.files.push_back(std::move(file));
        } else if (!MissingDirectory(response)) {
            allDirectoriesQueried = false;
            scan.transportErrorCode = response.transportErrorCode;
            scan.networkFailure = scan.networkFailure || IsNetworkFailure(response);
            if (!scan.error.empty()) scan.error += L"；";
            scan.error += response.error.empty()
                ? std::format(L"服务器返回 HTTP {}", response.statusCode) : response.error;
        }
    }
    scan.success = allDirectoriesQueried;
    return scan;
}

BroadcastSourceScan QuerySplitDailySource(
    const SourceDefinition& source, const QueryDate& date,
    const std::stop_token stopToken) {
    BroadcastSourceScan scan;
    scan.sourceKey = source.key;
    scan.sourcePriority = source.priority;
    bool allDirectoriesQueried = true;
    for (const auto& url : SplitDailyUrls(source, date)) {
        if (!scan.listingUrl.empty()) scan.listingUrl += L";";
        scan.listingUrl += url;
        const HttpTextResult response = HttpListFtpDirectory(
            url, stopToken, 64U * 1024U * 1024U);
        if (response.ok()) {
            auto parsed = ParseBroadcastDirectoryListing(
                response.body, date.year, url);
            for (auto& file : parsed) {
                if (SameDate(file.nominalDate, date))
                    scan.files.push_back(std::move(file));
            }
            continue;
        }
        if (MissingDirectory(response)) continue;
        allDirectoriesQueried = false;
        scan.transportErrorCode = response.transportErrorCode;
        scan.networkFailure = scan.networkFailure || IsNetworkFailure(response);
        if (!scan.error.empty()) scan.error += L"；";
        scan.error += response.error.empty()
            ? std::format(L"服务器返回 HTTP {}", response.statusCode)
            : response.error;
    }
    scan.success = allDirectoriesQueried;
    return scan;
}

} // namespace

std::vector<BroadcastArchiveFile> ParseBroadcastDirectoryListing(
    const std::string& listing, const int expectedYear,
    const std::wstring& directoryUrl) {
    static const std::regex candidate(
        R"((?:^|[^A-Za-z0-9_.-])([A-Za-z0-9]{9}_[A-Za-z]_[0-9]{11}_[0-9]{2}[dDhHmMsSuU]_[A-Za-z0-9]{2}\.rnx(?:\.(?:gz|Z|bz2))?|(?:brdc|brdm|brdx|auto)[0-9]{3}0\.[0-9]{2}[A-Za-z](?:\.(?:gz|Z|bz2))?)(?=$|[^A-Za-z0-9_.-]))",
        std::regex::icase);
    std::set<std::string> seen;
    std::vector<BroadcastArchiveFile> result;
    for (std::sregex_iterator it(listing.begin(), listing.end(), candidate), end;
         it != end; ++it) {
        std::string name = (*it)[1].str();
        if (!seen.insert(Lower(name)).second) continue;
        BroadcastArchiveFile file;
        if (!ParseLongName(name, expectedYear, file)
            && !ParseShortName(name, expectedYear, file)) continue;
        file.fileName = std::move(name);
        file.downloadUrl = JoinUrl(directoryUrl, file.fileName);
        result.push_back(std::move(file));
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        if (left.nominalDate.dayOfYear != right.nominalDate.dayOfYear)
            return left.nominalDate.dayOfYear < right.nominalDate.dayOfYear;
        if (left.logicalKey != right.logicalKey) return left.logicalKey < right.logicalKey;
        return left.fileName < right.fileName;
    });
    return result;
}

std::vector<BroadcastSourceScan> QueryBroadcastDate(
    const QueryDate& date, const std::stop_token stopToken) {
    std::vector<std::future<BroadcastSourceScan>> futures;
    futures.reserve(kSources.size());
    for (const auto& source : kSources) {
        futures.push_back(std::async(std::launch::async, [&, source]() {
            if (source.mode == DirectoryMode::AnnualFtp
                || (source.mode == DirectoryMode::WhuHybrid
                    && date.year >= 2020)) {
                return QueryAnnualSource(source, date, stopToken);
            }
            if (source.mode == DirectoryMode::SplitDailyFtp
                || source.mode == DirectoryMode::WhuHybrid) {
                return QuerySplitDailySource(source, date, stopToken);
            }
            if (std::string_view(source.key) == "CAS" && date.year < 2025) {
                BroadcastSourceScan scan;
                scan.sourceKey = source.key;
                scan.sourcePriority = source.priority;
                scan.success = true;
                scan.listingUrl = L"not-applicable-before-2025";
                return scan;
            }
            return QueryDailySource(source, date, stopToken);
        }));
    }
    std::vector<BroadcastSourceScan> scans;
    scans.reserve(kSources.size());
    for (auto& future : futures) scans.push_back(future.get());
    std::ranges::sort(scans, [](const auto& left, const auto& right) {
        return left.sourcePriority < right.sourcePriority;
    });
    return scans;
}

namespace {

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
        error = DbError(database, L"无法准备广播星历知识库查询");
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
        error = L"无法创建广播星历知识库目录：" + path.parent_path().wstring();
        return false;
    }
    sqlite3* raw = nullptr;
    const int status = sqlite3_open16(path.c_str(), &raw);
    database.reset(raw);
    if (status != SQLITE_OK || raw == nullptr) {
        error = DbError(raw, L"无法打开广播星历知识库");
        return false;
    }
    sqlite3_busy_timeout(raw, 10000);
    const char* schema =
        "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS source_definitions(source_id INTEGER PRIMARY KEY,source_key TEXT NOT NULL UNIQUE,source_priority INTEGER NOT NULL UNIQUE);"
        "CREATE TABLE IF NOT EXISTS daily_scans(date TEXT PRIMARY KEY,status TEXT NOT NULL CHECK(status IN('complete','partial')),source_success_count INTEGER NOT NULL,source_expected_count INTEGER NOT NULL,product_count INTEGER NOT NULL,is_provisional INTEGER NOT NULL,scanned_at_utc TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS daily_source_scans(date TEXT NOT NULL,source_id INTEGER NOT NULL,status TEXT NOT NULL CHECK(status IN('complete','failed')),listing_url TEXT NOT NULL,file_count INTEGER NOT NULL,error_message TEXT NOT NULL,PRIMARY KEY(date,source_id),FOREIGN KEY(date) REFERENCES daily_scans(date) ON DELETE CASCADE,FOREIGN KEY(source_id) REFERENCES source_definitions(source_id));"
        "CREATE TABLE IF NOT EXISTS products(logical_key TEXT PRIMARY KEY,date TEXT NOT NULL,start_hour INTEGER NOT NULL,start_minute INTEGER NOT NULL,series TEXT NOT NULL,producer TEXT NOT NULL,rinex_version TEXT NOT NULL,navigation_type TEXT NOT NULL,constellation TEXT NOT NULL,duration TEXT NOT NULL,source_flag TEXT NOT NULL,FOREIGN KEY(date) REFERENCES daily_scans(date) ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS products_date ON products(date,producer,rinex_version,navigation_type);"
        "CREATE TABLE IF NOT EXISTS product_sources(logical_key TEXT NOT NULL,source_id INTEGER NOT NULL,file_name TEXT NOT NULL,download_url TEXT NOT NULL,PRIMARY KEY(logical_key,source_id,file_name,download_url),FOREIGN KEY(logical_key) REFERENCES products(logical_key) ON DELETE CASCADE,FOREIGN KEY(source_id) REFERENCES source_definitions(source_id));"
        "INSERT OR IGNORE INTO source_definitions VALUES"
        "(1,'CDDIS',1),(2,'BKG',2),(3,'WHU',3),(4,'KASI',4),"
        "(5,'IGN',5),(6,'ESA',6),(7,'SOPAC',7),(8,'CAS',8);"
        "INSERT INTO metadata VALUES('schema_version','2') ON CONFLICT(key) DO UPDATE SET value='2';"
        "INSERT INTO metadata VALUES('product_type','broadcast_navigation') ON CONFLICT(key) DO UPDATE SET value='broadcast_navigation';";
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
        error = DbError(raw, L"无法以只读方式打开广播星历知识库");
        return false;
    }
    sqlite3_busy_timeout(raw, 10000);
    return true;
}

bool ProvisionalDate(const QueryDate& date) {
    const auto today = std::chrono::floor<std::chrono::days>(
        std::chrono::system_clock::now());
    return ToSysDays(date) >= today - std::chrono::days{3};
}

} // namespace

std::filesystem::path GetBroadcastAvailabilityDatabasePath() {
    const auto explicitPath = EnvironmentPath(
        L"GNSS_DOWNLOADER_BROADCAST_DB_PATH");
    if (!explicitPath.empty()) return explicitPath;
    const auto directory = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
    if (!directory.empty()) {
        return ResolveDatabaseFilePath(directory / L"broadcast_availability.sqlite");
    }
    return ResolveDatabaseFilePath(GetIgsAvailabilityDatabasePath().parent_path()
        / L"broadcast_availability.sqlite");
}

bool StoreBroadcastDate(
    const QueryDate& date, const std::vector<BroadcastSourceScan>& sources,
    std::wstring& error) {
    std::scoped_lock lock(gDatabaseMutex);
    const int successful = static_cast<int>(std::ranges::count_if(
        sources, [](const auto& source) { return source.success; }));
    if (successful == 0) {
        error = L"广播星历公开数据源均未读取成功";
        return false;
    }
    std::map<std::string, BroadcastArchiveFile> products;
    for (const auto& source : sources) {
        for (const auto& file : source.files)
            products.try_emplace(file.logicalKey, file);
    }
    DbPtr database;
    if (!OpenDatabase(GetBroadcastAvailabilityDatabasePath(), database, error))
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
            if (error.empty()) error = DbError(database.get(), L"无法替换广播星历日期记录");
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
            error = DbError(database.get(), L"无法写入广播星历日期记录");
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
            error = DbError(database.get(), L"无法写入广播星历来源状态");
            rollback();
            return false;
        }
    }
    auto productStatement = Prepare(database.get(),
        "INSERT INTO products VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11);",
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
        Bind(productStatement.get(), 6, file.producer);
        Bind(productStatement.get(), 7, file.rinexVersion);
        Bind(productStatement.get(), 8, file.navigationType);
        Bind(productStatement.get(), 9, file.constellation);
        Bind(productStatement.get(), 10, file.duration);
        Bind(productStatement.get(), 11, file.sourceFlag);
        if (sqlite3_step(productStatement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法写入广播星历产品");
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
                    error = DbError(database.get(), L"无法写入广播星历下载来源");
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

BroadcastAvailabilityLookup LoadBroadcastAvailability(const QueryDate& date) {
    std::scoped_lock lock(gDatabaseMutex);
    BroadcastAvailabilityLookup result;
    result.sourcesExpected = kSourceCount;
    result.databasePath = GetBroadcastAvailabilityDatabasePath();
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
        "SELECT logical_key,start_hour,start_minute,series,producer,rinex_version,navigation_type,constellation,duration,source_flag FROM products WHERE date=?1 ORDER BY producer,rinex_version,navigation_type,series,start_hour,start_minute;",
        result.error);
    if (!products || !Bind(products.get(), 1, dateText)) return result;
    auto routes = Prepare(database.get(),
        "SELECT d.source_key,d.source_priority,s.file_name,s.download_url FROM product_sources s JOIN source_definitions d ON d.source_id=s.source_id WHERE s.logical_key=?1 ORDER BY d.source_priority,s.file_name;",
        result.error);
    if (!routes) return result;
    while (sqlite3_step(products.get()) == SQLITE_ROW) {
        BroadcastProductRecord record;
        record.logicalKey = ColumnText(products.get(), 0);
        record.nominalDate = date;
        record.startHour = sqlite3_column_int(products.get(), 1);
        record.startMinute = sqlite3_column_int(products.get(), 2);
        record.series = ColumnText(products.get(), 3);
        record.producer = ColumnText(products.get(), 4);
        record.rinexVersion = ColumnText(products.get(), 5);
        record.navigationType = ColumnText(products.get(), 6);
        record.constellation = ColumnText(products.get(), 7);
        record.duration = ColumnText(products.get(), 8);
        record.sourceFlag = ColumnText(products.get(), 9);
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

bool IsBroadcastDateComplete(
    const QueryDate& date, bool& complete, std::wstring& error) {
    std::scoped_lock lock(gDatabaseMutex);
    complete = false;
    DbPtr database;
    if (!OpenDatabase(GetBroadcastAvailabilityDatabasePath(), database, error))
        return false;
    auto statement = Prepare(database.get(),
        "SELECT status,is_provisional,source_expected_count FROM daily_scans WHERE date=?1;", error);
    if (!statement || !Bind(statement.get(), 1, DateText(date))) return false;
    if (sqlite3_step(statement.get()) == SQLITE_ROW) {
        complete = ColumnText(statement.get(), 0) == "complete"
            && sqlite3_column_int(statement.get(), 1) == 0
            && sqlite3_column_int(statement.get(), 2) == kSourceCount;
    }
    return true;
}

BroadcastAvailabilityStats GetBroadcastAvailabilityStats() {
    std::scoped_lock lock(gDatabaseMutex);
    BroadcastAvailabilityStats result;
    result.databasePath = GetBroadcastAvailabilityDatabasePath();
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

int BroadcastExpectedSourceCount() {
    return kSourceCount;
}
