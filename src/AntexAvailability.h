#pragma once

#include "Models.h"

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

struct AntexArchiveFile {
    std::string logicalKey;
    std::string groupKey;
    std::string fileName;
    std::wstring downloadUrl;
    std::string archiveClass;
    std::string producer;
    std::string modelName;
    std::string referenceFrame;
    std::string productFamily;
    std::string scope;
    std::string formatVersion;
    std::string calibrationType;
    int revisionWeek = -1;
    std::string variant;
    bool currentAlias = false;
    QueryDate validFrom{};
    QueryDate validTo{};
};

struct AntexSourceScan {
    std::string sourceKey;
    int sourcePriority = 0;
    bool success = false;
    bool networkFailure = false;
    long transportErrorCode = 0;
    std::vector<std::wstring> listingUrls;
    std::wstring error;
    std::vector<AntexArchiveFile> files;
};

struct AntexProductRoute {
    std::string sourceKey;
    int sourcePriority = 0;
    std::string fileName;
    std::wstring downloadUrl;
    std::string archiveClass;
};

struct AntexProductRecord {
    std::string logicalKey;
    std::string groupKey;
    std::string producer;
    std::string modelName;
    std::string referenceFrame;
    std::string productFamily;
    std::string scope;
    std::string formatVersion;
    std::string calibrationType;
    int revisionWeek = -1;
    std::string variant;
    bool currentAlias = false;
    QueryDate validFrom{};
    QueryDate validTo{};
    std::vector<AntexProductRoute> routes;
};

struct AntexAvailabilityLookup {
    bool indexed = false;
    bool complete = false;
    int sourcesSuccessful = 0;
    int sourcesExpected = 0;
    int gpsWeek = 0;
    std::vector<AntexProductRecord> products;
    std::filesystem::path databasePath;
    std::wstring error;
};

struct AntexAvailabilityRangeLookup {
    bool indexed = false;
    bool complete = false;
    int expectedDays = 0;
    int indexedDays = 0;
    std::vector<QueryDate> incompleteDates;
    std::vector<AntexProductRecord> products;
    std::filesystem::path databasePath;
    std::wstring error;
};

struct AntexAvailabilityStats {
    std::filesystem::path databasePath;
    bool indexed = false;
    bool complete = false;
    int sourcesSuccessful = 0;
    int sourcesExpected = 0;
    std::int64_t products = 0;
    std::int64_t sourceRoutes = 0;
    std::string scannedAt;
    std::wstring error;
};

std::vector<AntexArchiveFile> ParseAntexDirectoryListing(
    const std::string& listing,
    const std::wstring& downloadDirectory,
    const std::string& archiveClass,
    const std::string& sourceKey);

std::vector<AntexSourceScan> QueryAntexCatalog(
    int jobs,
    std::stop_token stopToken);

bool StoreAntexCatalog(
    const std::vector<AntexSourceScan>& sources,
    std::wstring& error);

AntexAvailabilityLookup LoadAntexAvailability(const QueryDate& date);
AntexAvailabilityRangeLookup LoadAntexAvailabilityRange(
    const QueryDate& startDate, const QueryDate& endDate);
AntexAvailabilityStats GetAntexAvailabilityStats();
std::filesystem::path GetAntexAvailabilityDatabasePath();
int AntexExpectedSourceCount();

// Full physical catalog for explicit CLK-declared revision lookup.
AntexAvailabilityRangeLookup LoadAntexAvailabilityCatalog();
