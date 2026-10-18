#include "PreciseBundlePlanner.h"

#include "DateUtil.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>
#include <chrono>
#include <unordered_map>

namespace {

bool IsZeroDate(const QueryDate& d) {
    return d.year == 0 && d.month == 0 && d.day == 0;
}

bool IsValidDate(const QueryDate& d) {
    using namespace std::chrono;
    if (d.year < 1 || d.month < 1 || d.month > 12 || d.day < 1 || d.day > 31) return false;
    const year_month_day ymd{year{d.year}, month{static_cast<unsigned>(d.month)}, day{static_cast<unsigned>(d.day)}};
    return ymd.ok();
}

int DateCode(const QueryDate& d) {
    return d.year * 10000 + d.month * 100 + d.day;
}

bool Covers(const PreciseBundleProduct& p, const QueryDate& d) {
    const bool hasStart = !IsZeroDate(p.coverageStart);
    const bool hasEnd = !IsZeroDate(p.coverageEnd);
    if ((hasStart && !IsValidDate(p.coverageStart))
        || (hasEnd && !IsValidDate(p.coverageEnd))) {
        return false;
    }
    if (hasStart || hasEnd) {
        const int value = DateCode(d);
        const bool afterStart = !hasStart || value >= DateCode(p.coverageStart);
        const bool beforeEnd = !hasEnd || value <= DateCode(p.coverageEnd);
        return afterStart && beforeEnd;
    }
    // Only ANTEX is intentionally date-independent.  A daily SP3/CLK/OSB/
    // ATT/UPD row with no date metadata is malformed and must not turn every
    // requested day into a false positive.
    if (IsZeroDate(p.nominalDate)) {
        return p.type == PreciseBundleProductType::Antex;
    }
    if (!IsValidDate(p.nominalDate)) return false;
    if (p.type == PreciseBundleProductType::Erp) {
        // Weekly ERP rows in older indexes only carry the GPS-week start.
        const auto weekEnd = AddDays(p.nominalDate, 6);
        return DateCode(d) >= DateCode(p.nominalDate) && DateCode(d) <= DateCode(weekEnd);
    }
    return DateCode(p.nominalDate) == DateCode(d);
}

bool ListedUnavailable(const PreciseBundleCatalog& catalog,
                       const PreciseBundleProductType type,
                       const QueryDate& date) {
    const auto it = catalog.unavailableDates.find(type);
    if (it == catalog.unavailableDates.end()) return false;
    return std::any_of(it->second.begin(), it->second.end(), [&](const QueryDate& value) {
        return DateCode(value) == DateCode(date);
    });
}

std::vector<QueryDate> Enumerate(const QueryDate& start, const QueryDate& end) {
    std::vector<QueryDate> result;
    if (!IsValidDate(start) || !IsValidDate(end) || DateCode(start) > DateCode(end)) {
        return result;
    }
    QueryDate current = start;
    // A generous guard protects callers from malformed dates/DateUtil loops.
    for (int i = 0; i < 100000 && DateCode(current) <= DateCode(end); ++i) {
        result.push_back(current);
        if (DateCode(current) == DateCode(end)) {
            break;
        }
        current = AddDays(current, 1);
    }
    return result;
}

std::string MaybeNormalize(const std::string& value) {
    return NormalizeBundleToken(value);
}

bool FieldCompatible(const std::string& a, const std::string& b) {
    return a.empty() || b.empty() || a == b;
}

bool IsAllFrequencyToken(const std::string& value) {
    const auto token = NormalizeBundleToken(value);
    return token == "ALL" || token == "ALLFREQUENCY"
        || token == "ALL_FREQUENCY" || token == "FULLFREQUENCY"
        || token == "FULL_FREQUENCY";
}

bool ReferenceFrequencyCompatible(const std::string& a,
                                  const std::string& b) {
    return a.empty() || b.empty() || IsAllFrequencyToken(a)
        || IsAllFrequencyToken(b) || a == b;
}

constexpr std::string_view kBundleSystemDesignators = "GRECJSI";

char SystemDesignator(const std::string& raw) {
    const auto token = NormalizeBundleToken(raw);
    if (token == "GPS" || token == "NAVSTAR" || token == "G") return 'G';
    if (token == "GLO" || token == "GLONASS" || token == "GLONAS"
        || token == "R") return 'R';
    if (token == "GAL" || token == "GALILEO" || token == "GA"
        || token == "E") return 'E';
    if (token == "BDS" || token == "BEIDOU" || token == "BEI"
        || token == "COMPASS" || token == "C") return 'C';
    if (token == "QZSS" || token == "QZS" || token == "J") return 'J';
    if (token == "SBAS" || token == "SBS" || token == "WAAS"
        || token == "S") return 'S';
    if (token == "IRNSS" || token == "IRNS" || token == "NAVIC"
        || token == "IRN" || token == "I") return 'I';
    return '\0';
}

// Parse both the compact RINEX spelling ("GRECJ") and human-readable names
// ("GPS GAL BDS").  A null result means that the input contained no known
// system token; this is intentionally distinct from an empty field, which is
// handled as an unknown/wildcard by the compatibility rules.
std::optional<std::set<char>> ParseConstellationSet(
    const std::string& raw) {
    std::set<char> result;
    bool sawToken = false;
    for (std::size_t i = 0; i < raw.size();) {
        while (i < raw.size()
            && !std::isalnum(static_cast<unsigned char>(raw[i]))) {
            ++i;
        }
        const auto begin = i;
        while (i < raw.size()
            && std::isalnum(static_cast<unsigned char>(raw[i]))) {
            ++i;
        }
        if (begin == i) continue;
        const std::string token = raw.substr(begin, i - begin);
        if (const char designator = SystemDesignator(token);
            designator != '\0') {
            result.insert(designator);
            sawToken = true;
            continue;
        }

        // A compact token such as GRECJ is common in the availability
        // indexes.  Accept it only when every character is a known
        // one-letter designator; arbitrary words must remain unknown rather
        // than accidentally turning letters into satellite systems.
        const auto normalized = NormalizeBundleToken(token);
        if (normalized.size() > 1U) {
            bool compact = true;
            for (const char character : normalized) {
                if (kBundleSystemDesignators.find(character)
                    == std::string_view::npos) {
                    compact = false;
                    break;
                }
            }
            if (compact) {
                for (const char character : normalized) {
                    result.insert(character);
                }
                sawToken = true;
            }
        }
    }
    if (!sawToken) return std::nullopt;
    return result;
}

std::string CanonicalConstellation(const std::string& raw) {
    if (raw.empty()) return {};
    const auto parsed = ParseConstellationSet(raw);
    if (!parsed.has_value()) return NormalizeBundleToken(raw);
    std::string result;
    result.reserve(parsed->size());
    for (const char designator : kBundleSystemDesignators) {
        if (parsed->contains(designator)) result.push_back(designator);
    }
    return result;
}

bool ConstellationsEqual(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return true;
    const auto left = ParseConstellationSet(a);
    const auto right = ParseConstellationSet(b);
    if (left.has_value() && right.has_value()) return *left == *right;
    return a == b;
}

// Auxiliary products (notably WUM OSB and some ATT/ORBEX streams) may omit a
// constellation that is present in the SP3 family.  They must never add a
// system absent from the anchor, however.  This directional check is used by
// the planner; the public equality helper above remains useful for comparing
// two complete metadata keys.
bool ConstellationSubsetOf(const std::string& anchor,
                           const std::string& candidate) {
    if (anchor.empty() || candidate.empty()) return true;
    const auto anchorSet = ParseConstellationSet(anchor);
    const auto candidateSet = ParseConstellationSet(candidate);
    if (!anchorSet.has_value() || !candidateSet.has_value()) {
        return FieldCompatible(anchor, candidate);
    }
    return std::ranges::all_of(*candidateSet,
        [&](const char value) { return anchorSet->contains(value); });
}

bool NonConstellationFieldsCompatible(
    const PreciseBundleCompatibilityKey& a,
    const PreciseBundleCompatibilityKey& b) {
    return FieldCompatible(a.center, b.center) &&
        FieldCompatible(a.series, b.series) &&
        FieldCompatible(a.productLine, b.productLine) &&
        FieldCompatible(a.latency, b.latency) &&
        ReferenceFrequencyCompatible(
            a.referenceFrequency, b.referenceFrequency) &&
        FieldCompatible(a.orbitInterval, b.orbitInterval) &&
        FieldCompatible(a.clockInterval, b.clockInterval) &&
        FieldCompatible(a.biasConvention, b.biasConvention) &&
        FieldCompatible(a.referenceFrame, b.referenceFrame) &&
        FieldCompatible(a.apcModel, b.apcModel);
}

bool BundleProductCompatibleWithAnchor(
    const PreciseBundleCompatibilityKey& anchor,
    const PreciseBundleCompatibilityKey& candidate,
    const bool strict) {
    if (!strict) return true;
    return NonConstellationFieldsCompatible(anchor, candidate)
        && ConstellationSubsetOf(anchor.constellation, candidate.constellation);
}

bool IsWumSupplementType(const PreciseBundleProductType type) {
    return type >= PreciseBundleProductType::SinexCrd
        && type <= PreciseBundleProductType::Brdc;
}

bool MatchesWumSupplement(const PreciseBundleProduct& product) {
    const auto type = product.type;
    const auto center = NormalizeBundleToken(product.center);
    const auto series = NormalizeBundleToken(product.series);
    const auto line = NormalizeBundleToken(product.productLine);
    const auto latency = NormalizeBundleToken(product.latency);
    const auto constellation = NormalizeBundleToken(product.constellation);
    const auto interval = NormalizeBundleToken(product.interval);
    if (type == PreciseBundleProductType::SinexCrd
        || type == PreciseBundleProductType::SinexSol) {
        return center == "IGS" && series == "IGS0OPSSNX"
            && line == "DAILY" && latency == "FINAL"
            && constellation == (type == PreciseBundleProductType::SinexCrd
                ? "CRD/DAILY" : "SOL/DAILY");
    }
    if (type == PreciseBundleProductType::Gim) {
        return center == "CODE" && series == "CODE0OPSFIN"
            && latency == "FINAL" && constellation == "GIM";
    }
    if (type == PreciseBundleProductType::Brdc) {
        return center == "IGS" && series == "BRDC00IGS"
            && line == "MN" && interval == "RINEX3";
    }
    const bool vmf1 = type >= PreciseBundleProductType::Vmf1H00
        && type <= PreciseBundleProductType::Vmf1H18;
    const bool vmf3 = type >= PreciseBundleProductType::Vmf3H00
        && type <= PreciseBundleProductType::Vmf3H18;
    if (!vmf1 && !vmf3) return false;
    return center == "TUWIEN" && series == (vmf1 ? "VMF1" : "VMF3")
        && line == "OP" && latency == "DAILY" && interval == "06H";
}

std::optional<std::set<char>> RequestedConstellationSet(
    const std::string& raw) {
    return ParseConstellationSet(raw);
}

int LatencyPreference(const std::string& latency) {
    const auto normalized = NormalizeBundleToken(latency);
    if (normalized == "FINAL") return 0;
    if (normalized == "RAPID") return 1;
    // Unknown/legacy levels sort after the two supported automatic choices.
    return 2;
}

int MetadataSpecificity(const PreciseBundleCompatibilityKey& key) {
    return static_cast<int>(!key.center.empty())
        + static_cast<int>(!key.series.empty())
        + static_cast<int>(!key.productLine.empty())
        + static_cast<int>(!key.latency.empty())
        + static_cast<int>(!key.constellation.empty())
        + static_cast<int>(!key.referenceFrequency.empty())
        + static_cast<int>(!key.orbitInterval.empty())
        + static_cast<int>(!key.clockInterval.empty())
        + static_cast<int>(!key.biasConvention.empty())
        + static_cast<int>(!key.referenceFrame.empty())
        + static_cast<int>(!key.apcModel.empty());
}

bool IsGlobalEopProduct(const PreciseBundleProduct& product) {
    if (product.type != PreciseBundleProductType::Erp) return false;
    // WUM and other analysis centres publish their own ERP streams, even
    // though ERP has no GNSS constellation.  Generic Earth/IERS EOP rows may
    // bypass the orbit producer-family check; the named WUM Rapid stream is
    // kept centre-specific by the compatibility-key adapter below.
    const auto center = NormalizeBundleToken(product.center);
    const auto productLine = NormalizeBundleToken(product.productLine);
    const auto series = NormalizeBundleToken(product.series);
    const auto constellation = NormalizeBundleToken(product.constellation);
    return constellation == "EARTH" || center == "IERS" || productLine == "EOP"
        || series.starts_with("IERS");
}

int ClockIntervalSeconds(const std::string& interval) {
    const auto value = NormalizeBundleToken(interval);
    if (value.size() < 2) return 86400;
    const char unit = value.back();
    const int multiplier = unit == 'S' ? 1 : unit == 'M' ? 60
        : unit == 'H' ? 3600 : 0;
    if (multiplier == 0) return 86400;
    int count = 0;
    for (std::size_t index = 0; index + 1 < value.size(); ++index) {
        const unsigned char digit = static_cast<unsigned char>(value[index]);
        if (!std::isdigit(digit) || count > 86400 / multiplier / 10) {
            return 86400;
        }
        count = count * 10 + (digit - '0');
    }
    if (count <= 0 || count > 86400 / multiplier) return 86400;
    return count * multiplier;
}

bool PreferCandidate(const PreciseBundleProduct& candidate,
                     const PreciseBundleCompatibilityKey& candidateKey,
                     const PreciseBundleProduct& selected,
                     const PreciseBundleCompatibilityKey& selectedKey,
                     const PreciseBundleRequest& request) {
    if (candidate.currentAlias != selected.currentAlias) {
        return candidate.currentAlias;
    }
    if (candidate.revisionWeek != selected.revisionWeek
        && candidate.revisionWeek >= 0 && selected.revisionWeek >= 0) {
        return candidate.revisionWeek > selected.revisionWeek;
    }
    if (request.plan == PreciseBundlePlan::AutoStandardPpp
        && candidate.type == PreciseBundleProductType::Clk) {
        const int candidateSeconds = ClockIntervalSeconds(candidate.interval);
        const int selectedSeconds = ClockIntervalSeconds(selected.interval);
        if (candidateSeconds != selectedSeconds) {
            return candidateSeconds < selectedSeconds;
        }
    }
    const int candidateSpecificity = MetadataSpecificity(candidateKey);
    const int selectedSpecificity = MetadataSpecificity(selectedKey);
    if (candidateSpecificity != selectedSpecificity) {
        return candidateSpecificity > selectedSpecificity;
    }
    // Keep the richer mirror set when metadata is otherwise equivalent.
    if (candidate.routes.size() != selected.routes.size()) {
        return candidate.routes.size() > selected.routes.size();
    }
    if (candidate.logicalKey != selected.logicalKey) {
        return candidate.logicalKey < selected.logicalKey;
    }
    return candidate.fileName < selected.fileName;
}

bool MatchesRequest(const PreciseBundleProduct& p, const PreciseBundleRequest& req) {
    if (req.plan == PreciseBundlePlan::WumPridePppAr
        && p.type == PreciseBundleProductType::Erp) {
        return NormalizeBundleToken(p.center) == "WUM"
            && NormalizeBundleToken(p.series) == "WUM0MGXRAP"
            && NormalizeBundleToken(p.productLine) == "MGX"
            && NormalizeBundleToken(p.latency) == "RAPID";
    }
    if (req.plan == PreciseBundlePlan::WumPridePppAr
        && IsWumSupplementType(p.type)) {
        return MatchesWumSupplement(p);
    }
    // ANTEX is a calibration catalogue and ERP/EOP is a global
    // Earth-orientation product; neither is an analysis-centre orbit/clock/
    // bias stream.  Producer-family filters therefore do not apply to
    // either auxiliary type.  Core products must honor the requested
    // centre/series/project instead of silently falling back to another one.
    if (p.type != PreciseBundleProductType::Antex
        && !IsGlobalEopProduct(p)) {
        if (!req.center.empty()
            && NormalizeBundleToken(p.center)
                != NormalizeBundleToken(req.center)) {
            return false;
        }
        if (!req.series.empty()
            && NormalizeBundleToken(p.series)
                != NormalizeBundleToken(req.series)) {
            return false;
        }
        if (!req.productLine.empty()
            && NormalizeBundleToken(p.productLine)
                != NormalizeBundleToken(req.productLine)) {
            return false;
        }
    }
    if (req.latency == PreciseBundleLatency::Automatic) {
        // Automatic is intentionally limited to the two operational solution
        // levels.  Static ANTEX rows can have no latency label; the SP3 anchor
        // filter below still requires a populated Final/Rapid level in strict
        // mode, while unknown auxiliary labels remain wildcard metadata.
        if (p.type == PreciseBundleProductType::Sp3
            && !p.latency.empty()) {
            const auto latency = NormalizeBundleToken(p.latency);
            if (latency != "FINAL" && latency != "RAPID") return false;
        }
    } else {
        const std::string expected = req.latency == PreciseBundleLatency::Final ? "FINAL" : "RAPID";
        // Static catalogues (ANTEX) and legacy UPD indexes may not carry a
        // Final/Rapid label.  An empty value is unknown and is accepted here;
        // strict compatibility still reports it in the result warning.  A
        // populated, conflicting label is never silently mixed.
        if (!p.latency.empty() && NormalizeBundleToken(p.latency) != expected) {
            return false;
        }
    }
    if (!req.referenceFrequency.empty()) {
        const auto candidateFrequency = MaybeNormalize(p.referenceFrequency);
        const auto requestedFrequency = MaybeNormalize(req.referenceFrequency);
        // Orbit/EOP/ANTEX products are frequency-independent and many CLK
        // indexes do not encode a signal reference.  Bias products are the
        // members for which an explicit reference pair is mandatory; a
        // populated conflicting value on any member is still rejected.
        const bool frequencyDefining =
            p.type == PreciseBundleProductType::Osb
            || p.type == PreciseBundleProductType::Upd;
        if (candidateFrequency.empty() && req.strictMatching
            && frequencyDefining) return false;
        if (!candidateFrequency.empty()
            && !ReferenceFrequencyCompatible(
                candidateFrequency, requestedFrequency)) {
            return false;
        }
    }
    std::string requestedInterval;
    if (p.type == PreciseBundleProductType::Sp3) {
        requestedInterval = req.orbitInterval;
    } else if (p.type == PreciseBundleProductType::Clk) {
        requestedInterval = req.clockInterval;
        if (requestedInterval.empty()
            && (req.plan == PreciseBundlePlan::WumPridePppAr
                || req.plan == PreciseBundlePlan::StandardPpp)) {
            // Both first-release PPP presets are defined around the 30-second
            // precise-clock stream.  Callers can override this explicitly,
            // but never let a 5-minute CLK win by database row order.
            requestedInterval = "30S";
        }
    }
    if (!requestedInterval.empty()) {
        const auto candidateInterval = NormalizeBundleToken(p.interval);
        if (candidateInterval.empty() && req.strictMatching) return false;
        if (!candidateInterval.empty()
            && candidateInterval != NormalizeBundleToken(requestedInterval)) {
            return false;
        }
    }
    if (!req.systems.empty()) {
        const auto requested = RequestedConstellationSet(req.systems);
        const auto available = ParseConstellationSet(p.constellation);
        // ERP/EOP and ANTEX are intentionally constellation-independent.  All
        // other members need a known constellation when strict mode is asked
        // to prove an explicit systems filter; accepting an empty field here
        // would otherwise turn a missing system declaration into a wildcard.
        const bool constellationIndependent =
            p.type == PreciseBundleProductType::Erp
            || p.type == PreciseBundleProductType::Antex;
        if (req.strictMatching && !constellationIndependent
            && requested.has_value()
            && (!available.has_value() || p.constellation.empty())) {
            return false;
        }
        // Unknown textual filters/candidate metadata remain wildcards.  When
        // both sides identify known systems, every requested system must be
        // represented by the candidate (BDS=C and QZSS=J are distinct).
        if (requested.has_value() && available.has_value()
            && !std::ranges::all_of(*requested,
                [&](const char value) { return available->contains(value); })) {
            return false;
        }
    }
    return true;
}

// SP3 is the family anchor for every quick plan.  In strict mode it must be
// identifiable on the dimensions that the anchor database can actually
// provide; otherwise an unknown row can create a wildcard family and permit
// a different series/solution to be selected for the remaining members.  A
// clock interval is deliberately not checked here because it belongs to CLK,
// not SP3; explicit interval/frequency/system requests are enforced by
// MatchesRequest for each corresponding candidate type above.
bool AnchorMetadataCompleteForRequest(
    const PreciseBundleCompatibilityKey& key,
    const PreciseBundleRequest& request) {
    if (key.center.empty() || key.series.empty() || key.latency.empty()) {
        return false;
    }
    if (!request.productLine.empty() && key.productLine.empty()) return false;
    if (!request.orbitInterval.empty() && key.orbitInterval.empty()) return false;
    if (!request.systems.empty()) {
        const auto requested = RequestedConstellationSet(request.systems);
        if (requested.has_value()
            && (key.constellation.empty()
                || !ParseConstellationSet(key.constellation).has_value())) {
            return false;
        }
    }
    return true;
}

std::string FamilyKey(const PreciseBundleCompatibilityKey& key) {
    return key.Canonical();
}

void MergeManifestRoutes(std::vector<DownloadRoute>& destination,
                         const std::vector<DownloadRoute>& incoming) {
    for (const auto& route : incoming) {
        const auto duplicate = std::ranges::find_if(destination,
            [&](const DownloadRoute& existing) {
                if (!route.url.empty() && !existing.url.empty()) {
                    return route.url == existing.url;
                }
                return route.source == existing.source
                    && route.fileName == existing.fileName;
            });
        if (duplicate == destination.end()) destination.push_back(route);
    }
}

bool HasUnknownCriticalMetadata(
    const PreciseBundleProductType type,
    const PreciseBundleCompatibilityKey& key,
    const PreciseBundleRequest& request) {
    if (request.plan == PreciseBundlePlan::WumPridePppAr
        && IsWumSupplementType(type)) return false;
    if (type != PreciseBundleProductType::Antex
        && type != PreciseBundleProductType::Erp
        && (key.center.empty() || key.series.empty())) return true;
    if (type != PreciseBundleProductType::Antex && key.latency.empty()) {
        return true;
    }
    if (type != PreciseBundleProductType::Erp
        && type != PreciseBundleProductType::Antex
        && key.constellation.empty()) {
        return true;
    }
    if (type == PreciseBundleProductType::Osb && key.biasConvention.empty()) {
        return true;
    }
    if (type == PreciseBundleProductType::Antex
        && (key.referenceFrame.empty() || key.apcModel.empty())) {
        return true;
    }
    if (!request.referenceFrequency.empty()
        && (type == PreciseBundleProductType::Osb
            || type == PreciseBundleProductType::Upd)
        && key.referenceFrequency.empty()) {
        return true;
    }
    if (type == PreciseBundleProductType::Osb
        && key.referenceFrequency.empty()) return true;
    if (type == PreciseBundleProductType::Upd
        && key.referenceFrequency.empty()) {
        return true;
    }
    return false;
}

} // namespace

