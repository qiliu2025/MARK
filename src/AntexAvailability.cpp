#include "AntexAvailability.h"

#include "DateUtil.h"
#include "HttpClient.h"
#include "IgsAvailabilityDb.h"

#include "sqlite3.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <format>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace {

constexpr int kSourceCount = 4;

struct DbCloser { void operator()(sqlite3* value) const { if (value) sqlite3_close(value); } };
struct StmtCloser { void operator()(sqlite3_stmt* value) const { if (value) sqlite3_finalize(value); } };
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

std::string DateText(const QueryDate& date) {
    return std::format("{:04}-{:02}-{:02}", date.year, date.month, date.day);
}

QueryDate ParseDate(const std::string& value) {
    int year = 0, month = 0, day = 0;
    if (sscanf_s(value.c_str(), "%d-%d-%d", &year, &month, &day) != 3)
        return {};
    return MakeQueryDate(year, month, day);
}

std::chrono::sys_days ToSysDays(const QueryDate& date) {
    return std::chrono::sys_days{std::chrono::year{date.year}
        / std::chrono::month{static_cast<unsigned>(date.month)}
        / std::chrono::day{static_cast<unsigned>(date.day)}};
}

int GpsWeek(const QueryDate& date) {
    const auto epoch = std::chrono::sys_days{
        std::chrono::year{1980} / std::chrono::January / 6};
    return static_cast<int>((ToSysDays(date) - epoch).count() / 7);
}

// A dated ANTEX filename (for example IGS20_2425.ATX) is a revision of a
// logical group, while the catalogue stores the frame window for every
// revision as the same broad IGS20 interval.  When a range query returns all
// revisions verbatim, a planner that simply picks the first row can silently
// use a future calibration file for dates before its publication (or keep an
// obsolete revision after a newer one exists).  Derive the effective revision
// windows here; the single-day loader already performs the equivalent choice
// in memory.
std::chrono::sys_days RevisionStart(const int gpsWeek) {
    const auto epoch = std::chrono::sys_days{
        std::chrono::year{1980} / std::chrono::January / 6};
    return epoch + std::chrono::days{static_cast<long long>(gpsWeek) * 7};
}

QueryDate FromSysDays(const std::chrono::sys_days value) {
    const std::chrono::year_month_day date{value};
    return MakeQueryDate(static_cast<int>(date.year()),
        static_cast<unsigned>(date.month()),
        static_cast<unsigned>(date.day()));
}

void ApplyEffectiveRevisionWindows(std::vector<AntexProductRecord>& records) {
    std::map<std::string, std::vector<std::size_t>> groups;
    for (std::size_t index = 0; index < records.size(); ++index) {
        groups[records[index].groupKey].push_back(index);
    }
    std::vector<AntexProductRecord> earlyAliases;
    for (const auto& [group, indices] : groups) {
        (void)group;
        std::vector<int> revisions;
        bool hasAlias = false;
        for (const auto index : indices) {
            const auto& record = records[index];
            if (record.currentAlias) {
                hasAlias = true;
            } else if (record.revisionWeek >= 0) {
                revisions.push_back(record.revisionWeek);
            }
        }
        std::ranges::sort(revisions);
        revisions.erase(std::unique(revisions.begin(), revisions.end()),
            revisions.end());
        const int newest = revisions.empty() ? -1 : revisions.back();
        for (const auto index : indices) {
            auto& record = records[index];
            auto effectiveStart = ToSysDays(record.validFrom);
            auto effectiveEnd = ToSysDays(record.validTo);
            if (record.currentAlias) {
                // The undated alias is also the single-day fallback before
                // the first numbered revision.  Preserve that earlier
                // coverage as a second interval of the same logical file.
                if (newest >= 0) {
                    const auto firstRevision = RevisionStart(revisions.front());
                    if (effectiveStart < firstRevision) {
                        auto early = record;
                        const auto earlyEnd = std::min(effectiveEnd,
                            firstRevision - std::chrono::days{1});
                        early.validTo = FromSysDays(earlyEnd);
                        if (effectiveStart <= earlyEnd) {
                            earlyAliases.push_back(std::move(early));
                        }
                    }
                    effectiveStart = std::max(
                        effectiveStart, RevisionStart(newest + 1));
                }
            } else if (record.revisionWeek >= 0) {
                effectiveStart = std::max(
                    effectiveStart, RevisionStart(record.revisionWeek));
                const auto next = std::upper_bound(
                    revisions.begin(), revisions.end(), record.revisionWeek);
                if (next != revisions.end()) {
                    effectiveEnd = std::min(
                        effectiveEnd,
                        RevisionStart(*next) - std::chrono::days{1});
                } else if (hasAlias) {
                    effectiveEnd = std::min(
                        effectiveEnd,
                        RevisionStart(record.revisionWeek + 1)
                            - std::chrono::days{1});
                }
            }
            // Keep malformed/empty windows harmless: Covers() will reject an
            // inverted interval instead of accidentally treating it as open.
            record.validFrom = FromSysDays(effectiveStart);
            record.validTo = FromSysDays(effectiveEnd);
        }
    }
    records.insert(records.end(), std::make_move_iterator(earlyAliases.begin()),
        std::make_move_iterator(earlyAliases.end()));
}

