#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

// UTC calendar ordinals use the Unix epoch. Retain compact immutable query
// bitmaps, then expand just the selected station into contiguous display runs.
struct CalendarBitmapBlock {
    int firstOrdinal = 0;
    std::vector<std::uint64_t> words;
};
struct CalendarCoverage {
    int firstOrdinal = 0;
    int lastOrdinal = -1;
    bool restrictEligibleDates = false;
    std::vector<CalendarBitmapBlock> available;
    std::vector<CalendarBitmapBlock> unverified;
    std::vector<CalendarBitmapBlock> eligible;
};

inline int CalendarOrdinal(int year, int month, int day) {
    return static_cast<int>(std::chrono::sys_days{
        std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)}
        / std::chrono::day{static_cast<unsigned>(day)}}.time_since_epoch().count());
}
inline std::wstring CalendarDateText(int ordinal) {
    const std::chrono::year_month_day date{
        std::chrono::sys_days{std::chrono::days{ordinal}}};
    return std::format(L"{:04}-{:02}-{:02}", static_cast<int>(date.year()),
        static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()));
}
inline bool CalendarHasDay(const std::vector<CalendarBitmapBlock>& blocks, int day) {
    for (const auto& block : blocks) {
        const auto offset = static_cast<long long>(day) - block.firstOrdinal;
        if (offset >= 0 && offset / 64 < static_cast<long long>(block.words.size()))
            return (block.words[static_cast<std::size_t>(offset / 64)]
                & (std::uint64_t{1} << (offset % 64))) != 0;
    }
    return false;
}
template<class StatusAt>
std::wstring CalendarCoverageJson(int first, int last, StatusAt statusAt) {
    if (last < first) return L"null";
    std::wstring ranges = L"[";
    int begin = first;
    std::wstring status = statusAt(first);
    for (int day = first + 1; day <= last + 1; ++day) {
        const std::wstring next = day <= last ? statusAt(day) : L"";
        if (next == status) continue;
        if (ranges.size() > 1) ranges += L',';
        ranges += std::format(L"[\"{}\",\"{}\",\"{}\"]",
            CalendarDateText(begin), CalendarDateText(day - 1), status);
        begin = day;
        status = next;
    }
    ranges += L']';
    return std::format(L"{{\"start\":\"{}\",\"end\":\"{}\",\"ranges\":{}}}",
        CalendarDateText(first), CalendarDateText(last), ranges);
}
inline std::wstring CalendarCoverageJson(const CalendarCoverage& value) {
    return CalendarCoverageJson(value.firstOrdinal, value.lastOrdinal,
        [&](int day) -> std::wstring {
            if (value.restrictEligibleDates && !CalendarHasDay(value.eligible, day))
                return L"excluded";
            if (CalendarHasDay(value.unverified, day)) return L"unknown";
            return CalendarHasDay(value.available, day) ? L"available" : L"missing";
        });
}
