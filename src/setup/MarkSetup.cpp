#include "PackageReader.h"
#include "SetupResources.h"
#include "SetupFiles.h"
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <duckdb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using mark::setup::Progress;
namespace {
constexpr UINT UpdateMessage = WM_APP + 11;
constexpr UINT DoneMessage = WM_APP + 12;
constexpr int PathId = 101, BrowseId = 102, StartId = 103, CancelId = 104;
constexpr int ShortcutId = 105, LaunchId = 106, LicenseId = 107;

std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), size);
    return output;
}

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), size, nullptr, nullptr);
    return output;
}

std::string Quote(const std::string& value) {
    std::string output = "'";
    for (const char c : value) { if (c == '\'') output += '\''; output += c; }
    return output + "'";
}

std::string Ident(const std::string& value) {
    std::string output = "\"";
    for (const char c : value) { if (c == '"') output += '"'; output += c; }
    return output + "\"";
}

std::string PathText(const fs::path& path) { return Utf8(path.wstring()); }

std::string Json(const std::string& value) {
    std::string output = "\"";
    for (const unsigned char c : value) {
        if (c == '\\' || c == '"') { output += '\\'; output += static_cast<char>(c); }
        else if (c == '\n') output += "\\n";
        else if (c == '\r') output += "\\r";
        else if (c == '\t') output += "\\t";
        else if (c >= 32) output += static_cast<char>(c);
    }
    return output + "\"";
}

std::string CleanError(std::string error) {
    const auto key = mark::setup::DatabaseKey();
    if (!key.empty()) {
        std::size_t offset = 0;
        while ((offset = error.find(key, offset)) != std::string::npos) {
            error.replace(offset, key.size(), "[redacted]");
            offset += 10;
        }
    }
    if (error.size() > 1600) error.resize(1600);
    return error;
}

void CheckCancel(const std::atomic_bool& cancelled) {
    if (cancelled) throw std::runtime_error("Setup cancelled.");
}

struct Options {
    fs::path executable, destination, payload, report;
    bool quiet = false, verify_only = false, space_only = false, shortcut = true, launch = true;
    std::uint64_t requiredSpace = 0, estimatedPeak = 0, spaceMargin = 0;
    unsigned preparationWorkers = 1;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};

Options ParseOptions() {
    Options options;
    std::wstring module(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (count == 0 || count >= module.size()) throw std::runtime_error("Unable to locate the installer.");
    module.resize(count);
    options.executable = module;
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) throw std::runtime_error("Unable to read setup options.");
    try {
        for (int i = 1; i < argc; ++i) {
            const std::wstring argument = argv[i];
            if (argument == L"--quiet") options.quiet = true;
            else if (argument == L"--no-launch") options.launch = false;
            else if (argument == L"--no-shortcut") options.shortcut = false;
            else if (argument == L"--verify-only") options.verify_only = true;
            else if (argument == L"--check-space") options.space_only = true;
            else if ((argument == L"--destination" || argument == L"--payload" || argument == L"--report") && i + 1 < argc) {
                const fs::path value = argv[++i];
                if (argument == L"--destination") options.destination = value;
                else if (argument == L"--payload") options.payload = value;
                else options.report = value;
            } else throw std::runtime_error("Unknown or incomplete setup option.");
        }
    } catch (...) { LocalFree(argv); throw; }
    LocalFree(argv);
    if (options.quiet) options.launch = options.shortcut = false;
    if (options.destination.empty()) {
        options.destination = options.executable.parent_path() / L"MARK";
        for (int i = 1; fs::exists(options.destination) && !fs::is_empty(options.destination); ++i)
            options.destination = options.executable.parent_path() / (L"MARK-v1.0.0" + (i == 1 ? L"" : L"-" + std::to_wstring(i)));
    }
    options.destination = fs::absolute(options.destination).lexically_normal();
    return options;
}

void WriteReport(const Options& options, const std::string& status, int percent, const std::wstring& message) {
    if (options.report.empty()) return;
    std::ofstream output(options.report, std::ios::binary | std::ios::trunc);
    if (!output) return;
    output << "{\"status\":" << Json(status) << ",\"percent\":" << percent
           << ",\"elapsed_seconds\":" << std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::steady_clock::now() - options.started).count()
           << ",\"message\":" << Json(Utf8(message)) << ",\"destination\":" << Json(PathText(options.destination))
           << ",\"required_space_bytes\":" << options.requiredSpace << ",\"estimated_peak_bytes\":" << options.estimatedPeak
           << ",\"space_margin_bytes\":" << options.spaceMargin << ",\"preparation_workers\":" << options.preparationWorkers << "}\n";
}

std::uint64_t SetupToolsBytes() {
    std::uint64_t total = 0;
    for (const int id : {IDR_SETUP_7Z, IDR_SETUP_7Z_DLL, IDR_SETUP_DUCKDB}) {
        const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!resource) throw std::runtime_error("A required setup component is missing.");
        total = mark::setup::AddBytes(total, SizeofResource(nullptr, resource));
    }
    return total;
}

