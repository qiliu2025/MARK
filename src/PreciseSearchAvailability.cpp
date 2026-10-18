#include "PreciseSearchAvailability.h"

#include "BroadcastAvailability.h"
#include "AttAvailability.h"
#include "ClkAvailability.h"
#include "DateUtil.h"
#include "DcbAvailability.h"
#include "ErpAvailability.h"
#include "IgsAvailabilityDb.h"
#include "IonexAvailability.h"
#include "OsbAvailability.h"
#include "SinexAvailability.h"
#include "Sp3Availability.h"
#include "TroAvailability.h"
#include "UpdAvailability.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "sqlite3.h"

namespace {

constexpr int kSp3Category = 1;
constexpr int kClkCategory = 2;
constexpr int kErpCategory = 3;
constexpr int kDcbCategory = 4;
constexpr int kAttCategory = 5;
constexpr int kIonexCategory = 6;
constexpr int kTroCategory = 7;
constexpr int kSinexCategory = 8;
constexpr int kBroadcastCategory = 10;
constexpr int kOsbCategory = 11;
constexpr int kUpdCategory = 12;

struct DbCloser {
    void operator()(sqlite3* database) const noexcept {
        if (database != nullptr) sqlite3_close(database);
    }
};
struct StmtCloser {
    void operator()(sqlite3_stmt* statement) const noexcept {
        if (statement != nullptr) sqlite3_finalize(statement);
    }
};
using DbPtr = std::unique_ptr<sqlite3, DbCloser>;
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtCloser>;

struct FileStamp {
    bool exists = false;
    std::uintmax_t size = 0;
    std::int64_t modified = 0;
    std::uintmax_t walSize = 0;
    std::int64_t walModified = 0;
    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};

struct CachedFamily {
    std::string key;
    PreciseProductFile product;
    std::string series;
    std::string productLine;
    std::string referenceFrequency;
    std::string biasConvention;
    std::string referenceFrame;
    std::string apcModel;
    int revisionWeek = -1;
    bool currentAlias = false;
    int baseOrdinal = 0;
    int bitCount = 0;
    std::vector<unsigned char> bits;
};

struct CachedScan {
    int ordinal = 0;
    bool complete = false;
};

struct Cache {
    std::filesystem::path path;
    FileStamp stamp;
    std::wstring error;
    std::vector<CachedFamily> families;
    std::unordered_map<std::string, std::size_t> familyIndex;
    std::array<std::vector<CachedScan>, 12> scans;
    bool fixedCutoff = false;
};

std::mutex gCacheMutex;
std::unordered_map<std::wstring, std::shared_ptr<Cache>> gCaches;

std::wstring EnvironmentPath(const wchar_t* name) {
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0) return {};
    std::wstring value(length, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), length);
    if (copied == 0) return {};
    value.resize(copied);
    return value;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), required);
    return result;
}

std::string ColumnText(sqlite3_stmt* statement, const int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value == nullptr ? std::string{}
        : reinterpret_cast<const char*>(value);
}

std::wstring DbError(sqlite3* database, const wchar_t* action) {
    const char* message = database == nullptr ? "unknown SQLite error"
        : sqlite3_errmsg(database);
    return std::wstring(action) + L"：" + Utf8ToWide(message ? message : "");
}

StmtPtr Prepare(sqlite3* database, const char* sql, std::wstring& error) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK) {
        error = DbError(database, L"无法准备精密产品快速索引查询");
        return {};
    }
    return StmtPtr(raw);
}

