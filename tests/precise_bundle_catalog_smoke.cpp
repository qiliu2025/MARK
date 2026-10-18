#include "AntexAvailability.h"
#include "DateUtil.h"
#include "ErpAvailability.h"
#include "OsbAvailability.h"
#include "PreciseBundlePlanner.h"
#include "Sp3Availability.h"
#include "UpdAvailability.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <string>

namespace {

std::string Upper(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char value) {
        return static_cast<char>(std::toupper(value));
    });
    return value;
}

void AppendRoutes(std::vector<DownloadRoute>& destination,
                  const std::vector<Sp3ProductRoute>& routes) {
    for (const auto& route : routes) {
        destination.push_back({route.sourceKey, route.fileName,
            route.downloadUrl});
    }
}

void AppendFamilyProducts(PreciseBundleCatalog& catalog,
                          const Sp3ProductFamilyRangeLookup& families,
                          const QueryDate& start,
                          const QueryDate& end,
                          PreciseBundleProductType type) {
    for (const auto& family : families.families) {
        const auto& representative = family.representative;
        // Keep the real-database smoke bounded to the IGS Final/OPS and
        // WUM/MGX families used by the two first-release quick presets.
        const bool standard = Upper(representative.center) == "IGS"
            && Upper(representative.project) == "OPS"
            && Upper(representative.latency) == "FINAL";
        const bool wum = Upper(representative.center) == "WUM"
            && Upper(representative.project) == "MGX";
        if (!standard && !wum) {
            continue;
        }
        const auto hydrated = type == PreciseBundleProductType::Sp3
            ? LoadSp3ProductFamilyRoutes(family.familyKey, start, end)
            : type == PreciseBundleProductType::Clk
            ? LoadClkProductFamilyRoutes(family.familyKey, start, end)
            : LoadAttProductFamilyRoutes(family.familyKey, start, end);
        for (const auto& dated : hydrated.records) {
            PreciseBundleProduct product;
            product.type = type;
            product.logicalKey = dated.record.logicalKey;
            product.nominalDate = dated.record.nominalDate;
            product.coverageStart = dated.record.nominalDate;
            product.coverageEnd = dated.record.nominalDate;
            product.fileName = dated.record.routes.empty()
                ? std::string{} : dated.record.routes.front().fileName;
            product.center = dated.record.center;
            product.series = dated.record.series;
            product.productLine = dated.record.project;
            product.latency = dated.record.latency;
            product.constellation = dated.record.constellation;
            product.interval = dated.record.interval;
            AppendRoutes(product.routes, dated.record.routes);
            if (!product.fileName.empty() && !product.routes.empty()) {
                catalog.products.push_back(std::move(product));
            }
        }
    }
}

void AppendOsbProducts(PreciseBundleCatalog& catalog,
                       const OsbAvailabilityRangeLookup& range) {
    for (const auto& record : range.products) {
        if (Upper(record.center) != "WUM"
            || Upper(record.project) != "MGX") {
            continue;
        }
        PreciseBundleProduct product;
        product.type = PreciseBundleProductType::Osb;
        product.logicalKey = record.logicalKey;
        product.nominalDate = record.coverageStart;
        product.coverageStart = record.coverageStart;
        product.coverageEnd = record.coverageEnd;
        product.center = record.center;
        product.series = record.series;
        product.productLine = record.project;
        product.latency = record.latency;
        product.constellation = record.constellation;
        product.interval = record.interval;
        product.biasConvention = record.biasClass;
        if (!record.format.empty()) {
            if (!product.biasConvention.empty()) product.biasConvention += ":";
            product.biasConvention += record.format;
        }
        if (Upper(record.biasClass).find("PHASE") != std::string::npos) {
            product.referenceFrequency = "ALL_FREQUENCY";
        }
        for (const auto& route : record.routes) {
            product.routes.push_back({route.sourceKey, route.fileName,
                route.downloadUrl});
        }
        product.fileName = product.routes.empty()
            ? std::string{} : product.routes.front().fileName;
        if (!product.fileName.empty() && !product.routes.empty()) {
            catalog.products.push_back(std::move(product));
        }
    }
}

void AppendErpProducts(PreciseBundleCatalog& catalog,
                       const ErpAvailabilityRangeLookup& range) {
    for (const auto& record : range.products) {
        PreciseBundleProduct product;
        product.type = PreciseBundleProductType::Erp;
        product.logicalKey = record.logicalKey;
        product.nominalDate = record.nominalDate;
        product.coverageStart = record.coverageStart;
        product.coverageEnd = record.coverageEnd;
        product.center = record.center;
        product.series = record.series;
        product.productLine = record.project;
        product.latency = record.latency;
        product.interval = record.interval;
        for (const auto& route : record.routes) {
            product.routes.push_back({route.sourceKey, route.fileName,
                route.downloadUrl});
        }
        product.fileName = product.routes.empty()
            ? std::string{} : product.routes.front().fileName;
        if (!product.fileName.empty() && !product.routes.empty()) {
            catalog.products.push_back(std::move(product));
        }
    }
}

void AppendAntexProducts(PreciseBundleCatalog& catalog,
                         const AntexAvailabilityRangeLookup& range) {
    for (const auto& record : range.products) {
        if (Upper(record.producer) != "IGS"
            || Upper(record.productFamily) != "IGS_GLOBAL"
            || Upper(record.scope) != "SATELLITE_RECEIVER"
            || Upper(record.variant) != "STANDARD") {
            continue;
        }
        PreciseBundleProduct product;
        product.type = PreciseBundleProductType::Antex;
        product.logicalKey = record.logicalKey;
        product.nominalDate = record.validFrom;
        product.coverageStart = record.validFrom;
        product.coverageEnd = record.validTo;
        product.fileName = record.routes.empty()
            ? std::string{} : record.routes.front().fileName;
        product.referenceFrame = record.referenceFrame;
        product.apcModel = record.calibrationType;
        product.revisionWeek = record.revisionWeek;
        product.currentAlias = record.currentAlias;
        for (const auto& route : record.routes) {
            product.routes.push_back({route.sourceKey, route.fileName,
                route.downloadUrl});
        }
        if (!product.fileName.empty() && !product.routes.empty()) {
            catalog.products.push_back(std::move(product));
        }
    }
}

