#pragma once

#include "Models.h"

#include <string>
#include <vector>

enum class PreciseProductCategory {
    Orbit,
    Clock,
    EarthRotation,
    DifferentialCodeBias,
    Attitude,
    Ionosphere,
    Troposphere,
    Sinex,
    Antenna,
    BroadcastNavigation,
    ObservableSpecificBias,
    UncalibratedPhaseDelay,
    VmfGrid
};

enum class PreciseProductLatency {
    Final,
    Rapid,
    UltraRapid,
    NearRealTime,
    RealTime,
    Predicted,
    Daily,
    Weekly,
    Static,
    Other
};

struct PreciseProductMirror {
    std::wstring source;
    std::wstring url;
    std::wstring fileName;
};

struct PreciseUpdFileRecord {
    QueryDate date{};
    std::wstring logicalKey;
    std::wstring fileName;
    std::wstring dcbFileName;
    bool flagged = false;
    bool selected = false;
};

struct PreciseProductFile {
    std::wstring id;
    std::wstring name;
    PreciseProductCategory category = PreciseProductCategory::Orbit;
    PreciseProductLatency latency = PreciseProductLatency::Final;
    std::wstring center;
    std::wstring constellation;
    std::wstring interval;
    std::vector<PreciseProductMirror> mirrors;
    bool selected = false;
    bool localInventoryOnly = false;
    std::wstring status;
    // Availability over the currently displayed date range.  A single-day
    // query reports 1/1 when the product is present; range queries aggregate
    // the same logical product across all requested days.
    int availableDays = 0;
    int expectedDays = 0;
    double completenessPercent = 0.0;
    // ANTEX/RINEX format metadata is independent of sampling interval.
    std::wstring formatVersion;
    std::vector<PreciseUpdFileRecord> updRecords;
    // A preset requirement with no matching records in the displayed range.
    // It has no files or routes and cannot be selected for download.
    bool missingRequired = false;
};

std::vector<PreciseProductFile> BuildPreciseProductCatalog(
    const QueryDate& date,
    const std::wstring& troposphereStation = L"ZIM2");

// CAS/BDSmart publishes a deterministic daily RT-PPP bundle (SP3, CLK and
// OSB/BIA) under product/rts/rtpp.  These routes are not part of the weekly
// IGS archive index, so the precision pane can add them directly when the
// requested date is covered by the public BDSmart archive.
std::vector<PreciseProductFile> BuildBdsMartRtppProducts(
    const QueryDate& date);

const wchar_t* PreciseProductCategoryName(PreciseProductCategory category);
const wchar_t* PreciseProductLatencyName(PreciseProductLatency latency);
