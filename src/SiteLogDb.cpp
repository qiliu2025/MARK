#include "SiteLogDb.h"

#include "HttpClient.h"
#include "Models.h"

#include "sqlite3.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <unordered_map>
#include <unordered_set>

namespace {

std::string WideToUtf8(const std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return std::wstring(text.begin(), text.end());
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string Trim(const std::string_view input) {
    std::size_t first = 0;
    while (first < input.size()
        && std::isspace(static_cast<unsigned char>(input[first])) != 0) {
        ++first;
    }
    std::size_t last = input.size();
    while (last > first
        && std::isspace(static_cast<unsigned char>(input[last - 1])) != 0) {
        --last;
    }
    return std::string(input.substr(first, last - first));
}

// Site Log section headings use a variable number of spaces between the
// section number and its title (for example ``11.   On-Site``).  Collapse
// ASCII whitespace before matching headings so older and newer templates are
// handled by the same parser.
std::string CollapseWhitespace(std::string value) {
    std::string result;
    result.reserve(value.size());
    bool inWhitespace = false;
    for (const unsigned char character : value) {
        if (std::isspace(character) != 0) {
            if (!inWhitespace) result.push_back(' ');
            inWhitespace = true;
        } else {
            result.push_back(static_cast<char>(character));
            inWhitespace = false;
        }
    }
    return Trim(result);
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string UpperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    return value;
}

bool Contains(const std::string_view haystack, const std::string_view needle) {
    return LowerAscii(std::string(haystack)).find(
        LowerAscii(std::string(needle))) != std::string::npos;
}

bool IsPlaceholder(const std::string_view value) {
    const auto trimmed = Trim(value);
    if (trimmed.size() < 2 || trimmed.front() != '(' || trimmed.back() != ')') {
        return false;
    }
    // The standard template contains parenthesized examples such as
    // ``(GPS+GLO+GAL+BDS+QZSS+SBAS)`` that do not include a marker word like
    // CCYY/A20.  Treat a complete parenthesized value as a template so it
    // cannot overwrite the last real receiver/antenna entry.
    return true;
}

std::string ValueAfterColon(const std::string_view line) {
    const auto colon = line.find(':');
    return colon == std::string_view::npos
        ? std::string{}
        : Trim(line.substr(colon + 1));
}

std::string LabelBeforeColon(const std::string_view line) {
    const auto colon = line.find(':');
    return colon == std::string_view::npos
        ? LowerAscii(Trim(line))
        : LowerAscii(Trim(line.substr(0, colon)));
}

bool ParseDouble(const std::string_view input, double& value) {
    const std::string text = Trim(input);
    if (text.empty()) return false;
    char* end = nullptr;
    value = std::strtod(text.c_str(), &end);
    return end != text.c_str() && std::isfinite(value);
}

double CoordinateFromParts(
    const std::vector<double>& numbers,
    const bool latitude,
    const bool negative) {
    if (numbers.empty()) return std::numeric_limits<double>::quiet_NaN();
    double result = 0.0;
    if (numbers.size() >= 3) {
        result = std::abs(numbers[0])
            + std::abs(numbers[1]) / 60.0
            + std::abs(numbers[2]) / 3600.0;
    } else {
        result = std::abs(numbers[0]);
        // The old IGS form uses DDMMSS.ss / DDDMMSS.ss without separators,
        // for example -365037.54 or -1472957.26.  Do not mistake ordinary
        // decimal degrees for this representation.  The previous upper
        // bound of 1,000,000 rejected valid western-longitude values whose
        // degree component has three digits (e.g. 1472957.26), leaving them
        // as impossible longitudes and therefore off the map.
        const double limit = latitude ? 90.0 : 180.0;
        if (result > limit) {
            const double degrees = std::floor(result / 10000.0);
            const double minutes = std::floor(
                (result - degrees * 10000.0) / 100.0);
            const double seconds = result - degrees * 10000.0
                - minutes * 100.0;
            if (degrees <= limit && minutes >= 0.0 && minutes < 60.0
                && seconds >= 0.0 && seconds < 60.0) {
                result = degrees + minutes / 60.0 + seconds / 3600.0;
            }
        }
    }
    if (negative) result = -result;
    const double limit = latitude ? 90.0 : 180.0;
    return result <= limit ? result : std::numeric_limits<double>::quiet_NaN();
}

bool ParseCoordinate(
    const std::string_view input,
    const bool latitude,
    double& result) {
    std::string text = Trim(input);
    if (text.empty()) return false;
    bool negative = false;
    for (const char character : text) {
        if (character == 'S' || character == 's'
            || character == 'W' || character == 'w') {
            negative = true;
        }
    }
    if (!negative && text.find('-') != std::string::npos) negative = true;

    static const std::regex numberPattern(
        R"([+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+))",
        std::regex::optimize);
    std::vector<double> numbers;
    for (std::sregex_iterator it(text.begin(), text.end(), numberPattern),
         end;
         it != end;
         ++it) {
        double number = 0.0;
        if (ParseDouble(it->str(), number)) numbers.push_back(number);
    }
    if (numbers.empty()) return false;
    result = CoordinateFromParts(numbers, latitude, negative);
    return std::isfinite(result);
}

std::string FileNameFromUrl(const std::string_view url) {
    const auto query = url.find_first_of("?#");
    const auto clean = url.substr(0, query == std::string_view::npos
        ? url.size() : query);
    const auto slash = clean.find_last_of('/');
    return std::string(clean.substr(
        slash == std::string_view::npos ? 0 : slash + 1));
}

std::string RevisionDateFromFileName(const std::string_view fileName) {
    static const std::regex datePattern(R"(_([0-9]{8})\.(?:log|txt)$)",
        std::regex::icase | std::regex::optimize);
    std::cmatch match;
    const std::string copy(fileName);
    if (std::regex_search(copy.c_str(), match, datePattern)) {
        const std::string digits = match[1].str();
        return digits.substr(0, 4) + "-" + digits.substr(4, 2)
            + "-" + digits.substr(6, 2);
    }
    // A number of regional archives (notably the historical GA/APREF
    // archive) name a log as A4MMYY, e.g. ALIC0414.  Keep this conservative:
    // only an exactly four-character prefix followed by four digits is
    // accepted, and the first two digits must be a valid month.  If that is
    // not true, also accept the unambiguous YYMM ordering used by a few older
    // mirrors.
    static const std::regex compactPattern(
        R"(^[A-Za-z0-9]{4}([0-9]{4})\.(?:log|txt)$)",
        std::regex::icase | std::regex::optimize);
    std::smatch compactMatch;
    if (!std::regex_match(copy, compactMatch, compactPattern)) return {};
    const std::string digits = compactMatch[1].str();
    const int first = std::stoi(digits.substr(0, 2));
    const int second = std::stoi(digits.substr(2, 2));
    const auto expandYear = [](const int value) {
        // Use a rolling 30-year window so legacy logs such as ALIC0499 are
        // ordered as 1999 rather than 2099 while newly issued logs remain in
        // the 2000s.  The actual full Date Prepared field, when present, is
        // preferred later by ParseSiteLogText.
        return value <= 30 ? 2000 + value : 1900 + value;
    };
    if (first >= 1 && first <= 12) {
        return std::to_string(expandYear(second)) + "-"
            + digits.substr(0, 2) + "-01";
    }
    if (second >= 1 && second <= 12) {
        return std::to_string(expandYear(first)) + "-"
            + digits.substr(2, 2) + "-01";
    }
    return {};
}

void InferIdsFromFileName(
    const std::string_view fileName,
    std::string& stationId,
    std::string& fourCharId) {
    std::string base = FileNameFromUrl(fileName);
    const auto dot = LowerAscii(base).rfind(".log");
    if (dot != std::string::npos) base.resize(dot);
    const auto underscore = base.find('_');
    if (underscore != std::string::npos) base.resize(underscore);
    std::string alphanumeric;
    for (const unsigned char character : base) {
        if (std::isalnum(character) != 0) alphanumeric.push_back(
            static_cast<char>(character));
    }
    // Regional legacy logs often use a four-character station code followed
    // by a four-digit month/year (ALIC0414).  Do not manufacture a nine-
    // character ID from the date suffix; the content parser can supply the
    // authoritative ID when one exists.
    const bool compactFourCharDate = alphanumeric.size() == 8
        && std::all_of(
            alphanumeric.begin() + 4,
            alphanumeric.end(),
            [](const unsigned char character) {
                return std::isdigit(character) != 0;
            });
    if (alphanumeric.size() >= 9 && !compactFourCharDate) {
        stationId = UpperAscii(alphanumeric.substr(0, 9));
    }
    if (alphanumeric.size() >= 4) {
        fourCharId = UpperAscii(alphanumeric.substr(0, 4));
    }
}

std::string CurrentUtc() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_s(&utc, &time);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

struct DatabaseCloser {
    void operator()(sqlite3* database) const noexcept {
        if (database != nullptr) sqlite3_close(database);
    }
};
struct StatementCloser {
    void operator()(sqlite3_stmt* statement) const noexcept {
        if (statement != nullptr) sqlite3_finalize(statement);
    }
};
using DatabasePtr = std::unique_ptr<sqlite3, DatabaseCloser>;
using StatementPtr = std::unique_ptr<sqlite3_stmt, StatementCloser>;

std::wstring SqliteError(sqlite3* database, const std::wstring_view action) {
    const char* message = database == nullptr ? "unknown SQLite error"
        : sqlite3_errmsg(database);
    return std::wstring(action) + L"："
        + Utf8ToWide(message == nullptr ? "" : message);
}

bool OpenDatabase(
    const std::filesystem::path& path,
    DatabasePtr& database,
    std::wstring& error) {
    std::error_code directoryError;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, directoryError);
    }
    if (directoryError) {
        error = L"无法创建 Site Log 数据库目录：" + parent.wstring();
        return false;
    }
    sqlite3* raw = nullptr;
    const int status = sqlite3_open16(path.c_str(), &raw);
    database.reset(raw);
    if (status != SQLITE_OK || raw == nullptr) {
        error = SqliteError(raw, L"无法打开 Site Log 数据库");
        return false;
    }
    sqlite3_busy_timeout(raw, 10'000);
    return true;
}

// Interactive lookups must never open a Site Log database for writing.  The
// builders use OpenDatabase above because they create/upgrade their schema,
// while the GUI only needs a snapshot.  Opening read-only avoids taking a
// writer lock (and avoids journal work) when a large regional availability
// database is being updated by a background builder.
bool OpenReadOnlyDatabase(
    const std::filesystem::path& path,
    DatabasePtr& database,
    std::wstring& error) {
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(path, filesystemError)) {
        return false;
    }
    const std::string utf8Path = WideToUtf8(path.wstring());
    if (utf8Path.empty()) {
        error = L"无法编码 Site Log 数据库路径";
        return false;
    }
    sqlite3* raw = nullptr;
    const int status = sqlite3_open_v2(
        utf8Path.c_str(),
        &raw,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    database.reset(raw);
    if (status != SQLITE_OK || raw == nullptr) {
        error = SqliteError(raw, L"无法以只读方式打开 Site Log 数据库");
        return false;
    }
    sqlite3_busy_timeout(raw, 2'000);
    return true;
}

bool Execute(sqlite3* database, const char* sql, std::wstring& error) {
    char* message = nullptr;
    const int status = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    if (status == SQLITE_OK) return true;
    error = L"SQLite：" + Utf8ToWide(
        message == nullptr ? sqlite3_errmsg(database) : message);
    sqlite3_free(message);
    return false;
}

StatementPtr Prepare(
    sqlite3* database,
    const char* sql,
    std::wstring& error);

bool TableExists(sqlite3* database, const std::string_view name) {
    std::wstring ignored;
    auto statement = Prepare(
        database,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1;",
        ignored);
    if (!statement) return false;
    if (sqlite3_bind_text(
            statement.get(), 1, name.data(), static_cast<int>(name.size()),
            SQLITE_TRANSIENT) != SQLITE_OK) {
        return false;
    }
    return sqlite3_step(statement.get()) == SQLITE_ROW;
}

const char* ColumnText(sqlite3_stmt* statement, const int index) {
    const auto* text = sqlite3_column_text(statement, index);
    return text == nullptr ? "" : reinterpret_cast<const char*>(text);
}

StatementPtr Prepare(
    sqlite3* database,
    const char* sql,
    std::wstring& error) {
    sqlite3_stmt* raw = nullptr;
    if (database == nullptr || sql == nullptr
        || sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK) {
        error = SqliteError(database, L"无法准备 SQLite 查询");
        return {};
    }
    return StatementPtr(raw);
}

// Site Log sections 11/12 name the operating/responsible organizations.
// Contact names in those sections are people, not operating units.
void ApplyOrganizationRoles(SiteLogRecord& record, std::string_view text) {
    static const std::regex heading(R"(^\s*(11|12)\.\s*(?:On-Site|Responsible Agency))",std::regex::icase);
    static const std::regex otherHeading(R"(^\s*[0-9]+\.\s+)");
    std::array<std::string,2> agencies;
    int section=-1; bool collecting=false, found=false;
    std::istringstream stream{std::string(text)}; std::string line;
    while(std::getline(stream,line)) {
        std::smatch match;
        if(std::regex_search(line,match,heading)) {
            section=match[1].str()=="11"?0:1;collecting=false;found=true;continue;
        }
        if(std::regex_search(line,otherHeading)){section=-1;collecting=false;}
        if(section<0)continue;
        const auto colon=line.find(':');
        if(colon==std::string::npos){collecting=false;continue;}
        const auto label=LowerAscii(Trim(line.substr(0,colon)));
        const auto value=Trim(line.substr(colon+1));
        if(label=="agency") {
            collecting=true;
            agencies[section]=IsPlaceholder(value)?"":value;
        } else if(label.empty()&&collecting) {
            if(!value.empty()&&!IsPlaceholder(value)) {
                if(!agencies[section].empty())agencies[section]+=' ';
                agencies[section]+=value;
            }
        } else collecting=false;
    }
    if(!found)return; // Preserve explicitly named fields in regional formats.
    if(!agencies[1].empty())record.agency=agencies[1];
    else if(!agencies[0].empty())record.agency=agencies[0];
    record.operatorName=agencies[0];
}

SiteLogRecord ReadRecord(sqlite3_stmt* statement) {
    SiteLogRecord record;
    record.sourceUrl = ColumnText(statement, 0);
    record.stationId = ColumnText(statement, 1);
    record.fourCharId = ColumnText(statement, 2);
    record.domes = ColumnText(statement, 3);
    record.siteName = ColumnText(statement, 4);
    record.city = ColumnText(statement, 5);
    record.country = ColumnText(statement, 6);
    record.x = sqlite3_column_double(statement, 7);
    record.y = sqlite3_column_double(statement, 8);
    record.z = sqlite3_column_double(statement, 9);
    record.hasXyz = sqlite3_column_int(statement, 10) != 0;
    record.latitude = sqlite3_column_double(statement, 11);
    record.longitude = sqlite3_column_double(statement, 12);
    record.elevation = sqlite3_column_double(statement, 13);
    record.hasCoordinate = sqlite3_column_int(statement, 14) != 0;
    // A number of legacy/foreign Site Logs persist only the mandatory ECEF
    // position.  Derive LLH on read as well as during parsing so existing
    // databases become usable without downloading or rewriting every log.
    if ((!record.hasCoordinate
            || !std::isfinite(record.latitude)
            || !std::isfinite(record.longitude)
            || record.latitude < -90.0 || record.latitude > 90.0
            || record.longitude < -180.0 || record.longitude > 180.0)
        && record.hasXyz) {
        double latitude = 0.0;
        double longitude = 0.0;
        double elevation = 0.0;
        if (SiteLogEcefToGeodetic(
                record.x, record.y, record.z,
                latitude, longitude, elevation)) {
            record.latitude = latitude;
            record.longitude = longitude;
            if (!std::isfinite(record.elevation) || record.elevation == 0.0) {
                record.elevation = elevation;
            }
            record.hasCoordinate = true;
        }
    }
    record.receiverType = ColumnText(statement, 15);
    record.receiverSystems = ColumnText(statement, 16);
    record.receiverSerial = ColumnText(statement, 17);
    record.receiverFirmware = ColumnText(statement, 18);
    record.receiverDateInstalled = ColumnText(statement, 19);
    record.antennaType = ColumnText(statement, 20);
    record.antennaRadome = ColumnText(statement, 21);
    record.antennaSerial = ColumnText(statement, 22);
    record.antennaReferencePoint = ColumnText(statement, 23);
    record.antennaDateInstalled = ColumnText(statement, 24);
    record.agency = ColumnText(statement, 25);
    record.operatorName = ColumnText(statement, 26);
    record.networkMask = static_cast<std::uint64_t>(
        sqlite3_column_int64(statement, 27));
    record.sourceDirectory = ColumnText(statement, 28);
    record.fileName = ColumnText(statement, 29);
    record.revisionDate = ColumnText(statement, 30);
    record.fetchedAtUtc = ColumnText(statement, 31);
    record.rawText = ColumnText(statement, 32);
    ApplyOrganizationRoles(record,record.rawText);
    return record;
}

const char* SiteLogSelectColumns() {
    return "source_url,station_id,four_char_id,domes,site_name,city,country,"
        "x,y,z,has_xyz,latitude,longitude,elevation,has_coordinate,"
        "receiver_type,receiver_systems,receiver_serial,receiver_firmware,"
        "receiver_date_installed,antenna_type,antenna_radome,antenna_serial,"
        "antenna_reference_point,antenna_date_installed,agency,operator_name,"
        "network_mask,source_directory,file_name,revision_date,fetched_at_utc,"
        "raw_text";
}

// Interactive station enrichment never needs the original Site Log body.
// Keep the same 33-column layout expected by ReadRecord, but replace the
// potentially large raw_text BLOB/TEXT with an empty literal.  This avoids
// pulling tens or hundreds of megabytes into the GUI merely to display a
// receiver or DOMES value.
const char* SiteLogSummarySelectColumns() {
    return "source_url,station_id,four_char_id,domes,site_name,city,country,"
        "x,y,z,has_xyz,latitude,longitude,elevation,has_coordinate,"
        "receiver_type,receiver_systems,receiver_serial,receiver_firmware,"
        "receiver_date_installed,antenna_type,antenna_radome,antenna_serial,"
        "antenna_reference_point,antenna_date_installed,agency,operator_name,"
        "network_mask,source_directory,file_name,revision_date,fetched_at_utc,"
        "'' AS raw_text";
}

bool BindText(sqlite3_stmt* statement, const int index, const std::string& value) {
    return sqlite3_bind_text(statement, index, value.c_str(),
        static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

bool BindRecord(sqlite3_stmt* statement, const SiteLogRecord& record) {
    int index = 1;
    const auto bind = [&](const std::string& value) {
        return BindText(statement, index++, value);
    };
    if (!bind(record.sourceUrl) || !bind(record.stationId)
        || !bind(record.fourCharId) || !bind(record.domes)
        || !bind(record.siteName) || !bind(record.city)
        || !bind(record.country)) return false;
    if (sqlite3_bind_double(statement, index++, record.x) != SQLITE_OK
        || sqlite3_bind_double(statement, index++, record.y) != SQLITE_OK
        || sqlite3_bind_double(statement, index++, record.z) != SQLITE_OK
        || sqlite3_bind_int(statement, index++, record.hasXyz ? 1 : 0) != SQLITE_OK
        || sqlite3_bind_double(statement, index++, record.latitude) != SQLITE_OK
        || sqlite3_bind_double(statement, index++, record.longitude) != SQLITE_OK
        || sqlite3_bind_double(statement, index++, record.elevation) != SQLITE_OK
        || sqlite3_bind_int(statement, index++, record.hasCoordinate ? 1 : 0) != SQLITE_OK) {
        return false;
    }
    if (!bind(record.receiverType) || !bind(record.receiverSystems)
        || !bind(record.receiverSerial) || !bind(record.receiverFirmware)
        || !bind(record.receiverDateInstalled) || !bind(record.antennaType)
        || !bind(record.antennaRadome) || !bind(record.antennaSerial)
        || !bind(record.antennaReferencePoint)
        || !bind(record.antennaDateInstalled) || !bind(record.agency)
        || !bind(record.operatorName)) return false;
    if (sqlite3_bind_int64(statement, index++, static_cast<sqlite3_int64>(
            record.networkMask)) != SQLITE_OK) return false;
    if (!bind(record.sourceDirectory) || !bind(record.fileName)
        || !bind(record.revisionDate) || !bind(record.fetchedAtUtc)
        || !bind(record.rawText)) return false;
    return true;
}

// Implemented in the history-parser section below; forward declarations keep
// the SQLite persistence helpers close to BindRecord without changing the
// public API.
std::string NormalizeEquipmentDate(std::string value);
bool EquipmentHasData(const SiteLogEquipmentEpoch& epoch);

// Keep the history table narrow and independent from the (occasionally very
// large) raw_text column.  The key is stable for one source document and one
// numbered 3.x/4.x block, which makes repeated refreshes idempotent.
std::string EquipmentHistoryKey(const SiteLogEquipmentEpoch& epoch) {
    std::string key = epoch.sourceUrl;
    key.push_back('\x1f');
    key += LowerAscii(epoch.kind);
    key.push_back('\x1f');
    key += std::to_string(epoch.sequence);
    return key;
}

void MergeEquipmentHistory(
    std::vector<SiteLogEquipmentEpoch>& target,
    const std::vector<SiteLogEquipmentEpoch>& source) {
    std::unordered_map<std::string, std::size_t> positions;
    positions.reserve(target.size() + source.size());
    for (std::size_t index = 0; index < target.size(); ++index) {
        positions.emplace(EquipmentHistoryKey(target[index]), index);
    }
    for (const auto& epoch : source) {
        const std::string key = EquipmentHistoryKey(epoch);
        const auto found = positions.find(key);
        if (found == positions.end()) {
            positions.emplace(key, target.size());
            target.push_back(epoch);
            continue;
        }
        // A re-parsed row with the same source URL/block is authoritative for
        // that block.  Replace rather than append a duplicate interval.
        target[found->second] = epoch;
    }
}

void NormalizeEquipmentProvenance(
    SiteLogRecord& record,
    std::vector<SiteLogEquipmentEpoch>& history) {
    if (record.fetchedAtUtc.empty()) record.fetchedAtUtc = CurrentUtc();
    for (auto& epoch : history) {
        if (epoch.kind.empty()) continue;
        epoch.kind = LowerAscii(Trim(epoch.kind));
        if (epoch.kind != "receiver" && epoch.kind != "antenna") continue;
        if (epoch.sequence <= 0) epoch.sequence = 1;
        if (epoch.stationId.empty()) epoch.stationId = record.stationId;
        if (epoch.fourCharId.empty()) epoch.fourCharId = record.fourCharId;
        if (epoch.sourceUrl.empty()) epoch.sourceUrl = record.sourceUrl;
        if (epoch.revisionDate.empty()) epoch.revisionDate = record.revisionDate;
        if (epoch.fetchedAtUtc.empty()) epoch.fetchedAtUtc = record.fetchedAtUtc;
        if (epoch.networkMask == 0) epoch.networkMask = record.networkMask;
        // Dates coming from API/native logs are not always normalized by the
        // text parser.  Store only recognizable ISO dates.  An unparseable
        // token must not participate in lexical date ordering as if it were
        // a real interval boundary.
        if (!epoch.dateInstalled.empty()) {
            const auto normalized = NormalizeEquipmentDate(epoch.dateInstalled);
            epoch.dateInstalled = normalized;
        }
        if (!epoch.dateRemoved.empty()) {
            const auto normalized = NormalizeEquipmentDate(epoch.dateRemoved);
            epoch.dateRemoved = normalized;
        }
    }
}

bool BindEquipmentEpoch(
    sqlite3_stmt* statement,
    const SiteLogEquipmentEpoch& epoch) {
    int index = 1;
    const auto bind = [&](const std::string& value) {
        return BindText(statement, index++, value);
    };
    if (!bind(epoch.sourceUrl) || !bind(epoch.stationId)
        || !bind(epoch.fourCharId) || !bind(epoch.kind)) return false;
    if (sqlite3_bind_int(statement, index++, epoch.sequence) != SQLITE_OK) {
        return false;
    }
    if (!bind(epoch.type) || !bind(epoch.systems) || !bind(epoch.serial)
        || !bind(epoch.firmware) || !bind(epoch.radome)
        || !bind(epoch.referencePoint) || !bind(epoch.dateInstalled)
        || !bind(epoch.dateRemoved) || !bind(epoch.revisionDate)
        || !bind(epoch.fetchedAtUtc)) return false;
    if (sqlite3_bind_int64(statement, index++, static_cast<sqlite3_int64>(
            epoch.networkMask)) != SQLITE_OK) return false;
    return true;
}

using EquipmentHistoryMap =
    std::unordered_map<std::string, std::vector<SiteLogEquipmentEpoch>>;

bool LoadEquipmentHistoryMap(
    sqlite3* database,
    const std::uint64_t preferredNetworkMask,
    EquipmentHistoryMap& historyBySource,
    std::wstring& error) {
    historyBySource.clear();
    if (database == nullptr) return false;
    auto table = Prepare(
        database,
        "SELECT 1 FROM sqlite_master WHERE type='table'"
        " AND name='site_log_equipment' LIMIT 1;",
        error);
    if (!table) return false;
    if (sqlite3_step(table.get()) != SQLITE_ROW) return true;

    const char* sql =
        "SELECT source_url,station_id,four_char_id,equipment_kind,sequence,"
        "equipment_type,systems,serial,firmware,radome,reference_point,"
        "date_installed,date_removed,revision_date,fetched_at_utc,network_mask "
        "FROM site_log_equipment "
        "WHERE ?1=0 OR (network_mask & ?1) != 0;";
    auto statement = Prepare(database, sql, error);
    if (!statement) return false;
    if (sqlite3_bind_int64(
            statement.get(), 1,
            static_cast<sqlite3_int64>(preferredNetworkMask)) != SQLITE_OK) {
        error = SqliteError(database, L"绑定 Site Log 履历来源筛选失败");
        return false;
    }
    while (true) {
        const int status = sqlite3_step(statement.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            error = SqliteError(database, L"读取 Site Log 设备履历失败");
            historyBySource.clear();
            return false;
        }
        SiteLogEquipmentEpoch epoch;
        epoch.sourceUrl = ColumnText(statement.get(), 0);
        epoch.stationId = ColumnText(statement.get(), 1);
        epoch.fourCharId = ColumnText(statement.get(), 2);
        epoch.kind = LowerAscii(ColumnText(statement.get(), 3));
        epoch.sequence = sqlite3_column_int(statement.get(), 4);
        epoch.type = ColumnText(statement.get(), 5);
        epoch.systems = ColumnText(statement.get(), 6);
        epoch.serial = ColumnText(statement.get(), 7);
        epoch.firmware = ColumnText(statement.get(), 8);
        epoch.radome = ColumnText(statement.get(), 9);
        epoch.referencePoint = ColumnText(statement.get(), 10);
        epoch.dateInstalled = ColumnText(statement.get(), 11);
        epoch.dateRemoved = ColumnText(statement.get(), 12);
        epoch.revisionDate = ColumnText(statement.get(), 13);
        epoch.fetchedAtUtc = ColumnText(statement.get(), 14);
        epoch.networkMask = static_cast<std::uint64_t>(
            sqlite3_column_int64(statement.get(), 15));
        if (!epoch.dateInstalled.empty()) {
            epoch.dateInstalled = NormalizeEquipmentDate(epoch.dateInstalled);
        }
        if (!epoch.dateRemoved.empty()) {
            epoch.dateRemoved = NormalizeEquipmentDate(epoch.dateRemoved);
        }
        if (epoch.sourceUrl.empty()
            || (epoch.kind != "receiver" && epoch.kind != "antenna")) {
            continue;
        }
        historyBySource[epoch.sourceUrl].push_back(std::move(epoch));
    }
    return true;
}

bool EquipmentDateActive(
    const SiteLogEquipmentEpoch& epoch,
    const std::string_view asOfDate) {
    if (asOfDate.empty()) return true;
    if (!epoch.dateInstalled.empty()
        && epoch.dateInstalled > asOfDate) return false;
    if (!epoch.dateRemoved.empty() && asOfDate >= epoch.dateRemoved) {
        return false;
    }
    return true;
}

bool HasDatedEquipment(
    const std::vector<SiteLogEquipmentEpoch>& history,
    const std::string_view kind) {
    return std::any_of(
        history.begin(), history.end(),
        [&](const SiteLogEquipmentEpoch& epoch) {
            return epoch.kind == kind
                && (!epoch.dateInstalled.empty()
                    || !epoch.dateRemoved.empty());
        });
}

bool EquipmentPreferred(
    const SiteLogEquipmentEpoch& candidate,
    const SiteLogEquipmentEpoch& current) {
    // ISO dates sort lexically.  Empty installation dates are intentionally
    // least specific and lose to a dated interval.  Revision/fetch are only
    // provenance tie breakers when two records describe the same effective
    // day; they never define the effective interval themselves.
    if (candidate.dateInstalled != current.dateInstalled) {
        if (candidate.dateInstalled.empty()) return false;
        if (current.dateInstalled.empty()) return true;
        return candidate.dateInstalled > current.dateInstalled;
    }
    if (candidate.revisionDate != current.revisionDate) {
        return candidate.revisionDate > current.revisionDate;
    }
    if (candidate.fetchedAtUtc != current.fetchedAtUtc) {
        return candidate.fetchedAtUtc > current.fetchedAtUtc;
    }
    return candidate.sequence > current.sequence;
}

const SiteLogEquipmentEpoch* SelectEffectiveEquipment(
    const std::vector<SiteLogEquipmentEpoch>& history,
    const std::string_view kind,
    const std::string_view asOfDate) {
    // Once a document contains a dated equipment timeline, an undated
    // snapshot is not safe as a substitute for a date-specific lookup.  An
    // undated row may be a later/current record and would otherwise make a
    // decommissioned interval look active forever.  We still support logs
    // that have no dates at all by using their undated snapshot below.
    const bool hasDated = HasDatedEquipment(history, kind);
    const SiteLogEquipmentEpoch* selected = nullptr;
    for (const auto& epoch : history) {
        if (epoch.kind != kind || !EquipmentHasData(epoch)
            || (!asOfDate.empty() && hasDated
                && epoch.dateInstalled.empty() && epoch.dateRemoved.empty())
            || !EquipmentDateActive(epoch, asOfDate)) continue;
        if (selected == nullptr || EquipmentPreferred(epoch, *selected)) {
            selected = &epoch;
        }
    }
    return selected;
}

void ApplyEffectiveEquipmentFromHistory(
    SiteLogRecord& record,
    const std::vector<SiteLogEquipmentEpoch>& history,
    const std::string_view asOfDate) {
    const bool hasDatedReceiver = HasDatedEquipment(
        history, "receiver");
    const bool hasDatedAntenna = HasDatedEquipment(
        history, "antenna");
    const auto* receiver = SelectEffectiveEquipment(
        history, "receiver", asOfDate);
    const auto* antenna = SelectEffectiveEquipment(
        history, "antenna", asOfDate);
    if (receiver != nullptr) {
        // Replace the complete receiver snapshot.  Leaving a non-empty
        // scalar field from a newer block in place when the selected block
        // omits it would silently mix two different physical receivers.
        record.receiverType = receiver->type;
        record.receiverSystems = receiver->systems;
        record.receiverSerial = receiver->serial;
        record.receiverFirmware = receiver->firmware;
        record.receiverDateInstalled = receiver->dateInstalled;
    } else if (!asOfDate.empty() && hasDatedReceiver) {
        // The station can still have an observation file after its last
        // recorded receiver was removed.  Keep the station in the daily
        // availability result, but do not attach a stale/current receiver.
        record.receiverType.clear();
        record.receiverSystems.clear();
        record.receiverSerial.clear();
        record.receiverFirmware.clear();
        record.receiverDateInstalled.clear();
    }
    if (antenna != nullptr) {
        record.antennaType = antenna->type;
        record.antennaRadome = antenna->radome;
        record.antennaSerial = antenna->serial;
        record.antennaReferencePoint = antenna->referencePoint;
        record.antennaDateInstalled = antenna->dateInstalled;
    } else if (!asOfDate.empty() && hasDatedAntenna) {
        record.antennaType.clear();
        record.antennaRadome.clear();
        record.antennaSerial.clear();
        record.antennaReferencePoint.clear();
        record.antennaDateInstalled.clear();
    }
}

void ApplyEffectiveEquipment(
    SiteLogRecord& record,
    const std::string_view asOfDate) {
    ApplyEffectiveEquipmentFromHistory(record, record.equipmentHistory, asOfDate);
}

std::string KeyForRecord(const SiteLogRecord& record) {
    return !record.stationId.empty()
        ? UpperAscii(record.stationId)
        : UpperAscii(record.fourCharId);
}

void FillMissing(SiteLogRecord& target, const SiteLogRecord& source) {
    const auto fill = [](std::string& dst, const std::string& src) {
        if (dst.empty() && !src.empty()) dst = src;
    };
    fill(target.stationId, source.stationId);
    fill(target.fourCharId, source.fourCharId);
    fill(target.domes, source.domes);
    fill(target.siteName, source.siteName);
    fill(target.city, source.city);
    fill(target.country, source.country);
    fill(target.receiverType, source.receiverType);
    fill(target.receiverSystems, source.receiverSystems);
    fill(target.receiverSerial, source.receiverSerial);
    fill(target.receiverFirmware, source.receiverFirmware);
    fill(target.receiverDateInstalled, source.receiverDateInstalled);
    fill(target.antennaType, source.antennaType);
    fill(target.antennaRadome, source.antennaRadome);
    fill(target.antennaSerial, source.antennaSerial);
    fill(target.antennaReferencePoint, source.antennaReferencePoint);
    fill(target.antennaDateInstalled, source.antennaDateInstalled);
    fill(target.agency, source.agency);
    fill(target.operatorName, source.operatorName);
    if (!target.hasXyz && source.hasXyz) {
        target.x = source.x; target.y = source.y; target.z = source.z;
        target.hasXyz = true;
    }
    if (!target.hasCoordinate && source.hasCoordinate) {
        target.latitude = source.latitude;
        target.longitude = source.longitude;
        target.elevation = source.elevation;
        target.hasCoordinate = true;
    }
}

// Merge two rows that refer to the same station (possibly through a
// four-character/9-character alias).  The directory query is grouped by
// identifier, so an older four-character final log can be visited before a
// newer 9-character current log.  Prefer the newest revision as the base row
// while retaining non-empty metadata that only exists in the older row.
bool IsNewerRevision(const SiteLogRecord& candidate,
                     const SiteLogRecord& current) {
    if (!candidate.revisionDate.empty() || !current.revisionDate.empty()) {
        if (candidate.revisionDate.empty()) return false;
        if (current.revisionDate.empty()) return true;
        if (candidate.revisionDate != current.revisionDate) {
            return candidate.revisionDate > current.revisionDate;
        }
    }
    return candidate.fetchedAtUtc > current.fetchedAtUtc;
}

void MergeSiteLogRecords(SiteLogRecord& target,
                         const SiteLogRecord& source) {
    // Scalar fields still follow the newest document revision for backwards
    // compatibility, but equipment intervals are independent historical
    // facts.  Retain/merge every numbered block before choosing the scalar
    // snapshot so a later Site Log cannot erase an older receiver interval.
    MergeEquipmentHistory(target.equipmentHistory, source.equipmentHistory);
    if (IsNewerRevision(source, target)) {
        SiteLogRecord preferred = source;
        FillMissing(preferred, target);
        preferred.networkMask |= target.networkMask;
        // The history merge above was performed on target; carry it onto the
        // preferred scalar record rather than replacing it with source's
        // subset of intervals.
        preferred.equipmentHistory = target.equipmentHistory;
        target = std::move(preferred);
    } else {
        target.networkMask |= source.networkMask;
        FillMissing(target, source);
    }
}

struct DiscoveredLog {
    std::wstring url;
    std::wstring directory;
    std::uint64_t networkMask = 0;
};

bool SafeLogName(std::string value, const bool allowText = false) {
    if (value.empty() || value.size() > 160) return false;
    const std::string lower = LowerAscii(value);
    const bool logExtension = lower.ends_with(".log");
    const bool textExtension = allowText && lower.ends_with(".txt");
    if (!logExtension && !textExtension) return false;
    return value.find('/') == std::string::npos
        && value.find('\\') == std::string::npos
        && value.find("..") == std::string::npos;
}

std::vector<DiscoveredLog> ParseDirectory(
    const std::string& body,
    const std::wstring& root,
    const std::uint64_t networkMask,
    const bool allowText = false) {
    std::vector<DiscoveredLog> result;
    std::unordered_set<std::string> seen;
    auto add = [&](std::string value) {
        const auto amp = value.find("&amp;");
        if (amp != std::string::npos) value.replace(amp, 5, "&");
        const auto query = value.find_first_of("?#");
        if (query != std::string::npos) value.resize(query);
        const auto slash = value.find_last_of('/');
        if (slash != std::string::npos) value = value.substr(slash + 1);
        if (!SafeLogName(value, allowText)) return;
        const std::string key = LowerAscii(value);
        if (!seen.insert(key).second) return;
        std::wstring url(root);
        url.append(Utf8ToWide(value));
        result.push_back(DiscoveredLog{std::move(url), root, networkMask});
    };

    const std::regex hrefPattern(
        allowText
            ? R"(href\s*=\s*["']([^"']+\.(?:log|txt))["'])"
            : R"(href\s*=\s*["']([^"']+\.log)["'])",
        std::regex::icase | std::regex::optimize);
    for (std::sregex_iterator it(body.begin(), body.end(), hrefPattern),
         end;
         it != end;
         ++it) {
        add((*it)[1].str());
    }
    // Apache directory listings normally expose href attributes.  The plain
    // fallback also handles a future text/FTP mirror without changing the
    // parser or database format.
    const std::regex plainPattern(
        allowText
            ? R"(([A-Za-z0-9][A-Za-z0-9_.-]*\.(?:log|txt)))"
            : R"(([A-Za-z0-9][A-Za-z0-9_.-]*\.log))",
        std::regex::icase | std::regex::optimize);
    for (std::sregex_iterator it(body.begin(), body.end(), plainPattern),
         end;
         it != end;
         ++it) {
        add((*it)[1].str());
    }
    return result;
}

std::string DecodeHtmlEntities(std::string value) {
    const std::pair<std::string_view, std::string_view> entities[] = {
        {"&amp;", "&"}, {"&quot;", "\""}, {"&#x2F;", "/"},
        {"&#47;", "/"}
    };
    for (const auto [entity, replacement] : entities) {
        std::size_t position = 0;
        while ((position = value.find(entity, position))
               != std::string::npos) {
            value.replace(position, entity.size(), replacement);
            position += replacement.size();
        }
    }
    return value;
}

std::wstring AbsoluteSiteLogUrl(
    std::string href,
    const std::wstring_view pageUrl) {
    href = DecodeHtmlEntities(std::move(href));
    const auto query = href.find_first_of("?#");
    if (query != std::string::npos) href.resize(query);
    if (href.empty()) return {};
    const std::string lower = LowerAscii(href);
    if (lower.starts_with("https://") || lower.starts_with("http://")
        || lower.starts_with("ftp://")) {
        return Utf8ToWide(href);
    }
    // The Hong Kong page uses root-relative /common/SiteLog/*.txt links.
    // Keep normalization host-scoped to the page that supplied the links;
    // a third-party URL embedded in HTML is never accepted implicitly.
    const std::wstring page(pageUrl);
    const auto schemeEnd = page.find(L"://");
    if (schemeEnd == std::wstring::npos) return {};
    const auto hostEnd = page.find(L'/', schemeEnd + 3);
    if (hostEnd == std::wstring::npos) return {};
    if (!href.empty() && href.front() == '/') {
        return page.substr(0, hostEnd) + Utf8ToWide(href);
    }
    const auto slash = page.find_last_of(L'/');
    const std::wstring directory = slash == std::wstring::npos
        ? page + L"/" : page.substr(0, slash + 1);
    return directory + Utf8ToWide(href);
}

std::vector<DiscoveredLog> ParseHtmlSiteLogLinks(
    const std::string& body,
    const std::wstring& pageUrl,
    const std::uint64_t networkMask) {
    std::vector<DiscoveredLog> result;
    std::unordered_set<std::string> seen;
    // Site-log links on regional portals are not always named *.log; HK's
    // SatRef page uses /common/SiteLog/HKCL_log.txt.  Require the path to
    // contain "sitelog" (case-insensitive) so coordinate PDFs and images on
    // the same page cannot be mistaken for a log.
    static const std::regex pattern(
        R"(href\s*=\s*["']([^"']*sitelog[^"']+\.(?:log|txt))["'])",
        std::regex::icase | std::regex::optimize);
    for (std::sregex_iterator it(body.begin(), body.end(), pattern), end;
         it != end;
         ++it) {
        const std::wstring url = AbsoluteSiteLogUrl((*it)[1].str(), pageUrl);
        if (url.empty()) continue;
        const std::string key = LowerAscii(WideToUtf8(url));
        if (!seen.insert(key).second) continue;
        const auto slash = url.find_last_of(L'/');
        const std::wstring directory = slash == std::wstring::npos
            ? pageUrl : url.substr(0, slash + 1);
        result.push_back(DiscoveredLog{url, directory, networkMask});
    }
    return result;
}

void AddDiscoveredUrl(
    std::vector<DiscoveredLog>& destination,
    std::unordered_set<std::string>& seen,
    const std::wstring& url,
    const std::wstring& directory,
    const std::uint64_t networkMask) {
    if (url.empty()) return;
    const std::string key = LowerAscii(WideToUtf8(url));
    if (!seen.insert(key).second) return;
    destination.push_back(DiscoveredLog{url, directory, networkMask});
}

// Directory listings and individual site-log files are small, idempotent
// read-only requests.  A transient FTP data-channel reset or HTTPS gateway
// timeout should not make an otherwise healthy metadata build fail.  Keep the
// retry policy local to SiteLog so observation downloads retain their own
// network semantics.  Explicit 4xx/"not found" responses are not retried.
bool IsRetryableSiteLogResponse(const HttpTextResult& response) {
    if (response.ok()) return false;
    const std::string lower = LowerAscii(WideToUtf8(response.error));
    constexpr std::string_view permanent[] = {
        "401", "403", "404", "410", "550", "not found",
        "no such file", "permission denied", "unauthorized",
        "forbidden"
    };
    for (const auto marker : permanent) {
        if (lower.find(marker) != std::string::npos) return false;
    }
    // Curl/WinINet leave statusCode at zero for transport failures.  A 5xx
    // response is also safe to retry; ordinary 4xx responses are not.
    return response.transportErrorCode != 0
        || response.statusCode == 0
        || response.statusCode >= 500;
}

HttpTextResult FetchSiteLogTextWithRetry(
    const std::wstring& url,
    const std::stop_token stopToken,
    const std::size_t maximumBytes,
    const int maximumAttempts = 3) {
    HttpTextResult response;
    const int attempts = std::max(1, std::min(maximumAttempts, 3));
    for (int attempt = 0;
         attempt < attempts && !stopToken.stop_requested();
         ++attempt) {
        response = HttpGetText(url, stopToken, maximumBytes);
        if (response.ok() || !IsRetryableSiteLogResponse(response)
            || attempt + 1 >= attempts) {
            return response;
        }
        // Keep backoff short: Site Log builds may contain several hundred
        // files, while still giving a reset FTP data channel time to recover.
        const auto delay = std::chrono::milliseconds(250 * (1 << attempt));
        const auto deadline = std::chrono::steady_clock::now() + delay;
        while (!stopToken.stop_requested()
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    return response;
}

struct EposIcelandStationSeed {
    std::string marker;
    double latitude = 0.0;
    double longitude = 0.0;
    bool hasCoordinate = false;
};

std::string JsonUnescapeSiteLog(std::string value) {
    std::string result;
    result.reserve(value.size());
    bool escaped = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (!escaped) {
            if (character == '\\') {
                escaped = true;
            } else {
                result.push_back(character);
            }
            continue;
        }
        escaped = false;
        switch (character) {
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        default:
            // Preserve unknown JSON escapes rather than silently dropping
            // metadata; this also keeps a literal Unicode escape visible in
            // raw_text when the endpoint returns one.
            result.push_back(character);
            break;
        }
    }
    if (escaped) result.push_back('\\');
    return result;
}

// Locate a JSON member without using std::regex.  The GA catalogue embeds
// tens of kilobytes of escaped GeodesyML in every object; the old ECMAScript
// regular expressions could backtrack deeply on that field and terminate the
// process with a stack-buffer overrun on some MSVC runtimes.  This small
// string-aware scanner is linear, ignores member-looking text inside quoted
// values, and is sufficient for the read-only metadata fields consumed here.
std::size_t JsonMemberValueStart(
    const std::string_view object,
    const std::string_view key) {
    std::size_t index = 0;
    while (index < object.size()) {
        if (object[index] != '"') {
            ++index;
            continue;
        }
        // A JSON property name follows `{` or `,` (apart from whitespace).
        // String values, especially GA's large escaped XML document, can be
        // skipped without allocating/copying their contents.
        std::size_t previous = index;
        while (previous > 0
               && std::isspace(static_cast<unsigned char>(
                   object[previous - 1])) != 0) {
            --previous;
        }
        const bool isPropertyName = previous > 0
            && (object[previous - 1] == '{' || object[previous - 1] == ',');
        const std::size_t stringBegin = index + 1;
        std::size_t end = stringBegin;
        bool escaped = false;
        for (; end < object.size(); ++end) {
            const char character = object[end];
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                break;
            }
        }
        if (end >= object.size()) return std::string_view::npos;
        if (!isPropertyName) {
            index = end + 1;
            continue;
        }
        const std::string_view rawKey = object.substr(
            stringBegin, end - stringBegin);
        bool keyMatches = rawKey == key;
        if (!keyMatches && rawKey.find('\\') != std::string_view::npos) {
            keyMatches = JsonUnescapeSiteLog(std::string(rawKey)) == key;
        }
        if (!keyMatches) {
            index = end + 1;
            continue;
        }
        std::size_t cursor = end + 1;
        while (cursor < object.size()
               && std::isspace(static_cast<unsigned char>(
                   object[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= object.size() || object[cursor] != ':') continue;
        ++cursor;
        while (cursor < object.size()
               && std::isspace(static_cast<unsigned char>(
                   object[cursor])) != 0) {
            ++cursor;
        }
        return cursor;
    }
    return std::string_view::npos;
}

// Escape a JSON member name before embedding it in the small ECMAScript
// regular expressions used by the lightweight parser below.  M3G uses
// human-readable keys such as `GNSS Receiver (Current)`; without escaping,
// the parentheses are interpreted as regex groups and the field silently
// fails to match.
std::string RegexEscapeJsonKey(const std::string_view key) {
    std::string escaped;
    escaped.reserve(key.size() * 2);
    constexpr std::string_view metacharacters = R"(\^$.*+?()[]{}|)";
    for (const char character : key) {
        if (metacharacters.find(character) != std::string_view::npos) {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

std::string JsonStringField(
    const std::string_view object,
    const std::string_view key) {
    const std::size_t begin = JsonMemberValueStart(object, key);
    if (begin == std::string_view::npos || begin >= object.size()
        || object[begin] != '"') {
        return {};
    }
    std::size_t end = begin + 1;
    bool escaped = false;
    for (; end < object.size(); ++end) {
        const char character = object[end];
        if (escaped) {
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == '"') {
            break;
        }
    }
    if (end >= object.size()) return {};
    return JsonUnescapeSiteLog(std::string(object.substr(
        begin + 1, end - begin - 1)));
}

bool JsonNumberField(
    const std::string_view object,
    const std::string_view key,
    double& value) {
    const std::size_t begin = JsonMemberValueStart(object, key);
    if (begin == std::string_view::npos) return false;
    std::size_t end = begin;
    while (end < object.size()
           && object[end] != ',' && object[end] != '}'
           && object[end] != ']' && std::isspace(static_cast<unsigned char>(
               object[end])) == 0) {
        ++end;
    }
    if (end == begin) return false;
    return ParseDouble(Trim(object.substr(begin, end - begin)), value);
}

std::string JsonIntegerField(
    const std::string_view object,
    const std::string_view key) {
    const std::size_t begin = JsonMemberValueStart(object, key);
    if (begin == std::string_view::npos) return {};
    std::size_t end = begin;
    while (end < object.size()
           && std::isdigit(static_cast<unsigned char>(object[end])) != 0) {
        ++end;
    }
    if (end == begin) return {};
    return std::string(object.substr(begin, end - begin));
}

std::vector<EposIcelandStationSeed> ParseEposIcelandStationList(
    const std::string& body) {
    std::vector<EposIcelandStationSeed> result;
    std::unordered_set<std::string> seen;
    // The IMO API currently returns an array of objects containing marker,
    // lat and lon.  Accept station_marker/id aliases as older deployments
    // used those names.  A few responses expose the nine-character marker;
    // normalize it to the four-character prefix because the sitelog endpoint
    // accepts (and documents) that prefix as its canonical selector.
    static const std::regex markerPattern(
        R"rx("(?:marker|station_marker|stationMarker|id)"\s*:\s*"([A-Za-z0-9]{4,9})")rx",
        std::regex::icase | std::regex::optimize);
    for (std::sregex_iterator it(body.begin(), body.end(), markerPattern),
         end;
         it != end;
         ++it) {
        std::string marker = UpperAscii((*it)[1].str());
        if (marker.size() >= 9) marker.resize(4);
        if (marker.size() != 4) continue;
        if (!seen.insert(marker).second) continue;
        // Use the enclosing object when available to associate coordinates.
        const std::size_t position = static_cast<std::size_t>(it->position());
        const std::size_t objectBegin = body.rfind('{', position);
        const std::size_t objectEnd = body.find('}', position);
        const std::string object = objectBegin != std::string::npos
            && objectEnd != std::string::npos && objectEnd > objectBegin
            ? body.substr(objectBegin, objectEnd - objectBegin + 1)
            : std::string{};
        EposIcelandStationSeed seed;
        seed.marker = std::move(marker);
        seed.hasCoordinate = JsonNumberField(object, "lat", seed.latitude)
            && JsonNumberField(object, "lon", seed.longitude);
        if (!seed.hasCoordinate) {
            seed.hasCoordinate =
                JsonNumberField(object, "latitude", seed.latitude)
                && JsonNumberField(object, "longitude", seed.longitude);
        }
        if (!seed.hasCoordinate) {
            // GeoJSON responses keep the marker in `properties` and the
            // coordinates in a sibling `geometry` object.  The small window
            // below stays inside the same feature in normal responses and
            // interprets GeoJSON's [longitude, latitude] ordering.
            const std::size_t windowBegin = position > 2048
                ? position - 2048 : 0;
            const std::size_t windowEnd = std::min(
                body.size(), position + 4096);
            const std::string window = body.substr(
                windowBegin, windowEnd - windowBegin);
            static const std::regex coordinatesPattern(
                R"rx("coordinates"\s*:\s*\[\s*(-?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+))\s*,\s*(-?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)))rx",
                std::regex::icase | std::regex::optimize);
            std::smatch coordinateMatch;
            if (std::regex_search(window, coordinateMatch,
                                  coordinatesPattern)
                && coordinateMatch.size() >= 3) {
                double longitude = 0.0;
                double latitude = 0.0;
                if (ParseDouble(coordinateMatch[1].str(), longitude)
                    && ParseDouble(coordinateMatch[2].str(), latitude)
                    && std::abs(latitude) <= 90.0
                    && std::abs(longitude) <= 180.0) {
                    seed.latitude = latitude;
                    seed.longitude = longitude;
                    seed.hasCoordinate = true;
                }
            }
        }
        result.push_back(std::move(seed));
    }
    return result;
}

std::string EposIcelandLogBody(
    const std::string& responseBody) {
    const std::string lower = LowerAscii(responseBody);
    if (lower.find("site identification") != std::string::npos
        || lower.find("nine character id") != std::string::npos) {
        return responseBody;
    }
    // Some IMO deployments wrap the ASCII log in a JSON object (usually under
    // `sitelog` or `content`).  Extract and unescape that value before passing
    // it to the normal IGS parser.
    constexpr std::string_view keys[] = {
        "sitelog", "site_log", "siteLog", "content", "text", "data"
    };
    for (const auto key : keys) {
        const std::string candidate = JsonStringField(responseBody, key);
        const std::string candidateLower = LowerAscii(candidate);
        if (candidateLower.find("site identification") != std::string::npos
            || candidateLower.find("nine character id") != std::string::npos) {
            return candidate;
        }
    }
    return responseBody;
}

std::string EposMarkerFromUrl(const std::wstring_view url) {
    const std::string narrow = WideToUtf8(url);
    const std::string lower = LowerAscii(narrow);
    const auto markerPosition = lower.find("station_marker=");
    if (markerPosition == std::string::npos) return {};
    const auto begin = markerPosition + std::string("station_marker=").size();
    const auto end = narrow.find_first_of("&#?", begin);
    std::string marker = narrow.substr(
        begin, end == std::string::npos ? std::string::npos : end - begin);
    if (marker.size() != 4) return {};
    return UpperAscii(marker);
}

// Return the JSON value belonging to a member when that value is an object
// or an array.  The GA metadata service is a Spring Data REST endpoint and
// embeds a complete GeodesyML document inside each site-log object.  A full
// JSON dependency would be excessive for this small read-only adapter, so we
// use a string-aware balanced scanner.  It deliberately ignores braces and
// brackets occurring inside quoted strings (the embedded XML contains many
// of those).
std::string JsonCompositeField(
    const std::string_view object,
    const std::string_view key) {
    const std::string marker = "\"" + std::string(key) + "\"";
    std::size_t search = 0;
    while ((search = object.find(marker, search)) != std::string_view::npos) {
        if (search > 0 && object[search - 1] == '\\') {
            search += marker.size();
            continue;
        }
        std::size_t colon = search + marker.size();
        while (colon < object.size()
               && std::isspace(static_cast<unsigned char>(object[colon])) != 0) {
            ++colon;
        }
        if (colon >= object.size() || object[colon] != ':') {
            search += marker.size();
            continue;
        }
        std::size_t begin = colon + 1;
        while (begin < object.size()
               && std::isspace(static_cast<unsigned char>(object[begin])) != 0) {
            ++begin;
        }
        if (begin >= object.size()
            || (object[begin] != '{' && object[begin] != '[')) {
            search = begin;
            continue;
        }
        const char opening = object[begin];
        const char closing = opening == '{' ? '}' : ']';
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        for (std::size_t index = begin; index < object.size(); ++index) {
            const char character = object[index];
            if (inString) {
                if (escaped) {
                    escaped = false;
                } else if (character == '\\') {
                    escaped = true;
                } else if (character == '"') {
                    inString = false;
                }
                continue;
            }
            if (character == '"') {
                inString = true;
            } else if (character == opening) {
                ++depth;
            } else if (character == closing) {
                --depth;
                if (depth == 0) {
                    return std::string(object.substr(
                        begin, index - begin + 1));
                }
            }
        }
        return {};
    }
    return {};
}

std::vector<std::string> JsonTopLevelObjects(
    const std::string_view value) {
    std::vector<std::string> result;
    std::size_t begin = std::string_view::npos;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                inString = false;
            }
            continue;
        }
        if (character == '"') {
            inString = true;
        } else if (character == '{') {
            if (depth == 0) begin = index;
            ++depth;
        } else if (character == '}' && depth > 0) {
            --depth;
            if (depth == 0 && begin != std::string_view::npos) {
                result.emplace_back(value.substr(begin, index - begin + 1));
                begin = std::string_view::npos;
            }
        }
    }
    return result;
}

std::vector<double> JsonNumberArray(const std::string_view value) {
    std::vector<double> result;
    // Coordinates in the GA representation are a plain numeric array.  Do
    // not parse numbers inside quoted metadata if a future response adds one.
    static const std::regex numberPattern(
        R"([-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)",
        std::regex::optimize);
    bool inString = false;
    bool escaped = false;
    std::string unquoted;
    unquoted.reserve(value.size());
    for (const char character : value) {
        if (inString) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') inString = false;
            continue;
        }
        if (character == '"') {
            inString = true;
        } else {
            unquoted.push_back(character);
        }
    }
    for (std::sregex_iterator it(unquoted.begin(), unquoted.end(), numberPattern),
         end; it != end; ++it) {
        double number = 0.0;
        if (ParseDouble(it->str(), number)) result.push_back(number);
    }
    return result;
}

std::string IsoDateOnly(std::string value) {
    value = Trim(value);
    if (value.size() >= 10
        && std::isdigit(static_cast<unsigned char>(value[0])) != 0
        && std::isdigit(static_cast<unsigned char>(value[4])) != 0
        && std::isdigit(static_cast<unsigned char>(value[7])) != 0) {
        return value.substr(0, 10);
    }
    return {};
}

// M3G returns ISO timestamps in the GeoJSON properties and occasionally
// exposes a compact YYYYMMDD revision in the site-log file name.  Keep this
// tolerant helper local to the adapter so a format change does not make a
// valid metadata record look undated.
std::string M3gDateOnly(std::string value) {
    value = Trim(value);
    if (value.size() >= 10
        && std::isdigit(static_cast<unsigned char>(value[0])) != 0
        && std::isdigit(static_cast<unsigned char>(value[1])) != 0
        && std::isdigit(static_cast<unsigned char>(value[2])) != 0
        && std::isdigit(static_cast<unsigned char>(value[3])) != 0
        && value[4] == '-'
        && value[5] >= '0' && value[5] <= '9'
        && value[6] >= '0' && value[6] <= '9'
        && value[7] == '-'
        && value[8] >= '0' && value[8] <= '9'
        && value[9] >= '0' && value[9] <= '9') {
        return value.substr(0, 10);
    }
    for (std::size_t index = 0; index + 8 <= value.size(); ++index) {
        bool digits = true;
        for (std::size_t offset = 0; offset < 8; ++offset) {
            if (std::isdigit(static_cast<unsigned char>(
                    value[index + offset])) == 0) {
                digits = false;
                break;
            }
        }
        if (!digits) continue;
        const std::string compact = value.substr(index, 8);
        return compact.substr(0, 4) + "-" + compact.substr(4, 2)
            + "-" + compact.substr(6, 2);
    }
    return {};
}

std::string NormalizedGaId(std::string value) {
    std::string result;
    for (const unsigned char character : value) {
        if (std::isalnum(character) != 0) {
            result.push_back(static_cast<char>(character));
        }
    }
    return UpperAscii(result);
}

std::string GaSyntheticSiteLog(
    const SiteLogRecord& record,
    const std::string_view geodesyMl) {
    std::ostringstream text;
    text << "1. Site Identification\n"
         << "Four Character ID: " << record.fourCharId << "\n"
         << "Nine-Character ID: " << record.stationId << "\n"
         << "IERS DOMES Number: " << record.domes << "\n"
         << "Site Name: " << record.siteName << "\n"
         << "Country or Region: " << record.country << "\n"
         << "2. Site Location\n"
         << "City or Town: " << record.city << "\n";
    if (record.hasXyz) {
        text << "X coordinate (m): " << std::setprecision(17) << record.x << "\n"
             << "Y coordinate (m): " << std::setprecision(17) << record.y << "\n"
             << "Z coordinate (m): " << std::setprecision(17) << record.z << "\n";
    }
    if (record.hasCoordinate) {
        text << "Latitude (deg): " << std::setprecision(17) << record.latitude << "\n"
             << "Longitude (deg): " << std::setprecision(17) << record.longitude << "\n"
             << "Elevation (m): " << std::setprecision(17) << record.elevation << "\n";
    }
    text << "3. GNSS Receiver\n"
         << "Receiver Type: " << record.receiverType << "\n"
         << "Satellite System: " << record.receiverSystems << "\n"
         << "Serial Number: " << record.receiverSerial << "\n"
         << "Firmware Version: " << record.receiverFirmware << "\n"
         << "Date Installed: " << record.receiverDateInstalled << "\n"
         << "4. GNSS Antenna\n"
         << "Antenna Type: " << record.antennaType << "\n"
         << "Radome Type: " << record.antennaRadome << "\n"
         << "Serial Number: " << record.antennaSerial << "\n"
         << "Antenna Reference Point: " << record.antennaReferencePoint << "\n"
         << "Date Installed: " << record.antennaDateInstalled << "\n"
         << "11. On-Site\n"
         << "Agency: " << record.agency << "\n"
         << "Operator Name: " << record.operatorName << "\n";
    if (!record.revisionDate.empty()) {
        text << "Date Prepared: " << record.revisionDate << "\n";
    }
    text << "# Original machine-readable metadata returned by the official API\n"
         << geodesyMl;
    return text.str();
}

// The GA metadata service returns the actual site log as an escaped
// GeodesyML document in the `siteLogText` JSON property.  It is tempting to
// treat that property as if it were the structured JSON representation, but
// the production endpoint does not expose the nested fields at the object
// level.  Keep a small namespace-agnostic XML reader here instead of adding a
// heavyweight XML dependency to the Windows builder.  The reader is limited
// to text elements used by the GA schema and is deliberately tolerant of
// prefix changes (geo:, gml:, gmd:, ...).
std::string XmlDecodeEntities(std::string value) {
    const auto replaceAll = [&](const std::string_view from,
                                const std::string_view to) {
        std::size_t offset = 0;
        while ((offset = value.find(from, offset)) != std::string::npos) {
            value.replace(offset, from.size(), to);
            offset += to.size();
        }
    };
    replaceAll("&amp;", "&");
    replaceAll("&lt;", "<");
    replaceAll("&gt;", ">");
    replaceAll("&quot;", "\"");
    replaceAll("&apos;", "'");
    return value;
}

std::vector<std::string> XmlBlocks(
    const std::string_view xml,
    const std::string_view localName) {
    if (xml.empty() || localName.empty()) return {};
    // Build the pattern in pieces so a local name containing '-' remains
    // literal.  All names passed by this file are fixed schema names.
    const std::string pattern =
        R"(<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?)" + std::string(localName)
        + R"((?:\s[^>]*)?>([\s\S]*?)</(?:[A-Za-z_][A-Za-z0-9_.-]*:)?)"
        + std::string(localName) + R"(\s*>)";
    std::regex expression(pattern, std::regex::icase | std::regex::optimize);
    const std::string copy(xml);
    std::vector<std::string> result;
    for (std::sregex_iterator it(copy.begin(), copy.end(), expression), end;
         it != end; ++it) {
        if (it->size() >= 2) result.push_back((*it)[1].str());
    }
    return result;
}

// GeodesyML uses a lower-case collection element (gnssReceiver/gnssAntenna)
// containing one or more title-case records (GnssReceiver/GnssAntenna).
// Keep an exact-case variant for those two elements so the collection is not
// mistaken for a single historical equipment record.
std::vector<std::string> XmlBlocksExact(
    const std::string_view xml,
    const std::string_view localName) {
    if (xml.empty() || localName.empty()) return {};
    const std::string pattern =
        R"(<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?)" + std::string(localName)
        + R"((?:\s[^>]*)?>([\s\S]*?)</(?:[A-Za-z_][A-Za-z0-9_.-]*:)?)"
        + std::string(localName) + R"(\s*>)";
    std::regex expression(pattern, std::regex::optimize);
    const std::string copy(xml);
    std::vector<std::string> result;
    for (std::sregex_iterator it(copy.begin(), copy.end(), expression), end;
         it != end; ++it) {
        if (it->size() >= 2) result.push_back((*it)[1].str());
    }
    return result;
}

// CDATA contains character data, not markup.  The old <[^>]*> replacement
// erased the entire <![CDATA[...]]> field in M3G equipment timelines.
std::string XmlCharacterData(const std::string_view xml) {
    std::string result;
    std::size_t position = 0;
    while (position < xml.size()) {
        if (xml.substr(position, 9) == "<![CDATA[") {
            const auto end = xml.find("]]>", position + 9);
            if (end == std::string_view::npos) break;
            result.append(xml.substr(position + 9, end - position - 9));
            position = end + 3;
        } else if (xml[position] == '<') {
            const auto end = xml.find('>', position + 1);
            if (end == std::string_view::npos) break;
            result += ' ';
            position = end + 1;
        } else {
            const auto end = xml.find('<', position);
            const auto count = end == std::string_view::npos
                ? xml.size() - position : end - position;
            result += XmlDecodeEntities(std::string(xml.substr(position, count)));
            position += count;
        }
    }
    return Trim(result);
}

std::string XmlText(
    const std::string_view xml,
    const std::string_view localName) {
    const auto blocks = XmlBlocks(xml, localName);
    if (blocks.empty()) return {};
    return XmlCharacterData(blocks.front());
}

std::vector<std::string> XmlTexts(
    const std::string_view xml,
    const std::string_view localName) {
    std::vector<std::string> result;
    for (auto& block : XmlBlocks(xml, localName)) {
        block = XmlCharacterData(block);
        if (!block.empty()) result.push_back(std::move(block));
    }
    return result;
}

// Forward declaration: the implementation lives next to the ASCII history
// parser below.  Keeping one GeodesyML extractor lets both the live GA
// reader and --backfill-history recover every receiver/antenna interval from
// the XML embedded in an already stored raw_text value.
std::vector<SiteLogEquipmentEpoch> ParseGeodesyMlEquipmentHistory(
    std::string_view xml);

std::vector<double> XmlNumbers(const std::string_view value) {
    static const std::regex numberPattern(
        R"([-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)",
        std::regex::optimize);
    std::vector<double> result;
    const std::string copy(value);
    for (std::sregex_iterator it(copy.begin(), copy.end(), numberPattern),
         end; it != end; ++it) {
        double number = 0.0;
        if (ParseDouble(it->str(), number)) result.push_back(number);
    }
    return result;
}

std::string JoinUniqueXmlValues(const std::vector<std::string>& values) {
    std::string result;
    std::unordered_set<std::string> seen;
    for (const auto& value : values) {
        const std::string normalized = Trim(value);
        if (normalized.empty() || !seen.insert(normalized).second) continue;
        if (!result.empty()) result += "+";
        result += normalized;
    }
    return result;
}

SiteLogRecord ParseGaGeodesyMl(const std::string_view xml) {
    SiteLogRecord record;
    record.fourCharId = UpperAscii(XmlText(xml, "fourCharacterID"));
    if (record.fourCharId.empty()) {
        record.fourCharId = UpperAscii(XmlText(xml, "fourCharacterId"));
    }
    record.stationId = NormalizedGaId(XmlText(xml, "nineCharacterID"));
    if (record.stationId.empty()) {
        record.stationId = NormalizedGaId(XmlText(xml, "nineCharacterId"));
    }
    record.domes = Trim(XmlText(xml, "iersDOMESNumber"));
    record.siteName = Trim(XmlText(xml, "siteName"));
    record.city = Trim(XmlText(xml, "city"));
    record.country = UpperAscii(Trim(XmlText(xml, "countryCodeISO")));
    if (record.country.empty()) {
        record.country = UpperAscii(Trim(XmlText(xml, "country")));
    }

    const auto locations = XmlBlocks(xml, "approximatePositionITRF");
    const std::string location = locations.empty() ? std::string(xml)
                                                   : locations.front();
    const auto cartesian = XmlBlocks(location, "cartesianPosition");
    const auto xyz = XmlNumbers(XmlText(
        cartesian.empty() ? location : cartesian.front(), "pos"));
    if (xyz.size() >= 3) {
        record.x = xyz[0]; record.y = xyz[1]; record.z = xyz[2];
        record.hasXyz = true;
    }
    const auto geodetic = XmlBlocks(location, "geodeticPosition");
    const auto llh = XmlNumbers(XmlText(
        geodetic.empty() ? location : geodetic.front(), "pos"));
    if (llh.size() >= 2 && std::isfinite(llh[0]) && std::isfinite(llh[1])) {
        record.latitude = llh[0];
        record.longitude = llh[1];
        record.elevation = llh.size() >= 3 ? llh[2] : 0.0;
        record.hasCoordinate = true;
    }

    auto receiverBlocks = XmlBlocksExact(xml, "GnssReceiver");
    if (receiverBlocks.empty()) {
        receiverBlocks = XmlBlocks(xml, "GnssReceiver");
    }
    std::string selectedReceiver;
    for (const auto& candidate : receiverBlocks) {
        if (XmlText(candidate, "dateRemoved").empty()) {
            selectedReceiver = candidate;
            break;
        }
        if (selectedReceiver.empty()) selectedReceiver = candidate;
    }
    if (!selectedReceiver.empty()) {
        record.receiverType = Trim(XmlText(selectedReceiver, "igsModelCode"));
        if (record.receiverType.empty()) {
            record.receiverType = Trim(XmlText(selectedReceiver, "receiverType"));
        }
        record.receiverSystems = JoinUniqueXmlValues(
            XmlTexts(selectedReceiver, "satelliteSystem"));
        record.receiverSerial = Trim(XmlText(
            selectedReceiver, "manufacturerSerialNumber"));
        record.receiverFirmware = Trim(XmlText(
            selectedReceiver, "firmwareVersion"));
        record.receiverDateInstalled = Trim(XmlText(
            selectedReceiver, "dateInstalled"));
    }

    auto antennaBlocks = XmlBlocksExact(xml, "GnssAntenna");
    if (antennaBlocks.empty()) {
        antennaBlocks = XmlBlocks(xml, "GnssAntenna");
    }
    std::string selectedAntenna;
    for (const auto& candidate : antennaBlocks) {
        if (XmlText(candidate, "dateRemoved").empty()) {
            selectedAntenna = candidate;
            break;
        }
        if (selectedAntenna.empty()) selectedAntenna = candidate;
    }
    if (!selectedAntenna.empty()) {
        record.antennaType = Trim(XmlText(selectedAntenna, "igsModelCode"));
        if (record.antennaType.empty()) {
            record.antennaType = Trim(XmlText(selectedAntenna, "type"));
        }
        record.antennaRadome = Trim(XmlText(
            selectedAntenna, "antennaRadomeType"));
        record.antennaSerial = Trim(XmlText(
            selectedAntenna, "manufacturerSerialNumber"));
        record.antennaReferencePoint = Trim(XmlText(
            selectedAntenna, "antennaReferencePoint"));
        record.antennaDateInstalled = Trim(XmlText(
            selectedAntenna, "dateInstalled"));
    }
    // Keep every numbered GeodesyML receiver/antenna block, not just the
    // currently open one selected for the legacy scalar fields above.  The
    // dates in these blocks are the physical installation/removal dates and
    // are the only values used later for a date-specific equipment lookup.
    record.equipmentHistory = ParseGeodesyMlEquipmentHistory(xml);
    record.agency = Trim(XmlText(xml, "organisationName"));
    // An individualName is a contact person, not an operating unit.
    record.operatorName.clear();
    if (record.fourCharId.empty() && record.stationId.size() >= 4) {
        record.fourCharId = record.stationId.substr(0, 4);
    }
    if (record.stationId.empty() && record.fourCharId.size() == 4) {
        const std::string country = record.country.size() == 3
            ? record.country : "AUS";
        record.stationId = record.fourCharId + "00" + country;
    }
    return record;
}

SiteLogRecord ParseGaSiteLogObject(const std::string& object) {
    SiteLogRecord record;
    const std::string geodesyMl = JsonStringField(object, "siteLogText");
    if (!geodesyMl.empty()) {
        record = ParseGaGeodesyMl(geodesyMl);
        record.revisionDate = IsoDateOnly(JsonStringField(
            object, "lastModifiedDate"));
        if (record.fourCharId.empty() && record.stationId.empty()) {
            // Fall through to the structured representation for a future API
            // deployment that sends an empty/partial GeodesyML payload.
            record = SiteLogRecord{};
        } else {
            return record;
        }
    }
    const std::string identification =
        JsonCompositeField(object, "siteIdentification");
    const std::string location = JsonCompositeField(object, "siteLocation");
    record.stationId = NormalizedGaId(
        JsonStringField(identification, "nineCharacterId"));
    record.fourCharId = NormalizedGaId(
        JsonStringField(identification, "fourCharacterId"));
    record.domes = Trim(JsonStringField(identification, "iersDOMESNumber"));
    record.siteName = Trim(JsonStringField(identification, "siteName"));
    record.city = Trim(JsonStringField(location, "city"));
    record.country = UpperAscii(Trim(JsonStringField(location, "country")));

    const std::string position = JsonCompositeField(
        location, "approximatePosition");
    const std::string cartesian = JsonCompositeField(
        position, "cartesianPosition");
    const std::string geodetic = JsonCompositeField(
        position, "geodeticPosition");
    const auto xyz = JsonNumberArray(JsonCompositeField(cartesian, "coordinates"));
    if (xyz.size() >= 3) {
        record.x = xyz[0]; record.y = xyz[1]; record.z = xyz[2];
        record.hasXyz = true;
    }
    const auto llh = JsonNumberArray(JsonCompositeField(geodetic, "coordinates"));
    if (llh.size() >= 2) {
        record.latitude = llh[0];
        record.longitude = llh[1];
        record.elevation = llh.size() >= 3 ? llh[2] : 0.0;
        record.hasCoordinate = std::isfinite(record.latitude)
            && std::isfinite(record.longitude);
    }

    const std::string receivers = JsonCompositeField(object, "gnssReceivers");
    const auto receiverObjects = JsonTopLevelObjects(receivers);
    std::string selectedReceiver;
    for (const auto& candidate : receiverObjects) {
        const std::string deleted = LowerAscii(JsonStringField(candidate, "deleted"));
        const bool explicitlyDeleted = deleted == "true";
        const bool hasRemoval = !JsonStringField(candidate, "dateRemoved").empty();
        if (!explicitlyDeleted && !hasRemoval) {
            selectedReceiver = candidate;
            break;
        }
        if (selectedReceiver.empty()) selectedReceiver = candidate;
    }
    if (!selectedReceiver.empty()) {
        record.receiverType = Trim(JsonStringField(selectedReceiver, "type"));
        record.receiverSystems = Trim(JsonStringField(
            selectedReceiver, "satelliteSystem"));
        record.receiverSerial = Trim(JsonStringField(
            selectedReceiver, "serialNumber"));
        record.receiverFirmware = Trim(JsonStringField(
            selectedReceiver, "firmwareVersion"));
        record.receiverDateInstalled = Trim(JsonStringField(
            selectedReceiver, "dateInstalled"));
    }
    const std::string antenna = JsonCompositeField(object, "gnssAntennas");
    record.antennaType = Trim(JsonStringField(antenna, "type"));
    record.antennaRadome = Trim(JsonStringField(antenna, "antennaRadomeType"));
    record.antennaSerial = Trim(JsonStringField(antenna, "serialNumber"));
    record.antennaReferencePoint = Trim(JsonStringField(
        antenna, "antennaReferencePoint"));
    record.antennaDateInstalled = Trim(JsonStringField(
        antenna, "dateInstalled"));
    record.agency = Trim(JsonStringField(object, "organisationName"));
    // An individualName is a contact person, not an operating unit.
    record.operatorName.clear();
    record.revisionDate = IsoDateOnly(JsonStringField(object, "lastModifiedDate"));
    if (record.revisionDate.empty()) {
        record.revisionDate = IsoDateOnly(JsonStringField(
            JsonCompositeField(object, "formInformation"), "datePrepared"));
    }
    if (record.fourCharId.empty() && record.stationId.size() >= 4) {
        record.fourCharId = record.stationId.substr(0, 4);
    }
    if (record.stationId.empty() && record.fourCharId.size() == 4) {
        const std::string country = record.country.size() == 3
            ? record.country : "AUS";
        record.stationId = record.fourCharId + "00" + country;
    }
    return record;
}

std::vector<std::string> ParseGaSiteLogPage(const std::string& body) {
    const std::string embedded = JsonCompositeField(body, "_embedded");
    const std::string logs = JsonCompositeField(embedded, "siteLogs");
    return JsonTopLevelObjects(logs);
}

struct NativeSiteLogDefinition {
    const char* key;
    const wchar_t* catalogUrl;
    std::uint64_t networkMask;
    bool htmlLinks = false;
    bool httpsListing = false;
    const wchar_t* fallbackCatalogUrl = nullptr;
    // Some national portals expose a station catalogue API rather than a
    // browsable .log directory.  For those sources catalogUrl is the station
    // list endpoint and each returned marker is queried at the corresponding
    // site-log endpoint (currently EPOS-IS/IMO).
    bool stationApi = false;
    // Geoscience Australia publishes a paginated GeodesyML/JSON catalogue;
    // it is not an HTML directory and therefore has its own parser.
    bool gaApi = false;
    // M3G publishes a public GeoJSON view of standardized IGS Site Logs and
    // allows filtering by the owning/linked network.  This is intentionally
    // kept separate from the native-directory flags above: records imported
    // through M3G are labelled as M3G provenance in source_directory rather
    // than being presented as an operator-native archive.
    const char* m3gNetwork = nullptr;
    bool m3gApi = false;
};

constexpr NativeSiteLogDefinition kNativeSiteLogDefinitions[] = {
    {"HK", L"https://www.geodetic.gov.hk/en/satref/__CORS_Stations_Details.htm",
        NetworkBit(ObservationNetwork::Hk), true, false, nullptr},
    {"GA-GDC", L"https://metadata.gnss.ga.gov.au/api/siteLogs",
        NetworkBit(ObservationNetwork::GaGdc), false, false, nullptr, false,
        true},
    {"GEONET-NZ", L"https://data.geonet.org.nz/gnss/sitelogs/logs/",
        NetworkBit(ObservationNetwork::GeoNetNz), false, true,
        L"ftp://ftp.geonet.org.nz/gps/sitelogs/logs/"},
    {"IGN-RAMSAC",
        L"https://wilkilen.fcaglp.unlp.edu.ar/gnss/rinex/RAMSAC/LOG/",
        NetworkBit(ObservationNetwork::Ramsac), false, true,
        L"https://www.sirgas.org/archive/gps/DGF/station/log/"},
    {"EPOS-IS",
        L"https://api.vedur.is/epos/gps/station?bbox=62.8%2C-25.0%2C67.0%2C-12.5&format_type=JSON",
        NetworkBit(ObservationNetwork::EposIceland), false, true, nullptr,
        true},
    {"RGP", L"ftp://rgpdata.ign.fr/pub/logsheet/",
        NetworkBit(ObservationNetwork::Rgp), false, false,
        L"ftp://rgpdata.ensg.eu/pub/logsheet/"},
    {"GREF", L"ftp://igs-ftp.bkg.bund.de/GREF/station/log_9char/",
        NetworkBit(ObservationNetwork::Gref), false, false, nullptr},
    {"SONEL", L"ftp://ftp.sonel.org/meta/gpslog/",
        NetworkBit(ObservationNetwork::Sonel), false, false, nullptr},
    // Garner serves the same flat archive through an Apache HTTPS index.  It
    // is considerably more reliable than the legacy FTP data channel (which
    // can enumerate the directory but then stall on individual listings).
    {"SOPAC-GARNER", L"https://garner.ucsd.edu/pub/docs/station_logs/",
        NetworkBit(ObservationNetwork::SopacGarner), false, true,
        L"https://garner.ucsd.edu/pub/docs/site_logs/"},
    {"NGS", L"https://geodesy.noaa.gov/corsdata/station_log/",
        NetworkBit(ObservationNetwork::Ngs), false, true, nullptr},
    {"EPN", L"https://epncb.oma.be/pub/station/log/",
        NetworkBit(ObservationNetwork::Epn), false, true, nullptr},
    {"CACS", L"ftp://cacsa.nrcan.gc.ca/gps/station_logs/",
        NetworkBit(ObservationNetwork::Cacs), false, false,
        L"ftp://cacsb.nrcan.gc.ca/gps/station_logs/"},
    // These agencies publish station logs in publicly browsable HTTPS
    // directories. Keep each source tied to its native network mask.
    {"ERGNSS", L"https://datos-geodesia.ign.es/ERGNSS/log/",
        NetworkBit(ObservationNetwork::Ergnss), false, true, nullptr},
    // BEV's public directory contains only CHECK_*.BEV health files, not
    // standard Site Logs.  M3G publishes the corresponding APOS records
    // (the UI may label this network APOS(P)) as the public, machine-readable
    // metadata source, so use the currently accepted API name directly.
    {"APOS", L"https://gnss-metadata.eu/v1/sitelog/gnss-metadata",
        NetworkBit(ObservationNetwork::Apos), false, false, nullptr, false,
        false, "APOS", true},
    {"IBGE-RBMC",
        L"https://geoftp.ibge.gov.br/informacoes_sobre_posicionamento_geodesico/rbmc/relatorio/log_sirgas/",
        NetworkBit(ObservationNetwork::Rbmc), false, true, nullptr},
    {"MAGNET", L"https://geodesy.unr.edu/magnet/logs/",
        NetworkBit(ObservationNetwork::Magnet), false, true, nullptr},
    {"PANGA-CWU", L"https://www.panga.org/pub/sites/logs/",
        NetworkBit(ObservationNetwork::PangaCwu), false, true,
        L"https://www.panga.org/data_ftp_pub/sites/logs/"},
    // The following networks do not expose a consistently enumerable public
    // operator log directory.  M3G is their public standardized metadata
    // gateway; keep the network names exactly as advertised by its API.
    {"EPOS-GNSS", L"https://gnss-metadata.eu/v1/sitelog/gnss-metadata",
        NetworkBit(ObservationNetwork::EposGnss), false, false, nullptr, false,
        false, "EPOS", true},
    {"RENAG", L"https://gnss-metadata.eu/v1/sitelog/gnss-metadata",
        NetworkBit(ObservationNetwork::Renag), false, false, nullptr, false,
        false, "RENAG", true},
    {"RING", L"https://gnss-metadata.eu/v1/sitelog/gnss-metadata",
        NetworkBit(ObservationNetwork::Ring), false, false, nullptr, false,
        false, "RING", true},
    {"ROB-BELGIUM", L"https://gnss-metadata.eu/v1/sitelog/gnss-metadata",
        NetworkBit(ObservationNetwork::RobBelgium), false, false, nullptr,
        false, false, "ROB_GNSS", true},
    {"DPGA", L"https://gnss-metadata.eu/v1/sitelog/gnss-metadata",
        NetworkBit(ObservationNetwork::Dpga), false, false, nullptr, false,
        false, "AGRS.NL", true},
    {"CATNET", L"https://gnss-metadata.eu/v1/sitelog/gnss-metadata",
        NetworkBit(ObservationNetwork::CatNet), false, false, nullptr, false,
        false, "CatNet", true},
    {"SPSLUX", L"https://gnss-metadata.eu/v1/sitelog/gnss-metadata",
        NetworkBit(ObservationNetwork::SpsLux), false, false, nullptr, false,
        false, "SPSLUX", true},
    {"NOA-GNSS", L"https://gnss-metadata.eu/v1/sitelog/gnss-metadata",
        NetworkBit(ObservationNetwork::NoaGnss), false, false, nullptr, false,
        false, "NOANET", true},
    {"OS-NET", L"https://gnss-metadata.eu/v1/sitelog/gnss-metadata",
        NetworkBit(ObservationNetwork::OsNet), false, false, nullptr, false,
        false, "OS Net", true},
    {"RENEP", L"https://gnss-metadata.eu/v1/sitelog/gnss-metadata",
        NetworkBit(ObservationNetwork::Renep), false, false, nullptr, false,
        false, "ReNEP", true}
};

const NativeSiteLogDefinition* FindNativeSiteLogDefinition(
    std::string_view requested) {
    std::string key = UpperAscii(Trim(requested));
    if (key == "GEONET" || key == "GEONET/NZ" || key == "GEONET_NZ") {
        key = "GEONET-NZ";
    } else if (key == "SOPAC" || key == "GARNER") {
        key = "SOPAC-GARNER";
    } else if (key == "GA" || key == "APREF") {
        key = "GA-GDC";
    } else if (key == "IGN-ERGNSS" || key == "ERGNSS/IGN") {
        key = "ERGNSS";
    } else if (key == "APOS/BEV" || key == "BEV-APOS") {
        key = "APOS";
    } else if (key == "RBMC" || key == "IBGE" || key == "IBGE/RBMC") {
        key = "IBGE-RBMC";
    } else if (key == "MAGNET/NGL" || key == "NGL") {
        key = "MAGNET";
    } else if (key == "PANGA" || key == "CWU") {
        key = "PANGA-CWU";
    } else if (key == "RAMSAC" || key == "IGN/RAMSAC"
               || key == "IGN_RAMSAC") {
        key = "IGN-RAMSAC";
    } else if (key == "EPOS_IS" || key == "IMO" || key == "ISGPSN") {
        key = "EPOS-IS";
    } else if (key == "EPOS" || key == "EPOSGNSS" || key == "EPOS-GNSS") {
        key = "EPOS-GNSS";
    } else if (key == "ROB" || key == "ROB_GNSS" || key == "ROB-BELGIUM") {
        key = "ROB-BELGIUM";
    } else if (key == "OSNET" || key == "OS NET" || key == "OS-NET") {
        key = "OS-NET";
    } else if (key == "NOA" || key == "NOANET" || key == "NOA-GNSS") {
        key = "NOA-GNSS";
    } else if (key == "RENEP" || key == "RENEP-GNSS") {
        key = "RENEP";
    } else if (key == "CATNET" || key == "CAT-NET") {
        key = "CATNET";
    } else if (key == "SPSLUX" || key == "SPS-LUX") {
        key = "SPSLUX";
    } else if (key == "DPGA/AGRS.NL" || key == "AGRS.NL"
               || key == "DPGA-AGRS.NL") {
        key = "DPGA";
    }
    for (const auto& definition : kNativeSiteLogDefinitions) {
        if (key == definition.key) return &definition;
    }
    return nullptr;
}

const std::vector<std::wstring>& HongKongFallbackLogUrls() {
    // SatRef currently lists 19 CORS/IM stations.  These fixed URLs are a
    // safety net for a transient HTML layout change; newly added links from
    // the page are still preferred and included automatically.
    static const std::vector<std::wstring> urls = [] {
        constexpr const wchar_t* ids[] = {
            L"HKCL", L"HKFN", L"HKKS", L"HKKT", L"HKLM", L"HKLT",
            L"HKMW", L"HKNP", L"HKOH", L"HKPC", L"HKQT", L"HKSC",
            L"HKSL", L"HKSS", L"HKST", L"HKTK", L"HKWS", L"TCHK",
            L"T430"
        };
        std::vector<std::wstring> result;
        for (const auto* id : ids) {
            result.push_back(
                std::wstring(L"https://www.geodetic.gov.hk/common/SiteLog/")
                + id + L"_log.txt");
        }
        return result;
    }();
    return urls;
}

// ---- Time-aware receiver/antenna history ---------------------------------
//
// IGS site logs are deceptively simple text files.  The current equipment is
// not represented by one special field: every numbered 3.x/4.x block is an
// interval, and older logs use a compact ``Type``/``Date`` pair.  Keep this
// parser deliberately independent from the scalar snapshot parser below so
// adding history cannot change the long-standing interpretation of legacy
// fields.

std::string NormalizeEquipmentDate(std::string value) {
    value = Trim(value);
    if (value.empty()) return {};

    // Strip wrapping punctuation before testing for a template placeholder.
    // A few otherwise valid logs write dates as ``(2020-01-02)``; checking
    // IsPlaceholder first would incorrectly discard those dates.
    while (!value.empty()
        && (value.front() == '(' || value.front() == '['
            || value.front() == '{')) value.erase(value.begin());
    while (!value.empty()
        && (value.back() == ')' || value.back() == ']'
            || value.back() == '}')) value.pop_back();
    value = Trim(value);
    if (value.empty() || IsPlaceholder(value)) return {};

    std::string lower = LowerAscii(value);
    if (lower.find("present") != std::string::npos
        || lower.find("current") != std::string::npos
        || lower.find("ongoing") != std::string::npos
        || lower.find("now") != std::string::npos) {
        return {};
    }
    static const std::regex isoPattern(
        R"((\d{4})[-/]([0-9]{1,2})[-/]([0-9]{1,2}))",
        std::regex::optimize);
    std::smatch iso;
    if (std::regex_search(value, iso, isoPattern)) {
        const int year = std::stoi(iso[1].str());
        const int month = std::stoi(iso[2].str());
        const int day = std::stoi(iso[3].str());
        if (year >= 1900 && year <= 2200 && month >= 1 && month <= 12
            && day >= 1 && day <= 31) {
            return std::format("{:04}-{:02}-{:02}", year, month, day);
        }
    }

    static const std::array<const char*, 12> months{
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    static const std::regex textPattern(
        R"((\d{1,2})[-/ ]([A-Za-z]{3})[-/ ](\d{4}|\d{2})(?!\d))",
        std::regex::icase | std::regex::optimize);
    std::smatch textual;
    if (std::regex_search(value, textual, textPattern)) {
        const int day = std::stoi(textual[1].str());
        std::string monthText = UpperAscii(textual[2].str());
        int month = 0;
        for (std::size_t index = 0; index < months.size(); ++index) {
            if (monthText == months[index]) {
                month = static_cast<int>(index) + 1;
                break;
            }
        }
        int year = std::stoi(textual[3].str());
        if (year < 100) year = year <= 30 ? 2000 + year : 1900 + year;
        if (month != 0 && day >= 1 && day <= 31
            && year >= 1900 && year <= 2200) {
            return std::format("{:04}-{:02}-{:02}", year, month, day);
        }
    }
    return {};
}

std::pair<std::string, std::string> EquipmentDateRange(
    const std::string_view value) {
    std::vector<std::string> dates;
    // Pull only recognizable date tokens; this avoids interpreting serial
    // numbers or firmware versions as dates.
    static const std::regex tokenPattern(
        R"((\d{4}[-/]\d{1,2}[-/]\d{1,2}|\d{1,2}[-/ ]?[A-Za-z]{3}[-/ ]?\d{2,4}))",
        std::regex::icase | std::regex::optimize);
    const std::string copy(value);
    for (std::sregex_iterator it(copy.begin(), copy.end(), tokenPattern), end;
         it != end; ++it) {
        const std::string normalized = NormalizeEquipmentDate(it->str());
        if (!normalized.empty()) dates.push_back(normalized);
    }
    if (dates.empty()) {
        return {NormalizeEquipmentDate(copy), {}};
    }
    // Preserve the first occurrence and only use a second distinct token as
    // the removal date.  A timestamp can contain the same date twice in a
    // malformed log; treating it as an open interval is safer than creating
    // a zero-length epoch.
    const std::string removed = dates.size() > 1 && dates[1] != dates[0]
        ? dates[1] : std::string{};
    return {dates[0], removed};
}

bool EquipmentHasData(const SiteLogEquipmentEpoch& epoch) {
    return !epoch.type.empty() || !epoch.systems.empty()
        || !epoch.serial.empty() || !epoch.firmware.empty()
        || !epoch.radome.empty() || !epoch.referencePoint.empty()
        || !epoch.dateInstalled.empty() || !epoch.dateRemoved.empty();
}

std::vector<SiteLogEquipmentEpoch> ParseEquipmentHistoryImpl(
    const std::string_view text) {
    std::vector<SiteLogEquipmentEpoch> result;
    enum class Section { None, Receiver, Antenna };
    Section section = Section::None;
    SiteLogEquipmentEpoch current;
    int nextReceiverSequence = 1;
    int nextAntennaSequence = 1;

    const auto flush = [&]() {
        if (!EquipmentHasData(current)) {
            current = SiteLogEquipmentEpoch{};
            return;
        }
        if (current.sequence <= 0) {
            current.sequence = current.kind == "antenna"
                ? nextAntennaSequence++ : nextReceiverSequence++;
        } else if (current.kind == "antenna") {
            nextAntennaSequence = std::max(
                nextAntennaSequence, current.sequence + 1);
        } else {
            nextReceiverSequence = std::max(
                nextReceiverSequence, current.sequence + 1);
        }
        result.push_back(std::move(current));
        current = SiteLogEquipmentEpoch{};
    };
    const auto startBlock = [&](const Section newSection, const int sequence) {
        flush();
        section = newSection;
        current.kind = newSection == Section::Antenna
            ? "antenna" : "receiver";
        current.sequence = sequence;
    };

    std::istringstream stream{std::string(text)};
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string lower = LowerAscii(line);
        const std::string heading = CollapseWhitespace(lower);
        const bool sectionThree = heading.starts_with("3.")
            && (heading.find("receiver") != std::string::npos
                || heading.find("gps") != std::string::npos
                || heading.find("gnss") != std::string::npos);
        const bool sectionFour = heading.starts_with("4.")
            && (heading.find("antenna") != std::string::npos
                || heading.find("gps") != std::string::npos
                || heading.find("gnss") != std::string::npos);
        // A standard heading is usually ``3. GNSS Receiver Information``;
        // older forms use ``3. GPS Receiver(s):`` and M3G/GeodesyML-derived
        // summaries often use the shorter ``3. GNSS Receiver``.  Recognize
        // all of them, but do not mistake a numbered field such as
        // ``3.1 Receiver Type:`` for a section heading.
        static const std::regex numberedSectionField(
            R"(^[34]\.\s*[0-9]+(?:\.|\s|$))",
            std::regex::icase | std::regex::optimize);
        const bool numberedField = std::regex_search(
            heading, numberedSectionField);
        const bool receiverHeading = sectionThree && !numberedField;
        const bool antennaHeading = sectionFour && !numberedField;
        if (receiverHeading) {
            flush();
            section = Section::Receiver;
        } else if (antennaHeading) {
            flush();
            section = Section::Antenna;
        } else if (heading.starts_with("5.") || heading.starts_with("6.")
                   || heading.starts_with("7.") || heading.starts_with("8.")
                   || heading.starts_with("9.") || heading.starts_with("10.")
                   || heading.starts_with("11.") || heading.starts_with("12.")
                   || heading.starts_with("13.")) {
            flush();
            section = Section::None;
        }

        std::smatch blockMatch;
        static const std::regex numberedBlock(
            R"(^\s*([34])\.([0-9]+)(?:\.[0-9]+)?\b)",
            std::regex::icase | std::regex::optimize);
        if (std::regex_search(line, blockMatch, numberedBlock)) {
            const Section numberedSection = blockMatch[1].str() == "4"
                ? Section::Antenna : Section::Receiver;
            const int sequence = std::stoi(blockMatch[2].str());
            startBlock(numberedSection, sequence);
        }

        const auto colon = line.find(':');
        if (colon == std::string::npos || section == Section::None) continue;
        const std::string label = LabelBeforeColon(line);
        const std::string value = ValueAfterColon(line);
        if (value.empty() || IsPlaceholder(value)) continue;

        // Legacy (pre-v2) logs have no 3.1/4.1 prefix and use repeated
        // ``Type`` lines.  Treat a second type as the start of another epoch.
        const bool receiverType = section == Section::Receiver
            && (Contains(label, "receiver type") || label == "type");
        const bool antennaType = section == Section::Antenna
            && (Contains(label, "antenna type") || label == "type");
        if ((receiverType || antennaType)
            && !current.type.empty()
            && ((receiverType && current.kind == "receiver")
                || (antennaType && current.kind == "antenna"))) {
            startBlock(section, 0);
        }
        if (current.kind.empty()) {
            current.kind = section == Section::Antenna ? "antenna" : "receiver";
        }
        if (receiverType && current.kind == "receiver") {
            current.type = value;
        } else if (section == Section::Receiver
                   && (Contains(label, "satellite system")
                       || label == "system")) {
            current.systems = value;
        } else if (section == Section::Receiver
                   && Contains(label, "serial number")) {
            current.serial = value;
        } else if (section == Section::Receiver
                   && Contains(label, "firmware version")) {
            current.firmware = value;
        } else if (antennaType && current.kind == "antenna") {
            current.type = value;
        } else if (section == Section::Antenna
                   && (Contains(label, "radome type")
                       || label == "radome")) {
            current.radome = value;
        } else if (section == Section::Antenna
                   && Contains(label, "serial number")
                   && !Contains(label, "radome")) {
            current.serial = value;
        } else if (section == Section::Antenna
                   && (Contains(label, "reference point")
                       || Contains(label, "ref. point"))) {
            current.referencePoint = value;
        }

        const bool installedLabel = Contains(label, "date installed");
        const bool removedLabel = Contains(label, "date removed");
        const bool legacyDate = label == "date";
        if ((installedLabel || removedLabel || legacyDate)
            && ((section == Section::Receiver && current.kind == "receiver")
                || (section == Section::Antenna && current.kind == "antenna"))) {
            const auto dates = EquipmentDateRange(value);
            if (legacyDate) {
                if (current.dateInstalled.empty()) current.dateInstalled = dates.first;
                if (!dates.second.empty()) current.dateRemoved = dates.second;
            } else if (removedLabel) {
                current.dateRemoved = dates.first.empty() ? dates.second : dates.first;
            } else {
                current.dateInstalled = dates.first;
                if (!dates.second.empty()) current.dateRemoved = dates.second;
            }
        }
    }
    flush();
    return result;
}

std::vector<SiteLogEquipmentEpoch> ParseGeodesyMlEquipmentHistory(
    const std::string_view xml) {
    std::vector<SiteLogEquipmentEpoch> result;
    // Most archives contain plain ASCII Site Logs.  Avoid constructing and
    // running two regular expressions over every such document during a
    // migration; only GA/M3G payloads carrying GeodesyML need XML parsing.
    if (xml.find("GnssReceiver") == std::string_view::npos
        && xml.find("GnssAntenna") == std::string_view::npos) {
        return result;
    }
    auto receiverBlocks = XmlBlocksExact(xml, "GnssReceiver");
    if (receiverBlocks.empty()) {
        receiverBlocks = XmlBlocks(xml, "GnssReceiver");
    }
    int receiverSequence = 1;
    for (const auto& block : receiverBlocks) {
        SiteLogEquipmentEpoch epoch;
        epoch.kind = "receiver";
        epoch.sequence = receiverSequence++;
        epoch.type = Trim(XmlText(block, "igsModelCode"));
        if (epoch.type.empty()) epoch.type = Trim(XmlText(block, "receiverType"));
        epoch.systems = JoinUniqueXmlValues(XmlTexts(block, "satelliteSystem"));
        epoch.serial = Trim(XmlText(block, "manufacturerSerialNumber"));
        epoch.firmware = Trim(XmlText(block, "firmwareVersion"));
        epoch.dateInstalled = Trim(XmlText(block, "dateInstalled"));
        epoch.dateRemoved = Trim(XmlText(block, "dateRemoved"));
        if (!epoch.type.empty() || !epoch.systems.empty()
            || !epoch.serial.empty() || !epoch.firmware.empty()
            || !epoch.dateInstalled.empty() || !epoch.dateRemoved.empty()) {
            result.push_back(std::move(epoch));
        }
    }

    auto antennaBlocks = XmlBlocksExact(xml, "GnssAntenna");
    if (antennaBlocks.empty()) {
        antennaBlocks = XmlBlocks(xml, "GnssAntenna");
    }
    int antennaSequence = 1;
    for (const auto& block : antennaBlocks) {
        SiteLogEquipmentEpoch epoch;
        epoch.kind = "antenna";
        epoch.sequence = antennaSequence++;
        epoch.type = Trim(XmlText(block, "igsModelCode"));
        if (epoch.type.empty()) epoch.type = Trim(XmlText(block, "type"));
        epoch.radome = Trim(XmlText(block, "antennaRadomeType"));
        epoch.serial = Trim(XmlText(block, "manufacturerSerialNumber"));
        epoch.referencePoint = Trim(XmlText(block, "antennaReferencePoint"));
        epoch.dateInstalled = Trim(XmlText(block, "dateInstalled"));
        epoch.dateRemoved = Trim(XmlText(block, "dateRemoved"));
        if (!epoch.type.empty() || !epoch.radome.empty()
            || !epoch.serial.empty() || !epoch.referencePoint.empty()
            || !epoch.dateInstalled.empty() || !epoch.dateRemoved.empty()) {
            result.push_back(std::move(epoch));
        }
    }
    return result;
}

}  // namespace

bool SiteLogEcefToGeodetic(
    const double x,
    const double y,
    const double z,
    double& latitude,
    double& longitude,
    double& elevation) {
    // WGS-84 semi-major axis and eccentricity squared.  Site Logs normally
    // contain ITRF coordinates in metres; reject values outside a generous
    // Earth-sized shell so a malformed/empty field can never create a point
    // at (0,0) or an absurd map location.
    constexpr double a = 6378137.0;
    constexpr double e2 = 6.6943799901413165e-3;
    constexpr double b = 6356752.314245179;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return false;
    }
    const double p = std::hypot(x, y);
    const double radius = std::hypot(p, z);
    if (!std::isfinite(radius) || radius < 6.0e6 || radius > 7.5e6) {
        return false;
    }
    longitude = std::atan2(y, x) * 180.0 / 3.14159265358979323846;
    if (p < 1.0e-9) {
        latitude = z < 0.0 ? -90.0 : 90.0;
        elevation = std::abs(z) - b;
        return std::isfinite(elevation);
    }
    // Bowring's closed-form initial value followed by a few fixed-point
    // iterations.  This is stable for polar and ordinary stations alike and
    // avoids pulling a geodesy dependency into the downloader.
    constexpr double ep2 = (a * a - b * b) / (b * b);
    const double theta = std::atan2(a * z, b * p);
    const double st = std::sin(theta);
    const double ct = std::cos(theta);
    double phi = std::atan2(
        z + ep2 * b * st * st * st,
        p - e2 * a * ct * ct * ct);
    double h = 0.0;
    for (int iteration = 0; iteration < 8; ++iteration) {
        const double sinPhi = std::sin(phi);
        const double cosPhi = std::cos(phi);
        const double n = a / std::sqrt(1.0 - e2 * sinPhi * sinPhi);
        h = p / std::max(std::abs(cosPhi), 1.0e-15) - n;
        const double next = std::atan2(
            z, p * (1.0 - e2 * n / (n + h)));
        if (std::abs(next - phi) < 1.0e-13) {
            phi = next;
            break;
        }
        phi = next;
    }
    const double sinPhi = std::sin(phi);
    const double cosPhi = std::cos(phi);
    const double n = a / std::sqrt(1.0 - e2 * sinPhi * sinPhi);
    h = std::abs(cosPhi) > 1.0e-12
        ? p / cosPhi - n
        : z / std::max(std::abs(sinPhi), 1.0e-15)
            - n * (1.0 - e2);
    latitude = phi * 180.0 / 3.14159265358979323846;
    elevation = h;
    return std::isfinite(latitude) && std::isfinite(longitude)
        && std::isfinite(elevation) && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0;
}

std::vector<SiteLogEquipmentEpoch> ParseSiteLogEquipmentHistory(
    const std::string_view text) {
    // GA GeodesyML payloads are commonly appended to a short human-readable
    // summary in raw_text.  Prefer the XML timeline when present; parsing the
    // summary first would otherwise retain only the current snapshot and
    // hide older receiver/antenna blocks during a backfill.
    const auto geodesyMl = ParseGeodesyMlEquipmentHistory(text);
    if (!geodesyMl.empty()) return geodesyMl;
    return ParseEquipmentHistoryImpl(text);
}

void ApplySiteLogEquipmentAsOf(
    SiteLogRecord& record,
    const std::string_view asOfDate) {
    // Keep this small public wrapper next to the parser/persistence API.  The
    // actual selection rules live in ApplyEffectiveEquipment above and are
    // shared by LoadSiteLogSummaryMap; exposing the wrapper lets the GUI
    // reuse the exact same installed/removed-date semantics for each daily
    // row in a date-range query without reopening SQLite for every row.
    ApplyEffectiveEquipment(record, asOfDate);
}

void ApplySiteLogEquipmentAsOf(
    const SiteLogRecord& source,
    const std::string_view asOfDate,
    SiteLogRecord& target) {
    // The two-argument overload is intentionally allowed to mutate its
    // record in place.  This overload is for the range-query worker, where a
    // SiteLogRecord can carry a large equipmentHistory vector; assigning the
    // source wholesale here would deep-copy that vector once per observation
    // day.  Copy only the scalar/provenance fields that the GUI consumes and
    // select the effective equipment directly from source.equipmentHistory.
    if (&source == &target) {
        ApplySiteLogEquipmentAsOf(target, asOfDate);
        return;
    }

    target = SiteLogRecord{};
    target.stationId = source.stationId;
    target.fourCharId = source.fourCharId;
    target.domes = source.domes;
    target.siteName = source.siteName;
    target.city = source.city;
    target.country = source.country;
    target.x = source.x;
    target.y = source.y;
    target.z = source.z;
    target.latitude = source.latitude;
    target.longitude = source.longitude;
    target.elevation = source.elevation;
    target.hasXyz = source.hasXyz;
    target.hasCoordinate = source.hasCoordinate;

    // Start with the source scalar snapshot.  If a dated equipment history
    // has no active interval for the requested date, the same clearing rules
    // as ApplyEffectiveEquipment below replace these fields with empty
    // values.  For logs without dated history this preserves the legacy
    // scalar-only behaviour.
    target.receiverType = source.receiverType;
    target.receiverSystems = source.receiverSystems;
    target.receiverSerial = source.receiverSerial;
    target.receiverFirmware = source.receiverFirmware;
    target.receiverDateInstalled = source.receiverDateInstalled;
    target.antennaType = source.antennaType;
    target.antennaRadome = source.antennaRadome;
    target.antennaSerial = source.antennaSerial;
    target.antennaReferencePoint = source.antennaReferencePoint;
    target.antennaDateInstalled = source.antennaDateInstalled;
    target.agency = source.agency;
    target.operatorName = source.operatorName;
    target.networkMask = source.networkMask;
    target.sourceDirectory = source.sourceDirectory;
    target.sourceUrl = source.sourceUrl;
    target.fileName = source.fileName;
    target.revisionDate = source.revisionDate;
    target.fetchedAtUtc = source.fetchedAtUtc;
    // rawText and equipmentHistory deliberately remain empty.

    ApplyEffectiveEquipmentFromHistory(
        target, source.equipmentHistory, asOfDate);
}

SiteLogRecord ParseSiteLogText(
    const std::string_view text,
    const std::string_view sourceUrl,
    const std::string_view sourceDirectory,
    const std::uint64_t networkMask) {
    SiteLogRecord record;
    record.sourceUrl = std::string(sourceUrl);
    record.sourceDirectory = std::string(sourceDirectory);
    record.fileName = FileNameFromUrl(sourceUrl);
    record.networkMask = networkMask;
    InferIdsFromFileName(record.fileName, record.stationId, record.fourCharId);
    record.revisionDate = RevisionDateFromFileName(record.fileName);

    enum class Section { None, Site, Location, Receiver, Antenna, Operator };
    Section section = Section::None;
    double latitude = 0.0;
    double longitude = 0.0;
    bool hasLatitude = false;
    bool hasLongitude = false;
    bool hasX = false;
    bool hasY = false;
    bool hasZ = false;

    std::istringstream stream{std::string(text)};
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string lower = LowerAscii(line);
        const std::string heading = CollapseWhitespace(lower);
        if (heading.find("1. site identification") != std::string::npos) {
            section = Section::Site;
        } else if (heading.find("2. site location") != std::string::npos) {
            section = Section::Location;
        } else if (heading.find("3. gnss receiver") != std::string::npos) {
            section = Section::Receiver;
        } else if (heading.find("4. gnss antenna") != std::string::npos) {
            section = Section::Antenna;
        } else if (heading.find("11. on-site") != std::string::npos) {
            section = Section::Operator;
        } else if (heading.starts_with("5.")
            || heading.starts_with("6.")
            || heading.starts_with("7.")
            || heading.starts_with("8.")
            || heading.starts_with("9.")
            || heading.starts_with("10.")) {
            // Do not let receiver/antenna context leak into later sections;
            // otherwise an Agency or Contact Name in section 11 can be
            // mistaken for an antenna field in logs with a three-space title.
            section = Section::None;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string label = LabelBeforeColon(line);
        const std::string value = ValueAfterColon(line);
        if (value.empty() || IsPlaceholder(value)) continue;

        if (Contains(label, "9-character id")
            || Contains(label, "9 character id")
            || Contains(label, "nine character id")) {
            std::string id;
            for (const unsigned char character : value) {
                if (std::isalnum(character) != 0) id.push_back(
                    static_cast<char>(character));
            }
            if (id.size() == 9) record.stationId = UpperAscii(id);
            continue;
        }
        if (Contains(label, "four character id")) {
            if (record.fourCharId.empty()) record.fourCharId = UpperAscii(value);
            continue;
        }
        if (Contains(label, "iers domes") || Contains(label, "domes number")) {
            if (record.domes.empty()) record.domes = value;
            continue;
        }
        if (Contains(label, "site name") || Contains(label, "station name")) {
            if (record.siteName.empty()) record.siteName = value;
            continue;
        }
        if (Contains(label, "city or town")) {
            if (record.city.empty()) record.city = value;
            continue;
        }
        if (label == "country" || Contains(label, "country or region")) {
            if (record.country.empty()) record.country = value;
            continue;
        }
        if (Contains(label, "x coordinate")) {
            if (ParseDouble(value, record.x)) hasX = true;
            continue;
        }
        if (Contains(label, "y coordinate")) {
            if (ParseDouble(value, record.y)) hasY = true;
            continue;
        }
        if (Contains(label, "z coordinate")) {
            if (ParseDouble(value, record.z)) hasZ = true;
            continue;
        }
        if (Contains(label, "latitude")) {
            hasLatitude = ParseCoordinate(value, true, latitude);
            continue;
        }
        if (Contains(label, "longitude")) {
            hasLongitude = ParseCoordinate(value, false, longitude);
            continue;
        }
        // The receiver section also contains "Elevation Cutoff Setting".
        // That is an angular mask, not the monument height; do not let it
        // overwrite the geodetic elevation parsed from section 2.
        if (Contains(label, "elevation")
            && !Contains(label, "cutoff")) {
            ParseDouble(value, record.elevation);
            continue;
        }

        const bool receiverField = section == Section::Receiver
            || label.starts_with("3.");
        const bool antennaField = section == Section::Antenna
            || label.starts_with("4.");
        if (receiverField) {
            if (Contains(label, "receiver type")) record.receiverType = value;
            else if (Contains(label, "satellite system")) record.receiverSystems = value;
            else if (Contains(label, "serial number")) record.receiverSerial = value;
            else if (Contains(label, "firmware version")) record.receiverFirmware = value;
            else if (Contains(label, "date installed")) record.receiverDateInstalled = value;
            continue;
        }
        if (antennaField) {
            if (Contains(label, "antenna type")) record.antennaType = value;
            else if (Contains(label, "radome type")) record.antennaRadome = value;
            else if (Contains(label, "serial number")) record.antennaSerial = value;
            else if (Contains(label, "reference point")) record.antennaReferencePoint = value;
            else if (Contains(label, "date installed")) record.antennaDateInstalled = value;
            continue;
        }
        if (Contains(label, "responsible agency")
            || label == "agency" || Contains(label, "site owner")) {
            if (record.agency.empty()) record.agency = value;
            continue;
        }
        if (label == "operator" || Contains(label, "operator name")
            || (section == Section::Operator
                && (Contains(label, "contact name")
                    || Contains(label, "primary contact")))) {
            if (record.operatorName.empty()) record.operatorName = value;
            continue;
        }
        if (Contains(label, "date prepared")) {
            static const std::regex datePattern(R"(([0-9]{4})-([0-9]{2})-([0-9]{2}))");
            std::smatch match;
            if (std::regex_search(value, match, datePattern)) {
                record.revisionDate = match[0].str();
            }
        }
    }
    if (record.fourCharId.empty() && record.stationId.size() >= 4) {
        record.fourCharId = record.stationId.substr(0, 4);
    }
    ApplyOrganizationRoles(record,text);
    if (hasX && hasY && hasZ) record.hasXyz = true;
    if (hasLatitude && hasLongitude) {
        record.latitude = latitude;
        record.longitude = longitude;
        record.hasCoordinate = true;
    }
    if (!record.hasCoordinate && record.hasXyz) {
        double derivedLatitude = 0.0;
        double derivedLongitude = 0.0;
        double derivedElevation = 0.0;
        if (SiteLogEcefToGeodetic(
                record.x, record.y, record.z,
                derivedLatitude, derivedLongitude, derivedElevation)) {
            record.latitude = derivedLatitude;
            record.longitude = derivedLongitude;
            // Keep an explicitly supplied geodetic height; otherwise use the
            // ellipsoidal height derived from the Cartesian coordinates.
            if (!std::isfinite(record.elevation) || record.elevation == 0.0) {
                record.elevation = derivedElevation;
            }
            record.hasCoordinate = true;
        }
    }
    record.rawText = std::string(text);
    record.equipmentHistory = ParseSiteLogEquipmentHistory(text);
    for (auto& epoch : record.equipmentHistory) {
        epoch.stationId = record.stationId;
        epoch.fourCharId = record.fourCharId;
        epoch.sourceUrl = record.sourceUrl;
        epoch.revisionDate = record.revisionDate;
        epoch.fetchedAtUtc = record.fetchedAtUtc;
        epoch.networkMask = record.networkMask;
    }
    if (record.fetchedAtUtc.empty()) record.fetchedAtUtc = CurrentUtc();
    // The provenance timestamp is normally filled above by the parser.  Make
    // sure history rows created from a text document receive the same value
    // even when the caller supplied no fetch timestamp.
    if (!record.fetchedAtUtc.empty()) {
        for (auto& epoch : record.equipmentHistory) {
            if (epoch.fetchedAtUtc.empty()) epoch.fetchedAtUtc = record.fetchedAtUtc;
        }
    }
    return record;
}

bool EnsureSiteLogSchema(
    const std::filesystem::path& databasePath,
    std::wstring& error) {
    error.clear();
    DatabasePtr database;
    if (!OpenDatabase(databasePath, database, error)) return false;
    constexpr const char* schema =
        "CREATE TABLE IF NOT EXISTS site_logs("
        "source_url TEXT PRIMARY KEY NOT NULL,"
        "station_id TEXT NOT NULL DEFAULT '',"
        "four_char_id TEXT NOT NULL DEFAULT '',"
        "domes TEXT NOT NULL DEFAULT '',"
        "site_name TEXT NOT NULL DEFAULT '',"
        "city TEXT NOT NULL DEFAULT '',"
        "country TEXT NOT NULL DEFAULT '',"
        "x REAL NOT NULL DEFAULT 0,"
        "y REAL NOT NULL DEFAULT 0,"
        "z REAL NOT NULL DEFAULT 0,"
        "has_xyz INTEGER NOT NULL DEFAULT 0,"
        "latitude REAL NOT NULL DEFAULT 0,"
        "longitude REAL NOT NULL DEFAULT 0,"
        "elevation REAL NOT NULL DEFAULT 0,"
        "has_coordinate INTEGER NOT NULL DEFAULT 0,"
        "receiver_type TEXT NOT NULL DEFAULT '',"
        "receiver_systems TEXT NOT NULL DEFAULT '',"
        "receiver_serial TEXT NOT NULL DEFAULT '',"
        "receiver_firmware TEXT NOT NULL DEFAULT '',"
        "receiver_date_installed TEXT NOT NULL DEFAULT '',"
        "antenna_type TEXT NOT NULL DEFAULT '',"
        "antenna_radome TEXT NOT NULL DEFAULT '',"
        "antenna_serial TEXT NOT NULL DEFAULT '',"
        "antenna_reference_point TEXT NOT NULL DEFAULT '',"
        "antenna_date_installed TEXT NOT NULL DEFAULT '',"
        "agency TEXT NOT NULL DEFAULT '',"
        "operator_name TEXT NOT NULL DEFAULT '',"
        "network_mask INTEGER NOT NULL DEFAULT 0,"
        "source_directory TEXT NOT NULL DEFAULT '',"
        "file_name TEXT NOT NULL DEFAULT '',"
        "revision_date TEXT NOT NULL DEFAULT '',"
        "fetched_at_utc TEXT NOT NULL DEFAULT '',"
        "raw_text TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE INDEX IF NOT EXISTS site_logs_station_idx"
        " ON site_logs(station_id);"
        "CREATE INDEX IF NOT EXISTS site_logs_four_char_idx"
        " ON site_logs(four_char_id);"
        "CREATE INDEX IF NOT EXISTS site_logs_revision_idx"
        " ON site_logs(revision_date);"
        "CREATE INDEX IF NOT EXISTS site_logs_network_idx"
        " ON site_logs(network_mask);"
        // One row per receiver/antenna installation interval.  Keep this
        // separate from site_logs so existing readers and old databases stay
        // binary/schema compatible while temporal queries can use narrow,
        // indexed columns instead of reparsing raw_text.
        "CREATE TABLE IF NOT EXISTS site_log_equipment("
        "source_url TEXT NOT NULL DEFAULT '',"
        "station_id TEXT NOT NULL DEFAULT '',"
        "four_char_id TEXT NOT NULL DEFAULT '',"
        "equipment_kind TEXT NOT NULL," // receiver / antenna
        "sequence INTEGER NOT NULL DEFAULT 0,"
        "equipment_type TEXT NOT NULL DEFAULT '',"
        "systems TEXT NOT NULL DEFAULT '',"
        "serial TEXT NOT NULL DEFAULT '',"
        "firmware TEXT NOT NULL DEFAULT '',"
        "radome TEXT NOT NULL DEFAULT '',"
        "reference_point TEXT NOT NULL DEFAULT '',"
        "date_installed TEXT NOT NULL DEFAULT '',"
        "date_removed TEXT NOT NULL DEFAULT '',"
        "revision_date TEXT NOT NULL DEFAULT '',"
        "fetched_at_utc TEXT NOT NULL DEFAULT '',"
        "network_mask INTEGER NOT NULL DEFAULT 0,"
        "PRIMARY KEY(source_url,equipment_kind,sequence)"
        ");"
        "CREATE INDEX IF NOT EXISTS site_log_equipment_station_idx"
        " ON site_log_equipment(station_id);"
        "CREATE INDEX IF NOT EXISTS site_log_equipment_four_char_idx"
        " ON site_log_equipment(four_char_id);"
        "CREATE INDEX IF NOT EXISTS site_log_equipment_dates_idx"
        " ON site_log_equipment(date_installed,date_removed);"
        "CREATE INDEX IF NOT EXISTS site_log_equipment_network_idx"
        " ON site_log_equipment(network_mask);"
        // A conservative station-identity bridge used when an observation
        // archive and a Site Log use different four/nine-character IDs (or a
        // documented historical alias).  The referenced source_url remains
        // the original Site Log row; aliases never duplicate raw_text.
        "CREATE TABLE IF NOT EXISTS site_log_aliases("
        "alias_key TEXT PRIMARY KEY NOT NULL,"
        "source_url TEXT NOT NULL,"
        "match_method TEXT NOT NULL DEFAULT '',"
        "distance_km REAL NOT NULL DEFAULT 0,"
        "evidence TEXT NOT NULL DEFAULT '',"
        "created_at_utc TEXT NOT NULL DEFAULT '',"
        "network_mask INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS site_log_aliases_source_idx"
        " ON site_log_aliases(source_url);"
        "CREATE INDEX IF NOT EXISTS site_log_aliases_network_idx"
        " ON site_log_aliases(network_mask);";
    return Execute(database.get(), schema, error);
}

bool StoreSiteLogRecords(
    const std::filesystem::path& databasePath,
    const std::vector<SiteLogRecord>& records,
    std::wstring& error) {
    error.clear();
    if (!EnsureSiteLogSchema(databasePath, error)) return false;
    if (records.empty()) return true;

    DatabasePtr database;
    if (!OpenDatabase(databasePath, database, error)) return false;
    if (!Execute(database.get(), "BEGIN IMMEDIATE;", error)) return false;

    constexpr const char* sql =
        "INSERT OR REPLACE INTO site_logs("
        "source_url,station_id,four_char_id,domes,site_name,city,country,"
        "x,y,z,has_xyz,latitude,longitude,elevation,has_coordinate,"
        "receiver_type,receiver_systems,receiver_serial,receiver_firmware,"
        "receiver_date_installed,antenna_type,antenna_radome,antenna_serial,"
        "antenna_reference_point,antenna_date_installed,agency,operator_name,"
        "network_mask,source_directory,file_name,revision_date,fetched_at_utc,raw_text"
        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* rawStatement = nullptr;
    if (sqlite3_prepare_v2(database.get(), sql, -1, &rawStatement, nullptr)
        != SQLITE_OK) {
        error = SqliteError(database.get(), L"无法准备 Site Log 写入");
        Execute(database.get(), "ROLLBACK;", error);
        return false;
    }
    StatementPtr statement(rawStatement);
    auto deleteHistory = Prepare(
        database.get(),
        "DELETE FROM site_log_equipment WHERE source_url=?;",
        error);
    auto insertHistory = Prepare(
        database.get(),
        "INSERT OR REPLACE INTO site_log_equipment("
        "source_url,station_id,four_char_id,equipment_kind,sequence,"
        "equipment_type,systems,serial,firmware,radome,reference_point,"
        "date_installed,date_removed,revision_date,fetched_at_utc,network_mask"
        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        error);
    if (!deleteHistory || !insertHistory) {
        Execute(database.get(), "ROLLBACK;", error);
        return false;
    }
    for (const auto& original : records) {
        SiteLogRecord record = original;
        // Normalize legacy Cartesian-only records before persistence.  This
        // also upgrades logs supplied by JSON/XML adapters, not just the
        // ASCII parser, and makes the coordinate available to every later
        // central-index rebuild.
        if ((!record.hasCoordinate
                || !std::isfinite(record.latitude)
                || !std::isfinite(record.longitude)
                || record.latitude < -90.0 || record.latitude > 90.0
                || record.longitude < -180.0 || record.longitude > 180.0)
            && record.hasXyz) {
            double latitude = 0.0;
            double longitude = 0.0;
            double elevation = 0.0;
            if (SiteLogEcefToGeodetic(
                    record.x, record.y, record.z,
                    latitude, longitude, elevation)) {
                record.latitude = latitude;
                record.longitude = longitude;
                if (!std::isfinite(record.elevation) || record.elevation == 0.0) {
                    record.elevation = elevation;
                }
                record.hasCoordinate = true;
            }
        }
        // Callers that construct SiteLogRecord manually (and old migration
        // paths) may not have populated equipmentHistory.  Parse raw_text at
        // write time so every newly stored document gets the same temporal
        // representation as records fetched by the builder.
        std::vector<SiteLogEquipmentEpoch> history = record.equipmentHistory;
        if (history.empty() && !record.rawText.empty()) {
            history = ParseSiteLogEquipmentHistory(record.rawText);
        }
        NormalizeEquipmentProvenance(record, history);
        record.equipmentHistory = history;
        sqlite3_reset(statement.get());
        sqlite3_clear_bindings(statement.get());
        if (!BindRecord(statement.get(), record)) {
            error = SqliteError(database.get(), L"绑定 Site Log 字段失败");
            Execute(database.get(), "ROLLBACK;", error);
            return false;
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            error = SqliteError(database.get(), L"写入 Site Log 失败");
            Execute(database.get(), "ROLLBACK;", error);
            return false;
        }
        // A refreshed document may have fewer blocks than its previous
        // version.  Delete all old rows for this URL first, then insert the
        // current set, making the operation an exact replacement.
        sqlite3_reset(deleteHistory.get());
        sqlite3_clear_bindings(deleteHistory.get());
        if (!BindText(deleteHistory.get(), 1, record.sourceUrl)
            || sqlite3_step(deleteHistory.get()) != SQLITE_DONE) {
            error = SqliteError(database.get(), L"清理 Site Log 旧设备履历失败");
            Execute(database.get(), "ROLLBACK;", error);
            return false;
        }
        for (const auto& epoch : record.equipmentHistory) {
            if ((epoch.kind != "receiver" && epoch.kind != "antenna")
                || !EquipmentHasData(epoch)) {
                continue;
            }
            sqlite3_reset(insertHistory.get());
            sqlite3_clear_bindings(insertHistory.get());
            if (!BindEquipmentEpoch(insertHistory.get(), epoch)
                || sqlite3_step(insertHistory.get()) != SQLITE_DONE) {
                error = SqliteError(database.get(), L"写入 Site Log 设备履历失败");
                Execute(database.get(), "ROLLBACK;", error);
                return false;
            }
        }
    }
    if (!Execute(database.get(), "COMMIT;", error)) {
        Execute(database.get(), "ROLLBACK;", error);
        return false;
    }
    return true;
}

bool LoadSiteLogRecords(
    const std::filesystem::path& databasePath,
    std::vector<SiteLogRecord>& records,
    std::wstring& error) {
    error.clear();
    records.clear();
    if (!EnsureSiteLogSchema(databasePath, error)) return false;
    DatabasePtr database;
    if (!OpenDatabase(databasePath, database, error)) return false;
    const std::string sql = "SELECT " + std::string(SiteLogSelectColumns())
        + " FROM site_logs ORDER BY station_id,four_char_id,revision_date;";
    sqlite3_stmt* rawStatement = nullptr;
    if (sqlite3_prepare_v2(database.get(), sql.c_str(), -1, &rawStatement, nullptr)
        != SQLITE_OK) {
        error = SqliteError(database.get(), L"无法准备 Site Log 查询");
        return false;
    }
    StatementPtr statement(rawStatement);
    EquipmentHistoryMap historyBySource;
    if (!LoadEquipmentHistoryMap(database.get(), 0, historyBySource, error)) {
        records.clear();
        return false;
    }
    while (true) {
        const int status = sqlite3_step(statement.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            error = SqliteError(database.get(), L"读取 Site Log 失败");
            records.clear();
            return false;
        }
        SiteLogRecord record = ReadRecord(statement.get());
        const auto found = historyBySource.find(record.sourceUrl);
        if (found != historyBySource.end()) {
            record.equipmentHistory = found->second;
        }
        records.push_back(std::move(record));
    }
    return true;
}

bool BackfillSiteLogEquipment(
    const std::filesystem::path& databasePath,
    std::wstring& error,
    int* recordsProcessed,
    int* epochsStored) {
    error.clear();
    if (recordsProcessed != nullptr) *recordsProcessed = 0;
    if (epochsStored != nullptr) *epochsStored = 0;
    if (!EnsureSiteLogSchema(databasePath, error)) return false;

    DatabasePtr database;
    if (!OpenDatabase(databasePath, database, error)) return false;
    if (!Execute(database.get(), "BEGIN IMMEDIATE;", error)) return false;

    auto rows = Prepare(
        database.get(),
        "SELECT source_url,station_id,four_char_id,revision_date,"
        "fetched_at_utc,network_mask,raw_text FROM site_logs;",
        error);
    auto deleteHistory = Prepare(
        database.get(),
        "DELETE FROM site_log_equipment WHERE source_url=?;",
        error);
    auto insertHistory = Prepare(
        database.get(),
        "INSERT OR REPLACE INTO site_log_equipment("
        "source_url,station_id,four_char_id,equipment_kind,sequence,"
        "equipment_type,systems,serial,firmware,radome,reference_point,"
        "date_installed,date_removed,revision_date,fetched_at_utc,network_mask"
        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        error);
    if (!rows || !deleteHistory || !insertHistory) {
        std::wstring ignored;
        Execute(database.get(), "ROLLBACK;", ignored);
        return false;
    }

    while (true) {
        const int status = sqlite3_step(rows.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            error = SqliteError(database.get(), L"读取待重解析 Site Log 失败");
            std::wstring ignored;
            Execute(database.get(), "ROLLBACK;", ignored);
            return false;
        }
        SiteLogRecord record;
        record.sourceUrl = ColumnText(rows.get(), 0);
        record.stationId = ColumnText(rows.get(), 1);
        record.fourCharId = ColumnText(rows.get(), 2);
        record.revisionDate = ColumnText(rows.get(), 3);
        record.fetchedAtUtc = ColumnText(rows.get(), 4);
        record.networkMask = static_cast<std::uint64_t>(
            sqlite3_column_int64(rows.get(), 5));
        const std::string rawText = ColumnText(rows.get(), 6);
        std::vector<SiteLogEquipmentEpoch> history =
            ParseSiteLogEquipmentHistory(rawText);
        NormalizeEquipmentProvenance(record, history);
        if (recordsProcessed != nullptr) ++*recordsProcessed;

        // Audited cross-network carriers may have no raw_text. A migration
        // must not destroy their already stored temporal evidence.
        if (history.empty()) continue;

        sqlite3_reset(deleteHistory.get());
        sqlite3_clear_bindings(deleteHistory.get());
        if (!BindText(deleteHistory.get(), 1, record.sourceUrl)
            || sqlite3_step(deleteHistory.get()) != SQLITE_DONE) {
            error = SqliteError(database.get(), L"清理 Site Log 旧设备履历失败");
            std::wstring ignored;
            Execute(database.get(), "ROLLBACK;", ignored);
            return false;
        }
        for (const auto& epoch : history) {
            if ((epoch.kind != "receiver" && epoch.kind != "antenna")
                || !EquipmentHasData(epoch)) continue;
            sqlite3_reset(insertHistory.get());
            sqlite3_clear_bindings(insertHistory.get());
            if (!BindEquipmentEpoch(insertHistory.get(), epoch)
                || sqlite3_step(insertHistory.get()) != SQLITE_DONE) {
                error = SqliteError(database.get(), L"写入 Site Log 设备履历失败");
                std::wstring ignored;
                Execute(database.get(), "ROLLBACK;", ignored);
                return false;
            }
            if (epochsStored != nullptr) ++*epochsStored;
        }
    }
    std::wstring commitError;
    if (!Execute(database.get(), "COMMIT;", commitError)) {
        error = std::move(commitError);
        std::wstring ignored;
        Execute(database.get(), "ROLLBACK;", ignored);
        return false;
    }
    return true;
}

bool LoadSiteLogSummaryMap(
    const std::filesystem::path& databasePath,
    const std::string_view asOfDate,
    std::unordered_map<std::string, SiteLogRecord>& recordsById,
    std::wstring& error,
    const std::uint64_t preferredNetworkMask) {
    error.clear();
    recordsById.clear();
    // Interactive lookups must stay read-only.  In particular, do not create
    // a site_logs table in every regional database merely because the user
    // clicked Query.  A missing database/table simply means that Site Logs
    // have not been propagated to that network yet; the availability query
    // itself can still proceed normally.
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(databasePath, filesystemError)) {
        return true;
    }
    DatabasePtr database;
    if (!OpenReadOnlyDatabase(databasePath, database, error)) return false;
    auto schema = Prepare(
        database.get(),
        "SELECT 1 FROM sqlite_master WHERE type='table'"
        " AND name='site_logs' LIMIT 1;",
        error);
    if (!schema) return false;
    if (sqlite3_step(schema.get()) != SQLITE_ROW) return true;
    // Keep any incidental SQLite temp structures in memory.  The summary
    // query below is deliberately unsorted, but this guard also prevents a
    // future planner change from placing a transient B-tree in the user's
    // (possibly full) system TEMP directory.
    char* pragmaError = nullptr;
    sqlite3_exec(database.get(), "PRAGMA temp_store=MEMORY;", nullptr, nullptr,
        &pragmaError);
    sqlite3_free(pragmaError);
    // This is an interactive lookup, not an export.  Do not select raw_text
    // (which can be large for an archive such as SOPAC/GARNER), and do not
    // impose a SQL ORDER BY here.  The canonical merge below compares
    // revision/fetch timestamps itself, so row order is immaterial.  The
    // former CASE/ORDER BY forced SQLite to materialize a temporary B-tree;
    // on systems whose TEMP directory is full that could stall or return
    // SQLITE_FULL for an otherwise tiny metadata query.
    //
    // Notice that the query deliberately does *not* restrict revision_date
    // when asOfDate is supplied.  A Site Log published in 2025 can contain a
    // receiver block installed in 2010; excluding that document would make a
    // 2010 lookup lose the correct historical equipment.  Revision filtering
    // is performed only for the scalar document snapshot below, while every
    // equipment interval is retained and selected by its own installed /
    // removed dates.
    const std::string sql = "SELECT "
        + std::string(SiteLogSummarySelectColumns())
        + " FROM site_logs WHERE (?1=0 OR (network_mask & ?1) != 0);";
    sqlite3_stmt* rawStatement = nullptr;
    if (sqlite3_prepare_v2(database.get(), sql.c_str(), -1, &rawStatement, nullptr)
        != SQLITE_OK) {
        error = SqliteError(database.get(), L"无法准备 Site Log 汇总查询");
        return false;
    }
    StatementPtr statement(rawStatement);
    const std::string asOf(asOfDate);
    if (sqlite3_bind_int64(
            statement.get(), 1,
            static_cast<sqlite3_int64>(preferredNetworkMask)) != SQLITE_OK) {
        error = SqliteError(database.get(), L"绑定 Site Log 来源筛选失败");
        return false;
    }
    EquipmentHistoryMap historyBySource;
    if (!LoadEquipmentHistoryMap(
            database.get(), preferredNetworkMask, historyBySource, error)) {
        return false;
    }

    struct CanonicalAggregate {
        SiteLogRecord scalar;
        SiteLogRecord fallback;
        std::vector<SiteLogEquipmentEpoch> history;
        bool hasScalar = false;
        bool hasFallback = false;
    };
    std::vector<CanonicalAggregate> canonical;
    std::unordered_map<std::string, std::size_t> canonicalIndex;
    std::unordered_map<std::string, std::size_t> canonicalBySourceUrl;
    const auto revisionEligible = [&](const SiteLogRecord& record) {
        return asOf.empty() || record.revisionDate.empty()
            || record.revisionDate <= asOf;
    };
    const auto addAliases = [&](const SiteLogRecord& record,
                                const std::size_t index) {
        if (!record.stationId.empty()) {
            canonicalIndex.emplace(UpperAscii(record.stationId), index);
        }
        if (!record.fourCharId.empty()) {
            canonicalIndex.emplace(UpperAscii(record.fourCharId), index);
        }
    };
    while (true) {
        const int status = sqlite3_step(statement.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            error = SqliteError(database.get(), L"读取 Site Log 汇总失败");
            recordsById.clear();
            return false;
        }
        SiteLogRecord record = ReadRecord(statement.get());
        const auto historyFound = historyBySource.find(record.sourceUrl);
        if (historyFound != historyBySource.end()) {
            record.equipmentHistory = historyFound->second;
        }
        const std::string key = KeyForRecord(record);
        if (key.empty()) continue;
        // A historical final-log file may contain only the four-character
        // code (for example CONZ), while the current log has CONZ00CHL.  Use
        // either identifier to collapse these into one station summary.
        std::size_t foundIndex = std::numeric_limits<std::size_t>::max();
        const auto primaryFound = canonicalIndex.find(key);
        if (primaryFound != canonicalIndex.end()) {
            foundIndex = primaryFound->second;
        } else if (!record.fourCharId.empty()) {
            const auto aliasFound = canonicalIndex.find(
                UpperAscii(record.fourCharId));
            if (aliasFound != canonicalIndex.end()) {
                foundIndex = aliasFound->second;
            }
        }
        const bool eligible = revisionEligible(record);
        if (foundIndex == std::numeric_limits<std::size_t>::max()) {
            foundIndex = canonical.size();
            CanonicalAggregate aggregate;
            aggregate.history = record.equipmentHistory;
            aggregate.fallback = record;
            aggregate.fallback.equipmentHistory.clear();
            aggregate.hasFallback = true;
            if (eligible) {
                aggregate.scalar = record;
                aggregate.scalar.equipmentHistory.clear();
                aggregate.hasScalar = true;
            } else {
                // Keep only identifiers as a grouping anchor until an
                // eligible scalar document for this station is encountered.
                aggregate.scalar.stationId = record.stationId;
                aggregate.scalar.fourCharId = record.fourCharId;
                aggregate.scalar.networkMask = record.networkMask;
            }
            canonical.push_back(std::move(aggregate));
            addAliases(record, foundIndex);
        } else {
            auto& aggregate = canonical[foundIndex];
            MergeEquipmentHistory(aggregate.history, record.equipmentHistory);
            aggregate.scalar.networkMask |= record.networkMask;
            SiteLogRecord fallbackCandidate = record;
            fallbackCandidate.equipmentHistory.clear();
            if (!aggregate.hasFallback) {
                aggregate.fallback = fallbackCandidate;
                aggregate.hasFallback = true;
            } else {
                MergeSiteLogRecords(aggregate.fallback, fallbackCandidate);
            }
            if (eligible) {
                if (!aggregate.hasScalar) {
                    aggregate.scalar = record;
                    aggregate.scalar.equipmentHistory.clear();
                    aggregate.hasScalar = true;
                } else {
                    // Merge scalar metadata using the historical revision
                    // rule, but keep equipment history in the separate
                    // aggregate vector (otherwise a future document could
                    // accidentally replace it during move assignment).
                    SiteLogRecord candidate = record;
                    candidate.equipmentHistory.clear();
                    MergeSiteLogRecords(aggregate.scalar, candidate);
                }
            }
            addAliases(record, foundIndex);
            addAliases(aggregate.scalar, foundIndex);
        }
        if (!record.sourceUrl.empty()) {
            canonicalBySourceUrl.emplace(
                UpperAscii(record.sourceUrl), foundIndex);
        }
    }
    for (auto& aggregate : canonical) {
        // A document may have been published after the observation date yet
        // still contain the complete historical 3.x/4.x equipment timeline.
        // If no document revision is old enough for the scalar snapshot,
        // retain the newest available snapshot as a metadata carrier and let
        // ApplyEffectiveEquipment replace its receiver/antenna fields from
        // the dated intervals.  Otherwise an otherwise valid 2019 station
        // would disappear merely because its log was revised in 2024.
        if (!aggregate.hasScalar) {
            if (!aggregate.hasFallback) continue;
            aggregate.scalar = std::move(aggregate.fallback);
            aggregate.hasScalar = true;
        }
        if (aggregate.hasFallback) {
            aggregate.scalar.networkMask |= aggregate.fallback.networkMask;
        }
        aggregate.scalar.equipmentHistory = std::move(aggregate.history);
        ApplyEffectiveEquipment(aggregate.scalar, asOf);
        const auto& record = aggregate.scalar;
        if (!record.stationId.empty()) {
            recordsById.emplace(UpperAscii(record.stationId), record);
        }
        if (!record.fourCharId.empty()) {
            recordsById.emplace(UpperAscii(record.fourCharId), record);
        }
    }
    // Alias rows are intentionally applied after the normal canonical merge.
    // This keeps the original Site Log identity authoritative while allowing
    // a separately audited historical/alternate station key to resolve to
    // that same record.  An alias that collides with a real canonical key is
    // ignored rather than replacing the canonical row.
    if (TableExists(database.get(), "site_log_aliases")) {
        const std::string aliasSql =
            "SELECT alias_key,source_url FROM site_log_aliases "
            "WHERE (?1=0 OR (network_mask & ?1) != 0);";
        auto aliases = Prepare(database.get(), aliasSql.c_str(), error);
        if (!aliases) return false;
        if (sqlite3_bind_int64(
                aliases.get(), 1,
                static_cast<sqlite3_int64>(preferredNetworkMask)) != SQLITE_OK) {
            error = SqliteError(database.get(), L"绑定 Site Log 别名来源筛选失败");
            return false;
        }
        while (true) {
            const int status = sqlite3_step(aliases.get());
            if (status == SQLITE_DONE) break;
            if (status != SQLITE_ROW) {
                error = SqliteError(database.get(), L"读取 Site Log 别名失败");
                recordsById.clear();
                return false;
            }
            const std::string alias = UpperAscii(Trim(ColumnText(aliases.get(), 0)));
            const std::string source = UpperAscii(Trim(ColumnText(aliases.get(), 1)));
            if (alias.empty() || source.empty()) continue;
            const auto sourceFound = canonicalBySourceUrl.find(source);
            if (sourceFound == canonicalBySourceUrl.end()) continue;
            const auto& record = canonical[sourceFound->second].scalar;
            if (record.stationId.empty() && record.fourCharId.empty()) continue;
            recordsById.emplace(alias, record);
        }
    }
    return true;
}

SiteLogBuildStats BuildSiteLogIndex(
    const std::filesystem::path& databasePath,
    const int jobs,
    const bool refresh,
    const std::stop_token stopToken,
    const std::function<void(int completed, int total)>& progress) {
    SiteLogBuildStats stats;
    stats.databasePath = databasePath;
    std::wstring schemaError;
    if (!EnsureSiteLogSchema(databasePath, schemaError)) {
        stats.error = std::move(schemaError);
        return stats;
    }

    struct SourceDirectory {
        const wchar_t* url;
        const char* name;
        std::uint64_t networkMask;
    };
    constexpr SourceDirectory directories[]{
        {L"https://files.igs.org/pub/station/log/", "log", 1ULL},
        {L"https://files.igs.org/pub/station/finallog/", "finallog", 1ULL},
        {L"https://files.igs.org/pub/station/mgexlog/", "mgexlog", 2ULL},
        {L"https://files.igs.org/pub/station/mgexfinallog/", "mgexfinallog", 2ULL}
    };
    std::vector<DiscoveredLog> discovered;
    std::unordered_set<std::string> discoveredUrls;
    std::vector<std::wstring> directoryErrors;
    for (const auto& directory : directories) {
        if (stopToken.stop_requested()) break;
        ++stats.directoriesAttempted;
        const HttpTextResult response = HttpGetText(
            directory.url, stopToken, 8U * 1024U * 1024U);
        if (!response.ok()) {
            directoryErrors.push_back(
                Utf8ToWide(directory.name) + L"："
                + (response.error.empty()
                    ? L"HTTP " + std::to_wstring(response.statusCode)
                    : response.error));
            continue;
        }
        ++stats.directoriesSuccessful;
        auto files = ParseDirectory(
            response.body, directory.url, directory.networkMask);
        for (auto& file : files) {
            const std::string url = WideToUtf8(file.url);
            if (discoveredUrls.insert(LowerAscii(url)).second) {
                discovered.push_back(std::move(file));
            } else {
                // A mirror can expose the same URL in more than one logical
                // directory.  Keep the route once, but do not lose its IGS /
                // MGEX membership bit when that happens.
                const auto duplicate = std::find_if(
                    discovered.begin(),
                    discovered.end(),
                    [&](const DiscoveredLog& existing) {
                        return LowerAscii(WideToUtf8(existing.url))
                            == LowerAscii(url);
                    });
                if (duplicate != discovered.end()) {
                    duplicate->networkMask |= file.networkMask;
                }
            }
        }
    }
    stats.filesDiscovered = static_cast<int>(discovered.size());
    if (!directoryErrors.empty()) {
        stats.warning = L"部分 Site Log 目录读取失败：";
        for (std::size_t index = 0; index < directoryErrors.size(); ++index) {
            if (index != 0) stats.warning += L"；";
            stats.warning += directoryErrors[index];
        }
    }
    if (stats.directoriesSuccessful == 0) {
        stats.error = stats.warning.empty()
            ? L"四个官方 Site Log 目录均无法读取。"
            : stats.warning;
        return stats;
    }

    std::unordered_set<std::string> existingUrls;
    if (!refresh) {
        DatabasePtr database;
        std::wstring error;
        if (OpenDatabase(databasePath, database, error)) {
            sqlite3_stmt* rawStatement = nullptr;
            if (sqlite3_prepare_v2(
                    database.get(), "SELECT source_url FROM site_logs;", -1,
                    &rawStatement, nullptr) == SQLITE_OK) {
                StatementPtr statement(rawStatement);
                while (sqlite3_step(statement.get()) == SQLITE_ROW) {
                    existingUrls.insert(LowerAscii(ColumnText(statement.get(), 0)));
                }
            }
        }
    }
    std::vector<DiscoveredLog> pending;
    pending.reserve(discovered.size());
    for (auto& file : discovered) {
        if (refresh || !existingUrls.contains(
                LowerAscii(WideToUtf8(file.url)))) {
            pending.push_back(std::move(file));
        }
    }

    const int workerCount = std::max(1, std::min(jobs, 16));
    std::atomic<std::size_t> next{0};
    std::atomic<int> completed{0};
    std::atomic<int> fetched{0};
    std::atomic<int> parsed{0};
    std::atomic<int> failed{0};
    std::mutex recordsMutex;
    std::mutex errorMutex;
    std::vector<SiteLogRecord> records;
    std::wstring firstFetchError;
    const auto worker = [&]() {
        while (!stopToken.stop_requested()) {
            const std::size_t index = next.fetch_add(1);
            if (index >= pending.size()) break;
            const auto& file = pending[index];
            const auto response = FetchSiteLogTextWithRetry(
                file.url, stopToken, 2U * 1024U * 1024U, 3);
            if (!response.ok()) {
                ++failed;
                std::scoped_lock lock(errorMutex);
                if (firstFetchError.empty()) {
                    firstFetchError = response.error.empty()
                        ? L"HTTP " + std::to_wstring(response.statusCode)
                        : response.error;
                }
            } else {
                ++fetched;
                SiteLogRecord record = ParseSiteLogText(
                    response.body,
                    WideToUtf8(file.url),
                    WideToUtf8(file.directory),
                    file.networkMask);
                if (!record.stationId.empty() || !record.fourCharId.empty()) {
                    ++parsed;
                    std::scoped_lock lock(recordsMutex);
                    records.push_back(std::move(record));
                } else {
                    ++failed;
                }
            }
            const int done = ++completed;
            if (progress) progress(done, static_cast<int>(pending.size()));
        }
    };
    std::vector<std::jthread> workers;
    workers.reserve(static_cast<std::size_t>(workerCount));
    for (int index = 0; index < workerCount; ++index) {
        workers.emplace_back(worker);
    }
    workers.clear();

    stats.filesFetched = fetched.load();
    stats.filesParsed = parsed.load();
    stats.filesFailed = failed.load();
    if (!firstFetchError.empty()) {
        if (!stats.warning.empty()) stats.warning += L"；";
        stats.warning += L"部分文件读取失败：" + firstFetchError;
    }
    if (stopToken.stop_requested()) {
        stats.error = L"Site Log 扫描已取消；本次未写入未完成文件。";
        return stats;
    }
    std::wstring storeError;
    if (!StoreSiteLogRecords(databasePath, records, storeError)) {
        stats.error = std::move(storeError);
        return stats;
    }
    stats.recordsStored = static_cast<int>(records.size());
    return stats;
}

// Build the GA-GDC native log table from the official anonymous GeodesyML
// API.  The current GA SFTP endpoint is excellent for bulk file transfer,
// but it requires an external OpenSSH client and its directory listing is
// intentionally unbounded.  The read-only API gives us the same current and
// historical records together with stable IDs and structured fields, so it is
// a safer default for a portable builder.  Pages are fetched sequentially to
// respect the service's modest request limits; individual log parsing is
// local and does not issue one HTTP request per station.
SiteLogBuildStats BuildGaGdcSiteLogIndex(
    const std::filesystem::path& databasePath,
    const int jobs,
    const bool refresh,
    const std::stop_token stopToken,
    const std::function<void(int completed, int total)>& progress) {
    // The GA endpoint exposes a paginated catalogue and is deliberately read
    // page-by-page to avoid stressing the public service.  Keep the common
    // builder signature (and its CLI --jobs option) without pretending that
    // concurrent page requests are safe here.
    (void)jobs;
    SiteLogBuildStats stats;
    stats.databasePath = databasePath;
    std::wstring schemaError;
    if (!EnsureSiteLogSchema(databasePath, schemaError)) {
        stats.error = std::move(schemaError);
        return stats;
    }

    // Existing API rows are keyed by the synthetic /{numeric-id} URL below.
    // Reading this set before the network loop keeps a normal incremental run
    // cheap while --refresh still replaces every returned revision.
    std::unordered_set<std::string> existingUrls;
    if (!refresh) {
        DatabasePtr database;
        std::wstring error;
        if (OpenDatabase(databasePath, database, error)) {
            auto statement = Prepare(
                database.get(), "SELECT source_url FROM site_logs;", error);
            if (statement) {
                while (sqlite3_step(statement.get()) == SQLITE_ROW) {
                    existingUrls.insert(
                        LowerAscii(ColumnText(statement.get(), 0)));
                }
            }
        }
    }

    constexpr std::wstring_view apiRoot =
        L"https://metadata.gnss.ga.gov.au/api/siteLogs";
    // 100 is accepted by the current Spring Data endpoint and keeps the
    // number of requests reasonable.  If an older deployment rejects that
    // size, the first-page retry below falls back to its documented default
    // of 20.
    int pageSize = 100;
    int page = 0;
    int totalPages = 0;
    bool usedFallbackPageSize = false;
    bool incomplete = false;
    std::wstring firstError;
    std::vector<SiteLogRecord> records;

    while (!stopToken.stop_requested()) {
        ++stats.directoriesAttempted;
        const std::wstring url = std::wstring(apiRoot)
            + L"?page=" + std::to_wstring(page)
            + L"&size=" + std::to_wstring(pageSize);
        HttpTextResult response = FetchSiteLogTextWithRetry(
            url, stopToken, 32U * 1024U * 1024U, 3);
        if (!response.ok()
            && page == 0 && !usedFallbackPageSize && pageSize != 20) {
            // Some older GA deployments cap `size` at 20 and return a 400.
            // Retry only the first page with that conservative size; this is
            // also useful when an intermediary rejects a long query string.
            pageSize = 20;
            usedFallbackPageSize = true;
            ++stats.directoriesAttempted;
            response = FetchSiteLogTextWithRetry(
                std::wstring(apiRoot) + L"?page=0&size=20",
                stopToken, 16U * 1024U * 1024U, 3);
        }
        if (!response.ok()) {
            incomplete = true;
            firstError = response.error.empty()
                ? L"HTTP " + std::to_wstring(response.statusCode)
                : response.error;
            break;
        }
        ++stats.directoriesSuccessful;
        const auto objects = ParseGaSiteLogPage(response.body);
        if (objects.empty()) {
            if (page == 0) {
                incomplete = true;
                firstError = L"GA GeodesyML API 返回成功，但没有 siteLogs 记录。";
            }
            break;
        }

        // Keep the progress/reporting counters meaningful for the paginated
        // API.  Unlike directory listings, GA does not expose a file count
        // through the discovery vector; the catalogue's totalElements is the
        // number of logical Site Logs and is available on every page.
        if (stats.filesDiscovered == 0) {
            double totalElements = 0.0;
            if (JsonNumberField(response.body, "totalElements", totalElements)
                && totalElements >= 0.0 && totalElements < 10000000.0) {
                stats.filesDiscovered = static_cast<int>(totalElements);
            }
        }
        if (stats.filesDiscovered == 0) {
            stats.filesDiscovered += static_cast<int>(objects.size());
        }

        double pageCountNumber = 0.0;
        if (totalPages == 0
            && JsonNumberField(response.body, "totalPages", pageCountNumber)
            && pageCountNumber > 0.0 && pageCountNumber < 5000.0) {
            totalPages = static_cast<int>(pageCountNumber);
        }
        if (totalPages == 0) totalPages = page + 1;
        for (const auto& object : objects) {
            ++stats.filesFetched;
            SiteLogRecord record = ParseGaSiteLogObject(object);
            const std::string id = JsonIntegerField(object, "id");
            if (record.stationId.empty() && record.fourCharId.empty()) {
                ++stats.filesFailed;
                continue;
            }
            const std::string xml = JsonStringField(object, "siteLogText");
            record.networkMask = NetworkBit(ObservationNetwork::GaGdc);
            record.sourceDirectory = WideToUtf8(apiRoot);
            record.sourceUrl = WideToUtf8(apiRoot)
                + (id.empty() ? "?page=" + std::to_string(page)
                              : "/" + id);
            const std::string revisionDigits = record.revisionDate.empty()
                ? "undated"
                : record.revisionDate.substr(0, 4)
                    + record.revisionDate.substr(5, 2)
                    + record.revisionDate.substr(8, 2);
            record.fileName = (record.stationId.empty()
                ? record.fourCharId : record.stationId)
                + "_" + revisionDigits + ".log";
            record.rawText = GaSyntheticSiteLog(record, xml);
            record.fetchedAtUtc = CurrentUtc();
            ++stats.filesParsed;
            if (refresh || !existingUrls.contains(
                    LowerAscii(record.sourceUrl))) {
                records.push_back(std::move(record));
            }
        }
        if (progress) {
            progress(
                std::min(page + 1, totalPages),
                std::max(totalPages, page + 1));
        }
        ++page;
        if (page >= totalPages || objects.size() <
                static_cast<std::size_t>(pageSize)) {
            break;
        }
        if (page >= 5000) {
            incomplete = true;
            firstError = L"GA GeodesyML API 页数超过安全上限。";
            break;
        }
    }
    if (stopToken.stop_requested()) {
        stats.error = L"GA-GDC Site Log 扫描已取消；本次未写入未完成页面。";
        return stats;
    }
    if (incomplete) {
        stats.error = L"GA-GDC GeodesyML API 未完整读取：" + firstError;
        return stats;
    }
    std::wstring storeError;
    if (!StoreSiteLogRecords(databasePath, records, storeError)) {
        stats.error = std::move(storeError);
        return stats;
    }
    stats.recordsStored = static_cast<int>(records.size());
    return stats;
}

std::string FirstJsonStringField(
    const std::string_view object,
    const std::initializer_list<std::string_view> keys) {
    for (const auto key : keys) {
        const std::string value = JsonStringField(object, key);
        if (!Trim(value).empty()) return Trim(value);
    }
    return {};
}

std::string UrlEncodeQueryValue(const std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char character : value) {
        const bool unreserved =
            (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9')
            || character == '-' || character == '_' || character == '.'
            || character == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[(character >> 4) & 0x0F]);
            encoded.push_back(hex[character & 0x0F]);
        }
    }
    return encoded;
}

