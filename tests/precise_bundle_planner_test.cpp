#include "DateUtil.h"
#include "PreciseBundlePlanner.h"

#include <algorithm>
#undef NDEBUG  // Release test binaries must still execute their assertions.
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Type = PreciseBundleProductType;

const QueryDate kFirst = MakeQueryDate(2025, 1, 1);
const QueryDate kSecond = MakeQueryDate(2025, 1, 2);

DownloadRoute Route(const std::string& suffix) {
    return {"test", suffix, L"https://example.test/"};
}

PreciseBundleProduct Product(
    const Type type,
    const QueryDate& date,
    const std::string& center = "WUM",
    const std::string& latency = "RAPID",
    const std::string& frequency = "GPS L1/L2",
    const std::string& tag = "") {
    PreciseBundleProduct product;
    product.type = type;
    product.logicalKey = tag + PreciseBundleProductTypeName(type) + "_"
        + std::to_string(date.year) + std::to_string(date.month)
        + std::to_string(date.day);
    product.nominalDate = date;
    product.fileName = product.logicalKey + ".gz";
    product.center = center;
    product.series = center + "0OPS";
    product.productLine = "OPS";
    product.latency = latency;
    product.constellation = "G";
    product.interval = type == Type::Sp3 ? "05M" : "30S";
    product.referenceFrequency = frequency;
    product.referenceFrame = "IGS14";
    product.apcModel = "APC";
    product.biasConvention = type == Type::Osb ? "OSB" : "";
    if (type == Type::SinexCrd || type == Type::SinexSol) {
        product.center = "IGS";
        product.series = "IGS0OPSSNX";
        product.productLine = "DAILY";
        product.latency = "FINAL";
        product.constellation = type == Type::SinexCrd
            ? "CRD / DAILY" : "SOL / DAILY";
        product.interval = "01D";
    } else if (type == Type::Gim) {
        product.center = "CODE";
        product.series = "COD0OPSFIN";
        product.latency = "FINAL";
        product.constellation = "GIM";
        product.interval = "01H";
    } else if (type >= Type::Vmf1H00 && type <= Type::Vmf3H18) {
        product.center = "TU Wien";
        product.series = type <= Type::Vmf1H18 ? "VMF1" : "VMF3";
        product.productLine = "OP";
        product.latency = "DAILY";
        product.constellation = "Earth";
        product.interval = "06H";
    } else if (type == Type::Brdc) {
        product.center = "IGS";
        product.series = "BRDC00IGS";
        product.productLine = "MN";
        product.latency = "DAILY";
        product.constellation = "GRECJS";
        product.interval = "RINEX 3";
    }
    product.routes.push_back(Route(product.fileName));
    return product;
}

PreciseBundleProduct StaticProduct(
    const Type type,
    const std::string& center = "WUM",
    const std::string& latency = "RAPID",
    const std::string& frequency = "GPS L1/L2",
    const std::string& tag = "") {
    auto product = Product(type, {}, center, latency, frequency, tag);
    product.nominalDate = {};
    product.coverageStart = {};
    product.coverageEnd = {};
    return product;
}

void AddDaily(
    PreciseBundleCatalog& catalog,
    const Type type,
    const std::string& center = "WUM",
    const std::string& latency = "RAPID",
    const std::string& frequency = "GPS L1/L2",
    const std::string& tag = "") {
    catalog.products.push_back(Product(type, kFirst, center, latency, frequency, tag));
    catalog.products.push_back(Product(type, kSecond, center, latency, frequency, tag));
}

