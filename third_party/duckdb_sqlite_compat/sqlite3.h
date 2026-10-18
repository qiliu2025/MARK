#pragma once

// A deliberately small SQLite C-API compatibility surface used by the
// DuckDB build of GNSSDownloader.  The existing application is read-mostly
// and uses prepared statements, scalar binds and row iteration; this header
// lets that code run against a DuckDB file without changing its model code.

#include <stddef.h>
#include <stdint.h>

typedef int64_t sqlite3_int64;
typedef uint64_t sqlite3_uint64;
typedef void (*sqlite3_destructor_type)(void*);

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

#define SQLITE_OK 0
#define SQLITE_ERROR 1
#define SQLITE_INTERNAL 2
#define SQLITE_PERM 3
#define SQLITE_ABORT 4
#define SQLITE_BUSY 5
#define SQLITE_LOCKED 6
#define SQLITE_NOMEM 7
#define SQLITE_READONLY 8
#define SQLITE_INTERRUPT 9
#define SQLITE_IOERR 10
#define SQLITE_CORRUPT 11
#define SQLITE_NOTFOUND 12
#define SQLITE_FULL 13
#define SQLITE_CANTOPEN 14
#define SQLITE_PROTOCOL 15
#define SQLITE_EMPTY 16
#define SQLITE_SCHEMA 17
#define SQLITE_TOOBIG 18
#define SQLITE_CONSTRAINT 19
#define SQLITE_MISMATCH 20
#define SQLITE_MISUSE 21
#define SQLITE_ROW 100
#define SQLITE_DONE 101

#define SQLITE_INTEGER 1
#define SQLITE_FLOAT 2
#define SQLITE_TEXT 3
#define SQLITE_BLOB 4
#define SQLITE_NULL 5

#define SQLITE_OPEN_READONLY 0x00000001
#define SQLITE_OPEN_READWRITE 0x00000002
#define SQLITE_OPEN_CREATE 0x00000004
#define SQLITE_OPEN_NOMUTEX 0x00008000
#define SQLITE_OPEN_FULLMUTEX 0x00010000
#define SQLITE_TRANSIENT ((sqlite3_destructor_type)(intptr_t)-1)
#define SQLITE_STATIC ((sqlite3_destructor_type)0)

#ifdef __cplusplus
extern "C" {
#endif

int sqlite3_open(const char* filename, sqlite3** database);
int sqlite3_open_v2(const char* filename, sqlite3** database, int flags,
                    const char* vfs);
int sqlite3_open16(const void* filename, sqlite3** database);
int sqlite3_close(sqlite3* database);
int sqlite3_busy_timeout(sqlite3* database, int milliseconds);
const char* sqlite3_errmsg(sqlite3* database);

int sqlite3_exec(sqlite3* database, const char* sql,
                 int (*callback)(void*, int, char**, char**), void* callback_arg,
                 char** error_message);
void sqlite3_free(void* memory);

int sqlite3_prepare_v2(sqlite3* database, const char* sql, int byte_count,
                       sqlite3_stmt** statement, const char** tail);
int sqlite3_finalize(sqlite3_stmt* statement);
int sqlite3_reset(sqlite3_stmt* statement);
int sqlite3_clear_bindings(sqlite3_stmt* statement);
int sqlite3_step(sqlite3_stmt* statement);

int sqlite3_bind_text(sqlite3_stmt* statement, int index, const char* value,
                      int byte_count, sqlite3_destructor_type destructor);
int sqlite3_bind_blob(sqlite3_stmt* statement, int index, const void* value,
                      int byte_count, sqlite3_destructor_type destructor);
int sqlite3_bind_int(sqlite3_stmt* statement, int index, int value);
int sqlite3_bind_int64(sqlite3_stmt* statement, int index, sqlite3_int64 value);
int sqlite3_bind_double(sqlite3_stmt* statement, int index, double value);
int sqlite3_bind_null(sqlite3_stmt* statement, int index);

int sqlite3_column_count(sqlite3_stmt* statement);
int sqlite3_column_type(sqlite3_stmt* statement, int column);
const unsigned char* sqlite3_column_text(sqlite3_stmt* statement, int column);
const void* sqlite3_column_blob(sqlite3_stmt* statement, int column);
int sqlite3_column_bytes(sqlite3_stmt* statement, int column);
int sqlite3_column_int(sqlite3_stmt* statement, int column);
sqlite3_int64 sqlite3_column_int64(sqlite3_stmt* statement, int column);
double sqlite3_column_double(sqlite3_stmt* statement, int column);

int sqlite3_table_column_metadata(sqlite3* database, const char* schema,
                                  const char* table, const char* column,
                                  const char** data_type, const char** collation,
                                  int* not_null, int* primary_key,
                                  int* auto_increment);

#ifdef __cplusplus
}
#endif