std::string NormalizeM3gStationId(std::string value) {
    value = Trim(value);
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char character : value) {
        if (std::isalnum(character) != 0 || character == '_') {
            normalized.push_back(static_cast<char>(character));
        }
    }
    normalized = UpperAscii(normalized);
    // M3G normally returns a 9-character RINEX-3 identifier.  A few legacy
    // records expose a longer display label; retaining the leading canonical
    // identifier avoids putting display text into the station key.
    if (normalized.size() > 9) normalized.resize(9);
    return normalized;
}

bool ParseM3gLatLonHeight(
    const std::string_view value,
    double& latitude,
    double& longitude,
    double& height) {
    std::array<double, 3> values{};
    std::size_t begin = 0;
    int count = 0;
    while (begin <= value.size() && count < 3) {
        const auto slash = value.find('/', begin);
        const auto end = slash == std::string_view::npos
            ? value.size() : slash;
        double parsed = 0.0;
        if (!ParseDouble(value.substr(begin, end - begin), parsed)) return false;
        values[static_cast<std::size_t>(count++)] = parsed;
        if (slash == std::string_view::npos) break;
        begin = slash + 1;
    }
    if (count < 2 || std::abs(values[0]) > 90.0
        || std::abs(values[1]) > 180.0) return false;
    latitude = values[0];
    longitude = values[1];
    height = count >= 3 ? values[2] : 0.0;
    return true;
}

