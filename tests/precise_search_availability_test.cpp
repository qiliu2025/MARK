#include "AntexAvailability.h"
#include "DateUtil.h"
#include "PreciseSearchAvailability.h"
#include "PreciseProductClassification.h"
#include "DateCoverage.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <iterator>
#include <stop_token>
#include <string>
#include <string_view>

namespace {

template <typename Result>
bool Check(const Result& result, const std::string_view name) {
    if (result.error.empty()) return true;
    const int length = WideCharToMultiByte(CP_UTF8, 0, result.error.c_str(),
        static_cast<int>(result.error.size()), nullptr, 0, nullptr, nullptr);
    std::string message(static_cast<std::size_t>(std::max(0, length)), '\0');
    if (length > 0) WideCharToMultiByte(CP_UTF8, 0, result.error.c_str(),
        static_cast<int>(result.error.size()), message.data(), length,
        nullptr, nullptr);
    std::cerr << "FAILED " << name << ": " << message << '\n';
    return false;
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
    wchar_t configuredIndex[32768]{};
    if (GetEnvironmentVariableW(L"GNSS_DOWNLOADER_INDEX_DIR",
                                configuredIndex,
                                static_cast<DWORD>(std::size(configuredIndex))) == 0) {
        std::cerr << "Set GNSS_DOWNLOADER_INDEX_DIR to the test knowledge-base directory.\n";
        return 2;
    }
    if (argc > 1 && std::wstring_view(argv[1]) == L"--history-scope") {
        const auto first = MakeQueryDate(1970, 1, 1);
        const auto last = MakeQueryDate(2026, 8, 1);
        for (const int category : {1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12}) {
            const auto range = LoadPreciseSearchFamilyRange(category, first, last);
            if (!Check(range, "history scope") || !range.hasHistoryStart) return 10;
            const auto& start = range.historyStart;
            const int startOrdinal = CalendarOrdinal(start.year, start.month, start.day);
            const auto prefixDays = startOrdinal - CalendarOrdinal(1970, 1, 1);
            if (range.beforeHistoryDates.size() != static_cast<std::size_t>(prefixDays)
                || range.complete) return 11;
            for (const auto& day : range.incompleteDates)
                if (CalendarOrdinal(day.year, day.month, day.day) < startOrdinal) return 12;
            std::cout << "Category " << category << ": " << prefixDays
                      << " dates before historical scope; " << range.incompleteDates.size()
                      << " actual incomplete/unscanned dates retained\n";
        }
        const auto incomplete = LoadPreciseSearchFamilyRange(1,
            MakeQueryDate(2026, 7, 26), MakeQueryDate(2026, 7, 31));
        if (incomplete.incompleteDates.size() != 6 || incomplete.complete
            || !incomplete.beforeHistoryDates.empty()) return 13;
        PreciseProductFile product;
        product.category = PreciseProductCategory::Sinex;
        product.latency = PreciseProductLatency::Final;
        product.constellation = L"SOL / DAILY";
        if (!PreciseProductMatchesLevel(product, 7) || PreciseProductMatchesLevel(product, 8)
            || !PreciseProductMatchesLevel(product, 1)
            || PreciseProductSolutionClass(product) != L"DAILY") return 14;
        product.constellation = L"CRD / WEEKLY";
        if (!PreciseProductMatchesLevel(product, 8) || PreciseProductMatchesLevel(product, 7)) return 15;
        product.constellation = L"CRD / CUMULATIVE_LEGACY";
        if (!PreciseProductMatchesLevel(product, 11)) return 16;
        product.constellation = L"SOL / MULTI_DAY";
        if (!PreciseProductMatchesLevel(product, 12)) return 17;
        product.category = PreciseProductCategory::Orbit;
        if (PreciseProductMatchesLevel(product, 11) || !PreciseProductMatchesLevel(product, 1)) return 18;
        std::cout << "PASS: historical scope for all 11 indexed categories, genuine scan gaps, "
                     "SINEX periods and independent publication latency\n";
        return 0;
    }

    if (argc > 1 && std::wstring_view(argv[1]) == L"--wum-2025") {
        const auto first = MakeQueryDate(2025, 1, 24);
        const auto last = MakeQueryDate(2025, 1, 31);
        for (const int category : {1, 2, 5}) {
            const auto range = LoadPreciseSearchFamilyRange(
                category, first, last);
            if (!Check(range, "WUM product family range")) return 6;
            const wchar_t* interval = category == 1 ? L"05M" : L"30S";
            bool found = false;
            for (const auto& family : range.families) {
                if (family.series != "WUM0MGXFIN"
                    || family.availableDays != 8
                    || family.representative.interval != interval) continue;
                const auto routes = LoadPreciseSearchFamilyRoutes(
                    category, family.familyKey, first, last);
                if (!Check(routes, "WUM product download routes")) return 7;
                if (routes.records.size() != 8) return 8;
                for (const auto& day : routes.records) {
                    if (day.mirrors.empty()) return 10;
                }
                std::stop_source stop;
                stop.request_stop();
                const auto cancelled = LoadPreciseSearchFamilyRoutes(
                    category, family.familyKey, first, last,
                    stop.get_token());
                if (cancelled.error.empty()
                    || !cancelled.records.empty()) return 11;
                std::cout << "WUM category " << category
                    << " download routes: 8/8 days; cancellation OK\n";
                found = true;
                break;
            }
            if (!found) {
                std::cerr << "FAILED WUM 8-day family for category "
                    << category << '\n';
                return 9;
            }
        }
        return 0;
    }
    const auto first = MakeQueryDate(2000, 1, 1);
    const auto last = MakeQueryDate(2024, 12, 31);
    const auto warmDate = MakeQueryDate(2000, 1, 1);

    // Match the application startup worker: pay every file-open/cache-build
    // cost before an interactive query is accepted.
    const auto coldStarted = std::chrono::steady_clock::now();
    bool ok = true;
    for (const int category : {1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12}) {
        ok = Check(LoadPreciseSearchFamilyRange(category, warmDate, warmDate),
                   "compact warm") && ok;
    }
    const auto coldMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - coldStarted).count();
    if (!ok) return 2;

