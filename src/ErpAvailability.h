#pragma once

#include "Models.h"

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

struct ErpArchiveFile {
    std::string logicalKey;
    std::string fileName;
    std::wstring downloadUrl;
    std::string archiveClass;
    QueryDate nominalDate{};
    QueryDate coverageStart{};
    QueryDate coverageEnd{};
    int startHour = 0;
    int startMinute = 0;
    std::string series;
    std::string center;
    std::string project;
    std::string solution;
    std::string latency;
    std::string duration;
    std::string interval;
    std::string productFamily = "ERP";
    std::string standard = "IGS_ERP";
};

struct ErpSourceScan {
    std::string sourceKey;
    int sourcePriority = 0;
    bool success = false;
    bool networkFailure = false;
    long transportErrorCode = 0;
    std::wstring error;
    std::vector<std::wstring> listingUrls;
    std::vector<ErpArchiveFile> files;
};

struct ErpProductRoute {
    std::string sourceKey;
    int sourcePriority = 0;
    std::string fileName;
    std::wstring downloadUrl;
    std::string archiveClass;
};

struct ErpProductRecord {
    std::string logicalKey;
    QueryDate nominalDate{};
    QueryDate coverageStart{};
    QueryDate coverageEnd{};
    int startHour = 0;
    int startMinute = 0;
    std::string series;
    std::string center;
    std::string project;
    std::string solution;
    std::string latency;
    std::string duration;
    std::string interval;
    std::string productFamily;
    std::string standard;
    std::vector<ErpProductRoute> routes;
};

struct IersEopScan {
    std::string series;
    std::string fileName;
    std::wstring downloadUrl;
    std::string latency;
    std::string interval;
    std::string standard;
    std::string notes;
    bool success = false;
    bool networkFailure = false;
    long transportErrorCode = 0;
    QueryDate coverageStart{};
    QueryDate coverageEnd{};
    std::wstring error;
};

struct ErpAvailabilityLookup {
    int gpsWeek = 0;
    bool indexed = false;
    bool complete = false;
    bool provisional = false;
    int sourcesSuccessful = 0;
    int sourcesExpected = 0;
    std::vector<ErpProductRecord> products;
    std::filesystem::path databasePath;
    std::wstring error;
};

// Read-only range view used by bundle planners.  A single SQLite connection
// serves the whole interval; products whose coverage intersects the interval
// are returned together with all known download routes.
struct ErpAvailabilityRangeLookup {
    bool indexed = false;
    bool complete = false;
    int expectedDays = 0;
    int indexedDays = 0;
    std::vector<QueryDate> incompleteDates;
    std::vector<ErpProductRecord> products;
    std::filesystem::path databasePath;
    std::wstring error;
};

struct ErpAvailabilityStats {
    std::filesystem::path databasePath;
    int completeWeeks = 0;
    int partialWeeks = 0;
    std::int64_t products = 0;
    std::int64_t sourceRoutes = 0;
    int iersSeries = 0;
    int firstGpsWeek = -1;
    int lastGpsWeek = -1;
    std::wstring error;
};

std::vector<ErpArchiveFile> ParseErpDirectoryListing(
    const std::string& listing,
    int expectedGpsWeek,
    const std::wstring& directoryUrl,
    const std::string& archiveClass);

std::vector<ErpSourceScan> QueryErpWeek(
    int gpsWeek,
    std::stop_token stopToken);

bool StoreErpWeek(
    int gpsWeek,
    const std::vector<ErpSourceScan>& sources,
    std::wstring& error);

std::vector<IersEopScan> QueryIersEopSeries(std::stop_token stopToken);
bool StoreIersEopSeries(
    const std::vector<IersEopScan>& scans,
    std::wstring& error);
bool IersEopSeriesNeedRefresh(bool& needRefresh, std::wstring& error);

ErpAvailabilityLookup LoadErpAvailability(const QueryDate& date);
ErpAvailabilityRangeLookup LoadErpAvailabilityRange(
    const QueryDate& startDate, const QueryDate& endDate);
ErpAvailabilityStats GetErpAvailabilityStats();
bool IsErpWeekComplete(int gpsWeek, bool& complete, std::wstring& error);
std::filesystem::path GetErpAvailabilityDatabasePath();
int ErpExpectedSourceCount();
int IersExpectedSeriesCount();
