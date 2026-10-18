#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// One equipment interval extracted from the numbered history blocks of an
// IGS Site Log.  The standard form allows multiple 3.x receiver and 4.x
// antenna blocks in one document; keeping them as separate intervals avoids
// treating the last block in a document as if it had always been installed.
// Dates are normalized to ISO YYYY-MM-DD when a date can be recognized.  An
// empty date_removed means that the equipment is still installed (or that
// the source only says "Present").
struct SiteLogEquipmentEpoch {
    std::string stationId;
    std::string fourCharId;
    // Stable lowercase discriminator: "receiver" or "antenna".
    std::string kind;
    int sequence = 0;

    std::string type;
    std::string systems;
    std::string serial;
    std::string firmware;
    std::string radome;
    std::string referencePoint;
    std::string dateInstalled;
    std::string dateRemoved;

    // Provenance is repeated in the history table so an effective interval
    // can be selected without loading the (potentially large) raw document.
    std::string sourceUrl;
    std::string revisionDate;
    std::string fetchedAtUtc;
    std::uint64_t networkMask = 0;
};

// A normalized snapshot of one IGS site-log file.  The original ASCII log is
// kept in rawText so fields that are not promoted yet can still be inspected
// without downloading the file again.  Strings are UTF-8, including sourceUrl
// and rawText.
struct SiteLogRecord {
    std::string stationId;             // 9-character ID, when available
    std::string fourCharId;
    std::string domes;
    std::string siteName;
    std::string city;
    std::string country;

    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double latitude = 0.0;
    double longitude = 0.0;
    double elevation = 0.0;
    bool hasXyz = false;
    bool hasCoordinate = false;

    std::string receiverType;
    std::string receiverSystems;
    std::string receiverSerial;
    std::string receiverFirmware;
    std::string receiverDateInstalled;

    std::string antennaType;
    std::string antennaRadome;
    std::string antennaSerial;
    std::string antennaReferencePoint;
    std::string antennaDateInstalled;

    std::string agency;
    std::string operatorName;

    // ObservationNetwork bit mask.  IGS = bit 0, MGEX = bit 1; regional
    // native sources use their corresponding network bit.  A log can be
    // present in more than one archive.
    std::uint64_t networkMask = 0;
    std::string sourceDirectory;
    std::string sourceUrl;
    std::string fileName;
    std::string revisionDate;
    std::string fetchedAtUtc;
    std::string rawText;

    // All receiver/antenna history blocks parsed from rawText.  Existing
    // callers may continue using the scalar fields above; the vector is the
    // lossless, time-aware representation used by site_log_equipment.
    std::vector<SiteLogEquipmentEpoch> equipmentHistory;
};

// Convert an Earth-centered Earth-fixed (ECEF/ITRF) station position in
// metres to WGS-84 geodetic latitude/longitude/ellipsoidal height.  The
// function rejects non-finite values and vectors that are clearly not an
// Earth station.  It is used when an older Site Log contains only the
// mandatory X/Y/Z fields and no explicit latitude/longitude pair.
bool SiteLogEcefToGeodetic(
    double x,
    double y,
    double z,
    double& latitude,
    double& longitude,
    double& elevation);

struct SiteLogBuildStats {
    std::filesystem::path databasePath;
    int directoriesAttempted = 0;
    int directoriesSuccessful = 0;
    int filesDiscovered = 0;
    int filesFetched = 0;
    int filesParsed = 0;
    int filesFailed = 0;
    int recordsStored = 0;
    std::wstring warning;
    std::wstring error;
};

// A single-source observation database that should receive the subset of
// Site Logs whose station identifiers occur in its daily_files table.
struct SiteLogPropagationTarget {
    std::string networkKey;
    std::filesystem::path databasePath;
};

struct SiteLogPropagationStats {
    int targetsSeen = 0;
    int databasesUpdated = 0;
    int stationKeysMatched = 0;
    int recordsStored = 0;
    std::wstring warning;
    std::wstring error;
};

// Parse one standard IGS ASCII site log (v2 and older fixed-width variants).
SiteLogRecord ParseSiteLogText(
    std::string_view text,
    std::string_view sourceUrl = {},
    std::string_view sourceDirectory = {},
    std::uint64_t networkMask = 0);

// Parse the numbered receiver (3.x) and antenna (4.x) equipment blocks in a
// standard/legacy ASCII Site Log.  This is public for tests and for local
// migrations of already-downloaded raw_text rows.
std::vector<SiteLogEquipmentEpoch> ParseSiteLogEquipmentHistory(
    std::string_view text);