std::pair<QueryDate, QueryDate> FrameWindow(const std::string& frame) {
    if (frame == "IGS01") return {MakeQueryDate(1994, 1, 1), MakeQueryDate(2006, 11, 4)};
    if (frame == "IGS05") return {MakeQueryDate(2006, 11, 5), MakeQueryDate(2011, 4, 16)};
    if (frame == "IGS08") return {MakeQueryDate(2011, 4, 17), MakeQueryDate(2017, 1, 28)};
    if (frame == "IGS14") return {MakeQueryDate(2017, 1, 29), MakeQueryDate(2022, 11, 26)};
    if (frame == "IGSR3") return {MakeQueryDate(1994, 1, 1), MakeQueryDate(2022, 11, 26)};
    return {MakeQueryDate(2022, 11, 27), MakeQueryDate(2099, 12, 31)};
}

std::wstring JoinUrl(const std::wstring& directory, const std::string& file) {
    std::wstring result = directory;
    if (!result.ends_with(L'/') && result.find(L'?') == std::wstring::npos)
        result.push_back(L'/');
    result += Utf8ToWide(file);
    return result;
}

bool IsNetworkFailure(const HttpTextResult& result) {
    const long code = result.transportErrorCode;
    return code == 5 || code == 6 || code == 7 || code == 28 || code == 35
        || code == 52 || code == 55 || code == 56 || code == 60
        || result.statusCode == 408 || result.statusCode == 429
        || result.statusCode >= 500;
}

std::string StripExtension(std::string name, std::string& version) {
    const std::string upper = Upper(name);
    if (upper.ends_with(".ATX2.GZ")) { version = "2.0"; return name.substr(0, name.size() - 8); }
    if (upper.ends_with(".ATX.GZ")) { version = "1.4"; return name.substr(0, name.size() - 7); }
    if (upper.ends_with(".ATX2")) { version = "2.0"; return name.substr(0, name.size() - 5); }
    if (upper.ends_with(".ATX")) { version = "1.4"; return name.substr(0, name.size() - 4); }
    version = "1.4";
    return name;
}

void FinishFile(AntexArchiveFile& file) {
    const auto [from, to] = FrameWindow(file.referenceFrame);
    file.validFrom = from;
    file.validTo = to;
    file.groupKey = file.producer + "|" + file.productFamily + "|"
        + file.modelName + "|" + file.referenceFrame + "|" + file.scope
        + "|" + file.formatVersion + "|" + file.variant;
    file.logicalKey = file.groupKey + "|"
        + (file.currentAlias ? "CURRENT" : std::to_string(file.revisionWeek));
}

bool ClassifyFile(const std::string& rawName, const std::string& sourceKey,
                  AntexArchiveFile& file) {
    std::string formatVersion;
    const std::string stem = Upper(StripExtension(rawName, formatVersion));
    file.fileName = rawName;
    file.formatVersion = formatVersion;
    file.scope = "SATELLITE_RECEIVER";
    file.calibrationType = "ABSOLUTE";
    file.revisionWeek = -1;
    file.currentAlias = true;

    std::smatch match;
    static const std::regex igsPattern(
        R"(^IGS(_01|R3|05|08|14|20)(?:_([0-9]{4}))?(?:_(.+))?$)");
    if (std::regex_match(stem, match, igsPattern)) {
        std::string frameToken = match[1].str();
        file.producer = "IGS";
        file.referenceFrame = frameToken == "_01" ? "IGS01" : "IGS" + frameToken;
        file.modelName = file.referenceFrame;
        file.productFamily = "IGS_GLOBAL";
        if (match[2].matched) {
            file.revisionWeek = std::stoi(match[2].str());
            file.currentAlias = false;
        }
        file.variant = match[3].matched ? Upper(match[3].str()) : "STANDARD";
        if (file.variant == "SAT") file.scope = "SATELLITE";
        if (file.variant == "RCV") file.scope = "RECEIVER";
        if (file.referenceFrame == "IGS01") file.calibrationType = "RELATIVE";
        if (file.referenceFrame == "IGSR3" || file.variant == "R3")
            file.calibrationType = "REPROCESSING";
        FinishFile(file);
        return true;
    }

    static const std::regex epnPattern(
        R"(^EPN(C)?_(05|08|14|20)(?:_([0-9]{4}))?(?:_(.+))?$)");
    if (std::regex_match(stem, match, epnPattern)) {
        const bool supplement = match[1].matched;
        file.producer = "EPN";
        file.referenceFrame = "IGS" + match[2].str();
        file.modelName = supplement ? "EPNC" + match[2].str() : "EPN" + match[2].str();
        file.productFamily = supplement ? "EPN_SUPPLEMENT" : "EPN_COMBINED";
        file.scope = supplement ? "RECEIVER" : "SATELLITE_RECEIVER";
        file.calibrationType = supplement ? "INDIVIDUAL_SUPPLEMENT" : "REGIONAL_COMBINED";
        if (match[3].matched) {
            file.revisionWeek = std::stoi(match[3].str());
            file.currentAlias = false;
        }
        file.variant = match[4].matched ? Upper(match[4].str()) : "STANDARD";
        if (file.variant == "R3") file.calibrationType = "REPROCESSING";
        FinishFile(file);
        return true;
    }

    static const std::regex codePattern(R"(^([IM])(05|08|14|20)(?:_([0-9]{4}))?$)");
    if (std::regex_match(stem, match, codePattern)) {
        const bool mgex = match[1].str() == "M";
        file.producer = "CODE";
        file.referenceFrame = "IGS" + match[2].str();
        file.modelName = match[1].str() + match[2].str();
        file.productFamily = mgex ? "CODE_MGEX" : "CODE_IGS";
        file.variant = "STANDARD";
        if (match[3].matched) {
            file.revisionWeek = std::stoi(match[3].str());
            file.currentAlias = false;
        }
        FinishFile(file);
        return true;
    }
    if (stem == "ATX" && sourceKey == "CODE") {
        file.producer = "CODE";
        file.referenceFrame = "IGS20";
        file.modelName = "ATX";
        file.productFamily = "CODE_IGS";
        file.variant = "CODE_CURRENT";
        FinishFile(file);
        return true;
    }

    static const std::regex ngsPattern(R"(^NGS(14|20)$)");
    if (std::regex_match(stem, match, ngsPattern)) {
        file.producer = "NGS";
        file.referenceFrame = "IGS" + match[1].str();
        file.modelName = "NGS" + match[1].str();
        file.productFamily = "NGS_COMPOSITE";
        file.scope = "RECEIVER";
        file.variant = "STANDARD";
        FinishFile(file);
        return true;
    }
    return false;
}

