#pragma once
#include "PreciseProducts.h"
#include <cwctype>
#include <limits>
#include <optional>
#include <string_view>

// Sampling tokens are positive durations, never file-format versions or the
// IGS unknown-rate marker 00U. Integer microseconds make equivalent units
// compare exactly (05M == 300S) without admitting nearby sampling rates.
inline std::optional<std::uint64_t> PreciseSampleIntervalMicroseconds(
    std::wstring_view value) {
    std::wstring token;
    for (const auto ch : value) {
        if (!std::iswspace(ch)) token += static_cast<wchar_t>(std::towupper(ch));
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t whole = 0, fraction = 0, fractionScale = 1;
    std::size_t offset = 0;
    bool haveDigit = false;
    while (offset < token.size() && token[offset] >= L'0' && token[offset] <= L'9') {
        const auto digit = static_cast<std::uint64_t>(token[offset++] - L'0');
        if (whole > (maximum - digit) / 10) return std::nullopt;
        whole = whole * 10 + digit;
        haveDigit = true;
    }
    if (offset < token.size() && token[offset] == L'.') {
        ++offset;
        const auto fractionStart = offset;
        while (offset < token.size() && token[offset] >= L'0' && token[offset] <= L'9') {
            if (fractionScale >= 1000000) return std::nullopt;
            fraction = fraction * 10 + static_cast<std::uint64_t>(token[offset++] - L'0');
            fractionScale *= 10;
        }
        if (offset == fractionStart) return std::nullopt;
        haveDigit = true;
    }
    if (!haveDigit || offset == token.size()) return std::nullopt;
    const std::wstring_view unit(token.data() + offset, token.size() - offset);
    std::uint64_t unitMicroseconds = 0;
    if (unit == L"C") unitMicroseconds = 10000;
    else if (unit == L"MS") unitMicroseconds = 1000;
    else if (unit == L"S" || unit == L"SEC" || unit == L"SECS"
        || unit == L"SECOND" || unit == L"SECONDS" || unit == L"秒") unitMicroseconds = 1000000;
    else if (unit == L"M" || unit == L"MIN" || unit == L"MINS"
        || unit == L"MINUTE" || unit == L"MINUTES" || unit == L"分钟") unitMicroseconds = 60000000;
    else if (unit == L"H" || unit == L"HR" || unit == L"HRS"
        || unit == L"HOUR" || unit == L"HOURS" || unit == L"小时") unitMicroseconds = 3600000000ULL;
    else if (unit == L"D" || unit == L"DAY" || unit == L"DAYS"
        || unit == L"天" || unit == L"日") unitMicroseconds = 86400000000ULL;
    else if (unit == L"W" || unit == L"WEEK" || unit == L"WEEKS") unitMicroseconds = 604800000000ULL;
    else return std::nullopt;
    if (whole > maximum / unitMicroseconds
        || fraction > maximum / unitMicroseconds) return std::nullopt;
    const auto fractionalProduct = fraction * unitMicroseconds;
    if (fractionalProduct % fractionScale != 0) return std::nullopt;
    const auto fractionalMicroseconds = fractionalProduct / fractionScale;
    const auto wholeMicroseconds = whole * unitMicroseconds;
    if (wholeMicroseconds > maximum - fractionalMicroseconds) return std::nullopt;
    const auto result = wholeMicroseconds + fractionalMicroseconds;
    return result == 0 ? std::nullopt : std::optional<std::uint64_t>{result};
}

inline std::wstring_view PreciseProductSamplingInterval(const PreciseProductFile& product) {
    // These catalogue columns contain ANTEX/RINEX versions or the SINEX
    // solution coverage period. They provide no sampling-rate evidence.
    if (product.category == PreciseProductCategory::Antenna
        || product.category == PreciseProductCategory::BroadcastNavigation
        || product.category == PreciseProductCategory::Sinex) return {};
    // Legacy CODE DCB uses 01M for a monthly bias estimate, not one minute.
    if (product.category == PreciseProductCategory::DifferentialCodeBias
        && (product.id.starts_with(L"P1C1") || product.id.starts_with(L"P1P2")
            || product.id.starts_with(L"P2C2"))) return {};
    return PreciseSampleIntervalMicroseconds(product.interval) ? product.interval : std::wstring_view{};
}

inline bool PreciseProductMatchesInterval(
    const PreciseProductFile& product, std::wstring_view filter) {
    if (filter.empty()) return true;
    const auto requested = PreciseSampleIntervalMicroseconds(filter);
    const auto actual = PreciseSampleIntervalMicroseconds(PreciseProductSamplingInterval(product));
    return requested && actual && *requested == *actual;
}

// Institution aliases are presentation/filter metadata. Indexed product IDs,
// filenames, series and download routes retain their original center codes.
inline std::wstring_view CanonicalPreciseCenterCode(std::wstring_view code) {
    if (code == L"COD") return L"CODE";
    if (code == L"COE") return L"CODE";
    if (code == L"COF") return L"CODE";
    if (code == L"COM") return L"CODE";
    if (code == L"CF") return L"CODE";
    if (code == L"EMX") return L"EMR";
    // NAR weekly SINEX explicitly declares NRCan/GSD and NAREF.
    if (code == L"NAR") return L"EMR";
    if (code == L"ESM") return L"ESA";
    if (code == L"ESR") return L"ESA";
    if (code == L"GBM") return L"GFZ";
    if (code == L"GFM") return L"GFZ";
    if (code == L"GT") return L"GFZ";
    if (code == L"GT1") return L"GFZ";
    if (code == L"IG") return L"IGS";
    if (code == L"IGB") return L"IGS";
    if (code == L"IGC") return L"IGS";
    if (code == L"IGP") return L"IGS";
    if (code == L"IGT") return L"IGS";
    if (code == L"JAX") return L"JAXA";
    if (code == L"JXF") return L"JAXA";
    if (code == L"HUS") return L"HUST";
    if (code == L"SHA") return L"SHAO";
    if (code == L"SHR") return L"SHAO";
    if (code == L"SIH") return L"SIO";
    if (code == L"SIR") return L"SIO";
    if (code == L"SIU") return L"SIO";
    if (code == L"USN") return L"USNO";
    if (code == L"NG1") return L"NGS";
    if (code == L"WUM") return L"WHU";
    if (code == L"OPT") return L"OPTIMAP";
    if (code == L"BK0") return L"BKG";
    if (code == L"GO1") return L"GOP";
    if (code == L"GO4") return L"GOP";
    if (code == L"GO6") return L"GOP";
    if (code == L"MU1") return L"MUT";
    if (code == L"MU2") return L"MUT";
    if (code == L"MU4") return L"MUT";
    if (code == L"LP1") return L"LPT";
    if (code == L"UPX") return L"UPA";
    if (code == L"UTX") return L"CSR";
    if (code == L"TU WIEN") return L"TU Wien";
    return code;
}
inline std::wstring_view PreciseProductDisplayCenter(const PreciseProductFile& product) {
    if (product.category == PreciseProductCategory::Ionosphere) {
        if (product.center == L"CAT") return L"CAS";
        if (product.center == L"CBI") return L"CAS";
        if (product.center == L"CRT") return L"CAS";
        if (product.center == L"GAL") return L"CAS";
        if (product.center == L"IRI") return L"CAS";
        if (product.center == L"CKM") return L"CODE";
        if (product.center == L"E1P") return L"ESA";
        if (product.center == L"E2P") return L"ESA";
        if (product.center == L"I1P") return L"IGS";
        if (product.center == L"I2P") return L"IGS";
        if (product.center == L"U1P") return L"UPC";
        if (product.center == L"U2P") return L"UPC";
        if (product.center == L"UAD") return L"UPC";
        if (product.center == L"UAR") return L"UPC";
        if (product.center == L"UHR") return L"UPC";
        if (product.center == L"UPP") return L"UPC";
        if (product.center == L"UQR") return L"UPC";
        if (product.center == L"USR") return L"UPC";
        // These old GIM series have different providers despite sharing GPS.
        if (product.id.starts_with(L"GPSK")) return L"CAS";
        if (product.id.starts_with(L"GPSG")) return L"CODE";
    }
    // Both archived TRO files explicitly identify DUT / DEOS in their headers.
    if (product.category == PreciseProductCategory::Troposphere
        && (product.center == L"NEW" || product.center == L"OLD")) return L"DEO";
    return CanonicalPreciseCenterCode(product.center);
}
inline std::wstring PreciseCenterUpper(std::wstring_view value) {
    std::wstring result(value);
    for (auto& ch : result) if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
    return result;
}
inline bool PreciseProductMatchesCenter(
    const PreciseProductFile& product, std::wstring_view filter) {
    const auto token = PreciseCenterUpper(filter);
    if (token.empty() || token == L"ALL ANALYSIS CENTERS") return true;
    const auto center = PreciseCenterUpper(PreciseProductDisplayCenter(product));
    if (token == L"OTHER") {
        static constexpr std::wstring_view known[] = {
L"IGS", L"CODE", L"EMR", L"ESA", L"GFZ", L"GRG", L"JPL", L"MIT", L"NGS", L"SIO", L"USNO", L"WHU", L"JGX", L"HUST", L"IERS", L"IAC", L"JAXA", L"SHAO", L"BKG", L"IGN", L"DLR", L"CAS", L"UPC", L"OPTIMAP", L"TU WIEN", L"WCC", L"ASI", L"AUS", L"BEK", L"BEV", L"CSR", L"DEO", L"EUR", L"GOP", L"GSI", L"IGE", L"LPT", L"MUT", L"NCL", L"NKG", L"OLG", L"PDR", L"PGC", L"RGA", L"ROB", L"SGO", L"SUT", L"TUG", L"TUM", L"ULR", L"UPA", L"WUT", L"GIA", L"MCC", L"EPN"};
        for (const auto value : known) if (center == value) return false;
        return true;
    }
    if (token == L"WHU/WUM") return center == L"WHU";
    if (token == L"EMR/NRCAN") return center == L"EMR";
    return center == PreciseCenterUpper(CanonicalPreciseCenterCode(token));
}

// SINEX solution period and publication latency are independent attributes.
// Preserve FINAL/RAPID in the model and the download planner.
inline std::wstring_view PreciseProductSolutionClass(const PreciseProductFile& product) {
    if (product.category != PreciseProductCategory::Sinex) return {};
    const auto slash = product.constellation.find_last_of(L'/');
    auto value = std::wstring_view(product.constellation).substr(
        slash == std::wstring::npos ? 0 : slash + 1);
    while (!value.empty() && value.front() == L' ') value.remove_prefix(1);
    while (!value.empty() && value.back() == L' ') value.remove_suffix(1);
    return value;
}
inline const wchar_t* PreciseProductDisplayLevel(const PreciseProductFile& product) {
    if (product.category != PreciseProductCategory::Sinex)
        return PreciseProductLatencyName(product.latency);
    const auto kind = PreciseProductSolutionClass(product);
    if (kind == L"DAILY") return L"日解";
    if (kind == L"WEEKLY") return L"周解";
    if (kind == L"CUMULATIVE" || kind == L"CUMULATIVE_LEGACY") return L"累计解";
    if (kind == L"MULTI_DAY") return L"多日解";
    return L"其他解";
}
inline bool PreciseProductMatchesLevel(const PreciseProductFile& product, int index) {
    if (index == 0) return true;
    if (product.category == PreciseProductCategory::Sinex) {
        const auto kind = PreciseProductSolutionClass(product);
        if (index == 7) return kind == L"DAILY";
        if (index == 8) return kind == L"WEEKLY";
        if (index == 11) return kind == L"CUMULATIVE" || kind == L"CUMULATIVE_LEGACY";
        if (index == 12) return kind == L"MULTI_DAY";
        if (index == 13) return kind != L"DAILY" && kind != L"WEEKLY"
            && kind != L"CUMULATIVE" && kind != L"CUMULATIVE_LEGACY" && kind != L"MULTI_DAY";
    }
    return index < 11 && static_cast<int>(product.latency) == index - 1;
}
