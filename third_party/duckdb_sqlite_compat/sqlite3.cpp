#include "sqlite3.h"

#include "duckdb.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string WideToUtf8(const wchar_t* value) {
    if (value == nullptr) return {};
    const int length = static_cast<int>(wcslen(value));
    if (length == 0) return {};
    const int bytes = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value, length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length,
                        result.data(), bytes, nullptr, nullptr);
    return result;
}

bool DatabasePathUtf8(const char* filename, std::string& result) {
    result = filename == nullptr ? ":memory:" : filename;
    if (result.empty()) return true;
    if (result.size() > static_cast<std::size_t>(
            std::numeric_limits<int>::max())) return false;
    const int length = static_cast<int>(result.size());
    // SQLite and DuckDB paths are UTF-8. Preserve valid UTF-8 exactly,
    // including non-existent files: a failed open is never a reason to
    // reinterpret an otherwise valid path using the Windows code page.
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            result.data(), length, nullptr, 0) > 0) return true;

    // Some older Windows callers pass filesystem::path::string(), which
    // uses the active ANSI code page. Accept that legacy representation
    // only after strict decoding and a lossless round trip; never replace
    // invalid bytes or use best-fit character substitutions.
    const UINT codePage = GetACP();
    if (codePage == CP_UTF8) return false;
    const int chars = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
        result.data(), length, nullptr, 0);
    if (chars <= 0) return false;
    std::wstring wide(static_cast<std::size_t>(chars), L'\0');
    if (MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
            result.data(), length, wide.data(), chars) != chars) return false;
    BOOL substituted = FALSE;
    const int bytes = WideCharToMultiByte(codePage, WC_NO_BEST_FIT_CHARS,
        wide.data(), chars, nullptr, 0, nullptr, &substituted);
    if (bytes <= 0 || substituted) return false;
    std::string original(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(codePage, WC_NO_BEST_FIT_CHARS,
            wide.data(), chars, original.data(), bytes, nullptr,
            &substituted) != bytes || substituted || original != result) {
        return false;
    }
    result = WideToUtf8(wide.c_str());
    return !result.empty();
}

bool PathExistsUtf8(const std::string_view value) {
    if (value.empty()) return false;
    const int chars = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (chars <= 0) return false;
    std::wstring wide(static_cast<std::size_t>(chars), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), wide.data(), chars) <= 0) {
        return false;
    }
    return GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// DuckDB takes ENCRYPTION_KEY on ATTACH, so encrypted files use an
// in-memory catalog rather than duckdb_open_ext().
std::string EnvironmentValue(const char* name) {
    if (name == nullptr || *name == '\0') return {};
    const DWORD required = GetEnvironmentVariableA(name, nullptr, 0);
    if (required == 0) return {};
    std::string value(static_cast<std::size_t>(required), '\0');
    const DWORD written = GetEnvironmentVariableA(
        name, value.data(), required);
    if (written == 0 || written >= required) return {};
    value.resize(written);
    return value;
}

std::string DatabaseEncryptionKey() {
    // A compiled key takes precedence. Builds without one use the environment.
#ifdef GNSS_DOWNLOADER_COMPILED_DB_KEY
    constexpr std::string_view compiled = GNSS_DOWNLOADER_COMPILED_DB_KEY;
    if (!compiled.empty()) return std::string(compiled);
#endif
    // Accept the older knowledge-base variable as an alias.
    auto key = EnvironmentValue("GNSS_DOWNLOADER_DB_KEY");
    if (key.empty()) key = EnvironmentValue("GNSS_DOWNLOADER_KB_KEY");
    return key;
}

std::string SqlString(const std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('\'');
    for (const char ch : value) {
        if (ch == '\'') result.push_back('\'');
        result.push_back(ch);
    }
    result.push_back('\'');
    return result;
}