std::wstring SpaceText(std::uint64_t bytes) {
    const auto tenths = bytes / 100'000'000 + (bytes % 100'000'000 != 0);
    return std::to_wstring(tenths / 10) + L"." + std::to_wstring(tenths % 10) + L" GB";
}

mark::setup::Payload CheckInstallSpace(Options& options) {
    const auto payload = mark::setup::ReadPayload(options.payload.empty() ? options.executable : options.payload, !options.payload.empty());
    auto volume = options.destination.parent_path();
    while (!fs::exists(volume) && volume.has_parent_path() && volume != volume.parent_path()) volume = volume.parent_path();
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(volume.c_str(), &available, nullptr, nullptr))
        throw std::runtime_error("Unable to check free space at the selected location.");
    MEMORYSTATUSEX memory{sizeof(memory)};
    const bool parallelAllowed = GlobalMemoryStatusEx(&memory) && memory.ullAvailPhys >= 4ULL * 1024 * 1024 * 1024;
    const auto tools = SetupToolsBytes();
    options.preparationWorkers = mark::setup::ChoosePreparationWorkers(payload.space, payload.length, tools, available.QuadPart, parallelAllowed);
    const auto budget = mark::setup::CalculateSpace(payload.space, payload.length, tools, options.preparationWorkers);
    options.requiredSpace = budget.required; options.estimatedPeak = budget.peak; options.spaceMargin = budget.margin;
    if (available.QuadPart < budget.required)
        throw std::runtime_error("Setup needs " + Utf8(SpaceText(budget.required))
            + " of free space at the selected location, including temporary preparation files. Choose another location or free some space.");
    return payload;
}

std::wstring WindowsError(DWORD code) {
    wchar_t message[1024]{};
    const auto size = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), message, 1024, nullptr);
    return size ? std::wstring(message, size) : L"Windows error " + std::to_wstring(code);
}

std::string FailureText(const std::exception& error) {
    if (const auto* fileError = dynamic_cast<const fs::filesystem_error*>(&error)) {
        const auto code = static_cast<DWORD>(fileError->code().value());
        std::string text = "File operation failed (Windows error " + std::to_string(code)
            + "): " + Utf8(WindowsError(code));
        if (!fileError->path1().empty()) text += " Location: " + PathText(fileError->path1());
        if (!fileError->path2().empty()) text += " Destination: " + PathText(fileError->path2());
        return CleanError(text);
    }
    return CleanError(error.what());
}

void SaveResource(int identifier, const fs::path& output) {
    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(identifier), RT_RCDATA);
    if (!resource) throw std::runtime_error("A required setup component is missing.");
    const auto loaded = LoadResource(nullptr, resource);
    const auto size = SizeofResource(nullptr, resource);
    const auto data = static_cast<const char*>(LockResource(loaded));
    if (!data || size == 0) throw std::runtime_error("A required setup component is invalid.");
    std::ofstream stream(output, std::ios::binary);
    stream.write(data, size);
    if (!stream) throw std::runtime_error("Unable to write setup components to the selected location.");
}

class DuckDb {
    HMODULE module_ = nullptr;
    template<class T> T Function(const char* name) {
        const auto address = GetProcAddress(module_, name);
        if (!address) throw std::runtime_error("The bundled database library is incompatible.");
        return reinterpret_cast<T>(address);
    }
public:
#define DB_FUNCTION(name) decltype(&duckdb_##name) name = nullptr;
    DB_FUNCTION(create_config) DB_FUNCTION(set_config) DB_FUNCTION(destroy_config)
    DB_FUNCTION(open_ext) DB_FUNCTION(close) DB_FUNCTION(connect) DB_FUNCTION(disconnect)
    DB_FUNCTION(query) DB_FUNCTION(destroy_result) DB_FUNCTION(result_error)
    DB_FUNCTION(row_count) DB_FUNCTION(column_count) DB_FUNCTION(value_varchar) DB_FUNCTION(free)
#undef DB_FUNCTION
    explicit DuckDb(const fs::path& library) {
        module_ = LoadLibraryExW(library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) throw std::runtime_error("Unable to load the bundled database library.");
        try {
#define LOAD_DB(name) name = Function<decltype(name)>("duckdb_" #name);
            LOAD_DB(create_config) LOAD_DB(set_config) LOAD_DB(destroy_config)
            LOAD_DB(open_ext) LOAD_DB(close) LOAD_DB(connect) LOAD_DB(disconnect)
            LOAD_DB(query) LOAD_DB(destroy_result) LOAD_DB(result_error)
            LOAD_DB(row_count) LOAD_DB(column_count) LOAD_DB(value_varchar) LOAD_DB(free)
#undef LOAD_DB
        } catch (...) { FreeLibrary(module_); throw; }
    }
    ~DuckDb() { if (module_) FreeLibrary(module_); }
    DuckDb(const DuckDb&) = delete;
    DuckDb& operator=(const DuckDb&) = delete;
};