std::string NormalizeBundleToken(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(std::toupper(ch)));
        } else if (ch == '/' || ch == ':' || ch == '+' || ch == '-' || ch == '_') {
            out.push_back(ch == '+' ? '/' : ch);
        }
    }
    // Historical WUM/WHU aliases identify the same analysis-centre line.
    if (out == "WHU") out = "WUM";
    else if (out.starts_with("WHU")) out.replace(0, 3, "WUM");
    // CODE products are commonly shortened to COD in legacy archive names.
    // Keep the alias canonical in planner keys so a COD ERP/CLK row cannot
    // be mistaken for a different analysis centre from CODE.
    if (out == "COD") out = "CODE";
    else if (out.starts_with("COD0")) out.replace(0, 3, "CODE");
    if (out == "B2A") out = "B2A"; // keep B1I/B3I and B1C/B2A distinct
    return out;
}

std::string PreciseBundleCompatibilityKey::Canonical() const {
    std::ostringstream stream;
    stream << center << '|' << series << '|' << productLine << '|'
           << latency << '|' << constellation << '|' << referenceFrequency
           << "|orb=" << orbitInterval << "|clk=" << clockInterval << '|'
           << biasConvention << '|' << referenceFrame << '|' << apcModel;
    return stream.str();
}

PreciseBundleCompatibilityKey MakeBundleCompatibilityKey(
    const PreciseBundleProduct& p) {
    PreciseBundleCompatibilityKey key;
    key.center = NormalizeBundleToken(p.center);
    key.series = NormalizeBundleToken(p.series);
    key.productLine = NormalizeBundleToken(p.productLine);
    key.latency = NormalizeBundleToken(p.latency);
    key.constellation = CanonicalConstellation(p.constellation);
    key.referenceFrequency = NormalizeBundleToken(p.referenceFrequency);
    key.biasConvention = NormalizeBundleToken(p.biasConvention);
    key.referenceFrame = NormalizeBundleToken(p.referenceFrame);
    key.apcModel = NormalizeBundleToken(p.apcModel);
    // iGMAS UPD rows use the directory variant (UC/IF) in `variant`; that is
    // not an analysis-centre product series and cannot be compared with an
    // SP3 prefix.  Treat these dimensions as unknown/wildcards while keeping
    // the UPD class, frequency and constellation available for matching.
    if (p.type == PreciseBundleProductType::Upd) {
        key.series.clear();
        key.productLine.clear();
        key.latency.clear();
    }
    // ERP/EOP is paired by its validity interval and latency/standard, not by
    // the SP3 analysis-centre prefix.  Keep latency for Final/Rapid matching
    // but treat IERS (or another EOP producer) as a wildcard producer family.
    if (p.type == PreciseBundleProductType::Erp) {
        key.constellation.clear();
    }
    if (IsGlobalEopProduct(p)
        && NormalizeBundleToken(p.series) != "WUM0MGXRAP") {
        key.center.clear();
        key.series.clear();
        key.productLine.clear();
    }
    if (p.type == PreciseBundleProductType::Sp3) key.orbitInterval = NormalizeBundleToken(p.interval);
    if (p.type == PreciseBundleProductType::Clk) key.clockInterval = NormalizeBundleToken(p.interval);
    return key;
}