std::vector<std::string> ExtractFileNames(const std::string& listing,
                                          const std::string& sourceKey) {
    static const std::regex pattern(
        R"(([A-Za-z0-9_]+\.(?:atx2|atx)(?:\.gz)?)(?=$|[^A-Za-z0-9_.-]))",
        std::regex::icase);
    std::set<std::string> unique;
    for (std::sregex_iterator it(listing.begin(), listing.end(), pattern), end;
         it != end; ++it) {
        unique.insert((*it)[1].str());
    }
    if (sourceKey == "CODE") {
        static const std::regex bareAtx(R"((?:>|/|=|\s)ATX(?:<|\s|$))",
            std::regex::icase);
        if (std::regex_search(listing, bareAtx)) unique.insert("ATX");
    }
    return {unique.begin(), unique.end()};
}

std::wstring SqliteError(sqlite3* db, const std::wstring_view action) {
    return std::wstring(action) + L"：" + Utf8ToWide(db ? sqlite3_errmsg(db) : "unknown");
}

bool OpenDatabase(const std::filesystem::path& path, DbPtr& database,
                  std::wstring& error) {
    std::error_code fsError;
    std::filesystem::create_directories(path.parent_path(), fsError);
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(reinterpret_cast<const char*>(path.u8string().c_str()), &raw,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr) != SQLITE_OK) {
        error = SqliteError(raw, L"无法打开 ANTEX 知识库");
        if (raw) sqlite3_close(raw);
        return false;
    }
    database.reset(raw);
    sqlite3_busy_timeout(database.get(), 10000);
    return true;
}

bool OpenReadDatabase(const std::filesystem::path& path, DbPtr& database,
                      std::wstring& error) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(reinterpret_cast<const char*>(path.u8string().c_str()), &raw,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        error = SqliteError(raw, L"无法以只读方式打开 ANTEX 知识库");
        if (raw) sqlite3_close(raw);
        return false;
    }
    database.reset(raw);
    sqlite3_busy_timeout(database.get(), 10000);
    return true;
}

bool Execute(sqlite3* db, const char* sql, std::wstring& error) {
    char* message = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &message) == SQLITE_OK)
        return true;
    error = L"SQLite：" + Utf8ToWide(message ? message : sqlite3_errmsg(db));
    sqlite3_free(message);
    return false;
}

bool Prepare(sqlite3* db, const char* sql, StmtPtr& statement,
             std::wstring& error) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) {
        error = SqliteError(db, L"无法准备 ANTEX SQL");
        return false;
    }
    statement.reset(raw);
    return true;
}

