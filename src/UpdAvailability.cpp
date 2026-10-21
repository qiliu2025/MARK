#include "UpdAvailability.h"

#include "DateUtil.h"
#include "IgsAvailabilityDb.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sqlite3.h>
#include <string_view>
#include <set>
#include <map>

namespace {

std::mutex gDatabaseMutex;

std::wstring ReadEnvironmentVariable(const wchar_t* name) {
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    std::wstring result(value, length > 0 && value[length - 1] == L'\0'
        ? length - 1 : length);
    free(value);
    return result;
}

std::wstring Utf8ToWide(const char* text) {
    if (text == nullptr || *text == '\0') return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 1) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text, -1, result.data(), length);
    result.resize(static_cast<std::size_t>(length - 1));
    return result;
}

std::wstring ColumnWide(sqlite3_stmt* statement, const int column) {
    return Utf8ToWide(reinterpret_cast<const char*>(
        sqlite3_column_text(statement, column)));
}

std::string DateText(const QueryDate& date) {
    char buffer[32]{};
    sprintf_s(buffer, "%04d-%02d-%02d", date.year, date.month, date.day);
    return buffer;
}

QueryDate ParseDate(const char* text) {
    if (text == nullptr) return {};
    int year = 0;
    int month = 0;
    int day = 0;
    if (sscanf_s(text, "%d-%d-%d", &year, &month, &day) != 3) {
        return {};
    }
    return MakeQueryDate(year, month, day);
}

std::wstring DbError(sqlite3* database, const wchar_t* action) {
    const char* message = database == nullptr
        ? "unknown SQLite error" : sqlite3_errmsg(database);
    std::wstring result(action);
    result += L"：";
    result += Utf8ToWide(message);
    return result;
}

struct DatabaseCloser {
    void operator()(sqlite3* database) const noexcept {
        if (database != nullptr) sqlite3_close(database);
    }
};
using DatabasePtr = std::unique_ptr<sqlite3, DatabaseCloser>;

struct StatementCloser {
    void operator()(sqlite3_stmt* statement) const noexcept {
        if (statement != nullptr) sqlite3_finalize(statement);
    }
};
using StatementPtr = std::unique_ptr<sqlite3_stmt, StatementCloser>;

bool BindText(sqlite3_stmt* statement, const int index,
              const std::string& value) {
    return sqlite3_bind_text(
        statement, index, value.c_str(), static_cast<int>(value.size()),
        SQLITE_TRANSIENT) == SQLITE_OK;
}

bool Prepare(sqlite3* database, const char* sql, StatementPtr& result,
             std::wstring& error) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK) {
        error = DbError(database, L"无法准备 UPD 知识库查询");
        return false;
    }
    result.reset(raw);
    return true;
}

bool OpenReadDatabase(const std::filesystem::path& path,
                      DatabasePtr& database, std::wstring& error) {
    sqlite3* raw = nullptr;
    const int status = sqlite3_open_v2(
        reinterpret_cast<const char*>(path.u8string().c_str()), &raw,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    database.reset(raw);
    if (status != SQLITE_OK || raw == nullptr) {
        error = DbError(raw, L"无法打开 UPD 知识库");
        return false;
    }
    sqlite3_busy_timeout(raw, 10000);
    return true;
}

bool TableExists(sqlite3* database, const char* table) {
    sqlite3_stmt* raw = nullptr;
    const char* sql =
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1;";
    if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK) {
        return false;
    }
    StatementPtr statement(raw);
    sqlite3_bind_text(statement.get(), 1, table, -1, SQLITE_STATIC);
    return sqlite3_step(statement.get()) == SQLITE_ROW;
}

bool IsValidRange(const QueryDate& start, const QueryDate& end) {
    using namespace std::chrono;
    const auto a = sys_days{year{start.year}
        / month{static_cast<unsigned>(start.month)}
        / day{static_cast<unsigned>(start.day)}};
    const auto b = sys_days{year{end.year}
        / month{static_cast<unsigned>(end.month)}
        / day{static_cast<unsigned>(end.day)}};
    return a <= b;
}