PreciseBundleCatalog CompleteCatalog(
    const PreciseBundlePlan plan,
    const std::string& center = "WUM",
    const std::string& latency = "RAPID",
    const std::string& frequency = "GPS L1/L2",
    const std::string& tag = "") {
    PreciseBundleCatalog catalog;
    for (const auto type : RequiredBundleProductTypes(plan)) {
        if (type == Type::Erp) {
            // ERP rows may be weekly; the planner must not emit one copy per
            // covered day in the saved manifest.
            auto erp = Product(type, kFirst, center, latency, frequency, tag);
            erp.logicalKey = tag + "ERP_WEEK";
            erp.fileName = erp.logicalKey + ".gz";
            catalog.products.push_back(std::move(erp));
        } else if (type == Type::Antex) {
            auto antex = StaticProduct(type, center, latency, frequency, tag);
            if (plan == PreciseBundlePlan::WumPridePppAr) {
                antex.center.clear();
                antex.series.clear();
                antex.productLine.clear();
                antex.latency.clear();
                antex.constellation.clear();
            }
            catalog.products.push_back(std::move(antex));
        } else {
            AddDaily(catalog, type, center, latency, frequency, tag);
        }
    }
    if (plan == PreciseBundlePlan::WumPridePppAr) {
        for (auto& product : catalog.products) {
            if (product.type == Type::Antex || product.type >= Type::SinexCrd)
                continue;
            product.series = center + "0MGXRAP";
            product.productLine = "MGX";
        }
    }
    return catalog;
}

PreciseBundleRequest Request(const PreciseBundlePlan plan) {
    PreciseBundleRequest request;
    request.startDate = kFirst;
    request.endDate = kSecond;
    request.plan = plan;
    request.latency = PreciseBundleLatency::Automatic;
    return request;
}

bool Contains(
    const std::vector<PreciseBundleProductType>& types,
    const Type type) {
    return std::find(types.begin(), types.end(), type) != types.end();
}

void TestRequiredPlansAndCompleteBundles() {
    const std::vector<Type> wum{
        Type::Sp3, Type::Clk, Type::Osb, Type::Erp, Type::Att, Type::Antex,
        Type::SinexCrd, Type::SinexSol, Type::Gim,
        Type::Vmf1H00, Type::Vmf1H06, Type::Vmf1H12, Type::Vmf1H18,
        Type::Vmf3H00, Type::Vmf3H06, Type::Vmf3H12, Type::Vmf3H18,
        Type::Brdc};
    const std::vector<Type> standard{
        Type::Sp3, Type::Clk, Type::Erp, Type::Antex};
    const std::vector<Type> upd{
        Type::Sp3, Type::Clk, Type::Upd, Type::Erp, Type::Antex};
    const std::vector<Type> osb{
        Type::Sp3, Type::Clk, Type::Osb, Type::Erp, Type::Antex};
    assert(RequiredBundleProductTypes(PreciseBundlePlan::WumPridePppAr) == wum);
    assert(RequiredBundleProductTypes(PreciseBundlePlan::StandardPpp) == standard);
    assert(RequiredBundleProductTypes(PreciseBundlePlan::AutoStandardPpp) == standard);
    assert(RequiredBundleProductTypes(PreciseBundlePlan::UpdPppAr) == upd);
    assert(RequiredBundleProductTypes(PreciseBundlePlan::OsbPppAr) == osb);
    assert(RequiredBundleProductTypes(
        PreciseBundlePlan::NetworkRtkPppRtk) == osb);

    for (const auto plan : {PreciseBundlePlan::WumPridePppAr,
                            PreciseBundlePlan::StandardPpp,
                            PreciseBundlePlan::AutoStandardPpp,
                            PreciseBundlePlan::UpdPppAr,
                            PreciseBundlePlan::OsbPppAr,
                            PreciseBundlePlan::NetworkRtkPppRtk}) {
        const auto result = PlanPreciseBundle(Request(plan), CompleteCatalog(plan));
        assert(result.valid);
        assert(result.complete);
        assert(result.expectedDays == 2);
        assert(result.completeDays == 2);
        assert(result.missingDates.empty());
        assert(result.manifest.size() ==
            (plan == PreciseBundlePlan::WumPridePppAr ? 34U
             : plan == PreciseBundlePlan::StandardPpp
                || plan == PreciseBundlePlan::AutoStandardPpp ? 6U : 8U));
    }
}

void TestUnavailableDatesAndMissingMembers() {
    auto catalog = CompleteCatalog(PreciseBundlePlan::WumPridePppAr);
    catalog.unavailableDates[Type::Osb].push_back(kSecond);
    const auto result = PlanPreciseBundle(
        Request(PreciseBundlePlan::WumPridePppAr), catalog);
    assert(result.valid);
    assert(!result.complete);
    assert(result.completeDays == 1);
    assert(result.missingDates.size() == 1);
    assert(result.missingDates.front().day == kSecond.day);
    assert(Contains(result.missingProductTypes, Type::Osb));
    assert(result.dates.size() == 2);
    assert(result.dates.back().missing.size() == 1);
    assert(result.dates.back().missing.front() == Type::Osb);
    // Incomplete dates must not contribute download entries.
    assert(result.manifest.size() == 18);
}

