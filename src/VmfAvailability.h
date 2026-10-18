#pragma once

#include "Models.h"
#include "PreciseProducts.h"

#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

struct VmfFamilyRecord {
    std::string familyKey;
    std::string model;
    std::string resolution;
    std::string grade;
    std::string latency;
    int availableDays = 0;
};

struct VmfRangeLookup {
    std::vector<VmfFamilyRecord> families;
    std::filesystem::path databasePath;
    std::wstring error;
};

struct VmfDatedRoute {
    QueryDate date{};
    std::string logicalKey;
    PreciseProductMirror mirror;
};

struct VmfRoutesLookup {
    std::vector<VmfDatedRoute> records;
    std::wstring error;
};

std::filesystem::path GetVmfAvailabilityDatabasePath();
VmfRangeLookup LoadVmfFamilyRange(const QueryDate& start, const QueryDate& end);
VmfRoutesLookup LoadVmfFamilyRoutes(const std::string& familyKey,
    const QueryDate& start, const QueryDate& end, std::stop_token stopToken = {});