bool BindText(sqlite3_stmt* statement, const int index, const std::string& value) {
    return sqlite3_bind_text(statement, index, value.c_str(),
        static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

bool BindInt(sqlite3_stmt* statement, const int index, const int value) {
    return sqlite3_bind_int(statement, index, value) == SQLITE_OK;
}

bool OpenReadDatabase(
    const std::filesystem::path& path, DbPtr& database, std::wstring& error,
    const wchar_t* label = L"精密产品知识库") {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        error = std::wstring(label) + L"文件不存在：" + path.wstring();
        return false;
    }
    sqlite3* raw = nullptr;
    const auto utf8PathValue = path.u8string();
    const std::string utf8Path(
        reinterpret_cast<const char*>(utf8PathValue.data()),
        utf8PathValue.size());
    const int status = sqlite3_open_v2(
        utf8Path.c_str(), &raw,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    database.reset(raw);
    if (status != SQLITE_OK || raw == nullptr) {
        error = DbError(raw, label);
        return false;
    }
    sqlite3_busy_timeout(raw, 10000);
    return true;
}

std::int64_t FileTimeValue(const std::filesystem::path& path) {
    // The Python side-index builder records POSIX nanoseconds.  The
    // filesystem clock used by MSVC has a different epoch, so comparing
    // `file_time_type::time_since_epoch()` directly would invalidate every
    // freshly-built summary.  Read the native FILETIME and convert its
    // Windows-epoch 100 ns ticks to the same Unix nanosecond representation.
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard,
                              &attributes)) {
        return 0;
    }
    ULARGE_INTEGER ticks{};
    ticks.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
    ticks.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
    constexpr std::uint64_t kUnixEpochInFileTime = 116444736000000000ULL;
    if (ticks.QuadPart < kUnixEpochInFileTime) return 0;
    return static_cast<std::int64_t>(
        (ticks.QuadPart - kUnixEpochInFileTime) * 100ULL);
}

FileStamp GetFileStamp(const std::filesystem::path& path) {
    FileStamp result;
    std::error_code ec;
    result.exists = std::filesystem::is_regular_file(path, ec);
    if (result.exists) {
        ec.clear();
        result.size = std::filesystem::file_size(path, ec);
        result.modified = FileTimeValue(path);
    }
    const auto wal = std::filesystem::path(path.wstring() + L"-wal");
    ec.clear();
    if (std::filesystem::is_regular_file(wal, ec)) {
        result.walSize = std::filesystem::file_size(wal, ec);
        result.walModified = FileTimeValue(wal);
    }
    return result;
}

std::chrono::sys_days ToSysDays(const QueryDate& date) {
    return std::chrono::sys_days{
        std::chrono::year{date.year}
            / std::chrono::month{static_cast<unsigned>(date.month)}
            / std::chrono::day{static_cast<unsigned>(date.day)}};
}

QueryDate FromSysDays(const std::chrono::sys_days date) {
    const std::chrono::year_month_day ymd{date};
    return MakeQueryDate(
        static_cast<int>(ymd.year()),
        static_cast<unsigned>(ymd.month()),
        static_cast<unsigned>(ymd.day()));
}

int DateOrdinal(const QueryDate& date) {
    const auto base = std::chrono::sys_days{
        std::chrono::year{1980} / std::chrono::January / std::chrono::day{1}};
    return static_cast<int>((ToSysDays(date) - base).count());
}

bool ValidRange(
    const QueryDate& start, const QueryDate& end, int& expected,
    int& firstOrdinal, int& lastOrdinal, std::wstring& error) {
    const auto first = ToSysDays(start);
    const auto last = ToSysDays(end);
    const auto span = (last - first).count();
    if (span < 0 || span > std::numeric_limits<int>::max() - 1LL) {
        error = L"精密产品日期范围无效";
        return false;
    }
    expected = static_cast<int>(span) + 1;
    firstOrdinal = DateOrdinal(start);
    lastOrdinal = DateOrdinal(end);
    return true;
}

int ScanSlot(const int category) {
    return category >= 1 && category <= 12 && category != 9
        ? category - 1 : -1;
}

std::filesystem::path SourcePath(const int category) {
    switch (category) {
    case kSp3Category: return GetSp3AvailabilityDatabasePath();
    case kClkCategory: return GetClkAvailabilityDatabasePath();
    case kErpCategory: return GetErpAvailabilityDatabasePath();
    case kDcbCategory: return GetDcbAvailabilityDatabasePath();
    case kAttCategory: return GetAttAvailabilityDatabasePath();
    case kIonexCategory: return GetIonexAvailabilityDatabasePath();
    case kTroCategory: return GetTroAvailabilityDatabasePath();
    case kSinexCategory: return GetSinexAvailabilityDatabasePath();
    case kBroadcastCategory: return GetBroadcastAvailabilityDatabasePath();
    case kOsbCategory: return GetOsbAvailabilityDatabasePath();
    case kUpdCategory: return GetUpdAvailabilityDatabasePath();
    default: return {};
    }
}

