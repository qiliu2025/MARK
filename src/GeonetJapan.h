#pragma once

#include "Models.h"

#include <functional>
#include <stop_token>
#include <string>
#include <vector>

struct GeonetJapanDailyResult {
    QueryDate date;
    std::vector<StationFile> stations;
    std::vector<StationFile> files;
};

struct GeonetJapanBlockResult {
    std::vector<GeonetJapanDailyResult> days;
    std::wstring error;
    bool networkTransportFailure = false;
    long transportErrorCode = 0;
    int requestsAttempted = 0;
    int requestsSuccessful = 0;
};

// A snapshot emitted while a Terras date block is being queried.  One
// station group contains at most ten stations.  Failed groups can be retried,
// so requestsCompleted may be greater than groupsSuccessful.
struct GeonetJapanProgress {
    int round = 1;
    std::size_t groupsTotal = 0;
    std::size_t roundGroupsTotal = 0;
    std::size_t roundGroupsCompleted = 0;
    int requestsStarted = 0;
    int requestsCompleted = 0;
    int groupsSuccessful = 0;
};

using GeonetJapanProgressCallback =
    std::function<void(const GeonetJapanProgress&)>;

// Terras permits at most ten stations and ten consecutive dates in one
// request.  This routine always stays within those documented limits and
// parallelizes only independent station groups.
GeonetJapanBlockResult QueryGeonetJapanBlock(
    const QueryDate& firstDate,
    const QueryDate& lastDate,
    int jobs,
    std::stop_token stopToken,
    const GeonetJapanProgressCallback& progressCallback = {});

// Registered Terras SFTP path.  The low-level call accepts up to 62
// consecutive daily directories.  The builder calls it for at most four
// consecutive UTC days per login; a successful result is committed one day
// at a time, while a failed multi-day result is discarded and retried as
// independent single days.  Other callers still receive all-or-nothing data.
// The password is used only for this call.
GeonetJapanBlockResult QueryGeonetJapanSftpBlock(
    const QueryDate& firstDate,
    const QueryDate& lastDate,
    const std::wstring& username,
    const std::wstring& password,
    std::stop_token stopToken,
    const GeonetJapanProgressCallback& progressCallback = {});
