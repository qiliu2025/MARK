#pragma once

#include "DateCoverage.h"

#include <memory>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct QueryDate {
    int year = 0;
    int month = 0;
    int day = 0;
    int dayOfYear = 0;
};

enum class RinexFormat {
    Rinex34,
    Rinex2
};

using RinexFormatMask = std::uint8_t;

constexpr RinexFormatMask RinexFormatBit(const RinexFormat format) {
    return format == RinexFormat::Rinex2
        ? RinexFormatMask{1U << 0}
        : RinexFormatMask{1U << 1};
}

enum class ObservationNetwork : std::uint64_t {
    Igs = 1ULL << 0,
    Mgex = 1ULL << 1,
    Ngs = 1ULL << 2,
    Epn = 1ULL << 3,
    Hk = 1ULL << 4,
    GaGdc = 1ULL << 5,
    Rgna = 1ULL << 6,
    Sonel = 1ULL << 7,
    TrigNet = 1ULL << 8,
    Cut = 1ULL << 9,
    Csn = 1ULL << 10,
    EposIceland = 1ULL << 11,
    Rbmc = 1ULL << 12,
    Ramsac = 1ULL << 13,
    RegnaRou = 1ULL << 14,
    Cacs = 1ULL << 15,
    Rgp = 1ULL << 16,
    SopacGarner = 1ULL << 17,
    MagnaEco = 1ULL << 18,
    PanamaCors = 1ULL << 19,
    HondurasCors = 1ULL << 20,
    // Pan-European EPOS-GNSS Data Gateway.  Keep this distinct from the
    // Iceland-only EPOS-IS/IMO source above.
    EposGnss = 1ULL << 21,
    Gref = 1ULL << 22,
    OsNet = 1ULL << 23,
    Ergnss = 1ULL << 24,
    Renep = 1ULL << 25,
    // GeoNet and PositioNZ daily RINEX files distributed together through
    // the New Zealand GeoNet open-data archive.
    GeoNetNz = 1ULL << 26,
    Ring = 1ULL << 27,
    Apos = 1ULL << 28,
    RobBelgium = 1ULL << 29,
    Dpga = 1ULL << 30,
    CatNet = 1ULL << 31,
    SpsLux = 1ULL << 32,
    // South Korea: files actually enumerated from the anonymous KASI
    // KASINet/KVN archive.  NGII GNSSData is kept in the public label because
    // it supplies the national station portal, but its generated ZIP workflow
    // is never treated as proof that an unlisted daily file exists.
    KasiNgii = 1ULL << 33,
    Renag = 1ULL << 34,
    Nsgi = 1ULL << 35,
    NoaGnss = 1ULL << 36,
    // REDGAE is a federation rather than one physical archive.  The online
    // adapter indexes the Spanish public members exposed by EPOS GLASS.
    Redgae = 1ULL << 37,
    // Japan's national GEONET continuous GNSS network.  This is distinct
    // from GeoNet/PositioNZ in New Zealand.
    GeoNetJapan = 1ULL << 38,
    // TU Delft DLAB GNSS field laboratory archive.
    Dlab = 1ULL << 39,
    // Nevada Geodetic Laboratory MAGNET archive.
    Magnet = 1ULL << 40,
    // Pacific Northwest Geodetic Array / Central Washington University.
    PangaCwu = 1ULL << 41
};

using ObservationNetworkMask = std::uint64_t;

constexpr ObservationNetworkMask NetworkBit(const ObservationNetwork network) {
    return static_cast<ObservationNetworkMask>(network);
}

constexpr bool HasNetwork(
    const ObservationNetworkMask mask,
    const ObservationNetwork network) {
    return (mask & NetworkBit(network)) != 0;
}

struct NetworkSelection {
    ObservationNetworkMask mask = NetworkBit(ObservationNetwork::Igs);

    [[nodiscard]] constexpr bool Any() const {
        return mask != 0;
    }

};

// Completeness policy used by the offline date-range observation query.
// `Ignore` keeps every station that is present on at least one day.  The
// other two policies apply the corresponding threshold in
// ObservationRangeOptions.  The Ratio/Days enumerator aliases are retained
// for callers written against the first prototype of the range API.
enum class CompletenessMode {
    Ignore,
    RatioAtLeast,
    DaysAtLeast,
    Ratio = RatioAtLeast,
    Days = DaysAtLeast
};

// Optional global daily coverage policy.  When enabled, only calendar days
// meeting the station-count threshold participate in the range result and
// per-station completeness calculation.
enum class DailyCoverageMode {
    Ignore,
    StationsAtLeast,
    PercentAtLeast
};