std::vector<std::string> ParseM3gFeatureObjects(const std::string& body) {
    // Current v1 endpoint returns a GeoJSON FeatureCollection.  Accept the
    // older `data`/`results` wrappers and a single Feature as well, because
    // public M3G deployments have used all three shapes over time.
    for (const std::string_view key : {"features", "data", "results"}) {
        const std::string composite = JsonCompositeField(body, key);
        if (!composite.empty()) {
            auto objects = JsonTopLevelObjects(composite);
            if (!objects.empty()) return objects;
        }
    }
    // The dedicated /sitelog/geojson endpoint returns the Feature objects as
    // a bare JSON array (and supports pagination=0).  Accept that shape too.
    const std::size_t firstNonWhitespace = body.find_first_not_of(
        " \t\r\n");
    if (firstNonWhitespace != std::string::npos
        && body[firstNonWhitespace] == '[') {
        auto objects = JsonTopLevelObjects(body);
        if (!objects.empty()) return objects;
    }
    const std::string type = LowerAscii(JsonStringField(body, "type"));
    if (type == "feature") return {body};
    return {};
}

std::string M3gAntennaType(std::string value, std::string& radome) {
    value = Trim(value);
    if (value.empty()) return {};
    // M3G's compact field normally contains "ANTENNA RADOME" separated by
    // runs of spaces.  Keep the complete original when it is unusual, while
    // promoting the two conventional tokens to the same fields used by the
    // native IGS parser.
    std::size_t split = value.find_first_of(" \t");
    if (split == std::string::npos) return value;
    const std::string type = Trim(value.substr(0, split));
    const std::string tail = Trim(value.substr(split));
    if (radome.empty() && !tail.empty()) {
        const std::size_t second = tail.find_first_of(" \t");
        radome = Trim(tail.substr(0, second));
    }
    return type.empty() ? value : type;
}

