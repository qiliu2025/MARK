#include "TroAvailability.h"

#include "DateUtil.h"
#include "ArchiveLegacyNames.h"
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
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <string_view>

namespace {

struct SourceDefinition {
    const char* key;
    int priority;
    const char* archiveClass;
};

constexpr std::array kSources{
    SourceDefinition{"CDDIS", 1, "IGS_ZPD"},
    SourceDefinition{"IGN", 2, "IGS_ZPD"},
    SourceDefinition{"ESA", 3, "IGS_ZPD"},
    SourceDefinition{"WHU", 4, "IGS_ZPD"},
    SourceDefinition{"CODE", 5, "CODE"},
    SourceDefinition{"EPN-BKG", 6, "EPN_OPS"},
    SourceDefinition{"EPN-R1", 7, "EPN_REPRO1"},
    SourceDefinition{"EPN-R2", 8, "EPN_REPRO2"},
};
constexpr int kSourceCount = static_cast<int>(kSources.size());

struct ListingDefinition {
    std::wstring listingUrl;
    std::wstring downloadDirectory;
    std::string archiveClass;
    bool ftp = false;
    int gpsWeek = -1;
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

bool SameDate(const QueryDate& left, const QueryDate& right) {
    return left.year == right.year && left.month == right.month
        && left.day == right.day;
}

QueryDate EndForDuration(const QueryDate& start, const std::string& duration) {
    if (duration.size() != 3) return start;
    int count = 0;
    try { count = std::stoi(duration.substr(0, 2)); }
    catch (...) { return start; }
    if (count < 1) return start;
    const char unit = static_cast<char>(std::toupper(
        static_cast<unsigned char>(duration[2])));
    if (unit == 'D')
        return FromSysDays(ToSysDays(start) + std::chrono::days{count - 1});
    return start;
}

bool CoversDate(const TroArchiveFile& file, const QueryDate& date) {
    const auto day = ToSysDays(date);
    return ToSysDays(file.coverageStart) <= day
        && ToSysDays(file.coverageEnd) >= day;
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

std::string LatencyFor(const std::string& solution) {
    const std::string value = Upper(solution);
    if (value == "FIN") return "FINAL";
    if (value == "RAP") return "RAPID";
    if (value == "ULT") return "ULTRA_RAPID";
    if (value == "NRT") return "NEAR_REAL_TIME";
    if (value == "RTS") return "REAL_TIME";
    if (value == "E01" || value == "E02" || value == "E03")
        return "REPROCESSING";
    return "OTHER";
}

std::string ProductIdentity(const TroArchiveFile& file) {
    std::string station = Upper(file.station);
    if (station.size() > 4) station.resize(4);
    return file.productFamily + "|" + file.center + "|" + station + "|"
        + DateText(file.coverageStart) + "|" + DateText(file.coverageEnd)
        + std::format("|{:02}{:02}|{}|{}|{}", file.startHour,
            file.startMinute, file.duration, file.interval, file.solution);
}

void FinishDefinition(TroArchiveFile& file, const QueryDate& indexedDate) {
    file.coverageEnd = EndForDuration(file.coverageStart, file.duration);
    file.latency = LatencyFor(file.solution);
    file.logicalKey = DateText(indexedDate) + "|" + ProductIdentity(file);
}

bool ParseLongName(
    const std::string& name, const QueryDate& expected, TroArchiveFile& file) {
    static const std::regex pattern(
        R"(^([A-Z0-9]{10})_([0-9]{4})([0-9]{3})([0-9]{2})([0-9]{2})_([0-9]{2}[DHMSUWLY])_([0-9]{2}[DHMSUWLY])(?:_([A-Z0-9]{4,9}))?_TRO\.(TRO|TRP)(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) return false;
    const int year = std::stoi(match[2].str());
    const int doy = std::stoi(match[3].str());
    if (doy < 1 || doy > (IsLeapYear(year) ? 366 : 365)) return false;
    file.series = Upper(match[1].str());
    file.center = file.series.substr(0, 3);
    file.project = file.series.substr(4, 3);
    file.solution = file.series.substr(7, 3);
    file.coverageStart = DateFromDoy(year, doy);
    file.nominalDate = file.coverageStart;
    file.startHour = std::stoi(match[4].str());
    file.startMinute = std::stoi(match[5].str());
    file.duration = Upper(match[6].str());
    file.interval = Upper(match[7].str());
    file.station = match[8].matched ? Upper(match[8].str()) : "";
    const std::string extension = Upper(match[9].str());
    file.format = extension == "TRP" ? "BERNESE_TRP" : "TRO_SINEX";
    if (file.project == "EPN") {
        file.productFamily = file.center == "EUR"
            ? "EPN_COMBINED" : "EPN_AC";
    } else if (file.center == "IGS" && !file.station.empty()) {
        file.productFamily = "IGS_SITE";
    } else if (file.center == "COD") {
        file.productFamily = extension == "TRP"
            ? "CODE_TRP" : "CODE_TRO";
    } else {
        file.productFamily = file.center + "_TRO";
    }
    FinishDefinition(file, expected);
    return CoversDate(file, expected);
}

bool ParseIgsLegacyName(
    const std::string& name, const QueryDate& expected, TroArchiveFile& file) {
    static const std::regex pattern(
        R"(^([A-Z0-9]{4})([0-9]{3})0\.([0-9]{2})ZPD(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) return false;
    if (std::stoi(match[2].str()) != expected.dayOfYear
        || std::stoi(match[3].str()) != expected.year % 100) return false;
    file.series = "IGS0OPSFIN";
    file.center = "IGS";
    file.project = "OPS";
    file.solution = "FIN";
    file.nominalDate = expected;
    file.coverageStart = expected;
    file.duration = "01D";
    file.interval = "05M";
    file.productFamily = "IGS_SITE";
    file.station = Upper(match[1].str());
    file.format = "IGS_ZPD_LEGACY";
    FinishDefinition(file, expected);
    return true;
}

bool ParseCodeLegacyName(
    const std::string& name, const QueryDate& expected, TroArchiveFile& file) {
    static const std::regex pattern(
        R"(^COD([0-9]{2})([0-9]{3})\.(TRO|TRP)(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) return false;
    if (std::stoi(match[1].str()) != expected.year % 100
        || std::stoi(match[2].str()) != expected.dayOfYear) return false;
    const std::string extension = Upper(match[3].str());
    file.series = "COD0OPSFIN";
    file.center = "COD";
    file.project = "OPS";
    file.solution = "FIN";
    file.nominalDate = expected;
    file.coverageStart = expected;
    file.duration = "01D";
    file.interval = extension == "TRP" ? "02H" : "01H";
    file.productFamily = extension == "TRP" ? "CODE_TRP" : "CODE_TRO";
    file.format = extension == "TRP" ? "BERNESE_TRP" : "TRO_SINEX";
    FinishDefinition(file, expected);
    return true;
}

std::string LegacyEpnCenter(const std::string& series) {
    static const std::map<std::string, std::string> centers{
        {"AS0", "ASI"}, {"BE0", "BEK"}, {"BV0", "BEV"},
        {"BG0", "BKG"}, {"CO0", "COD"}, {"DE0", "DEO"},
        {"EU0", "EUR"}, {"GF0", "GFZ"}, {"GO0", "GOP"},
        {"IG0", "IGE"}, {"IN0", "IGN"}, {"LP0", "LPT"},
        {"MU0", "MUT"}, {"NK0", "NKG"}, {"OL0", "OLG"},
        {"RG0", "RGA"}, {"RO0", "ROB"}, {"SG0", "SGO"},
        {"SU0", "SUT"}, {"UP0", "UPA"}, {"WU0", "WUT"},
    };
    const auto found = centers.find(Upper(series));
    return found == centers.end() ? Upper(series) : found->second;
}

bool ParseCodeWeekName(const std::string& name, const QueryDate& expected,
                      const int expectedWeek, const std::string& archiveClass, TroArchiveFile& file) {
    if (expectedWeek < 0 || archiveClass.starts_with("EPN")) return false;
    static const std::regex pattern(R"(^([A-Z0-9]{3})([0-9]{4})([0-7])\.TRO(?:\.(?:GZ|Z))?$)",std::regex::icase);
    std::smatch match;
    if (!std::regex_match(name,match,pattern) || std::stoi(match[2].str())!=expectedWeek) return false;
    const int offset=std::stoi(match[3].str());
    const QueryDate day=FromSysDays(ToSysDays(GpsWeekStartDate(expectedWeek))+std::chrono::days{offset==7?0:offset});
    const std::string prefix=Upper(match[1].str());
    const std::string project=ArchiveReprocessingProject(archiveClass);
    file.center=prefix=="COF"?"COD":ArchiveReprocessingCenter(prefix);
    file.project=project.empty()?"OPS":project;
    file.solution=project=="R01"?"E01":project=="R02"?"E02":"FIN";
    file.series=prefix+"0"+file.project+"FIN";
    file.nominalDate=day; file.coverageStart=day;
    file.duration=offset==7?"07D":"01D";
    file.interval=(prefix=="COD"||prefix=="CO1"||prefix=="CO2"||prefix=="CF1"||prefix=="CF2"||prefix=="COF")?"01H":"00U";
    file.productFamily=prefix=="COD"&&project.empty()?"CODE_TRO":"IGS_"+prefix+"_TRO";
    file.format="TRO_SINEX";
    FinishDefinition(file,expected);
    return CoversDate(file,expected);
}

bool ParseEpnLegacyName(
    const std::string& name, const QueryDate& expected,
    const int expectedGpsWeek, const std::string& archiveClass,
    TroArchiveFile& file) {
    static const std::regex pattern(
        R"(^([A-Z0-9]{3})([0-9]{4})([0-7])\.TRO(?:\.(?:GZ|Z))?$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) return false;
    const int gpsWeek = std::stoi(match[2].str());
    if (expectedGpsWeek >= 0 && gpsWeek != expectedGpsWeek) return false;
    const int day = std::stoi(match[3].str());
    file.coverageStart = GpsWeekStartDate(gpsWeek);
    file.nominalDate = file.coverageStart;
    file.duration = day == 7 ? "07D" : "01D";
    if (day < 7) {
        file.coverageStart = FromSysDays(
            ToSysDays(file.coverageStart) + std::chrono::days{day});
        file.nominalDate = file.coverageStart;
    }
    file.interval = "01H";
    file.center = LegacyEpnCenter(match[1].str());
    file.project = "EPN";
    if (archiveClass == "EPN_REPRO1") file.solution = "E01";
    else if (archiveClass == "EPN_REPRO2") file.solution = "E02";
    else file.solution = "FIN";
    file.series = file.center.substr(0, std::min<std::size_t>(3, file.center.size()));
    while (file.series.size() < 3) file.series.push_back('X');
    file.series += "0EPN" + file.solution;
    file.productFamily = file.center == "EUR"
        ? "EPN_COMBINED" : "EPN_AC";
    file.format = "TRO_SINEX";
    FinishDefinition(file, expected);
    return CoversDate(file, expected);
}

class ListingCache {
public:
    HttpTextResult Get(const ListingDefinition& definition,
                       const std::stop_token stopToken) {
        std::shared_future<HttpTextResult> future;
        const std::wstring key = (definition.ftp ? L"F|" : L"H|")
            + definition.listingUrl;
        {
            std::scoped_lock lock(mutex_);
            auto found = entries_.find(key);
            if (found == entries_.end()) {
                auto task = std::async(std::launch::async,
                    [definition, stopToken]() {
                        return definition.ftp
                            ? HttpListFtpDirectory(definition.listingUrl,
                                stopToken, 64U * 1024U * 1024U)
                            : HttpGetText(definition.listingUrl,
                                stopToken, 64U * 1024U * 1024U);
                    }).share();
                found = entries_.emplace(key,
                    CacheEntry{task, ++sequence_}).first;
            } else {
                found->second.sequence = ++sequence_;
            }
            future = found->second.future;
            TrimLocked(key);
        }
        HttpTextResult result = future.get();
        if (!result.ok() && !MissingDirectory(result)) {
            std::scoped_lock lock(mutex_);
            entries_.erase(key);
        }
        return result;
    }

private:
    struct CacheEntry {
        std::shared_future<HttpTextResult> future;
        std::uint64_t sequence = 0;
    };

    void TrimLocked(const std::wstring& currentKey) {
        while (entries_.size() > 96) {
            auto oldest = entries_.end();
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (it->first == currentKey
                    || it->second.future.wait_for(std::chrono::seconds{0})
                        != std::future_status::ready) continue;
                if (oldest == entries_.end()
                    || it->second.sequence < oldest->second.sequence) oldest = it;
            }
            if (oldest == entries_.end()) break;
            entries_.erase(oldest);
        }
    }

    std::mutex mutex_;
    std::map<std::wstring, CacheEntry> entries_;
    std::uint64_t sequence_ = 0;
};

ListingCache gListingCache;

std::vector<ListingDefinition> ListingsFor(
    const SourceDefinition& source, const QueryDate& date) {
    const std::wstring year = std::to_wstring(date.year);
    const std::wstring doy = std::format(L"{:03}", date.dayOfYear);
    const std::string_view key(source.key);
    if (key == "CDDIS") {
        const std::wstring directory =
            L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/products/troposphere/zpd/"
            + year + L"/" + doy + L"/";
        const std::wstring root = L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/products/";
        const std::wstring week = std::format(L"{:04}/", CalculateGpsWeek(date));
        return {{directory, directory, source.archiveClass, true, -1},
            {root + week, root + week, "OPS", true, CalculateGpsWeek(date)},
            {root + L"repro1/" + week, root + L"repro1/" + week, "REPRO1", true, CalculateGpsWeek(date)},
            {root + L"repro2/" + week, root + L"repro2/" + week, "REPRO2", true, CalculateGpsWeek(date)}};
    }
    if (key == "IGN") {
        const std::wstring directory =
            L"ftp://igs.ign.fr/pub/igs/products/troposphere/"
            + year + L"/" + doy + L"/";
        return {{directory, directory, source.archiveClass, true, -1}};
    }
    if (key == "ESA") {
        const std::wstring directory =
            L"ftp://gssc.esa.int/gnss/products/troposphere_zpd/"
            + year + L"/" + doy + L"/";
        return {{directory, directory, source.archiveClass, true, -1}};
    }
    if (key == "WHU") {
        const std::wstring directory =
            L"ftp://igs.gnsswhu.cn/pub/gps/products/troposphere/new/"
            + year + L"/" + doy + L"/";
        return {{directory, directory, source.archiveClass, true, -1}};
    }
    if (key == "CODE") {
        return {
            {L"https://code.aiub.unibe.ch/s3_script/aiub_s3_bucket_listing.php?path=CODE%2F"
                + year,
             L"https://www.aiub.unibe.ch/download/CODE/" + year + L"/",
             "CODE_MAIN", false, -1},
            {L"https://code.aiub.unibe.ch/s3_script/aiub_s3_bucket_listing.php?path=CODE%2FTROP%2F"
                + year,
             L"https://www.aiub.unibe.ch/download/CODE/TROP/" + year + L"/",
             "CODE_TROP", false, -1},
        };
    }
    const int week = CalculateGpsWeek(date);
    std::wstring root;
    int firstWeek = 0;
    int lastWeek = 9999;
    if (key == "EPN-R1") {
        root = L"https://igs.bkg.bund.de/root_ftp/EPNrepro1/products/";
        firstWeek = 834;
        lastWeek = 1407;
    } else if (key == "EPN-R2") {
        root = L"https://igs.bkg.bund.de/root_ftp/EPNrepro2/products/";
        firstWeek = 834;
        lastWeek = 1824;
    } else {
        root = L"https://igs.bkg.bund.de/root_ftp/EUREF/products/";
        firstWeek = 829;
    }
    if (week < firstWeek || week > lastWeek) return {};
    const std::wstring directory = root + std::format(L"{:04}/", week);
    return {{directory, directory, source.archiveClass, false, week}};
}

TroSourceScan QuerySource(
    const SourceDefinition& source, const QueryDate& date,
    const std::stop_token stopToken) {
    TroSourceScan scan;
    scan.sourceKey = source.key;
    scan.sourcePriority = source.priority;
    bool fatal = false;
    for (const auto& definition : ListingsFor(source, date)) {
        scan.listingUrls.push_back(definition.listingUrl);
        const HttpTextResult response = gListingCache.Get(definition, stopToken);
        if (response.ok()) {
            auto files = ParseTroDirectoryListing(response.body, date,
                definition.downloadDirectory, definition.archiveClass,
                definition.gpsWeek);
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
        error = DbError(database, L"无法准备 TRO 知识库查询");
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
        error = L"无法创建 TRO 知识库目录：" + path.parent_path().wstring();
        return false;
    }
    sqlite3* raw = nullptr;
    const int status = sqlite3_open16(path.c_str(), &raw);
    database.reset(raw);
    if (status != SQLITE_OK || raw == nullptr) {
        error = DbError(raw, L"无法打开 TRO 知识库");
        return false;
    }
    sqlite3_busy_timeout(raw, 10000);
    const char* schema =
        "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS source_definitions(source_id INTEGER PRIMARY KEY,source_key TEXT NOT NULL UNIQUE,source_priority INTEGER NOT NULL UNIQUE);"
        "CREATE TABLE IF NOT EXISTS daily_scans(date TEXT PRIMARY KEY,status TEXT NOT NULL CHECK(status IN('complete','partial')),source_success_count INTEGER NOT NULL,source_expected_count INTEGER NOT NULL,product_count INTEGER NOT NULL,is_provisional INTEGER NOT NULL,scanned_at_utc TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS daily_source_scans(date TEXT NOT NULL,source_id INTEGER NOT NULL,status TEXT NOT NULL CHECK(status IN('complete','failed')),listing_urls TEXT NOT NULL,file_count INTEGER NOT NULL,error_message TEXT NOT NULL,PRIMARY KEY(date,source_id),FOREIGN KEY(date) REFERENCES daily_scans(date) ON DELETE CASCADE,FOREIGN KEY(source_id) REFERENCES source_definitions(source_id));"
        "CREATE TABLE IF NOT EXISTS products(logical_key TEXT PRIMARY KEY,date TEXT NOT NULL,nominal_date TEXT NOT NULL,coverage_start TEXT NOT NULL,coverage_end TEXT NOT NULL,start_hour INTEGER NOT NULL,start_minute INTEGER NOT NULL,series TEXT NOT NULL,center TEXT NOT NULL,project TEXT NOT NULL,solution TEXT NOT NULL,latency TEXT NOT NULL,duration TEXT NOT NULL,sample_interval TEXT NOT NULL,product_family TEXT NOT NULL,station TEXT NOT NULL,format TEXT NOT NULL,FOREIGN KEY(date) REFERENCES daily_scans(date) ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS products_date ON products(date,product_family,latency,center,station);"
        "CREATE TABLE IF NOT EXISTS product_sources(logical_key TEXT NOT NULL,source_id INTEGER NOT NULL,file_name TEXT NOT NULL,download_url TEXT NOT NULL,archive_class TEXT NOT NULL,PRIMARY KEY(logical_key,source_id,file_name,download_url),FOREIGN KEY(logical_key) REFERENCES products(logical_key) ON DELETE CASCADE,FOREIGN KEY(source_id) REFERENCES source_definitions(source_id));"
        "INSERT OR IGNORE INTO source_definitions VALUES"
        "(1,'CDDIS',1),(2,'IGN',2),(3,'ESA',3),(4,'WHU',4),"
        "(5,'CODE',5),(6,'EPN-BKG',6),(7,'EPN-R1',7),(8,'EPN-R2',8);"
        "INSERT INTO metadata VALUES('schema_version','1') ON CONFLICT(key) DO UPDATE SET value='1';"
        "INSERT INTO metadata VALUES('product_type','troposphere') ON CONFLICT(key) DO UPDATE SET value='troposphere';"
        "INSERT INTO metadata VALUES('excludes_vmf','1') ON CONFLICT(key) DO UPDATE SET value='1';";
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
        error = DbError(raw, L"无法以只读方式打开 TRO 知识库");
        return false;
    }
    sqlite3_busy_timeout(raw, 10000);
    return true;
}

bool ProvisionalDate(const QueryDate& date) {
    const auto today = std::chrono::floor<std::chrono::days>(
        std::chrono::system_clock::now());
    return ToSysDays(date) >= today - std::chrono::days{35};
}

QueryDate ParseDateText(const std::string& value) {
    int year = 0, month = 0, day = 0;
    if (sscanf_s(value.c_str(), "%d-%d-%d", &year, &month, &day) != 3)
        return {};
    return MakeQueryDate(year, month, day);
}

} // namespace

std::vector<TroArchiveFile> ParseTroDirectoryListing(
    const std::string& listing, const QueryDate& expectedDate,
    const std::wstring& downloadDirectory, const std::string& archiveClass,
    const int expectedGpsWeek) {
    static const std::regex candidate(
        R"(([A-Za-z0-9]{10}_[0-9]{11}_[0-9]{2}[dDhHmMsSuUwWlLyY]_[0-9]{2}[dDhHmMsSuUwWlLyY](?:_[A-Za-z0-9]{4,9})?_TRO\.(?:TRO|TRP)(?:\.(?:gz|Z))?|[A-Za-z0-9]{4}[0-9]{4}\.[0-9]{2}zpd(?:\.(?:gz|Z))?|COD[0-9]{5}\.(?:TRO|TRP)(?:\.(?:gz|Z))?|[A-Za-z0-9]{3}[0-9]{5}\.tro(?:\.(?:gz|Z))?)(?=$|[^A-Za-z0-9_.-]))",
        std::regex::icase);
    std::set<std::string> seen;
    std::vector<TroArchiveFile> result;
    for (std::sregex_iterator it(listing.begin(), listing.end(), candidate), end;
         it != end; ++it) {
        std::string name = (*it)[1].str();
        if (!seen.insert(Lower(name)).second) continue;
        TroArchiveFile file;
        const bool parsed = ParseLongName(name, expectedDate, file)
            || ParseIgsLegacyName(name, expectedDate, file)
            || ParseCodeWeekName(name, expectedDate, expectedGpsWeek, archiveClass, file)
            || (expectedGpsWeek < 0 && ParseCodeLegacyName(name, expectedDate, file))
            || (archiveClass.starts_with("EPN") && ParseEpnLegacyName(name, expectedDate, expectedGpsWeek,
                archiveClass, file));
        if (!parsed) continue;
        file.fileName = std::move(name);
        file.downloadUrl = JoinUrl(downloadDirectory, file.fileName);
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

std::vector<TroSourceScan> QueryTroDate(
    const QueryDate& date, const std::stop_token stopToken) {
    std::vector<std::future<TroSourceScan>> futures;
    futures.reserve(kSources.size());
    for (const auto& source : kSources) {
        futures.push_back(std::async(std::launch::async, [=, &stopToken]() {
            return QuerySource(source, date, stopToken);
        }));
    }
    std::vector<TroSourceScan> scans;
    scans.reserve(kSources.size());
    for (auto& future : futures) scans.push_back(future.get());
    std::ranges::sort(scans, [](const auto& left, const auto& right) {
        return left.sourcePriority < right.sourcePriority;
    });
    return scans;
}

std::filesystem::path GetTroAvailabilityDatabasePath() {
    const auto explicitPath = EnvironmentPath(L"GNSS_DOWNLOADER_TRO_DB_PATH");
    if (!explicitPath.empty()) return explicitPath;
    const auto directory = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
    if (!directory.empty()) {
        return ResolveDatabaseFilePath(directory / L"tro_availability.sqlite");
    }
    return ResolveDatabaseFilePath(GetIgsAvailabilityDatabasePath().parent_path()
        / L"tro_availability.sqlite");
}

bool StoreTroDate(
    const QueryDate& date, const std::vector<TroSourceScan>& sources,
    std::wstring& error) {
    std::scoped_lock lock(gDatabaseMutex);
    const int successful = static_cast<int>(std::ranges::count_if(
        sources, [](const auto& source) { return source.success; }));
    if (successful == 0) {
        error = L"TRO 公开数据源均未读取成功";
        return false;
    }
    std::map<std::string, TroArchiveFile> products;
    for (const auto& source : sources) {
        for (const auto& file : source.files)
            products.try_emplace(file.logicalKey, file);
    }
    DbPtr database;
    if (!OpenDatabase(GetTroAvailabilityDatabasePath(), database, error))
        return false;
    const std::string dateText = DateText(date);
    int existing = -1;
    int existingExpected = -1;
    {
        auto statement = Prepare(database.get(),
            "SELECT source_success_count,source_expected_count FROM daily_scans WHERE date=?1;",
            error);
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
            if (error.empty())
                error = DbError(database.get(), L"无法替换 TRO 日期记录");
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
        Bind(statement.get(), 2,
            successful == kSourceCount ? "complete" : "partial");
        sqlite3_bind_int(statement.get(), 3, successful);
        sqlite3_bind_int(statement.get(), 4, kSourceCount);
        sqlite3_bind_int64(statement.get(), 5,
            static_cast<sqlite3_int64>(products.size()));
        sqlite3_bind_int(statement.get(), 6, ProvisionalDate(date) ? 1 : 0);
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法写入 TRO 日期记录");
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
            error = DbError(database.get(), L"无法写入 TRO 来源状态");
            rollback();
            return false;
        }
    }
    auto productStatement = Prepare(database.get(),
        "INSERT INTO products VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17);",
        error);
    auto routeStatement = Prepare(database.get(),
        "INSERT OR IGNORE INTO product_sources VALUES(?1,?2,?3,?4,?5);",
        error);
    if (!productStatement || !routeStatement) { rollback(); return false; }
    for (const auto& [logicalKey, file] : products) {
        sqlite3_reset(productStatement.get());
        sqlite3_clear_bindings(productStatement.get());
        Bind(productStatement.get(), 1, logicalKey);
        Bind(productStatement.get(), 2, dateText);
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
        Bind(productStatement.get(), 16, file.station);
        Bind(productStatement.get(), 17, file.format);
        if (sqlite3_step(productStatement.get()) != SQLITE_DONE) {
            error = DbError(database.get(), L"无法写入 TRO 产品");
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
                    error = DbError(database.get(), L"无法写入 TRO 下载来源");
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

TroAvailabilityLookup LoadTroAvailability(const QueryDate& date) {
    std::scoped_lock lock(gDatabaseMutex);
    TroAvailabilityLookup result;
    result.sourcesExpected = kSourceCount;
    result.databasePath = GetTroAvailabilityDatabasePath();
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
        "SELECT logical_key,nominal_date,coverage_start,coverage_end,start_hour,start_minute,series,center,project,solution,latency,duration,sample_interval,product_family,station,format FROM products WHERE date=?1 ORDER BY product_family,latency,center,station,series;",
        result.error);
    if (!products || !Bind(products.get(), 1, dateText)) return result;
    auto routes = Prepare(database.get(),
        "SELECT d.source_key,d.source_priority,s.file_name,s.download_url,s.archive_class FROM product_sources s JOIN source_definitions d ON d.source_id=s.source_id WHERE s.logical_key=?1 ORDER BY d.source_priority,s.file_name;",
        result.error);
    if (!routes) return result;
    while (sqlite3_step(products.get()) == SQLITE_ROW) {
        TroProductRecord record;
        record.logicalKey = ColumnText(products.get(), 0);
        record.nominalDate = ParseDateText(ColumnText(products.get(), 1));
        record.coverageStart = ParseDateText(ColumnText(products.get(), 2));
        record.coverageEnd = ParseDateText(ColumnText(products.get(), 3));
        record.startHour = sqlite3_column_int(products.get(), 4);
        record.startMinute = sqlite3_column_int(products.get(), 5);
        record.series = ColumnText(products.get(), 6);
        record.center = ColumnText(products.get(), 7);
        record.project = ColumnText(products.get(), 8);
        record.solution = ColumnText(products.get(), 9);
        record.latency = ColumnText(products.get(), 10);
        record.duration = ColumnText(products.get(), 11);
        record.interval = ColumnText(products.get(), 12);
        record.productFamily = ColumnText(products.get(), 13);
        record.station = ColumnText(products.get(), 14);
        record.format = ColumnText(products.get(), 15);
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

bool IsTroDateComplete(
    const QueryDate& date, bool& complete, std::wstring& error) {
    std::scoped_lock lock(gDatabaseMutex);
    complete = false;
    DbPtr database;
    if (!OpenDatabase(GetTroAvailabilityDatabasePath(), database, error))
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

TroAvailabilityStats GetTroAvailabilityStats() {
    std::scoped_lock lock(gDatabaseMutex);
    TroAvailabilityStats result;
    result.databasePath = GetTroAvailabilityDatabasePath();
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

int TroExpectedSourceCount() {
    return kSourceCount;
}