bool QuerySucceeded(
    duckdb_connection connection,
    const std::string& sql,
    std::string& error) {
    duckdb_result result{};
    const auto status = duckdb_query(connection, sql.c_str(), &result);
    if (status != DuckDBSuccess) {
        const auto* message = duckdb_result_error(&result);
        error = message == nullptr ? "DuckDB query failed" : message;
        duckdb_destroy_result(&result);
        return false;
    }
    duckdb_destroy_result(&result);
    return true;
}

}  // namespace

struct sqlite3 {
    duckdb_database database{};
    duckdb_connection connection{};
    std::string error;
};

struct sqlite3_stmt {
    struct TextCell {
        std::string value;
        idx_t row = 0;
        int length = 0;
        bool valid = false;
        bool null = false;
    };
    sqlite3* owner = nullptr;
    duckdb_prepared_statement prepared{};
    // Parameterized statements may expose one INVALID column until their
    // parameters are bound. Refresh this cache from the executed result;
    // otherwise the first real column (notably Site Log source_url) is lost.
    // Cache scalar type ids once per execution, rather than allocating a
    // logical type for every cell in a large station/product list.
    std::vector<duckdb_type> column_types;
    duckdb_result result{};
    duckdb_data_chunk chunk{};
    idx_t current_row = 0;
    bool row_ready = false;
    bool started = false;
    bool finished = false;
    // SQLite pointers from different columns must coexist until step/reset.
    // A single shared scratch string would overwrite an earlier source URL
    // when the caller subsequently reads a model or another URL.
    std::vector<TextCell> text_cells;
};