void TestWumRapidExtrasAndNoFinalFallback() {
    auto catalog = CompleteCatalog(PreciseBundlePlan::WumPridePppAr);
    for (auto& product : catalog.products) {
        if (product.type == Type::Antex || product.type >= Type::SinexCrd)
            continue;
        product.series = "WUM0MGXRAP";
        product.productLine = "MGX";
    }
    auto request = Request(PreciseBundlePlan::WumPridePppAr);
    request.center = "WUM";
    request.productLine = "MGX";
    request.latency = PreciseBundleLatency::Rapid;
    request.clockInterval = "30S";
    const auto complete = PlanPreciseBundle(request, catalog);
    assert(complete.valid && complete.complete);
    assert(complete.manifest.size() == 34U);

    catalog.products.erase(std::remove_if(catalog.products.begin(),
        catalog.products.end(), [](const auto& product) {
            return product.type == Type::Vmf3H18
                && product.nominalDate.day == kSecond.day;
        }), catalog.products.end());
    auto finalClock = Product(Type::Clk, kSecond, "WUM", "FINAL");
    finalClock.series = "WUM0MGXFIN";
    finalClock.productLine = "MGX";
    catalog.products.push_back(std::move(finalClock));
    const auto incomplete = PlanPreciseBundle(request, catalog);
    assert(incomplete.valid && !incomplete.complete);
    assert(incomplete.completeDays == 1);
    assert(Contains(incomplete.missingProductTypes, Type::Vmf3H18));
    assert(incomplete.latency == "RAPID");
}

void TestLatencyDoesNotMixFinalAndRapid() {
    auto catalog = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                   "IGS", "FINAL", "GPS L1/L2", "F_");
    auto rapid = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                 "IGS", "RAPID", "GPS L1/L2", "R_");
    catalog.products.insert(catalog.products.end(), rapid.products.begin(), rapid.products.end());

    auto automatic = Request(PreciseBundlePlan::StandardPpp);
    automatic.latency = PreciseBundleLatency::Automatic;
    const auto automaticResult = PlanPreciseBundle(automatic, catalog);
    assert(automaticResult.valid && automaticResult.complete);
    assert(automaticResult.latency == "FINAL");
    assert(automaticResult.familyKey.find("FINAL") != std::string::npos);
    for (const auto& entry : automaticResult.manifest) {
        assert(entry.logicalKey.rfind("F_", 0) == 0);
    }

    auto explicitRapid = Request(PreciseBundlePlan::StandardPpp);
    explicitRapid.latency = PreciseBundleLatency::Rapid;
    const auto rapidResult = PlanPreciseBundle(explicitRapid, catalog);
    assert(rapidResult.valid && rapidResult.complete);
    assert(rapidResult.latency == "RAPID");
    for (const auto& entry : rapidResult.manifest) {
        assert(entry.logicalKey.rfind("R_", 0) == 0);
    }
}

void TestBdsFrequencyIsPartOfMatching() {
    const std::string b1i = "B1I/B3I";
    const std::string b1c = "B1C/B2A";
    auto catalog = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                   "CAS", "FINAL", b1i, "I_");
    auto b1cCatalog = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                      "CAS", "FINAL", b1c, "C_");
    for (auto& product : catalog.products) product.constellation = "C";
    for (auto& product : b1cCatalog.products) product.constellation = "C";
    catalog.products.insert(catalog.products.end(), b1cCatalog.products.begin(), b1cCatalog.products.end());

    auto request = Request(PreciseBundlePlan::StandardPpp);
    request.systems = "BDS";
    request.referenceFrequency = b1i;
    const auto b1iResult = PlanPreciseBundle(request, catalog);
    assert(b1iResult.valid && b1iResult.complete);
    assert(b1iResult.referenceFrequency == b1i);
    assert(b1iResult.familyKey.find("B1I/B3I") != std::string::npos);
    for (const auto& entry : b1iResult.manifest) {
        assert(entry.logicalKey.rfind("I_", 0) == 0);
    }

    request.referenceFrequency = b1c;
    const auto b1cResult = PlanPreciseBundle(request, catalog);
    assert(b1cResult.valid && b1cResult.complete);
    assert(b1cResult.referenceFrequency == b1c);
    assert(b1cResult.familyKey.find("B1C/B2A") != std::string::npos);
}

