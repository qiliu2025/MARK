#pragma once
#include "Sp3Availability.h"
#include "DateUtil.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

// CDDIS historical SP3 headers read on 2026-09-29. The filename's day=7
// identifies a weekly submission, whose actual data can cover fewer days.
// These fixed archive facts keep a later native rebuild from guessing dates.
inline bool ApplyVerifiedLegacyOrbitHeader(const std::string& filename, Sp3ArchiveFile& file) {
    struct Header { bool supported; int year,month,day,hour,minute; const char* duration; const char* interval; };
    static const std::unordered_map<std::string,Header> headers{
        {"emr06567.eph", {false,0,0,0,0,0,"",""}},
        {"emr06577.eph", {false,0,0,0,0,0,"",""}},
        {"emr06587.eph", {false,0,0,0,0,0,"",""}},
        {"emr06597.eph", {false,0,0,0,0,0,"",""}},
        {"emr06607.eph", {false,0,0,0,0,0,"",""}},
        {"emr06617.eph", {false,0,0,0,0,0,"",""}},
        {"emr06627.eph", {false,0,0,0,0,0,"",""}},
        {"emr06637.eph", {false,0,0,0,0,0,"",""}},
        {"emr06647.eph", {false,0,0,0,0,0,"",""}},
        {"emr06657.eph", {false,0,0,0,0,0,"",""}},
        {"emr06667.eph", {false,0,0,0,0,0,"",""}},
        {"emr06677.eph", {false,0,0,0,0,0,"",""}},
        {"emr06687.eph", {false,0,0,0,0,0,"",""}},
        {"emr06697.eph", {false,0,0,0,0,0,"",""}},
        {"emr06707.eph", {false,0,0,0,0,0,"",""}},
        {"emr06717.eph", {false,0,0,0,0,0,"",""}},
        {"emr06727.eph", {false,0,0,0,0,0,"",""}},
        {"emr06737.eph", {false,0,0,0,0,0,"",""}},
        {"emr06747.eph", {false,0,0,0,0,0,"",""}},
        {"emr06757.eph", {false,0,0,0,0,0,"",""}},
        {"emr06767.eph", {false,0,0,0,0,0,"",""}},
        {"emr06777.eph", {false,0,0,0,0,0,"",""}},
        {"emr06787.eph", {false,0,0,0,0,0,"",""}},
        {"emr06797.eph", {false,0,0,0,0,0,"",""}},
        {"emr06807.eph", {false,0,0,0,0,0,"",""}},
        {"emr06817.eph", {false,0,0,0,0,0,"",""}},
        {"emr06827.eph", {false,0,0,0,0,0,"",""}},
        {"emr06837.eph", {false,0,0,0,0,0,"",""}},
        {"emr06847.eph", {false,0,0,0,0,0,"",""}},
        {"emr06857.eph", {false,0,0,0,0,0,"",""}},
        {"emr06867.eph", {false,0,0,0,0,0,"",""}},
        {"emr06877.eph", {false,0,0,0,0,0,"",""}},
        {"emr06887.eph", {false,0,0,0,0,0,"",""}},
        {"emr06897.eph", {false,0,0,0,0,0,"",""}},
        {"emr06907.eph", {false,0,0,0,0,0,"",""}},
        {"emr06917.eph", {false,0,0,0,0,0,"",""}},
        {"gfz06507.eph", {true,1992,6,21,0,0,"07D","05M"}},
        {"gfz06517.eph", {true,1992,6,28,0,0,"07D","05M"}},
        {"jpl06697.sp3", {true,1992,11,2,0,0,"06D","15M"}},
        {"jpl06707.sp3", {true,1992,11,8,0,0,"07D","15M"}},
        {"jpl06717.sp3", {false,0,0,0,0,0,"",""}},
        {"jpl06727.sp3", {true,1992,11,23,0,0,"05D","15M"}},
        {"jpl06737.sp3", {true,1992,11,30,0,0,"03D","15M"}},
        {"jpl06747.sp3", {true,1992,12,8,0,0,"05D","15M"}},
        {"jpl06757.sp3", {true,1992,12,13,0,0,"07D","15M"}},
        {"jpl06767.sp3", {true,1992,12,20,0,0,"07D","15M"}},
        {"jpl06777.sp3", {true,1992,12,27,0,0,"07D","15M"}},
        {"jpl06787.sp3", {true,1993,1,3,0,0,"07D","15M"}},
        {"jpl06797.sp3", {true,1993,1,10,0,0,"07D","15M"}},
        {"jpl06807.sp3", {true,1993,1,17,0,0,"07D","15M"}},
        {"jpl06817.sp3", {true,1993,1,24,0,0,"07D","15M"}},
        {"jpl06827.sp3", {true,1993,1,31,0,0,"07D","15M"}},
        {"jpl06837.sp3", {true,1993,2,7,0,0,"07D","15M"}},
        {"jpl06847.sp3", {true,1993,2,14,0,0,"07D","15M"}},
        {"jpl06857.sp3", {true,1993,2,21,0,0,"07D","15M"}},
        {"jpl06877.sp3", {true,1993,3,7,0,0,"07D","15M"}},
        {"jpl06897.sp3", {true,1993,3,21,0,0,"07D","15M"}},
        {"jpl06907.sp3", {true,1993,3,28,0,0,"07D","15M"}},
        {"jpl06917.sp3", {true,1993,4,4,0,0,"07D","15M"}},
        {"jpl06937.sp3", {true,1993,4,18,0,0,"07D","15M"}},
        {"jpl06947.sp3", {true,1993,4,25,0,0,"07D","15M"}},
        {"jpl06967.sp3", {true,1993,5,9,0,0,"07D","15M"}},
        {"jpl06977.sp3", {true,1993,5,16,0,0,"07D","15M"}},
        {"jpl06987.sp3", {true,1993,5,23,0,0,"07D","15M"}},
        {"jpl06997.sp3", {true,1993,5,30,0,0,"07D","15M"}},
        {"jpl07007.sp3", {true,1993,6,6,0,0,"07D","15M"}},
        {"jpl07017.sp3", {true,1993,6,13,0,0,"07D","15M"}},
        {"jpl07027.sp3", {true,1993,6,20,0,0,"07D","15M"}},
        {"jpl07037.sp3", {true,1993,6,27,0,0,"07D","15M"}},
        {"jpl07047.sp3", {true,1993,7,4,0,0,"07D","15M"}},
        {"jpl07057.sp3", {true,1993,7,11,0,0,"07D","15M"}},
        {"jpl07067.sp3", {true,1993,7,18,0,0,"07D","15M"}},
        {"jpl07077.sp3", {true,1993,7,25,0,0,"07D","15M"}},
        {"jpl07087.sp3", {true,1993,8,1,0,0,"07D","15M"}},
        {"jpl07097.sp3", {true,1993,8,8,0,0,"07D","15M"}},
        {"jpl07107.sp3", {true,1993,8,15,0,0,"07D","15M"}},
        {"jpl07117.sp3", {true,1993,8,22,0,0,"07D","15M"}},
        {"jpl07147.sp3", {true,1993,9,12,0,0,"07D","15M"}},
        {"jpl07157.sp3", {true,1993,9,19,0,0,"07D","15M"}},
        {"jpl07167.sp3", {true,1993,9,26,0,0,"07D","15M"}},
        {"jpl07177.sp3", {true,1993,10,3,0,0,"07D","15M"}},
        {"utx06507.eph", {true,1992,6,21,0,0,"07D","15M"}},
        {"utx06517.eph", {true,1992,6,28,0,0,"07D","15M"}},
        {"utx06607.eph", {true,1992,9,5,0,0,"01D","15M"}},
    };
    std::string lower=filename;
    std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(lower.ends_with(".gz"))lower.resize(lower.size()-3);
    else if(lower.ends_with(".z"))lower.resize(lower.size()-2);
    const auto found=headers.find(lower);
    if(found==headers.end())return true;
    const auto& header=found->second;
    if(!header.supported)return false;
    file.nominalDate=MakeQueryDate(header.year,header.month,header.day);
    file.startHour=header.hour;file.startMinute=header.minute;
    file.duration=header.duration;file.interval=header.interval;file.constellation="G";
    return true;
}