SiteLogRecord ParseM3gGeoJsonFeature(
    const std::string& feature,
    const std::uint64_t networkMask,
    const std::string_view sourceUrl) {
    SiteLogRecord record;
    record.networkMask = networkMask;
    const std::string properties = JsonCompositeField(feature, "properties");
    const std::string geometry = JsonCompositeField(feature, "geometry");
    if (properties.empty()) return record;
    record.stationId = NormalizeM3gStationId(FirstJsonStringField(
        properties, {"id", "GNSS Station ID", "stationId"}));
    const std::string siteForm = JsonCompositeField(properties, "siteForm");
    const std::string location = JsonCompositeField(properties, "location");
    record.fourCharId = UpperAscii(FirstJsonStringField(
        siteForm, {"fourCharId", "fourCharacterId"}));
    if (record.fourCharId.empty() && record.stationId.size() >= 4) {
        record.fourCharId = record.stationId.substr(0, 4);
    }
    record.domes = Trim(FirstJsonStringField(siteForm, {"domes", "DOMES"}));
    record.siteName = Trim(FirstJsonStringField(siteForm,
        {"siteName", "name"}));
    record.city = Trim(FirstJsonStringField(location,
        {"location", "city", "City"}));
    record.country = UpperAscii(Trim(FirstJsonStringField(location,
        {"countryCode", "Country/Region", "country"})));
    const std::string llh = JsonCompositeField(
        location, "approximatePositionGRS80");
    const std::string lat = FirstJsonStringField(llh, {"latitude"});
    const std::string lon = FirstJsonStringField(llh, {"longitude"});
    const std::string elevation = FirstJsonStringField(llh, {"elevation"});
    if (!lat.empty() && !lon.empty()) {
        ParseDouble(lat, record.latitude);
        ParseDouble(lon, record.longitude);
        ParseDouble(elevation, record.elevation);
        record.hasCoordinate = std::abs(record.latitude) <= 90.0
            && std::abs(record.longitude) <= 180.0;
    }
    if (!record.hasCoordinate) {
        const auto coordinates = JsonNumberArray(
            JsonCompositeField(geometry, "coordinates"));
        if (coordinates.size() >= 2
            && std::abs(coordinates[0]) <= 180.0
            && std::abs(coordinates[1]) <= 90.0) {
            record.longitude = coordinates[0];
            record.latitude = coordinates[1];
            record.elevation = coordinates.size() >= 3 ? coordinates[2] : 0.0;
            record.hasCoordinate = true;
        }
    }
    record.fileName = FirstJsonStringField(properties,
        {"sitelogName", "Site Log File Name", "fileName"});
    record.sourceUrl = FirstJsonStringField(
        properties, {"Site Log", "siteLog"});
    if (record.sourceUrl.empty()) {
        const std::string links = JsonCompositeField(properties, "_links");
        const std::string sitelog = JsonCompositeField(links, "sitelog");
        record.sourceUrl = FirstJsonStringField(sitelog, {"href", "url"});
    }
    if (record.sourceUrl.empty()) record.sourceUrl = std::string(sourceUrl);
    record.sourceDirectory = std::string(sourceUrl);
    record.revisionDate = M3gDateOnly(record.fileName);
    record.receiverDateInstalled = M3gDateOnly(FirstJsonStringField(
        siteForm, {"dateInstalled", "installationDate"}));
    if (record.siteName.empty()) record.siteName = record.city;
    record.rawText = GaSyntheticSiteLog(record, {});
    record.fetchedAtUtc = CurrentUtc();
    return record;
}