class Connection {
    DuckDb& api_;
    duckdb_database database_ = nullptr;
    duckdb_connection connection_ = nullptr;
public:
    explicit Connection(DuckDb& api) : api_(api) {
        duckdb_config config = nullptr;
        char* error = nullptr;
        if (api_.create_config(&config) != DuckDBSuccess) throw std::runtime_error("Unable to initialize the database library.");
        api_.set_config(config, "threads", "2");
        api_.set_config(config, "memory_limit", "768MB");
        const auto state = api_.open_ext(nullptr, &database_, config, &error);
        api_.destroy_config(&config);
        if (error) api_.free(error);
        if (state != DuckDBSuccess || api_.connect(database_, &connection_) != DuckDBSuccess) {
            if (database_) api_.close(&database_);
            throw std::runtime_error("Unable to initialize knowledge-base preparation.");
        }
    }
    ~Connection() { if (connection_) api_.disconnect(&connection_); if (database_) api_.close(&database_); }
    std::vector<std::vector<std::string>> Query(const std::string& sql) {
        duckdb_result result{};
        if (api_.query(connection_, sql.c_str(), &result) != DuckDBSuccess) {
            const auto raw = api_.result_error(&result);
            const auto error = CleanError(raw ? raw : "Database operation failed.");
            api_.destroy_result(&result);
            throw std::runtime_error("Knowledge-base preparation failed: " + error);
        }
        std::vector<std::vector<std::string>> rows;
        const auto count = api_.row_count(&result), columns = api_.column_count(&result);
        for (idx_t row = 0; row < count; ++row) {
            auto& values = rows.emplace_back();
            for (idx_t column = 0; column < columns; ++column) {
                char* text = api_.value_varchar(&result, column, row);
                values.emplace_back(text ? text : "");
                if (text) api_.free(text);
            }
        }
        api_.destroy_result(&result);
        return rows;
    }
};

void PrepareDatabase(DuckDb& api, const fs::path& source, const std::atomic_bool& cancelled) {
    CheckCancel(cancelled);
    const fs::path target = source.wstring() + L".building";
    if (fs::exists(target)) throw std::runtime_error("An incomplete preparation file already exists.");
    {
        Connection connection(api);
        connection.Query("SET preserve_insertion_order=false");
        connection.Query("ATTACH " + Quote(PathText(source)) + " AS plain (READ_ONLY)");
        connection.Query("ATTACH " + Quote(PathText(target)) + " AS secure (ENCRYPTION_KEY "
                         + Quote(mark::setup::DatabaseKey()) + ", ENCRYPTION_CIPHER 'GCM')");
        connection.Query("COPY FROM DATABASE plain TO secure");
        connection.Query("CHECKPOINT secure");
        const auto tables = connection.Query("SELECT schema_name,table_name FROM duckdb_tables() WHERE database_name='plain' ORDER BY 1,2");
        if (tables != connection.Query("SELECT schema_name,table_name FROM duckdb_tables() WHERE database_name='secure' ORDER BY 1,2"))
            throw std::runtime_error("Knowledge-base table verification failed.");
        for (const auto& table : tables) {
            CheckCancel(cancelled);
            const auto columns = connection.Query("SELECT column_name,data_type FROM duckdb_columns() WHERE database_name='plain' AND schema_name="
                + Quote(table[0]) + " AND table_name=" + Quote(table[1]) + " ORDER BY column_index");
            if (columns != connection.Query("SELECT column_name,data_type FROM duckdb_columns() WHERE database_name='secure' AND schema_name="
                + Quote(table[0]) + " AND table_name=" + Quote(table[1]) + " ORDER BY column_index"))
                throw std::runtime_error("Knowledge-base column verification failed.");
            std::string names;
            for (const auto& column : columns) { if (!names.empty()) names += ','; names += Ident(column[0]); }
            const auto expression = "hash(" + names + ")";
            const auto select = "SELECT CAST(count(*) AS VARCHAR),CAST(bit_xor(" + expression + ") AS VARCHAR),CAST(sum(" + expression + ") AS VARCHAR) FROM ";
            const auto suffix = Ident(table[0]) + "." + Ident(table[1]);
            if (connection.Query(select + "plain." + suffix) != connection.Query(select + "secure." + suffix))
                throw std::runtime_error("Knowledge-base content verification failed.");
        }
        for (const auto& specification : std::vector<std::pair<std::string, std::string>>{{"duckdb_views()", "view_name"}, {"duckdb_indexes()", "index_name"}}) {
            const auto predicate = specification.first == "duckdb_views()" ? " AND NOT internal" : "";
            const auto prefix = "SELECT schema_name," + specification.second + " FROM " + specification.first + " WHERE database_name=";
            if (connection.Query(prefix + "'plain'" + predicate + " ORDER BY 1,2") != connection.Query(prefix + "'secure'" + predicate + " ORDER BY 1,2"))
                throw std::runtime_error("Knowledge-base catalog verification failed.");
        }
    }
    fs::remove(source);
    fs::rename(target, source);
}

