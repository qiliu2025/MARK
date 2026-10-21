#pragma once

#include "Models.h"

#include <filesystem>
#include <string>
#include <vector>

// Read-only view of the iGMAS/SGG UPD index.
// UPD is deliberately kept separate from OSB: the two bias conventions are
// not interchangeable and a quick PPP-AR plan must be able to enforce that.
struct UpdProductRoute {
    std::wstring source;
    std::wstring url;
    std::wstring fileName;
};

struct UpdReferenceProduct {
    std::wstring type, key, fileName, center, series, project, solution, interval, logicalKey;
    QueryDate nominalDate{};
    std::vector<UpdProductRoute> routes;
};

struct UpdProductRecord {
    std::wstring logicalKey;
    QueryDate nominalDate{};
    std::wstring variant;
    std::wstring updClass;
    std::wstring frequencyCombination;
    std::wstring constellation;
    std::wstring interval;
    std::wstring provider;
    std::wstring format;
    std::vector<UpdProductRoute> routes;
    std::wstring fileName;
    bool dcbDateMismatch = false;
    std::vector<UpdReferenceProduct> references;
};

struct UpdAvailabilityLookup {
    bool indexed = false;
    bool complete = false;
    std::vector<UpdProductRecord> products;
    std::filesystem::path databasePath;
    std::wstring error;
};

struct UpdDeclaredInputs {
    std::wstring date;
    std::wstring fileName;
    // SP3, CLK, DCB, ANTEX, SINEX, in the order declared by the header.
    std::vector<std::wstring> files;
};

UpdDeclaredInputs LoadUpdDeclaredInputs(const std::string& productId,
    const QueryDate& startDate, const QueryDate& endDate);

struct UpdAvailabilityRangeLookup {
    bool indexed = false;
    bool complete = false;
    int expectedDays = 0;
    int indexedDays = 0;
    std::vector<QueryDate> incompleteDates;
    std::vector<UpdProductRecord> products;
    std::filesystem::path databasePath;
    std::wstring error;
};

UpdAvailabilityLookup LoadUpdAvailability(const QueryDate& date);
UpdAvailabilityRangeLookup LoadUpdAvailabilityRange(
    const QueryDate& startDate, const QueryDate& endDate);
std::filesystem::path GetUpdAvailabilityDatabasePath();

