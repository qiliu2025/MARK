#pragma once

#include "Sp3Availability.h"

// CLK and SP3 use the same archive-routing and SQLite record layout.  Keeping
// aliases here avoids two subtly different implementations of the seven-source
// completeness rules while each product still lives in its own database.
using ClkArchiveFile = Sp3ArchiveFile;
using ClkSourceScan = Sp3SourceScan;
using ClkProductRoute = Sp3ProductRoute;
using ClkProductRecord = Sp3ProductRecord;
using ClkAvailabilityLookup = Sp3AvailabilityLookup;
// The CLK reader shares the physical SQLite schema with SP3.  Expose named
// aliases as well as the historical Sp3* return types so callers that include
// only this header cannot accidentally couple their code to the orbit type
// name when using range/family queries.
using ClkAvailabilityRangeLookup = Sp3AvailabilityRangeLookup;
using ClkProductFamilyRecord = Sp3ProductFamilyRecord;
using ClkProductFamilyRangeLookup = Sp3ProductFamilyRangeLookup;
using ClkProductFamilyRoutesLookup = Sp3ProductFamilyRoutesLookup;
using ClkAvailabilityStats = Sp3AvailabilityStats;

std::vector<ClkArchiveFile> ParseClkDirectoryListing(
    const std::string& listing,
    int expectedGpsWeek,
    const std::wstring& directoryUrl,
    const std::string& archiveClass);

std::vector<ClkSourceScan> QueryClkWeek(
    int gpsWeek,
    std::stop_token stopToken);

bool StoreClkWeek(
    int gpsWeek,
    const std::vector<ClkSourceScan>& sources,
    std::wstring& error);

ClkAvailabilityLookup LoadClkAvailability(const QueryDate& date);
ClkAvailabilityRangeLookup LoadClkAvailabilityRange(
    const QueryDate& startDate, const QueryDate& endDate);
ClkProductFamilyRangeLookup LoadClkProductFamilyRange(
    const QueryDate& startDate, const QueryDate& endDate);
ClkProductFamilyRoutesLookup LoadClkProductFamilyRoutes(
    const std::string& familyKey,
    const QueryDate& startDate,
    const QueryDate& endDate,
    std::stop_token stopToken,
    Sp3ProductRouteProgress progress);
ClkAvailabilityStats GetClkAvailabilityStats();
bool IsClkWeekComplete(int gpsWeek, bool& complete, std::wstring& error);
std::filesystem::path GetClkAvailabilityDatabasePath();