void Extract(const fs::path& executable, const fs::path& archive, const fs::path& output,
             const std::atomic_bool& cancelled) {
    std::wstring command = L"\"" + executable.wstring() + L"\" x \"" + archive.wstring() + L"\" -o\"" + output.wstring() + L"\" -y -bd";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, executable.parent_path().c_str(), &startup, &process))
        throw std::runtime_error("Unable to start the bundled extraction tool.");
    CloseHandle(process.hThread);
    while (WaitForSingleObject(process.hProcess, 100) == WAIT_TIMEOUT) {
        if (cancelled) {
            TerminateProcess(process.hProcess, 2);
            WaitForSingleObject(process.hProcess, INFINITE);
            CloseHandle(process.hProcess);
            throw std::runtime_error("Setup cancelled.");
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (code != 0) throw std::runtime_error("Unable to unpack the installation files. Check disk space and download the installer again.");
}

void MakeShortcut(const fs::path& application) {
    PWSTR desktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop))) return;
    const fs::path link = fs::path(desktop) / L"MARK.lnk";
    CoTaskMemFree(desktop);
    if (fs::exists(link)) return;
    IShellLinkW* shortcut = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shortcut)))) {
        shortcut->SetPath(application.c_str());
        shortcut->SetWorkingDirectory(application.parent_path().c_str());
        shortcut->SetDescription(L"MARK GNSS Data Workspace");
        shortcut->SetIconLocation(application.c_str(), 0);
        IPersistFile* file = nullptr;
        if (SUCCEEDED(shortcut->QueryInterface(IID_PPV_ARGS(&file)))) { file->Save(link.c_str(), TRUE); file->Release(); }
        shortcut->Release();
    }
}

void RefreshIndexFingerprints(DuckDb& api, const fs::path& knowledge) {
    // File sizes and timestamps change during database conversion. Rebind the
    // verified summaries after every source has reached its final file form.
    for (const bool observation : {true, false}) {
        const auto index = knowledge / (observation ? L"observation_search.duckdb" : L"precise_search.duckdb");
        if (!fs::is_regular_file(index)) throw std::runtime_error("A required search index is missing.");
        Connection connection(api);
        connection.Query("ATTACH " + Quote(PathText(index)) + " AS summary (ENCRYPTION_KEY "
                         + Quote(mark::setup::DatabaseKey()) + ")");
        const auto rows = connection.Query(observation
            ? "SELECT CAST(network_mask AS VARCHAR),source_path FROM summary.source_state"
            : "SELECT CAST(category AS VARCHAR),path FROM summary.source_files");
        if (rows.empty()) throw std::runtime_error("A search index contains no source information.");
        for (const auto& row : rows) {
            auto name = fs::path(Wide(row[1])).filename();
            name.replace_extension(L".duckdb");
            auto source = knowledge / name;
            if (!fs::is_regular_file(source)) source = knowledge / L"upd" / name;
            if (!fs::is_regular_file(source) || source == index)
                throw std::runtime_error("A search-index source could not be located.");
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (!GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &attributes))
                throw std::runtime_error("Unable to verify a search-index source.");
            ULARGE_INTEGER ticks{};
            ticks.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
            ticks.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
            constexpr std::uint64_t UnixEpoch = 116444736000000000ULL;
            if (ticks.QuadPart < UnixEpoch) throw std::runtime_error("A knowledge-base timestamp is invalid.");
            const auto size = std::to_string(fs::file_size(source));
            const auto timestamp = std::to_string(observation ? ticks.QuadPart : (ticks.QuadPart - UnixEpoch) * 100ULL);
            const auto statement = observation
                ? "UPDATE summary.source_state SET file_size=" + size + ",file_mtime=" + timestamp
                    + ",wal_size=0,wal_mtime=0 WHERE network_mask=" + row[0] + " AND source_path=" + Quote(row[1])
                : "UPDATE summary.source_files SET size=" + size + ",mtime_ns=" + timestamp
                    + ",wal_size=0,wal_mtime_ns=0 WHERE category=" + row[0] + " AND path=" + Quote(row[1]);
            connection.Query(statement);
        }
        connection.Query("CHECKPOINT summary");
    }
}