bool BundleProductsCompatible(const PreciseBundleCompatibilityKey& a,
                              const PreciseBundleCompatibilityKey& b,
                              const bool strict) {
    if (!strict) return true;
    return NonConstellationFieldsCompatible(a, b)
        && ConstellationsEqual(a.constellation, b.constellation);
}

std::vector<PreciseBundleProductType> RequiredBundleProductTypes(
    const PreciseBundlePlan plan) {
    switch (plan) {
    case PreciseBundlePlan::WumPridePppAr:
        return {PreciseBundleProductType::Sp3, PreciseBundleProductType::Clk,
                PreciseBundleProductType::Osb, PreciseBundleProductType::Erp,
                PreciseBundleProductType::Att, PreciseBundleProductType::Antex,
                PreciseBundleProductType::SinexCrd,
                PreciseBundleProductType::SinexSol,
                PreciseBundleProductType::Gim,
                PreciseBundleProductType::Vmf1H00,
                PreciseBundleProductType::Vmf1H06,
                PreciseBundleProductType::Vmf1H12,
                PreciseBundleProductType::Vmf1H18,
                PreciseBundleProductType::Vmf3H00,
                PreciseBundleProductType::Vmf3H06,
                PreciseBundleProductType::Vmf3H12,
                PreciseBundleProductType::Vmf3H18,
                PreciseBundleProductType::Brdc};
    case PreciseBundlePlan::StandardPpp:
    case PreciseBundlePlan::AutoStandardPpp:
        return {PreciseBundleProductType::Sp3, PreciseBundleProductType::Clk,
                PreciseBundleProductType::Erp, PreciseBundleProductType::Antex};
    case PreciseBundlePlan::UpdPppAr:
        return {PreciseBundleProductType::Sp3, PreciseBundleProductType::Clk,
                PreciseBundleProductType::Upd, PreciseBundleProductType::Erp,
                PreciseBundleProductType::Antex};
    case PreciseBundlePlan::OsbPppAr:
    case PreciseBundlePlan::NetworkRtkPppRtk:
        return {PreciseBundleProductType::Sp3, PreciseBundleProductType::Clk,
                PreciseBundleProductType::Osb, PreciseBundleProductType::Erp,
                PreciseBundleProductType::Antex};
    }
    return {};
}

