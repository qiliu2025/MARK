#pragma once

#include "Models.h"

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

struct TroArchiveFile {
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
    std::string productFamily;
    std::string station;
    std::string format;
};

struct TroSourceScan {
    std::string sourceKey;
    int sourcePriority = 0;
    bool success = false;
    bool networkFailure = false;
    long transportErrorCode = 0;
    std::vector<std::wstring> listingUrls;
    std::wstring error;
    std::vector<TroArchiveFile> files;
};

struct TroProductRoute {
    std::string sourceKey;
    int sourcePriority = 0;
    std::string fileName;
    std::wstring downloadUrl;
    std::string archiveClass;
};

struct TroProductRecord {
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
    std::string station;
    std::string format;
    std::vector<TroProductRoute> routes;
};

struct TroAvailabilityLookup {
    bool indexed = false;
    bool complete = false;
    bool provisional = false;
    int sourcesSuccessful = 0;
    int sourcesExpected = 0;
    std::vector<TroProductRecord> products;
    std::filesystem::path databasePath;
    std::wstring error;
};

struct TroAvailabilityStats {
    std::filesystem::path databasePath;
    int completeDays = 0;
    int partialDays = 0;
    std::int64_t products = 0;
    std::int64_t sourceRoutes = 0;
    std::string firstDate;
    std::string lastDate;
    std::wstring error;
};

std::vector<TroArchiveFile> ParseTroDirectoryListing(
    const std::string& listing,
    const QueryDate& expectedDate,
    const std::wstring& downloadDirectory,
    const std::string& archiveClass,
    int expectedGpsWeek = -1);

std::vector<TroSourceScan> QueryTroDate(
    const QueryDate& date,
    std::stop_token stopToken);

bool StoreTroDate(
    const QueryDate& date,
    const std::vector<TroSourceScan>& sources,
    std::wstring& error);

TroAvailabilityLookup LoadTroAvailability(const QueryDate& date);
TroAvailabilityStats GetTroAvailabilityStats();
bool IsTroDateComplete(
    const QueryDate& date,
    bool& complete,
    std::wstring& error);
std::filesystem::path GetTroAvailabilityDatabasePath();
int TroExpectedSourceCount();
