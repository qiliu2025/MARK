#pragma once

#include "Models.h"

#include <string>

bool IsLeapYear(int year);
int DayOfYear(int year, int month, int day);
QueryDate MakeQueryDate(int year, int month, int day);
QueryDate AddDays(const QueryDate& date, int days);
int GpsWeekNumber(const QueryDate& date);
QueryDate GpsWeekStartForDate(const QueryDate& date);
std::wstring FormatDate(const QueryDate& date);
std::wstring FormatDoy(int dayOfYear);
std::wstring BuildBkgDailyDirectoryUrl(const QueryDate& date);