PreciseBundlePlanResult PlanPreciseBundle(const PreciseBundleRequest& request,
                                          const PreciseBundleCatalog& catalog) {
    PreciseBundlePlanResult result;
    result.plan = request.plan;
    result.strictMatching = request.strictMatching;
    const auto dates = Enumerate(request.startDate, request.endDate);
    result.expectedDays = static_cast<int>(dates.size());
    if (dates.empty()) {
        result.error = "Invalid or empty date range";
        return result;
    }
    const auto required = RequiredBundleProductTypes(request.plan);

    // Build a date/type index once.  The previous planner scanned every
    // catalog row for every (anchor, date, required-type) combination.  That
    // is acceptable for a compact summary, but a confirmed multi-year quick
    // bundle contains one hydrated row per day and turns into billions of
    // redundant predicate checks.  Daily rows are inserted by nominal date;
    // range/static rows (ERP, ANTEX and deferred family runs) are expanded
    // only across the requested dates.
    using ProductPtr = const PreciseBundleProduct*;
    std::unordered_map<int,
        std::unordered_map<int, std::vector<ProductPtr>>> candidatesByType;
    for (const auto& candidate : catalog.products) {
        auto& byDate = candidatesByType[static_cast<int>(candidate.type)];
        const bool hasCoverage = !IsZeroDate(candidate.coverageStart)
            || !IsZeroDate(candidate.coverageEnd);
        if (candidate.type == PreciseBundleProductType::Antex
            || hasCoverage
            || candidate.type == PreciseBundleProductType::Erp) {
            for (const auto& date : dates) {
                if (Covers(candidate, date)) {
                    byDate[DateCode(date)].push_back(&candidate);
                }
            }
        } else if (!IsZeroDate(candidate.nominalDate)) {
            const int code = DateCode(candidate.nominalDate);
            if (std::ranges::any_of(dates,
                    [&](const QueryDate& date) {
                        return DateCode(date) == code;
                    })) {
                byDate[code].push_back(&candidate);
            }
        }
    }

    std::vector<const PreciseBundleProduct*> anchors;
    for (const auto& p : catalog.products) {
        if (p.type != PreciseBundleProductType::Sp3 || !MatchesRequest(p, request)) continue;
        const auto key = MakeBundleCompatibilityKey(p);
        // The SP3 anchor defines the family.  Without the core identity fields
        // there is no safe way to prevent cross-analysis mixing in strict
        // mode.  Explicit request dimensions are checked as well; unknown
        // values must not hide a better, fully identified anchor row.
        if (request.strictMatching
            && !AnchorMetadataCompleteForRequest(key, request)) {
            continue;
        }
        anchors.push_back(&p);
    }
    if (anchors.empty()) {
        result.error = "No SP3 candidates satisfy the requested filters";
        return result;
    }

    // Evaluate each distinct SP3 family and retain the one with the greatest
    // date intersection.  This gives deterministic behaviour for automatic
    // Final/Rapid selection while still reporting every missing date.
    std::set<std::string> seen;
    int bestComplete = -1;
    bool bestHasUnknownCritical = true;
    int bestLatencyPreference = 99;
    long long bestClockPreference = LLONG_MAX;
    std::string bestFamily;
    const PreciseBundleProduct* bestAnchor = nullptr;
    std::vector<PreciseBundleDateStatus> bestStatuses;
    for (const auto* anchor : anchors) {
        const auto anchorKey = MakeBundleCompatibilityKey(*anchor);
        const auto family = FamilyKey(anchorKey);
        if (!seen.insert(family).second) continue;
        std::vector<PreciseBundleDateStatus> statuses;
        int completeDays = 0;
        for (const auto& date : dates) {
            PreciseBundleDateStatus status;
            status.date = date;
            for (const auto type : required) {
                const PreciseBundleProduct* selected = nullptr;
                PreciseBundleCompatibilityKey selectedKey;
                const auto typeIt = candidatesByType.find(
                    static_cast<int>(type));
                const std::vector<ProductPtr>* dateCandidates = nullptr;
                if (typeIt != candidatesByType.end()) {
                    const auto dateIt = typeIt->second.find(DateCode(date));
                    if (dateIt != typeIt->second.end()) {
                        dateCandidates = &dateIt->second;
                    }
                }
                if (ListedUnavailable(catalog, type, date)
                    || dateCandidates == nullptr) {
                    status.missing.push_back(type);
                    continue;
                }
                for (const auto* candidate : *dateCandidates) {
                    if (candidate == nullptr
                        || (!candidate->routesDeferred
                            && (candidate->fileName.empty()
                                || candidate->routes.empty()))
                        || !MatchesRequest(*candidate, request)) continue;
                    const auto candidateKey = MakeBundleCompatibilityKey(*candidate);
                    if (!IsWumSupplementType(candidate->type)
                        && !BundleProductCompatibleWithAnchor(
                            anchorKey,
                            // iGMAS UPD ``frequency_combination`` values
                            // (for example WL/NL or EWL) describe the bias
                            // solution, not the SP3 orbit's signal-reference
                            // pair.  They must still be filtered against an
                            // explicit request above, but cannot be compared
                            // directly with an anchor's GNSS frequency token.
                            candidate->type == PreciseBundleProductType::Upd
                                ? [&] {
                                    auto comparable = candidateKey;
                                    comparable.referenceFrequency.clear();
                                    return comparable;
                                }()
                                : candidateKey,
                            request.strictMatching)) {
                        continue;
                    }
                    if (selected == nullptr
                        || PreferCandidate(*candidate, candidateKey,
                            *selected, selectedKey, request)) {
                        selected = candidate;
                        selectedKey = candidateKey;
                    }
                }
                if (!selected) {
                    status.missing.push_back(type);
                } else {
                    status.members.push_back({date, type, selected->logicalKey,
                                              selected->fileName, selected->routes,
                                              selectedKey,
                                              NormalizeBundleToken(
                                                  selected->interval)});
                }
            }
            status.complete = status.missing.empty();
            if (status.complete) ++completeDays;
            statuses.push_back(std::move(status));
        }
        const int latencyPreference = LatencyPreference(anchorKey.latency);
        long long clockPreference = 0;
        if (request.plan == PreciseBundlePlan::AutoStandardPpp) {
            for (const auto& status : statuses) {
                for (const auto& member : status.members) {
                    if (member.type != PreciseBundleProductType::Clk) continue;
                    clockPreference += ClockIntervalSeconds(member.interval);
                }
            }
        }
        bool hasUnknownCritical = false;
        if (request.strictMatching) {
            for (const auto& status : statuses) {
                for (const auto& member : status.members) {
                    if (HasUnknownCriticalMetadata(
                            member.type, member.compatibility, request)) {
                        hasUnknownCritical = true;
                        break;
                    }
                }
                if (hasUnknownCritical) break;
            }
        }
        // Automatic must first maximize usable date coverage.  Otherwise a
        // partially indexed Final family could prevent a complete Rapid
        // family from satisfying the user's requested range.  For equally
        // complete strict plans, prefer a family whose compatibility metadata
        // can be verified before considering latency or lexical family names.
        // Otherwise a CODE OSB with an undeclared reference frequency can
        // hide a complete WUM family and leave the quick plan undownloadable.
        const bool better = completeDays > bestComplete
            || (completeDays == bestComplete
                && ((bestHasUnknownCritical && !hasUnknownCritical)
                    || (bestHasUnknownCritical == hasUnknownCritical
                        && (latencyPreference < bestLatencyPreference
                            || (latencyPreference == bestLatencyPreference
                                && (clockPreference < bestClockPreference
                                    || (clockPreference == bestClockPreference
                                        && (bestFamily.empty() || family < bestFamily))))))));
        if (better) {
            bestComplete = completeDays;
            bestHasUnknownCritical = hasUnknownCritical;
            bestLatencyPreference = latencyPreference;
            bestClockPreference = clockPreference;
            bestFamily = family;
            bestAnchor = anchor;
            bestStatuses = std::move(statuses);
        }
    }
    if (!bestAnchor) {
        result.error = "No compatible product family found";
        return result;
    }
    result.valid = true;
    result.completeDays = std::max(0, bestComplete);
    const auto bestKey = MakeBundleCompatibilityKey(*bestAnchor);
    result.familyKey = FamilyKey(bestKey);
    result.center = bestKey.center;
    result.series = bestKey.series;
    result.productLine = bestKey.productLine.empty() ? bestKey.series : bestKey.productLine;
    result.latency = bestKey.latency;
    result.constellation = bestKey.constellation;
    result.orbitInterval = bestKey.orbitInterval;
    result.dates = std::move(bestStatuses);
    result.routesDeferred = std::ranges::any_of(result.dates,
        [](const PreciseBundleDateStatus& status) {
            return std::ranges::any_of(status.members,
                [](const PreciseBundleManifestEntry& member) {
                    return member.routes.empty();
                });
        });

    // Summarise the exact selected members for UI/report consumers.  The
    // per-entry compatibility key below remains authoritative; these compact
    // fields intentionally join multiple date-effective values (for example
    // an ANTEX frame transition over a long request) instead of hiding them.
    std::set<std::string> referenceFrequencies;
    std::set<std::string> clockIntervals;
    std::set<std::string> biasConventions;
    std::set<std::string> referenceFrames;
    std::set<std::string> apcModels;
    if (!bestKey.referenceFrequency.empty()) {
        referenceFrequencies.insert(bestKey.referenceFrequency);
    }
    for (const auto& status : result.dates) {
        for (const auto& member : status.members) {
            const auto& key = member.compatibility;
            if ((member.type == PreciseBundleProductType::Osb
                    || member.type == PreciseBundleProductType::Upd)
                && !key.referenceFrequency.empty()) {
                referenceFrequencies.insert(key.referenceFrequency);
            }
            if (member.type == PreciseBundleProductType::Clk
                && !key.clockInterval.empty()) {
                clockIntervals.insert(key.clockInterval);
            }
            if ((member.type == PreciseBundleProductType::Osb
                    || member.type == PreciseBundleProductType::Upd)
                && !key.biasConvention.empty()) {
                biasConventions.insert(key.biasConvention);
            }
            if (member.type == PreciseBundleProductType::Antex) {
                if (!key.referenceFrame.empty()) {
                    referenceFrames.insert(key.referenceFrame);
                }
                if (!key.apcModel.empty()) apcModels.insert(key.apcModel);
            }
        }
    }
    const auto joinMetadata = [](const std::set<std::string>& values) {
        std::string joined;
        for (const auto& value : values) {
            if (!joined.empty()) joined += ";";
            joined += value;
        }
        return joined;
    };
    result.referenceFrequency = !request.referenceFrequency.empty()
        ? NormalizeBundleToken(request.referenceFrequency)
        : joinMetadata(referenceFrequencies);
    result.clockInterval = joinMetadata(clockIntervals);
    result.biasConvention = joinMetadata(biasConventions);
    result.referenceFrame = joinMetadata(referenceFrames);
    result.apcModel = joinMetadata(apcModels);
    std::set<std::string> manifestKeys;
    for (const auto& status : result.dates) {
        if (!status.complete) {
            result.missingDates.push_back(status.date);
            for (const auto type : status.missing) {
                if (std::find(result.missingProductTypes.begin(), result.missingProductTypes.end(), type) == result.missingProductTypes.end()) {
                    result.missingProductTypes.push_back(type);
                }
            }
        } else {
            for (const auto& member : status.members) {
                // Static ANTEX and weekly ERP rows can be selected on many
                // dates; emit one download entry for the same logical file.
                std::ostringstream key;
                key << static_cast<int>(member.type) << '|' << member.logicalKey
                    << '|' << member.fileName;
                if (manifestKeys.insert(key.str()).second) {
                    result.manifest.push_back(member);
                } else {
                    // A static/weekly product can be represented by more
                    // than one catalog row over a range.  Keep one manifest
                    // entry but retain every distinct mirror discovered for
                    // that logical file.
                    const auto existing = std::ranges::find_if(
                        result.manifest, [&](const auto& value) {
                            std::ostringstream existingKey;
                            existingKey << static_cast<int>(value.type) << '|'
                                << value.logicalKey << '|' << value.fileName;
                            return existingKey.str() == key.str();
                        });
                    if (existing != result.manifest.end()) {
                        MergeManifestRoutes(existing->routes, member.routes);
                    }
                }
            }
        }
    }
    result.complete = result.completeDays == result.expectedDays;
    if (!result.strictMatching) {
        result.warning = "Non-strict matching enabled; product metadata may be mixed.";
    }
    if (result.strictMatching) {
        for (const auto& status : result.dates) {
            for (const auto& member : status.members) {
                // The selected row's key is copied into the date status at
                // selection time, so duplicate logical keys or a later index
                // rebuild cannot make this audit inspect the wrong revision.
                if (HasUnknownCriticalMetadata(
                        member.type, member.compatibility, request)) {
                    result.warning = "Strict plan contains candidates with unknown compatibility metadata.";
                    break;
                }
            }
            if (!result.warning.empty()) break;
        }
    }
    return result;
}

