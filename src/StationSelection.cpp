#include "StationSelection.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <random>
#include <vector>

namespace {

constexpr double kEarthRadiusKm = 6371.0088;
constexpr double kDistanceEpsilonKm = 1e-7;

struct UnitVector {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct Candidate {
    std::size_t stationIndex = 0;
    UnitVector position;
};

UnitVector ToUnitVector(const double latitude, const double longitude) {
    const double radians = std::numbers::pi / 180.0;
    const double latitudeRadians = latitude * radians;
    const double longitudeRadians = longitude * radians;
    const double horizontal = std::cos(latitudeRadians);
    return {
        horizontal * std::cos(longitudeRadians),
        horizontal * std::sin(longitudeRadians),
        std::sin(latitudeRadians)};
}

double Dot(const UnitVector& left, const UnitVector& right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

double ChordDistanceSquared(
    const UnitVector& left,
    const UnitVector& right) {
    return std::clamp(2.0 - 2.0 * Dot(left, right), 0.0, 4.0);
}

double ChordToRadians(const double chordSquared) {
    const double chord = std::sqrt(std::clamp(chordSquared, 0.0, 4.0));
    return 2.0 * std::asin(std::clamp(chord / 2.0, 0.0, 1.0));
}

double DistanceKm(const UnitVector& left, const UnitVector& right) {
    return ChordToRadians(ChordDistanceSquared(left, right)) * kEarthRadiusKm;
}
double DistanceChord(const double km) {
    const auto sine = std::sin(std::clamp(km / kEarthRadiusKm, 0.0, std::numbers::pi) / 2);
    return 4 * sine * sine;
}

// The nearest point to a station's antipode is its farthest station. A balanced
// 3-D tree finds the exact seed pair without testing every pair in a large pool.
class AntipodeTree {
    struct Node {
        std::size_t candidate;
        int left = -1, right = -1;
        std::array<double, 3> low{}, high{};
    };
    const std::vector<Candidate>& candidates_;
    std::vector<std::size_t> order_;
    std::vector<Node> nodes_;
    static std::array<double, 3> Coordinates(const UnitVector& p) { return {p.x, p.y, p.z}; }
    int Build(std::size_t first, std::size_t last, unsigned axis) {
        if (first == last) return -1;
        const auto middle = first + (last - first) / 2;
        std::nth_element(order_.begin() + first, order_.begin() + middle, order_.begin() + last,
            [&](auto a, auto b) {
                const auto x = Coordinates(candidates_[a].position)[axis], y = Coordinates(candidates_[b].position)[axis];
                return x != y ? x < y : a < b;
            });
        const auto id = static_cast<int>(nodes_.size());
        Node node{order_[middle]}; node.low = node.high = Coordinates(candidates_[node.candidate].position);
        nodes_.push_back(node);
        const auto left = Build(first, middle, (axis + 1) % 3), right = Build(middle + 1, last, (axis + 1) % 3);
        auto& current = nodes_[id]; current.left = left; current.right = right;
        for (const auto child : {left, right}) if (child >= 0)
            for (unsigned a = 0; a < 3; ++a) {
                current.low[a] = std::min(current.low[a], nodes_[child].low[a]);
                current.high[a] = std::max(current.high[a], nodes_[child].high[a]);
            }
        return id;
    }
    double Bound(int id, const std::array<double, 3>& point) const {
        if (id < 0) return std::numeric_limits<double>::infinity();
        double distance = 0;
        for (unsigned a = 0; a < 3; ++a) {
            const auto delta = point[a] - std::clamp(point[a], nodes_[id].low[a], nodes_[id].high[a]);
            distance += delta * delta;
        }
        return distance;
    }
    void Search(int id, std::size_t source, const std::array<double, 3>& point,
                std::size_t& best, double& chord) const {
        if (id < 0 || Bound(id, point) > 4 - chord + 1e-12) return;
        const auto& node = nodes_[id];
        if (node.candidate != source) {
            const auto score = ChordDistanceSquared(candidates_[source].position, candidates_[node.candidate].position);
            if (score > chord || (score == chord && node.candidate < best)) { chord = score; best = node.candidate; }
        }
        const auto near = Bound(node.left, point) <= Bound(node.right, point) ? node.left : node.right;
        Search(near, source, point, best, chord);
        Search(near == node.left ? node.right : node.left, source, point, best, chord);
    }
public:
    AntipodeTree(const std::vector<Candidate>& candidates, const std::vector<std::size_t>& eligible)
        : candidates_(candidates), order_(eligible) { nodes_.reserve(eligible.size()); Build(0, order_.size(), 0); }
    std::size_t Farthest(std::size_t source) const {
        const auto p = candidates_[source].position;
        std::size_t best = candidates_.size(); double chord = -1;
        Search(0, source, {-p.x, -p.y, -p.z}, best, chord);
        return best;
    }
};

bool HasUsableCoordinate(const StationFile& station) {
    return station.hasCoordinate
        && std::isfinite(station.latitude)
        && std::isfinite(station.longitude)
        && station.latitude >= -90.0
        && station.latitude <= 90.0
        && station.longitude >= -180.0
        && station.longitude <= 180.0;
}

std::vector<std::size_t> LargestConnectedComponent(
    const std::vector<Candidate>& candidates,
    const double maximumNeighborDistanceKm) {
    if (candidates.empty()) {
        return {};
    }
    std::vector<bool> visited(candidates.size(), false);
    const auto maximumChord = DistanceChord(maximumNeighborDistanceKm + kDistanceEpsilonKm);
    std::vector<std::size_t> largest;
    for (std::size_t root = 0; root < candidates.size(); ++root) {
        if (visited[root]) {
            continue;
        }
        std::vector<std::size_t> component;
        std::vector<std::size_t> queue{root};
        visited[root] = true;
        for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
            const std::size_t current = queue[cursor];
            component.push_back(current);
            for (std::size_t other = 0; other < candidates.size(); ++other) {
                if (!visited[other]
                    && ChordDistanceSquared(
                           candidates[current].position,
                           candidates[other].position)
                        <= maximumChord) {
                    visited[other] = true;
                    queue.push_back(other);
                }
            }
        }
        if (component.size() > largest.size()) {
            largest = std::move(component);
        }
    }
    return largest;
}

std::size_t CentroidSeed(
    const std::vector<Candidate>& candidates,
    const std::vector<std::size_t>& component) {
    UnitVector centroid;
    for (const auto index : component) {
        centroid.x += candidates[index].position.x;
        centroid.y += candidates[index].position.y;
        centroid.z += candidates[index].position.z;
    }
    const double norm = std::sqrt(
        centroid.x * centroid.x
        + centroid.y * centroid.y
        + centroid.z * centroid.z);
    if (norm <= std::numeric_limits<double>::epsilon()) {
        return component.front();
    }
    centroid.x /= norm;
    centroid.y /= norm;
    centroid.z /= norm;
    std::size_t best = component.front();
    double bestDot = -2.0;
    for (const auto index : component) {
        const double score = Dot(candidates[index].position, centroid);
        if (score > bestDot) {
            bestDot = score;
            best = index;
        }
    }
    return best;
}

void CalculateMetrics(
    const std::vector<Candidate>& candidates,
    const std::vector<std::size_t>& selected,
    RegionalSelectionResult& result) {
    if (selected.size() < 2) {
        return;
    }
    double minimumPair = std::numeric_limits<double>::infinity();
    double maximumNearest = 0.0;
    for (std::size_t left = 0; left < selected.size(); ++left) {
        double nearest = std::numeric_limits<double>::infinity();
        for (std::size_t right = 0; right < selected.size(); ++right) {
            if (left == right) {
                continue;
            }
            const double distance = DistanceKm(
                candidates[selected[left]].position,
                candidates[selected[right]].position);
            nearest = std::min(nearest, distance);
            if (right > left) {
                minimumPair = std::min(minimumPair, distance);
            }
        }
        maximumNearest = std::max(maximumNearest, nearest);
    }
    result.achievedMinimumDistanceKm = minimumPair;
    result.achievedMaximumNearestNeighborKm = maximumNearest;
}

}  // namespace

RegionalSelectionResult SelectRegionallyDistributedStations(
    std::vector<StationFile>& stations,
    const std::vector<std::size_t>& candidateIndices,
    const RegionalSelectionOptions& options) {
    RegionalSelectionResult result;
    result.requested = options.requestedCount;
    for (auto& station : stations) {
        station.selected = false;
    }

    std::vector<bool> seen(stations.size(), false);
    std::vector<std::size_t> validIndices;
    std::vector<std::size_t> withoutCoordinates;
    std::vector<Candidate> candidates;
    validIndices.reserve(candidateIndices.size());
    candidates.reserve(candidateIndices.size());
    for (const auto index : candidateIndices) {
        if (index >= stations.size() || seen[index]) {
            continue;
        }
        seen[index] = true;
        validIndices.push_back(index);
        if (HasUsableCoordinate(stations[index])) {
            candidates.push_back({
                index,
                ToUnitVector(
                    stations[index].latitude,
                    stations[index].longitude)});
        } else {
            withoutCoordinates.push_back(index);
        }
    }
    result.candidateCount = validIndices.size();
    result.candidatesWithCoordinates = candidates.size();
    result.excludedWithoutCoordinates = withoutCoordinates.size();
    if (options.requestedCount == 0 || validIndices.empty()) {
        return result;
    }

    const std::size_t target = std::min(
        options.requestedCount,
        validIndices.size());
    std::vector<std::size_t> eligible(candidates.size());
    std::iota(eligible.begin(), eligible.end(), 0);
    if (options.maximumNeighborDistanceKm > 0.0) {
        eligible = LargestConnectedComponent(
            candidates,
            options.maximumNeighborDistanceKm);
    }
    result.largestConnectedCandidateCount = eligible.size();

    const std::size_t coordinateTarget = std::min(target, eligible.size());
    std::vector<bool> chosen(candidates.size(), false);
    std::vector<std::size_t> selected;
    selected.reserve(coordinateTarget);
    std::vector<double> nearestChord(candidates.size(), 4.0);
    const auto minimumChord = options.minimumDistanceKm - kDistanceEpsilonKm > std::numbers::pi * kEarthRadiusKm
        ? std::numeric_limits<double>::infinity()
        : DistanceChord(std::max(0.0, options.minimumDistanceKm - kDistanceEpsilonKm));
    const auto maximumChord = DistanceChord(options.maximumNeighborDistanceKm + kDistanceEpsilonKm);
    const auto choose = [&](const std::size_t index) {
        chosen[index] = true; selected.push_back(index);
        for (const auto candidate : eligible)
            nearestChord[candidate] = std::min(nearestChord[candidate],
                ChordDistanceSquared(candidates[candidate].position, candidates[index].position));
    };

    if (options.randomize) {
        // Randomized greedy selection.  The first station and every later
        // feasible station are sampled without replacement.  This keeps the
        // UI's "随机选站" action genuinely random while respecting the same
        // minimum-spacing and connected-neighbour constraints as uniform
        // selection.
        std::random_device device;
        std::seed_seq seed{
            device(), device(), device(),
            static_cast<unsigned>(
                std::chrono::steady_clock::now().time_since_epoch().count())};
        std::mt19937_64 generator(seed);
        if (options.randomSeed != 0) generator.seed(options.randomSeed);
        std::shuffle(eligible.begin(), eligible.end(), generator);
        if (coordinateTarget > 0) {
            const std::size_t first = eligible.front();
            choose(first);
        }
        while (selected.size() < coordinateTarget) {
            std::vector<std::size_t> feasible;
            feasible.reserve(eligible.size() - selected.size());
            for (const auto index : eligible) {
                if (chosen[index]) continue;
                const double nearest = nearestChord[index];
                if (nearest < minimumChord) {
                    continue;
                }
                if (options.maximumNeighborDistanceKm > 0.0
                    && nearest > maximumChord) {
                    continue;
                }
                feasible.push_back(index);
            }
            if (feasible.empty()) break;
            std::uniform_int_distribution<std::size_t> pick(
                0, feasible.size() - 1);
            const std::size_t next = feasible[pick(generator)];
            choose(next);
        }
    } else {
        if (coordinateTarget == 1) {
            const std::size_t first = options.maximumNeighborDistanceKm > 0.0
                ? CentroidSeed(candidates, eligible)
                : eligible.front();
            choose(first);
        } else if (coordinateTarget >= 2
            && options.maximumNeighborDistanceKm <= 0.0) {
            std::size_t first = eligible.front();
            std::size_t second = eligible[1];
            double farthest = -1.0;
            if (eligible.size() > 256) {
                const AntipodeTree tree(candidates, eligible);
                for (const auto source : eligible) {
                    const auto other = tree.Farthest(source);
                    const auto left = std::min(source, other), right = std::max(source, other);
                    const auto distance = ChordDistanceSquared(candidates[left].position, candidates[right].position);
                    if (distance > farthest || (distance == farthest && std::pair{left, right} < std::pair{first, second})) {
                        farthest = distance; first = left; second = right;
                    }
                }
            } else for (std::size_t left = 0; left < eligible.size(); ++left) {
                for (std::size_t right = left + 1; right < eligible.size(); ++right) {
                    const double distance = ChordDistanceSquared(
                        candidates[eligible[left]].position,
                        candidates[eligible[right]].position);
                    if (distance > farthest) {
                        farthest = distance;
                        first = eligible[left];
                        second = eligible[right];
                    }
                }
            }
            choose(first);
            if (farthest >= minimumChord) {
                choose(second);
            }
        } else if (coordinateTarget >= 2) {
            const std::size_t first = CentroidSeed(candidates, eligible);
            choose(first);
        }

        while (selected.size() < coordinateTarget) {
            std::size_t next = candidates.size();
            double bestNearest = -1.0;
            for (const auto index : eligible) {
                if (chosen[index]) {
                    continue;
                }
                const double nearest = nearestChord[index];
                if (nearest < minimumChord) {
                    continue;
                }
                if (options.maximumNeighborDistanceKm > 0.0
                    && nearest > maximumChord) {
                    continue;
                }
                if (nearest > bestNearest) {
                    bestNearest = nearest;
                    next = index;
                }
            }
            if (next == candidates.size()) {
                break;
            }
            choose(next);
        }
    }

    for (const auto index : selected) {
        stations[candidates[index].stationIndex].selected = true;
    }
    result.selectedWithCoordinates = selected.size();

    // Without spatial constraints, preserve the old useful fallback: a user
    // requesting more stations than have coordinates still receives records
    // that can be downloaded from the list.
    if (!options.randomize
        && options.minimumDistanceKm <= 0.0
        && options.maximumNeighborDistanceKm <= 0.0) {
        const std::size_t remaining = target - result.selectedWithCoordinates;
        for (std::size_t index = 0;
             index < remaining && index < withoutCoordinates.size();
             ++index) {
            stations[withoutCoordinates[index]].selected = true;
            ++result.selectedWithoutCoordinates;
        }
    }
    if (options.randomize
        && options.minimumDistanceKm <= 0.0
        && options.maximumNeighborDistanceKm <= 0.0
        && result.selected < target) {
        std::random_device device;
        std::mt19937_64 generator(device());
        if (options.randomSeed != 0) generator.seed(options.randomSeed);
        std::shuffle(withoutCoordinates.begin(), withoutCoordinates.end(), generator);
        const std::size_t remaining = target - result.selectedWithCoordinates;
        for (std::size_t index = 0;
             index < remaining && index < withoutCoordinates.size();
             ++index) {
            stations[withoutCoordinates[index]].selected = true;
            ++result.selectedWithoutCoordinates;
        }
    }
    result.selected = result.selectedWithCoordinates
        + result.selectedWithoutCoordinates;
    CalculateMetrics(candidates, selected, result);
    return result;
}

GlobalSelectionResult SelectGloballyDistributedStations(
    std::vector<StationFile>& stations,
    const std::size_t requestedCount) {
    std::vector<std::size_t> indices(stations.size());
    std::iota(indices.begin(), indices.end(), 0);
    const auto regional = SelectRegionallyDistributedStations(
        stations,
        indices,
        RegionalSelectionOptions{requestedCount, 0.0, 0.0});
    GlobalSelectionResult result;
    result.requested = regional.requested;
    result.selected = regional.selected;
    result.selectedWithCoordinates = regional.selectedWithCoordinates;
    result.selectedWithoutCoordinates = regional.selectedWithoutCoordinates;
    result.minimumSeparationDegrees =
        regional.achievedMinimumDistanceKm / kEarthRadiusKm
        * 180.0 / std::numbers::pi;
    return result;
}