std::shared_ptr<Cache> LoadCache() {
    const auto absolute = std::filesystem::absolute(
        GetPreciseSearchAvailabilityDatabasePath());
    const auto stamp = GetFileStamp(absolute);
    const auto cacheKey = absolute.wstring();
    std::scoped_lock lock(gCacheMutex);
    if (const auto found = gCaches.find(cacheKey); found != gCaches.end()
        && found->second != nullptr && found->second->stamp == stamp) {
        return found->second;
    }
    auto cache = std::make_shared<Cache>();
    cache->path = absolute;
    cache->stamp = stamp;
    if (!stamp.exists || stamp.size == 0) {
        cache->error = L"精密产品索引缺失或为空，请重新安装官方知识库后重试。";
        gCaches[cacheKey] = cache;
        return cache;
    }
    DbPtr database;
    if (!OpenReadDatabase(absolute, database, cache->error,
                          L"精密产品快速摘要")) {
        gCaches[cacheKey] = cache;
        return cache;
    }
    auto cutoff = Prepare(database.get(),
        "SELECT value FROM metadata WHERE key='data_cutoff_date';", cache->error);
    cache->fixedCutoff = cutoff && sqlite3_step(cutoff.get()) == SQLITE_ROW
        && !ColumnText(cutoff.get(), 0).empty();
    auto families = Prepare(database.get(),
        "SELECT category,family_key,id,name,latency,center,constellation,"
        "interval,status,base_ordinal,bit_count,day_bits,series,product_line,"
        "reference_frequency,bias_convention,reference_frame,apc_model,"
        "revision_week,current_alias FROM families;",
        cache->error);
    if (!families) {
        gCaches[cacheKey] = cache;
        return cache;
    }
    while (true) {
        const int status = sqlite3_step(families.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            cache->error = DbError(database.get(), L"读取精密产品产品族摘要失败");
            cache->families.clear();
            cache->familyIndex.clear();
            gCaches[cacheKey] = cache;
            return cache;
        }
        const int category = sqlite3_column_int(families.get(), 0);
        if (ScanSlot(category) < 0) continue;
        CachedFamily family;
        family.key = ColumnText(families.get(), 1);
        family.product.id = Utf8ToWide(ColumnText(families.get(), 2));
        family.product.name = Utf8ToWide(ColumnText(families.get(), 3));
        family.product.category = static_cast<PreciseProductCategory>(category - 1);
        family.product.latency = static_cast<PreciseProductLatency>(
            std::clamp(sqlite3_column_int(families.get(), 4), 0, 9));
        family.product.center = Utf8ToWide(ColumnText(families.get(), 5));
        family.product.constellation = Utf8ToWide(ColumnText(families.get(), 6));
        family.product.interval = Utf8ToWide(ColumnText(families.get(), 7));
        if (family.product.category == PreciseProductCategory::BroadcastNavigation) {
            family.product.formatVersion = std::move(family.product.interval);
            family.product.interval.clear();
        }
        family.product.status = Utf8ToWide(ColumnText(families.get(), 8));
        family.product.localInventoryOnly = category == 12
            && family.product.status.starts_with(L"Local UPD inventory;");
        family.baseOrdinal = sqlite3_column_int(families.get(), 9);
        family.bitCount = sqlite3_column_int(families.get(), 10);
        family.series = ColumnText(families.get(), 12);
        family.productLine = ColumnText(families.get(), 13);
        family.referenceFrequency = ColumnText(families.get(), 14);
        family.biasConvention = ColumnText(families.get(), 15);
        family.referenceFrame = ColumnText(families.get(), 16);
        family.apcModel = ColumnText(families.get(), 17);
        family.revisionWeek = sqlite3_column_int(families.get(), 18);
        family.currentAlias = sqlite3_column_int(families.get(), 19) != 0;
        const auto* blob = sqlite3_column_blob(families.get(), 11);
        const int bytes = sqlite3_column_bytes(families.get(), 11);
        if (blob != nullptr && bytes > 0) {
            const auto* first = static_cast<const unsigned char*>(blob);
            family.bits.assign(first, first + bytes);
        }
        const auto [it, inserted] = cache->familyIndex.emplace(
            std::format("{}|{}", category, family.key), cache->families.size());
        if (inserted) cache->families.push_back(std::move(family));
    }
    auto scans = Prepare(database.get(),
        "SELECT category,date_ordinal,complete FROM scans ORDER BY category,date_ordinal;",
        cache->error);
    if (!scans) {
        cache->families.clear();
        cache->familyIndex.clear();
        gCaches[cacheKey] = cache;
        return cache;
    }
    while (true) {
        const int status = sqlite3_step(scans.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            cache->error = DbError(database.get(), L"读取精密产品日期摘要失败");
            cache->families.clear();
            cache->familyIndex.clear();
            gCaches[cacheKey] = cache;
            return cache;
        }
        const int slot = ScanSlot(sqlite3_column_int(scans.get(), 0));
        if (slot >= 0) cache->scans[slot].push_back({
            sqlite3_column_int(scans.get(), 1),
            sqlite3_column_int(scans.get(), 2) != 0});
    }
    // The summary is a read-only materialization of the authoritative
    // indexes.  Refuse to serve it after any source database changed rather
    // than silently returning an old availability answer.  Size and WAL size
    // are deliberately used here: unlike filesystem clock epochs, they are
    // stable between the Python builder and this Windows reader.
    auto sourceStamps = Prepare(database.get(),
        "SELECT category,path,size,mtime_ns,wal_size,wal_mtime_ns FROM source_files;",
        cache->error);
    if (!sourceStamps) {
        cache->families.clear();
        cache->familyIndex.clear();
        gCaches[cacheKey] = cache;
        return cache;
    }
    while (true) {
        const int status = sqlite3_step(sourceStamps.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            cache->error = DbError(database.get(), L"读取精密产品摘要来源版本失败");
            cache->families.clear();
            cache->familyIndex.clear();
            gCaches[cacheKey] = cache;
            return cache;
        }
        const int category = sqlite3_column_int(sourceStamps.get(), 0);
        // The recorded path is provenance from the machine that built the
        // summary.  It is intentionally not used as an identity check:
        // portable DuckDB bundles may be moved to another directory, and a
        // .sqlite compatibility alias may resolve to the sibling .duckdb
        // file.  The active, environment/bundle-resolved source path below
        // is the one whose signature must match.
        (void)ColumnText(sourceStamps.get(), 1);
        const auto sourcePath = SourcePath(category);
        const auto expectedSize = static_cast<std::uintmax_t>(
            sqlite3_column_int64(sourceStamps.get(), 2));
        const auto expectedModified = static_cast<std::int64_t>(
            sqlite3_column_int64(sourceStamps.get(), 3));
        const auto expectedWalSize = static_cast<std::uintmax_t>(
            sqlite3_column_int64(sourceStamps.get(), 4));
        const auto expectedWalModified = static_cast<std::int64_t>(
            sqlite3_column_int64(sourceStamps.get(), 5));
        const auto current = GetFileStamp(sourcePath);
        // SQLite commonly leaves an empty `-wal` sidecar behind after a
        // checkpoint.  Its timestamp is not a content version when the
        // recorded WAL size is zero, so only compare WAL timestamps for a
        // non-empty sidecar.
        const bool walMatches = expectedWalSize == 0
            ? current.walSize == 0
            : current.walSize == expectedWalSize
                && current.walModified == expectedWalModified;
        if (!current.exists || current.size != expectedSize
            || current.modified != expectedModified || !walMatches) {
            cache->error = L"精密产品索引与知识库不匹配，请重新安装官方知识库后重试。";
            cache->families.clear();
            cache->familyIndex.clear();
            gCaches[cacheKey] = cache;
            return cache;
        }
    }
    gCaches[cacheKey] = cache;
    return cache;
}