namespace {
std::string DateText(const QueryDate& d) {
    std::ostringstream s;
    s << d.year << '-';
    if (d.month < 10) s << '0'; s << d.month << '-';
    if (d.day < 10) s << '0'; s << d.day;
    return s.str();
}
std::string JsonEscape(const std::string& value) {
    std::string out; out.reserve(value.size() + 8);
    for (char c : value) {
        if (c == '\\' || c == '"') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out.push_back(c);
    }
    return out;
}
std::string CsvEscape(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) return value;
    std::string out = "\"";
    for (char c : value) { if (c == '"') out += "\"\""; else out.push_back(c); }
    return out + '"';
}
std::string Narrow(const std::wstring& value) {
    // Archive routes are ASCII URLs in current indexes.  Preserve printable
    // Unicode source labels without pulling a platform codec into this core.
    std::string out;
    out.reserve(value.size());
    for (wchar_t c : value) out.push_back(c >= 0 && c <= 0x7f ? static_cast<char>(c) : '?');
    return out;
}
}

std::string SerializePreciseBundleManifestJson(const PreciseBundlePlanResult& result) {
    std::ostringstream out;
    out << "{\"plan\":\"" << JsonEscape(PreciseBundlePlanName(result.plan))
        << "\",\"valid\":" << (result.valid ? "true" : "false")
        << ",\"complete\":" << (result.complete ? "true" : "false")
        << ",\"strictMatching\":" << (result.strictMatching ? "true" : "false")
        << ",\"expectedDays\":" << result.expectedDays
        << ",\"completeDays\":" << result.completeDays
        << ",\"familyKey\":\"" << JsonEscape(result.familyKey)
        << "\",\"center\":\"" << JsonEscape(result.center)
        << "\",\"series\":\"" << JsonEscape(result.series)
        << "\",\"productLine\":\"" << JsonEscape(result.productLine)
        << "\",\"latency\":\"" << JsonEscape(result.latency)
        << "\",\"constellation\":\"" << JsonEscape(result.constellation)
        << "\",\"referenceFrequency\":\""
        << JsonEscape(result.referenceFrequency)
        << "\",\"orbitInterval\":\"" << JsonEscape(result.orbitInterval)
        << "\",\"clockInterval\":\"" << JsonEscape(result.clockInterval)
        << "\",\"biasConvention\":\""
        << JsonEscape(result.biasConvention)
        << "\",\"referenceFrame\":\""
        << JsonEscape(result.referenceFrame)
        << "\",\"apcModel\":\"" << JsonEscape(result.apcModel)
        << "\",\"missingDates\":[";
    for (std::size_t i = 0; i < result.missingDates.size(); ++i) {
        if (i) out << ',';
        out << '\"' << DateText(result.missingDates[i]) << '\"';
    }
    out << "],\"missingProducts\":[";
    for (std::size_t i = 0; i < result.missingProductTypes.size(); ++i) {
        if (i) out << ',';
        out << '\"' << PreciseBundleProductTypeName(result.missingProductTypes[i]) << '\"';
    }
    out << "],\"warning\":\"" << JsonEscape(result.warning)
        << "\",\"error\":\"" << JsonEscape(result.error)
        << "\",\"entries\":[";
    for (std::size_t i = 0; i < result.manifest.size(); ++i) {
        const auto& e = result.manifest[i];
        if (i) out << ',';
        out << "{\"date\":\"" << DateText(e.date) << "\",\"type\":\""
            << PreciseBundleProductTypeName(e.type) << "\",\"logicalKey\":\""
            << JsonEscape(e.logicalKey) << "\",\"fileName\":\""
            << JsonEscape(e.fileName)
            << "\",\"center\":\"" << JsonEscape(e.compatibility.center)
            << "\",\"series\":\"" << JsonEscape(e.compatibility.series)
            << "\",\"productLine\":\""
            << JsonEscape(e.compatibility.productLine)
            << "\",\"latency\":\"" << JsonEscape(e.compatibility.latency)
            << "\",\"constellation\":\""
            << JsonEscape(e.compatibility.constellation)
            << "\",\"referenceFrequency\":\""
            << JsonEscape(e.compatibility.referenceFrequency)
            << "\",\"interval\":\"" << JsonEscape(e.interval)
            << "\",\"orbitInterval\":\""
            << JsonEscape(e.compatibility.orbitInterval)
            << "\",\"clockInterval\":\""
            << JsonEscape(e.compatibility.clockInterval)
            << "\",\"biasConvention\":\""
            << JsonEscape(e.compatibility.biasConvention)
            << "\",\"referenceFrame\":\""
            << JsonEscape(e.compatibility.referenceFrame)
            << "\",\"apcModel\":\""
            << JsonEscape(e.compatibility.apcModel)
            << "\",\"routes\":[";
        for (std::size_t r = 0; r < e.routes.size(); ++r) {
            if (r) out << ',';
            out << "{\"source\":\"" << JsonEscape(e.routes[r].source)
                << "\",\"fileName\":\"" << JsonEscape(e.routes[r].fileName)
                << "\",\"url\":\"" << JsonEscape(Narrow(e.routes[r].url)) << "\"}";
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

std::string SerializePreciseBundleManifestCsv(const PreciseBundlePlanResult& result) {
    std::ostringstream out;
    out << "date,type,logicalKey,fileName,routeCount,center,series,productLine,"
           "latency,constellation,referenceFrequency,interval,orbitInterval,"
           "clockInterval,biasConvention,referenceFrame,apcModel\n";
    for (const auto& e : result.manifest) {
        out << DateText(e.date) << ',' << PreciseBundleProductTypeName(e.type) << ','
            << CsvEscape(e.logicalKey) << ',' << CsvEscape(e.fileName) << ','
            << e.routes.size() << ',' << CsvEscape(e.compatibility.center) << ','
            << CsvEscape(e.compatibility.series) << ','
            << CsvEscape(e.compatibility.productLine) << ','
            << CsvEscape(e.compatibility.latency) << ','
            << CsvEscape(e.compatibility.constellation) << ','
            << CsvEscape(e.compatibility.referenceFrequency) << ','
            << CsvEscape(e.interval) << ','
            << CsvEscape(e.compatibility.orbitInterval) << ','
            << CsvEscape(e.compatibility.clockInterval) << ','
            << CsvEscape(e.compatibility.biasConvention) << ','
            << CsvEscape(e.compatibility.referenceFrame) << ','
            << CsvEscape(e.compatibility.apcModel) << '\n';
    }
    return out.str();
}

const char* PreciseBundlePlanName(const PreciseBundlePlan plan) {
    switch (plan) {
    case PreciseBundlePlan::WumPridePppAr:
        return "PRIDE PPP-AR";
    case PreciseBundlePlan::StandardPpp: return "Standard PPP";
    case PreciseBundlePlan::AutoStandardPpp: return "Automatic standard PPP";
    case PreciseBundlePlan::UpdPppAr: return "UPD PPP-AR";
    case PreciseBundlePlan::OsbPppAr: return "OSB PPP-AR (same agency)";
    case PreciseBundlePlan::NetworkRtkPppRtk:
        return "PPP-RTK / Network RTK common products";
    }
    return "Unknown";
}

const char* PreciseBundleProductTypeName(const PreciseBundleProductType type) {
    switch (type) {
    case PreciseBundleProductType::Sp3: return "SP3";
    case PreciseBundleProductType::Clk: return "CLK";
    case PreciseBundleProductType::Osb: return "OSB/BIA";
    case PreciseBundleProductType::Erp: return "ERP";
    case PreciseBundleProductType::Att: return "ORBEX";
    case PreciseBundleProductType::Upd: return "UPD";
    case PreciseBundleProductType::Antex: return "ANTEX";
    case PreciseBundleProductType::SinexCrd: return "IGS SINEX CRD";
    case PreciseBundleProductType::SinexSol: return "IGS SINEX SOL";
    case PreciseBundleProductType::Gim: return "CODE GIM Final";
    case PreciseBundleProductType::Vmf1H00: return "VMF1 00 UTC";
    case PreciseBundleProductType::Vmf1H06: return "VMF1 06 UTC";
    case PreciseBundleProductType::Vmf1H12: return "VMF1 12 UTC";
    case PreciseBundleProductType::Vmf1H18: return "VMF1 18 UTC";
    case PreciseBundleProductType::Vmf3H00: return "VMF3 00 UTC";
    case PreciseBundleProductType::Vmf3H06: return "VMF3 06 UTC";
    case PreciseBundleProductType::Vmf3H12: return "VMF3 12 UTC";
    case PreciseBundleProductType::Vmf3H18: return "VMF3 18 UTC";
    case PreciseBundleProductType::Brdc: return "IGS daily BRDC";
    }
    return "Unknown";
}