SiteLogRecord ParseM3gFeature(
    const std::string& feature,
    const std::uint64_t networkMask,
    const std::string_view apiUrl,
    const std::string_view requestedNetwork) {
    SiteLogRecord record;
    record.networkMask = networkMask;
    const std::string properties = JsonCompositeField(feature, "properties");
    const std::string geometry = JsonCompositeField(feature, "geometry");
    if (properties.empty()) return record;

    record.stationId = NormalizeM3gStationId(FirstJsonStringField(
        properties, {"GNSS Station ID", "9-character ID", "Nine Character ID",
                     "Station ID", "stationId", "id"}));
    record.fourCharId = UpperAscii(FirstJsonStringField(
        properties, {"Four Character ID", "4-character ID", "Station",
                     "station", "markerName"}));
    if (record.fourCharId.empty() && record.stationId.size() >= 4) {
        record.fourCharId = record.stationId.substr(0, 4);
    }
    if (record.stationId.size() < 9 && record.fourCharId.size() >= 4) {
        // Do not invent a country suffix when M3G did not provide a canonical
        // 9-character ID; the four-character key remains authoritative.
        record.stationId.clear();
    }
    record.domes = Trim(FirstJsonStringField(
        properties, {"DOMES Number", "IERS DOMES Number", "domesNumber"}));
    record.siteName = Trim(FirstJsonStringField(
        properties, {"Site Name", "siteName", "Marker Name"}));
    record.city = Trim(FirstJsonStringField(
        properties, {"City/Town", "City", "city", "Town"}));
    record.country = UpperAscii(Trim(FirstJsonStringField(
        properties, {"Country/Region", "Country", "country", "countryCode"})));

    const auto coordinates = JsonNumberArray(
        JsonCompositeField(geometry, "coordinates"));
    if (coordinates.size() >= 2
        && std::abs(coordinates[0]) <= 180.0
        && std::abs(coordinates[1]) <= 90.0) {
        // GeoJSON is [longitude, latitude, height].
        record.longitude = coordinates[0];
        record.latitude = coordinates[1];
        record.elevation = coordinates.size() >= 3 ? coordinates[2] : 0.0;
        record.hasCoordinate = true;
    }
    if (!record.hasCoordinate) {
        const std::string latLonHeight = FirstJsonStringField(
            properties, {"Lat/Lon/Height", "Latitude/Longitude/Height"});
        if (ParseM3gLatLonHeight(latLonHeight, record.latitude,
                                 record.longitude, record.elevation)) {
            record.hasCoordinate = true;
        }
    } else if (coordinates.size() < 3) {
        // The flat M3G representation carries the height in its
        // Lat/Lon/Height member while GeoJSON geometry often contains only
        // longitude/latitude.
        const std::string latLonHeight = FirstJsonStringField(
            properties, {"Lat/Lon/Height", "Latitude/Longitude/Height"});
        double latitude = 0.0;
        double longitude = 0.0;
        double elevation = 0.0;
        if (ParseM3gLatLonHeight(latLonHeight, latitude, longitude,
                                 elevation)
            && std::abs(latitude - record.latitude) < 1.0e-5
            && std::abs(longitude - record.longitude) < 1.0e-5) {
            record.elevation = elevation;
        }
    }

    record.receiverType = Trim(FirstJsonStringField(
        properties, {"GNSS Receiver (Current)", "Receiver (Current)",
                     "Receiver Type", "receiverType"}));
    record.receiverSystems = Trim(FirstJsonStringField(
        properties, {"Satellite Constellation (Current)",
                     "Satellite System", "receiverSystems"}));
    record.receiverSerial = Trim(FirstJsonStringField(
        properties, {"Receiver Serial Number", "receiverSerial"}));
    record.receiverFirmware = Trim(FirstJsonStringField(
        properties, {"Receiver Firmware", "Firmware Version",
                     "receiverFirmware"}));
    record.receiverDateInstalled = Trim(FirstJsonStringField(
        properties, {"Receiver Installation Date", "Installation Date",
                     "receiverDateInstalled"}));
    record.antennaType = Trim(FirstJsonStringField(
        properties, {"GNSS Antenna (Current)", "Antenna (Current)",
                     "Antenna Type", "antennaType"}));
    record.antennaRadome = Trim(FirstJsonStringField(
        properties, {"GNSS Antenna Radome (Current)",
                     "Antenna Radome (Current)", "Radome Type",
                     "antennaRadome"}));
    record.antennaType = M3gAntennaType(
        record.antennaType, record.antennaRadome);
    record.antennaSerial = Trim(FirstJsonStringField(
        properties, {"Antenna Serial Number", "antennaSerial"}));
    record.antennaReferencePoint = Trim(FirstJsonStringField(
        properties, {"Antenna Reference Point", "antennaReferencePoint"}));
    record.antennaDateInstalled = Trim(FirstJsonStringField(
        properties, {"Antenna Installation Date", "antennaDateInstalled"}));
    record.agency = Trim(FirstJsonStringField(
        properties, {"Responsible Agency", "Station Owner", "Owner",
                     "Agency", "RINEX Data Provider", "Point Of Contact",
                     "agency"}));
    record.operatorName = Trim(FirstJsonStringField(
        properties, {"Station Metadata Maintainer", "Metadata Maintainer",
                     "Point Of Contact", "RINEX Data Provider",
                     "operatorName"}));
    record.revisionDate = M3gDateOnly(FirstJsonStringField(
        properties, {"Last Update Of The Metadata", "Last Update",
                     "Revision Date", "revisionDate"}));

    std::string siteLogUrl = FirstJsonStringField(
        properties, {"Site Log", "Site Log URL", "siteLog"});
    if (siteLogUrl.empty()) {
        siteLogUrl = std::string(apiUrl) + "#"
            + (record.stationId.empty() ? record.fourCharId : record.stationId);
    } else if (siteLogUrl.starts_with("/")) {
        siteLogUrl = "https://gnss-metadata.eu" + siteLogUrl;
    }
    record.sourceUrl = siteLogUrl;
    record.sourceDirectory = std::string(apiUrl);
    record.fileName = FirstJsonStringField(
        properties, {"Site Log File Name", "File Name", "fileName"});
    if (record.siteName.empty()) record.siteName = record.city;
    if (record.revisionDate.empty()) record.revisionDate = M3gDateOnly(
        record.fileName);
    if (record.fileName.empty()) {
        record.fileName = (record.stationId.empty()
            ? record.fourCharId : record.stationId)
            + "_" + (record.revisionDate.empty()
                ? std::string("undated")
                : record.revisionDate.substr(0, 4)
                    + record.revisionDate.substr(5, 2)
                    + record.revisionDate.substr(8, 2))
            + ".log";
    }
    // Keep a standard, locally parseable representation so --reparse works
    // even when the M3G API later changes its property names.
    record.rawText = GaSyntheticSiteLog(record, {});
    record.fetchedAtUtc = CurrentUtc();
    (void)requestedNetwork;
    return record;
}

