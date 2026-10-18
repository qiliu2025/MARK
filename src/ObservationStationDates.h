#pragma once

#include "Models.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Shared date-window predicate for reference-frame-aware queries and
// downloads. Ordinals intentionally match ObservationSearchDb's sys_days
// representation so the filter can be applied directly to bitmaps and
// hydrated dated rows.
class ObservationStationDateFilter {
public:
    explicit ObservationStationDateFilter(
            const ObservationRangeOptions& options) {
        active_ = options.restrictStationDates;
        if (!active_) return;
        for (const auto& window : options.stationDateWindows) {
            int start = 0;
            int end = 0;
            if (!ToOrdinal(window.start, start) || !ToOrdinal(window.end, end)
                || end < start) continue;
            for (const auto& rawCode : window.stationCodes) {
                auto code = Normalize(rawCode);
                if (code.empty()) continue;
                entries_.push_back(Entry{std::move(code), start, end});
            }
        }
    }

    bool active() const { return active_; }
    bool hasEntries() const { return !entries_.empty(); }

    bool Allows(const StationFile& station, const QueryDate& date) const {
        if (!active_) return true;
        int ordinal = 0;
        if (!ToOrdinal(date, ordinal)) return false;
        return AllowsOrdinal(station, ordinal);
    }

    bool AllowsOrdinal(const StationFile& station, const int ordinal) const {
        if (!active_) return true;
        const auto code = StationCode(station);
        if (code.empty()) return false;
        return std::any_of(entries_.begin(), entries_.end(),
            [&](const Entry& entry) {
                return entry.code == code
                    && ordinal >= entry.start && ordinal <= entry.end;
            });
    }

    // Build inclusive, merged intervals for a station. This is used to make
    // a bitmap of eligible days before completeness is counted.
    std::vector<std::pair<int, int>> Ranges(
            const StationFile& station,
            const int firstOrdinal,
            const int lastOrdinal) const {
        if (!active_) return {{firstOrdinal, lastOrdinal}};
        const auto code = StationCode(station);
        std::vector<std::pair<int, int>> ranges;
        if (code.empty()) return ranges;
        for (const auto& entry : entries_) {
            if (entry.code != code || entry.end < firstOrdinal
                || entry.start > lastOrdinal) continue;
            ranges.emplace_back(
                std::max(entry.start, firstOrdinal),
                std::min(entry.end, lastOrdinal));
        }
        std::sort(ranges.begin(), ranges.end());
        std::vector<std::pair<int, int>> merged;
        for (const auto& range : ranges) {
            if (merged.empty()
                || range.first > merged.back().second + 1) {
                merged.push_back(range);
            } else {
                merged.back().second =
                    std::max(merged.back().second, range.second);
            }
        }
        return merged;
    }

    static std::string StationCode(const StationFile& station) {
        std::string code = station.fourCharId.empty()
            ? station.stationId : station.fourCharId;
        if (code.size() > 4) code.resize(4);
        return Normalize(code);
    }

    static bool ToOrdinal(const QueryDate& date, int& ordinal) {
        const auto ymd = std::chrono::year{date.year}
            / std::chrono::month{static_cast<unsigned>(date.month)}
            / std::chrono::day{static_cast<unsigned>(date.day)};
        if (!ymd.ok()) return false;
        const auto count = std::chrono::sys_days{ymd}
            .time_since_epoch().count();
        if (count < std::numeric_limits<int>::min()
            || count > std::numeric_limits<int>::max()) return false;
        ordinal = static_cast<int>(count);
        return true;
    }

private:
    struct Entry {
        std::string code;
        int start;
        int end;
    };

    static std::string Normalize(std::string value) {
        value.erase(std::remove_if(value.begin(), value.end(),
            [](const unsigned char ch) {
                return std::isspace(ch) != 0;
            }), value.end());
        std::transform(value.begin(), value.end(), value.begin(),
            [](const unsigned char ch) {
                return static_cast<char>(std::toupper(ch));
            });
        return value;
    }

    bool active_ = false;
    std::vector<Entry> entries_;
};

