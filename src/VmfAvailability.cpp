#include "VmfAvailability.h"

#include "DateUtil.h"
#include "IgsAvailabilityDb.h"

#include "sqlite3.h"

#include <windows.h>

#include <format>
#include <memory>
#include <string_view>

namespace {

struct DatabaseCloser {
    void operator()(sqlite3* value) const { if (value) sqlite3_close(value); }
};
struct StatementCloser {
    void operator()(sqlite3_stmt* value) const { if (value) sqlite3_finalize(value); }
};
using Database = std::unique_ptr<sqlite3, DatabaseCloser>;
using Statement = std::unique_ptr<sqlite3_stmt, StatementCloser>;

std::wstring Utf8ToWide(const std::string_view value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::filesystem::path EnvironmentPath(const wchar_t* name) {
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length <= 1) return {};
    std::wstring value(length, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), length);
    if (!written || written >= length) return {};
    value.resize(written);
    return value;
}

std::string DateText(const QueryDate& day) {
    return std::format("{:04}-{:02}-{:02}", day.year, day.month, day.day);
}

std::string Text(sqlite3_stmt* statement, const int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char*>(value) : "";
}

bool OpenRead(Database& database, std::wstring& error) {
    const auto path = GetVmfAvailabilityDatabasePath();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        error = L"VMF 知识库不存在：" + path.wstring();
        return false;
    }
    sqlite3* raw = nullptr;
    const auto name = path.u8string();
    const int status = sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &raw,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    database.reset(raw);
    if (status != SQLITE_OK || !raw) {
        error = L"无法读取 VMF 知识库：" + path.wstring();
        return false;
    }
    return true;
}

bool Prepare(sqlite3* db, const char* sql, Statement& statement,
             std::wstring& error) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) {
        const auto* message = sqlite3_errmsg(db);
        error = L"VMF 查询失败：" + Utf8ToWide(message ? message : "unknown");
        return false;
    }
    statement.reset(raw);
    return true;
}

} // namespace

std::filesystem::path GetVmfAvailabilityDatabasePath() {
    const auto explicitPath = EnvironmentPath(L"GNSS_DOWNLOADER_VMF_DB_PATH");
    if (!explicitPath.empty()) return ResolveDatabaseFilePath(explicitPath);
    const auto directory = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
    if (!directory.empty()) {
        return directory / L"vmf_availability.duckdb";
    }
    return GetIgsAvailabilityDatabasePath().parent_path()
        / L"vmf_availability.duckdb";
}

VmfRangeLookup LoadVmfFamilyRange(const QueryDate& start, const QueryDate& end) {
    VmfRangeLookup result;
    result.databasePath = GetVmfAvailabilityDatabasePath();
    Database database;
    if (!OpenRead(database, result.error)) return result;
    Statement statement;
    if (!Prepare(database.get(),
            "SELECT family_key,model,resolution,grade,latency,"
            "COUNT(DISTINCT nominal_date) FROM products "
            "WHERE nominal_date>=? AND nominal_date<=? "
            "GROUP BY family_key,model,resolution,grade,latency "
            "ORDER BY model,resolution,grade,family_key",
            statement, result.error)) return result;
    const auto first = DateText(start), last = DateText(end);
    sqlite3_bind_text(statement.get(), 1, first.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, last.c_str(), -1, SQLITE_TRANSIENT);
    int status = SQLITE_ROW;
    while ((status = sqlite3_step(statement.get())) == SQLITE_ROW) {
        result.families.push_back({Text(statement.get(), 0), Text(statement.get(), 1),
            Text(statement.get(), 2), Text(statement.get(), 3),
            Text(statement.get(), 4), sqlite3_column_int(statement.get(), 5)});
    }
    if (status != SQLITE_DONE) {
        result.error = L"读取 VMF 产品族失败："
            + Utf8ToWide(sqlite3_errmsg(database.get()));
    }
    return result;
}

VmfRoutesLookup LoadVmfFamilyRoutes(const std::string& familyKey,
    const QueryDate& start, const QueryDate& end,
    const std::stop_token stopToken) {
    VmfRoutesLookup result;
    Database database;
    if (!OpenRead(database, result.error)) return result;
    Statement statement;
    if (!Prepare(database.get(),
            "SELECT nominal_date,file_name,download_url FROM products "
            "WHERE family_key=? AND nominal_date>=? AND nominal_date<=? "
            "ORDER BY nominal_date,file_name",
            statement, result.error)) return result;
    const auto first = DateText(start), last = DateText(end);
    sqlite3_bind_text(statement.get(), 1, familyKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, first.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 3, last.c_str(), -1, SQLITE_TRANSIENT);
    int status = SQLITE_ROW;
    while (!stopToken.stop_requested()
           && (status = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const auto day = Text(statement.get(), 0);
        int year = 0, month = 0, date = 0;
        if (sscanf_s(day.c_str(), "%d-%d-%d", &year, &month, &date) != 3) continue;
        const auto fileName = Text(statement.get(), 1);
        const auto url = Text(statement.get(), 2);
        if (fileName.empty() || url.empty()) continue;
        result.records.push_back({MakeQueryDate(year, month, date),
            familyKey + "|" + day, {L"TU Wien VMF", Utf8ToWide(url),
                Utf8ToWide(fileName)}});
    }
    if (!stopToken.stop_requested() && status != SQLITE_DONE) {
        result.error = L"读取 VMF 下载路线失败："
            + Utf8ToWide(sqlite3_errmsg(database.get()));
    }
    return result;
}
