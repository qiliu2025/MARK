#include "sqlite3.h"

#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 3) {
        std::wcerr << L"用法：DuckDbCompatSmokeTest <gnss_knowledge.duckdb> [observation_search.duckdb|--targeted-2026]\n";
        return 2;
    }
    std::wstring path(argv[1]);
    sqlite3* database = nullptr;
    if (sqlite3_open16(path.c_str(), &database) != SQLITE_OK) {
        std::cerr << "open failed: " << sqlite3_errmsg(database) << '\n';
        sqlite3_close(database);
        return 3;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT COUNT(*), COUNT(DISTINCT station_key) "
        "FROM daily_files WHERE network='IGS' "
        "AND observation_date='2024-01-01';";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK
        || sqlite3_step(statement) != SQLITE_ROW) {
        std::cerr << "query failed: " << sqlite3_errmsg(database) << '\n';
        sqlite3_finalize(statement);
        sqlite3_close(database);
        return 4;
    }
    std::cout << "daily_files=" << sqlite3_column_int64(statement, 0)
              << " stations=" << sqlite3_column_int(statement, 1) << '\n';
    sqlite3_finalize(statement);

    // Aggregate expressions are represented as HUGEINT by DuckDB.  The
    // compatibility layer must narrow them just like SQLite's integer
    // columns; this is also what the GUI uses for its local-index status.
    const char* statsSql =
        "SELECT COALESCE(SUM(file_count),0) FROM daily_scans "
        "WHERE network='IGS' AND status='complete';";
    if (sqlite3_prepare_v2(database, statsSql, -1, &statement, nullptr) != SQLITE_OK
        || sqlite3_step(statement) != SQLITE_ROW
        || sqlite3_column_int64(statement, 0) <= 0) {
        std::cerr << "aggregate query failed: " << sqlite3_errmsg(database) << '\n';
        sqlite3_finalize(statement);
        sqlite3_close(database);
        return 5;
    }
    std::cout << "indexed_files=" << sqlite3_column_int64(statement, 0) << '\n';
    sqlite3_finalize(statement);

    const char* statusSql =
        "SELECT SUM(CASE WHEN is_provisional=0 AND"
        " (SELECT COUNT(*) FROM daily_source_scans ss"
        "   WHERE ss.network=daily_scans.network"
        "     AND ss.observation_date=daily_scans.observation_date"
        "     AND ss.status='complete')=7 THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN is_provisional<>0 OR"
        " (SELECT COUNT(*) FROM daily_source_scans ss"
        "   WHERE ss.network=daily_scans.network"
        "     AND ss.observation_date=daily_scans.observation_date"
        "     AND ss.status='complete')<>7 THEN 1 ELSE 0 END)"
        " FROM daily_scans WHERE network='IGS' AND status='complete';";
    if (sqlite3_prepare_v2(database, statusSql, -1, &statement, nullptr) != SQLITE_OK
        || sqlite3_step(statement) != SQLITE_ROW
        || sqlite3_column_int(statement, 0) <= 0) {
        std::cerr << "status aggregate failed: " << sqlite3_errmsg(database) << '\n';
        sqlite3_finalize(statement);
        sqlite3_close(database);
        return 6;
    }
    std::cout << "complete_days=" << sqlite3_column_int(statement, 0)
              << " provisional_days=" << sqlite3_column_int(statement, 1) << '\n';
    sqlite3_finalize(statement);
    sqlite3_close(database);

    if (argc == 3 && std::wstring(argv[2]) == L"--targeted-2026") {
        const char* targetedSql =
            "WITH wanted(lo,hi) AS (VALUES ('AIRA','AIRB'),"
            "('ARUC','ARUD')) SELECT COUNT(*) FROM wanted w"
            " JOIN daily_observation_files o ON o.network='IGS'"
            " AND o.observation_date=?1"
            " AND o.observation_key>=w.lo AND o.observation_key<w.hi"
            " WHERE o.rinex_format=?2 AND o.sample_interval=?3;";
        sqlite3* targetedDatabase = nullptr;
        if (sqlite3_open16(path.c_str(), &targetedDatabase) != SQLITE_OK
            || sqlite3_prepare_v2(targetedDatabase, targetedSql, -1,
                &statement, nullptr) != SQLITE_OK) {
            std::cerr << "targeted prepare failed: "
                      << sqlite3_errmsg(targetedDatabase) << '\n';
            sqlite3_close(targetedDatabase);
            return 10;
        }
        sqlite3_bind_text(statement, 3, "30S", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 2, 3);
        int total = 0;
        bool firstDay = true;
        for (const char* day : {"2026-01-01", "2026-02-01"}) {
            if (!firstDay) sqlite3_reset(statement);
            firstDay = false;
            sqlite3_bind_text(statement, 1, day, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(statement) != SQLITE_ROW) {
                std::cerr << "targeted query failed: "
                          << sqlite3_errmsg(targetedDatabase) << '\n';
                sqlite3_finalize(statement);
                sqlite3_close(targetedDatabase);
                return 11;
            }
            const int found = sqlite3_column_int(statement, 0);
            std::cout << day << " targeted_files=" << found << '\n';
            total += found;
        }
        sqlite3_finalize(statement);
        sqlite3_close(targetedDatabase);
        return total == 4 ? 0 : 12;
    }

    // Optional bitmap check.  Observation and precise-search indexes contain
    // binary bitmaps; this verifies that the compatibility layer exposes the
    // restored bytes as a BLOB instead of silently returning NULL/text.
    if (argc == 3) {
        sqlite3* bitmapDatabase = nullptr;
        if (sqlite3_open16(argv[2], &bitmapDatabase) != SQLITE_OK) {
            std::cerr << "bitmap database open failed: "
                      << sqlite3_errmsg(bitmapDatabase) << '\n';
            sqlite3_close(bitmapDatabase);
            return 7;
        }
        const char* bitmapSql =
            "SELECT bits FROM profile_blocks WHERE bits IS NOT NULL LIMIT 1;";
        sqlite3_stmt* bitmapStatement = nullptr;
        if (sqlite3_prepare_v2(bitmapDatabase, bitmapSql, -1,
                               &bitmapStatement, nullptr) != SQLITE_OK
            || sqlite3_step(bitmapStatement) != SQLITE_ROW) {
            std::cerr << "bitmap query failed: "
                      << sqlite3_errmsg(bitmapDatabase) << '\n';
            sqlite3_finalize(bitmapStatement);
            sqlite3_close(bitmapDatabase);
            return 8;
        }
        const int bytes = sqlite3_column_bytes(bitmapStatement, 0);
        const void* blob = sqlite3_column_blob(bitmapStatement, 0);
        if (sqlite3_column_type(bitmapStatement, 0) != SQLITE_BLOB
            || blob == nullptr || bytes != 512) {
            std::cerr << "bitmap BLOB check failed: type="
                      << sqlite3_column_type(bitmapStatement, 0)
                      << " bytes=" << bytes << '\n';
            sqlite3_finalize(bitmapStatement);
            sqlite3_close(bitmapDatabase);
            return 9;
        }
        std::cout << "bitmap_bytes=" << bytes << '\n';
        sqlite3_finalize(bitmapStatement);
        sqlite3_close(bitmapDatabase);
    }
    return 0;
}