fs::path Install(Options& options, const std::atomic_bool& cancelled, const Progress& notify) {
    const auto destination = fs::absolute(options.destination).lexically_normal();
    if (destination == destination.root_path()) throw std::runtime_error("Choose a folder for MARK, not the root of a drive.");
    if (fs::exists(destination) && (!fs::is_directory(destination) || !fs::is_empty(destination)))
        throw std::runtime_error("The selected folder contains files. Choose an empty folder for MARK.");
    const auto payload = CheckInstallSpace(options);
    fs::create_directories(destination.parent_path());
    GUID identifier{};
    if (FAILED(CoCreateGuid(&identifier))) throw std::runtime_error("Unable to create a preparation folder.");
    wchar_t identifier_text[64]{};
    StringFromGUID2(identifier, identifier_text, 64);
    const fs::path work = destination.parent_path() / (L".MARK-setup-" + std::wstring(identifier_text));
    if (!fs::create_directory(work)) throw std::runtime_error("Unable to create a preparation folder.");
    const fs::path tools = work / L"tools", archive = work / L"payload.7z", unpacked = work / L"files";
    bool keepPrepared = false;
    const auto cleanup = [&] {
        if (work.parent_path() == destination.parent_path() && work.filename().wstring().starts_with(L".MARK-setup-")) {
            std::error_code ignored;
            fs::remove_all(work, ignored);
        }
    };
    try {
        fs::create_directory(tools);
        SaveResource(IDR_SETUP_7Z, tools / L"7z.exe");
        SaveResource(IDR_SETUP_7Z_DLL, tools / L"7z.dll");
        SaveResource(IDR_SETUP_DUCKDB, tools / L"duckdb.dll");
        const auto expected_count = mark::setup::DecodePayload(payload, archive, cancelled,
            [&](int percent, const std::wstring& text) { notify(percent * 15 / 100, text); });
        CheckCancel(cancelled);
        notify(16, L"Unpacking MARK and its knowledge bases");
        Extract(tools / L"7z.exe", archive, unpacked, cancelled);
        const fs::path root = unpacked / L"MARK", knowledge = root / L"knowledge-base";
        if (!fs::is_regular_file(root / L"software/MARK.exe") || !fs::is_directory(knowledge))
            throw std::runtime_error("The installation package has an invalid directory layout.");
        std::vector<fs::path> databases;
        std::uint64_t bytes = 0;
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            const auto attributes = GetFileAttributesW(entry.path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("The installation package contains an unsupported file link.");
            if (entry.is_regular_file() && entry.path().extension() == L".duckdb") {
                const auto relative = entry.path().lexically_relative(knowledge);
                if (relative.empty() || *relative.begin() == L"..") throw std::runtime_error("A knowledge base is outside its expected folder.");
                databases.push_back(entry.path());
                bytes += entry.file_size();
            }
        }
        if (databases.size() != expected_count) throw std::runtime_error("The package does not contain all required knowledge bases.");
        std::sort(databases.begin(), databases.end(), [](const auto& first, const auto& second) { return fs::file_size(first) > fs::file_size(second); });
        fs::remove(archive);
        std::uint64_t completed_bytes = 0;
        {
            DuckDb api(tools / L"duckdb.dll");
            std::size_t completed = 0;
            std::atomic_size_t next = 0;
            std::atomic_bool failed = false;
            std::exception_ptr failure;
            std::mutex progress_mutex;
            const std::size_t parallelism = options.preparationWorkers;
            notify(25, L"Preparing encrypted knowledge bases (0/" + std::to_wstring(databases.size()) + L")");
            const auto prepare = [&] {
                try {
                    while (!failed && !cancelled) {
                        const auto index = next.fetch_add(1);
                        if (index >= databases.size()) return;
                        const auto& database = databases[index];
                        const auto size = fs::file_size(database);
                        PrepareDatabase(api, database, cancelled);
                        std::scoped_lock lock(progress_mutex);
                        completed_bytes += size;
                        ++completed;
                        notify(25 + static_cast<int>(completed_bytes * 68 / std::max<std::uint64_t>(bytes, 1)),
                            L"Preparing encrypted knowledge bases (" + std::to_wstring(completed)
                                + L"/" + std::to_wstring(databases.size()) + L")");
                    }
                } catch (...) {
                    failed = true;
                    std::scoped_lock lock(progress_mutex);
                    if (!failure) failure = std::current_exception();
                }
            };
            std::vector<std::jthread> workers;
            for (std::size_t i = 0; i < parallelism; ++i) workers.emplace_back(prepare);
            workers.clear();
            if (failure) std::rethrow_exception(failure);
            CheckCancel(cancelled);
            notify(93, L"Finalizing search indexes");
            RefreshIndexFingerprints(api, knowledge);
        }
        CheckCancel(cancelled);
        notify(94, L"Verifying prepared files");
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() == L".building" || entry.path().extension() == L".wal")
                throw std::runtime_error("A knowledge base was not finalized.");
            files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        {
            std::ofstream hashes(root / L"SHA256SUMS.txt", std::ios::binary);
            for (const auto& file : files) {
                CheckCancel(cancelled);
                hashes << mark::setup::FileDigest(file) << "  " << PathText(file.lexically_relative(root)) << '\n';
            }
            if (!hashes) throw std::runtime_error("Unable to write the installed-file verification list.");
        }
        {
            std::ofstream metadata(root / L"MARK.installation.json", std::ios::binary);
            metadata << "{\"application\":\"MARK\",\"version\":\"1.0.0\",\"data_cutoff\":\"2026-08-01\",\"databases\":"
                     << databases.size() << ",\"encrypted\":true,\"status\":\"ready\"}\n";
            if (!metadata) throw std::runtime_error("Unable to write the installation information.");
        }
        CheckCancel(cancelled);
        notify(99, L"Finishing installation");
        const auto moveError = mark::setup::PublishPreparedDirectory(root, destination, cancelled);
        if (moveError != ERROR_SUCCESS) {
            CheckCancel(cancelled);
            keepPrepared = true;
            throw std::runtime_error("Windows could not move the prepared MARK folder to the selected location (error "
                + std::to_string(moveError) + "): " + Utf8(WindowsError(moveError))
                + " Prepared files are kept at: " + PathText(root));
        }
        cleanup();
        if (options.shortcut) MakeShortcut(destination / L"software/MARK.exe");
        notify(100, L"MARK is ready");
        return destination / L"software/MARK.exe";
    } catch (...) { if (!keepPrepared) cleanup(); throw; }
}

