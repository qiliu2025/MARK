#pragma once

#include "Models.h"

#include <functional>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

struct Sp3ArchiveFile {
    std::string logicalKey;
    std::string fileName;
    std::wstring downloadUrl;
    std::string archiveClass;
    QueryDate nominalDate{};
    int startHour = 0;
    int startMinute = 0;
    std::string series;
    std::string center;
    std::string project;
    std::string solution;
    std::string latency;
    std::string duration;
    std::string interval;
    std::string constellation;
};

struct Sp3SourceScan {
    std::string sourceKey;
    int sourcePriority = 0;
    bool success = false;
    bool networkFailure = false;
    long transportErrorCode = 0;
    std::wstring error;
    std::vector<std::wstring> listingUrls;
    std::vector<Sp3ArchiveFile> files;
};

struct Sp3ProductRoute {
    std::string sourceKey;
    int sourcePriority = 0;
    std::string fileName;
    std::wstring downloadUrl;
    std::string archiveClass;
};

struct Sp3ProductRecord {
    std::string logicalKey;
    QueryDate nominalDate{};
    int startHour = 0;
    int startMinute = 0;
    std::string series;
    std::string center;
    std::string project;
    std::string solution;
    std::string latency;
    std::string duration;
    std::string interval;
    std::string constellation;
    std::vector<Sp3ProductRoute> routes;
};

struct Sp3AvailabilityLookup {
    int gpsWeek = 0;
    bool indexed = false;
    bool complete = false;
    bool provisional = false;
    int sourcesSuccessful = 0;
    std::vector<Sp3ProductRecord> products;
    std::filesystem::path databasePath;
    std::wstring error;
};

// Bulk range reader used by the precision-product pane.  One SQLite
// connection and one joined products/routes query serve the complete date
// interval; the old daily loader remains available for single-day callers.
struct DatedSp3ProductRecord {
    QueryDate date{};
    Sp3ProductRecord record;
};

struct Sp3AvailabilityRangeLookup {
    bool complete = false;
    int expectedDays = 0;
    int indexedDays = 0;
    std::vector<QueryDate> incompleteDates;
    std::vector<DatedSp3ProductRecord> records;
    std::filesystem::path databasePath;
    std::wstring error;
};

// Compact range summary used by the interactive precision-product search.
// One record represents a stable product family across the requested period;
// daily logical keys and physical download routes remain in an internal
// immutable cache and the authoritative availability database respectively.
// This mirrors the observation pane's bitmap-summary/lazy-hydration design:
// changing dates or completeness filters does not reread millions of URLs.
struct Sp3ProductFamilyRecord {
    std::string familyKey;
    Sp3ProductRecord representative;
    int availableDays = 0;
    // Unique nominal dates represented by this family in the requested
    // interval.  The compact range reader intentionally omits physical
    // routes; callers can use this list (or the count above) to decide which
    // family to hydrate with Load*ProductFamilyRoutes().
    std::vector<QueryDate> availableDates;
};

struct Sp3ProductFamilyRangeLookup {
    bool complete = false;
    bool stale = false;
    int expectedDays = 0;
    int indexedDays = 0;
    std::vector<QueryDate> incompleteDates;
    std::vector<Sp3ProductFamilyRecord> families;
    std::filesystem::path databasePath;
    std::wstring error;
    std::wstring warning;
};

struct Sp3ProductFamilyRoutesLookup {
    std::vector<DatedSp3ProductRecord> records;
    std::filesystem::path databasePath;
    std::wstring error;
};

// Reports how many product/date route entries have been expanded while a
// lazy download manifest is being prepared.  Updates are deliberately
// throttled by the loader so a multi-decade range does not flood the UI
// message queue.
using Sp3ProductRouteProgress =
    std::function<void(std::size_t completed, std::size_t total)>;

struct Sp3AvailabilityStats {
    std::filesystem::path databasePath;
    int completeWeeks = 0;
    int partialWeeks = 0;
    std::int64_t products = 0;
    std::int64_t sourceRoutes = 0;
    int firstGpsWeek = -1;
    int lastGpsWeek = -1;
    std::wstring error;
};

int CalculateGpsWeek(const QueryDate& date);
QueryDate GpsWeekStartDate(int gpsWeek);

std::vector<Sp3ArchiveFile> ParseSp3DirectoryListing(
    const std::string& listing,
    int expectedGpsWeek,
    const std::wstring& directoryUrl,
    const std::string& archiveClass);

std::vector<Sp3SourceScan> QuerySp3Week(
    int gpsWeek,
    std::stop_token stopToken);

bool StoreSp3Week(
    int gpsWeek,
    const std::vector<Sp3SourceScan>& sources,
    std::wstring& error);

Sp3AvailabilityLookup LoadSp3Availability(const QueryDate& date);
Sp3AvailabilityRangeLookup LoadSp3AvailabilityRange(
    const QueryDate& startDate, const QueryDate& endDate);
Sp3AvailabilityRangeLookup LoadClkAvailabilityRange(
    const QueryDate& startDate, const QueryDate& endDate);
Sp3AvailabilityRangeLookup LoadAttAvailabilityRange(
    const QueryDate& startDate, const QueryDate& endDate);
Sp3ProductFamilyRangeLookup LoadSp3ProductFamilyRange(
    const QueryDate& startDate, const QueryDate& endDate);
Sp3ProductFamilyRangeLookup LoadClkProductFamilyRange(
    const QueryDate& startDate, const QueryDate& endDate);
Sp3ProductFamilyRangeLookup LoadAttProductFamilyRange(
    const QueryDate& startDate, const QueryDate& endDate);
Sp3ProductFamilyRoutesLookup LoadSp3ProductFamilyRoutes(
    const std::string& familyKey,
    const QueryDate& startDate,
    const QueryDate& endDate,
    std::stop_token stopToken = {},
    Sp3ProductRouteProgress progress = {});
Sp3ProductFamilyRoutesLookup LoadClkProductFamilyRoutes(
    const std::string& familyKey,
    const QueryDate& startDate,
    const QueryDate& endDate,
    std::stop_token stopToken = {},
    Sp3ProductRouteProgress progress = {});
Sp3ProductFamilyRoutesLookup LoadAttProductFamilyRoutes(
    const std::string& familyKey,
    const QueryDate& startDate,
    const QueryDate& endDate,
    std::stop_token stopToken = {},
    Sp3ProductRouteProgress progress = {});
Sp3AvailabilityStats GetSp3AvailabilityStats();
bool IsSp3WeekComplete(int gpsWeek, bool& complete, std::wstring& error);
std::filesystem::path GetSp3AvailabilityDatabasePath();