// M3G's GeoJSON/flat metadata endpoints intentionally expose only the
// currently installed receiver and antenna.  The linked GeodesyML document
// is the authoritative source for the complete installation/removal history.
// Keep this request separate from ParseM3gFeature so the latter remains a
// cheap, deterministic parser for catalogue responses.  The API export is
// tried first; the documented static XML mirror is a useful fallback during
// API maintenance or when an institutional gateway strips the endpoint.
struct M3gGeodesyMlResult {
    bool ok = false;
    std::string body;
    std::wstring error;
};

M3gGeodesyMlResult FetchM3gGeodesyMl(
    const std::string_view stationId,
    const std::stop_token stopToken) {
    M3gGeodesyMlResult result;
    const std::string id = NormalizeM3gStationId(std::string(stationId));
    if (id.empty()) {
        result.error = L"M3G 记录缺少有效的九字符站号";
        return result;
    }
    const std::string encoded = UrlEncodeQueryValue(id);
    const std::array<std::string, 2> urls{
        "https://gnss-metadata.eu/v1/sitelog/exportxml?id=" + encoded,
        "https://gnss-metadata.eu/data/station/xml/" + id + ".xml"
    };
    std::wstring lastError;
    for (const auto& url : urls) {
        if (stopToken.stop_requested()) {
            result.error = L"M3G GeodesyML 读取已取消";
            return result;
        }
        const auto response = FetchSiteLogTextWithRetry(
            Utf8ToWide(url), stopToken, 16U * 1024U * 1024U, 2);
        if (!response.ok()) {
            lastError = response.error.empty()
                ? L"HTTP " + std::to_wstring(response.statusCode)
                : response.error;
            continue;
        }
        // A successful HTTP response may still be an HTML login/error page.
        // Require an XML-looking payload and at least one equipment element;
        // otherwise try the alternate official route.
        const std::string lower = LowerAscii(response.body);
        const bool xmlLike = response.body.find('<') != std::string::npos;
        const bool hasEquipment = lower.find("gnssreceiver") != std::string::npos
            || lower.find("gnssantenna") != std::string::npos;
        if (!xmlLike || !hasEquipment) {
            lastError = L"响应不是包含设备履历的 GeodesyML";
            continue;
        }
        result.ok = true;
        result.body = response.body;
        return result;
    }
    result.error = lastError.empty()
        ? L"M3G GeodesyML 两个官方地址均未返回有效 XML"
        : lastError;
    return result;
}

