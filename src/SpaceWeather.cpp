#include "SpaceWeather.h"

#include "DateUtil.h"
#include "IgsAvailabilityDb.h"

#include "sqlite3.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace {

struct DbCloser {
    void operator()(sqlite3* value) const noexcept {
        if (value != nullptr) sqlite3_close(value);
    }
};
using DbPtr = std::unique_ptr<sqlite3, DbCloser>;

struct StmtCloser {
    void operator()(sqlite3_stmt* value) const noexcept {
        if (value != nullptr) sqlite3_finalize(value);
    }
};
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtCloser>;

struct FileStamp {
    bool exists = false;
    std::uintmax_t size = 0;
    std::int64_t modified = 0;
    bool walExists = false;
    std::uintmax_t walSize = 0;
    std::int64_t walModified = 0;
    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};

FileStamp Stamp(const std::filesystem::path& path) {
    FileStamp result;
    std::error_code ec;
    result.exists = std::filesystem::is_regular_file(path, ec);
    if (result.exists) {
        ec.clear();
        result.size = std::filesystem::file_size(path, ec);
        ec.clear();
        result.modified = std::filesystem::last_write_time(path, ec)
            .time_since_epoch().count();
    }
    const auto wal = std::filesystem::path(path.wstring() + L"-wal");
    ec.clear();
    result.walExists = std::filesystem::is_regular_file(wal, ec);
    if (result.walExists) {
        ec.clear();
        result.walSize = std::filesystem::file_size(wal, ec);
        ec.clear();
        result.walModified = std::filesystem::last_write_time(wal, ec)
            .time_since_epoch().count();
    }
    return result;
}

struct Cache {
    FileStamp stamp;
    std::unordered_map<int, SpaceWeatherDay> days;
    std::wstring error;
};

std::mutex gCacheMutex;
std::unordered_map<std::wstring, std::shared_ptr<const Cache>> gCaches;

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

std::wstring SqlError(sqlite3* db, const wchar_t* action) {
    const char* message = db == nullptr ? nullptr : sqlite3_errmsg(db);
    std::string text = message == nullptr ? "unknown SQLite error" : message;
    return std::wstring(action) + L"：" +
        std::wstring(text.begin(), text.end());
}