bool HasBit(const CachedFamily& family, const int ordinal) {
    const int offset = ordinal - family.baseOrdinal;
    if (offset < 0 || offset >= family.bitCount
        || offset / 8 >= static_cast<int>(family.bits.size())) return false;
    return (family.bits[static_cast<std::size_t>(offset / 8)]
        & static_cast<unsigned char>(1U << (offset & 7))) != 0;
}

// Count availability bits directly by byte instead of probing every day in
// the requested range.  A long all-products query can contain thousands of
// families and several thousand days; the old ordinal loop therefore did
// tens of millions of HasBit() calls even though the source is already a
// packed bitmap.  This keeps the work proportional to the bitmap bytes.
int CountBitsInRange(
    const CachedFamily& family,
    const int firstOrdinal,
    const int lastOrdinal) {
    if (family.bitCount <= 0 || family.bits.empty() || lastOrdinal < firstOrdinal) {
        return 0;
    }
    const int bitmapLast = family.baseOrdinal + family.bitCount - 1;
    const int first = std::max(firstOrdinal, family.baseOrdinal);
    const int last = std::min(lastOrdinal, bitmapLast);
    if (last < first) return 0;
    const int firstOffset = first - family.baseOrdinal;
    const int lastOffset = last - family.baseOrdinal;
    const std::size_t firstByte = static_cast<std::size_t>(firstOffset / 8);
    const std::size_t lastByte = static_cast<std::size_t>(lastOffset / 8);
    if (firstByte >= family.bits.size()) return 0;
    const std::size_t boundedLast = std::min(lastByte, family.bits.size() - 1U);
    int count = 0;
    for (std::size_t byte = firstByte; byte <= boundedLast; ++byte) {
        unsigned mask = 0xffU;
        if (byte == firstByte) {
            mask &= 0xffU << static_cast<unsigned>(firstOffset & 7);
        }
        if (byte == lastByte) {
            const unsigned high = static_cast<unsigned>(lastOffset & 7);
            mask &= high == 7U ? 0xffU : ((1U << (high + 1U)) - 1U);
        }
        count += std::popcount(static_cast<unsigned>(family.bits[byte]) & mask);
    }
    return count;
}