// A date-level space-weather predicate.  Kp and F10.7 use the daily maximum
// / daily value and compare with `>`; Dst uses the daily minimum (the most
// negative hourly value) and compares with `<=`, because a stronger storm has
// a more negative Dst.  Ignore is the default and never opens the weather DB.
enum class SpaceWeatherFilterMode {
    Ignore,
    KeepAbove,
    ExcludeAbove,
    KeepRange
};

struct SpaceWeatherFilter {
    SpaceWeatherFilterMode mode = SpaceWeatherFilterMode::Ignore;
    double threshold = 0.0;
    double upperThreshold = 0.0;
};

// Optional per-station date windows used by reference-frame quick plans.
// A station code may occur in more than one window when a frame is replaced;
// the permitted days are the union of all matching windows.
struct ObservationStationDateWindow {
    QueryDate start;
    QueryDate end;
    std::vector<std::string> stationCodes;
};

// Explicit opt-in windows for interactive downloads beyond the local index.
// No synthetic availability or file route is written to the knowledge base.
struct ObservationSnapshotWindow {
    ObservationNetworkMask networks = 0;
    QueryDate lastKnownDate;
};

struct ObservationRangeOptions {
    // Interactive download planning only. Dates after these snapshots are
    // provisional candidates; exact files must be resolved online.
    std::vector<ObservationSnapshotWindow> onlineSnapshots;
    CompletenessMode completenessMode = CompletenessMode::Ignore;
    // Percentage in the inclusive range 0..100.  Values outside that range
    // are clamped by the query implementation.
    double minimumCompletenessPercent = 0.0;
    int minimumAvailableDays = 0;
    DailyCoverageMode dailyCoverageMode = DailyCoverageMode::Ignore;
    int minimumDailyStations = 0;
    double minimumDailyStationPercent = 0.0;

    // 0 = any RINEX version; 2 = RINEX 2; 3 or 4 = the RINEX 3/4 family.
    int rinexVersion = 0;
    // Used when rinexVersion is 0.  Both are true by default to preserve the
    // historical query behaviour.
    bool includeRinex2 = true;
    bool includeRinex34 = true;
    // Empty means any interval.  Typical values are "30S", "15S", "01S".
    std::string sampleInterval;
    // Optional multi-value form.  When non-empty it takes precedence over
    // sampleInterval and accepts any listed interval.
    std::vector<std::string> sampleIntervals;

    // These metadata filters are also understood by the range engine when a
    // StationFile has already been enriched with Site Log fields.  The GUI
    // may apply them after its asynchronous Site Log enrichment as well.
    std::string systemsInclude;
    std::string systemsExclude;
    std::string receiverInclude;
    std::string receiverExclude;
    std::string antennaInclude;
    std::string antennaExclude;

    // Exact system-qualified RINEX observation-code filters.  Values are
    // comma/semicolon/pipe separated tokens such as ``GPS:C1C`` or
    // ``BDS:C2I``.  The query engine evaluates these against the dated
    // Site Log Plus capability intervals, not against a station-wide union.
    std::string signalCodesInclude;
    std::string signalCodesExclude;

    // Optional cross-product filter backed by runtime-data/gim_station.sqlite.
    // Provider codes are comma/semicolon/pipe separated (IGS,CODE,...).
    // `ignore` leaves the observation result unchanged; `keep` retains only
    // stations present in the selected providers' GIM station union for the
    // requested UTC date range; `exclude` removes those stations.
    std::string gimStationMode = "ignore";
    std::string gimProviders;
    // Empty selects all grades. Otherwise provider:grade pairs select products.
    std::string gimProductGrades;

    // Optional station-quality predicates backed by
    // gnss_availability.sqlite/daily_observation_quality.  The upstream IGS
    // Network service publishes one QC record per IGS station/day and does
    // not distinguish the IGS and MGEX archive views.  These switches are
    // therefore deliberately opt-in and apply only to stations which can be
    // matched to that shared IGS/MGEX quality catalogue.
    // All six enabled predicates are evaluated on individual UTC dates.
    // A missing metric or a failed predicate removes that date from qualified
    // file coverage. The station-level coverage threshold is applied afterwards.
    // Existing "Mean" names below are compatibility fields: means now refer
    // only to components/constellations within one day, never across dates.
    bool observationCompletenessEnabled = false;
    double minimumObservationCompletenessPercent = 90.0;
    std::string observationCompletenessSystem;
    bool latencyEnabled = false;
    double maximumMeanLatencyHours = 24.0;
    bool positionTimeSeriesEnabled = false;
    double maximumMeanAbsoluteDetrendedPositionMillimeters = 10.0;
    bool residualsEnabled = false;
    double maximumMeanAbsoluteResidualMillimeters = 10.0;
    bool meanMultipathEnabled = false;
    double maximumMeanMultipathCentimeters = 1.0;
    std::string multipathSystem;
    bool cycleSlipsEnabled = false;
    double maximumMeanDailyCycleSlips = 1000.0;
    std::string cycleSlipsSystem;