void TestWhuWumAliasAndManifestSerialization() {
    auto catalog = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                   "WHU", "RAPID", "GPS L1/L2", "");
    for (auto& product : catalog.products) {
        // Mix the historical WHU spelling with the current WUM spelling.
        if (product.type != Type::Sp3) product.center = "WUM";
    }
    assert(NormalizeBundleToken("WHU") == "WUM");
    assert(NormalizeBundleToken("WHU0OPS") == "WUM0OPS");
    const auto result = PlanPreciseBundle(
        Request(PreciseBundlePlan::StandardPpp), catalog);
    assert(result.valid && result.complete);
    assert(result.center == "WUM");
    assert(result.series == "WUM0OPS");
    assert(result.orbitInterval == "05M");
    assert(result.clockInterval == "30S");
    assert(result.referenceFrame == "IGS14");
    assert(result.apcModel == "APC");

    std::size_t erpEntries = 0;
    std::size_t antexEntries = 0;
    for (const auto& entry : result.manifest) {
        if (entry.type == Type::Erp) ++erpEntries;
        if (entry.type == Type::Antex) ++antexEntries;
    }
    assert(erpEntries == 1);
    assert(antexEntries == 1);

    const auto json = SerializePreciseBundleManifestJson(result);
    assert(json.find("\"complete\":true") != std::string::npos);
    assert(json.find("\"entries\":[") != std::string::npos);
    assert(json.find("\"series\":\"WUM0OPS\"") != std::string::npos);
    assert(json.find("\"clockInterval\":\"30S\"") != std::string::npos);
    assert(json.find("\"referenceFrame\":\"IGS14\"") != std::string::npos);
    assert(json.find("WUM/PRIDE PPP-AR") == std::string::npos);
    const auto csv = SerializePreciseBundleManifestCsv(result);
    assert(csv.rfind(
        "date,type,logicalKey,fileName,routeCount,center,series,productLine,"
        "latency,constellation,referenceFrequency,interval,orbitInterval,"
        "clockInterval,biasConvention,referenceFrame,apcModel\n", 0) == 0);
    assert(csv.find("ERP,ERP_WEEK") != std::string::npos);
}

void TestAllFrequencyBiasSatisfiesExplicitBdsPair() {
    auto catalog = CompleteCatalog(PreciseBundlePlan::WumPridePppAr,
                                   "WUM", "RAPID", "", "A_");
    for (auto& product : catalog.products) {
        if (product.type >= Type::SinexCrd) continue;
        product.constellation = "C";
        product.referenceFrequency.clear();
        if (product.type == Type::Osb) {
            product.referenceFrequency = "ALL_FREQUENCY";
        }
    }
    auto request = Request(PreciseBundlePlan::WumPridePppAr);
    request.systems = "BDS";
    request.referenceFrequency = "B1I/B3I";
    const auto result = PlanPreciseBundle(request, catalog);
    assert(result.valid && result.complete);
    assert(result.referenceFrequency == "B1I/B3I");
    assert(std::ranges::any_of(result.manifest, [](const auto& entry) {
        return entry.type == Type::Osb
            && entry.compatibility.referenceFrequency == "ALL_FREQUENCY";
    }));
}

void TestOpenEndedCoverage() {
    auto catalog = CompleteCatalog(PreciseBundlePlan::StandardPpp);
    // A static calibration/product row may have only one known validity
    // boundary.  An omitted end must mean "continues indefinitely", rather
    // than silently narrowing the row to the query date.
    for (auto& product : catalog.products) {
        if (product.type == Type::Antex) {
            product.coverageStart = kFirst;
            product.coverageEnd = {};
        }
    }
    const auto result = PlanPreciseBundle(
        Request(PreciseBundlePlan::StandardPpp), catalog);
    assert(result.valid && result.complete);
    assert(result.completeDays == 2);
}