    // Simulate the UI's requested worst case: All Products over 25 years.
    // No authoritative route database may be opened in this timed section.
    const auto queryStarted = std::chrono::steady_clock::now();
    std::size_t familyCount = 0;
    const auto antex = LoadAntexAvailabilityRange(first, last);
    ok = Check(antex, "ANTEX");
    familyCount += antex.products.size();
    for (const int category : {1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12}) {
        const auto lookup = LoadPreciseSearchFamilyRange(category, first, last);
        ok = Check(lookup, "compact range") && ok;
        if (lookup.expectedDays != 9132) {
            std::cerr << "FAILED expected day count for category " << category
                      << ": " << lookup.expectedDays << '\n';
            ok = false;
        }
        familyCount += lookup.families.size();
    }
    const auto queryMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - queryStarted).count();
    if (!ok || familyCount == 0) return 3;

    std::cout << "Cold index preparation: " << coldMs << " ms\n"
              << "All-products 9132-day query: " << queryMs << " ms, "
              << familyCount << " families\n";
    if (queryMs >= 250) {
        std::cerr << "FAILED interactive query budget (250 ms)\n";
        return 4;
    }

    // Physical routes are intentionally hydrated only after the user starts
    // a download.  Verify that split independently from query latency.
    for (const int category : {1, 2, 3, 4, 5, 6, 7, 8, 10, 11}) {
        const auto lookup = LoadPreciseSearchFamilyRange(category, first, last);
        if (lookup.families.empty()) continue;
        const auto routes = LoadPreciseSearchFamilyRoutes(
            category, lookup.families.front().familyKey, first, last);
        if (!Check(routes, "lazy routes") || routes.records.empty()) return 5;
    }
    std::cout << "Lazy download-route hydration OK\n";
    return 0;
}
