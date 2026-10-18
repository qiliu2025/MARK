#include "PreciseProducts.h"

#include "DateUtil.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <format>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace {

struct DateParts {
    int gpsWeek = 0;
    int gpsDay = 0;
    int year = 0;
    int doy = 0;
    int yy = 0;
    std::wstring yearText;
    std::wstring doyText;
    std::wstring stamp;
};

DateParts MakeDateParts(const QueryDate& date) {
    using namespace std::chrono;
    const sys_days day = year{date.year} / month{static_cast<unsigned>(date.month)}
        / std::chrono::day{static_cast<unsigned>(date.day)};
    const sys_days gpsEpoch = year{1980} / January / 6;
    const auto gpsDays = duration_cast<days>(day - gpsEpoch).count();
    DateParts result;
    result.gpsWeek = static_cast<int>(gpsDays / 7);
    result.gpsDay = static_cast<int>(gpsDays % 7);
    result.year = date.year;
    result.doy = date.dayOfYear;
    result.yy = date.year % 100;
    result.yearText = std::format(L"{:04}", result.year);
    result.doyText = std::format(L"{:03}", result.doy);
    result.stamp = result.yearText + result.doyText + L"0000";
    return result;
}

std::wstring JoinUrl(
    const std::wstring& directory,
    const std::wstring& fileName) {
    return directory + (directory.ends_with(L'/') ? L"" : L"/") + fileName;
}

void AppendMirror(
    PreciseProductFile& product,
    const std::wstring& source,
    const std::wstring& directory,
    const std::wstring& fileName) {
    const auto duplicate = std::ranges::find_if(
        product.mirrors,
        [&](const PreciseProductMirror& mirror) {
            return mirror.url == JoinUrl(directory, fileName);
        });
    if (duplicate == product.mirrors.end()) {
        product.mirrors.push_back(
            {source, JoinUrl(directory, fileName), fileName});
    }
}

std::vector<PreciseProductFile> BuildBdsMartRtppProductsImpl(
    const QueryDate& date) {
    // The historical 2024/2025 directory contains several CAS0MGX* series
    // with date-dependent availability.  Those rows are supplied by the
    // BDSmart annual scanner; do not manufacture a CAS1 fallback for them.
    // The deterministic direct fallback is retained for the current CAS1
    // series (2026 onward), so users can still download it before scanning.
    if (date.dayOfYear <= 0 || date.year < 2026) {
        return {};
    }

    const DateParts parts = MakeDateParts(date);
    const std::wstring directory =
        L"https://data.bdsmart.cn/pub/product/rts/rtpp/"
        + parts.yearText + L"/";
    const std::wstring stamp = parts.stamp;
    const auto makeFileName = [&](const std::wstring& suffix) {
        return L"CAS1IGSRTS_" + stamp + L"_01D_" + suffix + L".gz";
    };

    std::vector<PreciseProductFile> result;
    result.reserve(3);
    const auto add = [&](const wchar_t* id,
                         const wchar_t* name,
                         const PreciseProductCategory category,
                         const wchar_t* interval,
                         const std::wstring& fileName) {
        PreciseProductFile product;
        product.id = id;
        product.name = name;
        product.category = category;
        product.latency = PreciseProductLatency::RealTime;
        product.center = L"CAS/BDSmart";
        product.constellation = L"GRECJ";
        product.interval = interval;
        product.status = L"BDSmart RT-PPP 直链；按日文件下载";
        AppendMirror(product, L"BDSmart", directory, fileName);
        result.push_back(std::move(product));
    };

    add(L"CAS1IGSRTS_RTPP_ORB",
        L"CAS/BDSmart RT-PPP 实时轨道（SP3）",
        PreciseProductCategory::Orbit, L"01M",
        makeFileName(L"01M_ORB.SP3"));
    add(L"CAS1IGSRTS_RTPP_CLK",
        L"CAS/BDSmart RT-PPP 实时钟差（CLK）",
        PreciseProductCategory::Clock, L"30S",
        makeFileName(L"30S_CLK.CLK"));
    add(L"CAS1IGSRTS_RTPP_OSB",
        L"CAS/BDSmart RT-PPP 实时 OSB（BIA）",
        PreciseProductCategory::ObservableSpecificBias, L"01D",
        makeFileName(L"01D_OSB.BIA"));
    return result;
}

class CatalogBuilder {
public:
    CatalogBuilder(const QueryDate& date, std::wstring troposphereStation)
        : queryDate_(date),
          date_(MakeDateParts(date)),
          troposphereStation_(std::move(troposphereStation)) {
        std::ranges::transform(
            troposphereStation_, troposphereStation_.begin(),
            [](const wchar_t character) {
                return static_cast<wchar_t>(std::towupper(character));
            });
        troposphereStation_.erase(
            std::remove_if(
                troposphereStation_.begin(),
                troposphereStation_.end(),
                [](const wchar_t character) {
                    return !std::iswalnum(character);
                }),
            troposphereStation_.end());
        if (troposphereStation_.size() < 4) {
            troposphereStation_ = L"ZIM2";
        }
    }

