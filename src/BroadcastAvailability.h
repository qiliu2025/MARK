#pragma once

#include "Models.h"

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

struct BroadcastArchiveFile {
    std::string logicalKey;
    std::string fileName;
    std::wstring downloadUrl;
    QueryDate nominalDate{};
    int startHour = 0;
    int startMinute = 0;
    std::string series;
    std::string producer;
    std::string rinexVersion;
    std::string navigationType;
    std::string constellation;
    std::string duration;
    std::string sourceFlag;
};

struct BroadcastSourceScan {
    std::string sourceKey;
    int sourcePriority = 0;
    bool success = false;
    bool networkFailure = false;
    long transportErrorCode = 0;
    std::wstring listingUrl;
    std::wstring error;
    std::vector<BroadcastArchiveFile> files;
};

struct BroadcastProductRoute {
    std::string sourceKey;
    int sourcePriority = 0;
    std::string fileName;
    std::wstring downloadUrl;
};

struct BroadcastProductRecord {
    std::string logicalKey;
    QueryDate nominalDate{};
    int startHour = 0;
    int startMinute = 0;
    std::string series;
    std::string producer;
    std::string rinexVersion;
    std::string navigationType;
    std::string constellation;
    std::string duration;
    std::string sourceFlag;
    std::vector<BroadcastProductRoute> routes;
};

struct BroadcastAvailabilityLookup {
    bool indexed = false;
    bool complete = false;
    bool provisional = false;
    int sourcesSuccessful = 0;
    int sourcesExpected = 0;
    std::vector<BroadcastProductRecord> products;
    std::filesystem::path databasePath;
    std::wstring error;
};

struct BroadcastAvailabilityStats {
    std::filesystem::path databasePath;
    int completeDays = 0;
    int partialDays = 0;
    std::int64_t products = 0;
    std::int64_t sourceRoutes = 0;
    std::string firstDate;
    std::string lastDate;
    std::wstring error;
};

std::vector<BroadcastArchiveFile> ParseBroadcastDirectoryListing(
    const std::string& listing,
    int expectedYear,
    const std::wstring& directoryUrl);

std::vector<BroadcastSourceScan> QueryBroadcastDate(
    const QueryDate& date,
    std::stop_token stopToken);

bool StoreBroadcastDate(
    const QueryDate& date,
    const std::vector<BroadcastSourceScan>& sources,
    std::wstring& error);

BroadcastAvailabilityLookup LoadBroadcastAvailability(const QueryDate& date);
BroadcastAvailabilityStats GetBroadcastAvailabilityStats();
bool IsBroadcastDateComplete(
    const QueryDate& date,
    bool& complete,
    std::wstring& error);
std::filesystem::path GetBroadcastAvailabilityDatabasePath();
int BroadcastExpectedSourceCount();