template <typename Callback>
void ForEachSetBitInRange(
    const CachedFamily& family,
    const int firstOrdinal,
    const int lastOrdinal,
    Callback&& callback) {
    if (family.bitCount <= 0 || family.bits.empty() || lastOrdinal < firstOrdinal) {
        return;
    }
    const int bitmapLast = family.baseOrdinal + family.bitCount - 1;
    const int first = std::max(firstOrdinal, family.baseOrdinal);
    const int last = std::min(lastOrdinal, bitmapLast);
    if (last < first) return;
    const int firstOffset = first - family.baseOrdinal;
    const int lastOffset = last - family.baseOrdinal;
    const std::size_t firstByte = static_cast<std::size_t>(firstOffset / 8);
    const std::size_t lastByte = static_cast<std::size_t>(lastOffset / 8);
    if (firstByte >= family.bits.size()) return;
    const std::size_t boundedLast = std::min(lastByte, family.bits.size() - 1U);
    for (std::size_t byte = firstByte; byte <= boundedLast; ++byte) {
        unsigned mask = 0xffU;
        if (byte == firstByte) {
            mask &= 0xffU << static_cast<unsigned>(firstOffset & 7);
        }
        if (byte == lastByte) {
            const unsigned high = static_cast<unsigned>(lastOffset & 7);
            mask &= high == 7U ? 0xffU : ((1U << (high + 1U)) - 1U);
        }
        unsigned value = static_cast<unsigned>(family.bits[byte]) & mask;
        while (value != 0U) {
            const unsigned bit = std::countr_zero(value);
            callback(family.baseOrdinal + static_cast<int>(byte * 8U + bit));
            value &= value - 1U;
        }
    }
}

bool CompleteScan(const std::vector<CachedScan>& scans, const int ordinal) {
    const auto found = std::ranges::lower_bound(scans, ordinal,
        {}, &CachedScan::ordinal);
    return found != scans.end() && found->ordinal == ordinal && found->complete;
}

bool IndexedScan(const std::vector<CachedScan>& scans, const int ordinal) {
    const auto found = std::ranges::lower_bound(scans, ordinal,
        {}, &CachedScan::ordinal);
    return found != scans.end() && found->ordinal == ordinal;
}

PreciseProductMirror Mirror(
    sqlite3_stmt* statement, const bool hasArchiveClass) {
    (void)hasArchiveClass;
    return {
        Utf8ToWide(ColumnText(statement, 0)),
        Utf8ToWide(ColumnText(statement, 3)),
        Utf8ToWide(ColumnText(statement, 2))};
}

} // namespace

