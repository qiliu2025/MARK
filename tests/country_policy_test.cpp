#include "CountryFlags.h"
#include <iostream>
#include <stdexcept>
#include <string>

void Expect(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
}

int main() {
    try {
        for (const auto& code : CountryFlags::kFlagCodes) {
            Expect(CountryFlags::NormalizeArea(code.alpha2) == code.alpha3, "complete ISO alpha-2 table");
        }
        for (const auto input : {"GRL", "GL", "Greenland", "格陵兰", " grl\t"}) {
            Expect(CountryFlags::NormalizeArea(input) == "GRL", "preserve Greenland area");
            Expect(CountryFlags::Normalize(input) == "DNK", "Greenland joins Denmark scope");
        }
        for (const auto input : {"TWN", "TW", "ROC", "Taiwan", "台湾", "中国台湾",
                                 "HKG", "HK", "Hong Kong", "香港", "MAC", "MO", "Macau", "澳门"}) {
            Expect(CountryFlags::Normalize(input) == "CHN", "China includes Hong Kong/Macao/Taiwan");
            Expect(CountryFlags::IsKnownArea(CountryFlags::NormalizeArea(input)), "China area resolves");
        }
        Expect(CountryFlags::Normalize("FRO") == "DNK", "Faroe joins Denmark");
        Expect(CountryFlags::Normalize("GUM") == "USA", "Guam joins USA");
        Expect(CountryFlags::Normalize("GUF") == "FRA", "French Guiana joins France");
        Expect(CountryFlags::Normalize("XK") == "SRB", "Kosovo joins Serbia");
        Expect(CountryFlags::Normalize("KOS") == "SRB", "map Kosovo code matches station code");
        Expect(CountryFlags::Normalize("SOL") == "SOM", "Somaliland joins Somalia");
        Expect(CountryFlags::Normalize("CYN") == "CYP", "Northern Cyprus joins Cyprus");
        Expect(CountryFlags::Normalize("PSE") == "ISR", "Palestinian-area stations join Israel scope");
        for (const auto input : {"COK", "NIU", "JPN", "CAN", "AUS", "NZL"}) {
            Expect(CountryFlags::Normalize(input) == input, "independent countries stay independent");
        }
        for (const auto input : {"ESH", "SGS"}) {
            Expect(CountryFlags::IsKnownArea(input), "non-state areas remain filterable");
            Expect(CountryFlags::Index(input) == -1, "no independent national flag for unresolved areas");
        }
        Expect(CountryFlags::Index("ATA") >= 0, "Antarctica uses the IGS geographic flag");
        Expect(CountryPolicy::DisplayName("ATA") == "南极洲", "Antarctica name has no status suffix");
        for (const auto area : {"BES", "PYF", "ATF", "GRL", "GUM", "MTQ", "NCL", "VIR", "ATA"}) {
            Expect(CountryPolicy::OwnFlagArea(area), "chosen IGS regions use their own icon");
            Expect(CountryPolicy::SelectableRegionArea(area), "chosen IGS regions can be scoped individually");
        }
        Expect(CountryPolicy::OwnFlagArea("FLK") && !CountryPolicy::SelectableRegionArea("FLK"),
            "Malvinas uses its own flag but remains outside explicit country scopes");
        Expect(CountryFlags::Index("FLK") != CountryFlags::Index("ARG"), "Malvinas flag differs from Argentina");
        for (const auto area : {"REU", "GUF", "TWN"}) {
            Expect(!CountryPolicy::OwnFlagArea(area), "other regions use parent icon");
            Expect(!CountryPolicy::SelectableRegionArea(area), "other regions are not separate scope choices");
        }
        Expect(CountryFlags::Index("GRL") >= 0 && CountryFlags::Index("GRL") != CountryFlags::Index("DNK"), "Greenland has its own flag");
        Expect(CountryFlags::Index("GUM") >= 0 && CountryFlags::Index("GUM") != CountryFlags::Index("USA"), "Guam has its own flag");
        Expect(!CountryFlags::IsKnownArea("UNK"), "unknown provider code is not a country");
        Expect(CountryFlags::ResolveArea("GRL", "", "YMER") == "GRL", "Greenland station retains area");
        Expect(CountryFlags::ResolveArea("GRL", "Denmark", "YMER") == "GRL", "country-only site log does not erase Greenland area");
        Expect(CountryFlags::ResolveArea("CHN", "China", "ABCD00TWN") == "TWN", "country-only site log retains specific station suffix");
        Expect(CountryFlags::ResolveArea("CHN", "Taiwan", "ABCD00TWN") == "TWN", "site log keeps Taiwan region within China");
        Expect(CountryFlags::ResolveArea("???", "bad value", "ABCD00GRL") == "GRL", "station suffix fallback retains original region");
        Expect(CountryPolicy::DisplayName("GRL") == "格陵兰", "native hover names Greenland alone");
        Expect(CountryPolicy::DisplayName("GUM") == "关岛", "native hover names Guam alone");
        Expect(CountryPolicy::DisplayName("FLK") == "马尔维纳斯群岛", "native hover omits parent country");
        Expect(CountryFlags::NormalizeArea("BF") == "BFA", "ISO BF means Burkina Faso");
        Expect(CountryFlags::ResolveArea("BF", "", "NGSTEST", true) == "BHS", "NGS FIPS BF means Bahamas");
        Expect(CountryFlags::ResolveArea("BF", "Burkina Faso", "NGSTEST", true) == "BFA", "valid Site Log country takes precedence over provider FIPS");
        Expect(CountryFlags::Normalize("MYT") == "COM", "Mayotte joins Comoros according to MFA profile");
        Expect(CountryFlags::Normalize("IOT") == "MUS", "Chagos joins Mauritius according to MFA profile");
        Expect(CountryFlags::ResolveStationArea("RUS", "Russia", "TEST00RUS", false, 44.95, 34.1) == "CRM", "Crimea coordinate overrides conflicting provider nationality");
        Expect(CountryFlags::Normalize("CRM") == "UKR", "Crimea joins Ukraine");
        Expect(CountryFlags::ResolveStationArea("ISR", "Israel", "TEST00ISR", false, 33.0, 35.8) == "GLN", "Golan coordinate overrides conflicting provider nationality");
        Expect(CountryFlags::Normalize("GLN") == "SYR", "Golan joins Syria");
        Expect(CountryFlags::ResolveStationArea("FRA", "France", "TEST00FRA", false, -12.8, 45.15) == "MYT", "Mayotte map and station country agree");
        Expect(CountryFlags::ResolveStationArea("GBR", "United Kingdom", "TEST00GBR", false, -7.3, 72.42) == "IOT", "Chagos map and station country agree");
        Expect(CountryFlags::ResolveStationArea("RUS", "Russia", "TEST00RUS", false, 55.75, 37.62) == "RUS", "ordinary Russia station unaffected by nearby territorial rule");
        Expect(CountryFlags::ResolveStationArea("ISR", "Israel", "TEST00ISR", false, 32.1, 34.8) == "ISR", "ordinary Israel station unaffected by nearby territorial rule");
        Expect(CountryGeoPolicy::AreaAt(0, 0).empty(), "no region inferred outside polygons");
        Expect(CountryGeoPolicy::AreaAt(NAN, 0).empty(), "invalid coordinates never infer territorial affiliation");
        Expect(CountryFlags::ResolveStationArea("ISR", "Israel", "DRAG", false, 31.5932, 35.392069) == "PSE", "DRAG belongs geographically to Palestine despite Israeli agency metadata");
        Expect(CountryFlags::ResolveStationArea("ISR", "Israel", "YOSH", false, 32.10383, 35.209666) == "PSE", "YOSH in West Bank belongs to Palestine");
        Expect(CountryFlags::ResolveStationArea("ISR", "Israel", "JSLM", false, 31.771167, 35.202444) == "ISR", "west Jerusalem JSLM is outside OCHA Palestine outline");
        Expect(CountryFlags::ResolveStationArea("ISR", "Israel", "TEST", false, 31.78, 35.235) == "PSE", "East Jerusalem follows Palestine geographic reference");
        Expect(CountryFlags::ResolveStationArea("IND", "India", "TEST", false, 34.08, 74.83) == "KMR", "India-controlled Kashmir is not an undisputed India country assignment");
        Expect(CountryFlags::ResolveStationArea("PAK", "Pakistan", "TEST", false, 34.37, 73.48) == "KMR", "Pakistan-controlled Kashmir also retains unresolved status");
        Expect(CountryFlags::Index("KMR") == -1 && CountryFlags::Index("KAS") == -1, "Kashmir and Siachen have no national flag inferred from control");
        Expect(CountryPolicy::DisplayName("KMR") == "克什米尔", "station area names have no status suffix");
        Expect(CountryFlags::Normalize(CountryFlags::ResolveStationArea("IND", "India", "TEST", false, 28, 94.8)) == "CHN", "Southern Tibet geographic affiliation is China");
        Expect(CountryFlags::Normalize(CountryFlags::ResolveStationArea("IND", "India", "TEST", false, 35.2, 79.2)) == "CHN", "Aksai Chin geographic affiliation is China");
        Expect(CountryFlags::Normalize(CountryFlags::ResolveStationArea("PAK", "Pakistan", "TEST", false, 36, 76.7)) == "CHN", "China territory is not absorbed by neutral Kashmir overlay");
        for (const auto& island : CountryGeoPolicy::kIslandReferences) {
            const auto area=CountryFlags::ResolveStationArea("JPN", "Japan", "TEST", false, island.latitude,island.longitude);
            Expect(area == "DYA" && CountryFlags::Normalize(area)=="CHN", "all eight official Diaoyu island locations belong to China");
        }
        Expect(CountryFlags::ResolveStationArea("JPN", "Japan", "TEST", false, 26.21, 127.68) == "JPN", "nearby Okinawa is unaffected by Diaoyu island locations");
        Expect(CountryFlags::ResolveStationArea("IND", "India", "TEST", false, 28.61, 77.21) == "IND", "New Delhi is unaffected by Kashmir/China overrides");
        Expect(CountryFlags::ResolveStationArea("PAK", "Pakistan", "TEST", false, 33.6844, 73.0479) == "PAK", "Islamabad is unaffected by Kashmir overlay");
        std::cout << "PASS: country/area normalization, sovereignty scopes and flag semantics\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