int InclusiveDays(const QueryDate& start, const QueryDate& end) {
    if (!IsValidRange(start, end)) return 0;
    using namespace std::chrono;
    const auto a = sys_days{year{start.year}
        / month{static_cast<unsigned>(start.month)}
        / day{static_cast<unsigned>(start.day)}};
    const auto b = sys_days{year{end.year}
        / month{static_cast<unsigned>(end.month)}
        / day{static_cast<unsigned>(end.day)}};
    return static_cast<int>(duration_cast<days>(b - a).count()) + 1;
}

void ReadFiles(sqlite3* database, const std::string& /*where*/,
               const std::string& firstDate, const std::string& lastDate,
               std::vector<UpdProductRecord>& destination,
               std::wstring& error) {
    const char* sql =
        "SELECT logical_key,nominal_date,variant,upd_class,"
        "frequency_combination,constellation,sample_interval,file_name,"
        "download_url,provider,format FROM upd_files "
        "WHERE nominal_date>=?1 AND nominal_date<=?2 ORDER BY nominal_date,variant,file_name;";
    StatementPtr statement;
    if (!Prepare(database, sql, statement, error)) return;
    if (!BindText(statement.get(), 1, firstDate)
        || !BindText(statement.get(), 2, lastDate)) {
        error = DbError(database, L"无法绑定 UPD 日期范围");
        return;
    }
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        UpdProductRecord record;
        record.logicalKey = ColumnWide(statement.get(), 0);
        record.nominalDate = ParseDate(reinterpret_cast<const char*>(
            sqlite3_column_text(statement.get(), 1)));
        record.variant = ColumnWide(statement.get(), 2);
        record.updClass = ColumnWide(statement.get(), 3);
        record.frequencyCombination = ColumnWide(statement.get(), 4);
        record.constellation = ColumnWide(statement.get(), 5);
        record.interval = ColumnWide(statement.get(), 6);
        const auto fileName = ColumnWide(statement.get(), 7);
        record.fileName = fileName;
        const auto url = ColumnWide(statement.get(), 8);
        record.provider = ColumnWide(statement.get(), 9);
        record.format = ColumnWide(statement.get(), 10);
        if (!url.empty()) record.routes.push_back({record.provider, url, fileName});
        destination.push_back(std::move(record));
    }
    if (!TableExists(database, "upd_references") || !TableExists(database, "reference_products")) return;
    std::map<std::wstring, UpdProductRecord*> byKey;
    for (auto& record : destination) byKey.emplace(record.logicalKey, &record);
    const char* references =
        "SELECT r.upd_logical_key,r.reference_type,r.reference_key,r.declared_file_name,"
        "p.nominal_date,p.center,p.series,p.project,p.solution,p.sample_interval,"
        "q.source_id,q.file_name,q.download_url,q.logical_key FROM upd_references r "
        "JOIN upd_files u ON u.logical_key=r.upd_logical_key "
        "LEFT JOIN reference_products p ON p.reference_key=r.reference_key "
        "LEFT JOIN reference_routes q ON q.reference_key=r.reference_key "
        "WHERE u.nominal_date>=?1 AND u.nominal_date<=?2 "
        "ORDER BY r.upd_logical_key,r.reference_type,r.reference_key;";
    StatementPtr refs;
    if (!Prepare(database, references, refs, error)
        || !BindText(refs.get(), 1, firstDate) || !BindText(refs.get(), 2, lastDate)) return;
    while (sqlite3_step(refs.get()) == SQLITE_ROW) {
        const auto found = byKey.find(ColumnWide(refs.get(), 0));
        if (found == byKey.end()) continue;
        auto& record = *found->second;
        const auto key = ColumnWide(refs.get(), 2);
        auto ref = std::ranges::find(record.references, key, &UpdReferenceProduct::key);
        if (ref == record.references.end()) {
            UpdReferenceProduct value;
            value.type=ColumnWide(refs.get(),1);value.key=key;value.fileName=ColumnWide(refs.get(),3);
            value.nominalDate=ParseDate(reinterpret_cast<const char*>(sqlite3_column_text(refs.get(),4)));
            value.center=ColumnWide(refs.get(),5);value.series=ColumnWide(refs.get(),6);
            value.project=ColumnWide(refs.get(),7);value.solution=ColumnWide(refs.get(),8);value.interval=ColumnWide(refs.get(),9);
            record.references.push_back(std::move(value));ref=std::prev(record.references.end());
        }
        if (ref->type==L"DCB" && ref->nominalDate.year>0
            && DateText(ref->nominalDate)!=DateText(record.nominalDate)) record.dcbDateMismatch=true;
        const auto url=ColumnWide(refs.get(),12);
        if(ref->logicalKey.empty())ref->logicalKey=ColumnWide(refs.get(),13);
        if (!url.empty()) ref->routes.push_back({ColumnWide(refs.get(),10),url,ColumnWide(refs.get(),11)});
    }
}

} // namespace