std::filesystem::path GetPreciseSearchAvailabilityDatabasePath() {
    const auto explicitPath = EnvironmentPath(
        L"GNSS_DOWNLOADER_PRECISE_SEARCH_DB_PATH");
    if (!explicitPath.empty()) return std::filesystem::path(explicitPath);
    const auto directory = EnvironmentPath(L"GNSS_DOWNLOADER_INDEX_DIR");
    if (!directory.empty()) {
        return ResolveDatabaseFilePath(
            std::filesystem::path(directory) / L"precise_search.sqlite");
    }
    return ResolveDatabaseFilePath(GetIgsAvailabilityDatabasePath().parent_path()
        / L"precise_search.sqlite");
}

bool PreciseSearchHasFixedCutoff() {
    const auto cache = LoadCache();
    return cache && cache->fixedCutoff;
}

PreciseSearchCatalogLookup LoadPreciseSearchCatalog() {
    PreciseSearchCatalogLookup result;
    const auto cache = LoadCache();
    if (!cache || !cache->error.empty()) {
        result.error = cache == nullptr ? L"无法加载精密产品快速摘要" : cache->error;
        return result;
    }
    result.products.reserve(cache->families.size());
    for (const auto& family : cache->families) {
        if (!std::ranges::any_of(family.bits, [](const auto byte) { return byte != 0; })) continue;
        result.products.push_back(family.product);
        result.products.back().mirrors.clear();
    }
    return result;
}

PreciseSearchRangeLookup LoadPreciseSearchFamilyRange(
    const int categoryIndex, const QueryDate& startDate,
    const QueryDate& endDate, const bool includeAvailableDates,
    const std::string& onlyFamilyKey) {
    PreciseSearchRangeLookup result;
    result.databasePath = GetPreciseSearchAvailabilityDatabasePath();
    int firstOrdinal = 0;
    int lastOrdinal = 0;
    if (!ValidRange(startDate, endDate, result.expectedDays,
                   firstOrdinal, lastOrdinal, result.error)) return result;
    if (ScanSlot(categoryIndex) < 0) {
        result.error = L"该产品类型没有精密快速摘要实现";
        return result;
    }
    const auto cache = LoadCache();
    if (!cache || !cache->error.empty()) {
        result.error = cache == nullptr ? L"无法加载精密产品快速摘要" : cache->error;
        return result;
    }
    const auto& scans = cache->scans[ScanSlot(categoryIndex)];
    const auto epoch = std::chrono::sys_days{
        std::chrono::year{1980} / std::chrono::January / std::chrono::day{1}};
    if (!scans.empty()) {
        result.hasHistoryStart = true;
        result.historyStart = FromSysDays(epoch + std::chrono::days{scans.front().ordinal});
    }
    for (int ordinal = firstOrdinal; ordinal <= lastOrdinal; ++ordinal) {
        if (IndexedScan(scans, ordinal)) ++result.indexedDays;
        if (!scans.empty() && ordinal < scans.front().ordinal) {
            result.beforeHistoryDates.push_back(FromSysDays(epoch + std::chrono::days{ordinal}));
            continue;
        }
        if (!CompleteScan(scans, ordinal)) {
            result.incompleteDates.push_back(FromSysDays(
                std::chrono::sys_days{
                    std::chrono::year{1980} / std::chrono::January / std::chrono::day{1}
                } + std::chrono::days{ordinal}));
        }
    }
    result.complete = result.incompleteDates.empty() && result.beforeHistoryDates.empty();
    for (const auto& family : cache->families) {
        if (!onlyFamilyKey.empty() && family.key != onlyFamilyKey) continue;
        if (family.product.category != static_cast<PreciseProductCategory>(
                categoryIndex - 1)) continue;
        const int available = CountBitsInRange(
            family, firstOrdinal, lastOrdinal);
        if (available == 0) continue;
        PreciseSearchFamilyRecord row;
        row.familyKey = family.key;
        row.representative = family.product;
        row.representative.mirrors.clear();
        row.availableDays = available;
        row.series = family.series;
        row.productLine = family.productLine;
        row.referenceFrequency = family.referenceFrequency;
        row.biasConvention = family.biasConvention;
        row.referenceFrame = family.referenceFrame;
        row.apcModel = family.apcModel;
        row.revisionWeek = family.revisionWeek;
        row.currentAlias = family.currentAlias;
        if (includeAvailableDates) {
            row.availableDates.reserve(static_cast<std::size_t>(available));
            ForEachSetBitInRange(
                family, firstOrdinal, lastOrdinal,
                [&row](const int ordinal) {
                    row.availableDates.push_back(FromSysDays(
                        std::chrono::sys_days{
                            std::chrono::year{1980} / std::chrono::January
                                / std::chrono::day{1}}
                        + std::chrono::days{ordinal}));
                });
        }
        result.families.push_back(std::move(row));
    }
    std::ranges::sort(result.families,
        [](const auto& left, const auto& right) {
            if (left.representative.latency != right.representative.latency) {
                return left.representative.latency < right.representative.latency;
            }
            if (left.representative.center != right.representative.center) {
                return left.representative.center < right.representative.center;
            }
            return left.representative.id < right.representative.id;
        });
    return result;
}