void TestAntexPrefersCurrentAndLatestRevision() {
    auto catalog = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                   "IGS", "FINAL", "GPS L1/L2", "A_");
    auto old = std::ranges::find_if(catalog.products,
        [](const auto& product) { return product.type == Type::Antex; });
    assert(old != catalog.products.end());
    old->logicalKey = "ANTEX_OLD";
    old->fileName = "igs_old.atx";
    old->revisionWeek = 2400;
    old->currentAlias = false;

    auto current = *old;
    current.logicalKey = "ANTEX_CURRENT";
    current.fileName = "igs_current.atx";
    current.revisionWeek = -1;
    current.currentAlias = true;
    catalog.products.push_back(std::move(current));

    const auto result = PlanPreciseBundle(
        Request(PreciseBundlePlan::StandardPpp), catalog);
    assert(result.valid && result.complete);
    assert(std::ranges::any_of(result.manifest, [](const auto& entry) {
        return entry.type == Type::Antex
            && entry.fileName == "igs_current.atx";
    }));
}

void TestConstellationAliasesAndSubsetCompatibility() {
    auto catalog = CompleteCatalog(PreciseBundlePlan::WumPridePppAr,
                                   "WUM", "RAPID", "GPS L1/L2", "W_");
    // WUM OSB products intentionally omit GLONASS while the orbit family
    // advertises all supported systems.  Auxiliary systems must be a subset
    // of the SP3 anchor, not an exact string match.
    for (auto& product : catalog.products) {
        if (product.type >= Type::SinexCrd) continue;
        product.constellation = product.type == Type::Osb ? "GECJ" : "GRECJ";
    }
    auto request = Request(PreciseBundlePlan::WumPridePppAr);
    request.systems = "GPS BDS QZSS";
    const auto result = PlanPreciseBundle(request, catalog);
    assert(result.valid && result.complete);

    // BDS (C) and QZSS (J) are distinct RINEX designators.  A BDS-only
    // anchor must not satisfy a QZSS request, and vice versa.
    auto bdsOnly = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                   "CAS", "FINAL", "BDS_FREQ", "B_");
    for (auto& product : bdsOnly.products) product.constellation = "C";
    auto qzssRequest = Request(PreciseBundlePlan::StandardPpp);
    qzssRequest.systems = "QZSS";
    const auto rejected = PlanPreciseBundle(qzssRequest, bdsOnly);
    assert(!rejected.valid);

    auto textual = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                   "CAS", "FINAL", "BDS_FREQ", "T_");
    for (auto& product : textual.products) product.constellation = "BDS";
    auto bdsRequest = Request(PreciseBundlePlan::StandardPpp);
    bdsRequest.systems = "BDS";
    const auto accepted = PlanPreciseBundle(bdsRequest, textual);
    assert(accepted.valid && accepted.complete);
}

