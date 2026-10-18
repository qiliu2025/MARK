#pragma once

#include "Sp3Availability.h"

// IGS satellite-attitude products use the ATT content type in the ORBEX
// (*.OBX) format.  They share the weekly archive, route and SQLite layout used
// by SP3/CLK, but are kept in an independent database.
using AttArchiveFile = Sp3ArchiveFile;
using AttSourceScan = Sp3SourceScan;
using AttProductRoute = Sp3ProductRoute;
using AttProductRecord = Sp3ProductRecord;
using AttAvailabilityLookup = Sp3AvailabilityLookup;
// ATT/ORBEX uses the same weekly products/routes schema as SP3 and CLK;
// provide ATT-named aliases for the bulk and lazy family range results.
using AttAvailabilityRangeLookup = Sp3AvailabilityRangeLookup;
using AttProductFamilyRecord = Sp3ProductFamilyRecord;
using AttProductFamilyRangeLookup = Sp3ProductFamilyRangeLookup;
using AttProductFamilyRoutesLookup = Sp3ProductFamilyRoutesLookup;
using AttAvailabilityStats = Sp3AvailabilityStats;

std::vector<AttArchiveFile> ParseAttDirectoryListing(
    const std::string& listing,
    int expectedGpsWeek,
    const std::wstring& directoryUrl,
    const std::string& archiveClass);

std::vector<AttSourceScan> QueryAttWeek(
    int gpsWeek,
    std::stop_token stopToken);

bool StoreAttWeek(
    int gpsWeek,
    const std::vector<AttSourceScan>& sources,
    std::wstring& error);

AttAvailabilityLookup LoadAttAvailability(const QueryDate& date);
AttAvailabilityRangeLookup LoadAttAvailabilityRange(
    const QueryDate& startDate, const QueryDate& endDate);
AttProductFamilyRangeLookup LoadAttProductFamilyRange(
    const QueryDate& startDate, const QueryDate& endDate);
AttProductFamilyRoutesLookup LoadAttProductFamilyRoutes(
    const std::string& familyKey,
    const QueryDate& startDate,
    const QueryDate& endDate,
    std::stop_token stopToken,
    Sp3ProductRouteProgress progress);
AttAvailabilityStats GetAttAvailabilityStats();
bool IsAttWeekComplete(int gpsWeek, bool& complete, std::wstring& error);
std::filesystem::path GetAttAvailabilityDatabasePath();