PreciseSearchRoutesLookup LoadPreciseSearchFamilyRoutes(
    const int categoryIndex, const std::string& familyKey,
    const QueryDate& startDate, const QueryDate& endDate,
    const std::stop_token stopToken, const PreciseSearchRouteProgress progress) {
    PreciseSearchRoutesLookup result;
    result.databasePath = SourcePath(categoryIndex);
    int expected = 0;
    int firstOrdinal = 0;
    int lastOrdinal = 0;
    if (!ValidRange(startDate, endDate, expected, firstOrdinal, lastOrdinal,
                   result.error)) return result;
    if (ScanSlot(categoryIndex) < 0) {
        result.error = L"该产品类型没有精密快速摘要实现";
        return result;
    }
    const auto summaryPath = GetPreciseSearchAvailabilityDatabasePath();
    DbPtr summary;
    if (!OpenReadDatabase(summaryPath, summary, result.error,
                          L"精密产品快速摘要")) return result;
    auto days = Prepare(summary.get(),
        "SELECT date_ordinal,logical_key FROM family_days "
        "WHERE category=?1 AND family_key=?2 AND date_ordinal BETWEEN ?3 AND ?4 "
        "ORDER BY date_ordinal,logical_key;", result.error);
    if (!days || !BindInt(days.get(), 1, categoryIndex)
        || !BindText(days.get(), 2, familyKey)
        || !BindInt(days.get(), 3, firstOrdinal)
        || !BindInt(days.get(), 4, lastOrdinal)) {
        return result;
    }
    struct DayKey { int ordinal = 0; std::string logicalKey; };
    std::vector<DayKey> dayKeys;
    while (true) {
        const int status = sqlite3_step(days.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            result.error = DbError(summary.get(), L"读取精密产品下载日期映射失败");
            return result;
        }
        dayKeys.push_back({sqlite3_column_int(days.get(), 0), ColumnText(days.get(), 1)});
    }
    if (progress) progress(0, dayKeys.size());
    DbPtr source;
    if (!OpenReadDatabase(result.databasePath, source, result.error,
                          L"精密产品原始知识库")) return result;
    // Do not hydrate one logical key at a time.  The SQLite build happened
    // to have an index that made that approach acceptable, but the DuckDB
    // compatibility layer executes each prepared statement as a fresh query
    // and a point lookup becomes a full scan.  A decade-long selection can
    // therefore turn into tens of thousands of scans.  We collect the keys
    // first and fetch them in bounded IN-list batches instead.
    std::map<int, std::size_t> byDay;
    std::unordered_map<std::string, std::vector<std::size_t>> recordsByKey;
    std::vector<std::string> standardKeys;
    std::vector<std::string> eopKeys;
    std::vector<std::string> updKeys;
    std::unordered_set<std::string> seenStandard;
    std::unordered_set<std::string> seenEop;
    std::unordered_set<std::string> seenUpd;
    std::size_t completed = 0;
    result.records.reserve(dayKeys.size());
    for (const auto& day : dayKeys) {
        if (stopToken.stop_requested()) {
            result.error = L"精密产品下载路线读取已取消";
            result.records.clear();
            return result;
        }
        constexpr std::string_view eopPrefix{"@EOP@"};
        const bool isEop = categoryIndex == kErpCategory
            && day.logicalKey.starts_with(eopPrefix);
        const std::string actualKey = isEop
            ? day.logicalKey.substr(eopPrefix.size()) : day.logicalKey;
        auto [found, inserted] = byDay.emplace(day.ordinal, result.records.size());
        if (inserted) {
            PreciseSearchDatedRoutes item;
            item.date = FromSysDays(std::chrono::sys_days{
                std::chrono::year{1980} / std::chrono::January / std::chrono::day{1}
            } + std::chrono::days{day.ordinal});
            item.logicalKey = actualKey;
            result.records.push_back(std::move(item));
        }
        recordsByKey[actualKey].push_back(found->second);
        if (categoryIndex == kUpdCategory) {
            if (seenUpd.insert(actualKey).second) updKeys.push_back(actualKey);
        } else if (isEop) {
            if (seenEop.insert(actualKey).second) eopKeys.push_back(actualKey);
        } else if (seenStandard.insert(actualKey).second) {
            standardKeys.push_back(actualKey);
        }
    }

    constexpr std::size_t kRouteBatchSize = 512;
    const auto fetchBatches = [&](const std::vector<std::string>& keys,
                                  const char* queryPrefix,
                                  const char* querySuffix) -> bool {
        for (std::size_t offset = 0; offset < keys.size();
             offset += kRouteBatchSize) {
            if (stopToken.stop_requested()) {
                result.error = L"精密产品下载路线读取已取消";
                return false;
            }
            const auto count = std::min(kRouteBatchSize, keys.size() - offset);
            std::string sql(queryPrefix);
            sql += " IN (";
            for (std::size_t index = 0; index < count; ++index) {
                if (index != 0) sql += ',';
                sql += '?';
                sql += std::to_string(index + 1);
            }
            sql += ')';
            sql += querySuffix;
            auto routes = Prepare(source.get(), sql.c_str(), result.error);
            if (!routes) return false;
            for (std::size_t index = 0; index < count; ++index) {
                if (!BindText(routes.get(), static_cast<int>(index + 1),
                              keys[offset + index])) {
                    result.error = L"绑定精密产品下载路线失败";
                    return false;
                }
            }
            while (true) {
                const int status = sqlite3_step(routes.get());
                if (status == SQLITE_DONE) break;
                if (status != SQLITE_ROW) {
                    result.error = DbError(source.get(), L"读取精密产品下载路线失败");
                    return false;
                }
                // Every batch query returns the logical key in column zero,
                // followed by the same source/priority/name/URL tuple.
                const std::string key = ColumnText(routes.get(), 0);
                const auto found = recordsByKey.find(key);
                if (found == recordsByKey.end()) continue;
                const PreciseProductMirror mirror{
                    Utf8ToWide(ColumnText(routes.get(), 1)),
                    Utf8ToWide(ColumnText(routes.get(), 4)),
                    Utf8ToWide(ColumnText(routes.get(), 3))};
                if (mirror.url.empty()) continue;
                for (const auto recordIndex : found->second) {
                    auto& item = result.records[recordIndex];
                    const bool duplicate = std::ranges::any_of(item.mirrors,
                        [&](const auto& existing) {
                            return existing.url == mirror.url;
                        });
                    if (!duplicate) item.mirrors.push_back(mirror);
                }
            }
            completed += count;
            if (progress) progress(std::min(completed, dayKeys.size()),
                                   dayKeys.size());
        }
        return true;
    };

    // Keep the selected columns identical across the three source tables so
    // the row decoder above remains small and, importantly, all filtering is
    // done in one vectorised DuckDB query per batch.
    if (!fetchBatches(standardKeys,
            "SELECT s.logical_key,d.source_key,d.source_priority,s.file_name,s.download_url "
            "FROM product_sources s JOIN source_definitions d ON d.source_id=s.source_id "
            "WHERE s.logical_key",
            " ORDER BY s.logical_key,d.source_priority,s.file_name;")) return result;
    if (!fetchBatches(eopKeys,
            "SELECT logical_key,source_key,source_priority,file_name,download_url "
            "FROM eop_series_sources WHERE logical_key",
            " ORDER BY logical_key,source_priority,file_name;")) return result;
    if (!fetchBatches(updKeys,
            "SELECT logical_key,provider,0,file_name,download_url "
            "FROM upd_files WHERE logical_key",
            " ORDER BY logical_key,file_name;")) return result;

    // Drop dates for which no usable route was found.  This preserves the
    // previous API contract while avoiding a second pass over all route rows.
    result.records.erase(std::remove_if(result.records.begin(), result.records.end(),
        [](const auto& item) { return item.mirrors.empty(); }), result.records.end());
    return result;
}
