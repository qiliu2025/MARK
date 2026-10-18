#include "DateUtil.h"

#include <array>
#include <chrono>
#include <iomanip>
#include <sstream>

bool IsLeapYear(const int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DayOfYear(const int year, const int month, const int day) {
    static constexpr std::array<int, 12> daysBeforeMonth{
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return 0;
    }
    return daysBeforeMonth[static_cast<std::size_t>(month - 1)] + day
        + ((month > 2 && IsLeapYear(year)) ? 1 : 0);
}

QueryDate MakeQueryDate(const int year, const int month, const int day) {
    return QueryDate{year, month, day, DayOfYear(year, month, day)};
}

namespace {

std::chrono::sys_days ToSysDays(const QueryDate& date) {
    return std::chrono::sys_days{
        std::chrono::year{date.year}
        / std::chrono::month{static_cast<unsigned>(date.month)}
        / std::chrono::day{static_cast<unsigned>(date.day)}};
}

QueryDate FromSysDays(const std::chrono::sys_days value) {
    const std::chrono::year_month_day date{value};
    return MakeQueryDate(
        static_cast<int>(date.year()),
        static_cast<unsigned>(date.month()),
        static_cast<unsigned>(date.day()));
}

const std::chrono::sys_days kGpsEpoch{
    std::chrono::year{1980} / std::chrono::January / 6};

}  // namespace

QueryDate AddDays(const QueryDate& date, const int days) {
    return FromSysDays(ToSysDays(date) + std::chrono::days{days});
}

int GpsWeekNumber(const QueryDate& date) {
    const auto elapsed = (ToSysDays(date) - kGpsEpoch).count();
    return elapsed >= 0
        ? static_cast<int>(elapsed / 7)
        : static_cast<int>((elapsed - 6) / 7);
}

QueryDate GpsWeekStartForDate(const QueryDate& date) {
    return FromSysDays(kGpsEpoch + std::chrono::weeks{GpsWeekNumber(date)});
}

std::wstring FormatDate(const QueryDate& date) {
    std::wostringstream stream;
    stream << std::setfill(L'0')
           << std::setw(4) << date.year << L'-'
           << std::setw(2) << date.month << L'-'
           << std::setw(2) << date.day;
    return stream.str();
}

std::wstring FormatDoy(const int dayOfYear) {
    std::wostringstream stream;
    stream << std::setfill(L'0') << std::setw(3) << dayOfYear;
    return stream.str();
}

std::wstring BuildBkgDailyDirectoryUrl(const QueryDate& date) {
    std::wostringstream stream;
    stream << L"https://igs.bkg.bund.de/root_ftp/IGS/obs/"
           << date.year << L'/' << FormatDoy(date.dayOfYear) << L'/';
    return stream.str();
}