UpdDeclaredInputs LoadUpdDeclaredInputs(const std::string& productId,
    const QueryDate& startDate, const QueryDate& endDate) {
    std::scoped_lock lock(gDatabaseMutex);
    UpdDeclaredInputs result;
    DatabasePtr database;
    std::wstring error;
    if (!OpenReadDatabase(GetUpdAvailabilityDatabasePath(), database, error)
        || !TableExists(database.get(), "upd_references")) return result;
    StatementPtr statement;
    const char* sql =
        "SELECT nominal_date,file_name,sp3_file,clk_file,dcb_file,antex_file,sinex_file "
        "FROM upd_with_dependencies WHERE nominal_date>=?1 AND nominal_date<=?2 "
        "AND (logical_key=?3 OR 'IGMAS-UPD|'||variant||'|'||upd_class||'|'||"
        "frequency_combination||'|'||constellation||'|'||sample_interval=?4) "
        "ORDER BY nominal_date DESC,file_name LIMIT 1;";
    if (!Prepare(database.get(), sql, statement, error)
        || !BindText(statement.get(), 1, DateText(startDate))
        || !BindText(statement.get(), 2, DateText(endDate))
        || !BindText(statement.get(), 3, productId)
        || !BindText(statement.get(), 4, productId)
        || sqlite3_step(statement.get()) != SQLITE_ROW) return result;
    result.date = ColumnWide(statement.get(), 0);
    result.fileName = ColumnWide(statement.get(), 1);
    for (int column = 2; column < 7; ++column)
        result.files.push_back(ColumnWide(statement.get(), column));
    return result;
}

std::filesystem::path GetUpdAvailabilityDatabasePath() {
    const auto explicitPath = ReadEnvironmentVariable(
        L"GNSS_DOWNLOADER_UPD_DB_PATH");
    if (!explicitPath.empty()) {
        // The DuckDB release may be distributed without the legacy
        // ``.sqlite`` hard-link aliases.  Resolve an explicitly configured
        // legacy spelling to its sibling ``.duckdb`` file in the same way as
        // the other availability databases.  The SQLite build is unchanged
        // because ResolveDatabaseFilePath() is a no-op there.
        return ResolveDatabaseFilePath(explicitPath);
    }
    const auto indexDirectory = ReadEnvironmentVariable(
        L"GNSS_DOWNLOADER_INDEX_DIR");
    std::filesystem::path directory;
    if (!indexDirectory.empty()) {
        directory = indexDirectory;
    } else {
        directory = GetIgsAvailabilityDatabasePath().parent_path();
    }
    const auto nested = ResolveDatabaseFilePath(
        directory / L"upd" / L"_availability.sqlite");
    std::error_code error;
    if (std::filesystem::is_regular_file(nested, error)) return nested;
    const auto flat = ResolveDatabaseFilePath(directory / L"upd_availability.sqlite");
    if (std::filesystem::is_regular_file(flat, error)) return flat;
    return nested;
}

