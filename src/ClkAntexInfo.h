#pragma once
#include "AntexAvailability.h"
#include "PreciseBundlePlanner.h"
#include <map>
#include <string>
#include <vector>

struct ClkPcvsDeclaration {
    std::string system, model, appliedText, kind;
    bool exactFile = false;
};
struct ClkHeaderRecord {
    std::string logicalKey, date, fileName, status;
    std::vector<ClkPcvsDeclaration> declarations;
};
struct ClkHeaderRange {
    std::map<std::string, ClkHeaderRecord> byDate;
    std::wstring error;
};
// Accept a compact family key, ordinary catalogue ID, or dated logical key.
ClkHeaderRange LoadClkHeaderRange(const std::string& family,
    const QueryDate& first, const QueryDate& last);
std::wstring ClkHeaderDetailsJson(const ClkHeaderRange& headers,
    const std::vector<QueryDate>& availableDates);
// WUM only: replace date-effective ANTEX choices with CLK declarations.
// Historical revisions are matched by filename, without publication windows.
void ApplyClkDeclaredAntex(const PreciseBundleRequest& request,
    PreciseBundleCatalog& catalog, PreciseBundlePlanResult& plan,
    std::vector<std::wstring>& warnings);