struct Status { int percent; std::wstring text; };
struct Completion { bool success; std::wstring error; fs::path application; };
struct Window {
    Options options;
    HWND handle = nullptr, path = nullptr, browse = nullptr, start = nullptr, cancel = nullptr;
    HWND status = nullptr, progress = nullptr, timing = nullptr, shortcut = nullptr, launch = nullptr;
    HFONT font = nullptr, heading = nullptr;
    std::atomic_bool cancelled = false;
    std::thread worker;
    bool running = false, complete = false;
    int percent = 0;
    double estimatedTotal = 0;
    fs::path application;
    int scale = 96;
    int D(int value) const { return MulDiv(value, scale, 96); }
    HWND Control(const wchar_t* type, const std::wstring& text, DWORD style, int x, int y, int width, int height, int id = 0) {
        const auto child = CreateWindowExW(type == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, type, text.c_str(),
            WS_CHILD | WS_VISIBLE | style, D(x), D(y), D(width), D(height), handle, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return child;
    }
};

std::wstring DurationText(long long seconds) {
    seconds = std::max(0LL, seconds);
    return std::to_wstring(seconds / 60) + L"m "
        + std::to_wstring(seconds % 60) + L"s";
}

void UpdateTiming(Window& state) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - state.options.started).count();
    std::wstring text = L"Elapsed: " + DurationText(elapsed);
    if (state.running) {
        text += state.estimatedTotal > elapsed
            ? L"   |   Estimated time remaining: about "
                + DurationText(std::max(1LL, static_cast<long long>(state.estimatedTotal) - elapsed))
            : L"   |   Estimated time remaining: estimating...";
    }
    SetWindowTextW(state.timing, text.c_str());
}

std::wstring Text(HWND window) {
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    text.resize(wcslen(text.c_str()));
    return text;
}

void Browse(Window& state) {
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Choose a location for MARK");
    dialog->SetOkButtonLabel(L"Select folder");
    if (SUCCEEDED(dialog->Show(state.handle))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                fs::path selected(path);
                CoTaskMemFree(path);
                if (_wcsicmp(selected.filename().c_str(), L"MARK") != 0) selected /= L"MARK";
                SetWindowTextW(state.path, selected.c_str());
            }
            item->Release();
        }
    }
    dialog->Release();
}

void ShowLicense(HWND owner) {
    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_SETUP_LICENSE), RT_RCDATA);
    const auto data = resource ? LoadResource(nullptr, resource) : nullptr;
    const auto bytes = data ? static_cast<const char*>(LockResource(data)) : nullptr;
    const auto size = resource ? SizeofResource(nullptr, resource) : 0;
    if (bytes && size) {
        const std::string source(bytes, bytes + size);
        const auto heading = source.find("## English");
        const auto begin = heading == std::string::npos ? std::string::npos : source.find('\n', heading);
        if (begin == std::string::npos) {
            MessageBoxW(owner, L"See KNOWLEDGE_BASE_LICENSE.md in the installed MARK folder.",
                        L"MARK knowledge-base license", MB_OK | MB_ICONINFORMATION);
            return;
        }
        const auto text = Wide(source.substr(begin + 1));
        MessageBoxW(owner, text.c_str(), L"MARK knowledge-base license", MB_OK | MB_ICONINFORMATION);
    }
}

