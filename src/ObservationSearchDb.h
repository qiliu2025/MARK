#pragma once

#include "Models.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

// A compact, rebuildable search index over the observation availability
// databases.  The authoritative daily file names, URLs and IGS mirror routes
// remain in their original databases; this index deliberately contains only
// station metadata and day bitmaps needed by an interactive range search.

struct ObservationSearchBuildProgress {
    ObservationNetwork network = ObservationNetwork::Igs;
    std::wstring networkLabel;
    std::filesystem::path sourcePath;
    std::uint64_t rowsRead = 0;
    std::uint64_t profilesBuilt = 0;
    bool finishedSource = false;
};

using ObservationSearchProgressCallback =
    std::function<void(const ObservationSearchBuildProgress&)>;

struct ObservationSearchBuildOptions {
    std::filesystem::path sourceDirectory;
    std::filesystem::path outputPath;
    // Zero means every supported knowledge base found in sourceDirectory.
    ObservationNetworkMask networkMask = 0;
    ObservationSearchProgressCallback progress;
};

struct ObservationSearchBuildResult {
    bool complete = false;
    std::filesystem::path outputPath;
    ObservationNetworkMask indexedNetworks = 0;
    std::uint64_t sourceRowsRead = 0;
    std::uint64_t unifiedStations = 0;
    std::uint64_t profiles = 0;
    std::uint64_t bitmapBlocks = 0;
    std::wstring error;
    std::vector<std::wstring> warnings;
};

struct ObservationSearchQueryResult {
    std::vector<int> selectionEligibleOrdinals;
    bool complete = false;
    bool stale = false;
    bool siteLogMetadataIncluded = false;
    int expectedDays = 0;
    ObservationNetworkMask indexedNetworks = 0;
    std::vector<StationFile> stations;
    std::wstring error;
    std::wstring warning;
};

struct ObservationSearchIndexStatus {
    bool exists = false;
    bool compatible = false;
    bool stale = false;
    ObservationNetworkMask indexedNetworks = 0;
    std::uint64_t unifiedStations = 0;
    std::uint64_t profiles = 0;
    std::uint64_t bitmapBlocks = 0;
    std::wstring error;
};

// Values exposed by the advanced-filter checklist.  Receiver and antenna
// entries retain the complete model name (including spaces); systems are
// normalized constellation tokens.  The lists are collected from every
// equipment segment which overlaps the requested date range, so a model
// installed halfway through a long query is not hidden by the representative
// station snapshot shown for the first day.
struct ObservationSearchMetadataChoices {
    std::vector<std::string> systems;
    std::vector<std::string> receivers;
    std::vector<std::string> antennas;
    // System-qualified observation codes, serialized as ``SYSTEM:CODE``
    // (for example ``GPS:C1C`` and ``BDS:C2I``).  They are collected from
    // dated Site Log Plus capability intervals overlapping the query range.
    std::vector<std::string> signals;
    std::wstring error;
};

// Actual provider:grade combinations in the GIM station knowledge base.
struct GimProductGradeChoices {
    std::vector<std::string> values;
    std::wstring error;
};
GimProductGradeChoices QueryGimProductGradeChoices(
    const QueryDate& startDate, const QueryDate& endDate);

// Canonical constellation tokens used by the metadata filter UI and query
// engine.  Site Logs contain a mixture of RINEX one-letter codes and human
// readable aliases (for example G/GPS, R/GLONASS, BEI/BeiDou); callers should
// use this helper before displaying or comparing satellite-system values.
std::vector<std::string> CanonicalSatelliteSystemTokens(
    const std::string& value);

#include "ObservationEquipmentHistory.h"

struct ObservationStationDetail {
    StationFile station;
    std::vector<std::string> signals;
    std::vector<ObservationEquipmentPeriod> periods;
    std::wstring error;
};
ObservationStationDetail LoadObservationStationDetail(
    const std::filesystem::path& indexPath, const StationFile& station,
    const QueryDate& start, const QueryDate& end);

struct ObservationSearchDatedFilterResult {
    bool complete = false;
    bool stale = false;
    std::size_t inputFiles = 0;
    std::size_t retainedFiles = 0;
    std::wstring error;
    std::wstring warning;
};

using ObservationSearchDatedFilterProgressCallback =
    std::function<void(std::size_t processedFiles, std::size_t totalFiles)>;

// All observation networks for which the current application has a local
// SQLite knowledge-base file mapping.  IGS and MGEX share one fact database.
ObservationNetworkMask ObservationSearchSupportedNetworkMask();

// Default location used by the builder and GUI when no explicit path is
// supplied: <knowledge-base-directory>/observation_search.sqlite.
std::filesystem::path DefaultObservationSearchIndexPath(
    const std::filesystem::path& knowledgeBaseDirectory = {});

// Rebuild into a temporary sibling and atomically replace outputPath only
// after all readable source databases have been committed successfully.
// The function never reads daily_observation_file_sources or any download URL
// table.  Missing source databases are skipped and reported as warnings.
ObservationSearchBuildResult BuildObservationSearchIndex(
    const ObservationSearchBuildOptions& options,
    std::stop_token stopToken = {});

// Millisecond-oriented lightweight search.  Returned StationFile objects
// contain identity, network membership, coordinates, RINEX/interval summary,
// availableDays/expectedDays/completenessPercent, but intentionally contain no
// file name, URL or mirror routes.  Those must be resolved lazily from the
// authoritative fact databases after the user chooses stations to download.
// includeDisplayMetadata adds the equipment snapshot for the first matching
// day. It does not enable equipment filters or load the signal catalogue.
ObservationSearchQueryResult QueryObservationSearchIndex(
    const std::filesystem::path& indexPath,
    const QueryDate& startDate,
    const QueryDate& endDate,
    NetworkSelection selection,
    const ObservationRangeOptions& options,
    bool validateSourceFreshness = true,
    std::stop_token stopToken = {},
    bool includeDisplayMetadata = false);

std::vector<ObservationSnapshotWindow> ObservationSearchOnlineSnapshots(
    const std::filesystem::path& indexPath,
    NetworkSelection selection,
    const QueryDate& endDate);

ObservationSearchMetadataChoices QueryObservationSearchMetadataChoices(
    const std::filesystem::path& indexPath,
    const QueryDate& startDate,
    const QueryDate& endDate,
    NetworkSelection selection,
    bool validateSourceFreshness = true);

// Applies the same time-aware Site Log precedence and equipment predicates as
// QueryObservationSearchIndex to already hydrated authoritative daily rows.
// Enabled daily quality predicates are also rechecked against actual date records.
// Non-matching rows are removed in place; retained rows receive the exact
// receiver/antenna snapshot effective on their own UTC date.  Download code
// should call this after lazy hydration so a station that qualified on only a
// subset of days cannot accidentally download equipment-mismatched days.
ObservationSearchDatedFilterResult FilterObservationSearchDatedFiles(
    const std::filesystem::path& indexPath,
    NetworkSelection selection,
    const ObservationRangeOptions& options,
    std::vector<DatedStationFile>& files,
    bool validateSourceFreshness = true,
    std::stop_token stopToken = {},
    ObservationSearchDatedFilterProgressCallback progress = {});

ObservationSearchIndexStatus GetObservationSearchIndexStatus(
    const std::filesystem::path& indexPath,
    bool validateSourceFreshness = true,
    bool includeDisplayMetadata = false);