std::string M3gBatchUrl(
    const std::string_view baseUrl,
    const std::string_view network,
    const std::vector<std::string>& stationIds,
    const std::size_t begin,
    const std::size_t end) {
    std::string url = std::string(baseUrl) + "?network="
        + UrlEncodeQueryValue(network) + "&format=metadata-sitelog";
    // The API accepts the documented deep-object filter syntax.  Keeping
    // batches small avoids URI limits imposed by institutional gateways.
    for (std::size_t index = begin; index < end; ++index) {
        url += "&filter%5Bid%5D%5Bin%5D%5B"
            + std::to_string(index - begin) + "%5D="
            + UrlEncodeQueryValue(stationIds[index]);
    }
    return url;
}

SiteLogBuildStats BuildM3gSiteLogIndex(
    const NativeSiteLogDefinition& definition,
    const std::filesystem::path& databasePath,
    const int jobs,
    const bool refresh,
    const std::stop_token stopToken,
    const std::function<void(int completed, int total)>& progress) {
    SiteLogBuildStats stats;
    stats.databasePath = databasePath;
    std::wstring schemaError;
    if (!EnsureSiteLogSchema(databasePath, schemaError)) {
        stats.error = std::move(schemaError);
        return stats;
    }
    if (definition.m3gNetwork == nullptr || *definition.m3gNetwork == '\0') {
        stats.error = L"M3G Site Log 定义缺少网络筛选名。";
        return stats;
    }

    const std::string network = definition.m3gNetwork;
    const std::string encodedNetwork = UrlEncodeQueryValue(network);
    const std::string flatUrl = WideToUtf8(definition.catalogUrl);
    const std::string geoUrl =
        "https://gnss-metadata.eu/v1/sitelog/geojson?network="
        + encodedNetwork + "&pagination=0";

    // First obtain the complete station-id set.  The dedicated GeoJSON
    // endpoint supports pagination=0; a page fallback is retained for older
    // M3G deployments which silently cap a response at 20 features.
    ++stats.directoriesAttempted;
    auto geoResponse = FetchSiteLogTextWithRetry(
        Utf8ToWide(geoUrl), stopToken, 64U * 1024U * 1024U, 3);
    if (!geoResponse.ok()) {
        stats.error = L"M3G Site Log 枚举接口读取失败："
            + (geoResponse.error.empty()
                ? L"HTTP " + std::to_wstring(geoResponse.statusCode)
                : geoResponse.error);
        return stats;
    }
    auto geoFeatures = ParseM3gFeatureObjects(geoResponse.body);
    if (geoFeatures.empty()) {
        stats.error = L"M3G 返回成功，但没有可识别的 GeoJSON Feature。";
        return stats;
    }
    ++stats.directoriesSuccessful;

    std::vector<std::string> stationIds;
    std::unordered_set<std::string> seenIds;
    std::unordered_map<std::string, SiteLogRecord> geoFallback;
    const auto appendGeoFeatures = [&](const std::vector<std::string>& features) {
        for (const auto& feature : features) {
            SiteLogRecord base = ParseM3gGeoJsonFeature(
                feature, definition.networkMask, geoUrl);
            const std::string key = !base.stationId.empty()
                ? base.stationId : base.fourCharId;
            if (key.empty() || !seenIds.insert(key).second) continue;
            stationIds.push_back(key);
            geoFallback.emplace(key, std::move(base));
        }
    };
    appendGeoFeatures(geoFeatures);

    // If pagination=0 was ignored, request subsequent pages until a page
    // contributes no new station.  This also handles a server-side exact
    // multiple of the page size without relying on response headers.
    if (geoFeatures.size() == 20) {
        for (int page = 2; page <= 500 && !stopToken.stop_requested(); ++page) {
            const std::string pageUrl = geoUrl + "&page="
                + std::to_string(page);
            ++stats.directoriesAttempted;
            const auto pageResponse = FetchSiteLogTextWithRetry(
                Utf8ToWide(pageUrl), stopToken, 16U * 1024U * 1024U, 2);
            if (!pageResponse.ok()) break;
            auto pageFeatures = ParseM3gFeatureObjects(pageResponse.body);
            if (pageFeatures.empty()) break;
            const std::size_t before = stationIds.size();
            appendGeoFeatures(pageFeatures);
            if (stationIds.size() == before) break;
            ++stats.directoriesSuccessful;
            if (pageFeatures.size() < 20) break;
        }
    }
    if (stopToken.stop_requested()) {
        stats.error = L"M3G Site Log 扫描已取消；未写入未完成结果。";
        return stats;
    }
    if (stationIds.empty()) {
        stats.error = L"M3G 返回目录，但没有可识别的站标。";
        return stats;
    }
    stats.filesDiscovered = static_cast<int>(stationIds.size());

    // The flat metadata endpoint contains the current receiver, constellation
    // and antenna fields.  Query it in short batches; one request per station
    // would be needlessly slow, while 15 identifiers remain below common URI
    // limits and are accepted by the public API.
    constexpr std::size_t batchSize = 15;
    std::unordered_map<std::string, SiteLogRecord> detailed;
    std::wstring firstError;
    for (std::size_t begin = 0; begin < stationIds.size();
         begin += batchSize) {
        if (stopToken.stop_requested()) {
            stats.error = L"M3G Site Log 扫描已取消；未写入未完成结果。";
            return stats;
        }
        const std::size_t end = std::min(stationIds.size(), begin + batchSize);
        const std::string url = M3gBatchUrl(
            flatUrl, network, stationIds, begin, end);
        ++stats.directoriesAttempted;
        const auto response = FetchSiteLogTextWithRetry(
            Utf8ToWide(url), stopToken, 16U * 1024U * 1024U, 3);
        if (!response.ok()) {
            firstError = response.error.empty()
                ? L"HTTP " + std::to_wstring(response.statusCode)
                : response.error;
            stats.error = L"M3G 设备详情批次读取失败（"
                + std::to_wstring(begin + 1) + L"-"
                + std::to_wstring(end) + L"）：" + firstError;
            return stats;
        }
        ++stats.directoriesSuccessful;
        const auto features = ParseM3gFeatureObjects(response.body);
        if (features.empty()) {
            stats.error = L"M3G 设备详情批次返回空结果（"
                + std::to_wstring(begin + 1) + L"-"
                + std::to_wstring(end) + L"）。";
            return stats;
        }
        stats.filesFetched += static_cast<int>(features.size());
        for (const auto& feature : features) {
            // Keep the request URL out of the provenance directory field.
            // It contains a batch-specific filter and would create hundreds
            // of distinct "directories" for one M3G network.  The stable
            // catalogue endpoint remains the directory; record.sourceUrl is
            // still taken from the feature's actual Site Log link.
            SiteLogRecord record = ParseM3gFeature(
                feature, definition.networkMask, flatUrl, network);
            const std::string key = !record.stationId.empty()
                ? record.stationId : record.fourCharId;
            if (key.empty()) {
                ++stats.filesFailed;
                continue;
            }
            detailed[key] = std::move(record);
        }
        if (progress) {
            progress(static_cast<int>(std::min(end, stationIds.size())),
                     stats.filesDiscovered);
        }
    }

    std::vector<SiteLogRecord> records;
    records.reserve(stationIds.size());
    int fallbackCount = 0;
    for (const auto& stationId : stationIds) {
        const auto detailedIt = detailed.find(stationId);
        if (detailedIt != detailed.end()) {
            records.push_back(detailedIt->second);
            continue;
        }
        const auto fallbackIt = geoFallback.find(stationId);
        if (fallbackIt != geoFallback.end()
            && (!fallbackIt->second.stationId.empty()
                || !fallbackIt->second.fourCharId.empty())) {
            records.push_back(fallbackIt->second);
            ++fallbackCount;
        }
    }
    if (records.empty()) {
        stats.error = L"M3G 设备详情返回成功，但没有可识别的站标。";
        return stats;
    }
    stats.filesParsed = static_cast<int>(records.size());
    if (fallbackCount > 0) {
        stats.warning = L"M3G 有 " + std::to_wstring(fallbackCount)
            + L" 条设备详情未返回，已保留 GeoJSON 基础 Site Log 记录。";
    }

    // The GeoJSON/flat responses above intentionally contain only the
    // current equipment snapshot.  Fetch the linked GeodesyML document for
    // each station so the database has the complete receiver/antenna
    // installation timeline.  Each worker owns one vector element, therefore
    // no shared record mutation is involved; only the warning text/counters
    // are synchronized.  Keep the concurrency bounded because M3G is a
    // public service and jobs can be much larger than the service's sensible
    // request fan-out.
    const int xmlWorkerCount = std::max(1, std::min(jobs, 8));
    std::atomic<std::size_t> nextXml{0};
    std::atomic<int> xmlFetched{0};
    std::atomic<int> xmlWithHistory{0};
    std::atomic<int> xmlFailed{0};
    std::mutex xmlWarningMutex;
    std::wstring firstXmlWarning;
    const auto xmlWorker = [&]() {
        while (!stopToken.stop_requested()) {
            const std::size_t index = nextXml.fetch_add(1);
            if (index >= records.size()) break;
            auto& record = records[index];
            const std::string stationId = !record.stationId.empty()
                ? record.stationId : record.fourCharId;
            const auto fetched = FetchM3gGeodesyMl(stationId, stopToken);
            if (fetched.ok) {
                ++xmlFetched;
                auto history = ParseGeodesyMlEquipmentHistory(fetched.body);
                if (!history.empty()) {
                    for (auto& epoch : history) {
                        epoch.stationId = record.stationId;
                        epoch.fourCharId = record.fourCharId;
                        // Keep the stable Site Log link as the database key.
                        // The GeodesyML URL is a retrieval route, not a new
                        // logical Site Log revision.
                        epoch.sourceUrl = record.sourceUrl;
                        epoch.revisionDate = record.revisionDate;
                        epoch.fetchedAtUtc = record.fetchedAtUtc;
                        epoch.networkMask = record.networkMask;
                    }
                    record.equipmentHistory = std::move(history);
                    ++xmlWithHistory;
                }
                // Embed the machine-readable source in raw_text so a future
                // local --backfill-history/--reparse can reconstruct the same
                // intervals without another network request.
                record.rawText = GaSyntheticSiteLog(record, fetched.body);
            } else {
                ++xmlFailed;
                // Store the flat current snapshot as an undated fallback.
                // StoreSiteLogRecords parses this synthetic text when the
                // explicit history vector is empty, so a transient M3G XML
                // outage never erases usable current metadata.
                record.rawText = GaSyntheticSiteLog(record, {});
                std::scoped_lock lock(xmlWarningMutex);
                if (firstXmlWarning.empty()) {
                    firstXmlWarning = Utf8ToWide(stationId) + L"："
                        + fetched.error;
                }
            }
            const int done = static_cast<int>(index + 1);
            if (progress) progress(done, static_cast<int>(records.size()));
        }
    };
    std::vector<std::jthread> xmlWorkers;
    xmlWorkers.reserve(static_cast<std::size_t>(xmlWorkerCount));
    for (int index = 0; index < xmlWorkerCount; ++index) {
        xmlWorkers.emplace_back(xmlWorker);
    }
    xmlWorkers.clear();
    if (stopToken.stop_requested()) {
        stats.error = L"M3G Site Log 扫描已取消；未写入未完成的 GeodesyML 履历。";
        return stats;
    }
    if (xmlFailed.load() > 0) {
        if (!stats.warning.empty()) stats.warning += L"；";
        stats.warning += L"M3G 有 " + std::to_wstring(xmlFailed.load())
            + L" 个站点未取得完整 GeodesyML 履历，已保留当前快照";
        if (!firstXmlWarning.empty()) {
            stats.warning += L"（首个：" + firstXmlWarning + L"）";
        }
    }
    // `xmlFetched`/`xmlWithHistory` are intentionally not folded into the
    // legacy filesFetched/filesParsed counters: those counters describe the
    // catalogue/detail records, while the XML requests are enrichment routes
    // for the same logical stations.
    (void)xmlFetched;
    (void)xmlWithHistory;
    std::wstring storeError;
    // M3G detail records are upserted on every run.  `refresh` is retained for
    // API symmetry with the directory-based builders; the XML enrichment is
    // likewise refreshed so equipment changes are observed promptly.
    (void)refresh;
    if (!StoreSiteLogRecords(databasePath, records, storeError)) {
        stats.error = std::move(storeError);
        return stats;
    }
    stats.recordsStored = static_cast<int>(records.size());
    if (progress) progress(stats.filesParsed, stats.filesDiscovered);
    return stats;
}

