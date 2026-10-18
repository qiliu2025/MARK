#include "DateUtil.h"
#include "VmfAvailability.h"

#include <windows.h>

#include <iostream>
#include <string>

int wmain(int count, wchar_t** arguments) {
    if (count != 2 || !SetEnvironmentVariableW(
            L"GNSS_DOWNLOADER_VMF_DB_PATH", arguments[1])) {
        std::wcerr << L"Usage: VmfAvailabilityTest <vmf_availability.duckdb>\n";
        return 2;
    }
    const auto first = MakeQueryDate(2025, 1, 24);
    const auto last = MakeQueryDate(2025, 1, 31);
    const auto range = LoadVmfFamilyRange(first, last);
    if (!range.error.empty() || range.families.size() != 24) {
        std::wcerr << L"VMF family query failed: " << range.error
                   << L"; families=" << range.families.size() << L'\n';
        return 1;
    }
    for (const auto& family : range.families) {
        if (family.availableDays != 8) return 3;
        const auto routes = LoadVmfFamilyRoutes(family.familyKey, first, last);
        if (!routes.error.empty() || routes.records.size() != 8) return 4;
        for (const auto& route : routes.records) {
            if (route.mirror.url.find(L"https://vmf.geo.tuwien.ac.at/") != 0
                || route.mirror.fileName.empty()) return 5;
        }
    }
    const auto ei = LoadVmfFamilyRange(MakeQueryDate(2019, 1, 24),
        MakeQueryDate(2019, 1, 31));
    if (!ei.error.empty()) return 6;
    int eiFamilies = 0;
    for (const auto& family : ei.families) {
        if (family.grade != "EI") continue;
        ++eiFamilies;
        if (family.availableDays != 8) return 7;
    }
    if (eiFamilies != 8) return 8;
    std::wcout << L"VMF OP/FC: 24 families, 8 days each; EI: 8 families\n";
    return 0;
}
