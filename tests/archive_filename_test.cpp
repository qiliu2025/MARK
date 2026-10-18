#include "BroadcastAvailability.h"
#include "../src/IgsData.cpp" // Exercise the actual internal observation parsers.
#include "ClkAvailability.h"
#include "AttAvailability.h"
#include "Sp3Availability.h"
#include "DateUtil.h"
#include "ErpAvailability.h"
#include "OsbAvailability.h"
#include "SinexAvailability.h"
#include "IonexAvailability.h"
#include "TroAvailability.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

static void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        const auto date = MakeQueryDate(2025, 1, 1);
        const auto historicWum = MakeQueryDate(2022, 1, 1);
        const auto historicWeek = CalculateGpsWeek(historicWum);
        const auto ulaOrbit = ParseSp3DirectoryListing(
            "WUM0MGXULA_20220010900_01D_15M_ORB.SP3.gz", historicWeek,
            L"https://example.invalid/", "ops");
        const auto ulaClock = ParseClkDirectoryListing(
            "WUM0MGXULA_20220010900_01D_30S_CLK.CLK.gz", historicWeek,
            L"https://example.invalid/", "ops");
        Check(ulaOrbit.size()==1 && ulaOrbit[0].latency=="ULTRA_RAPID",
              "Historical WUM ULA orbit must be classified as ultra rapid");
        Check(ulaClock.size()==1 && ulaClock[0].latency=="ULTRA_RAPID",
              "Historical WUM ULA clock must be classified as ultra rapid");
        const auto html = [](const std::string& name) { return "<a href=\""+name+"\">file</a>"; };
        for (const auto& name : {"abmf0010.25o", "abmf0010.25o.Z", "abmf0010.25d.Z",
            "abmf0010.25d.gz", "abmf0010.25d.bz2", "ABMF00GLP_R_20250010000_01D_30S_MO.crx",
            "ABMF00GLP_R_20250010000_01D_30S_MO.rnx.gz", "ABMF00GLP_R_20250010000_01D_30S_MO.crx.Z",
            "ABMF00GLP_R_20250010000_01D_30S_MO.rnx.bz2"}) {
            const auto files = ParseDailyObservationFiles(html(name), date);
            Check(files.size()==1 && files[0].fileName==name, "An observation format was omitted");
        }
        for (const auto& name : {"abmf0010.25n.Z", "abmf0010.25p.Z", "abmf0010.24d.Z",
            "ABMF00GLP_R_20250010000_01D_30S_MO.rnx.gz.md5",
            "ABMF00GLP_R_20250010000_01D_30S_MN.rnx.gz"})
            Check(ParseDailyObservationFiles(html(name),date).empty(), "A non-observation file was admitted");
        int count = 0;
        for (const auto& name : {"abmf001A00.25d.Z", "abmf001A.25o.gz",
            "ABMF00GLP_R_20250010000_15M_01S_MO.crx.gz",
            "ABMF00GLP_R_20250010000_01D_01S_MO.crx.tar.gz"}) {
            Check(ParseHighrateObservationFiles(html(name),date,count).size()==1,
                "A historical high-rate filename was omitted");
            Check(count == (std::string(name).find(".tar") == std::string::npos ? 1 : 96),
                "High-rate archive count is incorrect");
        }
        for (const auto& name : {"abmf001A00.25n.Z", "abmf001A.25g.Z",
            "ABMF00GLP_R_20250010000_15M_01S_MN.rnx.gz",
            "ABMF00GLP_R_20250010000_15M_01S_MO.log.gz"})
            Check(ParseHighrateObservationFiles(html(name),date,count).empty(), "Navigation/log mistaken for high-rate observation");

        const auto historical = ParseBroadcastDirectoryListing(html("brdm0010.14p.Z"),2014,L"https://example.invalid/2014/001/");
        const auto modern = ParseBroadcastDirectoryListing(html("BRDM00DLR_S_20140010000_01D_MN.rnx.gz"),2014,L"https://example.invalid/2014/001/");
        Check(historical.size()==1 && modern.size()==1,"Broadcast naming generation omitted");
        Check(historical[0].logicalKey==modern[0].logicalKey,"Old and new navigation names became different products");
        Check(historical[0].logicalKey=="BRDM00DLR|2014-01-01|0000|01D|MN|RINEX 3|S","Unexpected broadcast import identity");
        for (const auto& name : {"brdc0010.25n.Z", "brdc0010.25g.gz", "brdm0010.25p",
            "BRDC00IGS_R_20250010000_01D_MN.rnx.gz", "BRDC00IGS_R_20250010000_01D_MN.rnx.bz2"})
            Check(ParseBroadcastDirectoryListing(html(name),2025,L"https://example.invalid/").size()==1,"Navigation format omitted");
        for (const auto& name : {"brdm0010.25p.Z.md5", "brdc0010.25n.Z.sha256",
            "BRDC00IGS_R_20250010000_01D_MN.rnx.gz.sha256", "brdm3660.25p.Z",
            "BRDC00IGS_R_20250012400_01D_MN.rnx.gz", "BRDC00IGS_R_20250010060_01D_MN.rnx.gz"})
            Check(ParseBroadcastDirectoryListing(html(name),2025,L"https://example.invalid/").empty(),"Invalid/sidecar navigation filename was admitted");
        const int week = CalculateGpsWeek(date);
        const auto day = std::to_string(week)+"3"; // 2025-01-01 is Wednesday.
        Check(ParseSp3DirectoryListing("igs"+day+".sp3.Z",week,L"https://example.invalid/","ops").size()==1,"Legacy SP3 omitted");
        Check(ParseClkDirectoryListing("cod"+day+".clk_30s.Z",week,L"https://example.invalid/","ops").size()==1,"Legacy CLK omitted");
        Check(ParseSp3DirectoryListing("igs"+day+".sp3.Z.md5",week,L"https://example.invalid/","ops").empty(),"SP3 checksum mistaken for product");
        Check(ParseClkDirectoryListing("cod"+day+".clk_30s.Z.md5",week,L"https://example.invalid/","ops").empty(),"CLK checksum mistaken for product");
        Check(ParseAttDirectoryListing("WUM0MGXRAP_20250010000_01D_30S_ATT.OBX.gz",week,L"https://example.invalid/","mgex").size()==1,"ATT product omitted");
        Check(ParseAttDirectoryListing("WUM0MGXRAP_20250010000_01D_30S_ATT.OBX.gz.sha256",week,L"https://example.invalid/","mgex").empty(),"ATT checksum mistaken for product");

        Check(ParseClkDirectoryListing("cod22370_v3.clk_05s.Z",2237,L"https://example.invalid/","OPS").size()==1,"Legacy RINEX 3 CLK variant omitted");
        const auto oldOrbit=ParseSp3DirectoryListing("cod07300.eph.Z",730,L"https://example.invalid/","OPS");
        Check(oldOrbit.size()==1 && oldOrbit[0].center=="COD","CODE EPH/SP3 orbit omitted");
        Check(ParseSp3DirectoryListing("cod07300.eph.Z.md5",730,L"https://example.invalid/","OPS").empty(),"EPH sidecar admitted");
        const auto repro=ParseSp3DirectoryListing("cf207300.eph.Z\nco207300.sp3.Z",730,L"https://example.invalid/","REPRO2");
        Check(repro.size()==2 && repro[0].latency=="REPROCESSING" && repro[1].center=="COD","Reprocessing center/class incorrect");
        Check(repro[0].logicalKey!=repro[1].logicalKey,"Distinct reprocessing streams collapsed");
        Check(ParseAttDirectoryListing("cod22370.obx.Z",2237,L"https://example.invalid/","OPS").size()==1,"Short ORBEX omitted");
        Check(ParseOsbDirectoryListing("cod22370.bia.Z",2237,L"https://example.invalid/","OPS").size()==1,"Short CODE OSB omitted");
        Check(ParseOsbDirectoryListing("cod22370.bia.Z.sha256",2237,L"https://example.invalid/","OPS").empty(),"OSB sidecar admitted");
        Check(ParseErpDirectoryListing("co107307.erp.Z",730,L"https://example.invalid/","REPRO1")[0].latency=="REPROCESSING","ERP reprocessing class incorrect");
        Check(ParseErpDirectoryListing("cod07307.erp.Z.md5",730,L"https://example.invalid/","OPS").empty(),"ERP sidecar admitted");
        const auto weeklyOrbit=ParseSp3DirectoryListing("gfz06517.eph.Z",651,L"https://example.invalid/","OPS");
        Check(weeklyOrbit.size()==1 && weeklyOrbit[0].duration=="07D" && weeklyOrbit[0].nominalDate.day==28 && weeklyOrbit[0].nominalDate.month==6,"Weekly orbit coverage start incorrect");
        const auto partialJpl=ParseSp3DirectoryListing("jpl06727.sp3.Z",672,L"https://example.invalid/","OPS");
        Check(partialJpl.size()==1 && partialJpl[0].duration=="05D" && partialJpl[0].nominalDate.day==23,"Verified JPL five-day header coverage was lost on a rescan");
        Check(ParseSp3DirectoryListing("emr06607.eph.Z",660,L"https://example.invalid/","OPS").empty(),"Known all-zero historic orbit admitted as SP3");
        const auto jplTrop=ParseTroDirectoryListing("jpl09716.tro.Z",MakeQueryDate(1998,8,22),L"https://example.invalid/","OPS",971);
        Check(jplTrop.size()==1 && jplTrop[0].center=="JPL" && jplTrop[0].project=="OPS","JPL historical TRO omitted or assigned to EPN");
        const auto oldDay=MakeQueryDate(1994,1,2);
        const auto trop=ParseTroDirectoryListing("cod07300.tro.Z",oldDay,L"https://example.invalid/","OPS",730);
        Check(trop.size()==1 && trop[0].productFamily=="CODE_TRO","CODE weekly TRO omitted or mislabeled EPN");
        Check(ParseTroDirectoryListing("cod07300.tro.Z",MakeQueryDate(1994,1,3),L"https://example.invalid/","OPS",730).empty(),"CODE TRO assigned to wrong day");
        Check(ParseSinexDirectoryListing("igs09567.snx.Z",956,L"https://example.invalid/","OPS").size()==1,"Pre-1000 SINEX omitted");
        Check(ParseSinexDirectoryListing("igs09567.snx.Z.md5",956,L"https://example.invalid/","OPS").empty(),"SINEX sidecar admitted");
        Check(ParseIonexDirectoryListing("codg0010.25i.Z.md5",date,L"https://example.invalid/").empty(),"IONEX sidecar admitted");
        std::cout << "Archive filename matrix passed: old/new observation, navigation, high-rate, SP3 and CLK; sidecars excluded.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