void Bind(sqlite3_stmt* statement, const int index, const std::string& value) {
    sqlite3_bind_text(statement, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

std::string ColumnText(sqlite3_stmt* statement, const int index) {
    const auto* value = sqlite3_column_text(statement, index);
    return value ? reinterpret_cast<const char*>(value) : "";
}

bool EnsureSchema(sqlite3* db, std::wstring& error) {
    return Execute(db, R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
CREATE TABLE IF NOT EXISTS catalog_scan(
 id INTEGER PRIMARY KEY CHECK(id=1), complete INTEGER NOT NULL,
 sources_successful INTEGER NOT NULL, sources_expected INTEGER NOT NULL,
 product_count INTEGER NOT NULL, route_count INTEGER NOT NULL,
 scanned_at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS catalog_source_scans(
 source_key TEXT PRIMARY KEY, source_priority INTEGER NOT NULL,
 success INTEGER NOT NULL, network_failure INTEGER NOT NULL,
 transport_error INTEGER NOT NULL, error TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS products(
 logical_key TEXT PRIMARY KEY, group_key TEXT NOT NULL, producer TEXT NOT NULL,
 model_name TEXT NOT NULL, reference_frame TEXT NOT NULL,
 product_family TEXT NOT NULL, scope TEXT NOT NULL,
 format_version TEXT NOT NULL, calibration_type TEXT NOT NULL,
 revision_week INTEGER NOT NULL, variant TEXT NOT NULL,
 current_alias INTEGER NOT NULL, valid_from TEXT NOT NULL,
 valid_to TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_antex_group ON products(group_key, revision_week);
CREATE INDEX IF NOT EXISTS idx_antex_validity ON products(valid_from, valid_to);
CREATE TABLE IF NOT EXISTS product_sources(
 logical_key TEXT NOT NULL, source_key TEXT NOT NULL,
 source_priority INTEGER NOT NULL, file_name TEXT NOT NULL,
 download_url TEXT NOT NULL, archive_class TEXT NOT NULL,
 PRIMARY KEY(logical_key, source_key, download_url));
)SQL", error);
}

AntexSourceScan QueryOneSource(const std::string& key, const int priority,
                               std::stop_token stopToken) {
    struct Listing { std::wstring url; std::wstring directory; std::string kind; };
    std::vector<Listing> listings;
    if (key == "IGS") {
        listings = {
            {L"https://files.igs.org/pub/station/general/", L"https://files.igs.org/pub/station/general/", "IGS_CURRENT"},
            {L"https://files.igs.org/pub/station/general/pcv_archive/", L"https://files.igs.org/pub/station/general/pcv_archive/", "IGS_ARCHIVE"}};
    } else if (key == "CODE") {
        listings = {
            {L"https://code.aiub.unibe.ch/s3_script/aiub_s3_bucket_listing.php?path=CODE", L"https://www.aiub.unibe.ch/download/CODE/", "CODE_MAIN"},
            {L"https://code.aiub.unibe.ch/s3_script/aiub_s3_bucket_listing.php?path=CODE_MGEX%2FBSWUSER52%2FGEN", L"https://www.aiub.unibe.ch/download/CODE_MGEX/BSWUSER52/GEN/", "CODE_MGEX"}};
    } else if (key == "EPN") {
        listings = {
            {L"https://epncb.oma.be/pub/station/general/", L"https://epncb.oma.be/pub/station/general/", "EPN_CURRENT"},
            {L"https://epncb.oma.be/pub/station/general/archive/", L"https://epncb.oma.be/pub/station/general/archive/", "EPN_ARCHIVE"}};
    } else {
        listings = {{L"https://nweb.ngs.noaa.gov/ANTCAL/index.xhtml",
            L"https://nweb.ngs.noaa.gov/ANTCAL/LoadFile?file=", "NGS_COMPOSITE"}};
    }

    AntexSourceScan scan;
    scan.sourceKey = key;
    scan.sourcePriority = priority;
    bool anyNetworkFailure = false;
    for (const auto& listing : listings) {
        scan.listingUrls.push_back(listing.url);
        const auto response = HttpGetText(listing.url, stopToken, 32U * 1024U * 1024U);
        if (!response.ok()) {
            anyNetworkFailure = anyNetworkFailure || IsNetworkFailure(response);
            scan.transportErrorCode = response.transportErrorCode;
            if (!scan.error.empty()) scan.error += L"；";
            scan.error += response.error.empty()
                ? std::format(L"HTTP {}", response.statusCode) : response.error;
            continue;
        }
        auto parsed = ParseAntexDirectoryListing(
            response.body, listing.directory, listing.kind, key);
        scan.files.insert(scan.files.end(),
            std::make_move_iterator(parsed.begin()),
            std::make_move_iterator(parsed.end()));
    }
    // The NGS landing page is server-side generated and, depending on the
    // accepted compression/locale, curl can receive a menu shell without the
    // two href attributes.  The two official composite names are stable and
    // are also stated by the page, so retain them after a successful page
    // request instead of incorrectly reporting an empty source.
    if (key == "NGS" && scan.error.empty() && scan.files.empty()) {
        auto composites = ParseAntexDirectoryListing(
            "ngs20.atx ngs14.atx",
            L"https://nweb.ngs.noaa.gov/ANTCAL/LoadFile?file=",
            "NGS_COMPOSITE", "NGS");
        scan.files.insert(scan.files.end(),
            std::make_move_iterator(composites.begin()),
            std::make_move_iterator(composites.end()));
    }
    std::map<std::pair<std::string, std::wstring>, AntexArchiveFile> unique;
    for (auto& file : scan.files)
        unique[{Upper(file.fileName), file.downloadUrl}] = std::move(file);
    scan.files.clear();
    for (auto& [unused, file] : unique) scan.files.push_back(std::move(file));
    scan.success = scan.error.empty() && !scan.files.empty();
    if (scan.error.empty() && scan.files.empty())
        scan.error = L"官方目录可访问，但没有识别到 ANTEX 产品。";
    scan.networkFailure = !scan.success && anyNetworkFailure;
    return scan;
}

} // namespace

std::vector<AntexArchiveFile> ParseAntexDirectoryListing(
    const std::string& listing, const std::wstring& downloadDirectory,
    const std::string& archiveClass, const std::string& sourceKey) {
    std::vector<AntexArchiveFile> result;
    for (const auto& name : ExtractFileNames(listing, sourceKey)) {
        AntexArchiveFile file;
        if (!ClassifyFile(name, sourceKey, file)) continue;
        file.archiveClass = archiveClass;
        file.downloadUrl = JoinUrl(downloadDirectory, name);
        result.push_back(std::move(file));
    }
    return result;
}

std::vector<AntexSourceScan> QueryAntexCatalog(
    const int jobs, std::stop_token stopToken) {
    const std::array<std::pair<const char*, int>, 4> sources{{
        {"IGS", 1}, {"CODE", 2}, {"EPN", 3}, {"NGS", 4}}};
    std::vector<AntexSourceScan> results(sources.size());
    const int workerCount = std::clamp(jobs, 1, kSourceCount);
    std::atomic_size_t next{0};
    std::vector<std::jthread> workers;
    workers.reserve(static_cast<std::size_t>(workerCount));
    for (int worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&](std::stop_token) {
            while (!stopToken.stop_requested()) {
                const auto index = next.fetch_add(1);
                if (index >= sources.size()) break;
                results[index] = QueryOneSource(
                    sources[index].first, sources[index].second, stopToken);
            }
        });
    }
    return results;
}

bool StoreAntexCatalog(const std::vector<AntexSourceScan>& sources,
                       std::wstring& error) {
    std::scoped_lock lock(gDatabaseMutex);
    DbPtr db;
    if (!OpenDatabase(GetAntexAvailabilityDatabasePath(), db, error)
        || !EnsureSchema(db.get(), error)) return false;

    const int successful = static_cast<int>(std::ranges::count_if(
        sources, [](const auto& source) { return source.success; }));
    const bool complete = successful == kSourceCount;
    if (!complete) {
        StmtPtr old;
        if (Prepare(db.get(), "SELECT complete FROM catalog_scan WHERE id=1", old, error)
            && sqlite3_step(old.get()) == SQLITE_ROW
            && sqlite3_column_int(old.get(), 0) != 0) {
            error = L"本次 ANTEX 扫描不完整，已保留原有完整知识库。";
            return false;
        }
        error.clear();
    }

    struct Merged { AntexArchiveFile file; std::vector<AntexProductRoute> routes; };
    std::map<std::string, Merged> merged;
    for (const auto& source : sources) {
        if (!source.success) continue;
        for (const auto& file : source.files) {
            auto& item = merged[file.logicalKey];
            item.file = file;
            const bool duplicate = std::ranges::any_of(item.routes, [&](const auto& route) {
                return route.sourceKey == source.sourceKey
                    && route.downloadUrl == file.downloadUrl;
            });
            if (!duplicate) item.routes.push_back({source.sourceKey,
                source.sourcePriority, file.fileName, file.downloadUrl,
                file.archiveClass});
        }
    }
    std::int64_t routeCount = 0;
    for (const auto& [key, item] : merged)
        routeCount += static_cast<std::int64_t>(item.routes.size());

    if (!Execute(db.get(), "BEGIN IMMEDIATE", error)) return false;
    const auto rollback = [&]() { std::wstring ignored; Execute(db.get(), "ROLLBACK", ignored); };
    if (!Execute(db.get(), "DELETE FROM catalog_source_scans;DELETE FROM product_sources;DELETE FROM products;", error)) {
        rollback(); return false;
    }
    StmtPtr sourceStatement;
    StmtPtr productStatement;
    StmtPtr routeStatement;
    StmtPtr scanStatement;
    if (!Prepare(db.get(), "INSERT INTO catalog_source_scans VALUES(?,?,?,?,?,?)", sourceStatement, error)
        || !Prepare(db.get(), "INSERT INTO products VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)", productStatement, error)
        || !Prepare(db.get(), "INSERT INTO product_sources VALUES(?,?,?,?,?,?)", routeStatement, error)
        || !Prepare(db.get(), "INSERT OR REPLACE INTO catalog_scan VALUES(1,?,?,?,?,?,datetime('now'))", scanStatement, error)) {
        rollback(); return false;
    }
    for (const auto& source : sources) {
        sqlite3_reset(sourceStatement.get());
        Bind(sourceStatement.get(), 1, source.sourceKey);
        sqlite3_bind_int(sourceStatement.get(), 2, source.sourcePriority);
        sqlite3_bind_int(sourceStatement.get(), 3, source.success ? 1 : 0);
        sqlite3_bind_int(sourceStatement.get(), 4, source.networkFailure ? 1 : 0);
        sqlite3_bind_int64(sourceStatement.get(), 5, source.transportErrorCode);
        Bind(sourceStatement.get(), 6, WideToUtf8(source.error));
        if (sqlite3_step(sourceStatement.get()) != SQLITE_DONE) {
            error = SqliteError(db.get(), L"写入 ANTEX 数据源状态失败"); rollback(); return false;
        }
    }
    for (const auto& [key, item] : merged) {
        const auto& file = item.file;
        sqlite3_reset(productStatement.get());
        int column = 1;
        Bind(productStatement.get(), column++, file.logicalKey);
        Bind(productStatement.get(), column++, file.groupKey);
        Bind(productStatement.get(), column++, file.producer);
        Bind(productStatement.get(), column++, file.modelName);
        Bind(productStatement.get(), column++, file.referenceFrame);
        Bind(productStatement.get(), column++, file.productFamily);
        Bind(productStatement.get(), column++, file.scope);
        Bind(productStatement.get(), column++, file.formatVersion);
        Bind(productStatement.get(), column++, file.calibrationType);
        sqlite3_bind_int(productStatement.get(), column++, file.revisionWeek);
        Bind(productStatement.get(), column++, file.variant);
        sqlite3_bind_int(productStatement.get(), column++, file.currentAlias ? 1 : 0);
        Bind(productStatement.get(), column++, DateText(file.validFrom));
        Bind(productStatement.get(), column++, DateText(file.validTo));
        if (sqlite3_step(productStatement.get()) != SQLITE_DONE) {
            error = SqliteError(db.get(), L"写入 ANTEX 产品失败"); rollback(); return false;
        }
        for (const auto& route : item.routes) {
            sqlite3_reset(routeStatement.get());
            Bind(routeStatement.get(), 1, file.logicalKey);
            Bind(routeStatement.get(), 2, route.sourceKey);
            sqlite3_bind_int(routeStatement.get(), 3, route.sourcePriority);
            Bind(routeStatement.get(), 4, route.fileName);
            Bind(routeStatement.get(), 5, WideToUtf8(route.downloadUrl));
            Bind(routeStatement.get(), 6, route.archiveClass);
            if (sqlite3_step(routeStatement.get()) != SQLITE_DONE) {
                error = SqliteError(db.get(), L"写入 ANTEX 下载路线失败"); rollback(); return false;
            }
        }
    }
    sqlite3_bind_int(scanStatement.get(), 1, complete ? 1 : 0);
    sqlite3_bind_int(scanStatement.get(), 2, successful);
    sqlite3_bind_int(scanStatement.get(), 3, kSourceCount);
    sqlite3_bind_int64(scanStatement.get(), 4, static_cast<sqlite3_int64>(merged.size()));
    sqlite3_bind_int64(scanStatement.get(), 5, routeCount);
    if (sqlite3_step(scanStatement.get()) != SQLITE_DONE) {
        error = SqliteError(db.get(), L"写入 ANTEX 扫描摘要失败"); rollback(); return false;
    }
    return Execute(db.get(), "COMMIT", error);
}

AntexAvailabilityLookup LoadAntexAvailability(const QueryDate& date) {
    std::scoped_lock lock(gDatabaseMutex);
    AntexAvailabilityLookup result;
    result.databasePath = GetAntexAvailabilityDatabasePath();
    result.gpsWeek = GpsWeek(date);
    if (!std::filesystem::exists(result.databasePath)) return result;
    DbPtr db;
    if (!OpenReadDatabase(result.databasePath, db, result.error)) return result;
    StmtPtr scan;
    if (!Prepare(db.get(), "SELECT complete,sources_successful,sources_expected FROM catalog_scan WHERE id=1", scan, result.error))
        return result;
    if (sqlite3_step(scan.get()) != SQLITE_ROW) return result;
    result.indexed = true;
    result.complete = sqlite3_column_int(scan.get(), 0) != 0;
    result.sourcesSuccessful = sqlite3_column_int(scan.get(), 1);
    result.sourcesExpected = sqlite3_column_int(scan.get(), 2);

    StmtPtr products;
    if (!Prepare(db.get(), R"SQL(
SELECT logical_key,group_key,producer,model_name,reference_frame,
 product_family,scope,format_version,calibration_type,revision_week,
 variant,current_alias,valid_from,valid_to
FROM products
ORDER BY group_key,revision_week,current_alias)SQL", products, result.error)) return result;
    std::map<std::string, std::vector<AntexProductRecord>> groups;
    while (sqlite3_step(products.get()) == SQLITE_ROW) {
        AntexProductRecord record;
        record.logicalKey = ColumnText(products.get(), 0);
        record.groupKey = ColumnText(products.get(), 1);
        record.producer = ColumnText(products.get(), 2);
        record.modelName = ColumnText(products.get(), 3);
        record.referenceFrame = ColumnText(products.get(), 4);
        record.productFamily = ColumnText(products.get(), 5);
        record.scope = ColumnText(products.get(), 6);
        record.formatVersion = ColumnText(products.get(), 7);
        record.calibrationType = ColumnText(products.get(), 8);
        record.revisionWeek = sqlite3_column_int(products.get(), 9);
        record.variant = ColumnText(products.get(), 10);
        record.currentAlias = sqlite3_column_int(products.get(), 11) != 0;
        record.validFrom = ParseDate(ColumnText(products.get(), 12));
        record.validTo = ParseDate(ColumnText(products.get(), 13));
        groups[record.groupKey].push_back(std::move(record));
    }

    StmtPtr routes;
    if (!Prepare(db.get(), "SELECT source_key,source_priority,file_name,download_url,archive_class FROM product_sources WHERE logical_key=? ORDER BY source_priority,file_name", routes, result.error))
        return result;
    for (auto& [group, candidates] : groups) {
        (void)group;
        ApplyEffectiveRevisionWindows(candidates);
        AntexProductRecord* selected = nullptr;
        for (auto& candidate : candidates) {
            if (ToSysDays(candidate.validFrom) <= ToSysDays(date)
                && ToSysDays(date) <= ToSysDays(candidate.validTo)
                && (!selected || candidate.currentAlias
                    || candidate.revisionWeek > selected->revisionWeek)) {
                selected = &candidate;
            }
        }
        if (!selected) continue;
        sqlite3_reset(routes.get());
        sqlite3_clear_bindings(routes.get());
        Bind(routes.get(), 1, selected->logicalKey);
        while (sqlite3_step(routes.get()) == SQLITE_ROW) {
            selected->routes.push_back({ColumnText(routes.get(), 0),
                sqlite3_column_int(routes.get(), 1), ColumnText(routes.get(), 2),
                Utf8ToWide(ColumnText(routes.get(), 3)), ColumnText(routes.get(), 4)});
        }
        result.products.push_back(std::move(*selected));
    }
    std::ranges::sort(result.products, {}, [](const auto& item) {
        return item.producer + "|" + item.productFamily + "|" + item.modelName + "|" + item.variant;
    });
    return result;
}

static AntexAvailabilityRangeLookup LoadAntexRangeImpl(
    const QueryDate& startDate, const QueryDate& endDate, const bool explicitCatalog) {
    std::scoped_lock lock(gDatabaseMutex);
    AntexAvailabilityRangeLookup result;
    result.databasePath = GetAntexAvailabilityDatabasePath();
    const auto a = std::chrono::sys_days{std::chrono::year{startDate.year}/std::chrono::month{static_cast<unsigned>(startDate.month)}/std::chrono::day{static_cast<unsigned>(startDate.day)}};
    const auto b = std::chrono::sys_days{std::chrono::year{endDate.year}/std::chrono::month{static_cast<unsigned>(endDate.month)}/std::chrono::day{static_cast<unsigned>(endDate.day)}};
    result.expectedDays = a <= b ? static_cast<int>((b - a).count()) + 1 : 0;
    if (result.expectedDays <= 0) { result.error = L"ANTEX 日期范围无效"; return result; }
    if (!std::filesystem::exists(result.databasePath)) return result;
    DbPtr db;
    if (!OpenReadDatabase(result.databasePath, db, result.error)) return result;
    StmtPtr scan;
    if (!Prepare(db.get(), "SELECT complete,sources_successful,sources_expected FROM catalog_scan WHERE id=1", scan, result.error)) return result;
    if (sqlite3_step(scan.get()) != SQLITE_ROW) return result;
    result.indexed = true;
    result.complete = sqlite3_column_int(scan.get(), 0) != 0;
    if (result.complete) result.indexedDays = result.expectedDays;
    else {
        for (int i = 0; i < result.expectedDays; ++i) {
            const auto ymd = std::chrono::year_month_day{a + std::chrono::days{i}};
            result.incompleteDates.push_back(MakeQueryDate(static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day())));
        }
    }
    StmtPtr products;
    if (!Prepare(db.get(), R"SQL(
SELECT logical_key,group_key,producer,model_name,reference_frame,
 product_family,scope,format_version,calibration_type,revision_week,
 variant,current_alias,valid_from,valid_to
FROM products
ORDER BY group_key,revision_week,current_alias)SQL", products, result.error)) return result;
    // Hydrate source routes in one pass to avoid per-product queries.
    // Index rows by product while retaining product and route order.
    int productStep = SQLITE_ROW;
    while ((productStep = sqlite3_step(products.get())) == SQLITE_ROW) {
        AntexProductRecord record;
        record.logicalKey = ColumnText(products.get(),0); record.groupKey = ColumnText(products.get(),1); record.producer = ColumnText(products.get(),2); record.modelName = ColumnText(products.get(),3); record.referenceFrame = ColumnText(products.get(),4); record.productFamily = ColumnText(products.get(),5); record.scope = ColumnText(products.get(),6); record.formatVersion = ColumnText(products.get(),7); record.calibrationType = ColumnText(products.get(),8); record.revisionWeek = sqlite3_column_int(products.get(),9); record.variant = ColumnText(products.get(),10); record.currentAlias = sqlite3_column_int(products.get(),11)!=0; record.validFrom = ParseDate(ColumnText(products.get(),12)); record.validTo = ParseDate(ColumnText(products.get(),13));
        result.products.push_back(std::move(record));
    }
    if (productStep != SQLITE_DONE) {
        result.error = SqliteError(db.get(), L"读取 ANTEX 产品失败");
        return result;
    }
    // Compute revision boundaries from the complete catalogue before
    // filtering by date.  Otherwise a different query range can change which
    // revision appears effective for the same day.
    if (!explicitCatalog) ApplyEffectiveRevisionWindows(result.products);
    if (!explicitCatalog) std::erase_if(result.products, [&](const AntexProductRecord& record) {
        return ToSysDays(record.validTo) < a
            || ToSysDays(record.validFrom) > b
            || ToSysDays(record.validFrom) > ToSysDays(record.validTo);
    });
    std::unordered_map<std::string, std::vector<std::size_t>> routeTargets;
    for (std::size_t index = 0; index < result.products.size(); ++index) {
        routeTargets[result.products[index].logicalKey].push_back(index);
    }

    StmtPtr routes;
    if (!Prepare(db.get(), R"SQL(
SELECT logical_key,source_key,source_priority,file_name,download_url,archive_class
FROM product_sources ORDER BY logical_key,source_priority,file_name)SQL", routes, result.error)) return result;
    int routeStep = SQLITE_ROW;
    while ((routeStep = sqlite3_step(routes.get())) == SQLITE_ROW) {
        const auto logicalKey = ColumnText(routes.get(), 0);
        const auto target = routeTargets.find(logicalKey);
        if (target == routeTargets.end()) continue;
        const AntexProductRoute route{
            ColumnText(routes.get(), 1), sqlite3_column_int(routes.get(), 2),
            ColumnText(routes.get(), 3), Utf8ToWide(ColumnText(routes.get(), 4)),
            ColumnText(routes.get(), 5)};
        for (const auto index : target->second) {
            result.products[index].routes.push_back(route);
        }
    }
    if (routeStep != SQLITE_DONE) {
        result.error = SqliteError(db.get(), L"读取 ANTEX 下载路线失败");
        return result;
    }
    return result;
}

AntexAvailabilityRangeLookup LoadAntexAvailabilityRange(
    const QueryDate& startDate, const QueryDate& endDate) {
    return LoadAntexRangeImpl(startDate, endDate, false);
}
AntexAvailabilityRangeLookup LoadAntexAvailabilityCatalog() {
    return LoadAntexRangeImpl(MakeQueryDate(1980,1,1), MakeQueryDate(2099,12,31), true);
}

AntexAvailabilityStats GetAntexAvailabilityStats() {
    std::scoped_lock lock(gDatabaseMutex);
    AntexAvailabilityStats result;
    result.databasePath = GetAntexAvailabilityDatabasePath();
    if (!std::filesystem::exists(result.databasePath)) return result;
    DbPtr db;
    if (!OpenDatabase(result.databasePath, db, result.error)
        || !EnsureSchema(db.get(), result.error)) return result;
    StmtPtr statement;
    if (!Prepare(db.get(), "SELECT complete,sources_successful,sources_expected,product_count,route_count,scanned_at FROM catalog_scan WHERE id=1", statement, result.error))
        return result;
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return result;
    result.indexed = true;
    result.complete = sqlite3_column_int(statement.get(), 0) != 0;
    result.sourcesSuccessful = sqlite3_column_int(statement.get(), 1);
    result.sourcesExpected = sqlite3_column_int(statement.get(), 2);
    result.products = sqlite3_column_int64(statement.get(), 3);
    result.sourceRoutes = sqlite3_column_int64(statement.get(), 4);
    result.scannedAt = ColumnText(statement.get(), 5);
    return result;
}

std::filesystem::path GetAntexAvailabilityDatabasePath() {
    const auto explicitPath = EnvironmentPath(L"GNSS_DOWNLOADER_ANTEX_DB_PATH");
    if (!explicitPath.empty()) return explicitPath;
    const auto directory = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
    if (!directory.empty()) {
        return ResolveDatabaseFilePath(directory / L"antex_availability.sqlite");
    }
    return ResolveDatabaseFilePath(GetIgsAvailabilityDatabasePath().parent_path()
        / L"antex_availability.sqlite");
}

int AntexExpectedSourceCount() { return kSourceCount; }