bool ParseDate(const std::string& text, QueryDate& date) {
    if (text.size() < 10 || text[4] != '-' || text[7] != '-') return false;
    int y = 0, m = 0, d = 0;
    if (sscanf_s(text.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return false;
    date = MakeQueryDate(y, m, d);
    return date.dayOfYear > 0;
}

bool ToOrdinal(const QueryDate& date, int& ordinal) {
    const auto ymd = std::chrono::year{date.year}
        / std::chrono::month{static_cast<unsigned>(date.month)}
        / std::chrono::day{static_cast<unsigned>(date.day)};
    if (!ymd.ok()) return false;
    const auto value = std::chrono::sys_days{ymd}.time_since_epoch().count();
    if (value < std::numeric_limits<int>::min()
        || value > std::numeric_limits<int>::max()) return false;
    ordinal = static_cast<int>(value);
    return true;
}

std::shared_ptr<const Cache> Load(const std::filesystem::path& path) {
    const auto absolute = std::filesystem::absolute(path);
    const std::wstring key = absolute.wstring();
    const FileStamp current = Stamp(absolute);
    std::scoped_lock lock(gCacheMutex);
    if (const auto found = gCaches.find(key); found != gCaches.end()
        && found->second != nullptr && found->second->stamp == current) {
        return found->second;
    }
    auto result = std::make_shared<Cache>();
    result->stamp = current;
    if (!current.exists || current.size == 0) {
        result->error = L"空间天气知识库不存在：" + absolute.wstring();
        gCaches[key] = result;
        return result;
    }
    sqlite3* raw = nullptr;
    const int status = sqlite3_open_v2(
        reinterpret_cast<const char*>(absolute.u8string().c_str()), &raw,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    DbPtr db(raw);
    if (status != SQLITE_OK || raw == nullptr) {
        result->error = SqlError(raw, L"无法打开空间天气知识库");
        gCaches[key] = result;
        return result;
    }
    sqlite3_busy_timeout(db.get(), 10000);
    const char* sql =
        "SELECT date,kp_max,ap_daily,dst_min,dst_mean,f107_daily," 
        "f107_81day,kp_count,dst_count,source,status,f107_adjusted FROM daily;";
    sqlite3_stmt* statement = nullptr;
    int prepared = sqlite3_prepare_v2(db.get(), sql, -1, &statement, nullptr);
    if (prepared != SQLITE_OK) {
        if (statement) sqlite3_finalize(statement);
        statement = nullptr;
        // Legacy indexes contain observed flux only. Never substitute it for adjusted flux.
        prepared = sqlite3_prepare_v2(db.get(),
            "SELECT date,kp_max,ap_daily,dst_min,dst_mean,f107_daily,"
            "f107_81day,kp_count,dst_count,source,status,NULL FROM daily;",
            -1, &statement, nullptr);
    }
    if (prepared != SQLITE_OK) {
        result->error = SqlError(db.get(), L"读取空间天气知识库失败");
        gCaches[key] = result;
        return result;
    }
    StmtPtr stmt(statement);
    while (true) {
        const int step = sqlite3_step(stmt.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            result->error = SqlError(db.get(), L"读取空间天气日记录失败");
            result->days.clear();
            break;
        }
        const auto* rawDate = sqlite3_column_text(stmt.get(), 0);
        QueryDate date{};
        if (rawDate == nullptr
            || !ParseDate(reinterpret_cast<const char*>(rawDate), date)) {
            continue;
        }
        int ordinal = 0;
        if (!ToOrdinal(date, ordinal)) continue;
        SpaceWeatherDay row;
        row.date = date;
        const auto number = [&](const int column) {
            return sqlite3_column_type(stmt.get(), column) == SQLITE_NULL
                ? std::numeric_limits<double>::quiet_NaN()
                : sqlite3_column_double(stmt.get(), column);
        };
        row.kpMax = number(1);
        row.apDaily = number(2);
        row.dstMin = number(3);
        row.dstMean = number(4);
        row.f107 = number(5);
        row.f10781 = number(6);
        row.f107Adjusted = number(11);
        row.kpCount = sqlite3_column_int(stmt.get(), 7);
        row.dstCount = sqlite3_column_int(stmt.get(), 8);
        const auto text = [&](const int column) {
            const auto* value = sqlite3_column_text(stmt.get(), column);
            return value == nullptr ? std::string{}
                : std::string(reinterpret_cast<const char*>(value));
        };
        row.source = text(9);
        row.status = text(10);
        result->days[ordinal] = std::move(row);
    }
    if (result->error.empty() && !(Stamp(absolute) == current)) {
        result->stamp = {};
    }
    gCaches[key] = result;
    return result;
}

bool Finite(const double value) {
    return std::isfinite(value)
        && value > -1.0e8 && value < 1.0e8;
}

bool Pass(const double value, const SpaceWeatherFilter& filter,
          const bool dst) {
    if (filter.mode == SpaceWeatherFilterMode::Ignore) return true;
    if (!Finite(value) || !Finite(filter.threshold)) return false;
    if (filter.mode == SpaceWeatherFilterMode::KeepRange) {
        return Finite(filter.upperThreshold)
            && value >= std::min(filter.threshold, filter.upperThreshold)
            && value <= std::max(filter.threshold, filter.upperThreshold);
    }
    const bool thresholdHit = dst
        ? value <= filter.threshold
        : value > filter.threshold;
    return filter.mode == SpaceWeatherFilterMode::KeepAbove
        ? thresholdHit : !thresholdHit;
}

} // namespace

std::filesystem::path DefaultSpaceWeatherDatabasePath(
    const std::filesystem::path& knowledgeBaseDirectory) {
    if (!knowledgeBaseDirectory.empty()) {
        return ResolveDatabaseFilePath(
            knowledgeBaseDirectory / L"space_weather_availability.sqlite");
    }
#ifdef GNSS_DOWNLOADER_DUCKDB_BACKEND
    {
        std::wstring moduleBuffer(32768, L'\0');
        const DWORD moduleLength = GetModuleFileNameW(
            nullptr, moduleBuffer.data(), static_cast<DWORD>(moduleBuffer.size()));
        if (moduleLength > 0 && moduleLength < moduleBuffer.size()) {
            moduleBuffer.resize(moduleLength);
            const auto bundled = std::filesystem::path(moduleBuffer).parent_path()
                .parent_path() / L"knowledge-base"
                / L"space_weather_availability.sqlite";
            const auto resolved = ResolveDatabaseFilePath(bundled);
            std::error_code bundledError;
            if (std::filesystem::is_regular_file(resolved, bundledError)) return resolved;
        }
    }
#endif
    // Match the observation index's storage discovery rules.  This keeps the
    // GUI and the standalone builder on the same database when the user has
    // configured a custom knowledge-base directory, while retaining the
    // project-local runtime-data fallback used by command-line builds.
    if (const auto configured = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
        !configured.empty()) {
        return ResolveDatabaseFilePath(configured / L"space_weather_availability.sqlite");
    }
    if (const auto configured = EnvironmentPath(L"GNSS_DOWNLOADER_DB_PATH");
        !configured.empty()) {
        return ResolveDatabaseFilePath(
            configured.parent_path() / L"space_weather_availability.sqlite");
    }
    auto local = EnvironmentPath(L"LOCALAPPDATA");
    if (local.empty()) local = std::filesystem::current_path();
    const auto settings = local / L"GNSSDownloader" / L"settings.ini";
    wchar_t configured[32768]{};
    const DWORD length = GetPrivateProfileStringW(
        L"storage", L"database_path", L"", configured,
        static_cast<DWORD>(std::size(configured)), settings.c_str());
    if (length > 0 && length < std::size(configured) - 1) {
        return std::filesystem::path(configured).parent_path()
            / L"space_weather_availability.sqlite";
    }
    return ResolveDatabaseFilePath(std::filesystem::path(L"runtime-data")
        / L"space_weather_availability.sqlite");
}

bool HasSpaceWeatherFilter(const ObservationRangeOptions& options) {
    return options.kpMaxFilter.mode != SpaceWeatherFilterMode::Ignore
        || options.dstMinFilter.mode != SpaceWeatherFilterMode::Ignore
        || options.f107Filter.mode != SpaceWeatherFilterMode::Ignore;
}

SpaceWeatherPreview PreviewSpaceWeatherDates(
    const std::filesystem::path& databasePath,
    const QueryDate& startDate,
    const QueryDate& endDate,
    const ObservationRangeOptions& options,
    const std::stop_token stopToken) {
    SpaceWeatherPreview result;
    int first = 0, last = 0;
    if (!ToOrdinal(startDate, first) || !ToOrdinal(endDate, last) || last < first) {
        result.error = L"空间天气日期范围无效。";
        return result;
    }
    const auto selection = SelectSpaceWeatherDates(
        databasePath, startDate, endDate, options, stopToken);
    if (!selection.error.empty()) {
        result.error = selection.error;
        return result;
    }
    const auto cache = Load(databasePath);
    if (!cache || !cache->error.empty()) {
        result.error = cache ? cache->error : L"无法加载空间天气知识库。";
        return result;
    }
    result.requestedDays = last - first + 1;
    result.days.reserve(static_cast<std::size_t>(result.requestedDays));
    for (int ordinal = first; ordinal <= last; ++ordinal) {
        if (((ordinal - first) & 0x3ff) == 0 && stopToken.stop_requested()) {
            result.error = L"空间天气日期筛选已取消。";
            result.days.clear();
            return result;
        }
        SpaceWeatherPreviewDay day;
        day.ordinal = ordinal;
        const auto found = cache->days.find(ordinal);
        if (found != cache->days.end()) {
            day.values = found->second;
            if (options.useAdjustedF107) day.values.f107 = day.values.f107Adjusted;
        } else {
            const auto missing = std::numeric_limits<double>::quiet_NaN();
            day.values.kpMax = day.values.dstMin = day.values.f107 = missing;
        }
        const auto classify = [&](double value, const SpaceWeatherFilter& filter,
                                  bool dst, unsigned bit) {
            if (filter.mode == SpaceWeatherFilterMode::Ignore) return;
            if (!Finite(value)) day.missingMetrics |= bit;
            else if (!Pass(value, filter, dst)) day.failedMetrics |= bit;
        };
        classify(day.values.kpMax, options.kpMaxFilter, false, 1);
        classify(day.values.dstMin, options.dstMinFilter, true, 2);
        classify(day.values.f107, options.f107Filter, false, 4);
        day.retained = selection.Contains(ordinal);
        if (day.retained) ++result.retainedDays;
        if (day.missingMetrics != 0) ++result.missingDays;
        result.days.push_back(std::move(day));
    }
    return result;
}

SpaceWeatherDateSelection SelectSpaceWeatherDates(
    const std::filesystem::path& databasePath,
    const QueryDate& startDate,
    const QueryDate& endDate,
    const ObservationRangeOptions& options,
    const std::stop_token stopToken) {
    SpaceWeatherDateSelection result;
    result.active = HasSpaceWeatherFilter(options);
    if (!result.active) return result;
    int first = 0, last = 0;
    if (!ToOrdinal(startDate, first) || !ToOrdinal(endDate, last) || last < first) {
        result.error = L"空间天气日期范围无效。";
        return result;
    }
    const auto cache = Load(databasePath);
    if (!cache || !cache->error.empty()) {
        result.error = cache == nullptr
            ? L"无法加载空间天气知识库。" : cache->error;
        return result;
    }
    result.requestedDays = last - first + 1;
    result.ordinals.reserve(static_cast<std::size_t>(result.requestedDays));
    result.ordinalSet.reserve(static_cast<std::size_t>(result.requestedDays));
    for (int ordinal = first; ordinal <= last; ++ordinal) {
        if ((((ordinal - first) & 0x3ff) == 0) && stopToken.stop_requested()) {
            result.error = L"空间天气日期筛选已取消。";
            return result;
        }
        const auto found = cache->days.find(ordinal);
        if (found == cache->days.end()) {
            ++result.missingDays;
            continue;
        }
        const auto& row = found->second;
        const double f107 = options.useAdjustedF107 ? row.f107Adjusted : row.f107;
        const bool metricsPresent =
            (options.kpMaxFilter.mode == SpaceWeatherFilterMode::Ignore
                || Finite(row.kpMax))
            && (options.dstMinFilter.mode == SpaceWeatherFilterMode::Ignore
                || Finite(row.dstMin))
            && (options.f107Filter.mode == SpaceWeatherFilterMode::Ignore
                || Finite(f107));
        if (!metricsPresent) {
            ++result.missingDays;
            continue;
        }
        if (!Pass(row.kpMax, options.kpMaxFilter, false)
            || !Pass(row.dstMin, options.dstMinFilter, true)
            || !Pass(f107, options.f107Filter, false)) {
            continue;
        }
        result.ordinals.push_back(ordinal);
        result.ordinalSet.insert(ordinal);
    }
    result.matchedDays = static_cast<int>(result.ordinals.size());
    if (result.missingDays > 0) {
        result.warning = std::format(
            L"空间天气知识库在所选范围缺少 {} 天；缺少指标的日期未参与高级筛选。",
            result.missingDays);
    }
    if (result.matchedDays == 0 && result.warning.empty()) {
        result.warning = L"没有日期满足当前空间天气筛选条件。";
    }
    return result;
}