    // Compatibility aliases used by a few integrations.  They intentionally
    // mirror the primary fields and are only consulted when the primary one
    // is empty.
    std::string includeSystems;
    std::string excludeSystems;
    std::string includeReceiver;
    std::string excludeReceiver;
    std::string includeAntenna;
    std::string excludeAntenna;

    // Optional date-level filters.  They are evaluated after the requested
    // calendar range is selected and before station completeness is counted.
    SpaceWeatherFilter kpMaxFilter{};
    SpaceWeatherFilter dstMinFilter{};
    SpaceWeatherFilter f107Filter{};
    bool useAdjustedF107 = false; // false: observed, true: adjusted to 1 AU.

    // When enabled, observation rows are retained only on dates covered by
    // one of stationDateWindows entries matching their four-character code.
    // Empty windows are fail-closed so a missing frame roster cannot broaden
    // a reference-frame download accidentally.
    bool restrictStationDates = false;
    std::vector<ObservationStationDateWindow> stationDateWindows;
};

using ObservationQueryOptions = ObservationRangeOptions;

// A small value type for callers that prefer passing a range object.  The
// two aliases keep the public API source-compatible with earlier drafts.
struct ObservationDateRange {
    QueryDate start;
    QueryDate end;
};

using DateRange = ObservationDateRange;
using QueryDateRange = ObservationDateRange;
using ObservationRange = ObservationDateRange;

struct DownloadRoute {
    std::string source;
    std::string fileName;
    std::wstring url;
};

struct StationFile {
    std::shared_ptr<const CalendarCoverage> calendarCoverage;
    int unverifiedDays = 0;
    std::string stationId;
    std::string fourCharId;
    std::string country;
    std::string fileName;
    std::wstring downloadUrl;
    // Exact archive routes for the same logical observation file.  IGS
    // knowledge bases populate this from all seven data centres so a failed
    // primary route can fall back without another online directory query.
    std::vector<DownloadRoute> downloadRoutes;
    // The local availability index keeps the preferred archive together with
    // every archive that reported this station for the observation day.  The
    // GUI still downloads only downloadUrl; these fields preserve the routing
    // information needed for later source fail-over.
    std::string preferredSource;
    std::vector<std::string> availabilitySources;
    // RGNA publishes one RINEX 3 ZIP per hour. The application keeps one
    // station marker and asks the official service to bundle these paths.
    std::vector<std::wstring> remoteParts;
    RinexFormat format = RinexFormat::Rinex34;
    // Union of the RINEX families that are actually available for this
    // station in the queried date range.  A zero mask means that the record
    // predates multi-format aggregation; callers then fall back to `format`.
    // `format` remains the preferred concrete file family used by legacy
    // download routing, while this mask is the truthful value shown in the
    // station list and used by aggregate filters.
    RinexFormatMask availableRinexFormats = 0;
    std::string interval;
    bool hasCoordinate = false;
    double latitude = 0.0;
    double longitude = 0.0;
    // UI-only candidate flag. Geographic/country filters update this value;
    // the map, station list and regional selector all use the same scope.
    bool inSelectionScope = true;
    bool selected = false;
    ObservationNetworkMask availableNetworks = 0;

    // Site Log metadata (populated for IGS/MGEX and regional records when the
    // local Site-Log index has been built and propagated).  These fields are deliberately kept
    // separate from the daily availability columns: Site Logs describe the
    // station/equipment, while daily_files describes whether a file exists
    // on a particular UTC date.  All strings are UTF-8.
    bool hasSiteLog = false;
    std::string siteLogDomes;
    std::string siteLogName;
    std::string siteLogCity;
    std::string siteLogCountry;
    std::string siteLogReceiver;
    std::string siteLogSystems;
    std::string siteLogReceiverSerial;
    std::string siteLogFirmware;
    std::string siteLogReceiverDateInstalled;
    std::string siteLogAntenna;
    std::string siteLogRadome;
    std::string siteLogAntennaSerial;
    std::string siteLogAntennaReferencePoint;
    std::string siteLogAntennaDateInstalled;
    std::string siteLogAgency;
    std::string siteLogOperator;
    std::string siteLogRevisionDate;
    std::string siteLogSourceDirectory;
    std::string siteLogUrl;