void TestAutomaticLatencyPreferenceAndCandidateSpecificity() {
    // Put Rapid rows before Final rows.  Automatic selection should still be
    // stable and prefer Final when both families cover the same dates.
    auto rapid = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                 "IGS", "RAPID", "GPS L1/L2", "R_");
    auto final = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                 "IGS", "FINAL", "GPS L1/L2", "F_");
    rapid.products.insert(rapid.products.end(), final.products.begin(),
                          final.products.end());
    const auto automatic = PlanPreciseBundle(
        Request(PreciseBundlePlan::StandardPpp), rapid);
    assert(automatic.valid && automatic.complete);
    assert(automatic.latency == "FINAL");
    for (const auto& entry : automatic.manifest) {
        assert(entry.logicalKey.rfind("F_", 0) == 0);
    }

    // An incomplete metadata row may appear first in a database result.  A
    // fully identified compatible row must win so strict mode does not emit
    // an avoidable wildcard candidate.
    auto catalog = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                   "IGS", "FINAL", "GPS L1/L2", "E_");
    auto unknown = Product(Type::Clk, kFirst, "", "", "", "U_");
    unknown.series.clear();
    unknown.productLine.clear();
    unknown.constellation.clear();
    unknown.referenceFrame.clear();
    unknown.apcModel.clear();
    catalog.products.insert(catalog.products.begin(), std::move(unknown));
    const auto ranked = PlanPreciseBundle(
        Request(PreciseBundlePlan::StandardPpp), catalog);
    assert(ranked.valid && ranked.complete);
    assert(std::ranges::all_of(ranked.manifest, [](const auto& entry) {
        return entry.logicalKey.rfind("E_", 0) == 0;
    }));

    // The same rule applies to the SP3 anchor itself.  A row with an unknown
    // series must not create a wildcard family that masks the identified
    // anchor inserted later in the catalog.
    auto anchorCatalog = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                         "IGS", "FINAL", "GPS L1/L2", "A_");
    auto unknownAnchor = Product(Type::Sp3, kFirst, "IGS", "FINAL",
                                 "GPS L1/L2", "U_");
    unknownAnchor.series.clear();
    unknownAnchor.productLine.clear();
    anchorCatalog.products.insert(anchorCatalog.products.begin(),
                                  std::move(unknownAnchor));
    const auto anchored = PlanPreciseBundle(
        Request(PreciseBundlePlan::StandardPpp), anchorCatalog);
    assert(anchored.valid && anchored.complete);
    assert(std::ranges::all_of(anchored.manifest, [](const auto& entry) {
        return entry.logicalKey.rfind("A_", 0) == 0;
    }));
}

void TestAutomaticPrefersCompleteRapidOverPartialFinal() {
    // A partially indexed Final family must not hide a complete Rapid family.
    // Automatic may prefer Final only when both latency levels cover the same
    // requested dates; it must never mix members from the two families.
    auto rapid = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                 "IGS", "RAPID", "GPS L1/L2", "R_");
    auto final = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                 "IGS", "FINAL", "GPS L1/L2", "F_");
    const auto missingFinalClock = std::ranges::find_if(
        final.products, [](const auto& product) {
            return product.type == Type::Clk
                && product.nominalDate.year == kSecond.year
                && product.nominalDate.month == kSecond.month
                && product.nominalDate.day == kSecond.day;
        });
    assert(missingFinalClock != final.products.end());
    final.products.erase(missingFinalClock);

    auto merged = std::move(rapid);
    merged.products.insert(merged.products.end(), final.products.begin(),
                           final.products.end());

    auto explicitFinal = Request(PreciseBundlePlan::StandardPpp);
    explicitFinal.latency = PreciseBundleLatency::Final;
    const auto finalResult = PlanPreciseBundle(explicitFinal, merged);
    assert(finalResult.valid && !finalResult.complete);
    assert(finalResult.completeDays == 1);
    assert(finalResult.latency == "FINAL");

    const auto automatic = PlanPreciseBundle(
        Request(PreciseBundlePlan::StandardPpp), merged);
    assert(automatic.valid && automatic.complete);
    assert(automatic.completeDays == 2);
    assert(automatic.latency == "RAPID");
    assert(std::ranges::all_of(automatic.manifest, [](const auto& entry) {
        return entry.logicalKey.rfind("R_", 0) == 0;
    }));
}

void TestAutoStandardPppChoosesCompleteProviderAndFastClock() {
    auto igs = CompleteCatalog(PreciseBundlePlan::AutoStandardPpp,
                               "IGS", "FINAL", "GPS L1/L2", "I_");
    igs.products.erase(std::ranges::find_if(igs.products,
        [](const auto& product) {
            return product.type == Type::Clk
                && product.nominalDate.day == kSecond.day;
        }));
    auto code = CompleteCatalog(PreciseBundlePlan::AutoStandardPpp,
                                "CODE", "FINAL", "GPS L1/L2", "C_");
    auto slowClock = Product(Type::Clk, kFirst, "CODE", "FINAL",
                             "GPS L1/L2", "A_SLOW_");
    slowClock.interval = "05M";
    code.products.push_back(std::move(slowClock));
    auto fastClock = Product(Type::Clk, kFirst, "CODE", "FINAL",
                             "GPS L1/L2", "Z_FAST_");
    fastClock.interval = "05S";
    code.products.push_back(std::move(fastClock));
    igs.products.insert(igs.products.end(), code.products.begin(),
                        code.products.end());

    const auto result = PlanPreciseBundle(
        Request(PreciseBundlePlan::AutoStandardPpp), igs);
    assert(result.valid && result.complete);
    assert(result.center == "CODE");
    for (const auto& member : result.manifest) {
        if (member.type == Type::Sp3 || member.type == Type::Clk) {
            assert(member.compatibility.center == "CODE");
        }
        if (member.type == Type::Clk) {
            assert(member.interval == (member.date.day == kFirst.day
                ? "05S" : "30S"));
        }
    }
}

