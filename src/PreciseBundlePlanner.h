#pragma once

#include "Models.h"

#include <map>
#include <string>
#include <vector>

// Supported quick-download workflows. Values are deliberately
// stable so callers may persist a selection in settings or a manifest.
enum class PreciseBundlePlan {
    WumPridePppAr,
    StandardPpp,
    UpdPppAr,
    OsbPppAr,
    NetworkRtkPppRtk,
    AutoStandardPpp
};

enum class PreciseBundleLatency {
    Automatic,
    Final,
    Rapid
};

enum class PreciseBundleProductType {
    Sp3,
    Clk,
    Osb,
    Erp,
    Att,
    Upd,
    Antex,
    SinexCrd,
    SinexSol,
    Gim,
    Vmf1H00,
    Vmf1H06,
    Vmf1H12,
    Vmf1H18,
    Vmf3H00,
    Vmf3H06,
    Vmf3H12,
    Vmf3H18,
    Brdc
};

// Canonical fields used to prevent accidental cross-centre/product mixing.
// Empty fields are unknown (and therefore treated as a wildcard); populated
// fields must match exactly in strict mode.  orbitInterval and clockInterval
// are separate because a valid bundle normally uses different sampling rates
// for those two products.
struct PreciseBundleCompatibilityKey {
    std::string center;
    std::string series;
    std::string productLine;
    std::string latency;
    std::string constellation;
    std::string referenceFrequency;
    std::string orbitInterval;
    std::string clockInterval;
    std::string biasConvention;
    std::string referenceFrame;
    std::string apcModel;

    [[nodiscard]] std::string Canonical() const;
    friend bool operator==(const PreciseBundleCompatibilityKey& a,
                           const PreciseBundleCompatibilityKey& b) {
        return a.center == b.center && a.series == b.series &&
            a.productLine == b.productLine && a.latency == b.latency &&
            a.constellation == b.constellation &&
            a.referenceFrequency == b.referenceFrequency &&
            a.orbitInterval == b.orbitInterval &&
            a.clockInterval == b.clockInterval &&
            a.biasConvention == b.biasConvention &&
            a.referenceFrame == b.referenceFrame && a.apcModel == b.apcModel;
    }
};

// A database-neutral product row.  Adapters for SP3/CLK/ERP/etc. can fill
// whichever metadata their index knows; unknown values remain empty.
struct PreciseBundleProduct {
    PreciseBundleProductType type = PreciseBundleProductType::Sp3;
    std::string logicalKey;
    // The product catalogue's own row identity and name.  Quick plans use
    // these for display while retaining logicalKey for dated route lookup.
    std::wstring displayId;
    std::wstring displayName;
    QueryDate nominalDate{};
    QueryDate coverageStart{};
    QueryDate coverageEnd{};
    std::string fileName;
    std::string center;
    std::string series;
    std::string productLine;
    std::string latency;
    std::string constellation;
    std::string interval;
    std::string referenceFrequency;
    std::string biasConvention;
    std::string referenceFrame;
    std::string apcModel;
    // ANTEX may expose several overlapping historical revisions plus a
    // date-effective CURRENT alias.  Keep these fields so the planner can
    // choose the current/latest calibration deterministically instead of
    // relying on lexical logical-key order.
    int revisionWeek = -1;
    bool currentAlias = false;
    // A fast query may intentionally omit physical routes.  Such rows are
    // valid for compatibility/coverage planning, but must be hydrated from
    // the authoritative product database before a download is started.
    bool routesDeferred = false;
    std::vector<DownloadRoute> routes;
};

struct PreciseBundleCatalog {
    std::vector<PreciseBundleProduct> products;
    // Optional per-type gaps reported by a range loader.  A date listed here
    // is treated as unavailable even when a stale product row happens to be
    // present in the catalog.
    std::map<PreciseBundleProductType, std::vector<QueryDate>> unavailableDates;
};

struct PreciseBundleRequest {
    QueryDate startDate{};
    QueryDate endDate{};
    PreciseBundlePlan plan = PreciseBundlePlan::WumPridePppAr;
    PreciseBundleLatency latency = PreciseBundleLatency::Automatic;
    // Optional system/frequency constraint (for example "GPS GAL BDS" or
    // "BDS:B1I/B3I").  Matching is case-insensitive after normalization.
    std::string systems;
    std::string referenceFrequency;
    // Optional producer-family constraints.  Empty values preserve the
    // database-neutral planner's historical wildcard behaviour; UI presets
    // should fill these fields when a named workflow (for example WUM/PRIDE
    // or IGS Final) must not fall back to another analysis centre.
    std::string center;
    std::string series;
    std::string productLine;
    // Optional sampling constraints.  The WUM/PRIDE and standard PPP
    // presets set clockInterval to 30S so a 5-minute CLK cannot be selected
    // merely because it happens to be the first indexed row.
    std::string orbitInterval;
    std::string clockInterval;
    bool strictMatching = true;
};

struct PreciseBundleManifestEntry {
    QueryDate date{};
    PreciseBundleProductType type = PreciseBundleProductType::Sp3;
    std::string logicalKey;
    std::string fileName;
    std::vector<DownloadRoute> routes;
    // Preserve the exact metadata of the selected physical row.  A saved
    // manifest must be independently auditable; looking up a logical key in
    // a later/rebuilt database can otherwise describe a different revision.
    PreciseBundleCompatibilityKey compatibility;
    std::string interval;
};

struct PreciseBundleDateStatus {
    QueryDate date{};
    bool complete = false;
    std::vector<PreciseBundleProductType> missing;
    std::vector<PreciseBundleManifestEntry> members;
};

struct PreciseBundlePlanResult {
    bool valid = false;
    bool complete = false;
    bool strictMatching = true;
    PreciseBundlePlan plan = PreciseBundlePlan::WumPridePppAr;
    std::string familyKey;
    std::string center;
    std::string series;
    std::string productLine;
    std::string latency;
    std::string constellation;
    std::string referenceFrequency;
    std::string orbitInterval;
    std::string clockInterval;
    std::string biasConvention;
    std::string referenceFrame;
    std::string apcModel;
    int expectedDays = 0;
    int completeDays = 0;
    std::vector<QueryDate> missingDates;
    std::vector<PreciseBundleProductType> missingProductTypes;
    std::vector<PreciseBundleDateStatus> dates;
    std::vector<PreciseBundleManifestEntry> manifest;
    std::string error;
    std::string warning;
    bool routesDeferred = false;
};

// Normalization and compatibility helpers are public to keep database/UI
// adapters deterministic and unit-testable.
std::string NormalizeBundleToken(const std::string& value);
PreciseBundleCompatibilityKey MakeBundleCompatibilityKey(
    const PreciseBundleProduct& product);
bool BundleProductsCompatible(const PreciseBundleCompatibilityKey& a,
                              const PreciseBundleCompatibilityKey& b,
                              bool strict = true);
std::vector<PreciseBundleProductType> RequiredBundleProductTypes(
    PreciseBundlePlan plan);

PreciseBundlePlanResult PlanPreciseBundle(const PreciseBundleRequest& request,
                                          const PreciseBundleCatalog& catalog);

// Stable, dependency-free serialization for saving a generated manifest.
std::string SerializePreciseBundleManifestJson(const PreciseBundlePlanResult& result);
std::string SerializePreciseBundleManifestCsv(const PreciseBundlePlanResult& result);

const char* PreciseBundlePlanName(PreciseBundlePlan plan);
const char* PreciseBundleProductTypeName(PreciseBundleProductType type);
