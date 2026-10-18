#pragma once

#include "Models.h"

#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <vector>

struct GlobalSelectionResult {
    std::size_t requested = 0;
    std::size_t selected = 0;
    std::size_t selectedWithCoordinates = 0;
    std::size_t selectedWithoutCoordinates = 0;
    double minimumSeparationDegrees = 0.0;
};

struct RegionalSelectionOptions {
    std::size_t requestedCount = 0;
    double minimumDistanceKm = 0.0;
    // Zero means unlimited. When enabled, every selected station after the
    // first must connect to the selected set with an edge no longer than this
    // value, producing one connected local network.
    double maximumNeighborDistanceKm = 0.0;
    // When true, choose a random feasible subset instead of the deterministic
    // spherical maximin subset.  Distance constraints, if supplied, are
    // still honoured by the randomized greedy pass.
    bool randomize = false;
    // A nonzero seed makes random selection reproducible.
    std::uint64_t randomSeed = 0;
};

struct RegionalSelectionResult {
    std::size_t requested = 0;
    std::size_t candidateCount = 0;
    std::size_t candidatesWithCoordinates = 0;
    std::size_t excludedWithoutCoordinates = 0;
    std::size_t largestConnectedCandidateCount = 0;
    std::size_t selected = 0;
    std::size_t selectedWithCoordinates = 0;
    std::size_t selectedWithoutCoordinates = 0;
    double achievedMinimumDistanceKm = 0.0;
    double achievedMaximumNearestNeighborKm = 0.0;
};

// Replaces the current selection with a deterministic spherical maximin
// subset.  The farthest available station pair is used as the seed; each
// subsequent station maximizes its distance to the nearest already selected
// station. Stations without coordinates are used only when the requested
// count exceeds the number of georeferenced candidates.
GlobalSelectionResult SelectGloballyDistributedStations(
    std::vector<StationFile>& stations,
    std::size_t requestedCount);

// Replaces the current selection with a deterministic maximin subset drawn
// only from candidateIndices. minimumDistanceKm prevents crowding. A positive
// maximumNeighborDistanceKm restricts selection to the largest connected
// candidate component and grows a connected station network.
RegionalSelectionResult SelectRegionallyDistributedStations(
    std::vector<StationFile>& stations,
    const std::vector<std::size_t>& candidateIndices,
    const RegionalSelectionOptions& options);

// Constraints on one fixed station set, over the original query calendar.
// Only an explicit date filter may remove dates. Missing/unknown observation
// dates remain in the report and unknown records never count as confirmed.
struct StationSelectionPlanOptions {
    RegionalSelectionOptions spatial;
    int firstOrdinal = 0;
    int lastOrdinal = -1;
    bool eligibleDatesKnown = false;
    std::vector<int> eligibleOrdinals;
    int minimumDailyStations = 0;
};

struct StationSelectionDay {
    int ordinal = 0;
    bool eligible = true;
    int available = 0;
    int unknown = 0;
    int candidateAvailable = 0;
    int candidateUnknown = 0;
};

struct StationSelectionPlan {
    std::wstring status;
    std::wstring message;
    bool success = false;
    bool provenInfeasible = false;
    bool generated = false;
    std::uint64_t randomSeed = 0;
    std::size_t requested = 0;
    std::size_t selected = 0;
    std::size_t candidates = 0;
    std::size_t withCoordinates = 0;
    std::size_t missingCalendars = 0;
    int minimumDailyStations = 0;
    int activeDays = 0;
    int excludedDays = 0;
    int failingDays = 0;
    int unknownDays = 0;
    int minimumAvailable = 0;
    double averageAvailable = 0.0;
    double minimumDistanceKm = 0.0;
    double maximumNearestKm = 0.0;
    bool spacingSatisfied = true;
    bool neighborSatisfied = true;
    std::vector<std::size_t> selectedIndices;
    std::vector<StationSelectionDay> days;
};

StationSelectionPlan EvaluateStationSelectionPlan(
    const std::vector<StationFile>& stations,
    const std::vector<std::size_t>& candidateIndices,
    const std::vector<std::size_t>& selectedIndices,
    const StationSelectionPlanOptions& options,
    std::stop_token stopToken = {});

// A bounded, cancellable heuristic. A failed search is reported as not-found,
// unless a necessary count/coverage condition proves infeasibility. It never
// mutates stations or silently accepts a partially satisfied plan.
StationSelectionPlan GenerateStationSelectionPlan(
    const std::vector<StationFile>& stations,
    const std::vector<std::size_t>& candidateIndices,
    const StationSelectionPlanOptions& options,
    std::stop_token stopToken = {});
