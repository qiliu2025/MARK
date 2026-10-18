#pragma once

#include "PreciseProducts.h"

#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

// The interactive precision pane uses this compact side index for every
// date-oriented catalogue whose authoritative database is too large to scan
// or route-hydrate during an interactive query (SP3, CLK, ERP/EOP, DCB, ATT,
// IONEX, TRO, SINEX, broadcast navigation, OSB and UPD).  The index
// contains display metadata and a bitset of available dates; physical routes
// remain in the authoritative product database and are hydrated only when a
// download is started.
struct PreciseSearchFamilyRecord {
    std::string familyKey;
    PreciseProductFile representative;
    int availableDays = 0;
    // Bundle-planner metadata.  The normal product table only needs the
    // display fields above, but quick plans must be able to match an orbit,
    // clock and bias family without opening the route-heavy source database.
    std::string series;
    std::string productLine;
    std::string referenceFrequency;
    std::string biasConvention;
    std::string referenceFrame;
    std::string apcModel;
    int revisionWeek = -1;
    bool currentAlias = false;
    // Filled only when the caller explicitly requests dated coverage.  This
    // keeps ordinary catalogue queries compact while allowing the quick-plan
    // path to create exact contiguous coverage runs from the same bitmap.
    std::vector<QueryDate> availableDates;
};

struct PreciseSearchRangeLookup {
    bool complete = false;
    int expectedDays = 0;
    int indexedDays = 0;
    std::vector<QueryDate> incompleteDates;
    // Dates before the retained archive scan scope are not failed scans.
    // This boundary does not assert that no historical file exists.
    std::vector<QueryDate> beforeHistoryDates;
    QueryDate historyStart{};
    bool hasHistoryStart = false;
    std::vector<PreciseSearchFamilyRecord> families;
    std::filesystem::path databasePath;
    std::wstring error;
};

struct PreciseSearchDatedRoutes {
    QueryDate date{};
    std::string logicalKey;
    std::vector<PreciseProductMirror> mirrors;
};

struct PreciseSearchRoutesLookup {
    std::vector<PreciseSearchDatedRoutes> records;
    std::filesystem::path databasePath;
    std::wstring error;
};

struct PreciseSearchCatalogLookup {
    std::vector<PreciseProductFile> products;
    std::wstring error;
};

// Complete local family metadata, independent of the selected date range or
// filters. Reuses the immutable summary cache and never hydrates file routes.
PreciseSearchCatalogLookup LoadPreciseSearchCatalog();

using PreciseSearchRouteProgress =
    std::function<void(std::size_t completed, std::size_t total)>;

// categoryIndex is the 1-based UI category index.  Categories 1-8 and 10-12
// are supported; ANTEX (9) already uses a small static range reader.
PreciseSearchRangeLookup LoadPreciseSearchFamilyRange(
    int categoryIndex,
    const QueryDate& startDate,
    const QueryDate& endDate,
    bool includeAvailableDates = false,
    const std::string& onlyFamilyKey = {});

PreciseSearchRoutesLookup LoadPreciseSearchFamilyRoutes(
    int categoryIndex,
    const std::string& familyKey,
    const QueryDate& startDate,
    const QueryDate& endDate,
    std::stop_token stopToken = {},
    PreciseSearchRouteProgress progress = {});

std::filesystem::path GetPreciseSearchAvailabilityDatabasePath();

// Fixed publications only expose products recorded in their dated catalogue.
bool PreciseSearchHasFixedCutoff();