void TestOsbPlanPrefersVerifiableFamily() {
    auto code = CompleteCatalog(PreciseBundlePlan::OsbPppAr,
                                "CODE", "FINAL", "", "C_");
    auto wum = CompleteCatalog(PreciseBundlePlan::OsbPppAr,
                               "WUM", "FINAL", "", "W_");
    for (auto& product : wum.products) {
        if (product.type == Type::Osb) {
            product.referenceFrequency = "ALL_FREQUENCY";
        }
    }
    code.products.insert(code.products.end(), wum.products.begin(),
                         wum.products.end());
    for (const auto plan : {PreciseBundlePlan::OsbPppAr,
                            PreciseBundlePlan::NetworkRtkPppRtk}) {
        auto request = Request(plan);
        request.strictMatching = true;
        const auto result = PlanPreciseBundle(request, code);
        assert(result.valid && result.complete);
        assert(result.center == "WUM");
        assert(result.warning.empty());
        assert(std::ranges::all_of(result.manifest, [](const auto& entry) {
            return entry.type == Type::Erp || entry.type == Type::Antex
                || entry.logicalKey.rfind("W_", 0) == 0;
        }));
    }
}

void TestExplicitProducerFilters() {
    auto other = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                  "CAS", "FINAL", "GPS L1/L2", "C_");
    auto igs = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                               "IGS", "FINAL", "GPS L1/L2", "I_");
    other.products.insert(other.products.end(), igs.products.begin(),
                          igs.products.end());
    auto request = Request(PreciseBundlePlan::StandardPpp);
    request.center = "IGS";
    const auto result = PlanPreciseBundle(request, other);
    assert(result.valid && result.complete);
    assert(result.center == "IGS");
    assert(std::ranges::all_of(result.manifest, [](const auto& entry) {
        return entry.logicalKey.rfind("I_", 0) == 0;
    }));

    // WHU is a historical spelling of WUM and must satisfy an explicit WUM
    // producer filter after normalization.
    auto whu = CompleteCatalog(PreciseBundlePlan::WumPridePppAr,
                                "WHU", "RAPID", "GPS L1/L2", "H_");
    auto wumRequest = Request(PreciseBundlePlan::WumPridePppAr);
    wumRequest.center = "WUM";
    const auto alias = PlanPreciseBundle(wumRequest, whu);
    assert(alias.valid && alias.complete);
    assert(alias.center == "WUM");
}

void TestGlobalEopCanPairWithNamedProductFamily() {
    auto catalog = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                   "IGS", "FINAL", "GPS L1/L2", "I_");
    for (auto& product : catalog.products) {
        if (product.type != Type::Erp) continue;
        // The real ERP database also contains IERS EOP series.  These are
        // global auxiliary products and must remain usable with a named IGS,
        // WUM, CODE, ... orbit/clock family without pretending that IERS is
        // the orbit analysis centre.
        product.center = "IERS";
        product.series = "IERS_FINALS_ALL_IAU2000";
        product.productLine = "EOP";
    }
    auto request = Request(PreciseBundlePlan::StandardPpp);
    request.center = "IGS";
    request.productLine = "OPS";
    request.latency = PreciseBundleLatency::Final;
    const auto result = PlanPreciseBundle(request, catalog);
    assert(result.valid && result.complete);
    assert(result.warning.empty());
    assert(std::ranges::any_of(result.manifest, [](const auto& entry) {
        return entry.type == Type::Erp
            && entry.logicalKey == "I_ERP_WEEK";
    }));
}

