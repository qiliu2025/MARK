#pragma once

#include "Models.h"

#include <functional>
#include <span>
#include <stop_token>
#include <string>

enum class KnowledgeBaseQueryMode {
    // Interactive queries read indexed networks from their local SQLite
    // knowledge bases. Networks without an index continue to query online.
    LocalKnowledgeBaseOnly,
    // Only offline index builders may scan indexed archives online.
    BuildKnowledgeBaseOnline,
    // Resolve real files for an interactive download beyond the local index.
    OnlineDownloadOnly
};

struct ObservationManifestProgress {
    std::size_t completedUnits = 0;
    std::size_t totalUnits = 0;
    std::size_t completedSources = 0;
    std::size_t totalSources = 0;
    std::size_t discoveredFiles = 0;
    std::wstring currentSource;
};

struct IgsHighrateBuildResult {
    bool complete = false;
    int sourcesAttempted = 0;
    int sourcesSuccessful = 0;
    int networkFailures = 0;
    int sourcesOutsideCoverage = 0;
    int stations = 0;
    int files = 0;
    std::wstring sourceSummary;
    std::wstring error;
};

// Scan the IGS/MGEX 1-second archive and store a compact station/day
// manifest in the same gnss_availability.sqlite used by the normal 30-second
// index.  The manifest contains source directories, not one row per
// fifteen-minute file.
IgsHighrateBuildResult BuildIgsHighrateAvailability(
    const QueryDate& date,
    std::stop_token stopToken = {});

using ObservationManifestProgressCallback =
    std::function<void(const ObservationManifestProgress&)>;

QueryResult QueryDailyObservationFiles(
    const QueryDate& date,
    NetworkSelection selection,
    std::stop_token stopToken,
    KnowledgeBaseQueryMode mode =
        KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly,
    // Keep the historical preferred-file behavior for existing callers.
    // Range queries set this flag to read every daily_files row from
    // single-source indexes before applying interval/version filters.
    bool includeAllFiles = false);

// Query an inclusive calendar-date range using only the local observation
// knowledge bases.  Every date is checked through
// QueryDailyObservationFiles(..., LocalKnowledgeBaseOnly); no network
// fallback is permitted.  Stations are returned only when they satisfy the
// requested completeness policy, and periodFiles retains the preferred daily
// record for each qualifying station/date pair.
QueryResult QueryObservationDateRange(
    const QueryDate& startDate,
    const QueryDate& endDate,
    NetworkSelection selection,
    const ObservationRangeOptions& options,
    std::stop_token stopToken = {});

// Lightweight map/list query.  It applies the same date, network, format,
// interval and completeness rules as QueryObservationDateRange(), but does
// not materialise one DatedStationFile per station/day in periodFiles.
// `periodFilesDeferred` is true on a successful result.
QueryResult QueryObservationDateRangeSummary(
    const QueryDate& startDate,
    const QueryDate& endDate,
    NetworkSelection selection,
    const ObservationRangeOptions& options,
    std::stop_token stopToken = {});

// Resolve exact daily files only for representatives selected from a prior
// range-summary result.  Filtering happens before RangeAggregate records are
// retained, so this does not rebuild the full-range periodFiles vector.
// options.onlineSnapshots enables real online file resolution after each
// indexed network's last known date; historical holes remain local-only.
QueryResult LoadObservationDateRangeFilesForStations(
    const QueryDate& startDate,
    const QueryDate& endDate,
    NetworkSelection selection,
    const ObservationRangeOptions& options,
    std::span<const StationFile> selectedStations,
    std::stop_token stopToken = {},
    ObservationManifestProgressCallback progress = {});

// Convenience overload for callers that keep a range value object.
QueryResult QueryObservationDateRange(
    const ObservationDateRange& range,
    NetworkSelection selection,
    const ObservationRangeOptions& options,
    std::stop_token stopToken = {});

QueryResult QueryGpsWeekObservationFiles(
    const QueryDate& dateWithinWeek,
    NetworkSelection selection,
    std::stop_token stopToken,
    KnowledgeBaseQueryMode mode =
        KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly,
    const ObservationRangeOptions& options = {});
