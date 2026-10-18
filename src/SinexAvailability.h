#pragma once

#include "Models.h"

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

struct SinexArchiveFile {
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
    std::string streamCode;
    std::string center;
    std::string project;
    std::string solution;
    std::string latency;
    std::string duration;
    std::string interval;
    std::string contentType;
    std::string format;
    std::string productClass;
};

struct SinexSourceScan {
    std::string sourceKey;
    int sourcePriority = 0;
    bool success = false;
    bool networkFailure = false;
    long transportErrorCode = 0;
    std::wstring error;
    std::vector<std::wstring> listingUrls;
    std::vector<SinexArchiveFile> files;
};

struct SinexProductRoute {
    std::string sourceKey;
    int sourcePriority = 0;
    std::string fileName;
    std::wstring downloadUrl;
    std::string archiveClass;
};

struct SinexProductRecord {
    std::string logicalKey;
    QueryDate nominalDate{};
    QueryDate coverageStart{};
    QueryDate coverageEnd{};
    int startHour = 0;
    int startMinute = 0;
    std::string series;
    std::string streamCode;
    std::string center;
    std::string project;
    std::string solution;
    std::string latency;
    std::string duration;
    std::string interval;
    std::string contentType;
    std::string format;
    std::string productClass;
    std::vector<SinexProductRoute> routes;
};

struct SinexAvailabilityLookup {
    int gpsWeek = 0;
    bool indexed = false;
    bool complete = false;
    bool provisional = false;
    int sourcesSuccessful = 0;
    int sourcesExpected = 0;
    std::vector<SinexProductRecord> products;
    std::filesystem::path databasePath;
    std::wstring error;
};

struct SinexAvailabilityStats {
    std::filesystem::path databasePath;
    int completeWeeks = 0;
    int partialWeeks = 0;
    std::int64_t products = 0;
    std::int64_t sourceRoutes = 0;
    int firstGpsWeek = -1;
    int lastGpsWeek = -1;
    std::wstring error;
};

std::vector<SinexArchiveFile> ParseSinexDirectoryListing(
    const std::string& listing,
    int expectedGpsWeek,
    const std::wstring& directoryUrl,
    const std::string& archiveClass);

std::vector<SinexSourceScan> QuerySinexWeek(
    int gpsWeek,
    std::stop_token stopToken);

bool StoreSinexWeek(
    int gpsWeek,
    const std::vector<SinexSourceScan>& sources,
    std::wstring& error);

SinexAvailabilityLookup LoadSinexAvailability(const QueryDate& date);
SinexAvailabilityStats GetSinexAvailabilityStats();
bool IsSinexWeekComplete(int gpsWeek, bool& complete, std::wstring& error);
std::filesystem::path GetSinexAvailabilityDatabasePath();
int SinexExpectedSourceCount();
