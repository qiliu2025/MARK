#pragma once

#include "Models.h"

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

struct OsbArchiveFile {
    std::string logicalKey;
    std::string fileName;
    std::wstring downloadUrl;
    std::string archiveClass;
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
    std::string constellation;
    std::string biasClass;
    std::string format;
};

struct OsbSourceScan {
    std::string sourceKey;
    int sourcePriority = 0;
    bool success = false;
    bool networkFailure = false;
    long transportErrorCode = 0;
    std::vector<std::wstring> listingUrls;
    std::wstring error;
    std::vector<OsbArchiveFile> files;
};

struct OsbProductRoute {
    std::string sourceKey;
    int sourcePriority = 0;
    std::string fileName;
    std::wstring downloadUrl;
    std::string archiveClass;
};

struct OsbProductRecord {
    std::string logicalKey;
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
    std::string constellation;
    std::string biasClass;
    std::string format;
    std::vector<OsbProductRoute> routes;
};

struct OsbAvailabilityLookup {
    int gpsWeek = 0;
    bool indexed = false;
    bool complete = false;
    bool provisional = false;
    int sourcesSuccessful = 0;
    int sourcesExpected = 0;
    std::vector<OsbProductRecord> products;
    std::filesystem::path databasePath;
    std::wstring error;
};

struct OsbAvailabilityRangeLookup {
    bool indexed = false;
    bool complete = false;
    int expectedDays = 0;
    int indexedDays = 0;
    std::vector<QueryDate> incompleteDates;
    std::vector<OsbProductRecord> products;
    std::filesystem::path databasePath;
    std::wstring error;
};

struct OsbAvailabilityStats {
    std::filesystem::path databasePath;
    int completeWeeks = 0;
    int partialWeeks = 0;
    std::int64_t products = 0;
    std::int64_t sourceRoutes = 0;
    int firstGpsWeek = -1;
    int lastGpsWeek = -1;
    std::wstring error;
};

int CalculateOsbGpsWeek(const QueryDate& date);
QueryDate OsbGpsWeekStartDate(int gpsWeek);

std::vector<OsbArchiveFile> ParseOsbDirectoryListing(
    const std::string& listing,
    int expectedGpsWeek,
    const std::wstring& directoryUrl,
    const std::string& archiveClass);

std::vector<OsbSourceScan> QueryOsbWeek(
    int gpsWeek,
    std::stop_token stopToken);

bool StoreOsbWeek(
    int gpsWeek,
    const std::vector<OsbSourceScan>& sources,
    std::wstring& error);

OsbAvailabilityLookup LoadOsbAvailability(const QueryDate& date);
OsbAvailabilityRangeLookup LoadOsbAvailabilityRange(
    const QueryDate& startDate, const QueryDate& endDate);
OsbAvailabilityStats GetOsbAvailabilityStats();
bool IsOsbWeekComplete(int gpsWeek, bool& complete, std::wstring& error);
std::filesystem::path GetOsbAvailabilityDatabasePath();
int OsbExpectedSourceCount();