void TestCompactErpMetadataPairsWithNamedFamily() {
    auto catalog = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                   "WUM", "FINAL", "GPS L1/L2", "C_");
    for (auto& product : catalog.products) {
        // This is the shape emitted by the compact side index for an ERP
        // family that reuses the WUM/MGX producer labels.  Earth is not a
        // GNSS constellation and must not be compared with the SP3 anchor.
        if (product.type == Type::Erp) {
            product.center = "WUM";
            product.series = "WUM0MGXFIN";
            product.productLine = "MGX";
            product.constellation = "Earth";
        }
        // Compact planning deliberately has no physical routes yet.
        product.fileName = "(deferred)";
        product.routes.clear();
        product.routesDeferred = true;
    }
    const auto result = PlanPreciseBundle(
        Request(PreciseBundlePlan::StandardPpp), catalog);
    assert(result.valid && result.complete);
    assert(result.routesDeferred);
    assert(std::ranges::any_of(result.manifest, [](const auto& entry) {
        return entry.type == Type::Erp && entry.fileName == "(deferred)";
    }));
}

void TestUnknownDailyDateDoesNotCoverRange() {
    auto catalog = CompleteCatalog(PreciseBundlePlan::StandardPpp,
                                   "IGS", "FINAL", "GPS L1/L2", "D_");
    auto malformed = Product(Type::Clk, {}, "IGS", "FINAL", "GPS L1/L2",
                             "BAD_");
    malformed.nominalDate = {};
    malformed.coverageStart = {};
    malformed.coverageEnd = {};
    catalog.products.insert(catalog.products.begin(), std::move(malformed));
    const auto result = PlanPreciseBundle(
        Request(PreciseBundlePlan::StandardPpp), catalog);
    assert(result.valid && result.complete);
    assert(std::ranges::all_of(result.manifest, [](const auto& entry) {
        return entry.logicalKey.rfind("D_", 0) == 0;
    }));
}

void TestUpdVariantIsNotAnAnalysisSeries() {
    auto catalog = CompleteCatalog(PreciseBundlePlan::UpdPppAr,
                                   "WUM", "RAPID", "GPS L1/L2", "U_");
    // Keep the fixture's anchor and UPD on the same constellation; this test
    // isolates the fact that UC/IF is a bias variant rather than an SP3
    // analysis-series token.  UPD frequency-combination labels are tested as
    // an independent bias dimension below, not against the orbit's signal
    // reference pair.
    for (auto& product : catalog.products) {
        product.constellation = "C";
        product.referenceFrequency.clear();
    }
    for (auto& product : catalog.products) {
        if (product.type != Type::Upd) continue;
        product.center.clear();
        product.series = "UC";  // iGMAS directory variant
        product.productLine.clear();
        product.latency.clear();
        product.referenceFrequency = "B1I/B3I";
        product.constellation = "C";
        product.biasConvention = "WL";
    }
    const auto result = PlanPreciseBundle(
        Request(PreciseBundlePlan::UpdPppAr), catalog);
    assert(result.valid && result.complete);
    assert(!result.warning.empty());  // centre/latency remain unknown
}

} // namespace

int main() {
    TestRequiredPlansAndCompleteBundles();
    TestUnavailableDatesAndMissingMembers();
    TestWumRapidExtrasAndNoFinalFallback();
    TestLatencyDoesNotMixFinalAndRapid();
    TestBdsFrequencyIsPartOfMatching();
    TestWhuWumAliasAndManifestSerialization();
    TestAllFrequencyBiasSatisfiesExplicitBdsPair();
    TestOpenEndedCoverage();
    TestConstellationAliasesAndSubsetCompatibility();
    TestAutomaticLatencyPreferenceAndCandidateSpecificity();
    TestAutomaticPrefersCompleteRapidOverPartialFinal();
    TestAutoStandardPppChoosesCompleteProviderAndFastClock();
    TestOsbPlanPrefersVerifiableFamily();
    TestExplicitProducerFilters();
    TestGlobalEopCanPairWithNamedProductFamily();
    TestCompactErpMetadataPairsWithNamedFamily();
    TestUnknownDailyDateDoesNotCoverRange();
    TestAntexPrefersCurrentAndLatestRevision();
    TestUpdVariantIsNotAnAnalysisSeries();
    std::cout << "Precise bundle planner tests passed\n";
    return 0;
}
