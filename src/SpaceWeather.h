#pragma once

#include "Models.h"

#include <filesystem>
#include <limits>
#include <stop_token>
#include <string>
#include <unordered_set>
#include <vector>

struct SpaceWeatherDay {
    QueryDate date{};
    double kpMax = 0.0;
    double apDaily = 0.0;
    double dstMin = 0.0;
    double dstMean = 0.0;
    double f107 = 0.0;
    double f10781 = 0.0;
    double f107Adjusted = std::numeric_limits<double>::quiet_NaN();
    int kpCount = 0;
    int dstCount = 0;
    std::string source;
    std::string status;
};

struct SpaceWeatherDateSelection {
    bool active = false;
    int requestedDays = 0;
    int matchedDays = 0;
    int missingDays = 0;
    std::vector<int> ordinals;
    std::unordered_set<int> ordinalSet;
    std::wstring warning;
    std::wstring error;

    [[nodiscard]] bool Contains(const int ordinal) const {
        return !active || ordinalSet.contains(ordinal);
    }
};

std::filesystem::path DefaultSpaceWeatherDatabasePath(
    const std::filesystem::path& knowledgeBaseDirectory = {});

bool HasSpaceWeatherFilter(const ObservationRangeOptions& options);

struct SpaceWeatherPreviewDay {
    int ordinal = 0;
    SpaceWeatherDay values;
    bool retained = false;
    unsigned failedMetrics = 0;  // Kp=1, Dst=2, F10.7=4.
    unsigned missingMetrics = 0;
};

struct SpaceWeatherPreview {
    int requestedDays = 0;
    int retainedDays = 0;
    int missingDays = 0;
    std::vector<SpaceWeatherPreviewDay> days;
    std::wstring error;
};

// The preview uses the same date selection as observation queries.
SpaceWeatherPreview PreviewSpaceWeatherDates(
    const std::filesystem::path& databasePath,
    const QueryDate& startDate,
    const QueryDate& endDate,
    const ObservationRangeOptions& options,
    std::stop_token stopToken = {});

// Reads the local daily Kp/Dst/F10.7 index and returns the dates satisfying
// all enabled predicates.  No network access is performed here.  Missing
// metric rows are excluded conservatively and reported in warning.
SpaceWeatherDateSelection SelectSpaceWeatherDates(
    const std::filesystem::path& databasePath,
    const QueryDate& startDate,
    const QueryDate& endDate,
    const ObservationRangeOptions& options,
    std::stop_token stopToken = {});

