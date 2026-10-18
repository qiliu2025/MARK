#pragma once

#include "Models.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

struct IonexArchiveFile {
    std::string logicalKey;
    std::string fileName;
    std::wstring downloadUrl;
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
    std::string contentType;
    std::string originalContent;
    std::string format;
};

struct IonexSourceScan {
    std::string sourceKey;
    int sourcePriority = 0;
    bool success = false;
    bool networkFailure = false;
    long transportErrorCode = 0;
    std::wstring listingUrl;
    std::wstring error;
    std::vector<IonexArchiveFile> files;
};

struct IonexProductRoute {
    std::string sourceKey;
    int sourcePriority = 0;
    std::string fileName;
    std::wstring downloadUrl;
};

struct IonexProductRecord {
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
    std::string contentType;
    std::string originalContent;
    std::string format;
    std::vector<IonexProductRoute> routes;
};

struct IonexAvailabilityLookup {
    bool indexed = false;
    bool complete = false;
    bool provisional = false;
    int sourcesSuccessful = 0;
    int sourcesExpected = 0;
    std::vector<IonexProductRecord> products;
    std::filesystem::path databasePath;
    std::wstring error;
};

// Compact range summary for the precision-product pane. Product metadata and
// date coverage are cached in memory; physical download URLs are loaded only
// when the user starts a download. This keeps a multi-decade GIM query close
// to the cost of a binary search instead of opening SQLite once per day.
struct IonexProductFamilyRecord {
    std::string familyKey;
    IonexProductRecord representative;
    int availableDays = 0;
};

struct IonexProductFamilyRangeLookup {
    bool complete = false;
    int expectedDays = 0;
    int indexedDays = 0;
    std::vector<QueryDate> incompleteDates;
    std::vector<IonexProductFamilyRecord> families;
    std::filesystem::path databasePath;
    std::wstring error;
};

struct DatedIonexProductRecord {
    QueryDate date{};
    IonexProductRecord record;
};

struct IonexProductFamilyRoutesLookup {
    std::vector<DatedIonexProductRecord> records;
    std::filesystem::path databasePath;
    std::wstring error;
};

using IonexProductRouteProgress =
    std::function<void(std::size_t completed, std::size_t total)>;

struct IonexAvailabilityStats {
    std::filesystem::path databasePath;
    int completeDays = 0;
    int partialDays = 0;
    std::int64_t products = 0;
    std::int64_t sourceRoutes = 0;
    std::string firstDate;
    std::string lastDate;
    std::wstring error;
};

std::vector<IonexArchiveFile> ParseIonexDirectoryListing(
    const std::string& listing,
    const QueryDate& expectedDate,
    const std::wstring& directoryUrl);

std::vector<IonexSourceScan> QueryIonexDate(
    const QueryDate& date,
    std::stop_token stopToken);

bool StoreIonexDate(
    const QueryDate& date,
    const std::vector<IonexSourceScan>& sources,
    std::wstring& error);

IonexAvailabilityLookup LoadIonexAvailability(const QueryDate& date);
IonexProductFamilyRangeLookup LoadIonexProductFamilyRange(
    const QueryDate& startDate,
    const QueryDate& endDate);
IonexProductFamilyRoutesLookup LoadIonexProductFamilyRoutes(
    const std::string& familyKey,
    const QueryDate& startDate,
    const QueryDate& endDate,
    std::stop_token stopToken = {},
    IonexProductRouteProgress progress = {});
IonexAvailabilityStats GetIonexAvailabilityStats();
bool IsIonexDateComplete(
    const QueryDate& date,
    bool& complete,
    std::wstring& error);
std::filesystem::path GetIonexAvailabilityDatabasePath();
int IonexExpectedSourceCount();
