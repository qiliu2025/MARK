#pragma once

#include "Models.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Resolve a packaged database without making the DuckDB build depend on
// filesystem hard-link aliases.  The legacy application asks for *.sqlite
// paths throughout the codebase; the portable DuckDB release stores the
// actual files as *.duckdb and may be copied/extracted by a tool that drops
// hard links.  Keep the legacy path when it exists, otherwise use the sibling
// DuckDB file.  The legacy SQLite build is unchanged.
inline std::filesystem::path ResolveDatabaseFilePath(
    const std::filesystem::path& requested) {
#ifdef GNSS_DOWNLOADER_DUCKDB_BACKEND
    std::error_code error;
    if (std::filesystem::is_regular_file(requested, error)) {
        return requested;
    }
    if (requested.extension() == L".sqlite") {
        auto duckdb = requested;
        duckdb.replace_extension(L".duckdb");
        error.clear();
        if (std::filesystem::is_regular_file(duckdb, error)) {
            return duckdb;
        }
    }
#endif
    return requested;
}

struct IndexedIgsAvailability {
    StationFile station;
    int preferenceScore = 0;
    std::string sourceKey;
    int sourcePriority = 0;
};

struct IgsAvailabilitySourceScan {
    std::string sourceKey;
    int sourcePriority = 0;
    std::wstring listingUrl;
    std::wstring dataUrl;
    bool success = false;
    bool networkFailure = false;
    std::wstring error;
    // Best station-level record per source, used by the map and current UI.
    std::vector<IndexedIgsAvailability> records;
    // Every matched physical daily observation file from this source. This
    // feeds the file-level union and source-availability tables.
    std::vector<IndexedIgsAvailability> files;
};

// High-rate scans intentionally retain one station/day summary instead of
// one row for every fifteen-minute physical file.  The download hydrator
// enumerates the stored source directories later and downloads all matching
// 01S files for the selected day.
struct IgsHighrateStationScan {
    std::string stationId;
    std::string fourCharId;
    int rinexFormatMask = 0;
    int fileCount = 0;
};

struct IgsHighrateSourceScan {
    std::string sourceKey;
    int sourcePriority = 0;
    std::wstring listingUrl;
    bool success = false;
    // Transport failures may justify probing/switching the SSR node.
    // Historical directories that do not exist are successful coverage
    // checks and must not trigger proxy recovery.
    bool networkFailure = false;
    bool outsideCoverage = false;
    int fileCount = 0;
    std::wstring error;
    std::vector<IgsHighrateStationScan> stations;
};

std::optional<IgsHighrateSourceScan> LoadIgsHighrateSourceCache(
    const QueryDate& date,
    std::string_view sourceKey);

// All four anonymous archives must have successful checkpoints from the
// current scan revision. Legacy manifests remain stored, but require rescan.
bool IsIgsHighrateDateComplete(const QueryDate& date);

struct IgsAvailabilityLookup {
    bool complete = false;
    bool provisional = false;
    std::vector<IndexedIgsAvailability> records;
    std::filesystem::path databasePath;
    std::wstring error;
};

struct DatedIndexedIgsAvailability {
    QueryDate date;
    IndexedIgsAvailability record;
};

// Bulk read used by long local date-range queries.  It validates every UTC
// day once and streams the logical IGS file union through one SQLite
// connection, instead of reopening the multi-gigabyte database (and reading
// the route tables twice) for every individual day.
struct IgsAvailabilityRangeLookup {
    bool complete = false;
    int expectedDays = 0;
    std::vector<QueryDate> incompleteDates;
    std::vector<DatedIndexedIgsAvailability> records;
    std::filesystem::path databasePath;
    std::wstring error;
};

// Metadata returned by the callback-based IGS range reader.  Logical file
// rows are streamed directly from SQLite and are not accumulated in memory.
struct IgsAvailabilityRangeStream {
    bool complete = false;
    bool stopped = false;
    int expectedDays = 0;
    std::vector<QueryDate> incompleteDates;
    std::filesystem::path databasePath;
    std::wstring error;
};