// Creates the site_logs table in the supplied IGS/MGEX availability database.
// This function is idempotent and is called by both the builder and the GUI
// lookup path, so a query can safely run before the first Site Log build.
bool EnsureSiteLogSchema(
    const std::filesystem::path& databasePath,
    std::wstring& error);

// Upsert parsed logs.  Multiple revisions and source routes are retained;
// identical URL/file pairs are replaced atomically with the newest payload.
bool StoreSiteLogRecords(
    const std::filesystem::path& databasePath,
    const std::vector<SiteLogRecord>& records,
    std::wstring& error);

// Load all records for diagnostics/export.
bool LoadSiteLogRecords(
    const std::filesystem::path& databasePath,
    std::vector<SiteLogRecord>& records,
    std::wstring& error);

// Load one preferred record per station.  A non-empty asOfDate selects the
// latest document metadata not newer than that ISO date and, independently,
// applies the receiver/antenna installation/removal intervals recorded in
// the Site Log.  In other words, revision_date is only a compatibility
// fallback for logs that contain no numbered equipment history; it is never
// used as the equipment effective date.  Pass an empty date to use the
// newest available Site Log/current equipment.  The result is keyed by both
// the normalized 9-character ID and the four-character ID, allowing the
// existing StationFile representation to be enriched without opening SQLite
// once per row.
// When preferredNetworkMask is non-zero, only rows carrying that network bit
// are returned.  This lets a regional native Site Log take precedence over
// older rows propagated from the shared IGS/MGEX table.
bool LoadSiteLogSummaryMap(
    const std::filesystem::path& databasePath,
    std::string_view asOfDate,
    std::unordered_map<std::string, SiteLogRecord>& recordsById,
    std::wstring& error,
    std::uint64_t preferredNetworkMask = 0);

// Re-select the receiver and antenna fields of a record for one observation
// date using its already-loaded equipmentHistory.  This is intentionally
// separate from LoadSiteLogSummaryMap so a caller scanning a long date range
// can load the Site Log rows once and apply the dated equipment timeline in
// memory for every daily observation record.
void ApplySiteLogEquipmentAsOf(
    SiteLogRecord& record,
    std::string_view asOfDate);

// Build a date-specific scalar snapshot from an already-loaded Site Log
// record.  The source record (and its complete equipmentHistory) is left
// untouched.  The target receives the station/provenance scalar fields and
// the receiver/antenna fields effective on asOfDate, but intentionally keeps
// rawText and equipmentHistory empty so callers can process a long date range
// without deep-copying the full history for every daily row.
void ApplySiteLogEquipmentAsOf(
    const SiteLogRecord& source,
    std::string_view asOfDate,
    SiteLogRecord& target);

// Populate site_log_equipment from raw_text already stored in site_logs.
// This operation is local-only and idempotent; it is intended for upgrading
// an existing database after the history table is introduced (the GUI does
// not call it on every interactive query).
bool BackfillSiteLogEquipment(
    const std::filesystem::path& databasePath,
    std::wstring& error,
    int* recordsProcessed = nullptr,
    int* epochsStored = nullptr);

// Fetch the four public IGS site-log directory mirrors and populate the same
// IGS/MGEX database.  No credentials are needed.  Existing rows are retained
// unless refresh is true; the stop token can cancel between requests.
SiteLogBuildStats BuildSiteLogIndex(
    const std::filesystem::path& databasePath,
    int jobs,
    bool refresh,
    std::stop_token stopToken = {},
    const std::function<void(int completed, int total)>& progress = {});

// Fetch the native Site Log source published by one regional network and
// store the records in that network's own observation database.  The source
// is deliberately selected by a short, documented key (for example HK,
// GA-GDC or SONEL); an unknown key is rejected instead of falling back to the
// shared IGS/MGEX archive.  This keeps a regional log authoritative and makes
// it possible for the GUI to distinguish native metadata from a coincidental
// IGS station match.
SiteLogBuildStats BuildNativeSiteLogIndex(
    std::string_view networkKey,
    const std::filesystem::path& databasePath,
    int jobs,
    bool refresh,
    std::stop_token stopToken = {},
    const std::function<void(int completed, int total)>& progress = {});

// Keys currently backed by an explicit official/native source definition.
// The list is intended for CLI help and diagnostics; it is not a claim that
// every endpoint is online at every moment.
std::vector<std::string> NativeSiteLogNetworkKeys();

// Copy matching Site Log records from the shared IGS/MGEX table into the
// existing single-source observation databases.  This is local-only: it
// does not fetch the remote directories and is safe to repeat after every
// Site Log refresh.
SiteLogPropagationStats PropagateSiteLogsToNetworkDatabases(
    const std::filesystem::path& sourceDatabasePath,
    const std::vector<SiteLogPropagationTarget>& targets);