QueryDate ParseDateArgument(const char* value, const QueryDate& fallback) {
    if (value == nullptr) return fallback;
    int year = 0;
    int month = 0;
    int day = 0;
    if (sscanf_s(value, "%d-%d-%d", &year, &month, &day) != 3) {
        return fallback;
    }
    const auto result = MakeQueryDate(year, month, day);
    return result.dayOfYear > 0 ? result : fallback;
}

void PrintRange(const char* label, const auto& range) {
    std::cout << label << ": indexed=" << range.indexed
              << " complete=" << range.complete
              << " expectedDays=" << range.expectedDays
              << " indexedDays=" << range.indexedDays
              << " products=" << range.products.size()
              << " gaps=" << range.incompleteDates.size();
    if (!range.error.empty()) std::wcout << L" error=" << range.error;
    std::cout << '\n';
}

} // namespace

int main(int argc, char** argv) {
    const auto defaultStart = MakeQueryDate(2025, 1, 1);
    const auto defaultEnd = MakeQueryDate(2025, 1, 2);
    const auto start = ParseDateArgument(argc > 1 ? argv[1] : nullptr,
        defaultStart);
    const auto end = ParseDateArgument(argc > 2 ? argv[2] : nullptr,
        defaultEnd);
    if (start.dayOfYear <= 0 || end.dayOfYear <= 0
        || (start.year > end.year
            || (start.year == end.year && start.month > end.month)
            || (start.year == end.year && start.month == end.month
                && start.day > end.day))) {
        std::cerr << "usage: PreciseBundleCatalogSmoke [YYYY-MM-DD YYYY-MM-DD]\n";
        return 2;
    }
    const auto sp3 = LoadSp3ProductFamilyRange(start, end);
    std::cout << "SP3 families=" << sp3.families.size()
              << " indexed=" << sp3.indexedDays
              << "/" << sp3.expectedDays << '\n';
    const auto clk = LoadClkProductFamilyRange(start, end);
    std::cout << "CLK families=" << clk.families.size()
              << " indexed=" << clk.indexedDays
              << "/" << clk.expectedDays << '\n';
    const auto att = LoadAttProductFamilyRange(start, end);
    std::cout << "ATT families=" << att.families.size()
              << " indexed=" << att.indexedDays
              << "/" << att.expectedDays << '\n';
    PrintRange("ERP", LoadErpAvailabilityRange(start, end));
    PrintRange("OSB", LoadOsbAvailabilityRange(start, end));
    PrintRange("ANTEX", LoadAntexAvailabilityRange(start, end));
    const auto upd = LoadUpdAvailabilityRange(start, end);
    PrintRange("UPD", upd);

    const auto erp = LoadErpAvailabilityRange(start, end);
    const auto antex = LoadAntexAvailabilityRange(start, end);
    PreciseBundleCatalog catalog;
    AppendFamilyProducts(catalog, sp3, start, end,
        PreciseBundleProductType::Sp3);
    AppendFamilyProducts(catalog, clk, start, end,
        PreciseBundleProductType::Clk);
    AppendErpProducts(catalog, erp);
    AppendAntexProducts(catalog, antex);
    PreciseBundleRequest request;
    request.startDate = start;
    request.endDate = end;
    request.plan = PreciseBundlePlan::StandardPpp;
    request.latency = PreciseBundleLatency::Final;
    request.center = "IGS";
    request.productLine = "OPS";
    request.clockInterval = "30S";
    request.strictMatching = true;
    const auto result = PlanPreciseBundle(request, catalog);
    std::cout << "Standard PPP real catalog: valid=" << result.valid
              << " complete=" << result.complete
              << " completeDays=" << result.completeDays
              << "/" << result.expectedDays
              << " manifest=" << result.manifest.size();
    if (!result.error.empty()) std::cout << " error=" << result.error;
    if (!result.warning.empty()) std::cout << " warning=" << result.warning;
    std::cout << '\n';

    AppendFamilyProducts(catalog, att, start, end,
        PreciseBundleProductType::Att);
    AppendOsbProducts(catalog, LoadOsbAvailabilityRange(start, end));
    PreciseBundleRequest wumRequest;
    wumRequest.startDate = start;
    wumRequest.endDate = end;
    wumRequest.plan = PreciseBundlePlan::WumPridePppAr;
    wumRequest.latency = PreciseBundleLatency::Automatic;
    wumRequest.center = "WUM";
    wumRequest.productLine = "MGX";
    wumRequest.clockInterval = "30S";
    wumRequest.strictMatching = true;
    const auto wumResult = PlanPreciseBundle(wumRequest, catalog);
    std::cout << "WUM PPP-AR real catalog: valid=" << wumResult.valid
              << " complete=" << wumResult.complete
              << " completeDays=" << wumResult.completeDays
              << "/" << wumResult.expectedDays
              << " manifest=" << wumResult.manifest.size();
    if (!wumResult.error.empty()) std::cout << " error=" << wumResult.error;
    if (!wumResult.warning.empty()) std::cout << " warning=" << wumResult.warning;
    std::cout << '\n';
    return 0;
}
