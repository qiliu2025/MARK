#pragma once

#include "Models.h"

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

struct DcbArchiveFile {
    std::string logicalKey;
    std::string fileName;
    std::wstring downloadUrl;
    QueryDate coverageStart{};
    QueryDate coverageEnd{};
    std::string series;
    std::string center;
    std::string variant;
    std::string latency;
    std::string duration;
    std::string interval;
    std::string constellation;
    std::string signalPair;
    std::string scope;
    std::string format;
};

struct DcbSourceScan {
    std::string sourceKey;
    int sourcePriority = 0;
    bool success = false;
    bool networkFailure = false;
    long transportErrorCode = 0;
    std::vector<std::wstring> listingUrls;
    std::wstring error;
    std::vector<DcbArchiveFile> files;
};

struct DcbProductRoute {
    std::string sourceKey;
    int sourcePriority = 0;
    std::string fileName;
    std::wstring downloadUrl;
};

struct DcbProductRecord {
    std::string logicalKey;
    QueryDate coverageStart{};
    QueryDate coverageEnd{};
    std::string series;
    std::string center;
    std::string variant;
    std::string latency;
    std::string duration;
    std::string interval;
    std::string constellation;
    std::string signalPair;
    std::string scope;
    std::string format;
    std::vector<DcbProductRoute> routes;
};

struct DcbAvailabilityLookup {
    int archiveYear = 0;
    bool indexed = false;
    bool complete = false;
    bool provisional = false;
    int sourcesSuccessful = 0;
    int sourcesExpected = 0;
    std::vector<DcbProductRecord> products;
    std::filesystem::path databasePath;
    std::wstring error;
};

struct DcbAvailabilityStats {
    std::filesystem::path databasePath;
    int completeYears = 0;
    int partialYears = 0;
    std::int64_t products = 0;
    std::int64_t sourceRoutes = 0;
    int firstYear = 0;
    int lastYear = 0;
    std::wstring error;
};

std::vector<DcbArchiveFile> ParseDcbDirectoryListing(
    const std::string& listing,
    int expectedYear,
    const std::wstring& directoryUrl,
    bool codeArchive);

std::vector<DcbSourceScan> QueryDcbYear(
    int year,
    std::stop_token stopToken);

bool StoreDcbYear(
    int year,
    const std::vector<DcbSourceScan>& sources,
    std::wstring& error);

DcbAvailabilityLookup LoadDcbAvailability(const QueryDate& date);
DcbAvailabilityStats GetDcbAvailabilityStats();
bool IsDcbYearComplete(int year, bool& complete, std::wstring& error);
std::filesystem::path GetDcbAvailabilityDatabasePath();
int DcbExpectedSourceCount();