SiteLogBuildStats BuildNativeSiteLogIndex(
    const std::string_view networkKey,
    const std::filesystem::path& databasePath,
    const int jobs,
    const bool refresh,
    const std::stop_token stopToken,
    const std::function<void(int completed, int total)>& progress) {
    SiteLogBuildStats stats;
    stats.databasePath = databasePath;

    const NativeSiteLogDefinition* definition =
        FindNativeSiteLogDefinition(networkKey);
    if (definition == nullptr) {
        stats.error = L"没有为站网 " + Utf8ToWide(Trim(networkKey))
            + L" 配置原生 Site Log 官方来源。";
        return stats;
    }
    if (definition->gaApi) {
        return BuildGaGdcSiteLogIndex(
            databasePath, jobs, refresh, stopToken, progress);
    }
    if (definition->m3gApi) {
        return BuildM3gSiteLogIndex(
            *definition, databasePath, jobs, refresh, stopToken, progress);
    }
    std::wstring schemaError;
    if (!EnsureSiteLogSchema(databasePath, schemaError)) {
        stats.error = std::move(schemaError);
        return stats;
    }

    std::vector<DiscoveredLog> discovered;
    std::unordered_set<std::string> discoveredUrls;
    std::vector<std::wstring> catalogErrors;
    // EPOS-IS (Icelandic Met Office) exposes its station catalogue through a
    // JSON API instead of a browsable .log directory.  Keep the coordinates
    // returned by that API so a sparse/JSON site-log response can still be
    // represented on the map without downloading observation files.
    std::unordered_map<std::string, EposIcelandStationSeed> eposSeeds;
    const auto readCatalog = [&](const std::wstring_view catalogUrl) {
        if (stopToken.stop_requested()) return false;
        ++stats.directoriesAttempted;
        HttpTextResult response;
        if (definition->htmlLinks || definition->httpsListing) {
            response = FetchSiteLogTextWithRetry(
                std::wstring(catalogUrl), stopToken,
                16U * 1024U * 1024U, 3);
        } else {
            // HttpListFtpDirectory is intentionally kept as the listing API
            // (it preserves curl's FTP parsing), while the retry helper wraps
            // the same request semantics for transient data-channel resets.
            for (int attempt = 0; attempt < 3; ++attempt) {
                response = HttpListFtpDirectory(
                    std::wstring(catalogUrl), stopToken,
                    16U * 1024U * 1024U);
                if (response.ok() || !IsRetryableSiteLogResponse(response)
                    || attempt == 2 || stopToken.stop_requested()) {
                    break;
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(250 * (1 << attempt)));
            }
        }
        if (!response.ok()) {
            catalogErrors.push_back(
                Utf8ToWide(WideToUtf8(catalogUrl)) + L"："
                + (response.error.empty()
                    ? L"HTTP/FTP " + std::to_wstring(response.statusCode)
                    : response.error));
            return false;
        }
        ++stats.directoriesSuccessful;
        std::vector<DiscoveredLog> files;
        if (definition->htmlLinks) {
            files = ParseHtmlSiteLogLinks(
                response.body,
                std::wstring(catalogUrl),
                definition->networkMask);
        } else {
            files = ParseDirectory(
                response.body,
                std::wstring(catalogUrl),
                definition->networkMask,
                true);
        }
        for (auto& file : files) {
            AddDiscoveredUrl(
                discovered,
                discoveredUrls,
                file.url,
                file.directory,
                file.networkMask);
        }
        return true;
    };

    if (definition->stationApi) {
        ++stats.directoriesAttempted;
        const HttpTextResult response = FetchSiteLogTextWithRetry(
            std::wstring(definition->catalogUrl), stopToken,
            16U * 1024U * 1024U, 3);
        if (!response.ok()) {
            catalogErrors.push_back(
                Utf8ToWide(WideToUtf8(definition->catalogUrl)) + L"："
                + (response.error.empty()
                    ? L"HTTP " + std::to_wstring(response.statusCode)
                    : response.error));
        } else {
            ++stats.directoriesSuccessful;
            for (auto& seed : ParseEposIcelandStationList(response.body)) {
                if (seed.marker.empty()) continue;
                const std::string marker = UpperAscii(seed.marker);
                eposSeeds.emplace(marker, seed);
                const std::wstring markerWide = Utf8ToWide(marker);
                const std::wstring url =
                    L"https://api.vedur.is/epos/gps/sitelog?station_marker="
                    + markerWide;
                AddDiscoveredUrl(
                    discovered, discoveredUrls, url,
                    L"https://api.vedur.is/epos/gps/",
                    definition->networkMask);
            }
            if (discovered.empty()) {
                catalogErrors.push_back(
                    L"EPOS-IS 站点 API 返回成功，但没有识别到四字符站标。");
            }
        }
    } else {
        const std::wstring primary(definition->catalogUrl);
        const bool primaryRead = readCatalog(primary);
        if ((!primaryRead || discovered.empty())
            && definition->fallbackCatalogUrl != nullptr
            && !stopToken.stop_requested()) {
            readCatalog(definition->fallbackCatalogUrl);
        }
    }

    if (definition->htmlLinks
        && discovered.size() < HongKongFallbackLogUrls().size()
        && !stopToken.stop_requested()) {
        // The HK page is human-oriented and has changed its HTML layout in
        // the past.  The official station-specific URLs are stable, so add
        // the bounded list whenever the page yields only a partial set.
        // AddDiscoveredUrl de-duplicates links already found in the page.
        for (const auto& url : HongKongFallbackLogUrls()) {
            const auto slash = url.find_last_of(L'/');
            const std::wstring directory = slash == std::wstring::npos
                ? url : url.substr(0, slash + 1);
            AddDiscoveredUrl(
                discovered,
                discoveredUrls,
                url,
                directory,
                definition->networkMask);
        }
    }

    stats.filesDiscovered = static_cast<int>(discovered.size());
    if (!catalogErrors.empty()) {
        stats.warning = L"部分原生 Site Log 目录读取失败：";
        for (std::size_t index = 0; index < catalogErrors.size(); ++index) {
            if (index != 0) stats.warning += L"；";
            stats.warning += catalogErrors[index];
        }
    }
    if (discovered.empty()) {
        if (stats.directoriesSuccessful == 0) {
            stats.error = stats.warning.empty()
                ? L"原生 Site Log 目录可访问但没有发现文件。"
                : stats.warning;
        } else {
            stats.warning = stats.warning.empty()
                ? L"原生 Site Log 目录读取成功，但没有发现可识别的 .log/.txt 文件。"
                : stats.warning + L"；目录中没有可识别的 .log/.txt 文件。";
        }
        return stats;
    }

    std::unordered_set<std::string> existingUrls;
    if (!refresh) {
        DatabasePtr database;
        std::wstring error;
        if (OpenDatabase(databasePath, database, error)) {
            auto statement = Prepare(
                database.get(),
                "SELECT source_url FROM site_logs;",
                error);
            if (statement) {
                while (sqlite3_step(statement.get()) == SQLITE_ROW) {
                    existingUrls.insert(
                        LowerAscii(ColumnText(statement.get(), 0)));
                }
            }
        }
    }
    std::vector<DiscoveredLog> pending;
    pending.reserve(discovered.size());
    for (auto& file : discovered) {
        if (refresh || !existingUrls.contains(
                LowerAscii(WideToUtf8(file.url)))) {
            pending.push_back(std::move(file));
        }
    }

    const int workerCount = std::max(1, std::min(jobs, 16));
    std::atomic<std::size_t> next{0};
    std::atomic<int> completed{0};
    std::atomic<int> fetched{0};
    std::atomic<int> parsed{0};
    std::atomic<int> failed{0};
    std::mutex recordsMutex;
    std::mutex errorMutex;
    std::vector<SiteLogRecord> records;
    std::wstring firstFetchError;
    const auto worker = [&]() {
        while (!stopToken.stop_requested()) {
            const std::size_t index = next.fetch_add(1);
            if (index >= pending.size()) break;
            const auto& file = pending[index];
            const HttpTextResult response = FetchSiteLogTextWithRetry(
                file.url, stopToken, 4U * 1024U * 1024U, 3);
            if (!response.ok()) {
                ++failed;
                std::scoped_lock lock(errorMutex);
                if (firstFetchError.empty()) {
                    firstFetchError = response.error.empty()
                        ? L"HTTP " + std::to_wstring(response.statusCode)
                        : response.error;
                }
            } else {
                ++fetched;
                std::string body = response.body;
                if (definition->stationApi) {
                    body = EposIcelandLogBody(body);
                }
                SiteLogRecord record = ParseSiteLogText(
                    body,
                    WideToUtf8(file.url),
                    WideToUtf8(file.directory),
                    file.networkMask);
                if (definition->stationApi) {
                    // The API may return a compact JSON representation or an
                    // ASCII log without a filename.  Fill the stable marker
                    // and country from the request/catalog in either case;
                    // coordinates from the station list are a final fallback.
                    const std::string marker = EposMarkerFromUrl(file.url);
                    const auto seed = eposSeeds.find(marker);
                    if (record.fourCharId.empty() && !marker.empty()) {
                        record.fourCharId = marker;
                    }
                    if (record.stationId.empty() && !marker.empty()) {
                        record.stationId = marker + "00ISL";
                    }
                    if (record.country.empty()) record.country = "ISL";
                    if (record.fileName.empty() || record.fileName == "sitelog") {
                        record.fileName = marker.empty()
                            ? "epos_is_sitelog.log"
                            : marker + "_sitelog.log";
                    }
                    if (!record.hasCoordinate && seed != eposSeeds.end()
                        && seed->second.hasCoordinate) {
                        record.latitude = seed->second.latitude;
                        record.longitude = seed->second.longitude;
                        record.hasCoordinate = true;
                    }
                }
                if (!record.stationId.empty() || !record.fourCharId.empty()) {
                    ++parsed;
                    std::scoped_lock lock(recordsMutex);
                    records.push_back(std::move(record));
                } else {
                    ++failed;
                }
            }
            const int done = ++completed;
            if (progress) progress(done, static_cast<int>(pending.size()));
        }
    };
    std::vector<std::jthread> workers;
    workers.reserve(static_cast<std::size_t>(workerCount));
    for (int index = 0; index < workerCount; ++index) {
        workers.emplace_back(worker);
    }
    workers.clear();

    stats.filesFetched = fetched.load();
    stats.filesParsed = parsed.load();
    stats.filesFailed = failed.load();
    if (!firstFetchError.empty()) {
        if (!stats.warning.empty()) stats.warning += L"；";
        stats.warning += L"部分原生 Site Log 文件读取失败：" + firstFetchError;
    }
    if (stopToken.stop_requested()) {
        stats.error = L"原生 Site Log 扫描已取消；本次未写入未完成文件。";
        return stats;
    }
    if (!pending.empty() && records.empty() && failed.load() > 0) {
        // Do not report a network outage as a successful zero-row build.  Any
        // rows already present in the database are intentionally retained;
        // the caller can rerun without --refresh after fixing the endpoint.
        stats.error = L"原生 Site Log 文件均未能读取或解析；本次未写入新记录。";
        return stats;
    }
    std::wstring storeError;
    if (!StoreSiteLogRecords(databasePath, records, storeError)) {
        stats.error = std::move(storeError);
        return stats;
    }
    stats.recordsStored = static_cast<int>(records.size());
    return stats;
}

std::vector<std::string> NativeSiteLogNetworkKeys() {
    std::vector<std::string> result;
    result.reserve(std::size(kNativeSiteLogDefinitions));
    for (const auto& definition : kNativeSiteLogDefinitions) {
        result.emplace_back(definition.key);
    }
    return result;
}

SiteLogPropagationStats PropagateSiteLogsToNetworkDatabases(
    const std::filesystem::path& sourceDatabasePath,
    const std::vector<SiteLogPropagationTarget>& targets) {
    SiteLogPropagationStats stats;
    stats.targetsSeen = static_cast<int>(targets.size());

    std::vector<SiteLogRecord> sourceRecords;
    std::wstring sourceError;
    if (!LoadSiteLogRecords(sourceDatabasePath, sourceRecords, sourceError)) {
        stats.error = L"无法读取共享 Site Log：" + sourceError;
        return stats;
    }
    if (sourceRecords.empty()) return stats;

    const auto normalized = [](std::string value) {
        return UpperAscii(Trim(value));
    };
    const auto appendWarning = [&](const std::wstring& message) {
        if (message.empty()) return;
        if (!stats.warning.empty()) stats.warning += L"；";
        stats.warning += message;
    };
    std::error_code pathError;
    const auto sourceAbsolute = std::filesystem::absolute(
        sourceDatabasePath, pathError).lexically_normal();

    for (const auto& target : targets) {
        pathError.clear();
        if (target.databasePath.empty()
            || !std::filesystem::is_regular_file(target.databasePath, pathError)) {
            // A network that has not been built yet has no database to enrich.
            continue;
        }
        pathError.clear();
        const auto targetAbsolute = std::filesystem::absolute(
            target.databasePath, pathError).lexically_normal();
        if (targetAbsolute == sourceAbsolute) continue;

        DatabasePtr database;
        std::wstring error;
        if (!OpenDatabase(target.databasePath, database, error)) {
            appendWarning(
                L"" + Utf8ToWide(target.networkKey)
                + L"：无法打开数据库（" + error + L"）");
            continue;
        }

        // Product databases also live beside observation databases.  Only a
        // database with daily_files is an observation knowledge base and is
        // eligible for Site Log propagation.
        auto table = Prepare(
            database.get(),
            "SELECT 1 FROM sqlite_master WHERE type='table'"
            " AND name='daily_files' LIMIT 1;",
            error);
        if (!table) {
            appendWarning(
                L"" + Utf8ToWide(target.networkKey)
                + L"：无法检查观测表（" + error + L"）");
            continue;
        }
        if (sqlite3_step(table.get()) != SQLITE_ROW) continue;

        std::unordered_set<std::string> stationIds;
        std::unordered_map<std::string, std::unordered_set<std::string>>
            stationsByFour;
        auto stationRows = Prepare(
            database.get(),
            "SELECT DISTINCT station_id,four_char_id FROM daily_files"
            " WHERE station_id<>'' OR four_char_id<>'';",
            error);
        if (!stationRows) {
            appendWarning(
                L"" + Utf8ToWide(target.networkKey)
                + L"：无法读取测站标识（" + error + L"）");
            continue;
        }
        while (true) {
            const int status = sqlite3_step(stationRows.get());
            if (status == SQLITE_DONE) break;
            if (status != SQLITE_ROW) {
                appendWarning(
                    L"" + Utf8ToWide(target.networkKey)
                    + L"：读取测站标识失败");
                stationIds.clear();
                stationsByFour.clear();
                break;
            }
            const std::string stationId = normalized(
                ColumnText(stationRows.get(), 0));
            const std::string fourCharId = normalized(
                ColumnText(stationRows.get(), 1));
            if (!stationId.empty()) stationIds.insert(stationId);
            if (!fourCharId.empty()) {
                stationsByFour[fourCharId].insert(stationId);
            }
        }
        if (stationIds.empty() && stationsByFour.empty()) continue;

        std::vector<SiteLogRecord> matching;
        matching.reserve(sourceRecords.size());
        std::unordered_set<std::string> matchedKeys;
        for (const auto& record : sourceRecords) {
            const std::string stationId = normalized(record.stationId);
            const std::string fourCharId = normalized(record.fourCharId);
            const bool exactMatch = !stationId.empty()
                && stationIds.contains(stationId);
            bool fourMatch = false;
            if (!exactMatch && !fourCharId.empty()) {
                const auto found = stationsByFour.find(fourCharId);
                // A four-character code is used only when it identifies one
                // physical station in this network.  This avoids attaching a
                // Site Log to the wrong station when a regional archive
                // reuses a four-character code.
                fourMatch = found != stationsByFour.end()
                    && (found->second.size() == 1
                        || (found->second.size() == 0 && stationId.empty()));
            }
            if (!exactMatch && !fourMatch) continue;
            matching.push_back(record);
            const std::string key = !stationId.empty()
                ? stationId : fourCharId;
            if (!key.empty()) matchedKeys.insert(key);
        }
        if (matching.empty()) continue;

        std::wstring storeError;
        if (!StoreSiteLogRecords(
                target.databasePath, matching, storeError)) {
            appendWarning(
                L"" + Utf8ToWide(target.networkKey)
                + L"：Site Log 写入失败（" + storeError + L"）");
            continue;
        }
        ++stats.databasesUpdated;
        stats.stationKeysMatched += static_cast<int>(matchedKeys.size());
        stats.recordsStored += static_cast<int>(matching.size());
    }
    return stats;
}