    PreciseProductFile& Add(
        std::wstring id,
        std::wstring name,
        const PreciseProductCategory category,
        const PreciseProductLatency latency,
        std::wstring center,
        std::wstring constellation,
        std::wstring interval) {
        products_.push_back({
            std::move(id),
            std::move(name),
            category,
            latency,
            std::move(center),
            std::move(constellation),
            std::move(interval)});
        return products_.back();
    }

    void AddProductArchiveMirrors(
        PreciseProductFile& product,
        const std::initializer_list<std::wstring_view> fileNames,
        const bool mgex = false) const {
        const std::wstring week = std::to_wstring(date_.gpsWeek);
        const std::array<std::pair<std::wstring_view, std::wstring>, 7> roots{{
            {L"BKG", L"https://igs.bkg.bund.de/root_ftp/IGS/products/"},
            {L"WHU", L"ftp://igs.gnsswhu.cn/pub/gps/products/"},
            {L"KASI", L"ftp://nfs.kasi.re.kr/gps/products/"},
            {L"IGN", L"ftp://igs.ign.fr/pub/igs/products/"},
            {L"ESA GSSC", L"ftp://gssc.esa.int/gnss/products/"},
            {L"SOPAC/GARNER", L"http://garner.ucsd.edu/pub/products/"},
            {L"NASA CDDIS", L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/products/"}
        }};
        for (const auto& file : fileNames) {
            if (file.empty()) {
                continue;
            }
            for (const auto& [source, root] : roots) {
                if (mgex) {
                    AppendMirror(product, std::wstring(source), root + L"mgex/" + week, std::wstring(file));
                }
                AppendMirror(product, std::wstring(source), root + week, std::wstring(file));
            }
        }
    }

    void AddLong(
        const wchar_t* id,
        const wchar_t* displayName,
        const PreciseProductCategory category,
        const PreciseProductLatency latency,
        const wchar_t* center,
        const wchar_t* constellation,
        const wchar_t* interval,
        const wchar_t* prefix,
        const wchar_t* duration,
        const wchar_t* content,
        const wchar_t* extension,
        const bool mgex = false,
        const std::wstring& legacy = {}) {
        auto& product = Add(
            id, displayName, category, latency, center, constellation, interval);
        const std::wstring longName = std::format(
            L"{}_{}_{}_{}_{}.{}.gz",
            prefix,
            date_.stamp,
            duration,
            interval,
            content,
            extension);
        if (legacy.empty()) {
            AddProductArchiveMirrors(product, {longName}, mgex);
        } else {
            AddProductArchiveMirrors(product, {longName, legacy}, mgex);
        }
    }

    void AddOrbitClockFamilies() {
        struct CenterDef {
            const wchar_t* id;
            const wchar_t* center;
            const wchar_t* prefix;
        };
        const std::array finalCenters{
            CenterDef{L"COD", L"CODE", L"COD0OPSFIN"},
            CenterDef{L"EMR", L"NRCan/EMR", L"EMR0OPSFIN"},
            CenterDef{L"ESA", L"ESA", L"ESA0OPSFIN"},
            CenterDef{L"GFZ", L"GFZ", L"GFZ0OPSFIN"},
            CenterDef{L"GRG", L"CNES/GRGS", L"GRG0OPSFIN"},
            CenterDef{L"IGS", L"IGS", L"IGS0OPSFIN"},
            CenterDef{L"JGX", L"JGX", L"JGX0OPSFIN"},
            CenterDef{L"JPL", L"JPL", L"JPL0OPSFIN"},
            CenterDef{L"MIT", L"MIT", L"MIT0OPSFIN"}
        };
        for (const auto& center : finalCenters) {
            const std::wstring legacySp3 = std::format(
                L"{}{}{}.sp3.Z",
                std::wstring(center.id).substr(0, 3), date_.gpsWeek, date_.gpsDay);
            const std::wstring legacyClk = std::format(
                L"{}{}{}.clk.Z",
                std::wstring(center.id).substr(0, 3), date_.gpsWeek, date_.gpsDay);
            AddLong(
                (std::wstring(L"GPS_") + center.id + L"_F_sp3").c_str(),
                (std::wstring(center.center) + L" 最终轨道").c_str(),
                PreciseProductCategory::Orbit, PreciseProductLatency::Final,
                center.center, L"G", L"15M", center.prefix,
                L"01D", L"ORB", L"SP3", false, legacySp3);
            AddLong(
                (std::wstring(L"GPS_") + center.id + L"_F_clk").c_str(),
                (std::wstring(center.center) + L" 最终钟差").c_str(),
                PreciseProductCategory::Clock, PreciseProductLatency::Final,
                center.center, L"G", L"30S", center.prefix,
                L"01D", L"CLK", L"CLK", false, legacyClk);
        }

        const std::array rapidCenters{
            CenterDef{L"COD", L"CODE", L"COD0OPSRAP"},
            CenterDef{L"EMR", L"NRCan/EMR", L"EMR0OPSRAP"},
            CenterDef{L"ESA", L"ESA", L"ESA0OPSRAP"},
            CenterDef{L"GFZ", L"GFZ", L"GFZ0OPSRAP"},
            CenterDef{L"IGS", L"IGS", L"IGS0OPSRAP"}
        };
        for (const auto& center : rapidCenters) {
            const std::wstring shortRoot = std::wstring(center.id).substr(0, 3);
            AddLong(
                (std::wstring(L"GPS_") + center.id + L"_R_sp3").c_str(),
                (std::wstring(center.center) + L" 快速轨道").c_str(),
                PreciseProductCategory::Orbit, PreciseProductLatency::Rapid,
                center.center, L"G", L"15M", center.prefix,
                L"01D", L"ORB", L"SP3", false,
                std::format(L"{}{}{}.sp3.Z", shortRoot, date_.gpsWeek, date_.gpsDay));
            AddLong(
                (std::wstring(L"GPS_") + center.id + L"_R_clk").c_str(),
                (std::wstring(center.center) + L" 快速钟差").c_str(),
                PreciseProductCategory::Clock, PreciseProductLatency::Rapid,
                center.center, L"G", L"05M", center.prefix,
                L"01D", L"CLK", L"CLK", false,
                std::format(L"{}{}{}.clk.Z", shortRoot, date_.gpsWeek, date_.gpsDay));
        }

        const std::array mgexCenters{
            CenterDef{L"COD", L"CODE", L"COD0MGXFIN"},
            CenterDef{L"GFZ", L"GFZ", L"GFZ0MGXFIN"},
            CenterDef{L"GRG", L"CNES/GRGS", L"GRG0MGXFIN"},
            CenterDef{L"IAC", L"IAC", L"IAC0MGXFIN"},
            CenterDef{L"JAX", L"JAXA", L"JAX0MGXFIN"},
            CenterDef{L"SHA", L"SHAO", L"SHA0MGXFIN"},
            CenterDef{L"WHU", L"WHU", L"WHU0MGXFIN"},
            CenterDef{L"WMC", L"WUM/WMC", L"WUM0MGXFIN"}
        };
        for (const auto& center : mgexCenters) {
            AddLong(
                (std::wstring(L"GCRE_") + center.id + L"_F_sp3").c_str(),
                (std::wstring(center.center) + L" 多系统最终轨道").c_str(),
                PreciseProductCategory::Orbit, PreciseProductLatency::Final,
                center.center, L"G/R/E/C/J/S", L"05M", center.prefix,
                L"01D", L"ORB", L"SP3", true);
            AddLong(
                (std::wstring(L"GCRE_") + center.id + L"_F_clk").c_str(),
                (std::wstring(center.center) + L" 多系统最终钟差").c_str(),
                PreciseProductCategory::Clock, PreciseProductLatency::Final,
                center.center, L"G/R/E/C/J/S", L"30S", center.prefix,
                L"01D", L"CLK", L"CLK", true);
        }

        struct OperationalDef {
            const wchar_t* id;
            const wchar_t* center;
            const wchar_t* prefix;
            PreciseProductLatency latency;
            const wchar_t* label;
        };
        const std::array operational{
            OperationalDef{L"WHU_R", L"WHU", L"WHU0MGXRAP", PreciseProductLatency::Rapid, L"快速"},
            OperationalDef{L"GFZ_R", L"GFZ", L"GFZ0MGXRAP", PreciseProductLatency::Rapid, L"快速"},
            OperationalDef{L"COD_R", L"CODE", L"COD0OPSRAP", PreciseProductLatency::Rapid, L"快速"},
            OperationalDef{L"WHU_U", L"WHU", L"WHU0MGXULT", PreciseProductLatency::UltraRapid, L"超快速"},
            OperationalDef{L"JAX_U", L"JAXA", L"JAX0MGXULT", PreciseProductLatency::UltraRapid, L"超快速"},
            OperationalDef{L"GRG_U", L"CNES/GRGS", L"GRG0MGXULT", PreciseProductLatency::UltraRapid, L"超快速"},
            OperationalDef{L"WHU_RTS", L"WHU", L"WHU0MGXRTS", PreciseProductLatency::RealTime, L"实时流离线"},
            OperationalDef{L"GRG_RTS", L"CNES/GRGS", L"GRG0MGXRTS", PreciseProductLatency::RealTime, L"实时流离线"},
            OperationalDef{L"WHU_H", L"WHU", L"WHU0MGXFIN", PreciseProductLatency::Final, L"高率"}
        };
        for (const auto& item : operational) {
            AddLong(
                (std::wstring(L"GCRE_") + item.id + L"_sp3").c_str(),
                (std::wstring(item.center) + L" 多系统" + item.label + L"轨道").c_str(),
                PreciseProductCategory::Orbit, item.latency,
                item.center, L"G/R/E/C/J", L"05M", item.prefix,
                L"01D", L"ORB", L"SP3", true);
            AddLong(
                (std::wstring(L"GCRE_") + item.id + L"_clk").c_str(),
                (std::wstring(item.center) + L" 多系统" + item.label + L"钟差").c_str(),
                PreciseProductCategory::Clock, item.latency,
                item.center, L"G/R/E/C/J", L"30S", item.prefix,
                L"01D", L"CLK", L"CLK", true);
        }

        for (const int hour : {0, 6, 12, 18}) {
            auto& orbit = Add(
                std::format(L"GPS_IGU_{:02}_sp3", hour),
                std::format(L"IGS 超快速轨道（{:02} UTC批次）", hour),
                PreciseProductCategory::Orbit,
                PreciseProductLatency::UltraRapid,
                L"IGS", L"G", L"15M");
            AddProductArchiveMirrors(
                orbit,
                {std::format(L"igu{}{}_{:02}.sp3.Z", date_.gpsWeek, date_.gpsDay, hour)});
            auto& clock = Add(
                std::format(L"GPS_IGU_{:02}_clk", hour),
                std::format(L"IGS 超快速钟差（{:02} UTC批次）", hour),
                PreciseProductCategory::Clock,
                PreciseProductLatency::UltraRapid,
                L"IGS", L"G", L"05M");
            AddProductArchiveMirrors(
                clock,
                {std::format(L"igu{}{}_{:02}.clk.Z", date_.gpsWeek, date_.gpsDay, hour)});
        }
        auto& iglOrbit = Add(
            L"GLO_IGL_F_sp3", L"IGS GLONASS 最终轨道",
            PreciseProductCategory::Orbit, PreciseProductLatency::Final,
            L"IGS/IGL", L"R", L"15M");
        AddProductArchiveMirrors(
            iglOrbit,
            {std::format(L"igl{}{}.sp3.Z", date_.gpsWeek, date_.gpsDay)});
        auto& iglClock = Add(
            L"GLO_IGL_F_clk", L"IGS GLONASS 最终钟差",
            PreciseProductCategory::Clock, PreciseProductLatency::Final,
            L"IGS/IGL", L"R", L"05M");
        AddProductArchiveMirrors(
            iglClock,
            {std::format(L"igl{}{}.clk.Z", date_.gpsWeek, date_.gpsDay)});
    }

    void AddEarthRotation() {
        struct Def {
            const wchar_t* id;
            const wchar_t* center;
            const wchar_t* prefix;
            PreciseProductLatency latency;
        };
        const std::array defs{
            Def{L"IGS_F_erp", L"IGS", L"IGS0OPSFIN", PreciseProductLatency::Final},
            Def{L"IGS_R_erp", L"IGS", L"IGS0OPSRAP", PreciseProductLatency::Rapid},
            Def{L"WHU_F_erp", L"WHU", L"WHU0MGXFIN", PreciseProductLatency::Final},
            Def{L"WHU_U_erp", L"WHU", L"WHU0MGXULT", PreciseProductLatency::UltraRapid},
            Def{L"WHU_H_erp", L"WHU", L"WHU0MGXFIN", PreciseProductLatency::Final},
            Def{L"COD_F_erp", L"CODE", L"COD0OPSFIN", PreciseProductLatency::Final},
            Def{L"COD_R_erp", L"CODE", L"COD0OPSRAP", PreciseProductLatency::Rapid},
            Def{L"GFZ_R_erp", L"GFZ", L"GFZ0OPSRAP", PreciseProductLatency::Rapid},
            Def{L"GRG_F_erp", L"CNES/GRGS", L"GRG0OPSFIN", PreciseProductLatency::Final},
            Def{L"ESA_U_erp", L"ESA", L"ESA0OPSULT", PreciseProductLatency::UltraRapid},
            Def{L"GFZ_U_erp", L"GFZ", L"GFZ0OPSULT", PreciseProductLatency::UltraRapid},
            Def{L"IGS_U_erp", L"IGS", L"IGS0OPSULT", PreciseProductLatency::UltraRapid}
        };
        for (const auto& def : defs) {
            AddLong(
                def.id,
                (std::wstring(def.center) + L" 地球自转参数").c_str(),
                PreciseProductCategory::EarthRotation, def.latency,
                def.center, L"Earth", L"01D", def.prefix,
                L"07D", L"ERP", L"ERP", true);
        }
        auto& c04 = Add(
            L"EOP_C04", L"IERS EOP C04", PreciseProductCategory::EarthRotation,
            PreciseProductLatency::Final, L"IERS", L"Earth", L"01D");
        AppendMirror(c04, L"IERS", L"https://datacenter.iers.org/data/latestVersion", L"EOP_20_C04_one_file_1962-now.txt");
        auto& bulletin = Add(
            L"SW_EOP", L"IERS Bulletin A / finals", PreciseProductCategory::EarthRotation,
            PreciseProductLatency::Rapid, L"IERS", L"Earth", L"01D");
        AppendMirror(bulletin, L"IERS", L"https://datacenter.iers.org/data/latestVersion", L"finals2000A.all");
    }

    void AddBiasAndAttitude() {
        struct BiasDef {
            const wchar_t* id;
            const wchar_t* center;
            const wchar_t* prefix;
            const wchar_t* kind;
            PreciseProductLatency latency;
            PreciseProductCategory category;
        };
        const std::array biases{
            BiasDef{L"GPS_COD_F_osb", L"CODE", L"COD0OPSFIN", L"OSB", PreciseProductLatency::Final, PreciseProductCategory::ObservableSpecificBias},
            BiasDef{L"GCRE_COD_F_osb", L"CODE", L"COD0MGXFIN", L"OSB", PreciseProductLatency::Final, PreciseProductCategory::ObservableSpecificBias},
            BiasDef{L"GRE_COD_R_osb", L"CODE", L"COD0OPSRAP", L"OSB", PreciseProductLatency::Rapid, PreciseProductCategory::ObservableSpecificBias},
            BiasDef{L"GCRE_CAS_R_osb", L"CAS", L"CAS0MGXRAP", L"OSB", PreciseProductLatency::Rapid, PreciseProductCategory::ObservableSpecificBias},
            BiasDef{L"GCRE_GFZ_R_osb", L"GFZ", L"GFZ0MGXRAP", L"OSB", PreciseProductLatency::Rapid, PreciseProductCategory::ObservableSpecificBias},
            BiasDef{L"GCE_GRG_F_osb", L"CNES/GRGS", L"GRG0MGXFIN", L"OSB", PreciseProductLatency::Final, PreciseProductCategory::ObservableSpecificBias},
            BiasDef{L"GCRE_WHU_F_osb", L"WHU", L"WHU0MGXFIN", L"OSB", PreciseProductLatency::Final, PreciseProductCategory::ObservableSpecificBias},
            BiasDef{L"GCRE_WHU_R_osb", L"WHU", L"WHU0MGXRAP", L"OSB", PreciseProductLatency::Rapid, PreciseProductCategory::ObservableSpecificBias},
            BiasDef{L"GCRE_WHU_U_osb", L"WHU", L"WHU0MGXULT", L"OSB", PreciseProductLatency::UltraRapid, PreciseProductCategory::ObservableSpecificBias},
            BiasDef{L"GCRE_WHU_RTS_osb", L"WHU", L"WHU0MGXRTS", L"OSB", PreciseProductLatency::RealTime, PreciseProductCategory::ObservableSpecificBias},
            BiasDef{L"GCRE_WHU_R_abs", L"WHU", L"WHU0MGXRAP", L"ABS", PreciseProductLatency::Rapid, PreciseProductCategory::ObservableSpecificBias},
            BiasDef{L"GPS_COD_dcb", L"CODE", L"COD0OPSFIN", L"DSB", PreciseProductLatency::Final, PreciseProductCategory::DifferentialCodeBias},
            BiasDef{L"GCRE_CAS_R_dcb", L"CAS", L"CAS0MGXRAP", L"DSB", PreciseProductLatency::Rapid, PreciseProductCategory::DifferentialCodeBias},
            BiasDef{L"CNES_bia", L"CNES", L"GRG0MGXRTS", L"BIA", PreciseProductLatency::RealTime, PreciseProductCategory::ObservableSpecificBias}
        };
        for (const auto& def : biases) {
            AddLong(
                def.id,
                (std::wstring(def.center) + L" " + def.kind + L"偏差").c_str(),
                def.category, def.latency,
                def.center, L"G/R/E/C/J", L"01D", def.prefix,
                L"01D", def.kind, L"BIA", true);
        }

        struct AttDef {
            const wchar_t* id;
            const wchar_t* center;
            const wchar_t* prefix;
            PreciseProductLatency latency;
        };
        const std::array attitudes{
            AttDef{L"GPS_COD_F_obx", L"CODE", L"COD0OPSFIN", PreciseProductLatency::Final},
            AttDef{L"GPS_GRG_F_obx", L"CNES/GRGS", L"GRG0OPSFIN", PreciseProductLatency::Final},
            AttDef{L"GCRE_COD_F_obx", L"CODE", L"COD0MGXFIN", PreciseProductLatency::Final},
            AttDef{L"GCRE_GFZ_R_obx", L"GFZ", L"GFZ0MGXRAP", PreciseProductLatency::Rapid},
            AttDef{L"GCRE_WHU_F_obx", L"WHU", L"WHU0MGXFIN", PreciseProductLatency::Final},
            AttDef{L"GCRE_WHU_R_obx", L"WHU", L"WHU0MGXRAP", PreciseProductLatency::Rapid},
            AttDef{L"GCRE_WHU_U_obx", L"WHU", L"WHU0MGXULT", PreciseProductLatency::UltraRapid},
            AttDef{L"CNES_RTS_obx", L"CNES", L"GRG0MGXRTS", PreciseProductLatency::RealTime}
        };
        for (const auto& def : attitudes) {
            AddLong(
                def.id,
                (std::wstring(def.center) + L" 卫星姿态 ORBEX").c_str(),
                PreciseProductCategory::Attitude, def.latency,
                def.center, L"G/R/E/C/J", L"30S", def.prefix,
                L"01D", L"ATT", L"OBX", true);
        }
    }

    void AddIonosphere() {
        struct IonDef {
            const wchar_t* id;
            const wchar_t* center;
            const wchar_t* prefix;
            const wchar_t* legacy;
            PreciseProductLatency latency;
        };
        const std::array ions{
            IonDef{L"IGSG_ion", L"IGS", L"IGS0OPSFIN", L"igsg", PreciseProductLatency::Final},
            IonDef{L"IGRG_ion", L"IGS", L"IGS0OPSRAP", L"igrg", PreciseProductLatency::Rapid},
            IonDef{L"WHUG_ion", L"WHU", L"WHU0OPSFIN", L"whug", PreciseProductLatency::Final},
            // WHU publishes the rapid legacy IONEX series as whrg (not wurg).
            // Keep the product id aligned with the public filename so the
            // precision catalog can reach WHU's dedicated ionosphere tree.
            IonDef{L"WHRG_ion", L"WHU", L"WHU0OPSRAP", L"whrg", PreciseProductLatency::Rapid},
            IonDef{L"CODG_ion", L"CODE", L"COD0OPSFIN", L"codg", PreciseProductLatency::Final},
            IonDef{L"CORG_ion", L"CODE", L"COD0OPSRAP", L"corg", PreciseProductLatency::Rapid},
            IonDef{L"UQRG_ion", L"UPC", L"UPC0OPSRAP", L"uqrg", PreciseProductLatency::Rapid},
            IonDef{L"UPRG_ion", L"UPC", L"UPC0OPSFIN", L"uprg", PreciseProductLatency::Final},
            IonDef{L"JPLG_ion", L"JPL", L"JPL0OPSFIN", L"jplg", PreciseProductLatency::Final},
            IonDef{L"JPRG_ion", L"JPL", L"JPL0OPSRAP", L"jprg", PreciseProductLatency::Rapid},
            IonDef{L"CASG_ion", L"CAS", L"CAS0OPSFIN", L"casg", PreciseProductLatency::Final},
            IonDef{L"CARG_ion", L"CAS", L"CAS0OPSRAP", L"carg", PreciseProductLatency::Rapid},
            IonDef{L"ESAG_ion", L"ESA", L"ESA0OPSFIN", L"esag", PreciseProductLatency::Final},
            IonDef{L"ESRG_ion", L"ESA", L"ESA0OPSRAP", L"esrg", PreciseProductLatency::Rapid},
            IonDef{L"EMRG_ion", L"NRCan/EMR", L"EMR0OPSFIN", L"emrg", PreciseProductLatency::Final}
        };
        for (const auto& def : ions) {
            auto& product = Add(
                def.id,
                std::wstring(def.center) + L" 全球电离层图 IONEX",
                PreciseProductCategory::Ionosphere,
                def.latency,
                def.center,
                L"GIM",
                L"01H");
            const std::wstring longName = std::format(
                L"{}_{}_01D_01H_GIM.INX.gz", def.prefix, date_.stamp);
            const std::wstring legacyName = std::format(
                L"{}{}0.{:02}i.Z", def.legacy, date_.doyText, date_.yy);
            AddIonexMirrors(product, {longName, legacyName});
        }
        auto& roti = Add(
            L"IGS_ROTI", L"IGS 全球 ROTI 图",
            PreciseProductCategory::Ionosphere,
            PreciseProductLatency::Daily,
            L"IGS", L"ROTI", L"01H");
        AddIonexMirrors(
            roti,
            {std::format(L"roti{}0.{:02}f.gz", date_.doyText, date_.yy),
             std::format(L"roti{}0.{:02}f.Z", date_.doyText, date_.yy)});
    }

    void AddIonexMirrors(
        PreciseProductFile& product,
        const std::initializer_list<std::wstring_view> files) const {
        const std::array<std::pair<std::wstring_view, std::wstring>, 4> roots{{
            {L"BKG", L"https://igs.bkg.bund.de/root_ftp/IGS/IONEX/"},
            {L"WHU", L"ftp://igs.gnsswhu.cn/pub/gps/products/ionex/"},
            {L"IGN", L"ftp://igs.ign.fr/pub/igs/products/ionosphere/"},
            {L"NASA CDDIS", L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/products/ionex/"}
        }};
        for (const auto& file : files) {
            for (const auto& [source, root] : roots) {
                AppendMirror(
                    product,
                    std::wstring(source),
                    root + date_.yearText + L"/" + date_.doyText,
                    std::wstring(file));
            }
            // WHU's current daily GIM products are published in its own
            // MGEX/ionosphere tree.  The generic /pub/gps/products/ionex/
            // mirror contains many IAAC products, but it is not a reliable
            // route for WHU's whug/whrg legacy files.
            if (product.center == L"WHU"
                && file == std::format(
                    L"{}{}0.{:02}i.Z",
                    product.id == L"WHRG_ion" ? L"whrg" : L"whug",
                    date_.doyText,
                    date_.yy)) {
                AppendMirror(
                    product,
                    L"WHU MGEX ionosphere",
                    L"ftp://igs.gnsswhu.cn/pub/whu/MGEX/ionosphere/"
                        + date_.yearText,
                    std::wstring(file));
            }
        }
    }

    void AddTroposphereSinexAntenna() {
        const std::wstring station4 = troposphereStation_.substr(0, 4);
        std::wstring station4Lower = station4;
        std::ranges::transform(
            station4Lower, station4Lower.begin(),
            [](const wchar_t character) {
                return static_cast<wchar_t>(std::towlower(character));
            });
        auto& igsTro = Add(
            L"IGS_trop", L"IGS 测站对流层天顶延迟（" + troposphereStation_ + L"）", PreciseProductCategory::Troposphere,
            PreciseProductLatency::Final, L"IGS", L"TRO", L"05M");
        const std::wstring zpdShort = std::format(
            L"{}{}0.{:02}zpd.gz", station4Lower, date_.doyText, date_.yy);
        AppendMirror(
            igsTro, L"WHU",
            L"ftp://igs.gnsswhu.cn/pub/gps/products/troposphere/new/"
                + date_.yearText + L"/" + date_.doyText,
            zpdShort);
        AppendMirror(
            igsTro, L"NASA CDDIS",
            L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/products/troposphere/zpd/"
                + date_.yearText + L"/" + date_.doyText,
            zpdShort);
        if (troposphereStation_.size() >= 9) {
            const std::wstring zpdLong = std::format(
                L"IGS0OPSFIN_{}_01D_05M_{}_TRO.TRO.gz",
                date_.stamp,
                troposphereStation_.substr(0, 9));
            AppendMirror(
                igsTro, L"WHU",
                L"ftp://igs.gnsswhu.cn/pub/gps/products/troposphere/new/"
                    + date_.yearText + L"/" + date_.doyText,
                zpdLong);
        }
        auto& codTro = Add(
            L"COD_trop", L"CODE 对流层产品", PreciseProductCategory::Troposphere,
            PreciseProductLatency::Final, L"CODE", L"TRO", L"01H");
        AddProductArchiveMirrors(
            codTro,
            {std::format(L"COD0OPSTRO_{}_01D_01H_TRO.TRO.gz", date_.stamp)});
        auto& jplTro = Add(
            L"JPL_trop", L"JPL 对流层产品", PreciseProductCategory::Troposphere,
            PreciseProductLatency::Final, L"JPL", L"TRO", L"05M");
        AddProductArchiveMirrors(
            jplTro,
            {std::format(L"JPL0OPSTRO_{}_01D_05M_TRO.TRO.gz", date_.stamp)});

        struct SnxDef {
            const wchar_t* id;
            const wchar_t* center;
            const wchar_t* prefix;
            PreciseProductLatency latency;
        };
        const std::array sinexDefs{
            SnxDef{L"IGS_day_snx", L"IGS daily", L"IGS0OPSSNX", PreciseProductLatency::Daily},
            SnxDef{L"IGS_week_snx", L"IGS weekly", L"IGS0OPSFIN", PreciseProductLatency::Weekly},
            SnxDef{L"IVS_week_snx", L"IVS", L"IVS0OPSFIN", PreciseProductLatency::Weekly},
            SnxDef{L"ILS_week_snx", L"ILRS", L"ILS0OPSFIN", PreciseProductLatency::Weekly},
            SnxDef{L"IDS_week_snx", L"IDS", L"IDS0OPSFIN", PreciseProductLatency::Weekly},
            SnxDef{L"IGS_crd_snx", L"IGS coordinates", L"IGS0OPSSNX", PreciseProductLatency::Final},
            SnxDef{L"COD_sol_snx", L"CODE", L"COD0OPSFIN", PreciseProductLatency::Final},
            SnxDef{L"ESA_sol_snx", L"ESA", L"ESA0OPSFIN", PreciseProductLatency::Final},
            SnxDef{L"GFZ_sol_snx", L"GFZ", L"GFZ0OPSFIN", PreciseProductLatency::Final},
            SnxDef{L"GRG_sol_snx", L"CNES/GRGS", L"GRG0OPSFIN", PreciseProductLatency::Final},
            SnxDef{L"NGS_sol_snx", L"NGS", L"NGS0OPSFIN", PreciseProductLatency::Final},
            SnxDef{L"SIO_sol_snx", L"SIO", L"SIO0OPSFIN", PreciseProductLatency::Final}
        };
        for (const auto& def : sinexDefs) {
            AddLong(
                def.id,
                (std::wstring(def.center) + L" SINEX").c_str(),
                PreciseProductCategory::Sinex, def.latency,
                def.center, L"SNX", L"01D", def.prefix,
                def.latency == PreciseProductLatency::Weekly ? L"07D" : L"01D",
                L"SOL", L"SNX", false);
        }

        auto& igs14 = Add(
            L"MGEX_IGS14_atx", L"IGS14/MGEX 天线相位中心模型",
            PreciseProductCategory::Antenna, PreciseProductLatency::Static,
            L"IGS", L"ANTEX", L"静态");
        AppendMirror(igs14, L"IGS Files", L"https://files.igs.org/pub/station/general", L"igs14.atx");
        AppendMirror(igs14, L"BKG", L"https://igs.bkg.bund.de/root_ftp/IGS/ATX", L"igs14.atx");
        auto& igs20 = Add(
            L"MGEX_IGS20_atx", L"IGS20/MGEX 天线相位中心模型",
            PreciseProductCategory::Antenna, PreciseProductLatency::Static,
            L"IGS", L"ANTEX", L"静态");
        AppendMirror(igs20, L"IGS Files", L"https://files.igs.org/pub/station/general", L"igs20.atx");
        AppendMirror(igs20, L"BKG", L"https://igs.bkg.bund.de/root_ftp/IGS/ATX", L"igs20.atx");
    }

    std::vector<PreciseProductFile> Build() {
        AddOrbitClockFamilies();
        const auto bdsMartProducts = BuildBdsMartRtppProducts(queryDate_);
        for (auto product : bdsMartProducts) {
            products_.push_back(std::move(product));
        }
        AddEarthRotation();
        AddBiasAndAttitude();
        AddIonosphere();
        AddTroposphereSinexAntenna();
        std::ranges::sort(products_, {}, &PreciseProductFile::id);
        return std::move(products_);
    }

private:
    QueryDate queryDate_;
    DateParts date_;
    std::wstring troposphereStation_;
    std::vector<PreciseProductFile> products_;
};

}  // namespace

std::vector<PreciseProductFile> BuildPreciseProductCatalog(
    const QueryDate& date,
    const std::wstring& troposphereStation) {
    if (date.dayOfYear <= 0) {
        return {};
    }
    return CatalogBuilder(date, troposphereStation).Build();
}

std::vector<PreciseProductFile> BuildBdsMartRtppProducts(
    const QueryDate& date) {
    return BuildBdsMartRtppProductsImpl(date);
}

const wchar_t* PreciseProductCategoryName(
    const PreciseProductCategory category) {
    switch (category) {
    case PreciseProductCategory::Orbit: return L"SP3 轨道";
    case PreciseProductCategory::Clock: return L"CLK 钟差";
    case PreciseProductCategory::EarthRotation: return L"ERP / EOP";
    case PreciseProductCategory::DifferentialCodeBias: return L"DCB 差分码偏差";
    case PreciseProductCategory::Attitude: return L"ORBEX 姿态";
    case PreciseProductCategory::Ionosphere: return L"IONEX 电离层";
    case PreciseProductCategory::Troposphere: return L"对流层";
    case PreciseProductCategory::Sinex: return L"SINEX";
    case PreciseProductCategory::Antenna: return L"ANTEX";
    case PreciseProductCategory::BroadcastNavigation: return L"广播星历";
    case PreciseProductCategory::ObservableSpecificBias: return L"OSB 观测量偏差";
    case PreciseProductCategory::UncalibratedPhaseDelay: return L"UPD 未校准相位延迟";
    case PreciseProductCategory::VmfGrid: return L"VMF 对流层格网";
    }
    return L"其他";
}

const wchar_t* PreciseProductLatencyName(
    const PreciseProductLatency latency) {
    switch (latency) {
    case PreciseProductLatency::Final: return L"最终";
    case PreciseProductLatency::Rapid: return L"快速";
    case PreciseProductLatency::UltraRapid: return L"超快速";
    case PreciseProductLatency::NearRealTime: return L"近实时";
    case PreciseProductLatency::RealTime: return L"实时流归档（非实时服务）";
    case PreciseProductLatency::Predicted: return L"预报";
    case PreciseProductLatency::Daily: return L"每日";
    case PreciseProductLatency::Weekly: return L"每周";
    case PreciseProductLatency::Static: return L"静态";
    case PreciseProductLatency::Other: return L"其他";
    }
    return L"其他";
}