LRESULT CALLBACK Procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) {
    auto state = reinterpret_cast<Window*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        state->handle = handle;
        SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(handle, message, wparam, lparam);
    if (message == WM_CREATE) {
        state->font = CreateFontW(-state->D(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        state->heading = CreateFontW(-state->D(30), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        const auto title = state->Control(L"STATIC", L"MARK 1.0.0", 0, 94, 25, 500, 40);
        SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(state->heading), TRUE);
        state->Control(L"STATIC", L"Metadata-Guided Automation of Resource Workflows", 0, 94, 70, 565, 22);
        state->Control(L"STATIC", L"Choose a folder for MARK and its encrypted knowledge bases.", 0, 28, 128, 620, 24);
        state->Control(L"STATIC", L"Installation folder", 0, 28, 168, 570, 22);
        state->path = state->Control(L"EDIT", state->options.destination.wstring(), ES_AUTOHSCROLL | WS_TABSTOP, 28, 194, 500, 29, PathId);
        state->browse = state->Control(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 540, 194, 112, 29, BrowseId);
        std::wstring spaceDescription = L"Disk-space requirements are calculated from the installation package.";
        try {
            const auto package = mark::setup::ReadPayload(state->options.executable);
            const auto budget = mark::setup::CalculateSpace(package.space, package.length, SetupToolsBytes(), 1);
            spaceDescription = L"About " + SpaceText(package.space.installed) + L" installed; allow "
                + SpaceText(budget.required) + L" free during setup.";
        } catch (...) {}
        state->Control(L"STATIC", spaceDescription, 0, 28, 234, 620, 22);
        state->shortcut = state->Control(L"BUTTON", L"Create a desktop shortcut", BS_AUTOCHECKBOX | WS_TABSTOP, 28, 272, 290, 25, ShortcutId);
        state->launch = state->Control(L"BUTTON", L"Launch MARK when ready", BS_AUTOCHECKBOX | WS_TABSTOP, 340, 272, 310, 25, LaunchId);
        SendMessageW(state->shortcut, BM_SETCHECK, state->options.shortcut ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(state->launch, BM_SETCHECK, state->options.launch ? BST_CHECKED : BST_UNCHECKED, 0);
        state->status = state->Control(L"STATIC", L"Knowledge-base data through 2026-08-01", 0, 28, 315, 624, 38);
        state->progress = state->Control(PROGRESS_CLASSW, L"", PBS_SMOOTH, 28, 360, 624, 14);
        SendMessageW(state->progress, PBM_SETRANGE32, 0, 100);
        state->timing = state->Control(L"STATIC", L"", 0, 28, 385, 624, 24);
        state->Control(L"BUTTON", L"Knowledge-base license", BS_PUSHBUTTON | WS_TABSTOP, 28, 425, 190, 32, LicenseId);
        state->cancel = state->Control(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 398, 425, 100, 32, CancelId);
        state->start = state->Control(L"BUTTON", L"Prepare MARK", BS_DEFPUSHBUTTON | WS_TABSTOP, 510, 425, 142, 32, StartId);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(handle, &paint);
        const auto icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_SETUP_MARK), IMAGE_ICON, state->D(56), state->D(56), LR_DEFAULTCOLOR));
        if (icon) { DrawIconEx(dc, state->D(27), state->D(30), icon, state->D(56), state->D(56), 0, nullptr, DI_NORMAL); DestroyIcon(icon); }
        EndPaint(handle, &paint);
        return 0;
    }
    if (message == WM_COMMAND) {
        const int id = LOWORD(wparam);
        if (id == BrowseId && !state->running) Browse(*state);
        else if (id == LicenseId) ShowLicense(handle);
        else if (id == StartId && !state->running) {
            if (state->complete) {
                ShellExecuteW(handle, L"open", state->application.c_str(), nullptr, state->application.parent_path().c_str(), SW_SHOWNORMAL);
                return 0;
            }
            state->options.destination = Text(state->path);
            state->options.shortcut = SendMessageW(state->shortcut, BM_GETCHECK, 0, 0) == BST_CHECKED;
            state->options.launch = SendMessageW(state->launch, BM_GETCHECK, 0, 0) == BST_CHECKED;
            state->cancelled = false;
            state->running = true;
            state->percent = 0;
            state->estimatedTotal = 0;
            state->options.started = std::chrono::steady_clock::now();
            UpdateTiming(*state);
            SetTimer(handle, 1, 1000, nullptr);
            for (const auto control : {state->path, state->browse, state->start, state->shortcut, state->launch}) EnableWindow(control, FALSE);
            state->worker = std::thread([state] {
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                auto completion = std::make_unique<Completion>();
                try {
                    completion->application = Install(state->options, state->cancelled, [state](int percent, const std::wstring& text) {
                        auto status = std::make_unique<Status>(Status{percent, text});
                        PostMessageW(state->handle, UpdateMessage, 0, reinterpret_cast<LPARAM>(status.release()));
                        WriteReport(state->options, "preparing", percent, text);
                    });
                    completion->success = true;
                    WriteReport(state->options, "complete", 100, L"MARK is ready");
                } catch (const std::exception& error) {
                    completion->success = false;
                    completion->error = Wide(FailureText(error));
                    WriteReport(state->options, state->cancelled ? "cancelled" : "failed", 0, completion->error);
                }
                CoUninitialize();
                PostMessageW(state->handle, DoneMessage, 0, reinterpret_cast<LPARAM>(completion.release()));
            });
        } else if (id == CancelId) SendMessageW(handle, WM_CLOSE, 0, 0);
        return 0;
    }
    if (message == UpdateMessage) {
        std::unique_ptr<Status> status(reinterpret_cast<Status*>(lparam));
        SetWindowTextW(state->status, status->text.c_str());
        SendMessageW(state->progress, PBM_SETPOS, status->percent, 0);
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - state->options.started).count();
        if (status->percent > state->percent && status->percent >= 3 && elapsed >= 3) {
            const double estimate = elapsed * 100.0 / status->percent;
            state->estimatedTotal = state->estimatedTotal == 0 ? estimate
                : 0.6 * estimate + 0.4 * state->estimatedTotal;
        }
        state->percent = status->percent;
        UpdateTiming(*state);
        return 0;
    }
    if (message == WM_TIMER && wparam == 1) {
        UpdateTiming(*state);
        return 0;
    }
    if (message == DoneMessage) {
        std::unique_ptr<Completion> completion(reinterpret_cast<Completion*>(lparam));
        if (state->worker.joinable()) state->worker.join();
        state->running = false;
        KillTimer(handle, 1);
        UpdateTiming(*state);
        EnableWindow(state->cancel, TRUE);
        SetWindowTextW(state->cancel, L"Close");
        if (completion->success) {
            state->complete = true;
            state->application = completion->application;
            EnableWindow(state->start, TRUE);
            SetWindowTextW(state->start, L"Open MARK");
            SetWindowTextW(state->status, L"MARK is ready. Your installed knowledge bases remain encrypted.");
            if (state->options.launch) {
                const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(handle, L"open", state->application.c_str(), nullptr, state->application.parent_path().c_str(), SW_SHOWNORMAL));
                if (result > 32) DestroyWindow(handle);
            }
        } else {
            SetWindowTextW(state->status, state->cancelled ? L"Setup cancelled." : L"Preparation could not be completed.");
            if (!state->cancelled) MessageBoxW(handle, completion->error.c_str(), L"MARK Setup", MB_OK | MB_ICONERROR);
            for (const auto control : {state->path, state->browse, state->start, state->shortcut, state->launch}) EnableWindow(control, TRUE);
            SetWindowTextW(state->cancel, L"Cancel");
        }
        return 0;
    }
    if (message == WM_CLOSE) {
        if (state->running) {
            state->cancelled = true;
            SetWindowTextW(state->status, L"Cancelling after the current file has finished...");
            EnableWindow(state->cancel, FALSE);
        } else DestroyWindow(handle);
        return 0;
    }
    if (message == WM_DESTROY) {
        if (state->font) DeleteObject(state->font);
        if (state->heading) DeleteObject(state->heading);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(handle, message, wparam, lparam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Options options;
    try {
        options = ParseOptions();
        if (options.space_only) {
            CheckInstallSpace(options);
            WriteReport(options, "checked", 0, L"Disk-space check passed");
            CoUninitialize();
            return 0;
        }
        if (options.verify_only) {
            fs::create_directories(options.destination);
            std::atomic_bool cancelled = false;
            const auto payload = mark::setup::ReadPayload(options.payload.empty() ? options.executable : options.payload, !options.payload.empty());
            mark::setup::DecodePayload(payload, options.destination / L"payload.7z", cancelled,
                [&](int percent, const std::wstring& text) { WriteReport(options, "verifying", percent, text); });
            WriteReport(options, "complete", 100, L"Package verification passed");
            CoUninitialize();
            return 0;
        }
        if (options.quiet) {
            std::atomic_bool cancelled = false;
            const auto application = Install(options, cancelled,
                [&](int percent, const std::wstring& text) { WriteReport(options, "preparing", percent, text); });
            WriteReport(options, "complete", 100, L"MARK is ready");
            if (options.launch) ShellExecuteW(nullptr, L"open", application.c_str(), nullptr, application.parent_path().c_str(), SW_SHOWNORMAL);
            CoUninitialize();
            return 0;
        }
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        Window state;
        state.options = options;
        state.scale = static_cast<int>(GetDpiForSystem());
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = Procedure;
        window_class.hInstance = instance;
        window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SETUP_MARK));
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = L"MARKSetupWindow";
        RegisterClassExW(&window_class);
        RECT area{0, 0, state.D(680), state.D(482)};
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRectExForDpi(&area, style, FALSE, 0, static_cast<UINT>(state.scale));
        const int width = area.right - area.left, height = area.bottom - area.top;
        const auto handle = CreateWindowExW(0, window_class.lpszClassName, L"MARK Setup", style,
            (GetSystemMetrics(SM_CXSCREEN) - width) / 2, (GetSystemMetrics(SM_CYSCREEN) - height) / 2,
            width, height, nullptr, nullptr, instance, &state);
        if (!handle) throw std::runtime_error("Unable to open the setup window.");
        ShowWindow(handle, show);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(handle, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        if (state.worker.joinable()) state.worker.join();
    } catch (const std::exception& error) {
        const auto text = Wide(FailureText(error));
        WriteReport(options, "failed", 0, text);
        if (!options.quiet) MessageBoxW(nullptr, text.c_str(), L"MARK Setup", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
    CoUninitialize();
    return 0;
}