UpdAvailabilityLookup LoadUpdAvailability(const QueryDate& date) {
    std::scoped_lock lock(gDatabaseMutex);
    UpdAvailabilityLookup result;
    result.databasePath = GetUpdAvailabilityDatabasePath();
    DatabasePtr database;
    if (!OpenReadDatabase(result.databasePath, database, result.error)) return result;
    if (!TableExists(database.get(), "upd_files")) {
        result.error = L"UPD 知识库缺少 upd_files 表";
        return result;
    }
    result.indexed = true;
    std::set<std::string> completeDates;
    if (TableExists(database.get(), "upd_daily_scans")) {
        StatementPtr status;
        if (!Prepare(database.get(),
                "SELECT status FROM upd_daily_scans WHERE nominal_date=?1;",
                status, result.error)) return result;
        if (!BindText(status.get(), 1, DateText(date))) {
            result.error = DbError(database.get(), L"无法绑定 UPD 日期");
            return result;
        }
        if (sqlite3_step(status.get()) == SQLITE_ROW) {
            result.complete = ColumnWide(status.get(), 0) == L"complete";
        }
    }
    const auto text = DateText(date);
    ReadFiles(database.get(), "", text, text, result.products, result.error);
    return result;
}

UpdAvailabilityRangeLookup LoadUpdAvailabilityRange(
    const QueryDate& startDate, const QueryDate& endDate) {
    std::scoped_lock lock(gDatabaseMutex);
    UpdAvailabilityRangeLookup result;
    result.databasePath = GetUpdAvailabilityDatabasePath();
    result.expectedDays = InclusiveDays(startDate, endDate);
    if (result.expectedDays <= 0) {
        result.error = L"UPD 日期范围无效";
        return result;
    }
    DatabasePtr database;
    if (!OpenReadDatabase(result.databasePath, database, result.error)) return result;
    if (!TableExists(database.get(), "upd_files")) {
        result.error = L"UPD 知识库缺少 upd_files 表";
        return result;
    }
    result.indexed = true;
    std::set<std::string> completeDates;
    if (TableExists(database.get(), "upd_daily_scans")) {
        StatementPtr status;
        const char* sql =
            "SELECT nominal_date,status FROM upd_daily_scans "
            "WHERE nominal_date>=?1 AND nominal_date<=?2 ORDER BY nominal_date;";
        if (!Prepare(database.get(), sql, status, result.error)) return result;
        const auto first = DateText(startDate);
        const auto last = DateText(endDate);
        if (!BindText(status.get(), 1, first) || !BindText(status.get(), 2, last)) {
            result.error = DbError(database.get(), L"无法绑定 UPD 日期范围");
            return result;
        }
        while (sqlite3_step(status.get()) == SQLITE_ROW) {
            const auto date = ParseDate(reinterpret_cast<const char*>(
                sqlite3_column_text(status.get(), 0)));
            if (ColumnWide(status.get(), 1) == L"complete") {
                completeDates.insert(DateText(date));
            } else if (date.dayOfYear > 0) {
                result.incompleteDates.push_back(date);
            }
        }
    }
    // Account for dates with no scan row at all (or a missing scan table).
    using namespace std::chrono;
    const auto firstDay = sys_days{year{startDate.year}
        / month{static_cast<unsigned>(startDate.month)}
        / day{static_cast<unsigned>(startDate.day)}};
    for (int i = 0; i < result.expectedDays; ++i) {
        const auto ymd = year_month_day{firstDay + days{i}};
        const auto date = MakeQueryDate(static_cast<int>(ymd.year()),
            static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
        if (completeDates.contains(DateText(date))) ++result.indexedDays;
        else if (std::ranges::none_of(result.incompleteDates,
                [&](const auto& value) { return DateText(value) == DateText(date); }))
            result.incompleteDates.push_back(date);
    }
    ReadFiles(database.get(), "", DateText(startDate), DateText(endDate),
        result.products, result.error);
    result.complete = result.indexedDays == result.expectedDays
        && result.incompleteDates.empty();
    return result;
}
