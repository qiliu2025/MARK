#pragma once

#include "Models.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct NetworkAvailabilityLookup {
    bool complete = false;
    bool provisional = false;
    std::vector<StationFile> stations;
    // When a caller asks for every daily_files row, `stations` remains the
    // preferred one-file-per-physical-station view and this vector contains
    // the raw file-level rows.  Keeping the two views separate prevents a
    // range query from counting the same file twice.
    std::vector<StationFile> allFiles;
    std::filesystem::path databasePath;
    std::wstring error;
};

// Strict, local-only view of one single-source network over an inclusive
// calendar range.  `complete` is true only when every requested UTC date has
// a non-provisional `complete` row in daily_scans.  Files are returned with
// their observation date so callers can aggregate without reopening SQLite
// once per day.
struct NetworkAvailabilityRangeLookup {
    bool complete = false;
    int expectedDays = 0;
    std::vector<DatedStationFile> files;
    std::filesystem::path databasePath;
    std::wstring error;
};

// Result metadata for a streaming range read.  The file rows themselves are
// delivered one at a time to the visitor and are deliberately not retained by
// the database layer.  A visitor returning false requests an early stop (for
// example when the UI cancellation token is set); `complete` still describes
// the validated source range, while `stopped` records that no further rows
// were consumed.
struct NetworkAvailabilityRangeStream {
    bool complete = false;
    bool stopped = false;
    int expectedDays = 0;
    // Dates that were missing, failed, or provisional in the compact scan
    // table.  This is populated only as diagnostic metadata; strict callers
    // still receive an error, while download hydration can skip them.
    std::vector<QueryDate> incompleteDates;
    std::filesystem::path databasePath;
    std::wstring error;
};

using NetworkAvailabilityRangeVisitor =
    std::function<bool(DatedStationFile&&)>;
using NetworkAvailabilityRangeProgress =
    std::function<void(std::size_t completedUnits, std::size_t totalUnits)>;

struct NetworkAvailabilityStats {
    std::filesystem::path databasePath;
    int completeDays = 0;
    int provisionalDays = 0;
    int failedDays = 0;
    std::int64_t stations = 0;
    std::int64_t files = 0;
    std::string earliestDate;
    std::string latestDate;
    std::wstring error;
};

bool HasSingleSourceKnowledgeBase(ObservationNetwork network);
std::string KnowledgeBaseNetworkKey(ObservationNetwork network);
std::wstring KnowledgeBaseNetworkLabel(ObservationNetwork network);

std::filesystem::path GetNetworkAvailabilityDatabasePath(
    ObservationNetwork network);

// Reserved for the separately managed RGP 1 Hz archive. The current
// The RGP availability index contains daily data_30 records.
std::filesystem::path GetRgpHighRateAvailabilityDatabasePath();

NetworkAvailabilityLookup LoadNetworkAvailability(
    const QueryDate& date,
    ObservationNetwork network,
    // The normal interactive query returns one preferred file per physical
    // station.  Date-range queries may request every daily_files row so that
    // a non-preferred RINEX version/interval can still satisfy the filter.
    bool includeAllFiles = false);

NetworkAvailabilityRangeLookup LoadNetworkAvailabilityRange(
    const QueryDate& startDate,
    const QueryDate& endDate,
    ObservationNetwork network,
    // True returns every daily_files row; false returns only the preferred
    // one-file-per-physical-station view.
    bool includeAllFiles = true,
    // Optional SQL predicates.  A zero version means any RINEX family;
    // an empty interval means any sampling interval.  The post-read range
    // matcher remains authoritative, so these are only a read-time shortcut.
    int rinexVersion = 0,
    std::string_view sampleInterval = {});

// Read a single-source network over an inclusive date range using one
// read-only SQLite connection.  Unlike LoadNetworkAvailabilityRange(), this
// API never materialises the file rows in a vector; each row is moved to
// `visitor` as soon as it is decoded.  The callback should return true to
// continue, false to stop early.
NetworkAvailabilityRangeStream StreamNetworkAvailabilityRange(
    const QueryDate& startDate,
    const QueryDate& endDate,
    ObservationNetwork network,
    bool includeAllFiles,
    const NetworkAvailabilityRangeVisitor& visitor,
    // Optional SQL predicates.  Existing callers may omit both arguments.
    int rinexVersion = 0,
    std::string_view sampleInterval = {},
    // When true, unavailable dates are omitted from the file stream instead
    // of aborting the whole download list.  Database/SQL errors stay fatal.
    bool allowIncompleteDates = false,
    // Optional hydration-only filter.  When non-empty, the reader performs
    // primary-key seeks for these four-character station prefixes instead of
    // scanning every file in the date range.  Normal/strict callers omit it.
    std::span<const std::string> stationFourCharIds = {},
    // Targeted reads report one completed unit per UTC-date/station block.
    // The callback may run on a background range-reader thread.
    const NetworkAvailabilityRangeProgress& progress = {});

bool StoreNetworkAvailability(
    const QueryDate& date,
    ObservationNetwork network,
    const std::vector<StationFile>& preferredStations,
    const std::vector<StationFile>& allFiles,
    std::wstring& error);

bool StoreNetworkAvailabilityFailure(
    const QueryDate& date,
    ObservationNetwork network,
    const std::wstring& message,
    std::wstring& error);

NetworkAvailabilityStats GetNetworkAvailabilityStats(
    ObservationNetwork network);
