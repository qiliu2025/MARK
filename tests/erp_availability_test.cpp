#include "ErpAvailability.h"
#include "DateUtil.h"
#include "Sp3Availability.h"

#include <iostream>
#include <string>

namespace {

bool SameDate(const QueryDate& date, const int year,
              const int month, const int day) {
    return date.year == year && date.month == month && date.day == day;
}

} // namespace

int main() {
    const QueryDate weekStart = MakeQueryDate(2025, 12, 28);
    const int week = CalculateGpsWeek(weekStart);
    const std::string listing = R"(
        IGS0OPSFIN_20253620000_07D_01D_ERP.ERP.gz
        IGS0OPSRAP_20253630000_01D_01D_ERP.ERP.gz
        COD0OPSULT_20253631800_02D_01D_ERP.ERP.gz
        igs23997.erp.Z
        igr23991.erp.Z
        cou23991_18.erp.Z
        IGS0OPSFIN_20253620000_07D_01D_ORB.SP3.gz
    )";
    const auto files = ParseErpDirectoryListing(
        listing, week, L"https://example.test/products/2399/", "OPS");
    if (files.size() != 6) {
        std::cerr << "expected 6 ERP files, got " << files.size() << "\n";
        return 1;
    }
    bool foundWeekly = false;
    bool foundRapid = false;
    bool foundUltra = false;
    for (const auto& file : files) {
        if (file.series == "IGS0OPSFIN" && file.duration == "07D") {
            foundWeekly = SameDate(file.coverageStart, 2025, 12, 28)
                && SameDate(file.coverageEnd, 2026, 1, 3);
        }
        if (file.series == "IGS0OPSRAP" && file.duration == "01D") {
            foundRapid = file.latency == "RAPID"
                && SameDate(file.coverageEnd, 2025, 12, 29);
        }
        if (file.series == "COD0OPSULT" && file.duration == "02D"
            && file.startHour == 18) {
            foundUltra = file.latency == "ULTRA_RAPID"
                && SameDate(file.coverageEnd, 2025, 12, 31);
        }
    }
    if (!foundWeekly || !foundRapid || !foundUltra) {
        std::cerr << "ERP classification or coverage calculation failed\n";
        return 2;
    }
    std::cout << "ERP parser tests passed\n";
    return 0;
}