namespace {

void SetError(sqlite3* database, const char* message) {
    if (database != nullptr) database->error = message == nullptr ? "" : message;
}

void DestroyChunk(sqlite3_stmt* statement) {
    if (statement != nullptr && statement->chunk != nullptr) {
        duckdb_destroy_data_chunk(&statement->chunk);
        statement->chunk = nullptr;
    }
}

void DestroyResult(sqlite3_stmt* statement) {
    if (statement != nullptr && statement->result.internal_data != nullptr) {
        duckdb_destroy_result(&statement->result);
        statement->result = {};
    }
}

bool IsNull(sqlite3_stmt* statement, int column) {
    if (statement == nullptr || statement->chunk == nullptr || column < 0) return true;
    const auto count = duckdb_data_chunk_get_column_count(statement->chunk);
    if (static_cast<idx_t>(column) >= count) return true;
    auto vector = duckdb_data_chunk_get_vector(statement->chunk, column);
    const auto validity = duckdb_vector_get_validity(vector);
    if (validity == nullptr) return false;
    const auto row = statement->current_row;
    return (validity[row / 64U] & (uint64_t{1} << (row % 64U))) == 0;
}

duckdb_type ColumnDuckType(sqlite3_stmt* statement, int column) {
    if (statement == nullptr || column < 0) {
        return DUCKDB_TYPE_INVALID;
    }
    const auto index = static_cast<std::size_t>(column);
    if (index < statement->column_types.size()
        && statement->column_types[index] != DUCKDB_TYPE_INVALID) {
        return statement->column_types[index];
    }
    // A statement with no prepared result schema should not normally reach a
    // column accessor, but retain the old defensive fallback for unusual
    // DuckDB table functions.
    if (statement->chunk == nullptr) return DUCKDB_TYPE_INVALID;
    const auto count = duckdb_data_chunk_get_column_count(statement->chunk);
    if (static_cast<idx_t>(column) >= count) return DUCKDB_TYPE_INVALID;
    auto vector = duckdb_data_chunk_get_vector(statement->chunk, column);
    auto logical = duckdb_vector_get_column_type(vector);
    const auto type = duckdb_get_type_id(logical);
    duckdb_destroy_logical_type(&logical);
    return type;
}

template <typename T>
T NumericValue(sqlite3_stmt* statement, int column) {
    if (IsNull(statement, column)) return T{};
    auto vector = duckdb_data_chunk_get_vector(statement->chunk, column);
    const auto type = ColumnDuckType(statement, column);
    const auto* data = static_cast<const std::byte*>(duckdb_vector_get_data(vector));
    const auto row = statement->current_row;
    switch (type) {
        case DUCKDB_TYPE_TINYINT: return static_cast<T>(reinterpret_cast<const int8_t*>(data)[row]);
        case DUCKDB_TYPE_SMALLINT: return static_cast<T>(reinterpret_cast<const int16_t*>(data)[row]);
        case DUCKDB_TYPE_INTEGER: return static_cast<T>(reinterpret_cast<const int32_t*>(data)[row]);
        case DUCKDB_TYPE_BIGINT: return static_cast<T>(reinterpret_cast<const int64_t*>(data)[row]);
        case DUCKDB_TYPE_UTINYINT: return static_cast<T>(reinterpret_cast<const uint8_t*>(data)[row]);
        case DUCKDB_TYPE_USMALLINT: return static_cast<T>(reinterpret_cast<const uint16_t*>(data)[row]);
        case DUCKDB_TYPE_UINTEGER: return static_cast<T>(reinterpret_cast<const uint32_t*>(data)[row]);
        case DUCKDB_TYPE_UBIGINT: return static_cast<T>(reinterpret_cast<const uint64_t*>(data)[row]);
        case DUCKDB_TYPE_FLOAT: return static_cast<T>(reinterpret_cast<const float*>(data)[row]);
        case DUCKDB_TYPE_DOUBLE: return static_cast<T>(reinterpret_cast<const double*>(data)[row]);
        case DUCKDB_TYPE_HUGEINT:
            return static_cast<T>(duckdb_hugeint_to_double(
                reinterpret_cast<const duckdb_hugeint*>(data)[row]));
        case DUCKDB_TYPE_UHUGEINT:
            return static_cast<T>(duckdb_uhugeint_to_double(
                reinterpret_cast<const duckdb_uhugeint*>(data)[row]));
        case DUCKDB_TYPE_DECIMAL:
            // DECIMAL vectors use their physical HUGEINT representation;
            // the scale is part of the logical type, not repeated per row.
            return static_cast<T>(duckdb_hugeint_to_double(
                reinterpret_cast<const duckdb_hugeint*>(data)[row]));
        default: return T{};
    }
}

const char* StringData(sqlite3_stmt* statement, int column, int* length) {
    if (length != nullptr) *length = 0;
    if (statement == nullptr || statement->chunk == nullptr || column < 0) {
        return nullptr;
    }
    const auto count = duckdb_data_chunk_get_column_count(statement->chunk);
    if (static_cast<idx_t>(column) >= count) return nullptr;
    if (statement->text_cells.size() < count) {
        statement->text_cells.resize(static_cast<std::size_t>(count));
    }
    auto& cell = statement->text_cells[static_cast<std::size_t>(column)];
    if (cell.valid && cell.row == statement->current_row) {
        if (length != nullptr) *length = cell.length;
        return cell.null ? nullptr : cell.value.c_str();
    }
    cell.row = statement->current_row;
    cell.length = 0;
    cell.valid = true;
    cell.null = IsNull(statement, column);
    if (cell.null) return nullptr;
    auto vector = duckdb_data_chunk_get_vector(statement->chunk, column);
    const auto type = ColumnDuckType(statement, column);
    const auto row = statement->current_row;
    if (type == DUCKDB_TYPE_VARCHAR) {
        const auto* values = static_cast<const duckdb_string_t*>(
            duckdb_vector_get_data(vector));
        const auto& value = values[row];
        const auto size = value.value.pointer.length;
        const char* data = size <= 12U
            ? value.value.inlined.inlined
            : value.value.pointer.ptr;
        cell.value.assign(data, size);
        cell.length = static_cast<int>(std::min<std::size_t>(
            size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        if (length != nullptr) *length = cell.length;
        return cell.value.c_str();
    }
    if (type == DUCKDB_TYPE_BLOB) {
        // Variable-length BLOB vectors use DuckDB's internal string_t layout
        // (the public duckdb_blob struct is only used by value/result APIs).
        // Treating this vector as duckdb_blob* reads the first bytes of the
        // payload as a pointer and can crash when the caller copies a bitmap.
        auto* values = static_cast<duckdb_string_t*>(
            duckdb_vector_get_data(vector));
        auto value = values[row];
        const auto size = std::min<std::size_t>(
            duckdb_string_t_length(value), std::numeric_limits<int>::max());
        // duckdb_string_t_data() points into the local string_t for an
        // inline value (<= 12 bytes).  Copy both inline and out-of-line BLOBs
        // into statement-owned storage before returning; otherwise a small
        // binary value would leave sqlite3_column_blob() with a dangling
        // pointer after this function returns.
        const char* data = duckdb_string_t_data(&value);
        cell.value.assign(data, size);
        cell.length = static_cast<int>(size);
        if (length != nullptr) *length = cell.length;
        return cell.value.c_str();
    }
    // SQLite exposes only text/blob through these accessors.  Mark an
    // unexpected DuckDB scalar as null in the cache so a repeated accessor
    // cannot accidentally return the previous cell's scratch buffer.
    cell.null = true;
    return nullptr;
}

// BLOB callers (notably the bitmap loader) do not need a NUL-terminated
// string.  DuckDB keeps variable-length values alive for the lifetime of the
// current result chunk, which is exactly the validity window promised by
// sqlite3_column_blob().  Return that chunk-owned pointer directly instead of
// copying up to 512 bytes into text_scratch.  Inline string_t values must be
// addressed in-place; copying the union to a local variable would make the
// returned pointer dangle immediately.
const void* BlobData(sqlite3_stmt* statement, int column, int* length) {
    if (length != nullptr) *length = 0;
    if (statement == nullptr || statement->chunk == nullptr || column < 0
        || IsNull(statement, column)
        || ColumnDuckType(statement, column) != DUCKDB_TYPE_BLOB) {
        return nullptr;
    }
    auto vector = duckdb_data_chunk_get_vector(statement->chunk, column);
    const auto* values = static_cast<const duckdb_string_t*>(
        duckdb_vector_get_data(vector));
    const auto& value = values[statement->current_row];
    const auto size = std::min<std::size_t>(
        duckdb_string_t_length(value),
        static_cast<std::size_t>(std::numeric_limits<int>::max()));
    if (length != nullptr) *length = static_cast<int>(size);
    return size <= 12U
        ? static_cast<const void*>(value.value.inlined.inlined)
        : static_cast<const void*>(value.value.pointer.ptr);
}

}  // namespace

namespace {

int OpenDuckDb(const char* filename, sqlite3** database, const int flags) {
    if (database == nullptr) return SQLITE_MISUSE;
    auto* result = new sqlite3();
    std::string utf8Path;
    if (!DatabasePathUtf8(filename, utf8Path)) {
        result->error = "Database path is neither valid UTF-8 nor lossless Windows ANSI";
        *database = result;
        return SQLITE_CANTOPEN;
    }
    const char* path = utf8Path.c_str();
    duckdb_state status = DuckDBError;
    char* openError = nullptr;
    const std::string key = std::strcmp(path, ":memory:") == 0
        ? std::string{}
        : DatabaseEncryptionKey();
    const bool readOnly = (flags & SQLITE_OPEN_READONLY) != 0;
#ifdef GNSS_DOWNLOADER_REQUIRE_ENCRYPTED_DB
    if (std::strcmp(path, ":memory:") != 0 && key.empty()) {
        result->error = "Encrypted release requires a compiled database key";
        *database = result;
        return SQLITE_CANTOPEN;
    }
#endif

    // DuckDB's native encrypted storage is selected with an ATTACH option.
    // A connection opened directly on an encrypted file cannot receive that
    // option through the C API, so use a tiny in-memory catalog as the host
    // connection and make the encrypted file the active catalog.  We first
    // try a normal open for existing unencrypted files; this keeps a key set
    // by a launcher backward-compatible with mixed old/new bundles.  A new
    // file is attached directly so builders create it encrypted from byte 0.
    bool attachedEncrypted = false;
#ifndef GNSS_DOWNLOADER_REQUIRE_ENCRYPTED_DB
    if (!key.empty() && std::strcmp(path, ":memory:") != 0
        && ((flags & SQLITE_OPEN_CREATE) == 0
            || PathExistsUtf8(path))) {
        const char* accessMode = readOnly ? "READ_ONLY" : "READ_WRITE";
        duckdb_config config = nullptr;
        if (duckdb_create_config(&config) == DuckDBSuccess
            && duckdb_set_config(config, "access_mode", accessMode)
                == DuckDBSuccess) {
            status = duckdb_open_ext(path, &result->database, config,
                                     &openError);
        }
        duckdb_destroy_config(&config);
        // An encrypted file is expected to fail this direct open because no
        // key is passed to duckdb_open_ext().  Discard that diagnostic and
        // retry through the ATTACH path below.
        if (status == DuckDBSuccess && result->database != nullptr) {
            attachedEncrypted = false;
        } else {
            if (openError != nullptr) {
                duckdb_free(openError);
                openError = nullptr;
            }
            if (result->database != nullptr) {
                duckdb_close(&result->database);
            }
            status = DuckDBError;
        }
    }
#endif

    if (status != DuckDBSuccess && !key.empty()
        && std::strcmp(path, ":memory:") != 0) {
        // Open the host catalog in memory.  The attached catalog receives the
        // actual READ_ONLY/READ_WRITE mode, so the host cannot accidentally
        // create a plaintext database beside an encrypted one.
        status = duckdb_open(":memory:", &result->database);
        attachedEncrypted = status == DuckDBSuccess;
    }

    if (!attachedEncrypted && status != DuckDBSuccess
        && std::strcmp(path, ":memory:") == 0) {
        status = duckdb_open(path, &result->database);
    } else if (!attachedEncrypted && status != DuckDBSuccess) {
        // Preserve the SQLite open-mode contract.  Interactive readers pass
        // SQLITE_OPEN_READONLY and must not acquire DuckDB's writer lock;
        // builder/maintenance paths may explicitly request READWRITE/CREATE.
        // The old shim forced every connection to READ_ONLY, which made a
        // legacy writer fail with a misleading "read-only" error.
        const char* accessMode = (flags & SQLITE_OPEN_READONLY) != 0
            ? "READ_ONLY" : "READ_WRITE";
        duckdb_config config = nullptr;
        if (duckdb_create_config(&config) == DuckDBSuccess
            && duckdb_set_config(config, "access_mode", accessMode)
                == DuckDBSuccess) {
            status = duckdb_open_ext(path, &result->database, config,
                                     &openError);
        }
        duckdb_destroy_config(&config);
    }

    if (status == DuckDBSuccess
        && duckdb_connect(result->database, &result->connection)
            != DuckDBSuccess) {
        status = DuckDBError;
    }

    if (status == DuckDBSuccess && attachedEncrypted) {
        const std::string access = readOnly ? "READ_ONLY, " : "";
        const std::string attach =
            "ATTACH " + SqlString(path) +
            " AS \"__gnss_encrypted\" (" + access +
            "ENCRYPTION_KEY " + SqlString(key) + ");";
        if (!QuerySucceeded(result->connection, attach, result->error)
            || !QuerySucceeded(
                result->connection,
                "USE \"__gnss_encrypted\";",
                result->error)) {
            status = DuckDBError;
        }
    }

    if (status != DuckDBSuccess) {
        if (openError != nullptr) {
            result->error = openError;
            duckdb_free(openError);
        }
        if (result->error.empty()) {
            result->error = "Unable to open DuckDB database";
        }
        if (result->connection != nullptr) {
            duckdb_disconnect(&result->connection);
        }
        if (result->database != nullptr) duckdb_close(&result->database);
        // SQLite keeps a handle available after an open failure so callers
        // can retrieve the diagnostic with sqlite3_errmsg() and then close
        // it.  Preserve that useful contract for the DuckDB shim too.
        *database = result;
        return SQLITE_CANTOPEN;
    }
    if (openError != nullptr) duckdb_free(openError);
    *database = result;
    return SQLITE_OK;
}

}  // namespace

extern "C" {

int sqlite3_open(const char* filename, sqlite3** database) {
    // sqlite3_open() is equivalent to READWRITE|CREATE for file databases.
    return OpenDuckDb(filename, database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
}

int sqlite3_open_v2(const char* filename, sqlite3** database, int flags,
                    const char*) {
    return OpenDuckDb(filename, database, flags);
}

int sqlite3_open16(const void* filename, sqlite3** database) {
    const auto utf8 = WideToUtf8(static_cast<const wchar_t*>(filename));
    // The compact SQLite compatibility header has no open16(flags) variant.
    // For an encrypted release, opening read-only is the safe default: the
    // application is a reader, while the monthly maintenance builder writes
    // the next immutable bundle separately.  An explicit opt-in keeps
    // developer/build tools able to request the old read-write behavior.
    if (!DatabaseEncryptionKey().empty()
        && EnvironmentValue("GNSS_DOWNLOADER_DB_WRITE") != "1") {
        return OpenDuckDb(utf8.c_str(), database, SQLITE_OPEN_READONLY);
    }
    return sqlite3_open(utf8.c_str(), database);
}

int sqlite3_close(sqlite3* database) {
    if (database == nullptr) return SQLITE_OK;
    if (database->connection) duckdb_disconnect(&database->connection);
    if (database->database) duckdb_close(&database->database);
    delete database;
    return SQLITE_OK;
}

int sqlite3_busy_timeout(sqlite3*, int) { return SQLITE_OK; }

const char* sqlite3_errmsg(sqlite3* database) {
    return database == nullptr ? "DuckDB: null database" : database->error.c_str();
}

int sqlite3_exec(sqlite3* database, const char* sql,
                 int (*)(void*, int, char**, char**), void*, char** error_message) {
    if (database == nullptr || sql == nullptr) return SQLITE_MISUSE;
    duckdb_result result{};
    const auto status = duckdb_query(database->connection, sql, &result);
    if (status != DuckDBSuccess) {
        const auto* error = duckdb_result_error(&result);
        SetError(database, error == nullptr ? "DuckDB query failed" : error);
        if (error_message != nullptr && error != nullptr) {
            *error_message = static_cast<char*>(duckdb_malloc(strlen(error) + 1));
            strcpy_s(*error_message, strlen(error) + 1, error);
        }
        duckdb_destroy_result(&result);
        return SQLITE_ERROR;
    }
    duckdb_destroy_result(&result);
    return SQLITE_OK;
}

void sqlite3_free(void* memory) { duckdb_free(memory); }

int sqlite3_prepare_v2(sqlite3* database, const char* sql, int byte_count,
                       sqlite3_stmt** statement, const char**) {
    if (database == nullptr || sql == nullptr || statement == nullptr) return SQLITE_MISUSE;
    std::string query(sql, byte_count < 0 ? strlen(sql) : static_cast<std::size_t>(byte_count));
    auto* result = new sqlite3_stmt();
    result->owner = database;
    if (duckdb_prepare(database->connection, query.c_str(), &result->prepared) != DuckDBSuccess) {
        const auto* error = duckdb_prepare_error(result->prepared);
        SetError(database, error == nullptr ? "DuckDB prepare failed" : error);
        duckdb_destroy_prepare(&result->prepared);
        delete result;
        *statement = nullptr;
        return SQLITE_ERROR;
    }
    const auto column_count = duckdb_prepared_statement_column_count(result->prepared);
    result->column_types.reserve(static_cast<std::size_t>(column_count));
    for (idx_t column = 0; column < column_count; ++column) {
        result->column_types.push_back(
            duckdb_prepared_statement_column_type(result->prepared, column));
    }
    *statement = result;
    return SQLITE_OK;
}

int sqlite3_finalize(sqlite3_stmt* statement) {
    if (statement == nullptr) return SQLITE_OK;
    DestroyChunk(statement);
    DestroyResult(statement);
    if (statement->prepared) duckdb_destroy_prepare(&statement->prepared);
    delete statement;
    return SQLITE_OK;
}

int sqlite3_reset(sqlite3_stmt* statement) {
    if (statement == nullptr) return SQLITE_MISUSE;
    DestroyChunk(statement);
    DestroyResult(statement);
    statement->started = false;
    statement->finished = false;
    statement->current_row = 0;
    statement->row_ready = false;
    for (auto& cell : statement->text_cells) cell.valid = false;
    return SQLITE_OK;
}

int sqlite3_clear_bindings(sqlite3_stmt* statement) {
    if (statement == nullptr) return SQLITE_MISUSE;
    return duckdb_clear_bindings(statement->prepared) == DuckDBSuccess
        ? SQLITE_OK : SQLITE_ERROR;
}

int sqlite3_step(sqlite3_stmt* statement) {
    if (statement == nullptr) return SQLITE_MISUSE;
    // A sqlite column pointer is only valid until the next step/reset.  Also
    // invalidate the conversion cache here so a reused scratch string can
    // never be returned for a different row.
    for (auto& cell : statement->text_cells) cell.valid = false;
    if (!statement->started) {
        statement->started = true;
        if (duckdb_execute_prepared_streaming(statement->prepared, &statement->result)
            != DuckDBSuccess) {
            const auto* error = duckdb_result_error(&statement->result);
            SetError(statement->owner, error == nullptr ? "DuckDB execute failed" : error);
            return SQLITE_ERROR;
        }
        // Resolve the schema after binding/execution. At prepare time a
        // parameterized SELECT can report only one INVALID column, even
        // though its executed result has the full Site Log/bitmap schema.
        const auto count = duckdb_column_count(&statement->result);
        statement->column_types.clear();
        statement->column_types.reserve(static_cast<std::size_t>(count));
        for (idx_t column = 0; column < count; ++column) {
            statement->column_types.push_back(
                duckdb_column_type(&statement->result, column));
        }
        statement->text_cells.resize(static_cast<std::size_t>(count));
    }
    for (;;) {
        if (statement->chunk != nullptr) {
            const auto size = duckdb_data_chunk_get_size(statement->chunk);
            if (statement->row_ready) {
                ++statement->current_row;
            } else {
                statement->row_ready = true;
            }
            if (statement->current_row < size) {
                return SQLITE_ROW;
            }
            DestroyChunk(statement);
        }
        if (statement->finished) return SQLITE_DONE;
        statement->chunk = duckdb_stream_fetch_chunk(statement->result);
        statement->current_row = 0;
        statement->row_ready = false;
        if (statement->chunk == nullptr) {
            statement->finished = true;
            return SQLITE_DONE;
        }
    }
}

int sqlite3_bind_text(sqlite3_stmt* statement, int index, const char* value,
                      int byte_count, sqlite3_destructor_type) {
    if (statement == nullptr) return SQLITE_MISUSE;
    const auto length = byte_count < 0 ? (value == nullptr ? 0 : strlen(value))
                                      : static_cast<std::size_t>(byte_count);
    const auto status = value == nullptr
        ? duckdb_bind_null(statement->prepared, static_cast<idx_t>(index))
        : duckdb_bind_varchar_length(statement->prepared,
                                     static_cast<idx_t>(index), value, length);
    return status == DuckDBSuccess ? SQLITE_OK : SQLITE_ERROR;
}

int sqlite3_bind_blob(sqlite3_stmt* statement, int index, const void* value,
                      int byte_count, sqlite3_destructor_type) {
    if (statement == nullptr) return SQLITE_MISUSE;
    const auto status = value == nullptr
        ? duckdb_bind_null(statement->prepared, static_cast<idx_t>(index))
        : duckdb_bind_blob(statement->prepared, static_cast<idx_t>(index),
                           value, static_cast<std::size_t>(std::max(0, byte_count)));
    return status == DuckDBSuccess ? SQLITE_OK : SQLITE_ERROR;
}

int sqlite3_bind_int(sqlite3_stmt* statement, int index, int value) {
    return statement != nullptr && duckdb_bind_int32(
        statement->prepared, static_cast<idx_t>(index), value) == DuckDBSuccess
        ? SQLITE_OK : SQLITE_ERROR;
}

int sqlite3_bind_int64(sqlite3_stmt* statement, int index, sqlite3_int64 value) {
    return statement != nullptr && duckdb_bind_int64(
        statement->prepared, static_cast<idx_t>(index), value) == DuckDBSuccess
        ? SQLITE_OK : SQLITE_ERROR;
}

int sqlite3_bind_double(sqlite3_stmt* statement, int index, double value) {
    return statement != nullptr && duckdb_bind_double(
        statement->prepared, static_cast<idx_t>(index), value) == DuckDBSuccess
        ? SQLITE_OK : SQLITE_ERROR;
}

int sqlite3_bind_null(sqlite3_stmt* statement, int index) {
    return statement != nullptr && duckdb_bind_null(
        statement->prepared, static_cast<idx_t>(index)) == DuckDBSuccess
        ? SQLITE_OK : SQLITE_ERROR;
}

int sqlite3_column_count(sqlite3_stmt* statement) {
    return statement == nullptr ? 0 : static_cast<int>(statement->column_types.size());
}

int sqlite3_column_type(sqlite3_stmt* statement, int column) {
    if (IsNull(statement, column)) return SQLITE_NULL;
    switch (ColumnDuckType(statement, column)) {
        case DUCKDB_TYPE_FLOAT:
        case DUCKDB_TYPE_DOUBLE: return SQLITE_FLOAT;
        case DUCKDB_TYPE_VARCHAR: return SQLITE_TEXT;
        case DUCKDB_TYPE_BLOB: return SQLITE_BLOB;
        default: return SQLITE_INTEGER;
    }
}

const unsigned char* sqlite3_column_text(sqlite3_stmt* statement, int column) {
    int length = 0;
    return reinterpret_cast<const unsigned char*>(StringData(statement, column, &length));
}

const void* sqlite3_column_blob(sqlite3_stmt* statement, int column) {
    int length = 0;
    const auto type = ColumnDuckType(statement, column);
    return type == DUCKDB_TYPE_BLOB
        ? BlobData(statement, column, &length)
        : StringData(statement, column, &length);
}

int sqlite3_column_bytes(sqlite3_stmt* statement, int column) {
    int length = 0;
    if (ColumnDuckType(statement, column) == DUCKDB_TYPE_BLOB) {
        (void)BlobData(statement, column, &length);
    } else {
        StringData(statement, column, &length);
    }
    return length;
}

int sqlite3_column_int(sqlite3_stmt* statement, int column) {
    const auto value = NumericValue<int64_t>(statement, column);
    return value > std::numeric_limits<int>::max()
        ? std::numeric_limits<int>::max()
        : static_cast<int>(value);
}

sqlite3_int64 sqlite3_column_int64(sqlite3_stmt* statement, int column) {
    return NumericValue<int64_t>(statement, column);
}

double sqlite3_column_double(sqlite3_stmt* statement, int column) {
    return NumericValue<double>(statement, column);
}

int sqlite3_table_column_metadata(sqlite3*, const char*, const char*, const char*,
                                  const char** data_type, const char** collation,
                                  int* not_null, int* primary_key,
                                  int* auto_increment) {
    if (data_type != nullptr) *data_type = "";
    if (collation != nullptr) *collation = "BINARY";
    if (not_null != nullptr) *not_null = 0;
    if (primary_key != nullptr) *primary_key = 0;
    if (auto_increment != nullptr) *auto_increment = 0;
    return SQLITE_OK;
}

}  // extern "C"