using IgsAvailabilityRangeVisitor =
    std::function<bool(DatedIndexedIgsAvailability&&)>;
using IgsAvailabilityRangeProgress =
    std::function<void(std::size_t completedUnits, std::size_t totalUnits)>;

struct IgsAvailabilityStats {
    std::filesystem::path databasePath;
    int completeDays = 0;
    int provisionalDays = 0;
    std::int64_t files = 0;
    std::string earliestDate;
    std::string latestDate;
    std::wstring error;
};

struct IgsAvailabilitySourceStats {
    std::string sourceKey;
    int sourcePriority = 0;
    bool complete = false;
    int files = 0;
    int stations = 0;
    int preferredStations = 0;
    int exclusiveStations = 0;
    std::wstring listingUrl;
    std::wstring error;
};

struct IgsAvailabilityDateStats {
    std::filesystem::path databasePath;
    int uniqueStations = 0;
    int sourceRoutes = 0;
    int multiSourceStations = 0;
    int logicalObservationFiles = 0;
    int observationSourceRoutes = 0;
    int physicalFileRoutes = 0;
    int multiSourceObservationFiles = 0;
    std::vector<IgsAvailabilitySourceStats> sources;
    std::wstring error;
};

// A date is complete only when all seven configured IGS archives were
// checked successfully. Partial unions remain stored for diagnosis and a
// later retry, but are reported as provisional rather than complete.
IgsAvailabilityLookup LoadIgsAvailability(const QueryDate& date);

// Load every logical IGS observation file recorded for a UTC date.  Unlike
// LoadIgsAvailability(), which intentionally exposes one preferred file per
// physical station for the fast daily view, this function reads the v4
// daily_observation_files union and attaches all routes from
// daily_observation_file_sources.  It is used by offline range queries when
// a RINEX-version/interval predicate must be able to select a non-preferred
// file.  The completeness/provisional semantics are identical to the normal
// loader and the call never accesses the network.
IgsAvailabilityLookup LoadIgsAvailabilityAllFiles(const QueryDate& date);

IgsAvailabilityRangeLookup LoadIgsAvailabilityRange(
    const QueryDate& startDate,
    const QueryDate& endDate,
    int rinexVersion = 0,
    std::string_view sampleInterval = {});

// Stream every logical IGS observation file in an inclusive date range.  The
// function validates the seven-source completeness contract before invoking
// the visitor, then decodes one logical file (and all of its routes) at a
// time.  Returning false from the visitor stops the read without retaining
// the unconsumed rows.
IgsAvailabilityRangeStream StreamIgsAvailabilityRange(
    const QueryDate& startDate,
    const QueryDate& endDate,
    const IgsAvailabilityRangeVisitor& visitor,
    // Optional SQL predicates.  Existing callers may omit both arguments.
    int rinexVersion = 0,
    std::string_view sampleInterval = {},
    // Download hydration may keep the files already indexed on a partial
    // seven-source day and skip dates with no usable scan.  Normal range
    // queries remain strict by default.
    bool allowIncompleteDates = false,
    // Optional hydration-only filter.  Four-character IDs are converted to
    // station-key prefix ranges so SQLite can seek the existing composite
    // primary keys without requiring a large secondary-index migration.
    std::span<const std::string> stationFourCharIds = {},
    // Targeted reads report one completed unit per UTC-date/station block
    // (unavailable dates are counted as immediately completed).  The
    // callback may run on a background range-reader thread.
    const IgsAvailabilityRangeProgress& progress = {});

bool StoreIgsAvailability(
    const QueryDate& date,
    const std::vector<IgsAvailabilitySourceScan>& sources,
    std::wstring& error);

bool StoreIgsHighrateAvailability(
    const QueryDate& date,
    const std::vector<IgsHighrateSourceScan>& sources,
    std::wstring& error);

bool RemoveIgsAvailability(const QueryDate& date, std::wstring& error);

IgsAvailabilityStats GetIgsAvailabilityStats();

IgsAvailabilityDateStats GetIgsAvailabilityDateStats(const QueryDate& date);

std::filesystem::path GetIgsAvailabilityDatabasePath();