    // Availability statistics over the queried period.  Date-range queries
    // use the inclusive calendar-day counts; GPS-week queries expose the
    // guaranteed 7/7 intersection so the same completeness column/filter is
    // meaningful in both modes.
    int availableDays = 0;
    int expectedDays = 0;
    double completenessPercent = 0.0;
};

inline RinexFormatMask EffectiveRinexFormatMask(
    const StationFile& station) {
    return station.availableRinexFormats != 0
        ? station.availableRinexFormats
        : RinexFormatBit(station.format);
}

inline bool HasRinexFormat(
    const StationFile& station,
    const RinexFormat format) {
    return (EffectiveRinexFormatMask(station) & RinexFormatBit(format)) != 0;
}

struct DatedStationFile {
    QueryDate date;
    // Index of the representative station in QueryResult::stations.
    std::size_t stationIndex = 0;
    StationFile station;
};

struct QueryResult {
    // Explicit date-filter calendar, independent of candidate availability.
    bool selectionDatesKnown = false;
    std::vector<int> selectionEligibleOrdinals;
    QueryDate date;
    NetworkSelection selection;
    std::wstring directoryUrl;
    std::vector<StationFile> stations;
    bool coversFullGpsWeek = false;
    int gpsWeek = -1;
    QueryDate periodStart{};
    QueryDate periodEnd{};
    // True only when a date-range query checked every calendar day in
    // [periodStart, periodEnd] against the local knowledge bases.
    bool coversDateRange = false;
    // In GPS-week mode this contains one preferred downloadable file for
    // every intersected station and every day of the week.
    std::vector<DatedStationFile> periodFiles;
    // True when this result intentionally contains only the station/range
    // summary.  The GUI can keep the lightweight map/list result and load
    // exact per-day files for the selected representatives immediately
    // before download.  Existing eager callers keep the default false value.
    bool periodFilesDeferred = false;
    // True when the station rows already carry the exact time-aware Site Log
    // snapshot selected by the compact observation index.  The GUI can then
    // skip the legacy per-database enrichment pass, which otherwise turns an
    // already-complete millisecond query into several seconds of background
    // SQLite work.
    bool siteLogMetadataIncluded = false;
    // Populated by the offline knowledge-base builder.  The normal station
    // list contains one preferred file per physical station; this list keeps
    // every daily RINEX candidate exposed by the selected archive.
    std::vector<StationFile> knowledgeBaseFiles;
    std::wstring catalogSource;
    std::wstring error;
    std::wstring warning;
    int filesWithoutCoordinates = 0;
    bool usedIgsLocalIndex = false;
    bool usedLocalKnowledgeBase = false;
    // Populated only by the seven-source IGS knowledge-base scan.  Keeping
    // these diagnostics structured lets the offline builder distinguish a
    // proxy/network incident from an ordinary missing historical directory.
    int igsSourcesAttempted = 0;
    int igsSourcesSuccessful = 0;
    int igsNetworkFailures = 0;
    std::vector<std::string> igsFailedSources;
    // Single-source knowledge-base builders use these fields to distinguish
    // a real proxy/transport outage from an ordinary missing historical
    // directory.  Only the former is allowed to restart/switch SSR nodes.
    bool networkTransportFailure = false;
    long transportErrorCode = 0;
    // The remote archive was queried successfully, but it published no
    // observation files for this date.  Knowledge-base builders must store
    // this as a checked zero-file day instead of treating it as a failure.
    bool emptyResultIsComplete = false;
};

enum class DownloadProgressPhase {
    PreparingManifest,
    VerifyingMetadata,
    Transfer
};

struct DownloadProgress {
    DownloadProgressPhase phase = DownloadProgressPhase::Transfer;
    std::size_t selectedIndex = 0;
    std::size_t completedFiles = 0;
    std::size_t totalFiles = 0;
    std::uint64_t downloadedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::wstring stationId;
    std::wstring message;
};

class DownloadJournal;
struct DownloadFinished {
    std::shared_ptr<DownloadJournal> journal;
    bool datedFolders = true;
    std::vector<std::wstring> completedPaths;
    std::size_t succeeded = 0;
    std::size_t skipped = 0;
    std::size_t failed = 0;
    bool cancelled = false;
    std::vector<std::wstring> errors;
    // Non-fatal knowledge-base gaps encountered while hydrating a range.
    // These are distinct from `skipped`, which means a local file existed.
    std::vector<std::wstring> warnings;
};
