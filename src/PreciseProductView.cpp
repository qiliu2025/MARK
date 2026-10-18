#include "DownloadPlan.h"
#include "DownloadFileValidation.h"
#include "DownloadJournal.h"
#include "VmfDownloadPath.h"
#include "NativeUiLanguage.h"
#include "PreciseProductView.h"
#include "PreciseProductClassification.h"

#include "DateUtil.h"
#include "AntexAvailability.h"
#include "ClkAntexInfo.h"
#include "DownloadFolderState.h"
#include "DownloadSettings.h"
#include "AttAvailability.h"
#include "BroadcastAvailability.h"
#include "ClkAvailability.h"
#include "DcbAvailability.h"
#include "DownloadConcurrency.h"
#include "ErpAvailability.h"
#include "HttpClient.h"
#include "IonexAvailability.h"
#include "OsbAvailability.h"
#include "PreciseBundlePlanner.h"
#include "PreciseProducts.h"
#include "PreciseSearchAvailability.h"
#include "SinexAvailability.h"
#include "Sp3Availability.h"
#include "TroAvailability.h"
#include "VmfAvailability.h"
#include "UpdAvailability.h"

#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <format>
#include <set>
#include <iterator>
#include <memory>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <stop_token>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kClassName[] = L"GNSSDownloaderPreciseProductView";
constexpr UINT WM_PRECISE_PROGRESS = WM_APP + 40;
constexpr UINT WM_PRECISE_FINISHED = WM_APP + 41;
constexpr UINT WM_PRECISE_QUERY_BUSY = WM_APP + 42;
constexpr UINT WM_PRECISE_FOCUS_FIRST = WM_APP + 44;
// Precision catalogue reads can touch several independent SQLite indexes and
// may span years.  Keep the work off the window thread and deliver one owned
// snapshot when the query finishes.
constexpr UINT WM_PRECISE_CATALOG_FINISHED = WM_APP + 46;
constexpr UINT WM_PRECISE_QUICK_BUNDLE_FINISHED = WM_APP + 47;
constexpr UINT WM_PRECISE_RULE_OPTIONS_READY = WM_APP + 48;

// Page colours match the main navigation rail.
constexpr COLORREF kPrecisionBackground = RGB(247, 249, 252);
constexpr COLORREF kPrecisionSurface = RGB(255, 255, 255);
constexpr COLORREF kPrecisionBorder = RGB(220, 227, 235);
constexpr COLORREF kPrecisionAccent = RGB(36, 112, 190);
constexpr COLORREF kPrecisionMuted = RGB(101, 109, 118);
constexpr COLORREF kPrecisionText = RGB(38, 50, 64);

HBRUSH PrecisionBackgroundBrush() {
    static HBRUSH brush = CreateSolidBrush(kPrecisionBackground);
    return brush;
}

HBRUSH PrecisionSurfaceBrush() {
    static HBRUSH brush = CreateSolidBrush(kPrecisionSurface);
    return brush;
}

void FillPrecisionRect(
    const HDC deviceContext,
    const RECT& rectangle,
    const COLORREF colour) {
    const HBRUSH brush = CreateSolidBrush(colour);
    if (brush == nullptr) return;
    FillRect(deviceContext, &rectangle, brush);
    DeleteObject(brush);
}

void DrawPrecisionCard(
    const HDC deviceContext,
    const RECT& rectangle) {
    const HBRUSH brush = CreateSolidBrush(kPrecisionSurface);
    const HPEN pen = CreatePen(PS_SOLID, 1, kPrecisionBorder);
    if (brush != nullptr && pen != nullptr) {
        const HGDIOBJ oldBrush = SelectObject(deviceContext, brush);
        const HGDIOBJ oldPen = SelectObject(deviceContext, pen);
        RoundRect(deviceContext, rectangle.left, rectangle.top,
            rectangle.right, rectangle.bottom, 8, 8);
        SelectObject(deviceContext, oldPen);
        SelectObject(deviceContext, oldBrush);
    }
    if (pen != nullptr) DeleteObject(pen);
    if (brush != nullptr) DeleteObject(brush);
    const RECT accent{
        rectangle.left + 12,
        rectangle.top + 1,
        std::min(rectangle.left + 48, rectangle.right - 12),
        rectangle.top + 4};
    FillPrecisionRect(deviceContext, accent, kPrecisionAccent);
}

void DrawPrecisionCaption(
    const HDC deviceContext,
    const int x,
    const int y,
    const int width,
    const wchar_t* text,
    const HFONT font) {
    if (deviceContext == nullptr || text == nullptr) return;
    const int oldMode = SetBkMode(deviceContext, TRANSPARENT);
    const COLORREF oldColour = SetTextColor(deviceContext, kPrecisionMuted);
    const HGDIOBJ oldFont = font == nullptr
        ? nullptr : SelectObject(deviceContext, font);
    RECT caption{x, y, x + std::max(1, width), y + 20};
    DrawTextW(deviceContext, text, -1, &caption,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    if (oldFont != nullptr) SelectObject(deviceContext, oldFont);
    SetTextColor(deviceContext, oldColour);
    SetBkMode(deviceContext, oldMode);
}

enum ControlId : int {
    IdDate = 2101,
    IdEndDate,
    IdGenerate,
    IdQuickPreset,
    IdQuickApply,
    IdCategory,
    IdLatency,
    IdCenter,
    IdCompleteness,
    IdSearch,
    IdList,
    IdSelectAll,
    IdClear,
    IdFolder,
    IdBrowse,
    IdDownload,
    IdPauseDownload,
    IdProgress,
    IdStatus
};

void DrawPrecisionButton(const DRAWITEMSTRUCT& draw) {
    if (draw.hDC == nullptr || draw.hwndItem == nullptr) return;
    RECT bounds = draw.rcItem;
    InflateRect(&bounds, -1, -1);
    const int id = GetDlgCtrlID(draw.hwndItem);
    const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool primary = id == IdGenerate || id == IdQuickApply
        || id == IdDownload;
    const bool pause = id == IdPauseDownload;
    COLORREF fill = kPrecisionSurface;
    COLORREF border = kPrecisionBorder;
    COLORREF text = kPrecisionText;
    if (primary) {
        fill = kPrecisionAccent;
        border = kPrecisionAccent;
        text = RGB(255, 255, 255);
    } else if (pause) {
        fill = RGB(255, 248, 235);
        border = RGB(225, 181, 96);
        text = RGB(130, 83, 18);
    }
    if (disabled) {
        fill = RGB(235, 239, 244);
        border = RGB(218, 224, 231);
        text = RGB(156, 166, 177);
    } else if (pressed) {
        fill = primary ? RGB(27, 88, 154) : RGB(229, 236, 244);
    }
    const HBRUSH brush = CreateSolidBrush(fill);
    const HPEN pen = CreatePen(PS_SOLID, 1, border);
    if (brush != nullptr && pen != nullptr) {
        const HGDIOBJ oldBrush = SelectObject(draw.hDC, brush);
        const HGDIOBJ oldPen = SelectObject(draw.hDC, pen);
        RoundRect(draw.hDC, bounds.left, bounds.top, bounds.right,
            bounds.bottom, 6, 6);
        SelectObject(draw.hDC, oldPen);
        SelectObject(draw.hDC, oldBrush);
    }
    if (pen != nullptr) DeleteObject(pen);
    if (brush != nullptr) DeleteObject(brush);

    std::wstring label;
    const int length = GetWindowTextLengthW(draw.hwndItem);
    if (length > 0) {
        label.resize(static_cast<std::size_t>(length) + 1U);
        GetWindowTextW(draw.hwndItem, label.data(), length + 1);
        label.resize(static_cast<std::size_t>(length));
    }
    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(draw.hDC, text);
    RECT textRect = bounds;
    if (pressed) OffsetRect(&textRect, 0, 1);
    DrawTextW(draw.hDC, label.c_str(), static_cast<int>(label.size()),
        &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if ((draw.itemState & ODS_FOCUS) != 0 && !disabled) {
        RECT focus = bounds;
        InflateRect(&focus, -4, -4);
        DrawFocusRect(draw.hDC, &focus);
    }
}

constexpr UINT_PTR kPrecisionSearchTimer = 0x6A02;
constexpr UINT_PTR kPrecisionCatalogTimer = 0x6A03;

struct PreciseProgress {
    enum class Phase {
        Manifest,
        Download,
    };

    Phase phase = Phase::Download;
    std::size_t completed = 0;
    std::size_t total = 0;
    std::uint64_t currentBytes = 0;
    std::uint64_t currentTotal = 0;
    std::wstring productId;
    std::wstring message;
};

struct PreciseFinished {
    std::shared_ptr<DownloadJournal> journal;
    std::size_t succeeded = 0;
    std::size_t skipped = 0;
    std::size_t failed = 0;
    // A stop request is a normal user action, not a transfer failure.  Keep
    // it explicit so the UI does not report a partially processed batch as
    // successfully completed (or move the progress bar to 100%).
    bool cancelled = false;
    std::vector<std::wstring> errors;
};

// A product row represents one logical product family over the requested
// period.  Keep the per-day mirrors separately so the downloader can fetch
// every available day without exposing one row per day in the UI.
struct PreciseDayFiles {
    QueryDate date{};
    // For products returned by the high-speed family index, routes are
    // intentionally empty until download starts.  The logical key identifies
    // the authoritative daily row used for lazy route hydration.
    std::string logicalKey;
    std::vector<PreciseProductMirror> mirrors;
};

struct PreciseCoverageRange {
    QueryDate first{};
    QueryDate last{};
};

struct PreciseDayCatalog {
    QueryDate date{};
    std::vector<PreciseProductFile> products;
    bool indexed = false;
    bool complete = false;
    std::wstring error;
};

struct PreciseCatalogResult {
    QueryDate start{};
    QueryDate end{};
    std::vector<PreciseProductFile> products;
    std::vector<std::vector<PreciseDayFiles>> productDays;
    std::vector<std::vector<PreciseCoverageRange>> coverageRanges;
    std::vector<std::string> indexedFamilyKeys;
    std::wstring status;
    bool cancelled = false;
    bool failed = false;
};

struct PreciseQuickBundleResult {
    QueryDate start{};
    QueryDate end{};
    int preset = 0;
    PreciseBundleRequest request;
    PreciseBundleCatalog catalog;
    PreciseBundlePlanResult plan;
    std::vector<std::wstring> warnings;
};

struct SelectedPreciseProduct {
    PreciseProductFile product;
    std::vector<PreciseDayFiles> days;
    std::vector<PreciseCoverageRange> coverageRanges;
    std::string indexedFamilyKey;
    int indexedCategory = 0;
    QueryDate startDate{};
    QueryDate endDate{};
    bool quickPlanRoutesDeferred = false;
};

struct PreciseQueuedDownload {
    std::uint64_t id = 0;
    std::wstring label;
    std::wstring key;
    std::wstring folder;
    std::wstring state = L"pending";
    int percent = 0;
    bool selected = true;
    bool removeAfterCancel = false;
    QueryDate start{};
    QueryDate end{};
    SelectedPreciseProduct payload;
};

struct UpdatePreciseQueueRequest {
    std::uint64_t id = 0;
    std::wstring action;
};

// Payload for the synchronous WebView-to-native product selection bridge.
// The request lives on the caller's stack while SendMessageW dispatches, so
// no ownership transfer or cross-thread allocation is needed.
struct SelectProductRequest {
    std::wstring productId;
    bool selected = false;
};

struct SetPreciseDatesRequest {
    SYSTEMTIME start{};
    SYSTEMTIME end{};
};

struct SetPreciseCenterRequest {
    std::wstring center;
};

struct SetPreciseFamilyRequest {
    std::wstring family;
};

struct SetPreciseCustomPresetRequest {
    std::wstring name;
    std::wstring rules;
};

struct SetPreciseFiltersRequest {
    std::wstring latency;
    // Legacy preset value retained for native callers; the Web shell now
    // also sends an explicit mode/value pair below.
    std::wstring completeness;
    std::wstring troStation;
    std::wstring search;
    std::wstring completenessMode;
    double completenessValue = 0.0;
};

enum class PreciseCompletenessMode {
    Ignore,
    Ratio,
    Days
};

struct PreciseCustomRule {
    int categoryIndex = 0;
    int latencyIndex = 0;
    std::wstring center;
    std::wstring interval;
};

int InclusiveDays(const QueryDate& first, const QueryDate& last);

constexpr int BdsMartOrbitCategoryIndex() {
    return static_cast<int>(PreciseProductCategory::Orbit) + 1;
}

constexpr int BdsMartClockCategoryIndex() {
    return static_cast<int>(PreciseProductCategory::Clock) + 1;
}

constexpr int BdsMartOsbCategoryIndex() {
    return static_cast<int>(PreciseProductCategory::ObservableSpecificBias) + 1;
}

bool IsPreciseSearchSummaryCategory(const int categoryIndex) {
    const int antex = static_cast<int>(PreciseProductCategory::Antenna) + 1;
    return categoryIndex >= 1 && categoryIndex <= 12
        && categoryIndex != antex;
}

std::once_flag gPreciseRangePrewarm;
std::atomic<bool> gPreciseRuleOptionsReady = false;
std::vector<PreciseUiRuleOption> gPreciseRuleOptions;

void InitializePreciseRuleOptions();

void PrewarmPreciseRangeIndexes() {
    std::call_once(gPreciseRangePrewarm, [] {
        const QueryDate warmDate = MakeQueryDate(2000, 1, 1);
        // These readers populate immutable process-wide family caches.  The
        // initialization runs on the catalogue worker, never the UI thread,
        // so the window remains responsive while every later query becomes
        // an in-memory date-bitmap/binary-search operation.
        for (const int category : {1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12}) {
            (void)LoadPreciseSearchFamilyRange(category, warmDate, warmDate);
        }
        InitializePreciseRuleOptions();
    });
}

bool IsBdsMartRtppProduct(
    const PreciseProductFile& existing,
    const PreciseProductFile& candidate) {
    if (existing.id == candidate.id) return true;
    // A scanned RT-PPP row is decoded through the normal SP3/CLK adapter,
    // which gives it a database-style id (CAS1IGSRTS_0000_01D_01M) while the
    // direct fallback uses CAS1IGSRTS_RTPP_ORB.  Match the stable metadata so
    // enabling the scanner never duplicates the same product in the GUI.
    return existing.category == candidate.category
        && existing.center == L"CAS"
        && existing.name.find(L"CAS1IGSRTS") != std::wstring::npos
        && existing.interval == candidate.interval
        && existing.latency == PreciseProductLatency::RealTime;
}

void AppendBdsMartProductsForDate(
    std::vector<PreciseProductFile>& destination,
    const QueryDate& date,
    const int categoryIndex) {
    if (PreciseSearchHasFixedCutoff()) return;
    if (categoryIndex != BdsMartOrbitCategoryIndex()
        && categoryIndex != BdsMartClockCategoryIndex()
        && categoryIndex != BdsMartOsbCategoryIndex()) {
        return;
    }
    for (auto product : BuildBdsMartRtppProducts(date)) {
        if (static_cast<int>(product.category) != categoryIndex - 1) {
            continue;
        }
        const bool duplicate = std::ranges::any_of(destination,
            [&](const PreciseProductFile& existing) {
                return IsBdsMartRtppProduct(existing, product);
            });
        if (!duplicate) {
            destination.push_back(std::move(product));
        }
    }
}

void AppendBdsMartProductsForRange(
    std::vector<PreciseProductFile>& products,
    std::vector<std::vector<PreciseDayFiles>>& productDays,
    std::vector<std::string>& indexedFamilyKeys,
    const QueryDate& start,
    const QueryDate& end,
    const int categoryIndex,
    const int expectedDays) {
    if (PreciseSearchHasFixedCutoff()) return;
    if (categoryIndex != BdsMartOrbitCategoryIndex()
        && categoryIndex != BdsMartClockCategoryIndex()) {
        return;
    }
    QueryDate firstAvailable = start;
    if (firstAvailable.year < 2024) {
        firstAvailable = MakeQueryDate(2024, 1, 1);
    }
    if (firstAvailable.year > end.year
        || (firstAvailable.year == end.year
            && (firstAvailable.month > end.month
                || (firstAvailable.month == end.month
                    && firstAvailable.day > end.day)))) {
        return;
    }

    const auto representatives = BuildBdsMartRtppProducts(firstAvailable);
    for (const auto& representative : representatives) {
        if (static_cast<int>(representative.category) != categoryIndex - 1) {
            continue;
        }
        if (std::ranges::any_of(products,
                [&](const PreciseProductFile& existing) {
                    return IsBdsMartRtppProduct(existing, representative);
                })) {
            continue;
        }
        PreciseProductFile product = representative;
        const int availableDays = InclusiveDays(firstAvailable, end);
        product.availableDays = availableDays;
        product.expectedDays = expectedDays;
        product.completenessPercent = expectedDays == 0 ? 0.0
            : 100.0 * static_cast<double>(availableDays)
                / static_cast<double>(expectedDays);
        product.status = L"BDSmart RT-PPP 直链；按日文件可用性待下载时确认";

        std::vector<PreciseDayFiles> days;
        days.reserve(static_cast<std::size_t>(availableDays));
        for (QueryDate day = firstAvailable;; day = AddDays(day, 1)) {
            const auto datedProducts = BuildBdsMartRtppProducts(day);
            const auto found = std::ranges::find_if(
                datedProducts,
                [&](const PreciseProductFile& candidate) {
                    return candidate.id == product.id;
                });
            if (found != datedProducts.end()) {
                days.push_back({day, {}, found->mirrors});
            }
            if (day.year == end.year && day.month == end.month
                && day.day == end.day) {
                break;
            }
        }
        products.push_back(std::move(product));
        productDays.push_back(std::move(days));
        indexedFamilyKeys.emplace_back();
    }
}

int InclusiveDays(const QueryDate& first, const QueryDate& last) {
    using namespace std::chrono;
    const sys_days a = year{first.year}
        / month{static_cast<unsigned>(first.month)}
        / day{static_cast<unsigned>(first.day)};
    const sys_days b = year{last.year}
        / month{static_cast<unsigned>(last.month)}
        / day{static_cast<unsigned>(last.day)};
    const auto count = duration_cast<days>(b - a).count();
    if (count < 0 || count > static_cast<long long>(std::numeric_limits<int>::max() - 1)) {
        return 0;
    }
    return static_cast<int>(count) + 1;
}

std::chrono::sys_days SysDay(const QueryDate& date) {
    return std::chrono::sys_days{
        std::chrono::year{date.year}
            / std::chrono::month{static_cast<unsigned>(date.month)}
            / std::chrono::day{static_cast<unsigned>(date.day)}};
}

std::wstring ProductLogicalKey(const PreciseProductFile& product) {
    // IDs for SINEX/TRO/DCB contain nominal dates.  The descriptive fields
    // are stable across days and therefore form the appropriate range key.
    std::wstring key = std::to_wstring(static_cast<int>(product.category)) + L"|"
        + std::to_wstring(static_cast<int>(product.latency)) + L"|"
        + product.name + L"|" + product.center + L"|"
        + product.constellation + L"|" + product.interval;
    // Most daily archives expose a date-independent product ID that also
    // carries duration/start-time variants (for example 01D vs 02H SP3).
    // SINEX/TRO/DCB IDs intentionally contain nominal/coverage dates and are
    // therefore grouped by their descriptive fields above.
    if (product.category != PreciseProductCategory::Sinex
        && product.category != PreciseProductCategory::Troposphere
        && product.category != PreciseProductCategory::DifferentialCodeBias) {
        key += L"|" + product.id;
    }
    return key;
}

void MergeMirrors(
    std::vector<PreciseProductMirror>& destination,
    const std::vector<PreciseProductMirror>& source) {
    for (const auto& mirror : source) {
        const auto duplicate = std::ranges::find_if(
            destination,
            [&](const PreciseProductMirror& existing) {
                return existing.url == mirror.url;
            });
        if (duplicate == destination.end()) {
            destination.push_back(mirror);
        }
    }
}

// A range result retains every dated mirror in productDays_ for downloads,
// while the display row only needs one representative route per source.
// Keeping the display copy small is important for multi-decade SP3 queries.
std::vector<PreciseProductMirror> RepresentativeMirrors(
    const std::vector<PreciseProductMirror>& mirrors) {
    std::vector<PreciseProductMirror> result;
    result.reserve(mirrors.size());
    for (const auto& mirror : mirrors) {
        const auto duplicateSource = std::ranges::find_if(
            result, [&](const PreciseProductMirror& existing) {
                return existing.source == mirror.source;
            });
        if (duplicateSource == result.end()) result.push_back(mirror);
    }
    return result;
}

std::filesystem::path DefaultProductFolder() {
    PWSTR downloads = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(
            FOLDERID_Downloads,
            KF_FLAG_DEFAULT,
            nullptr,
            &downloads))) {
        const std::filesystem::path path =
            std::filesystem::path(downloads) / L"GNSSDownloader" / L"products";
        CoTaskMemFree(downloads);
        return path;
    }
    wchar_t current[MAX_PATH]{};
    GetCurrentDirectoryW(static_cast<DWORD>(std::size(current)), current);
    return std::filesystem::path(current) / L"GNSSDownloader" / L"products";
}

void SetRecentDate(const HWND picker) {
    using namespace std::chrono;
    const auto recent = floor<days>(system_clock::now()) - days{2};
    const year_month_day value{recent};
    SYSTEMTIME date{};
    date.wYear = static_cast<WORD>(static_cast<int>(value.year()));
    date.wMonth = static_cast<WORD>(static_cast<unsigned>(value.month()));
    date.wDay = static_cast<WORD>(static_cast<unsigned>(value.day()));
    DateTime_SetSystemtime(picker, GDT_VALID, &date);
}

std::wstring WindowText(const HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring result(static_cast<std::size_t>(std::max(0, length)), L'\0');
    if (length > 0) {
        GetWindowTextW(window, result.data(), length + 1);
    }
    return result;
}

std::wstring Lower(std::wstring value) {
    std::ranges::transform(value, value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

int CustomCategoryIndex(const std::wstring_view raw) {
    const auto value = Lower(std::wstring(raw));
    if (value == L"sp3") return 1;
    if (value == L"clk") return 2;
    if (value == L"erp/eop") return 3;
    if (value == L"dcb") return 4;
    if (value == L"orbex") return 5;
    if (value == L"ionex") return 6;
    if (value == L"tro") return 7;
    if (value == L"sinex") return 8;
    if (value == L"antex") return 9;
    if (value == L"broadcast") return 10;
    if (value == L"osb") return 11;
    if (value == L"upd") return 12;
    if (value == L"vmf") return 13;
    return 0;
}

int CustomLatencyIndex(const std::wstring_view raw) {
    const auto value = Lower(std::wstring(raw));
    constexpr std::array<std::wstring_view, 14> values{
        L"all", L"final", L"rapid", L"ultra-rapid", L"near real-time",
        L"real-time", L"predicted", L"daily", L"weekly", L"static", L"other",
        L"cumulative", L"multi-day", L"solution-other"};
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (value == values[index]) return static_cast<int>(index);
    }
    return 0;
}

// The Web custom-plan editor uses the human-facing analysis-centre labels
// (for example CODE and USNO), while the compact product indexes retain the
// legacy three-letter centre tokens (COD and USN).  Keep this translation in
// one place so a custom rule behaves exactly like the native centre filter.
std::wstring PreciseCenterFilterToken(std::wstring value) {
    value = Lower(std::move(value));
    if (value == L"all analysis centers") return {};
    if (value == L"code") return L"cod";
    if (value == L"emr/nrcan") return L"emr";
    if (value == L"usno") return L"usn";
    if (value == L"hust") return L"hus";
    if (value == L"jaxa") return L"jax";
    if (value == L"shao") return L"sha";
    if (value == L"optimap") return L"opt";
    return value;
}

// Product indexes are local inputs (the UPD index in particular can be
// produced by an external builder).  A route is expected to contain a plain
// archive basename, never a path.  Rejecting separators, roots, ADS syntax
// and control characters keeps a malformed index from escaping the selected
// product directory when it is handed to the transfer worker.
bool IsSafeProductFileName(const std::wstring_view value) {
    if (value.empty() || value == L"." || value == L"..") {
        return false;
    }
    const std::filesystem::path path(value);
    if (path.has_root_name() || path.has_root_directory() || path.is_absolute()
        || path.filename() != path) {
        return false;
    }
    for (const wchar_t character : value) {
        if (character < 0x20 || character == L':' || character == L'/'
            || character == L'\\') {
            return false;
        }
    }
    return true;
}

std::wstring Utf8ToWide(const std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), size);
    return result;
}

// A background catalogue/download task must never let an allocation, a
// malformed index row, or a filesystem/formatting exception escape its
// thread entry point.  Escaping a std::jthread function calls std::terminate
// and looks like a random GUI crash to the user.  Keep the diagnostic helper
// deliberately defensive: the fallback text is enough to unblock the UI even
// if converting std::exception::what() itself fails.
std::wstring DescribeCurrentException() {
    try {
        throw;
    } catch (const std::exception& error) {
        try {
            const auto detail = Utf8ToWide(error.what());
            if (!detail.empty()) {
                return L"后台任务异常：" + detail;
            }
        } catch (...) {
        }
        return L"后台任务异常（标准库异常）。";
    } catch (...) {
        return L"后台任务异常（未知异常）。";
    }
}

void AppendFinishedError(PreciseFinished& finished, std::wstring message) {
    try {
        // The UI only displays the first few details.  Keep a bounded sample
        // so a large failed manifest cannot retain one string per task.
        constexpr std::size_t kMaximumErrorDetails = 256;
        if (finished.errors.size() >= kMaximumErrorDetails) {
            return;
        }
        finished.errors.push_back(std::move(message));
    } catch (...) {
        // The counters and completion message remain useful even when the
        // process is under memory pressure and the detail vector cannot grow.
    }
}

std::string WideToUtf8(const std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        // A few legacy index labels contain an unpaired surrogate.  Keep the
        // adapter lossless for normal UTF-8 while still allowing those rows
        // to be shown and planned.
        const int fallback = WideCharToMultiByte(
            CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
            nullptr, 0, nullptr, nullptr);
        if (fallback <= 0) {
            return {};
        }
        std::string result(static_cast<std::size_t>(fallback), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
            result.data(), fallback, nullptr, nullptr);
        return result;
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

DownloadRoute ToBundleRoute(const Sp3ProductRoute& route) {
    return {route.sourceKey, route.fileName, route.downloadUrl};
}

DownloadRoute ToBundleRoute(const ErpProductRoute& route) {
    return {route.sourceKey, route.fileName, route.downloadUrl};
}

DownloadRoute ToBundleRoute(const OsbProductRoute& route) {
    return {route.sourceKey, route.fileName, route.downloadUrl};
}

DownloadRoute ToBundleRoute(const AntexProductRoute& route) {
    return {route.sourceKey, route.fileName, route.downloadUrl};
}

DownloadRoute ToBundleRoute(const UpdProductRoute& route) {
    return {WideToUtf8(route.source), WideToUtf8(route.fileName), route.url};
}

template <typename Route>
std::vector<DownloadRoute> ToBundleRoutes(const std::vector<Route>& routes) {
    std::vector<DownloadRoute> result;
    result.reserve(routes.size());
    std::unordered_set<std::wstring> seen;
    for (const auto& route : routes) {
        if (route.fileName.empty() || route.downloadUrl.empty()) {
            continue;
        }
        if (seen.insert(route.downloadUrl).second) {
            result.push_back(ToBundleRoute(route));
        }
    }
    return result;
}

template <>
std::vector<DownloadRoute> ToBundleRoutes(
    const std::vector<UpdProductRoute>& routes) {
    std::vector<DownloadRoute> result;
    result.reserve(routes.size());
    std::unordered_set<std::wstring> seen;
    for (const auto& route : routes) {
        if (route.fileName.empty() || route.url.empty()) {
            continue;
        }
        if (seen.insert(route.url).second) {
            result.push_back(ToBundleRoute(route));
        }
    }
    return result;
}

std::string CanonicalBundleCenter(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    if (value == "COD") {
        return "CODE";
    }
    if (value == "WHU") {
        return "WUM";
    }
    return value;
}

std::string FirstRouteFileName(const std::vector<DownloadRoute>& routes) {
    return routes.empty() ? std::string{} : routes.front().fileName;
}

PreciseBundleProduct ToBundleProduct(
    const Sp3ProductRecord& record,
    const PreciseBundleProductType type) {
    PreciseBundleProduct product;
    product.type = type;
    product.logicalKey = record.logicalKey;
    product.displayId = Utf8ToWide(record.series)
        + std::format(L"_{:02}{:02}_{}_{}", record.startHour,
            record.startMinute, Utf8ToWide(record.duration),
            Utf8ToWide(record.interval));
    product.displayName = type == PreciseBundleProductType::Att
        ? Utf8ToWide(record.series) + L" ORBEX 姿态产品"
        : Utf8ToWide(record.series)
            + (type == PreciseBundleProductType::Clk
                ? L" CLK 精密钟差" : L" SP3 精密轨道");
    product.nominalDate = record.nominalDate;
    product.coverageStart = record.nominalDate;
    product.coverageEnd = record.nominalDate;
    product.center = CanonicalBundleCenter(record.center);
    product.series = record.series;
    product.productLine = record.project;
    product.latency = record.latency;
    product.constellation = record.constellation;
    product.interval = record.interval;
    product.routes = ToBundleRoutes(record.routes);
    product.fileName = FirstRouteFileName(product.routes);
    return product;
}

PreciseBundleProduct ToBundleProduct(const ErpProductRecord& record) {
    PreciseBundleProduct product;
    product.type = PreciseBundleProductType::Erp;
    product.logicalKey = record.logicalKey;
    product.displayId = Utf8ToWide(record.series)
        + std::format(L"_{:02}{:02}_{}_{}", record.startHour,
            record.startMinute, Utf8ToWide(record.duration),
            Utf8ToWide(record.standard));
    product.displayName = (record.center == "COD"
            ? L"CODE" : Utf8ToWide(record.center))
        + L" " + Utf8ToWide(record.series)
        + (record.productFamily == "EOP"
            ? L" 地球定向参数（EOP）" : L" 地球旋转参数（ERP）");
    product.nominalDate = record.nominalDate;
    product.coverageStart = record.coverageStart;
    product.coverageEnd = record.coverageEnd;
    product.center = CanonicalBundleCenter(record.center);
    product.series = record.series;
    product.productLine = record.project;
    product.latency = record.latency;
    product.interval = record.interval;
    // ERP/EOP rows do not describe a GNSS constellation.  Keep the field
    // empty so the planner does not confuse an EOP standard with a satellite
    // system; the shared series/centre/latency fields still enforce pairing.
    product.routes = ToBundleRoutes(record.routes);
    product.fileName = FirstRouteFileName(product.routes);
    return product;
}

PreciseBundleProduct ToBundleProduct(const OsbProductRecord& record) {
    PreciseBundleProduct product;
    product.type = PreciseBundleProductType::Osb;
    product.logicalKey = record.logicalKey;
    product.displayId = Utf8ToWide(record.series)
        + std::format(L"_{:02}{:02}_{}_{}", record.startHour,
            record.startMinute, Utf8ToWide(record.duration),
            Utf8ToWide(record.interval));
    product.displayName = (record.center == "COD"
            ? L"CODE" : Utf8ToWide(record.center))
        + L" " + Utf8ToWide(record.series)
        + L" OSB（" + Utf8ToWide(record.biasClass) + L"）";
    product.nominalDate = record.coverageStart;
    product.coverageStart = record.coverageStart;
    product.coverageEnd = record.coverageEnd;
    product.center = CanonicalBundleCenter(record.center);
    product.series = record.series;
    product.productLine = record.project;
    product.latency = record.latency;
    product.constellation = record.constellation;
    product.interval = record.interval;
    // Keep the two dimensions inside one canonical field without using '|',
    // which is the planner's outer family-key separator.  A colon survives
    // NormalizeBundleToken and keeps CODE_PHASE vs Bias-SINEX provenance
    // auditable in JSON/CSV manifests.
    product.biasConvention = record.biasClass;
    if (!record.format.empty()) {
        if (!product.biasConvention.empty()) product.biasConvention += ":";
        product.biasConvention += record.format;
    }
    // The named WUM/PRIDE MGX quick plan is explicitly the all-frequency
    // phase-bias stream.  The legacy OSB index has no separate signal-pair
    // column, so record that well-defined product-line fact rather than
    // treating a missing frequency field as a silent wildcard.
    if (NormalizeBundleToken(product.center) == "WUM"
        && NormalizeBundleToken(product.productLine) == "MGX"
        && NormalizeBundleToken(record.biasClass).find("PHASE")
            != std::string::npos) {
        product.referenceFrequency = "ALL_FREQUENCY";
    }
    product.routes = ToBundleRoutes(record.routes);
    product.fileName = FirstRouteFileName(product.routes);
    return product;
}

PreciseBundleProduct ToBundleProduct(const UpdProductRecord& record) {
    PreciseBundleProduct product;
    product.type = PreciseBundleProductType::Upd;
    product.logicalKey = WideToUtf8(record.logicalKey);
    product.displayId = record.logicalKey;
    product.displayName = L"iGMAS " + record.variant + L" " + record.updClass
        + L" UPD（" + record.frequencyCombination + L"）";
    product.nominalDate = record.nominalDate;
    product.coverageStart = record.nominalDate;
    product.coverageEnd = record.nominalDate;
    // The current iGMAS index identifies the producing service rather than
    // an IGS analysis-centre prefix.  Do not invent a WUM/CODE centre: leave
    // it unknown and let strict mode surface the warning in the report.
    // ``variant`` (UC/IF, WL/NL, ...) describes the bias solution, not the
    // SP3/CLK analysis-centre series.  Treating it as ``series`` would make
    // every otherwise usable UPD row look incompatible with an anchor such
    // as WUM0MGXFIN.  Keep the variant in the bias convention below and leave
    // producer-family fields unknown; the planner will surface that missing
    // provenance as a strict-mode warning instead of inventing a centre.
    product.series = {};
    product.productLine = {};
    product.latency = {};
    product.constellation = WideToUtf8(record.constellation);
    product.interval = WideToUtf8(record.interval);
    product.referenceFrequency = WideToUtf8(record.frequencyCombination);
    product.biasConvention = WideToUtf8(record.updClass);
    const auto variant = WideToUtf8(record.variant);
    if (!variant.empty()) {
        if (!product.biasConvention.empty()) product.biasConvention += ":";
        product.biasConvention += variant;
    }
    product.routes = ToBundleRoutes(record.routes);
    product.fileName = FirstRouteFileName(product.routes);
    return product;
}

PreciseBundleProduct ToBundleProduct(const AntexProductRecord& record) {
    PreciseBundleProduct product;
    product.type = PreciseBundleProductType::Antex;
    product.logicalKey = record.logicalKey;
    product.displayId = Utf8ToWide(record.modelName) + L"_"
        + Utf8ToWide(record.variant) + L"_"
        + (record.currentAlias ? L"CURRENT"
            : std::to_wstring(record.revisionWeek));
    product.displayName = Utf8ToWide(record.producer) + L" "
        + Utf8ToWide(record.modelName) + L" 天线校准模型（"
        + Utf8ToWide(record.scope) + L"）";
    product.nominalDate = record.validFrom;
    product.coverageStart = record.validFrom;
    product.coverageEnd = record.validTo;
    // ANTEX is a calibration catalogue, not an analysis-centre product.  Its
    // compatibility dimensions are represented by the reference-frame/APC
    // fields; centre/series/latency remain wildcards by design.
    product.interval = record.formatVersion;
    product.referenceFrame = record.referenceFrame;
    product.apcModel = record.calibrationType;
    product.revisionWeek = record.revisionWeek;
    product.currentAlias = record.currentAlias;
    product.routes = ToBundleRoutes(record.routes);
    product.fileName = FirstRouteFileName(product.routes);
    return product;
}

std::string BundleLatencyToken(const PreciseProductLatency latency) {
    switch (latency) {
    case PreciseProductLatency::Final: return "FINAL";
    case PreciseProductLatency::Rapid: return "RAPID";
    case PreciseProductLatency::UltraRapid: return "ULTRA_RAPID";
    case PreciseProductLatency::NearRealTime: return "NEAR_REAL_TIME";
    case PreciseProductLatency::RealTime: return "REAL_TIME";
    case PreciseProductLatency::Predicted: return "PREDICTED";
    case PreciseProductLatency::Daily: return "DAILY";
    case PreciseProductLatency::Weekly: return "WEEKLY";
    case PreciseProductLatency::Static: return "STATIC";
    default: return {};
    }
}

int BundleDateCode(const QueryDate& date) {
    return date.year * 10000 + date.month * 100 + date.day;
}

bool BundleFamilyMayMatchRequest(
    const PreciseBundleProduct& product,
    const PreciseBundleRequest& request);

PreciseBundleProduct ToCompactBundleProduct(
    const PreciseSearchFamilyRecord& family,
    const PreciseBundleProductType type,
    const QueryDate& first,
    const QueryDate& last) {
    PreciseBundleProduct product;
    product.type = type;
    product.logicalKey = family.familyKey;
    product.displayId = family.representative.id;
    product.displayName = family.representative.name;
    product.nominalDate = first;
    product.coverageStart = first;
    product.coverageEnd = last;
    product.fileName = "(deferred)";
    product.routesDeferred = true;
    product.center = WideToUtf8(family.representative.center);
    product.series = family.series;
    product.productLine = family.productLine;
    product.latency = BundleLatencyToken(family.representative.latency);
    product.constellation = WideToUtf8(family.representative.constellation);
    // The planner's existing BRDC field identifies its RINEX format; keep
    // that internal contract separate from the UI sampling column.
    product.interval = WideToUtf8(type == PreciseBundleProductType::Brdc
        ? family.representative.formatVersion : family.representative.interval);
    product.referenceFrequency = family.referenceFrequency;
    product.biasConvention = family.biasConvention;
    product.referenceFrame = family.referenceFrame;
    product.apcModel = family.apcModel;
    product.revisionWeek = family.revisionWeek;
    product.currentAlias = family.currentAlias;
    return product;
}

void AppendCompactBundleFamilyRuns(
    PreciseBundleCatalog& catalog,
    const PreciseSearchRangeLookup& range,
    const int categoryIndex,
    const PreciseBundleProductType type,
    const PreciseBundleRequest& request) {
    for (const auto& family : range.families) {
        if (family.availableDates.empty()) continue;
        auto representative = ToCompactBundleProduct(
            family, type, family.availableDates.front(),
            family.availableDates.front());
        if (!BundleFamilyMayMatchRequest(representative, request)) continue;
        std::size_t runStart = 0;
        for (std::size_t index = 1; index <= family.availableDates.size(); ++index) {
            const bool endOfRun = index == family.availableDates.size()
                || BundleDateCode(AddDays(family.availableDates[index - 1], 1))
                    != BundleDateCode(family.availableDates[index]);
            if (!endOfRun) continue;
            auto product = ToCompactBundleProduct(
                family, type, family.availableDates[runStart],
                family.availableDates[index - 1]);
            catalog.products.push_back(std::move(product));
            runStart = index;
        }
    }
    (void)categoryIndex;
}

bool IsQuickBundleAntexCandidate(const AntexProductRecord& record) {
    // Quick PPP plans need the date-effective global IGS calibration that
    // covers both satellites and receivers.  The ANTEX catalogue also holds
    // regional receiver-only supplements, split SAT/RCV files and AC-local
    // variants; selecting one of those merely because it has more mirrors is
    // not a valid replacement for the global APC/PCV model.
    return NormalizeBundleToken(record.producer) == "IGS"
        && NormalizeBundleToken(record.productFamily) == "IGS_GLOBAL"
        && NormalizeBundleToken(record.scope) == "SATELLITE_RECEIVER"
        && NormalizeBundleToken(record.variant) == "STANDARD";
}

// Quickly discard unrelated compact-family summaries before hydrating their
// daily routes.  This is deliberately a conservative pre-filter: fields that
// are absent in an index are retained and the authoritative planner performs
// the final strict compatibility check.  Keeping the check here avoids
// expanding every analysis centre's multi-year route graph just to select a
// named WUM or IGS preset.
bool BundleFamilyMayMatchRequest(
    const PreciseBundleProduct& product,
    const PreciseBundleRequest& request) {
    if (request.plan == PreciseBundlePlan::WumPridePppAr
        && product.type == PreciseBundleProductType::Erp) {
        return NormalizeBundleToken(product.center) == "WUM"
            && NormalizeBundleToken(product.series) == "WUM0MGXRAP"
            && NormalizeBundleToken(product.productLine) == "MGX"
            && NormalizeBundleToken(product.latency) == "RAPID";
    }
    if (request.plan == PreciseBundlePlan::WumPridePppAr
        && product.type >= PreciseBundleProductType::SinexCrd
        && product.type <= PreciseBundleProductType::Brdc) {
        const auto series = NormalizeBundleToken(product.series);
        const auto center = NormalizeBundleToken(product.center);
        const auto line = NormalizeBundleToken(product.productLine);
        const auto content = NormalizeBundleToken(product.constellation);
        if (product.type == PreciseBundleProductType::SinexCrd
            || product.type == PreciseBundleProductType::SinexSol) {
            return center == "IGS" && series == "IGS0OPSSNX"
                && line == "DAILY"
                && content == (product.type == PreciseBundleProductType::SinexCrd
                    ? "CRD/DAILY" : "SOL/DAILY");
        }
        if (product.type == PreciseBundleProductType::Gim)
            return center == "CODE" && series == "CODE0OPSFIN";
        if (product.type == PreciseBundleProductType::Brdc)
            return center == "IGS" && series == "BRDC00IGS"
                && line == "MN"
                && NormalizeBundleToken(product.interval) == "RINEX3";
        return false; // VMF families are loaded from their own indexed DB.
    }
    if (product.type != PreciseBundleProductType::Antex
        && product.type != PreciseBundleProductType::Erp) {
        if (!request.center.empty()
            && NormalizeBundleToken(product.center)
                != NormalizeBundleToken(request.center)) {
            return false;
        }
        if (!request.series.empty()
            && NormalizeBundleToken(product.series)
                != NormalizeBundleToken(request.series)) {
            return false;
        }
        if (!request.productLine.empty()
            && NormalizeBundleToken(product.productLine)
                != NormalizeBundleToken(request.productLine)) {
            return false;
        }
    }

    if (product.type == PreciseBundleProductType::Sp3
        || product.type == PreciseBundleProductType::Clk
        || product.type == PreciseBundleProductType::Att
        || product.type == PreciseBundleProductType::Osb
        || product.type == PreciseBundleProductType::Erp) {
        const auto level = NormalizeBundleToken(product.latency);
        if (request.latency == PreciseBundleLatency::Automatic) {
            // A compact family with an explicit non-Final/Rapid level can
            // never satisfy the first-release quick plans.  Unknown labels
            // remain candidates so the planner can report them as warnings.
            if (!level.empty() && level != "FINAL" && level != "RAPID") {
                return false;
            }
        } else {
            const auto expected = request.latency == PreciseBundleLatency::Final
                ? std::string("FINAL") : std::string("RAPID");
            if (!level.empty() && level != expected) return false;
        }
    }

    std::string requiredInterval;
    if (product.type == PreciseBundleProductType::Sp3) {
        requiredInterval = request.orbitInterval;
    } else if (product.type == PreciseBundleProductType::Clk) {
        requiredInterval = request.clockInterval;
        if (requiredInterval.empty()
            && (request.plan == PreciseBundlePlan::WumPridePppAr
                || request.plan == PreciseBundlePlan::StandardPpp)) {
            requiredInterval = "30S";
        }
    }
    if (!requiredInterval.empty() && !product.interval.empty()
        && NormalizeBundleToken(product.interval)
            != NormalizeBundleToken(requiredInterval)) {
        return false;
    }
    return true;
}

PreciseProductCategory ToUiCategory(const PreciseBundleProductType type) {
    switch (type) {
    case PreciseBundleProductType::Sp3:
        return PreciseProductCategory::Orbit;
    case PreciseBundleProductType::Clk:
        return PreciseProductCategory::Clock;
    case PreciseBundleProductType::Osb:
        return PreciseProductCategory::ObservableSpecificBias;
    case PreciseBundleProductType::Erp:
        return PreciseProductCategory::EarthRotation;
    case PreciseBundleProductType::Att:
        return PreciseProductCategory::Attitude;
    case PreciseBundleProductType::Upd:
        return PreciseProductCategory::UncalibratedPhaseDelay;
    case PreciseBundleProductType::Antex:
        return PreciseProductCategory::Antenna;
    case PreciseBundleProductType::SinexCrd:
    case PreciseBundleProductType::SinexSol:
        return PreciseProductCategory::Sinex;
    case PreciseBundleProductType::Gim:
        return PreciseProductCategory::Ionosphere;
    case PreciseBundleProductType::Vmf1H00:
    case PreciseBundleProductType::Vmf1H06:
    case PreciseBundleProductType::Vmf1H12:
    case PreciseBundleProductType::Vmf1H18:
    case PreciseBundleProductType::Vmf3H00:
    case PreciseBundleProductType::Vmf3H06:
    case PreciseBundleProductType::Vmf3H12:
    case PreciseBundleProductType::Vmf3H18:
        return PreciseProductCategory::VmfGrid;
    case PreciseBundleProductType::Brdc:
        return PreciseProductCategory::BroadcastNavigation;
    }
    return PreciseProductCategory::Orbit;
}

PreciseProductLatency ToUiLatency(const std::string& value,
                                  const PreciseBundleProductType type) {
    if (type == PreciseBundleProductType::Antex) {
        return PreciseProductLatency::Static;
    }
    const auto normalized = NormalizeBundleToken(value);
    if (normalized == "FINAL") return PreciseProductLatency::Final;
    if (normalized == "RAPID") return PreciseProductLatency::Rapid;
    if (normalized == "ULTRA_RAPID") return PreciseProductLatency::UltraRapid;
    if (normalized == "NEAR_REAL_TIME") return PreciseProductLatency::NearRealTime;
    if (normalized == "REAL_TIME") return PreciseProductLatency::RealTime;
    if (normalized == "PREDICTED" || normalized == "RAPID_PREDICTED") {
        return PreciseProductLatency::Predicted;
    }
    if (normalized == "WEEKLY") return PreciseProductLatency::Weekly;
    if (normalized == "DAILY" || type == PreciseBundleProductType::Upd) {
        return PreciseProductLatency::Daily;
    }
    return PreciseProductLatency::Other;
}

bool SameQueryDate(const QueryDate& left, const QueryDate& right) {
    return left.year == right.year && left.month == right.month
        && left.day == right.day;
}

std::wstring BundleTypeLabel(const PreciseBundleProductType type) {
    return Utf8ToWide(PreciseBundleProductTypeName(type));
}

PreciseProductLatency Sp3Latency(const std::string_view latency) {
    if (latency == "FINAL") {
        return PreciseProductLatency::Final;
    }
    if (latency == "RAPID") {
        return PreciseProductLatency::Rapid;
    }
    if (latency == "ULTRA_RAPID") {
        return PreciseProductLatency::UltraRapid;
    }
    if (latency == "NEAR_REAL_TIME") {
        return PreciseProductLatency::NearRealTime;
    }
    if (latency == "REAL_TIME") {
        return PreciseProductLatency::RealTime;
    }
    if (latency == "PREDICTED") {
        return PreciseProductLatency::Predicted;
    }
    if (latency == "RAPID_PREDICTED") {
        return PreciseProductLatency::Predicted;
    }
    if (latency == "DAILY") {
        return PreciseProductLatency::Daily;
    }
    return PreciseProductLatency::Other;
}

PreciseProductFile ToPreciseProductFile(const Sp3ProductRecord& record) {
    PreciseProductFile product;
    product.id = Utf8ToWide(record.series)
        + std::format(L"_{:02}{:02}_{}_{}",
            record.startHour,
            record.startMinute,
            Utf8ToWide(record.duration),
            Utf8ToWide(record.interval));
    product.name = Utf8ToWide(record.series) + L" SP3 精密轨道";
    product.category = PreciseProductCategory::Orbit;
    product.latency = Sp3Latency(record.latency);
    product.center = Utf8ToWide(record.center);
    product.constellation = Utf8ToWide(record.constellation);
    product.interval = Utf8ToWide(record.interval);
    for (const auto& route : record.routes) {
        product.mirrors.push_back({
            Utf8ToWide(route.sourceKey),
            route.downloadUrl,
            Utf8ToWide(route.fileName)});
    }
    product.status = std::format(
        L"本地知识库：{} 个数据中心",
        product.mirrors.size());
    return product;
}

PreciseProductFile ToClkProductFile(const ClkProductRecord& record) {
    PreciseProductFile product;
    product.id = Utf8ToWide(record.series)
        + std::format(L"_{:02}{:02}_{}_{}",
            record.startHour,
            record.startMinute,
            Utf8ToWide(record.duration),
            Utf8ToWide(record.interval));
    product.name = Utf8ToWide(record.series) + L" CLK 精密钟差";
    product.category = PreciseProductCategory::Clock;
    product.latency = Sp3Latency(record.latency);
    product.center = Utf8ToWide(record.center);
    product.constellation = Utf8ToWide(record.constellation);
    product.interval = Utf8ToWide(record.interval);
    for (const auto& route : record.routes) {
        product.mirrors.push_back({
            Utf8ToWide(route.sourceKey),
            route.downloadUrl,
            Utf8ToWide(route.fileName)});
    }
    product.status = std::format(
        L"本地知识库：{} 个数据中心", product.mirrors.size());
    return product;
}

PreciseProductFile ToBroadcastProductFile(
    const BroadcastProductRecord& record) {
    PreciseProductFile product;
    product.id = Utf8ToWide(record.series)
        + L"_" + Utf8ToWide(record.sourceFlag)
        + std::format(L"_{:02}{:02}_{}_{}_{}",
            record.startHour,
            record.startMinute,
            Utf8ToWide(record.duration),
            Utf8ToWide(record.navigationType),
            Utf8ToWide(record.rinexVersion));
    product.name = Utf8ToWide(record.series) + L" 广播星历（"
        + Utf8ToWide(record.navigationType) + L"）";
    product.category = PreciseProductCategory::BroadcastNavigation;
    product.latency = PreciseProductLatency::Daily;
    product.center = Utf8ToWide(record.producer);
    product.constellation = Utf8ToWide(record.constellation);
    product.formatVersion = Utf8ToWide(record.rinexVersion);
    for (const auto& route : record.routes) {
        product.mirrors.push_back({
            Utf8ToWide(route.sourceKey),
            route.downloadUrl,
            Utf8ToWide(route.fileName)});
    }
    product.status = std::format(
        L"本地知识库：{} 个实际下载源", product.mirrors.size());
    return product;
}

PreciseProductFile ToDcbProductFile(const DcbProductRecord& record) {
    PreciseProductFile product;
    product.id = Utf8ToWide(record.variant) + L"_"
        + FormatDate(record.coverageStart) + L"_"
        + FormatDate(record.coverageEnd) + L"_"
        + Utf8ToWide(record.interval);
    const std::wstring center = record.center == "COD"
        ? L"CODE" : Utf8ToWide(record.center);
    product.name = center + L" " + Utf8ToWide(record.variant)
        + L" DCB（" + Utf8ToWide(record.scope) + L"）";
    product.category = PreciseProductCategory::DifferentialCodeBias;
    product.latency = Sp3Latency(record.latency);
    product.center = center;
    product.constellation = Utf8ToWide(record.constellation);
    product.interval = Utf8ToWide(record.interval);
    for (const auto& route : record.routes) {
        product.mirrors.push_back({
            Utf8ToWide(route.sourceKey),
            route.downloadUrl,
            Utf8ToWide(route.fileName)});
    }
    product.status = std::format(
        L"本地知识库：{} 个下载源；覆盖 {} 至 {}",
        product.mirrors.size(), FormatDate(record.coverageStart),
        FormatDate(record.coverageEnd));
    return product;
}

PreciseProductFile ToOsbProductFile(const OsbProductRecord& record) {
    PreciseProductFile product;
    product.id = Utf8ToWide(record.series)
        + std::format(L"_{:02}{:02}_{}_{}",
            record.startHour,
            record.startMinute,
            Utf8ToWide(record.duration),
            Utf8ToWide(record.interval));
    const std::wstring center = record.center == "COD"
        ? L"CODE" : Utf8ToWide(record.center);
    product.name = center + L" " + Utf8ToWide(record.series)
        + L" OSB（" + Utf8ToWide(record.biasClass) + L"）";
    product.category = PreciseProductCategory::ObservableSpecificBias;
    product.latency = Sp3Latency(record.latency);
    product.center = center;
    product.constellation = Utf8ToWide(record.constellation);
    product.interval = Utf8ToWide(record.interval);
    for (const auto& route : record.routes) {
        product.mirrors.push_back({
            Utf8ToWide(route.sourceKey),
            route.downloadUrl,
            Utf8ToWide(route.fileName)});
    }
    product.status = std::format(
        L"本地知识库：{} 个下载源；覆盖 {} 至 {}",
        product.mirrors.size(), FormatDate(record.coverageStart),
        FormatDate(record.coverageEnd));
    return product;
}

PreciseProductFile ToUpdProductFile(const UpdProductRecord& record) {
    PreciseProductFile product;
    product.id = record.logicalKey;
    product.name = L"iGMAS " + record.variant + L" " + record.updClass
        + L" UPD（" + record.frequencyCombination + L"）";
    product.category = PreciseProductCategory::UncalibratedPhaseDelay;
    product.latency = PreciseProductLatency::Daily;
    product.center = record.provider.empty() ? L"iGMAS/SGG" : record.provider;
    product.constellation = record.constellation;
    product.interval = record.interval;
    for (const auto& route : record.routes) {
        product.mirrors.push_back({route.source, route.url, route.fileName});
    }
    product.localInventoryOnly = product.mirrors.empty();
    product.status = std::format(
        L"UPD：{}；{}；{} 个下载源",
        record.updClass, record.frequencyCombination,
        product.mirrors.size());
    if (product.localInventoryOnly)
        product.status = L"Local UPD inventory; online download URL not configured";
    return product;
}

PreciseProductFile ToIonexProductFile(const IonexProductRecord& record) {
    PreciseProductFile product;
    product.id = Utf8ToWide(record.series)
        + std::format(L"_{:02}{:02}_{}_{}_{}",
            record.startHour,
            record.startMinute,
            Utf8ToWide(record.duration),
            Utf8ToWide(record.interval),
            Utf8ToWide(record.contentType));
    const std::wstring center = record.center == "COD"
        ? L"CODE" : Utf8ToWide(record.center);
    product.name = center + L" " + Utf8ToWide(record.series)
        + (record.contentType == "ROT"
            ? L" ROTI 扰动图" : L" 全球电离层图（GIM）");
    product.category = PreciseProductCategory::Ionosphere;
    product.latency = Sp3Latency(record.latency);
    product.center = center;
    product.constellation = record.contentType == "ROT" ? L"ROTI" : L"GIM";
    product.interval = Utf8ToWide(record.interval);
    for (const auto& route : record.routes) {
        product.mirrors.push_back({
            Utf8ToWide(route.sourceKey),
            route.downloadUrl,
            Utf8ToWide(route.fileName)});
    }
    product.status = std::format(
        L"本地知识库：{} 个下载源；{} / {}",
        product.mirrors.size(), Utf8ToWide(record.format),
        Utf8ToWide(record.contentType));
    return product;
}

PreciseProductFile ToErpProductFile(const ErpProductRecord& record) {
    PreciseProductFile product;
    product.id = Utf8ToWide(record.series)
        + std::format(L"_{:02}{:02}_{}_{}",
            record.startHour,
            record.startMinute,
            Utf8ToWide(record.duration),
            Utf8ToWide(record.standard));
    const std::wstring center = record.center == "COD"
        ? L"CODE" : Utf8ToWide(record.center);
    product.name = center + L" " + Utf8ToWide(record.series)
        + (record.productFamily == "EOP"
            ? L" 地球定向参数（EOP）" : L" 地球旋转参数（ERP）");
    product.category = PreciseProductCategory::EarthRotation;
    product.latency = Sp3Latency(record.latency);
    product.center = center;
    product.constellation = record.productFamily == "EOP"
        ? Utf8ToWide(record.standard) : L"Earth";
    product.interval = Utf8ToWide(record.interval);
    for (const auto& route : record.routes) {
        product.mirrors.push_back({
            Utf8ToWide(route.sourceKey),
            route.downloadUrl,
            Utf8ToWide(route.fileName)});
    }
    product.status = std::format(
        L"本地知识库：{} 个下载源；覆盖 {} 至 {}",
        product.mirrors.size(), FormatDate(record.coverageStart),
        FormatDate(record.coverageEnd));
    return product;
}

PreciseProductFile ToSinexProductFile(const SinexProductRecord& record) {
    PreciseProductFile product;
    product.id = Utf8ToWide(record.series) + L"_"
        + Utf8ToWide(record.contentType) + L"_"
        + Utf8ToWide(record.productClass) + L"_"
        + FormatDate(record.nominalDate);
    const std::wstring center = record.center == "COD"
        ? L"CODE" : Utf8ToWide(record.center);
    std::wstring kind;
    if (record.format == "SSC") kind = L" SSC 坐标解";
    else if (record.contentType == "CRD") kind = L" SINEX 坐标解";
    else kind = L" SINEX 完整解";
    product.name = center + L" " + Utf8ToWide(record.series) + kind;
    product.category = PreciseProductCategory::Sinex;
    product.latency = Sp3Latency(record.latency);
    product.center = center;
    product.constellation = Utf8ToWide(record.contentType) + L" / "
        + Utf8ToWide(record.productClass);
    product.interval = Utf8ToWide(record.duration);
    for (const auto& route : record.routes) {
        product.mirrors.push_back({
            Utf8ToWide(route.sourceKey),
            route.downloadUrl,
            Utf8ToWide(route.fileName)});
    }
    product.status = std::format(
        L"本地知识库：{} 个下载源；覆盖 {} 至 {}；{}",
        product.mirrors.size(), FormatDate(record.coverageStart),
        FormatDate(record.coverageEnd), Utf8ToWide(record.format));
    return product;
}

PreciseProductFile ToTroProductFile(const TroProductRecord& record) {
    PreciseProductFile product;
    product.id = Utf8ToWide(record.series) + L"_"
        + Utf8ToWide(record.productFamily) + L"_"
        + (record.station.empty() ? L"NETWORK" : Utf8ToWide(record.station))
        + L"_" + FormatDate(record.nominalDate);
    const std::wstring center = record.center == "COD"
        ? L"CODE" : Utf8ToWide(record.center);
    std::wstring kind;
    if (record.productFamily == "IGS_SITE") {
        kind = L" IGS测站ZPD（" + Utf8ToWide(record.station) + L"）";
    } else if (record.productFamily == "CODE_TRP") {
        kind = L" CODE网络对流层参数（TRP）";
    } else if (record.productFamily == "CODE_TRO") {
        kind = L" CODE网络TRO-SINEX";
    } else if (record.productFamily == "EPN_COMBINED") {
        kind = L" EPN组合TRO-SINEX";
    } else if (record.productFamily == "EPN_AC") {
        kind = L" EPN分析中心TRO-SINEX";
    } else {
        kind = L" 对流层产品";
    }
    product.name = center + kind;
    product.category = PreciseProductCategory::Troposphere;
    product.latency = Sp3Latency(record.latency);
    product.center = center;
    product.constellation = record.station.empty()
        ? Utf8ToWide(record.productFamily) : Utf8ToWide(record.station);
    product.interval = Utf8ToWide(record.interval);
    for (const auto& route : record.routes) {
        product.mirrors.push_back({
            Utf8ToWide(route.sourceKey),
            route.downloadUrl,
            Utf8ToWide(route.fileName)});
    }
    product.status = std::format(
        L"本地知识库：{} 个下载源；覆盖 {} 至 {}；{}",
        product.mirrors.size(), FormatDate(record.coverageStart),
        FormatDate(record.coverageEnd), Utf8ToWide(record.format));
    return product;
}

PreciseProductFile ToAntexProductFile(const AntexProductRecord& record) {
    PreciseProductFile product;
    product.id = Utf8ToWide(record.modelName) + L"_"
        + Utf8ToWide(record.variant) + L"_"
        + (record.currentAlias ? L"CURRENT"
            : std::to_wstring(record.revisionWeek));
    std::wstring producer = Utf8ToWide(record.producer);
    product.name = producer + L" " + Utf8ToWide(record.modelName)
        + L" 天线校准模型（" + Utf8ToWide(record.scope) + L"）";
    product.category = PreciseProductCategory::Antenna;
    product.latency = PreciseProductLatency::Static;
    product.center = producer;
    product.constellation = Utf8ToWide(record.referenceFrame) + L" / "
        + Utf8ToWide(record.productFamily);
    product.formatVersion = Utf8ToWide(record.formatVersion);
    for (const auto& route : record.routes) {
        product.mirrors.push_back({
            Utf8ToWide(route.sourceKey),
            route.downloadUrl,
            Utf8ToWide(route.fileName)});
    }
    const std::wstring revision = record.currentAlias
        ? L"当前别名" : std::format(L"GPS周 {} 版本", record.revisionWeek);
    product.status = std::format(
        L"本地知识库：{}；{}；{} 个下载源；{}；ANTEX {}",
        Utf8ToWide(record.calibrationType), revision,
        product.mirrors.size(), Utf8ToWide(record.variant), product.formatVersion);
    return product;
}

void InitializePreciseRuleOptions() {
    auto catalog = LoadPreciseSearchCatalog();
    if (!catalog.error.empty()) return;
    const auto first = MakeQueryDate(1980, 1, 1);
    const auto last = MakeQueryDate(2100, 12, 31);
    const auto antennas = LoadAntexAvailabilityRange(first, last);
    if (antennas.error.empty()) {
        for (const auto& record : antennas.products) {
            catalog.products.push_back(ToAntexProductFile(record));
        }
    }
    const auto grids = LoadVmfFamilyRange(first, last);
    if (grids.error.empty()) {
        for (const auto& record : grids.families) {
            PreciseProductFile product;
            product.category = PreciseProductCategory::VmfGrid;
            product.center = L"TU Wien";
            product.interval = L"06H";
            product.latency = record.grade == "FC" ? PreciseProductLatency::Predicted
                : record.grade == "EI" ? PreciseProductLatency::Other : PreciseProductLatency::Daily;
            catalog.products.push_back(std::move(product));
        }
    }
    constexpr std::array<std::wstring_view, 13> families{
        L"sp3", L"clk", L"erp/eop", L"dcb", L"orbex", L"ionex", L"tro",
        L"sinex", L"antex", L"broadcast", L"osb", L"upd", L"vmf"};
    constexpr std::array<std::wstring_view, 13> levels{
        L"final", L"rapid", L"ultra-rapid", L"near real-time", L"real-time",
        L"predicted", L"daily", L"weekly", L"static", L"other", L"cumulative",
        L"multi-day", L"solution-other"};
    std::set<std::array<std::wstring, 4>> unique;
    for (const auto& product : catalog.products) {
        const auto category = static_cast<std::size_t>(product.category);
        if (category >= families.size()) continue;
        std::wstring center(PreciseProductDisplayCenter(product));
        if (center == L"EMR") center = L"EMR/NRCan";
        else if (center == L"WHU") center = L"WHU/WUM";
        else if (PreciseProductMatchesCenter(product, L"Other")) center = L"Other";
        std::wstring interval(PreciseProductSamplingInterval(product));
        for (auto& ch : interval) ch = static_cast<wchar_t>(std::towupper(ch));
        for (std::size_t level = 0; level < levels.size(); ++level) {
            if (!PreciseProductMatchesLevel(product, static_cast<int>(level + 1))) continue;
            unique.insert({std::wstring(families[category]), std::wstring(levels[level]), center, interval});
        }
    }
    for (const auto& values : unique) {
        gPreciseRuleOptions.push_back({values[0], values[1], values[2], values[3]});
    }
    gPreciseRuleOptionsReady.store(true, std::memory_order_release);
}

const wchar_t* CategoryFolder(const PreciseProductCategory category) {
    switch (category) {
    case PreciseProductCategory::Orbit: return L"orbit-sp3";
    case PreciseProductCategory::Clock: return L"clock-clk";
    case PreciseProductCategory::EarthRotation: return L"erp-eop";
    case PreciseProductCategory::DifferentialCodeBias: return L"dcb";
    case PreciseProductCategory::Attitude: return L"attitude-orbex";
    case PreciseProductCategory::Ionosphere: return L"ionex";
    case PreciseProductCategory::Troposphere: return L"troposphere";
    case PreciseProductCategory::Sinex: return L"sinex";
    case PreciseProductCategory::Antenna: return L"antex";
    case PreciseProductCategory::BroadcastNavigation:
        return L"broadcast-navigation";
    case PreciseProductCategory::ObservableSpecificBias: return L"osb";
    case PreciseProductCategory::UncalibratedPhaseDelay: return L"upd";
    case PreciseProductCategory::VmfGrid: return L"vmf-grid";
    }
    return L"other";
}

class PreciseProductView {
public:
    PreciseProductView(const HWND window, const HINSTANCE instance)
        : window_(window), instance_(instance) {
    }

    ~PreciseProductView() {
        KillTimer(window_, kPrecisionSearchTimer);
        KillTimer(window_, kPrecisionCatalogTimer);
        if (catalogThread_.joinable()) {
            catalogThread_.request_stop();
            catalogThread_.join();
        }
        // Publish the stop request before waking a transfer that is waiting
        // in the paused state.  Otherwise a worker can wake in the small
        // window between resume() and request_stop() and claim one more
        // product day while the window is being destroyed.
        planWorkers_.JoinAll();
        if (downloadThread_.joinable()) {
            downloadThread_.request_stop();
        }
        if (downloadControl_ != nullptr) {
            downloadControl_->resume();
        }
        // Keep window destruction deterministic even when the transfer was
        // paused and is currently inside DownloadControl::waitIfPaused().
        if (downloadThread_.joinable()) {
            downloadThread_.join();
        }
        if (titleFont_ != nullptr) {
            DeleteObject(titleFont_);
        }
        if (bodyFont_ != nullptr) {
            DeleteObject(bodyFont_);
        }
        if (smallFont_ != nullptr) {
            DeleteObject(smallFont_);
        }
    }

    bool CreateControls() {
        titleFont_ = CreateFontW(
            -22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");
        bodyFont_ = CreateFontW(
            -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");
        smallFont_ = CreateFontW(
            -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");

        heading_ = Static(L"精密产品工作台", SS_LEFT);
        explanation_ = Static(
            L"按 UTC 日期查询轨道、钟差、偏差和辅助产品；范围完整率与下载状态一目了然。",
            SS_LEFT);

        quickLabel_ = Static(L"快速方案", SS_LEFT);
        quickPreset_ = Combo(IdQuickPreset);
        for (const wchar_t* text : {
                 L"不使用快速方案",
                 L"PRIDE PPP-AR",
                 L"IGS 最终 PPP｜标准精密定位产品",
                 L"UPD PPP-AR｜相位偏差整数固定",
                 L"OSB PPP-AR｜观测偏差整数固定",
                 L"PPP-RTK｜网络高精度定位基础文件",
                 L"通用 PPP 智能优选｜自动找完整产品"}) {
            SendMessageW(quickPreset_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(text));
        }
        SendMessageW(quickPreset_, CB_SETCURSEL, 0, 0);
        quickApply_ = Button(L"应用方案并查询", IdQuickApply, BS_DEFPUSHBUTTON);
        quickInfo_ = Static(
            L"选择日期和方案后，点击“应用方案并查询”查看产品清单；确认后使用下方“开始下载”。",
            SS_LEFT | SS_NOPREFIX);
        dateLabel_ = Static(L"起始日期（UTC）", SS_LEFT);
        date_ = CreateWindowExW(
            0, DATETIMEPICK_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | DTS_SHORTDATEFORMAT,
            0, 0, 100, 30, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdDate)),
            instance_, nullptr);
        DateTime_SetFormat(date_, L"yyyy-MM-dd");
        SetRecentDate(date_);
        endDateLabel_ = Static(L"终止日期（UTC）", SS_LEFT);
        endDate_ = CreateWindowExW(
            0, DATETIMEPICK_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | DTS_SHORTDATEFORMAT,
            0, 0, 100, 30, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdEndDate)),
            instance_, nullptr);
        DateTime_SetFormat(endDate_, L"yyyy-MM-dd");
        SetRecentDate(endDate_);
        generate_ = Button(L"查询产品", IdGenerate);

        categoryLabel_ = Static(L"产品类型", SS_LEFT);
        category_ = Combo(IdCategory);
        for (const wchar_t* text : {
                 L"全部类型", L"SP3 轨道", L"CLK 钟差", L"ERP / EOP",
                 L"DCB 差分码偏差", L"ORBEX 姿态", L"IONEX 电离层",
                 L"对流层", L"SINEX", L"ANTEX", L"广播星历",
                 L"OSB 观测量偏差", L"UPD 未校准相位延迟",
                 L"VMF 对流层格网"}) {
            SendMessageW(category_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        }
        SendMessageW(category_, CB_SETCURSEL, 0, 0);

        latencyLabel_ = Static(L"产品等级", SS_LEFT);
        latency_ = Combo(IdLatency);
        for (const wchar_t* text : {
                 L"全部等级", L"最终", L"快速", L"超快速", L"近实时",
                 L"实时流归档（非实时服务）", L"预报", L"每日", L"每周", L"静态", L"其他",
                 L"累计解", L"多日解", L"其他解"}) {
            SendMessageW(latency_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        }
        SendMessageW(latency_, CB_SETCURSEL, 0, 0);

        centerLabel_ = Static(L"生产机构 / 分析中心", SS_LEFT);
        center_ = Combo(IdCenter);
        const std::array centerLabels{
            L"全部机构", L"IGS", L"CODE", L"EMR/NRCan", L"ESA",
            L"GFZ", L"GRG", L"JPL", L"MIT", L"NGS", L"SIO",
            L"USNO", L"WHU/WUM", L"JGX", L"HUST", L"IERS",
            L"IAC", L"JAXA", L"SHAO",
            L"BKG", L"IGN", L"DLR", L"CAS", L"UPC", L"OPTIMAP", L"其他",
            L"TU Wien", L"WCC", L"ASI", L"AUS", L"BEK", L"BEV", L"CSR", L"DEO", L"EUR", L"GOP", L"GSI", L"IGE", L"LPT", L"MUT", L"NCL", L"NKG", L"OLG", L"PDR", L"PGC", L"RGA", L"ROB", L"SGO", L"SUT", L"TUG", L"TUM", L"ULR", L"UPA", L"WUT", L"GIA", L"MCC", L"EPN"};
        centerFilters_ = {
            L"", L"IGS", L"COD", L"EMR", L"ESA", L"GFZ", L"GRG",
            L"JPL", L"MIT", L"NGS", L"SIO", L"USN", L"WHU/WUM",
            L"JGX", L"HUS", L"IERS", L"IAC",
            L"JAX", L"SHA", L"BKG", L"IGN", L"DLR", L"CAS", L"UPC",
            L"OPT", L"OTHER", L"TU Wien", L"WCC", L"ASI", L"AUS", L"BEK", L"BEV", L"CSR", L"DEO", L"EUR", L"GOP", L"GSI", L"IGE", L"LPT", L"MUT", L"NCL", L"NKG", L"OLG", L"PDR", L"PGC", L"RGA", L"ROB", L"SGO", L"SUT", L"TUG", L"TUM", L"ULR", L"UPA", L"WUT", L"GIA", L"MCC", L"EPN"};
        for (const wchar_t* text : centerLabels) {
            SendMessageW(center_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        }
        SendMessageW(center_, CB_SETCURSEL, 0, 0);

        completenessLabel_ = Static(L"范围完整性", SS_LEFT);
        completeness_ = Combo(IdCompleteness);
        for (const wchar_t* text : {
                 L"不限制", L"完整率 ≥ 100%", L"完整率 ≥ 95%",
                 L"完整率 ≥ 90%", L"完整率 ≥ 80%", L"完整率 ≥ 50%",
                 L"可用天数 ≥ 300", L"可用天数 ≥ 100", L"可用天数 ≥ 30",
                 L"可用天数 ≥ 7", L"可用天数 ≥ 1"}) {
            SendMessageW(completeness_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(text));
        }
        SendMessageW(completeness_, CB_SETCURSEL, 0, 0);

        troStationLabel_ = Static(L"TRO 测站", SS_LEFT);
        troStation_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"ZIM2",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 100, 30, window_, nullptr, instance_, nullptr);
        SendMessageW(troStation_, EM_SETLIMITTEXT, 9, 0);

        searchLabel_ = Static(L"搜索产品或分析中心", SS_LEFT);
        search_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 100, 30, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdSearch)),
            instance_, nullptr);
        SendMessageW(
            search_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(L"例如 IGS、WHU、IONEX、GCRE_WHU_F_sp3"));

        list_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT
                | LVS_SINGLESEL | LVS_OWNERDATA,
            0, 0, 100, 100, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdList)),
            instance_, nullptr);
        ListView_SetExtendedListViewStyle(
            list_, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP
                | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
        // Keep the product table quiet and readable while retaining the
        // standard Windows interaction model (sorting/selection/check boxes).
        // Explorer theming removes the legacy raised frame on supported
        // Windows versions; the explicit colours provide a consistent
        // fallback when the theme is unavailable.
        SetWindowTheme(list_, L"Explorer", nullptr);
        ListView_SetBkColor(list_, RGB(255, 255, 255));
        ListView_SetTextBkColor(list_, RGB(255, 255, 255));
        ListView_SetTextColor(list_, RGB(31,  41,  55));
        AddColumn(0, L"产品 ID", 200);
        AddColumn(1, L"产品类型", 132);
        AddColumn(2, L"分析中心", 120);
        AddColumn(3, L"等级", 94);
        AddColumn(4, L"系统", 110);
        AddColumn(5, L"间隔", 68);
        AddColumn(6, L"首选文件名", 330);
        AddColumn(7, L"候选源", 70);
        AddColumn(8, L"状态", 170);
        AddColumn(9, L"范围完整性", 105);

        summary_ = Static(L"尚未生成产品清单。", SS_LEFT);
        selectAll_ = Button(L"全选当前列表", IdSelectAll);
        clear_ = Button(L"清空选择", IdClear);
        folderLabel_ = Static(L"保存目录", SS_LEFT);
        folder_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT",
            DownloadFolderState::Load(
                L"precise", DefaultProductFolder()).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 100, 30, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdFolder)),
            instance_, nullptr);
        browse_ = Button(L"浏览…", IdBrowse);
        download_ = Button(L"开始下载", IdDownload);
        pause_ = Button(L"暂停下载", IdPauseDownload);
        EnableWindow(pause_, FALSE);
        progress_ = CreateWindowExW(
            0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
            0, 0, 100, 18, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdProgress)),
            instance_, nullptr);
        SendMessageW(progress_, PBM_SETRANGE32, 0, 1000);
        status_ = Static(
            L"初版保留服务器上的 .gz / .Z 原压缩文件；同一产品只下载首个成功镜像。",
            SS_LEFT | SS_NOPREFIX);

        if (heading_ == nullptr || date_ == nullptr || endDate_ == nullptr
            || generate_ == nullptr
            || quickLabel_ == nullptr || quickPreset_ == nullptr
            || quickApply_ == nullptr
            || quickInfo_ == nullptr
            || category_ == nullptr || latency_ == nullptr || center_ == nullptr
            || completeness_ == nullptr
            || troStation_ == nullptr || search_ == nullptr
            || list_ == nullptr || folder_ == nullptr || download_ == nullptr
            || pause_ == nullptr) {
            return false;
        }
        SendMessageW(heading_, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
        for (const HWND control : {
                  explanation_, quickLabel_, quickPreset_, quickApply_, quickInfo_,
                  dateLabel_, date_, endDateLabel_, endDate_, generate_,
                  categoryLabel_, category_,
                  latencyLabel_, latency_, centerLabel_, center_,
                  completenessLabel_, completeness_,
                 troStationLabel_, troStation_,
                 searchLabel_, search_, list_, summary_,
                 selectAll_, clear_, folderLabel_, folder_, browse_, download_,
                 pause_}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        }
        SendMessageW(explanation_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
        SendMessageW(quickInfo_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
        SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
        PrepareRuleOptions();
        // Opening the page only creates controls. Build a catalogue after an
        // explicit query so startup never fills the list with a default family.
        SetWindowTextW(status_, L"请选择日期和筛选条件，然后点击“查询产品”。");
        return true;
    }

    void Layout() const {
        RECT client{};
        GetClientRect(window_, &client);
        const int width = std::max(
            1, static_cast<int>(client.right - client.left));
        const int height = std::max(
            1, static_cast<int>(client.bottom - client.top));
        constexpr int margin = 18;
        constexpr int gap = 8;
        const int contentWidth = std::max(1, width - margin * 2);
        const int right = margin + contentWidth;

        // Suppress the dozens of intermediate repaints during a resize.  A
        // single redraw at the end keeps the precision page responsive when
        // the parent window is being dragged between monitors.
        SendMessageW(window_, WM_SETREDRAW, FALSE, 0);
        const auto place = [](const HWND control, const int x, const int y,
                              const int controlWidth, const int controlHeight) {
            if (control == nullptr) return;
            MoveWindow(control, x, y,
                std::max(1, controlWidth), std::max(1, controlHeight), FALSE);
        };

        const bool compact = height < 700;
        const int labelVisibility = compact ? SW_HIDE : SW_SHOW;
        for (const HWND control : {
                 dateLabel_, endDateLabel_, categoryLabel_, latencyLabel_,
                 centerLabel_, completenessLabel_, troStationLabel_,
                 searchLabel_}) {
            ShowWindow(control, labelVisibility);
        }
        ShowWindow(quickInfo_, compact ? SW_HIDE : SW_SHOW);

        if (compact) {
            // A main window can leave only about 540 px for this child on a
            // 1366x768 laptop.  Keep every action reachable by collapsing the
            // descriptive labels into the card captions and using two dense
            // filter rows; the list and download bar still retain usable
            // touch/keyboard targets.
            place(heading_, margin, 8, contentWidth, 28);
            place(explanation_, margin, 36, contentWidth, 18);

            constexpr int compactGap = 6;
            constexpr int compactQuickLabel = 70;
            constexpr int compactQuickApply = 118;
            const int compactPreset = std::clamp(
                contentWidth - compactQuickLabel - compactQuickApply
                    - compactGap * 2,
                160, 260);
            // Reserve a caption band above each compact row as well.  The
            // compact path used to put the canvas captions directly on top
            // of the native controls when a laptop window was short.
            const int quickTop = 82;
            int x = margin;
            place(quickLabel_, x, quickTop + 4,
                compactQuickLabel, 24);
            x += compactQuickLabel + compactGap;
            place(quickPreset_, x, quickTop,
                compactPreset, 31);
            x += compactPreset + compactGap;
            place(quickApply_, x, quickTop,
                compactQuickApply, 31);

            constexpr int dateTop = 120;
            constexpr int compactDateWidth = 145;
            SetWindowTextW(endDateLabel_, L"至");
            ShowWindow(endDateLabel_, SW_SHOW);
            place(date_, margin, dateTop,
                compactDateWidth, 31);
            place(endDateLabel_, margin + compactDateWidth,
                dateTop + 4, compactGap, 24);
            place(endDate_, margin + compactDateWidth + compactGap,
                dateTop, compactDateWidth, 31);
            const int compactGenerateLeft = margin
                + compactDateWidth * 2 + compactGap * 2;
            place(generate_, compactGenerateLeft, dateTop,
                std::max(110, right - compactGenerateLeft), 31);

            constexpr int compactCategoryTop = 158;
            const int compactCategoryWidth = std::clamp(
                contentWidth * 24 / 100, 140, 220);
            const int compactLatencyWidth = std::clamp(
                contentWidth * 18 / 100, 110, 170);
            const int compactCenterWidth = std::max(
                140, contentWidth - compactCategoryWidth
                    - compactLatencyWidth - compactGap * 2);
            x = margin;
            place(category_, x, compactCategoryTop,
                compactCategoryWidth, 31);
            x += compactCategoryWidth + compactGap;
            place(latency_, x, compactCategoryTop,
                compactLatencyWidth, 31);
            x += compactLatencyWidth + compactGap;
            place(center_, x, compactCategoryTop,
                compactCenterWidth, 31);

            constexpr int compactSecondTop = 194;
            const int compactCompletenessWidth = std::clamp(
                contentWidth * 24 / 100, 150, 230);
            const int compactTroWidth = std::clamp(
                contentWidth * 17 / 100, 105, 150);
            const int compactSearchWidth = std::max(
                140, contentWidth - compactCompletenessWidth
                    - compactTroWidth - compactGap * 2);
            x = margin;
            place(completeness_, x, compactSecondTop,
                compactCompletenessWidth, 31);
            x += compactCompletenessWidth + compactGap;
            place(troStation_, x, compactSecondTop,
                compactTroWidth, 31);
            x += compactTroWidth + compactGap;
            place(search_, x, compactSecondTop,
                compactSearchWidth, 31);

            constexpr int compactListTop = 232;
            constexpr int compactBottomReserve = 122;
            const int compactListHeight = std::max(
                90, height - compactListTop - compactBottomReserve);
            const int compactListBottom = compactListTop + compactListHeight;
            place(list_, margin, compactListTop,
                contentWidth, compactListHeight);
            ResizeListColumns(contentWidth);

            int y = compactListBottom + 2;
            constexpr int compactClearWidth = 92;
            constexpr int compactSelectWidth = 112;
            place(summary_, margin, y,
                std::max(100, contentWidth - compactSelectWidth
                    - compactClearWidth - compactGap * 2), 22);
            place(selectAll_, right - compactClearWidth
                    - compactSelectWidth - compactGap,
                y - 2, compactSelectWidth, 26);
            place(clear_, right - compactClearWidth,
                y - 2, compactClearWidth, 26);
            y += 24;
            constexpr int compactFolderLabel = 58;
            constexpr int compactBrowse = 70;
            place(folderLabel_, margin, y + 2,
                compactFolderLabel, 22);
            place(folder_, margin + compactFolderLabel + compactGap, y,
                std::max(100, contentWidth - compactFolderLabel
                    - compactBrowse - compactGap * 2), 26);
            place(browse_, right - compactBrowse, y,
                compactBrowse, 26);
            y += 30;
            constexpr int compactPause = 96;
            place(download_, margin, y,
                std::max(110, contentWidth - compactPause - compactGap), 30);
            place(pause_, right - compactPause, y,
                compactPause, 30);
            y += 34;
            place(progress_, margin, y, contentWidth, 10);
            y += 14;
            place(status_, margin, y, contentWidth, 18);

            SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
            RedrawWindow(window_, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            return;
        }

        place(heading_, margin, 16, contentWidth, 32);
        place(explanation_, margin, 49, contentWidth, 24);
        SetWindowTextW(endDateLabel_, L"终止日期（UTC）");

        // Keep the preset and its query action on one compact row; the
        // explanatory copy consumes the remaining space instead of forcing a
        // horizontal scroll at the parent window's minimum width.
        constexpr int quickLabelWidth = 100;
        constexpr int quickApplyWidth = 126;
        const int quickPresetWidth = std::clamp(
            contentWidth - quickLabelWidth - quickApplyWidth - gap * 2,
            220, 330);
        // Leave a clear header band above the first native combo.  The
        // previous row began immediately under the canvas caption, so the
        // themed combo could paint over the section title.
        const int quickTop = 102;
        int x = margin;
        place(quickLabel_, x, quickTop + 4, quickLabelWidth, 24);
        x += quickLabelWidth + gap;
        place(quickPreset_, x, quickTop, quickPresetWidth, 240);
        x += quickPresetWidth + gap;
        place(quickApply_, x, quickTop, quickApplyWidth, 32);
        x += quickApplyWidth + gap;
        place(quickInfo_, x, quickTop + 1,
            std::max(120, right - x), 40);

        // Date selection is deliberately separate from the product filters;
        // the query button therefore remains easy to find and never collides
        // with a combo box when the window is narrowed.
        constexpr int dateGroupWidth = 174;
        constexpr int datePickerWidth = 166;
        const int dateTop = quickTop + 54;
        place(dateLabel_, margin, dateTop, dateGroupWidth, 21);
        place(date_, margin, dateTop + 22, datePickerWidth, 31);
        const int endGroupLeft = margin + dateGroupWidth + gap;
        place(endDateLabel_, endGroupLeft, dateTop, dateGroupWidth, 21);
        place(endDate_, endGroupLeft, dateTop + 22, datePickerWidth, 31);
        const int generateLeft = endGroupLeft + dateGroupWidth + gap;
        place(generate_, generateLeft, dateTop + 22,
            std::max(132, right - generateLeft), 32);

        // Product filters are arranged as two rows.  This is the key
        // responsive change: the old implementation placed six fields at
        // x=1200+, which made most of the precision page unreachable after a
        // navigation rail was added to the parent window.
        constexpr int fieldLabelHeight = 21;
        constexpr int fieldHeight = 31;
        constexpr int filterRowHeight = 61;
        const int filtersTop = dateTop + 68;
        const int categoryWidth = std::clamp(contentWidth * 24 / 100, 160, 250);
        const int latencyWidth = std::clamp(contentWidth * 18 / 100, 130, 210);
        const int centerWidth = std::max(
            180, contentWidth - categoryWidth - latencyWidth - gap * 2);
        x = margin;
        place(categoryLabel_, x, filtersTop, categoryWidth, fieldLabelHeight);
        place(category_, x, filtersTop + fieldLabelHeight,
            categoryWidth, 230);
        x += categoryWidth + gap;
        place(latencyLabel_, x, filtersTop, latencyWidth, fieldLabelHeight);
        place(latency_, x, filtersTop + fieldLabelHeight,
            latencyWidth, 230);
        x += latencyWidth + gap;
        place(centerLabel_, x, filtersTop, centerWidth, fieldLabelHeight);
        place(center_, x, filtersTop + fieldLabelHeight,
            centerWidth, 230);

        const int secondRowTop = filtersTop + filterRowHeight;
        const int completenessWidth = std::clamp(
            contentWidth * 24 / 100, 180, 260);
        const int troWidth = std::clamp(contentWidth * 17 / 100, 120, 180);
        const int searchWidth = std::max(
            180, contentWidth - completenessWidth - troWidth - gap * 2);
        x = margin;
        place(completenessLabel_, x, secondRowTop,
            completenessWidth, fieldLabelHeight);
        place(completeness_, x, secondRowTop + fieldLabelHeight,
            completenessWidth, 230);
        x += completenessWidth + gap;
        place(troStationLabel_, x, secondRowTop,
            troWidth, fieldLabelHeight);
        place(troStation_, x, secondRowTop + fieldLabelHeight,
            troWidth, fieldHeight);
        x += troWidth + gap;
        place(searchLabel_, x, secondRowTop,
            searchWidth, fieldLabelHeight);
        place(search_, x, secondRowTop + fieldLabelHeight,
            searchWidth, fieldHeight);

        const int listTop = secondRowTop + filterRowHeight + 10;
        constexpr int bottomReserve = 190;
        const int minimumListHeight = height < 760 ? 112 : 180;
        const int listHeight = std::max(
            minimumListHeight, height - listTop - bottomReserve);
        const int listBottom = listTop + listHeight;
        place(list_, margin, listTop, contentWidth, listHeight);
        ResizeListColumns(contentWidth);

        int y = listBottom + 8;
        constexpr int clearWidth = 112;
        constexpr int selectWidth = 132;
        place(summary_, margin, y,
            std::max(150, contentWidth - selectWidth - clearWidth - gap * 2),
            24);
        place(selectAll_, right - clearWidth - selectWidth - gap, y - 3,
            selectWidth, 30);
        place(clear_, right - clearWidth, y - 3, clearWidth, 30);
        y += 34;

        constexpr int folderLabelWidth = 80;
        constexpr int browseWidth = 84;
        place(folderLabel_, margin, y + 4, folderLabelWidth, 24);
        place(folder_, margin + folderLabelWidth + gap, y,
            std::max(120, contentWidth - folderLabelWidth - browseWidth - gap * 2),
            31);
        place(browse_, right - browseWidth, y, browseWidth, 31);
        y += 39;

        constexpr int pauseWidth = 118;
        place(download_, margin, y,
            std::max(120, contentWidth - pauseWidth - gap), 36);
        place(pause_, right - pauseWidth, y, pauseWidth, 36);
        y += 42;
        place(progress_, margin, y, contentWidth, 16);
        y += 22;
        place(status_, margin, y, contentWidth, 24);

        SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(window_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    void PaintBackground(const HDC deviceContext) const {
        RECT client{};
        GetClientRect(window_, &client);
        const int width = std::max(
            1, static_cast<int>(client.right - client.left));
        const int height = std::max(
            1, static_cast<int>(client.bottom - client.top));
        constexpr int margin = 18;
        const int contentWidth = std::max(1, width - margin * 2);
        const int right = margin + contentWidth;
        const int quickTop = 102;
        const int dateTop = quickTop + 54;
        const int filtersTop = dateTop + 68;
        constexpr int filterRowHeight = 61;
        const int secondRowTop = filtersTop + filterRowHeight;
        const int listTop = secondRowTop + filterRowHeight + 10;
        constexpr int bottomReserve = 190;
        const int minimumListHeight = height < 760 ? 112 : 180;
        const int listHeight = std::max(
            minimumListHeight, height - listTop - bottomReserve);
        const int listBottom = listTop + listHeight;
        const int actionTop = listBottom + 8;

        FillPrecisionRect(deviceContext, client, kPrecisionBackground);
        if (height < 700) {
            constexpr int compactQuickTop = 82;
            constexpr int compactListTop = 232;
            constexpr int compactBottomReserve = 122;
            const int compactListHeight = std::max(
                90, height - compactListTop - compactBottomReserve);
            const int compactListBottom = compactListTop + compactListHeight;
            const int compactActionTop = compactListBottom + 2;

            FillPrecisionRect(deviceContext,
                RECT{margin, 52, right, 54}, kPrecisionAccent);
            // At this height the individual caption bands would overlap one
            // another (the native rows are only 36 px apart). Use one calm
            // workspace card for the controls and list instead; the compact
            // labels that remain visible in the rows provide the context,
            // while the download footer has its own separate card.
            DrawPrecisionCard(deviceContext,
                RECT{margin - 8, compactQuickTop - 8,
                     right + 8, compactListBottom});
            DrawPrecisionCard(deviceContext,
                RECT{margin - 8, compactActionTop - 2,
                     right + 8, std::max(compactActionTop + 120,
                         height - 8)});
            return;
        }
        // The header remains open so the page title reads as part of the
        // parent navigation rather than another nested group box.
        FillPrecisionRect(deviceContext,
            RECT{margin, 77, right, 79}, kPrecisionAccent);
        DrawPrecisionCard(deviceContext,
            RECT{margin - 8, quickTop - 28, right + 8, quickTop + 40});
        DrawPrecisionCard(deviceContext,
            RECT{margin - 8, dateTop - 28,
                 right + 8, dateTop + 58});
        DrawPrecisionCard(deviceContext,
            RECT{margin - 8, filtersTop - 28,
                 right + 8, secondRowTop + 52});
        DrawPrecisionCard(deviceContext,
            RECT{margin - 8, listTop - 28,
                 right + 8, listBottom + 8});
        DrawPrecisionCard(deviceContext,
            RECT{margin - 8, actionTop - 28,
                 right + 8, std::max(actionTop + 174, height - 12)});

        // Captions live on the canvas rather than inside child controls, so
        // the cards remain legible even when a native combo/list view paints
        // its own background.
        DrawPrecisionCaption(deviceContext, margin, quickTop - 23,
            contentWidth, L"快速方案", smallFont_);
        DrawPrecisionCaption(deviceContext, margin, dateTop - 23,
            contentWidth, L"查询范围（UTC）", smallFont_);
        DrawPrecisionCaption(deviceContext, margin, filtersTop - 23,
            contentWidth, L"产品筛选", smallFont_);
        DrawPrecisionCaption(deviceContext, margin, listTop - 23,
            contentWidth, L"本地产品清单", smallFont_);
        DrawPrecisionCaption(deviceContext, margin, actionTop - 23,
            contentWidth, L"下载任务", smallFont_);
    }

    HBRUSH HandleControlColor(
        const UINT message,
        const HDC deviceContext,
        const HWND control) const noexcept {
        if (deviceContext == nullptr) {
            return PrecisionBackgroundBrush();
        }
        const bool edit = message == WM_CTLCOLOREDIT;
        const bool primaryText = control == heading_ || control == summary_;
        SetBkMode(deviceContext, edit ? OPAQUE : TRANSPARENT);
        SetBkColor(deviceContext,
            edit ? kPrecisionSurface : kPrecisionBackground);
        SetTextColor(deviceContext,
            edit ? kPrecisionText
                 : primaryText ? kPrecisionText : kPrecisionMuted);
        return edit ? PrecisionSurfaceBrush() : PrecisionBackgroundBrush();
    }

    PreciseDayCatalog LoadDayCatalog(
        const QueryDate& date, const int categoryIndex,
        const std::wstring_view troStation) const {
        PreciseDayCatalog result;
        result.date = date;
        auto append = [&](const auto& records, auto converter) {
            result.products.reserve(records.size());
            for (const auto& record : records) {
                result.products.push_back(converter(record));
            }
        };
        if (categoryIndex == 0) {
            // "All products" must be the union of the same authoritative
            // knowledge bases used by each individual family.  The former
            // generated fallback only knew a subset (for example two ANTEX
            // rows), which made filters return different answers depending
            // on whether a family was selected first.
            result.indexed = false;
            result.complete = true;
            for (int family = 1; family <= 13; ++family) {
                if (family == 13) {
                    std::error_code ec;
                    if (!std::filesystem::is_regular_file(
                            GetVmfAvailabilityDatabasePath(), ec)) continue;
                }
                auto child = LoadDayCatalog(date, family, troStation);
                result.indexed = result.indexed || child.indexed;
                result.complete = result.complete && child.complete;
                if (!child.error.empty()) {
                    if (!result.error.empty()) result.error += L"；";
                    result.error += child.error;
                }
                result.products.insert(
                    result.products.end(),
                    std::make_move_iterator(child.products.begin()),
                    std::make_move_iterator(child.products.end()));
            }
            return result;
        }
        if (categoryIndex == 1) {
            const auto lookup = LoadSp3Availability(date);
            result.indexed = lookup.indexed;
            result.complete = lookup.complete;
            result.error = lookup.error;
            append(lookup.products, [](const auto& record) {
                return ToPreciseProductFile(record);
            });
        } else if (categoryIndex == 2) {
            const auto lookup = LoadClkAvailability(date);
            result.indexed = lookup.indexed;
            result.complete = lookup.complete;
            result.error = lookup.error;
            append(lookup.products, [](const auto& record) {
                return ToClkProductFile(record);
            });
        } else if (categoryIndex == static_cast<int>(PreciseProductCategory::EarthRotation) + 1) {
            const auto lookup = LoadErpAvailability(date);
            result.indexed = lookup.indexed;
            result.complete = lookup.complete;
            result.error = lookup.error;
            append(lookup.products, [](const auto& record) {
                return ToErpProductFile(record);
            });
        } else if (categoryIndex == static_cast<int>(PreciseProductCategory::DifferentialCodeBias) + 1) {
            const auto lookup = LoadDcbAvailability(date);
            result.indexed = lookup.indexed;
            result.complete = lookup.complete;
            result.error = lookup.error;
            append(lookup.products, [](const auto& record) {
                return ToDcbProductFile(record);
            });
        } else if (categoryIndex == static_cast<int>(PreciseProductCategory::Attitude) + 1) {
            const auto lookup = LoadAttAvailability(date);
            result.indexed = lookup.indexed;
            result.complete = lookup.complete;
            result.error = lookup.error;
            append(lookup.products, [](const auto& record) {
                auto product = ToPreciseProductFile(record);
                product.category = PreciseProductCategory::Attitude;
                product.name = Utf8ToWide(record.series) + L" ORBEX 姿态产品";
                product.status = std::format(
                    L"本地知识库：{} 个数据中心", product.mirrors.size());
                return product;
            });
        } else if (categoryIndex == static_cast<int>(PreciseProductCategory::Ionosphere) + 1) {
            const auto lookup = LoadIonexAvailability(date);
            result.indexed = lookup.indexed;
            result.complete = lookup.complete;
            result.error = lookup.error;
            append(lookup.products, [](const auto& record) {
                return ToIonexProductFile(record);
            });
        } else if (categoryIndex == static_cast<int>(PreciseProductCategory::Troposphere) + 1) {
            const auto lookup = LoadTroAvailability(date);
            result.indexed = lookup.indexed;
            result.complete = lookup.complete;
            result.error = lookup.error;
            append(lookup.products, [](const auto& record) {
                return ToTroProductFile(record);
            });
        } else if (categoryIndex == static_cast<int>(PreciseProductCategory::Sinex) + 1) {
            const auto lookup = LoadSinexAvailability(date);
            result.indexed = lookup.indexed;
            result.complete = lookup.complete;
            result.error = lookup.error;
            append(lookup.products, [](const auto& record) {
                return ToSinexProductFile(record);
            });
        } else if (categoryIndex == static_cast<int>(PreciseProductCategory::Antenna) + 1) {
            const auto lookup = LoadAntexAvailability(date);
            result.indexed = lookup.indexed;
            result.complete = lookup.complete;
            result.error = lookup.error;
            append(lookup.products, [](const auto& record) {
                return ToAntexProductFile(record);
            });
        } else if (categoryIndex == static_cast<int>(PreciseProductCategory::BroadcastNavigation) + 1) {
            const auto lookup = LoadBroadcastAvailability(date);
            result.indexed = lookup.indexed;
            result.complete = lookup.complete;
            result.error = lookup.error;
            append(lookup.products, [](const auto& record) {
                return ToBroadcastProductFile(record);
            });
        } else if (categoryIndex == static_cast<int>(PreciseProductCategory::ObservableSpecificBias) + 1) {
            const auto lookup = LoadOsbAvailability(date);
            result.indexed = lookup.indexed;
            result.complete = lookup.complete;
            result.error = lookup.error;
            append(lookup.products, [](const auto& record) {
                return ToOsbProductFile(record);
            });
        } else if (categoryIndex == static_cast<int>(PreciseProductCategory::UncalibratedPhaseDelay) + 1) {
            const auto lookup = LoadUpdAvailability(date);
            result.indexed = lookup.indexed;
            result.complete = lookup.complete;
            result.error = lookup.error;
            append(lookup.products, [](const auto& record) {
                return ToUpdProductFile(record);
            });
        }
        // RT-PPP is published in a separate BDSmart daily directory and is
        // therefore not part of the weekly SP3/CLK/OSB index databases.
        // Keep the existing indexed rows untouched and append the direct
        // routes only for the matching product category.
        AppendBdsMartProductsForDate(result.products, date, categoryIndex);
        return result;
    }

    std::unique_ptr<PreciseCatalogResult> BuildCatalogRange(
        const std::stop_token stopToken, const QueryDate& start,
        const QueryDate& end, const int categoryIndex,
        const std::wstring& troStation) const {
        const auto startedAt = std::chrono::steady_clock::now();
        const int expectedDays = InclusiveDays(start, end);
        auto result = std::make_unique<PreciseCatalogResult>();
        result->start = start;
        result->end = end;
        if (expectedDays <= 0) return result;
        if (categoryIndex == 0) {
            const auto startedAtAll = std::chrono::steady_clock::now();
            for (int family = 1; family <= 13; ++family) {
                if (family == 13) {
                    std::error_code ec;
                    if (!std::filesystem::is_regular_file(
                            GetVmfAvailabilityDatabasePath(), ec)) continue;
                }
                if (stopToken.stop_requested()) {
                    result->cancelled = true;
                    return result;
                }
                auto part = BuildCatalogRange(
                    stopToken, start, end, family, troStation);
                if (!part || part->cancelled) {
                    result->cancelled = true;
                    return result;
                }
                if (part->failed) {
                    result->failed = true;
                    result->status = std::move(part->status);
                    return result;
                }
                result->products.insert(result->products.end(),
                    std::make_move_iterator(part->products.begin()),
                    std::make_move_iterator(part->products.end()));
                result->productDays.insert(result->productDays.end(),
                    std::make_move_iterator(part->productDays.begin()),
                    std::make_move_iterator(part->productDays.end()));
                result->coverageRanges.insert(result->coverageRanges.end(),
                    std::make_move_iterator(part->coverageRanges.begin()),
                    std::make_move_iterator(part->coverageRanges.end()));
                result->indexedFamilyKeys.insert(
                    result->indexedFamilyKeys.end(),
                    std::make_move_iterator(part->indexedFamilyKeys.begin()),
                    std::make_move_iterator(part->indexedFamilyKeys.end()));
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAtAll).count();
            result->status = std::format(
                L"全部精密产品查询范围 {} 至 {}：合并已安装的真实知识库，"
                L"共 {} 个逻辑产品；后台读取耗时 {:.1f} 秒。",
                FormatDate(start), FormatDate(end), result->products.size(),
                static_cast<double>(elapsed) / 1000.0);
            return result;
        }
        std::unordered_map<std::wstring, std::size_t> byKey;
        std::size_t processedDays = 0;
        std::size_t indexedDays = 0;
        std::size_t incompleteDays = 0;
        std::vector<std::wstring> warnings;
        const auto consumeProduct = [&](const QueryDate& day,
                                        PreciseProductFile product) {
                const auto key = ProductLogicalKey(product);
                const auto [it, inserted] = byKey.emplace(key, result->products.size());
                std::size_t productIndex = it->second;
                auto dayMirrors = std::move(product.mirrors);
                if (inserted) {
                    product.mirrors = RepresentativeMirrors(dayMirrors);
                    product.selected = false;
                    product.expectedDays = expectedDays;
                    product.availableDays = 0;
                    product.completenessPercent = 0.0;
                    result->products.push_back(std::move(product));
                    result->productDays.emplace_back();
                    result->indexedFamilyKeys.emplace_back();
                } else {
                    MergeMirrors(result->products[productIndex].mirrors,
                        RepresentativeMirrors(dayMirrors));
                }
                auto& dayFiles = result->productDays[productIndex];
                // Range rows arrive in chronological order.  The old linear
                // search made a multi-decade query quadratic in the number
                // of days for every logical product; lower_bound keeps this
                // merge O(log days) and is also correct for duplicate routes
                // on the same date.
                const auto targetDay = SysDay(day);
                const auto dayIt = std::lower_bound(
                    dayFiles.begin(), dayFiles.end(), targetDay,
                    [](const PreciseDayFiles& entry,
                       const std::chrono::sys_days value) {
                        return SysDay(entry.date) < value;
                    });
                if (dayIt == dayFiles.end()
                    || SysDay(dayIt->date) != targetDay) {
                    dayFiles.insert(dayIt, {day, {}, std::move(dayMirrors)});
                    ++result->products[productIndex].availableDays;
                } else {
                    MergeMirrors(dayIt->mirrors, dayMirrors);
                }
        };
        // SP3/CLK/ATT are weekly products and share one indexed schema.  Use
        // the compact family cache so a range query reads product metadata
        // once (without physical URLs); routes are hydrated only if the user
        // actually starts a download.
        bool usedRangeReader = false;
        if (!IsPreciseSearchSummaryCategory(categoryIndex)
            && (categoryIndex == 1 || categoryIndex == 2
                || categoryIndex == 5)) {
            auto range = categoryIndex == 1
                ? LoadSp3ProductFamilyRange(start, end)
                : categoryIndex == 2
                    ? LoadClkProductFamilyRange(start, end)
                    : LoadAttProductFamilyRange(start, end);
            usedRangeReader = range.error.empty();
            processedDays = static_cast<std::size_t>(expectedDays);
            indexedDays = static_cast<std::size_t>(range.indexedDays);
            incompleteDays = range.incompleteDates.size();
            if (!range.error.empty()) {
                warnings.push_back(L"高速产品索引读取失败，改用兼容路径："
                    + range.error);
            } else {
                result->products.reserve(range.families.size());
                result->productDays.reserve(range.families.size());
                result->indexedFamilyKeys.reserve(range.families.size());
                for (const auto& family : range.families) {
                    auto product = categoryIndex == 2
                        ? ToClkProductFile(family.representative)
                        : ToPreciseProductFile(family.representative);
                    if (categoryIndex == 5) {
                        product.category = PreciseProductCategory::Attitude;
                        product.name = Utf8ToWide(family.representative.series)
                            + L" ORBEX 姿态产品";
                    }
                    product.mirrors.clear();
                    product.selected = false;
                    product.availableDays = family.availableDays;
                    product.expectedDays = expectedDays;
                    product.completenessPercent = expectedDays == 0 ? 0.0
                        : 100.0 * static_cast<double>(family.availableDays)
                            / static_cast<double>(expectedDays);
                    product.status = L"高速索引；下载时按日期读取下载源";
                    result->products.push_back(std::move(product));
                    result->productDays.emplace_back();
                    result->indexedFamilyKeys.push_back(family.familyKey);
                }
            }
            if (range.error.empty()) {
                AppendBdsMartProductsForRange(
                    result->products, result->productDays,
                    result->indexedFamilyKeys,
                    start, end, categoryIndex, expectedDays);
            }
        }
        const int ionexCategoryIndex =
            static_cast<int>(PreciseProductCategory::Ionosphere) + 1;
        if (!usedRangeReader && categoryIndex == ionexCategoryIndex
            && !IsPreciseSearchSummaryCategory(categoryIndex)) {
            const auto range = LoadIonexProductFamilyRange(start, end);
            usedRangeReader = range.error.empty();
            processedDays = static_cast<std::size_t>(expectedDays);
            indexedDays = static_cast<std::size_t>(range.indexedDays);
            incompleteDays = range.incompleteDates.size();
            if (!range.error.empty()) {
                warnings.push_back(L"高速 IONEX 索引读取失败，改用兼容路径："
                    + range.error);
            } else {
                result->products.reserve(range.families.size());
                result->productDays.reserve(range.families.size());
                result->indexedFamilyKeys.reserve(range.families.size());
                for (const auto& family : range.families) {
                    auto product = ToIonexProductFile(family.representative);
                    product.mirrors.clear();
                    product.selected = false;
                    product.availableDays = family.availableDays;
                    product.expectedDays = expectedDays;
                    product.completenessPercent = expectedDays == 0 ? 0.0
                        : 100.0 * static_cast<double>(family.availableDays)
                            / static_cast<double>(expectedDays);
                    product.status = L"高速 IONEX 索引；下载时按日期读取下载源";
                    result->products.push_back(std::move(product));
                    result->productDays.emplace_back();
                    result->indexedFamilyKeys.push_back(family.familyKey);
                }
            }
        }
        // DCB/TRO/SINEX/Broadcast have very large route-heavy databases.
        // They must use the compact side index as well; falling back to the
        // historical daily loader here would reopen the same multi-GB file
        // thousands of times for an "all products" query.  A missing side
        // index is reported immediately instead of silently reintroducing
        // that unbounded slow path.
        if (!usedRangeReader
            && IsPreciseSearchSummaryCategory(categoryIndex)) {
            const auto range = LoadPreciseSearchFamilyRange(
                categoryIndex, start, end);
            processedDays = static_cast<std::size_t>(expectedDays);
            indexedDays = static_cast<std::size_t>(range.indexedDays);
            incompleteDays = range.incompleteDates.size();
            usedRangeReader = range.error.empty();
            if (!range.error.empty()) {
                // The daily compatibility path opens a database for every
                // requested day.  A missing summary over decades must fail
                // promptly instead of freezing the application.
                result->status = L"高速精密摘要读取失败：" + range.error;
                result->failed = true;
                return result;
            } else {
                result->products.reserve(range.families.size());
                result->productDays.reserve(range.families.size());
                result->indexedFamilyKeys.reserve(range.families.size());
                for (const auto& family : range.families) {
                    auto product = family.representative;
                    product.mirrors.clear();
                    product.selected = false;
                    product.availableDays = family.availableDays;
                    product.expectedDays = expectedDays;
                    product.completenessPercent = expectedDays == 0 ? 0.0
                        : 100.0 * static_cast<double>(family.availableDays)
                            / static_cast<double>(expectedDays);
                    product.status = L"高速精密摘要；下载时按日期读取下载源";
                    result->products.push_back(std::move(product));
                    result->productDays.emplace_back();
                    result->indexedFamilyKeys.push_back(family.familyKey);
                }
                AppendBdsMartProductsForRange(
                    result->products, result->productDays,
                    result->indexedFamilyKeys, start, end, categoryIndex,
                    expectedDays);
            }
        }
        if (!usedRangeReader
            && categoryIndex == static_cast<int>(PreciseProductCategory::VmfGrid) + 1) {
            const auto range = LoadVmfFamilyRange(start, end);
            if (!range.error.empty()) {
                result->failed = true;
                result->status = range.error
                    + L"；请重新安装官方知识库后重试。";
                return result;
            }
            usedRangeReader = true;
            processedDays = static_cast<std::size_t>(expectedDays);
            for (const auto& family : range.families) {
                PreciseProductFile product;
                product.category = PreciseProductCategory::VmfGrid;
                product.id = Utf8ToWide(family.model + "_" + family.resolution
                    + "_" + family.grade + "_"
                    + family.familyKey.substr(family.familyKey.rfind('|') + 1));
                product.name = Utf8ToWide(family.model + " " + family.resolution
                    + " " + family.grade + " 格网（"
                    + family.familyKey.substr(family.familyKey.rfind('|') + 1)
                    + " UTC）");
                product.center = L"TU Wien";
                product.constellation = L"Earth";
                product.interval = L"06H";
                product.latency = family.grade == "FC"
                    ? PreciseProductLatency::Predicted
                    : family.grade == "EI" ? PreciseProductLatency::Other
                    : PreciseProductLatency::Daily;
                product.availableDays = family.availableDays;
                product.expectedDays = expectedDays;
                product.completenessPercent = expectedDays == 0 ? 0.0
                    : 100.0 * static_cast<double>(family.availableDays)
                        / static_cast<double>(expectedDays);
                product.status = L"TU Wien 官方目录；下载时读取实有文件路线";
                result->products.push_back(std::move(product));
                result->productDays.emplace_back();
                result->indexedFamilyKeys.push_back(family.familyKey);
                indexedDays = std::max(indexedDays,
                    static_cast<std::size_t>(family.availableDays));
            }
            incompleteDays = static_cast<std::size_t>(expectedDays)
                - std::min(static_cast<std::size_t>(expectedDays), indexedDays);
        }
        const auto dateCode = [](const QueryDate& value) {
            return value.year * 10000 + value.month * 100 + value.day;
        };
        const auto hasDate = [](const QueryDate& value) {
            return value.year > 0 && value.month > 0 && value.day > 0;
        };
        const auto consumeCoverage = [&](const QueryDate& first,
                                          const QueryDate& last,
                                          const PreciseProductFile& product) {
            const int firstCode = hasDate(first) ? dateCode(first) : dateCode(start);
            const int lastCode = hasDate(last) ? dateCode(last) : dateCode(end);
            for (QueryDate day = start;; day = AddDays(day, 1)) {
                const int code = dateCode(day);
                if (code >= firstCode && code <= lastCode) {
                    consumeProduct(day, product);
                }
                if (SameQueryDate(day, end)) break;
            }
        };
        if (!usedRangeReader && categoryIndex == 3) {
            const auto range = LoadErpAvailabilityRange(start, end);
            usedRangeReader = range.error.empty();
            processedDays = static_cast<std::size_t>(expectedDays);
            indexedDays = static_cast<std::size_t>(range.indexedDays);
            incompleteDays = range.incompleteDates.size();
            if (!range.error.empty()) warnings.push_back(range.error);
            else for (const auto& record : range.products) {
                QueryDate first = hasDate(record.coverageStart)
                    ? record.coverageStart : record.nominalDate;
                QueryDate last = hasDate(record.coverageEnd)
                    ? record.coverageEnd : AddDays(record.nominalDate, 6);
                consumeCoverage(first, last, ToErpProductFile(record));
            }
        }
        if (!usedRangeReader && categoryIndex == 9) {
            const auto range = LoadAntexAvailabilityRange(start, end);
            usedRangeReader = range.error.empty();
            processedDays = static_cast<std::size_t>(expectedDays);
            indexedDays = static_cast<std::size_t>(range.indexedDays);
            incompleteDays = range.incompleteDates.size();
            if (!range.error.empty()) {
                result->status = L"ANTEX 索引读取失败：" + range.error;
                result->failed = true;
                return result;
            }
            for (const auto& record : range.products) {
                const auto first = hasDate(record.validFrom)
                    ? std::max(SysDay(start), SysDay(record.validFrom))
                    : SysDay(start);
                const auto last = hasDate(record.validTo)
                    ? std::min(SysDay(end), SysDay(record.validTo))
                    : SysDay(end);
                if (last < first) continue;
                auto product = ToAntexProductFile(record);
                const auto key = ProductLogicalKey(product);
                const auto [it, inserted] = byKey.emplace(
                    key, result->products.size());
                const auto productIndex = it->second;
                if (inserted) {
                    product.selected = false;
                    result->products.push_back(std::move(product));
                    result->productDays.emplace_back();
                    result->indexedFamilyKeys.emplace_back();
                    result->coverageRanges.emplace_back();
                } else {
                    MergeMirrors(result->products[productIndex].mirrors,
                        product.mirrors);
                }
                result->coverageRanges[productIndex].push_back({
                    AddDays(start, static_cast<int>((first - SysDay(start)).count())),
                    AddDays(start, static_cast<int>((last - SysDay(start)).count()))});
            }
            for (std::size_t index = 0; index < result->products.size(); ++index) {
                auto& intervals = result->coverageRanges[index];
                std::sort(intervals.begin(), intervals.end(),
                    [](const auto& left, const auto& right) {
                        return SysDay(left.first) < SysDay(right.first);
                    });
                std::vector<PreciseCoverageRange> merged;
                for (const auto& interval : intervals) {
                    if (!merged.empty() && SysDay(interval.first)
                        <= SysDay(merged.back().last) + std::chrono::days{1}) {
                        if (SysDay(interval.last) > SysDay(merged.back().last))
                            merged.back().last = interval.last;
                    } else {
                        merged.push_back(interval);
                    }
                }
                intervals = std::move(merged);
                for (const auto& interval : intervals) {
                    result->products[index].availableDays +=
                        InclusiveDays(interval.first, interval.last);
                }
            }
        }
        if (!usedRangeReader && categoryIndex == 11) {
            const auto range = LoadOsbAvailabilityRange(start, end);
            usedRangeReader = range.error.empty();
            processedDays = static_cast<std::size_t>(expectedDays);
            indexedDays = static_cast<std::size_t>(range.indexedDays);
            incompleteDays = range.incompleteDates.size();
            if (!range.error.empty()) warnings.push_back(range.error);
            else for (const auto& record : range.products) {
                consumeCoverage(record.coverageStart, record.coverageEnd,
                    ToOsbProductFile(record));
            }
            if (usedRangeReader) {
                AppendBdsMartProductsForRange(
                    result->products, result->productDays,
                    result->indexedFamilyKeys, start, end, categoryIndex,
                    expectedDays);
            }
        }
        if (!usedRangeReader && categoryIndex == 12) {
            const auto range = LoadUpdAvailabilityRange(start, end);
            usedRangeReader = range.error.empty();
            processedDays = static_cast<std::size_t>(expectedDays);
            indexedDays = static_cast<std::size_t>(range.indexedDays);
            incompleteDays = range.incompleteDates.size();
            if (!range.error.empty()) warnings.push_back(range.error);
            else for (const auto& record : range.products) {
                consumeCoverage(record.nominalDate, record.nominalDate,
                    ToUpdProductFile(record));
            }
        }
        if (!usedRangeReader) for (QueryDate day = start;; day = AddDays(day, 1)) {
            if (stopToken.stop_requested()) {
                result->cancelled = true;
                return result;
            }
            const auto daily = LoadDayCatalog(day, categoryIndex, troStation);
            ++processedDays;
            if (daily.indexed) ++indexedDays;
            if (!daily.complete) ++incompleteDays;
            if (!daily.error.empty() && warnings.size() < 3) {
                warnings.push_back(FormatDate(day) + L"：" + daily.error);
            }
            for (const auto& product : daily.products) {
                consumeProduct(day, product);
            }
            if (day.year == end.year && day.month == end.month && day.day == end.day) {
                break;
            }
        }
        for (auto& product : result->products) {
            product.expectedDays = expectedDays;
            product.completenessPercent = expectedDays == 0 ? 0.0
                : 100.0 * static_cast<double>(product.availableDays)
                    / static_cast<double>(expectedDays);
            product.status += std::format(
                L"；范围可用 {}/{} 天 ({:.1f}%)",
                product.availableDays, expectedDays,
                product.completenessPercent);
        }
        result->coverageRanges.resize(result->products.size());
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt).count();
        result->status = std::format(
            L"精密产品查询范围 {} 至 {}：{} 个逻辑产品，已检查 {} 天；"
            L"知识库有索引 {} 天，待复扫/不完整 {} 天；读取耗时 {:.1f} 秒。",
            FormatDate(start), FormatDate(end), result->products.size(), processedDays,
            indexedDays, incompleteDays,
            static_cast<double>(elapsedMs) / 1000.0);
        if (!warnings.empty()) {
            result->status += L" 首个读取提示：" + warnings.front();
        }
        return result;
    }

    void GenerateCatalogRange(
        const QueryDate& start, const QueryDate& end, const int categoryIndex) {
        if (catalogLoading_) return;
        const int expectedDays = InclusiveDays(start, end);
        if (expectedDays <= 0) {
            NativeUiLanguage::ShowMessageBox(window_, L"日期范围无效：终止日期必须不早于起始日期。",
                L"GNSS 数据下载器", MB_OK | MB_ICONWARNING);
            return;
        }
        if (catalogThread_.joinable()) catalogThread_.join();
        catalogLoading_ = true;
        SetCatalogControlsEnabled(false);
        SetWindowTextW(status_, std::format(
            L"正在后台读取精密产品知识库：{} 至 {}…",
            FormatDate(start), FormatDate(end)).c_str());
        PublishUiState();
        const HWND target = window_;
        const std::wstring troStation = WindowText(troStation_);
        catalogThread_ = std::jthread(
            [this, target, start, end, categoryIndex, troStation](
                const std::stop_token stopToken) {
                std::unique_ptr<PreciseCatalogResult> result;
                try {
                    PrewarmPreciseRangeIndexes();
                    result = BuildCatalogRange(
                        stopToken, start, end, categoryIndex, troStation);
                } catch (...) {
                    result = std::make_unique<PreciseCatalogResult>();
                    result->start = start;
                    result->end = end;
                    result->cancelled = stopToken.stop_requested();
                    result->status = DescribeCurrentException();
                }
                if (!result) {
                    return;
                }
                auto* payload = result.release();
                if (!PostMessageW(target, WM_PRECISE_CATALOG_FINISHED, 0,
                                  reinterpret_cast<LPARAM>(payload))) {
                    delete payload;
                }
            });
    }

    void HandleCatalogFinished(std::unique_ptr<PreciseCatalogResult> result) {
        catalogLoading_ = false;
        SetCatalogControlsEnabled(true);
        if (!result || result->cancelled) {
            SetWindowTextW(status_, L"精密产品查询已取消。");
            PublishUiState();
            return;
        }
        if (result->failed) {
            SetWindowTextW(status_, result->status.c_str());
            PublishUiState();
            return;
        }
        ++catalogRevision_;
        products_ = std::move(result->products);
        productDays_ = std::move(result->productDays);
        coverageRanges_ = std::move(result->coverageRanges);
        indexedFamilyKeys_ = std::move(result->indexedFamilyKeys);
        periodStart_ = result->start;
        periodEnd_ = result->end;
        currentDate_ = result->start;
        RebuildList();
        SetWindowTextW(status_, result->status.c_str());
        PublishUiState();
    }

    void HandleQuickBundleFinished(
        std::unique_ptr<PreciseQuickBundleResult> result) {
        const bool continueDownload = quickBundleDownloadPending_;
        quickBundleDownloadPending_ = false;
        const bool continueQueue = quickBundleQueuePending_;
        quickBundleQueuePending_ = false;
        catalogLoading_ = false;
        SetCatalogControlsEnabled(true);
        if (catalogThread_.get_stop_token().stop_requested()) {
            SendMessageW(progress_, PBM_SETPOS, 0, 0);
            SetWindowTextW(status_, L"下载任务已取消，等待重新开始。");
            PublishUiState();
            return;
        }
        if (!result) {
            SetWindowTextW(status_, L"快速方案后台查询未返回结果。");
            PublishUiState();
            return;
        }
        completedQuickBundle_ = std::move(result);
        // Final list construction touches Win32 controls and therefore stays
        // on the window thread; all database and planning work is complete.
        QueryQuickBundle();
        if (continueQueue && quickBundleReady_) {
            AddSelectedToDownloadPlan();
        }
        if (continueDownload && quickBundleReady_
            && std::ranges::any_of(products_, [](const auto& product) {
                return product.selected;
            })) {
            StartOrCancelDownload();
        }
        PublishUiState();
    }

    void ApplyQuickPreset() {
        const int preset = static_cast<int>(SendMessageW(
            quickPreset_, CB_GETCURSEL, 0, 0));
        if (preset < 0) {
            return;
        }

        // All three first-release plans start with the SP3 orbit family.  The
        // regular list remains available so users can inspect or download a
        // particular member.  Combo indices are set explicitly to avoid
        // relying on display strings (which are localised in the UI).
        SendMessageW(category_, CB_SETCURSEL, preset == 0 ? 0 : 1, 0);
        if (preset == 1) {
            SendMessageW(latency_, CB_SETCURSEL, 2, 0); // 快速
            SendMessageW(center_, CB_SETCURSEL, 12, 0); // WHU/WUM
            SetWindowTextW(quickInfo_,
                L"WUM RAPID SP3、CLK、ERP、ORBEX、OSB/BIA + IGS ANTEX、"
                L"IGS 每日 SINEX、CODE Final GIM、VMF1/VMF3 OP 四时次、"
                L"IGS 每日 RINEX 3 BRDC；缺口会按日期区间显示在运行窗口。");
            SetWindowTextW(status_,
                L"已应用 PRIDE PPP-AR 方案；"
                L"正在核对 WUM RAPID 与辅助产品…");
        } else if (preset == 2) {
            SendMessageW(latency_, CB_SETCURSEL, 1, 0); // 最终
            SendMessageW(center_, CB_SETCURSEL, 1, 0); // IGS
            SetWindowTextW(quickInfo_,
                L"标准 PPP：IGS Final SP3 + CLK + ERP/EOP + ANTEX；"
                L"适合浮点 PPP，Bias/ORBEX 等可按需另行选择。");
            SetWindowTextW(status_,
                L"已应用标准 IGS Final PPP 方案；正在查询 IGS Final SP3 起始产品…");
        } else if (preset == 3) {
            SendMessageW(latency_, CB_SETCURSEL, 1, 0); // 最终
            SendMessageW(center_, CB_SETCURSEL, 0, 0); // 全部机构；UPD按AC匹配
            SetWindowTextW(quickInfo_,
                L"UPD PPP-AR 兼容模式：SP3 + CLK + 对应分析中心 UPD + ERP + "
                L"ANTEX；UPD 不能与其他机构的钟差/偏差混用。");
            SetWindowTextW(status_,
                L"已应用 UPD PPP-AR 兼容方案；请按分析中心检查 UPD 与轨道/钟差是否成套。");
        } else if (preset == 4) {
            SendMessageW(latency_, CB_SETCURSEL, 0, 0);
            SendMessageW(center_, CB_SETCURSEL, 0, 0);
            SetWindowTextW(quickInfo_,
                L"OSB PPP-AR：SP3 + CLK + OSB + ERP + ANTEX；"
                L"SP3、CLK 与 OSB 必须属于同一分析中心和产品线，不跨机构拼接。");
            SetWindowTextW(status_, L"已应用 OSB PPP-AR 同机构方案，正在核对产品族。");
        } else if (preset == 5) {
            SendMessageW(latency_, CB_SETCURSEL, 0, 0);
            SendMessageW(center_, CB_SETCURSEL, 0, 0);
            SetWindowTextW(quickInfo_,
                L"PPP-RTK / 网络 RTK 通用精密产品：SP3 + CLK + OSB + ERP + ANTEX；"
                L"区域大气改正和实时播发不属于本地精密文件库，需由用户的服务端另行提供。");
            SetWindowTextW(status_, L"已应用 PPP-RTK / 网络 RTK 通用精密产品方案。");
        } else if (preset == 6) {
            SendMessageW(latency_, CB_SETCURSEL, 0, 0);
            SendMessageW(center_, CB_SETCURSEL, 0, 0);
            SetWindowTextW(quickInfo_,
                L"通用 PPP 智能优选：自动选择一套完整的 SP3 轨道、CLK 钟差、ERP 和 ANTEX；"
                L"轨道与钟差保持同机构同产品线，优先完整覆盖，再选 Final 和高采样率钟差。");
            SetWindowTextW(status_, L"已应用通用 PPP 智能优选方案，正在比较分析中心与产品族。");
        } else {
            // Clearing a quick preset restores the unrestricted product list.
            SendMessageW(latency_, CB_SETCURSEL, 0, 0);
            SendMessageW(center_, CB_SETCURSEL, 0, 0);
            SetWindowTextW(quickInfo_,
                L"快速方案未启用；可继续使用下方日期、类型、等级和机构筛选。");
            SetWindowTextW(status_, L"已清除快速下载方案。请按需要查询本地知识库。");
        }
        // Rebuild the visible list with the selected filters before starting
        // the range query.  Calling GenerateCatalog also validates the date
        // range and preserves the existing no-network query semantics.
        if (preset == 0) {
            GenerateCatalog();
            return;
        }
        QueryQuickBundle();
    }

    bool ReadDateRange(QueryDate& start, QueryDate& end) const {
        SYSTEMTIME selected{};
        if (DateTime_GetSystemtime(date_, &selected) != GDT_VALID) {
            NativeUiLanguage::ShowMessageBox(window_, L"请选择有效日期。", L"GNSS 数据下载器",
                MB_OK | MB_ICONWARNING);
            return false;
        }
        start = MakeQueryDate(selected.wYear, selected.wMonth, selected.wDay);
        if (start.dayOfYear <= 0) {
            NativeUiLanguage::ShowMessageBox(window_, L"起始日期无效。", L"GNSS 数据下载器",
                MB_OK | MB_ICONWARNING);
            return false;
        }
        SYSTEMTIME selectedEnd{};
        if (DateTime_GetSystemtime(endDate_, &selectedEnd) != GDT_VALID) {
            NativeUiLanguage::ShowMessageBox(window_, L"请选择有效的终止日期。",
                L"GNSS 数据下载器", MB_OK | MB_ICONWARNING);
            return false;
        }
        end = MakeQueryDate(
            selectedEnd.wYear, selectedEnd.wMonth, selectedEnd.wDay);
        if (end.dayOfYear <= 0) {
            NativeUiLanguage::ShowMessageBox(window_, L"终止日期无效。", L"GNSS 数据下载器",
                MB_OK | MB_ICONWARNING);
            return false;
        }
        if (InclusiveDays(start, end) <= 0) {
            NativeUiLanguage::ShowMessageBox(window_, L"日期范围无效：终止日期必须不早于起始日期。",
                L"GNSS 数据下载器", MB_OK | MB_ICONWARNING);
            return false;
        }
        return true;
    }

    PreciseBundleCatalog LoadBundleCatalog(
        const QueryDate& start,
        const QueryDate& end,
        const PreciseBundleRequest& request,
        std::vector<std::wstring>& warnings,
        const bool hydrateRoutes = false,
        const std::unordered_set<std::string>* hydrateFamilyKeys = nullptr,
        const std::stop_token stopToken = {}) const {
        PreciseBundleCatalog catalog;
        const auto plan = request.plan;
        const auto required = RequiredBundleProductTypes(plan);
        const auto isRequired = [&required](
                                    const PreciseBundleProductType type) {
            return std::ranges::find(required, type) != required.end();
        };
        const auto allDates = [&] {
            std::vector<QueryDate> dates;
            const int count = InclusiveDays(start, end);
            dates.reserve(static_cast<std::size_t>(std::max(0, count)));
            for (int index = 0; index < count; ++index) {
                dates.push_back(AddDays(start, index));
            }
            return dates;
        };
        const auto recordAvailability = [&](const PreciseBundleProductType type,
                                             const bool indexed,
                                             const bool complete,
                                             const std::vector<QueryDate>& gaps,
                                             const std::wstring& error,
                                             const wchar_t* label) {
            auto unavailable = gaps;
            if (!error.empty()) {
                warnings.push_back(std::wstring(label) + L" 范围读取失败：" + error);
            }
            // A range reader that cannot prove a date was scanned must not
            // let a stale product row turn that date into a complete bundle.
            if (!error.empty() || !indexed || (!complete && unavailable.empty())) {
                unavailable = allDates();
            }
            if (!unavailable.empty()) {
                auto& destination = catalog.unavailableDates[type];
                for (const auto& date : unavailable) {
                    if (std::ranges::none_of(destination,
                            [&](const QueryDate& existing) {
                                return SameQueryDate(existing, date);
                            })) {
                        destination.push_back(date);
                    }
                }
            }
        };

        const auto appendWumVmf = [&] {
            if (plan != PreciseBundlePlan::WumPridePppAr) return;
            const auto families = LoadVmfFamilyRange(start, end);
            if (!families.error.empty()) {
                warnings.push_back(L"VMF 范围读取失败：" + families.error);
                return;
            }
            const std::array<std::pair<PreciseBundleProductType,
                const char*>, 8> requiredVmf{{
                {PreciseBundleProductType::Vmf1H00, "VMF1|2.5x2|OP|H00"},
                {PreciseBundleProductType::Vmf1H06, "VMF1|2.5x2|OP|H06"},
                {PreciseBundleProductType::Vmf1H12, "VMF1|2.5x2|OP|H12"},
                {PreciseBundleProductType::Vmf1H18, "VMF1|2.5x2|OP|H18"},
                {PreciseBundleProductType::Vmf3H00, "VMF3|1x1|OP|H00"},
                {PreciseBundleProductType::Vmf3H06, "VMF3|1x1|OP|H06"},
                {PreciseBundleProductType::Vmf3H12, "VMF3|1x1|OP|H12"},
                {PreciseBundleProductType::Vmf3H18, "VMF3|1x1|OP|H18"},
            }};
            for (const auto& [type, familyKey] : requiredVmf) {
                if (stopToken.stop_requested()) return;
                const auto found = std::ranges::find_if(families.families,
                    [&](const VmfFamilyRecord& value) {
                        return value.familyKey == familyKey;
                    });
                if (found == families.families.end()) continue;
                const auto routes = LoadVmfFamilyRoutes(
                    familyKey, start, end, stopToken);
                if (!routes.error.empty()) {
                    warnings.push_back(L"VMF 下载路线读取失败："
                        + routes.error);
                    continue;
                }
                for (const auto& dated : routes.records) {
                    PreciseBundleProduct product;
                    product.type = type;
                    product.logicalKey = dated.logicalKey;
                    const auto hour = found->familyKey.substr(
                        found->familyKey.rfind('|') + 1);
                    product.displayId = Utf8ToWide(found->model + "_"
                        + found->resolution + "_" + found->grade + "_" + hour);
                    product.displayName = Utf8ToWide(found->model + " "
                        + found->resolution + " " + found->grade + " 格网（"
                        + hour + " UTC）");
                    product.nominalDate = dated.date;
                    product.coverageStart = dated.date;
                    product.coverageEnd = dated.date;
                    product.fileName = WideToUtf8(dated.mirror.fileName);
                    product.center = "TU Wien";
                    product.series = found->model;
                    product.productLine = "OP";
                    product.latency = "DAILY";
                    product.constellation = "Earth";
                    product.interval = "06H";
                    product.routes.push_back({
                        WideToUtf8(dated.mirror.source),
                        product.fileName, dated.mirror.url});
                    catalog.products.push_back(std::move(product));
                }
            }
        };

        // Query-time quick planning uses only the process-wide bitmap
        // summary.  It deliberately does not open the multi-gigabyte SP3,
        // CLK, ERP or OSB route databases.  Each selected family is split
        // into exact contiguous date runs, so sparse archive coverage is not
        // mistaken for a continuous product.  Physical file names and mirror
        // URLs are hydrated only after the user confirms a download.
        if (!hydrateRoutes) {
            const auto appendCompact = [&](const int categoryIndex,
                                           const PreciseBundleProductType type,
                                           const wchar_t* label) {
                const auto range = LoadPreciseSearchFamilyRange(
                    categoryIndex, start, end, true);
                const bool indexed = range.error.empty()
                    && (range.complete || range.indexedDays > 0
                        || !range.families.empty());
                recordAvailability(type, indexed, range.complete,
                    range.incompleteDates, range.error, label);
                if (!range.error.empty()) return;
                AppendCompactBundleFamilyRuns(
                    catalog, range, categoryIndex, type, request);
            };

            if (isRequired(PreciseBundleProductType::Sp3)) {
                appendCompact(1, PreciseBundleProductType::Sp3, L"SP3");
            }
            if (isRequired(PreciseBundleProductType::Clk)) {
                appendCompact(2, PreciseBundleProductType::Clk, L"CLK");
            }
            if (isRequired(PreciseBundleProductType::Osb)) {
                appendCompact(11, PreciseBundleProductType::Osb, L"OSB");
            }
            if (isRequired(PreciseBundleProductType::Erp)) {
                appendCompact(3, PreciseBundleProductType::Erp, L"ERP/EOP");
            }
            if (isRequired(PreciseBundleProductType::Att)) {
                appendCompact(5, PreciseBundleProductType::Att, L"ORBEX");
            }
            if (plan == PreciseBundlePlan::WumPridePppAr) {
                appendCompact(8, PreciseBundleProductType::SinexCrd,
                    L"IGS SINEX CRD");
                appendCompact(8, PreciseBundleProductType::SinexSol,
                    L"IGS SINEX SOL");
                appendCompact(6, PreciseBundleProductType::Gim,
                    L"CODE GIM Final");
                appendCompact(10, PreciseBundleProductType::Brdc,
                    L"IGS BRDC");
                appendWumVmf();
            }

            // ANTEX and UPD are small catalogues compared with the route-heavy
            // daily archives.  Keep their existing adapters here; this also
            // preserves the date-effective calibration selection semantics.
            if (isRequired(PreciseBundleProductType::Antex)) {
                const auto antex = LoadAntexAvailabilityRange(start, end);
                recordAvailability(PreciseBundleProductType::Antex, antex.indexed,
                    antex.complete, antex.incompleteDates, antex.error, L"ANTEX");
                for (const auto& record : antex.products) {
                    if (!IsQuickBundleAntexCandidate(record)) continue;
                    catalog.products.push_back(ToBundleProduct(record));
                }
            }
            if (isRequired(PreciseBundleProductType::Upd)) {
                const auto upd = LoadUpdAvailabilityRange(start, end);
                recordAvailability(PreciseBundleProductType::Upd, upd.indexed,
                    upd.complete, upd.incompleteDates, upd.error, L"UPD");
                for (const auto& record : upd.products) {
                    auto product = ToBundleProduct(record);
                    if (BundleFamilyMayMatchRequest(product, request)) {
                        catalog.products.push_back(std::move(product));
                    }
                }
            }
            return catalog;
        }

        // SP3, CLK and ATT have very large route tables.  Read their compact
        // family summaries first and hydrate only families that could satisfy
        // this named request.  This keeps a multi-year quick plan bounded by
        // the selected centre/latency instead of materialising every mirror
        // in the database.
        const auto appendHydratedRecords = [&](const PreciseBundleProductType type,
                                                const wchar_t* label,
                                                const auto& hydrated) {
            if (!hydrated.error.empty()) {
                if (warnings.size() < 8) {
                    warnings.push_back(std::wstring(label)
                        + L" 产品族路线读取失败：" + hydrated.error);
                }
                return;
            }
            for (const auto& dated : hydrated.records) {
                auto product = ToBundleProduct(dated.record, type);
                if (product.fileName.empty() || product.routes.empty()) {
                    continue;
                }
                catalog.products.push_back(std::move(product));
            }
        };
        const auto appendSelectedFamilyRoutes = [&]( 
            const int categoryIndex,
            const PreciseBundleProductType type,
            const wchar_t* label) {
            if (hydrateFamilyKeys == nullptr || stopToken.stop_requested()) return;
            const auto range = LoadPreciseSearchFamilyRange(
                categoryIndex, start, end);
            recordAvailability(type, range.error.empty()
                    && (range.complete || range.indexedDays > 0),
                range.complete, range.incompleteDates, range.error, label);
            if (!range.error.empty()) return;
            const std::string prefix = std::to_string(
                static_cast<int>(type)) + "\x1f";
            for (const auto& encoded : *hydrateFamilyKeys) {
                if (stopToken.stop_requested()) return;
                if (!encoded.starts_with(prefix)) continue;
                const auto familyKey = encoded.substr(prefix.size());
                const auto family = std::ranges::find_if(range.families,
                    [&](const PreciseSearchFamilyRecord& row) {
                        return row.familyKey == familyKey;
                    });
                if (family == range.families.end()) {
                    warnings.push_back(std::wstring(label)
                        + L" 产品族已从快速摘要中消失");
                    continue;
                }
                const auto routes = LoadPreciseSearchFamilyRoutes(
                    categoryIndex, familyKey, start, end, stopToken);
                if (!routes.error.empty()) {
                    if (!stopToken.stop_requested()) {
                        warnings.push_back(std::wstring(label)
                            + L" 下载路线读取失败：" + routes.error);
                    }
                    continue;
                }
                for (const auto& dated : routes.records) {
                    auto product = ToCompactBundleProduct(
                        *family, type, dated.date, dated.date);
                    product.logicalKey = dated.logicalKey;
                    product.routesDeferred = false;
                    for (const auto& mirror : dated.mirrors) {
                        product.routes.push_back({WideToUtf8(mirror.source),
                            WideToUtf8(mirror.fileName), mirror.url});
                    }
                    product.fileName = FirstRouteFileName(product.routes);
                    if (!product.fileName.empty()) {
                        catalog.products.push_back(std::move(product));
                    }
                }
            }
        };
        const auto appendFamilyRange = [&](const auto& range,
                                            const PreciseBundleProductType type,
                                            const wchar_t* label,
                                            const auto& hydrate) {
            const bool indexed = range.error.empty()
                && (range.complete || range.indexedDays > 0
                    || !range.families.empty());
            recordAvailability(type, indexed, range.complete,
                range.incompleteDates, range.error, label);
            if (!range.error.empty()) return;
            for (const auto& family : range.families) {
                const auto representative = ToBundleProduct(
                    family.representative, type);
                if (!BundleFamilyMayMatchRequest(representative, request)) {
                    continue;
                }
                const auto hydrated = hydrate(family.familyKey);
                appendHydratedRecords(type, label, hydrated);
                if (hydrated.records.size()
                    < static_cast<std::size_t>(std::max(0,
                        family.availableDays))
                    && warnings.size() < 8) {
                    warnings.push_back(std::wstring(label)
                        + L" 产品族部分日期没有可用下载路线："
                        + Utf8ToWide(representative.series));
                }
            }
        };

        if (isRequired(PreciseBundleProductType::Sp3)) {
            const auto hydrate = [&](const std::string& familyKey) {
                return LoadSp3ProductFamilyRoutes(
                    familyKey, start, end, {}, {});
            };
            if (hydrateRoutes && hydrateFamilyKeys != nullptr) {
                appendSelectedFamilyRoutes(
                    1, PreciseBundleProductType::Sp3, L"SP3");
            } else {
                const auto sp3 = LoadSp3ProductFamilyRange(start, end);
                appendFamilyRange(sp3, PreciseBundleProductType::Sp3, L"SP3",
                    hydrate);
            }
        }

        if (isRequired(PreciseBundleProductType::Clk)) {
            const auto hydrate = [&](const std::string& familyKey) {
                return LoadClkProductFamilyRoutes(
                    familyKey, start, end, {}, {});
            };
            if (hydrateRoutes && hydrateFamilyKeys != nullptr) {
                appendSelectedFamilyRoutes(
                    2, PreciseBundleProductType::Clk, L"CLK");
            } else {
                const auto clk = LoadClkProductFamilyRange(start, end);
                appendFamilyRange(clk, PreciseBundleProductType::Clk, L"CLK",
                    hydrate);
            }
        }

        if (isRequired(PreciseBundleProductType::Osb)) {
            const auto osb = LoadOsbAvailabilityRange(start, end);
            recordAvailability(PreciseBundleProductType::Osb, osb.indexed,
                osb.complete, osb.incompleteDates, osb.error, L"OSB");
            for (const auto& record : osb.products) {
                auto product = ToBundleProduct(record);
                if (BundleFamilyMayMatchRequest(product, request)) {
                    catalog.products.push_back(std::move(product));
                }
            }
        }

        if (isRequired(PreciseBundleProductType::Erp)) {
            const auto erp = LoadErpAvailabilityRange(start, end);
            recordAvailability(PreciseBundleProductType::Erp, erp.indexed,
                erp.complete, erp.incompleteDates, erp.error, L"ERP/EOP");
            for (const auto& record : erp.products) {
                auto product = ToBundleProduct(record);
                if (BundleFamilyMayMatchRequest(product, request)) {
                    catalog.products.push_back(std::move(product));
                }
            }
        }

        if (isRequired(PreciseBundleProductType::Att)) {
            const auto hydrate = [&](const std::string& familyKey) {
                return LoadAttProductFamilyRoutes(
                    familyKey, start, end, {}, {});
            };
            if (hydrateRoutes && hydrateFamilyKeys != nullptr) {
                appendSelectedFamilyRoutes(
                    5, PreciseBundleProductType::Att, L"ORBEX");
            } else {
                const auto att = LoadAttProductFamilyRange(start, end);
                appendFamilyRange(att, PreciseBundleProductType::Att,
                    L"ORBEX", hydrate);
            }
        }
        if (plan == PreciseBundlePlan::WumPridePppAr) {
            appendSelectedFamilyRoutes(8,
                PreciseBundleProductType::SinexCrd, L"IGS SINEX CRD");
            appendSelectedFamilyRoutes(8,
                PreciseBundleProductType::SinexSol, L"IGS SINEX SOL");
            appendSelectedFamilyRoutes(6,
                PreciseBundleProductType::Gim, L"CODE GIM Final");
            appendSelectedFamilyRoutes(10,
                PreciseBundleProductType::Brdc, L"IGS BRDC");
            appendWumVmf();
        }

        if (isRequired(PreciseBundleProductType::Upd)) {
            const auto upd = LoadUpdAvailabilityRange(start, end);
            recordAvailability(PreciseBundleProductType::Upd, upd.indexed,
                upd.complete, upd.incompleteDates, upd.error, L"UPD");
            for (const auto& record : upd.products) {
                auto product = ToBundleProduct(record);
                if (BundleFamilyMayMatchRequest(product, request)) {
                    catalog.products.push_back(std::move(product));
                }
            }
        }

        if (isRequired(PreciseBundleProductType::Antex)) {
            const auto antex = LoadAntexAvailabilityRange(start, end);
            recordAvailability(PreciseBundleProductType::Antex, antex.indexed,
                antex.complete, antex.incompleteDates, antex.error, L"ANTEX");
            std::size_t acceptedAntex = 0;
            for (const auto& record : antex.products) {
                if (!IsQuickBundleAntexCandidate(record)) continue;
                auto product = ToBundleProduct(record);
                // Analysis-centre filters do not apply to ANTEX; the adapter
                // above has already selected the global IGS APC/PCV family,
                // and the planner checks its date-effective frame/model.
                catalog.products.push_back(std::move(product));
                ++acceptedAntex;
            }
            if (antex.error.empty() && acceptedAntex == 0) {
                warnings.push_back(
                    L"ANTEX 索引中没有适用的 IGS GLOBAL 卫星/接收机标准校准文件。");
            }
        }
        return catalog;
    }

    std::wstring BuildQuickBundleReport(
        const PreciseBundlePlanResult& result,
        const QueryDate& start,
        const QueryDate& end,
        const std::vector<std::wstring>& warnings) const {
        std::wstring report = L"方案："
            + Utf8ToWide(PreciseBundlePlanName(result.plan))
            + L"；范围：" + FormatDate(start) + L" 至 " + FormatDate(end);
        if (!result.valid) {
            report += L"；状态：无法生成";
            if (!result.error.empty()) {
                report += L"；原因：" + Utf8ToWide(result.error);
            }
            return report;
        }
        report += L"；整套完整 " + std::to_wstring(result.completeDays)
            + L"/" + std::to_wstring(result.expectedDays) + L" 天";
        if (!result.complete) {
            // Group consecutive dates with the same missing members.  Keep
            // the run-window entry bounded even for multi-year requests.
            struct MissingInterval {
                QueryDate first{};
                QueryDate last{};
                std::vector<PreciseBundleProductType> types;
            };
            std::vector<MissingInterval> intervals;
            for (const auto& status : result.dates) {
                if (status.complete) continue;
                if (!intervals.empty()
                    && intervals.back().types == status.missing
                    && SameQueryDate(AddDays(intervals.back().last, 1),
                        status.date)) {
                    intervals.back().last = status.date;
                } else {
                    intervals.push_back({status.date, status.date,
                        status.missing});
                }
            }
            constexpr std::size_t kShownIntervals = 4;
            report += L"；缺口：";
            for (std::size_t index = 0;
                 index < std::min(intervals.size(), kShownIntervals);
                 ++index) {
                if (index) report += L"；";
                const auto& interval = intervals[index];
                report += FormatDate(interval.first);
                if (!SameQueryDate(interval.first, interval.last)) {
                    report += L"—" + FormatDate(interval.last);
                }
                report += L" 缺 ";
                for (std::size_t typeIndex = 0;
                     typeIndex < interval.types.size(); ++typeIndex) {
                    if (typeIndex) report += L"/";
                    report += BundleTypeLabel(interval.types[typeIndex]);
                }
            }
            if (intervals.size() > kShownIntervals) {
                report += L"；另 "
                    + std::to_wstring(intervals.size() - kShownIntervals)
                    + L" 段，详见清单完整率";
            }
        }
        if (!result.warning.empty()) {
            report += L"；兼容性提示：" + Utf8ToWide(result.warning);
        }
        if (!warnings.empty()) {
            report += L"；索引提示：" + warnings.front().substr(0, 120);
            if (warnings.size() > 1) {
                report += L"（另 " + std::to_wstring(warnings.size() - 1)
                    + L" 条）";
            }
        }
        return report;
    }

    void QueryQuickBundle() {
        KillTimer(window_, kPrecisionCatalogTimer);
        std::unique_ptr<PreciseQuickBundleResult> completed;
        if (completedQuickBundle_) {
            completed = std::move(completedQuickBundle_);
        } else {
            if (catalogLoading_) return;
            // Discard a previous plan before validating a new request.
            InvalidateQuickBundle();
            const int preset = static_cast<int>(SendMessageW(
                quickPreset_, CB_GETCURSEL, 0, 0));
            if (preset <= 0) {
                NativeUiLanguage::ShowMessageBox(window_, L"请先选择一个快速下载方案。",
                    L"GNSS 数据下载器", MB_OK | MB_ICONINFORMATION);
                return;
            }
            QueryDate start{};
            QueryDate end{};
            if (!ReadDateRange(start, end)) return;

            PreciseBundleRequest request;
            request.startDate = start;
            request.endDate = end;
            request.strictMatching = true;
            if (preset == 1) {
                request.plan = PreciseBundlePlan::WumPridePppAr;
                request.latency = PreciseBundleLatency::Rapid;
                request.center = "WUM";
                request.series = "WUM0MGXRAP";
                request.productLine = "MGX";
                request.clockInterval = "30S";
            } else if (preset == 2) {
                request.plan = PreciseBundlePlan::StandardPpp;
                request.latency = PreciseBundleLatency::Final;
                request.center = "IGS";
                request.productLine = "OPS";
                request.clockInterval = "30S";
            } else if (preset == 3) {
                request.plan = PreciseBundlePlan::UpdPppAr;
                request.latency = PreciseBundleLatency::Automatic;
                request.center.clear();
                request.productLine.clear();
            } else if (preset == 4) {
                request.plan = PreciseBundlePlan::OsbPppAr;
                request.latency = PreciseBundleLatency::Automatic;
                request.clockInterval = "30S";
            } else if (preset == 5) {
                request.plan = PreciseBundlePlan::NetworkRtkPppRtk;
                request.latency = PreciseBundleLatency::Automatic;
                request.clockInterval = "30S";
            } else {
                request.plan = PreciseBundlePlan::AutoStandardPpp;
                request.latency = PreciseBundleLatency::Automatic;
                // Any identified centre/line may win, but SP3 and CLK must
                // still form one compatible family for every requested day.
            }
            request.referenceFrequency.clear();

            if (catalogThread_.joinable()) catalogThread_.join();
            catalogLoading_ = true;
            SetCatalogControlsEnabled(false);
            SetWindowTextW(status_, std::format(
                L"正在后台生成快速方案：{} 至 {}…",
                FormatDate(start), FormatDate(end)).c_str());
            PublishUiState();
            const HWND target = window_;
            catalogThread_ = std::jthread(
                [this, target, start, end, preset, request](
                    const std::stop_token stopToken) {
                    auto payload = std::make_unique<PreciseQuickBundleResult>();
                    try {
                        payload->start = start;
                        payload->end = end;
                        payload->preset = preset;
                        payload->request = request;
                        payload->catalog = LoadBundleCatalog(
                            start, end, request, payload->warnings);
                        if (!stopToken.stop_requested()) {
                            payload->plan = PlanPreciseBundle(
                                request, payload->catalog);
                            ApplyClkDeclaredAntex(request, payload->catalog,
                                payload->plan, payload->warnings);
                        }
                    } catch (...) {
                        try {
                            payload->warnings.push_back(
                                DescribeCurrentException());
                        } catch (...) {
                        }
                    }
                    auto* raw = payload.release();
                    if (!PostMessageW(target,
                            WM_PRECISE_QUICK_BUNDLE_FINISHED, 0,
                            reinterpret_cast<LPARAM>(raw))) {
                        delete raw;
                    }
                });
            return;
        }

        const QueryDate start = completed->start;
        const QueryDate end = completed->end;
        const int preset = completed->preset;
        const auto& request = completed->request;
        const auto& catalog = completed->catalog;
        const auto& result = completed->plan;
        const auto& warnings = completed->warnings;
        const auto report = BuildQuickBundleReport(result, start, end, warnings);
        SetWindowTextW(quickInfo_, report.c_str());
        if (!result.valid) {
            SetWindowTextW(status_, report.c_str());
            quickBundleReady_ = false;
            products_.clear();
            productDays_.clear();
            coverageRanges_.clear();
            indexedFamilyKeys_.clear();
            quickBundleDisplayedKeys_.clear();
            RebuildList();
            quickBundleSelectionKeys_.clear();
            quickBundleRestoreSelection_ = false;
            PublishUiState();
            return;
        }

        // Compatibility warnings remain visible in the short report.  The
        // download action checks them after deferred routes are hydrated.
        // Convert the planner's database-neutral manifest into the existing
        // product/day DTOs.  This keeps the proven downloader (mirror retry,
        // automatic concurrency, pause/resume/cancel and resume-by-file)
        // unchanged.
        std::unordered_map<std::string, const PreciseBundleProduct*> sourceByKey;
        sourceByKey.reserve(catalog.products.size());
        const auto manifestKey = [](const PreciseBundleProductType type,
                                    const std::string& logicalKey,
                                    const std::string& fileName) {
            return std::to_string(static_cast<int>(type)) + "\x1f"
                + logicalKey + "\x1f" + fileName;
        };
        // The planner's manifest describes complete daily bundles.  A range
        // can still contain useful individual products on incomplete days;
        // include those in the browse/download list without calling the
        // entire bundle complete.
        std::vector<PreciseBundleManifestEntry> displayedManifest;
        std::unordered_map<std::string, std::size_t> displayedKeys;
        for (const auto& status : result.dates) {
            for (const auto& member : status.members) {
                const auto key = manifestKey(member.type,
                    member.logicalKey, member.fileName);
                const auto [entry, inserted] = displayedKeys.emplace(
                    key, displayedManifest.size());
                if (inserted) {
                    displayedManifest.push_back(member);
                } else {
                    auto& routes = displayedManifest[entry->second].routes;
                    for (const auto& route : member.routes) {
                        if (!std::ranges::any_of(routes,
                                [&](const auto& existing) {
                                    return existing.source == route.source
                                        && existing.url == route.url
                                        && existing.fileName == route.fileName;
                                })) {
                            routes.push_back(route);
                        }
                    }
                }
            }
        }
        for (const auto& product : catalog.products) {
            sourceByKey.emplace(manifestKey(product.type,
                product.logicalKey, product.fileName), &product);
        }

        // Keep the same product IDs and names as the ordinary catalogue.
        // A row may still cover many dated files, but products with different
        // IDs must never be combined under an invented quick-plan label.
        const auto originalId = [&](const PreciseBundleManifestEntry& member) {
            const auto source = sourceByKey.find(manifestKey(member.type,
                member.logicalKey, member.fileName));
            if (source != sourceByKey.end()) {
                if (!source->second->displayId.empty()) {
                    return source->second->displayId;
                }
                if (!source->second->logicalKey.empty()) {
                    return Utf8ToWide(source->second->logicalKey);
                }
            }
            return Utf8ToWide(member.logicalKey.empty()
                ? member.fileName : member.logicalKey);
        };
        const auto rowKeyFor = [&](const PreciseBundleManifestEntry& member) {
            return std::to_wstring(static_cast<int>(member.type))
                + L"\x1f" + originalId(member);
        };
        std::unordered_map<std::wstring, std::size_t> rowByKey;
        std::unordered_map<std::wstring,
            std::unordered_map<int, std::size_t>> dayRowByKey;
        std::unordered_map<std::wstring,
            std::unordered_set<int>> coveredDaysByRow;
        for (const auto& status : result.dates) {
            for (const auto& member : status.members) {
                coveredDaysByRow[rowKeyFor(member)].insert(
                    status.date.year * 10000 + status.date.month * 100
                    + status.date.day);
            }
        }
        products_.clear();
        productDays_.clear();
        coverageRanges_.clear();
        indexedFamilyKeys_.clear();
        quickBundleDisplayedKeys_.clear();
        products_.reserve(displayedManifest.size());
        productDays_.reserve(displayedManifest.size());
        indexedFamilyKeys_.reserve(displayedManifest.size());
        quickBundleDisplayedKeys_.reserve(displayedManifest.size());
        for (const auto& member : displayedManifest) {
            const auto key = manifestKey(member.type,
                member.logicalKey, member.fileName);
            const auto rowKey = rowKeyFor(member);
            auto row = rowByKey.find(rowKey);
            if (row == rowByKey.end()) {
                PreciseProductFile product;
                product.id = originalId(member);
                product.category = ToUiCategory(member.type);
                const auto source = sourceByKey.find(key);
                if (source != sourceByKey.end()) {
                    const auto& metadata = *source->second;
                    product.name = metadata.displayName.empty()
                        ? product.id : metadata.displayName;
                    product.latency = ToUiLatency(metadata.latency, member.type);
                    product.center = Utf8ToWide(metadata.center.empty()
                        ? result.center : metadata.center);
                    product.constellation = Utf8ToWide(metadata.constellation.empty()
                        ? result.constellation : metadata.constellation);
                    if (member.type == PreciseBundleProductType::Antex
                        || member.type == PreciseBundleProductType::Brdc) {
                        product.formatVersion = Utf8ToWide(metadata.interval);
                    } else {
                        product.interval = Utf8ToWide(metadata.interval);
                    }
                } else {
                    product.name = product.id;
                    product.latency = ToUiLatency(result.latency, member.type);
                    product.center = Utf8ToWide(result.center);
                    product.constellation = Utf8ToWide(result.constellation);
                }
                // Quick ANTEX candidates are restricted to IGS_GLOBAL.
                // Keep the provider for the shared ANTEX destination instead
                // of inheriting the SP3 analysis centre from the bundle.
                if (member.type == PreciseBundleProductType::Antex) {
                    product.center = L"IGS";
                }
                for (const auto& route : member.routes) {
                    product.mirrors.push_back({Utf8ToWide(route.source),
                        route.url, Utf8ToWide(route.fileName)});
                }
                product.selected = !quickBundleRestoreSelection_
                    || quickBundleSelectionKeys_.contains(rowKey);
                product.expectedDays = result.expectedDays;
                const auto coverage = coveredDaysByRow.find(rowKey);
                product.availableDays = coverage == coveredDaysByRow.end()
                    ? 0 : static_cast<int>(coverage->second.size());
                product.completenessPercent = result.expectedDays == 0 ? 0.0
                    : 100.0 * static_cast<double>(product.availableDays)
                        / static_cast<double>(result.expectedDays);
                product.status = product.availableDays == result.expectedDays
                    ? L"查询范围完整" : L"部分日期有文件，可下载已找到的文件";
                if (member.type == PreciseBundleProductType::Antex
                    && request.plan == PreciseBundlePlan::WumPridePppAr) {
                    product.status = L"范围完整性按相关 CLK 引用日期统计，非 ANTEX 文件内部有效期。";
                }
                const std::size_t index = products_.size();
                rowByKey.emplace(rowKey, index);
                products_.push_back(std::move(product));
                quickBundleDisplayedKeys_.push_back(rowKey);
                productDays_.emplace_back();
                indexedFamilyKeys_.emplace_back();
                row = rowByKey.find(rowKey);
            }
            const std::size_t index = row->second;
            auto& days = productDays_[index];
            const int dateKey = member.date.year * 10000
                + member.date.month * 100 + member.date.day;
            auto& dayRows = dayRowByKey[rowKey];
            const auto dayIt = dayRows.find(dateKey);
            std::vector<PreciseProductMirror> mirrors;
            for (const auto& route : member.routes) {
                mirrors.push_back({Utf8ToWide(route.source), route.url,
                    Utf8ToWide(route.fileName)});
            }
            // Keep one representative route per source in the compact row;
            // each dated day retains all of its routes below for failover.
            products_[index].mirrors.insert(
                products_[index].mirrors.end(), mirrors.begin(), mirrors.end());
            products_[index].mirrors = RepresentativeMirrors(
                products_[index].mirrors);
            if (dayIt == dayRows.end()) {
                dayRows.emplace(dateKey, days.size());
                days.push_back({member.date, member.logicalKey,
                    std::move(mirrors)});
            } else {
                MergeMirrors(days[dayIt->second].mirrors, mirrors);
            }
        }
        coverageRanges_.resize(products_.size());
        for (const auto& [key, index] : rowByKey) {
            const auto found = coveredDaysByRow.find(key);
            if (found == coveredDaysByRow.end()) continue;
            for (const int value : found->second) {
                const auto day = MakeQueryDate(value / 10000, (value / 100) % 100, value % 100);
                coverageRanges_[index].push_back({day, day});
            }
        }
        // Missing types have no original product ID; keep them in the
        // completeness report instead of fabricating a catalogue row.
        ++catalogRevision_;
        periodStart_ = start;
        periodEnd_ = end;
        currentDate_ = start;
        // The generated list is independent of the single-type query filters.
        // Each displayed original product retains its dated download members.
        SendMessageW(category_, CB_SETCURSEL, 0, 0);
        SendMessageW(latency_, CB_SETCURSEL, 0, 0);
        SendMessageW(center_, CB_SETCURSEL, 0, 0);
        SendMessageW(completeness_, CB_SETCURSEL, 0, 0);
        SetWindowTextW(search_, L"");
        RebuildList();
        quickBundleResult_ = result;
        quickBundleStart_ = start;
        quickBundleEnd_ = end;
        quickBundlePreset_ = preset;
        quickBundleRequest_ = request;
        quickBundleReady_ = true;
        quickBundleSelectionKeys_.clear();
        quickBundleRestoreSelection_ = false;
        UpdateSummary();
        const std::wstring action = request.strictMatching
            && !result.warning.empty()
            ? L"；清单显示各产品完整率；兼容性待核对，不能直接下载。"
            : displayedManifest.empty()
                ? L"；清单显示各产品完整率；没有找到可下载的文件。"
                : L"；清单显示各产品完整率，可下载已找到的文件。";
        SetWindowTextW(status_, (report + action).c_str());
    }

    void BeginQuickBundleRouteHydration(const bool forQueue = false) {
        if (catalogLoading_ || !quickBundleReady_) return;
        if (!quickBundleResult_.routesDeferred) return;
        quickBundleSelectionKeys_.clear();
        for (std::size_t index = 0; index < products_.size(); ++index) {
            if (products_[index].selected
                && index < quickBundleDisplayedKeys_.size()) {
                quickBundleSelectionKeys_.insert(
                    quickBundleDisplayedKeys_[index]);
            }
        }
        quickBundleRestoreSelection_ = true;
        const QueryDate start = quickBundleStart_;
        const QueryDate end = quickBundleEnd_;
        const auto request = quickBundleRequest_;
        // The compact planner has already selected the exact SP3/CLK/ATT
        // family for every date.  Carry only those family keys into the
        // hydration worker; re-enumerating every centre's family here is what
        // made long quick-package downloads allocate hundreds of MB before
        // the first transfer could start.
        auto hydrateFamilyKeys = std::make_shared<
            std::unordered_set<std::string>>();
        for (const auto& status : quickBundleResult_.dates) {
            for (const auto& member : status.members) {
                if (!member.routes.empty()
                    || (member.type != PreciseBundleProductType::Sp3
                        && member.type != PreciseBundleProductType::Clk
                        && member.type != PreciseBundleProductType::Att)
                    || member.logicalKey.empty()) {
                    continue;
                }
                hydrateFamilyKeys->insert(
                    std::to_string(static_cast<int>(member.type)) + "\x1f"
                    + member.logicalKey);
            }
        }
        if (hydrateFamilyKeys->empty()) {
            hydrateFamilyKeys.reset();
        }
        if (request.startDate.dayOfYear <= 0
            || request.endDate.dayOfYear <= 0) {
            SetWindowTextW(status_, L"快速方案请求状态已失效，请重新查询。");
            return;
        }
        catalogLoading_ = true;
        quickBundleDownloadPending_ = !forQueue;
        quickBundleQueuePending_ = forQueue;
        SetCatalogControlsEnabled(false);
        SendMessageW(progress_, PBM_SETPOS, 0, 0);
        SetWindowTextW(status_, L"正在读取快速方案的实际下载路线…");
        PublishUiState();
        const HWND target = window_;
        const int preset = quickBundlePreset_;
        catalogThread_ = std::jthread(
            [this, target, start, end, preset, request, hydrateFamilyKeys](
                const std::stop_token stopToken) {
                auto payload = std::make_unique<PreciseQuickBundleResult>();
                try {
                    payload->start = start;
                    payload->end = end;
                    payload->preset = preset;
                    payload->request = request;
                    payload->catalog = LoadBundleCatalog(
                        start, end, request, payload->warnings, true,
                        hydrateFamilyKeys == nullptr
                            ? nullptr : hydrateFamilyKeys.get(), stopToken);
                    if (!stopToken.stop_requested()) {
                        payload->plan = PlanPreciseBundle(
                            request, payload->catalog);
                        ApplyClkDeclaredAntex(request, payload->catalog,
                            payload->plan, payload->warnings);
                    }
                } catch (...) {
                    try {
                        payload->warnings.push_back(
                            DescribeCurrentException());
                    } catch (...) {
                    }
                }
                auto* raw = payload.release();
                if (!PostMessageW(target, WM_PRECISE_QUICK_BUNDLE_FINISHED,
                                  0, reinterpret_cast<LPARAM>(raw))) {
                    delete raw;
                }
            });
    }

    void GenerateCatalog() {
        KillTimer(window_, kPrecisionCatalogTimer);
        if (catalogLoading_) {
            return;
        }
        InvalidateQuickBundle();
        // A normal single-category query returns to the historical
        // year/DOY directory layout.  Quick-package downloads set this flag
        // only after their cross-category manifest has passed validation.
        SYSTEMTIME selected{};
        if (DateTime_GetSystemtime(date_, &selected) != GDT_VALID) {
            NativeUiLanguage::ShowMessageBox(window_, L"请选择有效日期。", L"GNSS 数据下载器", MB_OK | MB_ICONWARNING);
            return;
        }
        currentDate_ = MakeQueryDate(selected.wYear, selected.wMonth, selected.wDay);
        if (currentDate_.dayOfYear <= 0) {
            NativeUiLanguage::ShowMessageBox(window_, L"日期无效。", L"GNSS 数据下载器", MB_OK | MB_ICONWARNING);
            return;
        }
        SYSTEMTIME selectedEnd{};
        if (DateTime_GetSystemtime(endDate_, &selectedEnd) != GDT_VALID) {
            NativeUiLanguage::ShowMessageBox(window_, L"请选择有效的终止日期。",
                L"GNSS 数据下载器", MB_OK | MB_ICONWARNING);
            return;
        }
        const QueryDate endDate = MakeQueryDate(
            selectedEnd.wYear, selectedEnd.wMonth, selectedEnd.wDay);
        if (endDate.dayOfYear <= 0) {
            NativeUiLanguage::ShowMessageBox(window_, L"终止日期无效。",
                L"GNSS 数据下载器", MB_OK | MB_ICONWARNING);
            return;
        }
        const int categoryIndex = static_cast<int>(
            SendMessageW(category_, CB_GETCURSEL, 0, 0));
        // One path for both one-day and range queries keeps "all products"
        // consistent with individual families and always runs SQLite work in
        // the background.
        GenerateCatalogRange(currentDate_, endDate, categoryIndex);
        return;
        if (endDate.year != currentDate_.year
            || endDate.month != currentDate_.month
            || endDate.day != currentDate_.day) {
            GenerateCatalogRange(currentDate_, endDate, categoryIndex);
            return;
        }
        ++catalogRevision_;
        periodStart_ = periodEnd_ = currentDate_;
        productDays_.clear();
        coverageRanges_.clear();
        indexedFamilyKeys_.clear();
        if (categoryIndex == 1) {
            const auto lookup = LoadSp3Availability(currentDate_);
            products_.clear();
            products_.reserve(lookup.products.size());
            for (const auto& record : lookup.products) {
                products_.push_back(ToPreciseProductFile(record));
            }
            AppendBdsMartProductsForDate(
                products_, currentDate_, BdsMartOrbitCategoryIndex());
            RebuildList();
            if (!lookup.error.empty()) {
                SetWindowTextW(status_,
                    (L"SP3 本地知识库读取失败：" + lookup.error).c_str());
            } else if (!lookup.indexed) {
                SetWindowTextW(status_, std::format(
                    L"SP3 本地知识库尚未收录 GPS 周 {}。本次查询没有联网。",
                    lookup.gpsWeek).c_str());
            } else {
                SetWindowTextW(status_, std::format(
                    L"SP3 查询来自本地知识库：GPS周 {}，{}/7 个数据中心{}，当天 {} 个逻辑产品。",
                    lookup.gpsWeek,
                    lookup.sourcesSuccessful,
                    lookup.complete ? L"完整" : L"（待复扫）",
                    lookup.products.size()).c_str());
            }
            return;
        }
        if (categoryIndex == 2) {
            const auto lookup = LoadClkAvailability(currentDate_);
            products_.clear();
            products_.reserve(lookup.products.size());
            for (const auto& record : lookup.products) {
                products_.push_back(ToClkProductFile(record));
            }
            AppendBdsMartProductsForDate(
                products_, currentDate_, BdsMartClockCategoryIndex());
            RebuildList();
            if (!lookup.error.empty()) {
                SetWindowTextW(status_,
                    (L"CLK 本地知识库读取失败：" + lookup.error).c_str());
            } else if (!lookup.indexed) {
                SetWindowTextW(status_, std::format(
                    L"CLK 本地知识库尚未收录 GPS 周 {}。本次查询没有联网。",
                    lookup.gpsWeek).c_str());
            } else {
                SetWindowTextW(status_, std::format(
                    L"CLK 查询来自本地知识库：GPS周 {}，{}/7 个数据中心{}，当天 {} 个逻辑产品。",
                    lookup.gpsWeek,
                    lookup.sourcesSuccessful,
                    lookup.complete ? L"完整" : L"（待复扫）",
                    lookup.products.size()).c_str());
            }
            return;
        }
        const int erpCategoryIndex =
            static_cast<int>(PreciseProductCategory::EarthRotation) + 1;
        if (categoryIndex == erpCategoryIndex) {
            const auto lookup = LoadErpAvailability(currentDate_);
            products_.clear();
            products_.reserve(lookup.products.size());
            std::size_t erpProducts = 0;
            std::size_t eopProducts = 0;
            for (const auto& record : lookup.products) {
                products_.push_back(ToErpProductFile(record));
                if (record.productFamily == "EOP") ++eopProducts;
                else ++erpProducts;
            }
            RebuildList();
            if (!lookup.error.empty()) {
                SetWindowTextW(status_,
                    (L"ERP/EOP 本地知识库读取失败：" + lookup.error).c_str());
            } else if (!lookup.indexed) {
                SetWindowTextW(status_, std::format(
                    L"ERP/EOP 知识库尚未收录 GPS 周 {}。"
                    L"本次查询没有联网。数据库：{}",
                    lookup.gpsWeek, lookup.databasePath.wstring()).c_str());
            } else {
                SetWindowTextW(status_, std::format(
                    L"ERP/EOP 查询来自本地知识库：GPS周 {} 的ERP档案 {}/{}{}；"
                    L"当天 {} 个ERP、{} 个IERS EOP长序列。",
                    lookup.gpsWeek,
                    lookup.sourcesSuccessful,
                    lookup.sourcesExpected,
                    lookup.complete ? L"完整" : L"（未扫描或待复扫）",
                    erpProducts, eopProducts).c_str());
            }
            return;
        }
        const int dcbCategoryIndex =
            static_cast<int>(PreciseProductCategory::DifferentialCodeBias) + 1;
        if (categoryIndex == dcbCategoryIndex) {
            const auto lookup = LoadDcbAvailability(currentDate_);
            products_.clear();
            products_.reserve(lookup.products.size());
            for (const auto& record : lookup.products) {
                products_.push_back(ToDcbProductFile(record));
            }
            RebuildList();
            if (!lookup.error.empty()) {
                SetWindowTextW(status_,
                    (L"DCB 本地知识库读取失败：" + lookup.error).c_str());
            } else if (!lookup.indexed) {
                SetWindowTextW(status_, std::format(
                    L"DCB 知识库尚未收录 {} 年。"
                    L"本次查询没有联网。数据库：{}",
                    lookup.archiveYear, lookup.databasePath.wstring()).c_str());
            } else {
                SetWindowTextW(status_, std::format(
                    L"DCB 查询来自本地知识库：{} 年，{}/{} 个档案源{}，"
                    L"当天 {} 个逻辑产品；OSB 未混入此结果。",
                    lookup.archiveYear,
                    lookup.sourcesSuccessful,
                    lookup.sourcesExpected,
                    lookup.complete ? L"完整" : L"（待复扫）",
                    lookup.products.size()).c_str());
            }
            return;
        }
        const int ionexCategoryIndex =
            static_cast<int>(PreciseProductCategory::Ionosphere) + 1;
        if (categoryIndex == ionexCategoryIndex) {
            const auto lookup = LoadIonexAvailability(currentDate_);
            products_.clear();
            products_.reserve(lookup.products.size());
            for (const auto& record : lookup.products) {
                products_.push_back(ToIonexProductFile(record));
            }
            RebuildList();
            if (!lookup.error.empty()) {
                SetWindowTextW(status_,
                    (L"IONEX 本地知识库读取失败：" + lookup.error).c_str());
            } else if (!lookup.indexed) {
                SetWindowTextW(status_, std::format(
                    L"IONEX 知识库尚未收录这一天。"
                    L"本次查询没有联网。数据库：{}",
                    lookup.databasePath.wstring()).c_str());
            } else {
                SetWindowTextW(status_, std::format(
                    L"IONEX 查询来自本地知识库：{}/{} 个档案源{}，"
                    L"当天 {} 个逻辑 GIM/ROTI 产品。",
                    lookup.sourcesSuccessful,
                    lookup.sourcesExpected,
                    lookup.complete ? L"完整" : L"（待复扫）",
                    lookup.products.size()).c_str());
            }
            return;
        }
        const int sinexCategoryIndex =
            static_cast<int>(PreciseProductCategory::Sinex) + 1;
        if (categoryIndex == sinexCategoryIndex) {
            const auto lookup = LoadSinexAvailability(currentDate_);
            products_.clear();
            products_.reserve(lookup.products.size());
            std::size_t solutionFiles = 0;
            std::size_t coordinateFiles = 0;
            for (const auto& record : lookup.products) {
                products_.push_back(ToSinexProductFile(record));
                if (record.contentType == "CRD") ++coordinateFiles;
                else ++solutionFiles;
            }
            RebuildList();
            if (!lookup.error.empty()) {
                SetWindowTextW(status_,
                    (L"SINEX 本地知识库读取失败：" + lookup.error).c_str());
            } else if (!lookup.indexed) {
                SetWindowTextW(status_, std::format(
                    L"SINEX 知识库尚未收录 GPS 周 {}。"
                    L"本次查询没有联网。数据库：{}",
                    lookup.gpsWeek, lookup.databasePath.wstring()).c_str());
            } else {
                SetWindowTextW(status_, std::format(
                    L"SINEX 查询来自本地知识库：GPS周 {}，{}/{} 个档案源{}，"
                    L"当天 {} 个完整解、{} 个坐标解；Bias/TRO-SINEX 未混入。",
                    lookup.gpsWeek,
                    lookup.sourcesSuccessful,
                    lookup.sourcesExpected,
                    lookup.complete ? L"完整" : L"（待复扫）",
                    solutionFiles, coordinateFiles).c_str());
            }
            return;
        }
        const int troCategoryIndex =
            static_cast<int>(PreciseProductCategory::Troposphere) + 1;
        if (categoryIndex == troCategoryIndex) {
            const auto lookup = LoadTroAvailability(currentDate_);
            products_.clear();
            products_.reserve(lookup.products.size());
            std::size_t stationProducts = 0;
            std::size_t networkProducts = 0;
            for (const auto& record : lookup.products) {
                products_.push_back(ToTroProductFile(record));
                if (record.productFamily == "IGS_SITE") ++stationProducts;
                else ++networkProducts;
            }
            RebuildList();
            if (!lookup.error.empty()) {
                SetWindowTextW(status_,
                    (L"TRO 本地知识库读取失败：" + lookup.error).c_str());
            } else if (!lookup.indexed) {
                SetWindowTextW(status_, std::format(
                    L"TRO 知识库尚未收录这一天。"
                    L"本次查询没有联网。数据库：{}",
                    lookup.databasePath.wstring()).c_str());
            } else {
                SetWindowTextW(status_, std::format(
                    L"TRO 查询来自本地知识库：{}/{} 个档案源{}；"
                    L"当天 {} 个IGS测站产品、{} 个CODE/EPN网络产品。",
                    lookup.sourcesSuccessful,
                    lookup.sourcesExpected,
                    lookup.complete ? L"完整" : L"（待复扫）",
                    stationProducts, networkProducts).c_str());
            }
            return;
        }
        const int antexCategoryIndex =
            static_cast<int>(PreciseProductCategory::Antenna) + 1;
        if (categoryIndex == antexCategoryIndex) {
            const auto lookup = LoadAntexAvailability(currentDate_);
            products_.clear();
            products_.reserve(lookup.products.size());
            for (const auto& record : lookup.products) {
                products_.push_back(ToAntexProductFile(record));
            }
            RebuildList();
            if (!lookup.error.empty()) {
                SetWindowTextW(status_,
                    (L"ANTEX 本地知识库读取失败：" + lookup.error).c_str());
            } else if (!lookup.indexed) {
                SetWindowTextW(status_, std::format(
                    L"ANTEX 知识库尚未构建。"
                    L"本次查询没有联网。数据库：{}",
                    lookup.databasePath.wstring()).c_str());
            } else {
                SetWindowTextW(status_, std::format(
                    L"ANTEX 查询来自本地知识库：查询日期对应 GPS周 {}，"
                    L"{}/{} 个生产体系{}，适用的天线模型 {} 个。",
                    lookup.gpsWeek,
                    lookup.sourcesSuccessful,
                    lookup.sourcesExpected,
                    lookup.complete ? L"完整" : L"（待复扫）",
                    lookup.products.size()).c_str());
            }
            return;
        }
        const int broadcastCategoryIndex =
            static_cast<int>(PreciseProductCategory::BroadcastNavigation) + 1;
        if (categoryIndex == broadcastCategoryIndex) {
            const auto lookup = LoadBroadcastAvailability(currentDate_);
            products_.clear();
            products_.reserve(lookup.products.size());
            for (const auto& record : lookup.products) {
                products_.push_back(ToBroadcastProductFile(record));
            }
            RebuildList();
            if (!lookup.error.empty()) {
                SetWindowTextW(status_,
                    (L"广播星历本地知识库读取失败：" + lookup.error).c_str());
            } else if (!lookup.indexed) {
                SetWindowTextW(status_, std::format(
                    L"广播星历知识库尚未收录这一天。本次查询没有联网。数据库：{}",
                    lookup.databasePath.wstring()).c_str());
            } else {
                SetWindowTextW(status_, std::format(
                    L"广播星历查询来自本地知识库：{}/{} 个档案中心{}，当天 {} 个逻辑产品。",
                    lookup.sourcesSuccessful,
                    lookup.sourcesExpected,
                    lookup.complete ? L"完整" : L"（待复扫）",
                    lookup.products.size()).c_str());
            }
            return;
        }
        const int osbCategoryIndex =
            static_cast<int>(PreciseProductCategory::ObservableSpecificBias) + 1;
        if (categoryIndex == osbCategoryIndex) {
            const auto lookup = LoadOsbAvailability(currentDate_);
            products_.clear();
            products_.reserve(lookup.products.size());
            for (const auto& record : lookup.products) {
                products_.push_back(ToOsbProductFile(record));
            }
            AppendBdsMartProductsForDate(
                products_, currentDate_, BdsMartOsbCategoryIndex());
            RebuildList();
            if (!lookup.error.empty()) {
                SetWindowTextW(status_,
                    (L"OSB 本地知识库读取失败：" + lookup.error).c_str());
            } else if (!lookup.indexed) {
                SetWindowTextW(status_, std::format(
                    L"OSB 知识库尚未收录 GPS 周 {}。"
                    L"本次查询没有联网。数据库：{}",
                    lookup.gpsWeek, lookup.databasePath.wstring()).c_str());
            } else {
                SetWindowTextW(status_, std::format(
                    L"OSB 查询来自本地知识库：GPS周 {}，{}/{} 个档案源{}，"
                    L"当天 {} 个逻辑产品；DCB、DSB、ABS 均未混入。",
                    lookup.gpsWeek,
                    lookup.sourcesSuccessful,
                    lookup.sourcesExpected,
                    lookup.complete ? L"完整" : L"（待复扫）",
                    lookup.products.size()).c_str());
            }
            return;
        }
        products_ = BuildPreciseProductCatalog(
            currentDate_, WindowText(troStation_));
        RebuildList();
        SetWindowTextW(status_,
            L"当前其他类别仍使用本地文件名规则生成候选；SP3、CLK、ERP/EOP、DCB、OSB、IONEX、TRO、SINEX 与广播星历只读各自的本地知识库。");
    }

    void EnsureProductRangeMetadata() {
        if (productDays_.size() != products_.size()) {
            productDays_.clear();
            productDays_.resize(products_.size());
            for (std::size_t index = 0; index < products_.size(); ++index) {
                productDays_[index].push_back({
                    currentDate_, {}, products_[index].mirrors});
                products_[index].availableDays = 1;
                products_[index].expectedDays = 1;
                products_[index].completenessPercent = 100.0;
            }
        } else {
            for (auto& product : products_) {
                if (product.expectedDays <= 0) {
                    product.expectedDays = 1;
                    product.availableDays = std::max(1, product.availableDays);
                    product.completenessPercent = 100.0;
                }
            }
        }
    }

    void RebuildList() {
        EnsureProductRangeMetadata();
        syncing_ = true;
        SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
        visible_.clear();
        const int categoryIndex = static_cast<int>(SendMessageW(category_, CB_GETCURSEL, 0, 0));
        const int latencyIndex = static_cast<int>(SendMessageW(latency_, CB_GETCURSEL, 0, 0));
        const int centerIndex = static_cast<int>(SendMessageW(center_, CB_GETCURSEL, 0, 0));
        const std::wstring search = Lower(WindowText(search_));
        for (std::size_t index = 0; index < products_.size(); ++index) {
            const auto& product = products_[index];
            if (!customRules_.empty()) {
                const bool matches = std::ranges::any_of(
                    customRules_, [&](const PreciseCustomRule& rule) {
                        if (rule.categoryIndex > 0
                            && static_cast<int>(product.category)
                                != rule.categoryIndex - 1) return false;
                        if (!PreciseProductMatchesLevel(product, rule.latencyIndex)) return false;
                        if (!PreciseProductMatchesInterval(product, rule.interval)) return false;
                        if (rule.center.empty()
                            || Lower(rule.center) == L"all analysis centers") {
                            return true;
                        }
                        return PreciseProductMatchesCenter(
                            product, PreciseCenterFilterToken(rule.center));
                    });
                if (!matches) continue;
            } else {
                if (categoryIndex > 0
                    && static_cast<int>(product.category) != categoryIndex - 1) {
                    continue;
                }
                if (!PreciseProductMatchesLevel(product, latencyIndex)) {
                    continue;
                }
            }
            if (completenessMode_ == PreciseCompletenessMode::Ratio
                && product.completenessPercent + 1e-9 < completenessValue_) {
                continue;
            }
            if (completenessMode_ == PreciseCompletenessMode::Days
                && product.availableDays + 1e-9 < completenessValue_) {
                continue;
            }
            if (centerIndex > 0
                && centerIndex < static_cast<int>(centerFilters_.size())) {
                if (!PreciseProductMatchesCenter(product, centerFilters_[centerIndex])) continue;
            }
            if (!search.empty()) {
                const std::wstring haystack = Lower(
                    product.id + L" " + product.name + L" " + product.center
                    + L" " + std::wstring(PreciseProductDisplayCenter(product))
                    + L" " + product.constellation);
                if (haystack.find(search) == std::wstring::npos) {
                    continue;
                }
            }
            visible_.push_back(index);
        }
        ListView_SetItemCountEx(list_, static_cast<int>(visible_.size()),
            LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
        // Product selection is represented by the checkbox, not the native
        // current-row highlight. Drop stale selection left by the previous
        // filtered list before repainting the table.
        ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetSelectionMark(list_, -1);
        SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list_, nullptr, TRUE);
        syncing_ = false;
        UpdateSummary();
        NotifyParentBusyChanged();
    }

    void HandleListDisplay(NMLVDISPINFOW& info) const {
        const auto row = info.item.iItem;
        if (row < 0 || static_cast<std::size_t>(row) >= visible_.size()) return;
        const auto& product = products_[visible_[row]];
        if ((info.item.mask & LVIF_STATE) != 0) {
            info.item.stateMask = LVIS_STATEIMAGEMASK;
            info.item.state = INDEXTOSTATEIMAGEMASK(product.selected ? 2 : 1);
        }
        if ((info.item.mask & LVIF_TEXT) == 0
            || info.item.pszText == nullptr || info.item.cchTextMax <= 0) return;
        std::wstring value;
        switch (info.item.iSubItem) {
        case 0: value = product.id; break;
        case 1: value = PreciseProductCategoryName(product.category); break;
        case 2: value = PreciseProductDisplayCenter(product); break;
        case 3: value = PreciseProductDisplayLevel(product); break;
        case 4: value = product.constellation; break;
        case 5: value = PreciseProductSamplingInterval(product); break;
        case 6: value = product.mirrors.empty()
            ? L"—" : product.mirrors.front().fileName; break;
        case 7: value = std::to_wstring(product.mirrors.size()); break;
        case 8: value = product.status.empty() ? L"待下载" : product.status; break;
        case 9: value = std::format(L"{}/{} 天 ({:.1f}%)",
            product.availableDays, product.expectedDays,
            product.completenessPercent); break;
        default: break;
        }
        lstrcpynW(info.item.pszText, value.c_str(), info.item.cchTextMax);
    }

    void HandleItemChanged(const NMLISTVIEW& change) {
        if (syncing_ || change.iItem < 0
            || (change.uChanged & LVIF_STATE) == 0) {
            return;
        }
        const UINT oldState =
            (change.uOldState & LVIS_STATEIMAGEMASK) >> 12;
        const UINT newState =
            (change.uNewState & LVIS_STATEIMAGEMASK) >> 12;
        if (oldState == newState || newState == 0) {
            return;
        }
        const std::size_t row = static_cast<std::size_t>(change.iItem);
        if (row >= visible_.size()) {
            return;
        }
        auto& product = products_[visible_[row]];
        product.selected = newState == 2
            && !(quickBundleReady_ && product.availableDays == 0);
        if (newState == 2 && !product.selected) {
            ListView_RedrawItems(list_, change.iItem, change.iItem);
        }
        UpdateSummary();
    }

    LRESULT HandleListCustomDraw(NMLVCUSTOMDRAW& draw) const noexcept {
        switch (draw.nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: {
            const bool selected = (draw.nmcd.uItemState & CDIS_SELECTED) != 0
                && GetFocus() == list_;
            draw.clrText = RGB(31, 41, 55);
            // A white catalogue is easier to scan than a permanently blue
            // native selection. Use only a faint focus cue while the list has
            // keyboard focus; checked state remains in the checkbox.
            draw.clrTextBk = selected
                ? RGB(239, 246, 255)
                : RGB(255, 255, 255);
            return CDRF_DODEFAULT;
        }
        default:
            return CDRF_DODEFAULT;
        }
    }

    void SelectVisible(const bool selected) {
        if (catalogLoading_) return;
        syncing_ = true;
        for (const auto row : visible_) {
            products_[row].selected = selected
                && !(quickBundleReady_ && products_[row].availableDays == 0);
        }
        if (!visible_.empty()) ListView_RedrawItems(list_, 0,
            static_cast<int>(visible_.size() - 1));
        syncing_ = false;
        UpdateSummary();
        NotifyParentBusyChanged();
    }

    void BrowseFolder() {
        IFileDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(
                CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&dialog)))) {
            NativeUiLanguage::ShowMessageBox(window_, L"无法打开文件夹选择器。", L"GNSS 数据下载器", MB_OK | MB_ICONERROR);
            return;
        }
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(NativeUiLanguage::Text(L"选择精密产品保存目录").c_str());
        if (SUCCEEDED(dialog->Show(window_))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    SetWindowTextW(folder_, path);
                    if (!DownloadFolderState::Save(L"precise", path)) {
                        NativeUiLanguage::ShowMessageBox(window_,
                            L"保存目录已更改，但无法记住到下次启动。",
                            L"GNSS 数据下载器", MB_OK | MB_ICONWARNING);
                    }
                    // A quick query is independent of the destination.  A
                    // changed destination must therefore require a fresh
                    // confirmation before its manifest is written.
                    InvalidateQuickBundle();
                    CoTaskMemFree(path);
                    NotifyParentBusyChanged();
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    void SaveFolderPreference() const {
        const auto folder = WindowText(folder_);
        if (!folder.empty()) {
            DownloadFolderState::Save(L"precise", folder);
        }
    }

    SelectedPreciseProduct SnapshotSelectedProduct(
        const std::size_t index) const {
        SelectedPreciseProduct item;
        item.product = products_[index];
        if (index < indexedFamilyKeys_.size()
            && !indexedFamilyKeys_[index].empty()) {
            item.indexedFamilyKey = indexedFamilyKeys_[index];
            item.indexedCategory = static_cast<int>(products_[index].category) + 1;
            item.startDate = periodStart_;
            item.endDate = periodEnd_;
        }
        if (index < productDays_.size() && !productDays_[index].empty()) {
            item.days = productDays_[index];
        } else if (item.product.category == PreciseProductCategory::Antenna
            && index < coverageRanges_.size()
            && !coverageRanges_[index].empty()) {
            item.days.push_back({coverageRanges_[index].front().first,
                {}, item.product.mirrors});
        } else if (index < coverageRanges_.size()
            && !coverageRanges_[index].empty()) {
            item.coverageRanges = coverageRanges_[index];
        } else if (item.indexedFamilyKey.empty()) {
            item.days.push_back({currentDate_, {}, products_[index].mirrors});
        }
        return item;
    }

    void AddSelectedToDownloadPlan() {
        if (catalogLoading_) return;
        // A strict plan with unresolved compatibility still needs the
        // planner's authoritative check before it can be queued.  Ordinary
        // quick plans can queue their dated family keys immediately.
        if (quickBundleReady_ && quickBundleResult_.routesDeferred
            && quickBundleRequest_.strictMatching
            && !quickBundleResult_.warning.empty()) {
            BeginQuickBundleRouteHydration(true);
            return;
        }
        if (quickBundleReady_ && quickBundleRequest_.strictMatching
            && !quickBundleResult_.warning.empty()) {
            SetWindowTextW(status_,
                L"方案兼容性元数据待核对，不能直接加入下载计划。");
            NotifyParentBusyChanged();
            return;
        }
        const auto folder = WindowText(folder_);
        constexpr std::size_t kMaximumQueuedProductDays = 250'000;
        std::size_t plannedDays = 0;
        for (const auto& queued : downloadQueue_) {
            if (queued.state != L"pending" && queued.state != L"running") {
                continue;
            }
            const auto days = queued.payload.product.category
                    == PreciseProductCategory::Antenna
                ? 1U : static_cast<std::size_t>(std::max(
                    1, queued.payload.product.availableDays));
            plannedDays = std::min(kMaximumQueuedProductDays,
                plannedDays + std::min(days, kMaximumQueuedProductDays));
        }
        std::vector<std::pair<std::size_t, std::wstring>> candidates;
        try {
        std::unordered_set<std::wstring> queuedKeys;
        queuedKeys.reserve(downloadQueue_.size() + products_.size());
        for (const auto& queued : downloadQueue_) {
            if (queued.state != L"cancelled") queuedKeys.insert(queued.key);
        }
        for (std::size_t index = 0; index < products_.size(); ++index) {
            const auto& product = products_[index];
            if (!product.selected || product.availableDays <= 0) continue;
            if (product.localInventoryOnly) continue;
            const std::wstring familyKey = index < indexedFamilyKeys_.size()
                ? Utf8ToWide(indexedFamilyKeys_[index]) : L"";
            std::wstring key = std::format(
                L"{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
                product.id, static_cast<int>(product.category),
                static_cast<int>(product.latency), product.center,
                product.constellation, product.interval,
                familyKey, FormatDate(periodStart_),
                FormatDate(periodEnd_), folder);
            if (!product.mirrors.empty()) {
                key += L"|" + product.mirrors.front().fileName;
                key += L"|" + product.mirrors.front().url;
            }
            if (!queuedKeys.insert(key).second) {
                continue;
            }
            const auto days = product.category == PreciseProductCategory::Antenna
                ? 1U : static_cast<std::size_t>(std::max(
                    1, product.availableDays));
            if (days > kMaximumQueuedProductDays - plannedDays) {
                SetWindowTextW(status_,
                    L"下载计划超过 25 万个产品日期；请缩短范围或分批添加。");
                NotifyParentBusyChanged();
                return;
            }
            plannedDays += days;
            candidates.emplace_back(index, std::move(key));
        }
            std::vector<PreciseQueuedDownload> prepared;
            prepared.reserve(candidates.size());
            for (auto& [index, key] : candidates) {
                PreciseQueuedDownload item;
                item.id = ++nextQueueId_;
                item.label = products_[index].id;
                item.key = std::move(key);
                item.folder = folder;
                item.start = periodStart_;
                item.end = periodEnd_;
                item.payload = SnapshotSelectedProduct(index);
                // A quick plan already knows exactly which family is wanted
                // on each date.  Queue that compact selection now; resolve
                // physical mirrors on the download worker, after the user
                // starts the task.  The queued snapshot survives later queries.
                if (quickBundleReady_ && quickBundleResult_.routesDeferred) {
                    item.payload.quickPlanRoutesDeferred = std::ranges::any_of(
                        item.payload.days, [](const auto& day) {
                            return day.mirrors.empty();
                        });
                    if (item.payload.quickPlanRoutesDeferred) {
                        item.payload.indexedCategory = static_cast<int>(
                            item.payload.product.category) + 1;
                    }
                }
                prepared.push_back(std::move(item));
            }
            downloadQueue_.reserve(downloadQueue_.size() + prepared.size());
            for (auto& item : prepared) {
                downloadQueue_.push_back(std::move(item));
            }
        } catch (const std::bad_alloc&) {
            SetWindowTextW(status_,
                L"内存不足，未能加入下载计划；请缩短日期范围或减少产品。");
            NotifyParentBusyChanged();
            return;
        }
        if (candidates.empty() && std::ranges::any_of(products_, [](const auto& product) {
                return product.selected && product.localInventoryOnly;
            })) {
            SetWindowTextW(status_, L"UPD 只有本地记录，尚未配置在线下载地址。");
            NotifyParentBusyChanged();
            return;
        }
        SetWindowTextW(status_, std::format(
            L"已将 {} 个精密产品添加到下载计划。", candidates.size()).c_str());
        NotifyParentBusyChanged();
        if (queueRunning_) DispatchNextQueuedDownload();
    }

    void DispatchNextQueuedDownload() {
        if (!queueRunning_ || downloadCancelRequested_) return;
        for (auto& item : downloadQueue_) {
            if (planWorkers_.Size() >= DownloadPlanWorkers::limit) break;
            if (item.state != L"pending" || !item.selected) continue;
            item.state = L"running";
            item.percent = 0;
            activeQueueId_ = item.id;
            StartOrCancelDownload(true);
            if (!planWorkers_.Contains(item.id)) {
                item.state = L"failed";
                item.selected = false;
            }
        }
        activeQueueId_ = planWorkers_.First();
        if (planWorkers_.Size() == 0) {
            queueRunning_ = false;
        }
        NotifyParentBusyChanged();
    }

    void StartQueuedDownloadPlan() {
        if (downloading_ || catalogLoading_) return;
        for (auto& item : downloadQueue_) {
            if (item.selected && item.state != L"running") {
                item.state = L"pending";
                item.percent = 0;
            }
        }
        queueRunning_ = true;
        DispatchNextQueuedDownload();
    }

    void UpdateQueuedDownloadItem(
        const std::uint64_t id, const std::wstring_view action) {
        for (auto& item : downloadQueue_) {
            if (id != 0 && item.id != id) continue;
            if (action == L"select" || action == L"unselect") {
                item.selected = action == L"select";
            } else if (action == L"cancel" || action == L"remove"
                       || (action == L"remove-selected" && item.selected)) {
                const bool remove = action != L"cancel";
                if (item.state == L"running") {
                    item.removeAfterCancel = remove;
                    planWorkers_.Stop(item.id);
                } else {
                    item.state = L"cancelled";
                    item.selected = false;
                    item.removeAfterCancel = remove;
                }
            }
        }
        std::erase_if(downloadQueue_, [](const auto& item) {
            return item.removeAfterCancel && item.state != L"running";
        });
        NotifyParentBusyChanged();
    }

    void StartOrCancelDownload(const bool fromPlan = false) {
        if (downloading_ && !fromPlan) {
            // Publish cancellation before waking paused workers.  This keeps
            // a worker from claiming another task in the resume/stop race.
            queueRunning_ = false;
            planWorkers_.StopAll();
            downloadCancelRequested_ = true;
            if (downloadThread_.joinable()) {
                downloadThread_.request_stop();
            }
            if (downloadControl_ != nullptr) {
                downloadControl_->resume();
            }
            SetBusy(true);
            SetWindowTextW(status_, L"正在取消精密产品下载…");
            return;
        }
        const auto queuedIt = std::ranges::find_if(downloadQueue_,
            [&](const auto& item) { return fromPlan && item.id == activeQueueId_; });
        const PreciseQueuedDownload* queued = queuedIt == downloadQueue_.end()
            ? nullptr : &*queuedIt;
        if ((queued != nullptr && queued->payload.product.localInventoryOnly)
            || (queued == nullptr && std::ranges::any_of(products_, [](const auto& p) {
                    return p.selected && p.localInventoryOnly;
                }) && !std::ranges::any_of(products_, [](const auto& p) {
                    return p.selected && !p.localInventoryOnly;
                }))) {
            SetWindowTextW(status_, L"UPD 只有本地记录，尚未配置在线下载地址。");
            NotifyParentBusyChanged();
            return;
        }
        // Quick presets may initially use compact family rows without
        // materialising every daily mirror.  Resolve those routes on demand,
        // then continue through the same ordinary download workflow used by
        // every other product query.
        if (queued == nullptr && quickBundleReady_
            && quickBundleResult_.routesDeferred) {
            BeginQuickBundleRouteHydration();
            return;
        }
        if (queued == nullptr && quickBundleReady_
            && quickBundleRequest_.strictMatching
            && !quickBundleResult_.warning.empty()) {
            NativeUiLanguage::ShowMessageBox(window_,
                L"方案产品的兼容性元数据不完整，请检查后使用普通产品查询下载。",
                L"方案兼容性提示", MB_OK | MB_ICONWARNING);
            return;
        }
        // Estimate the selected manifest before copying any large per-day
        // vectors.  The old code performed the deep copies first and only
        // then checked the limit, so a broad selection could throw
        // std::bad_alloc on the window thread and terminate the GUI.
        constexpr std::size_t kMaximumDownloadTasks = 250'000;
        std::size_t estimatedTasks = 0;
        bool hasSelection = false;
        for (std::size_t index = 0; queued == nullptr
             && index < products_.size(); ++index) {
            if (!products_[index].selected || products_[index].localInventoryOnly) continue;
            hasSelection = true;
            const bool indexed = index < indexedFamilyKeys_.size()
                && !indexedFamilyKeys_[index].empty();
            const bool staticAntex = products_[index].category
                == PreciseProductCategory::Antenna;
            const std::size_t days = staticAntex
                && index < coverageRanges_.size()
                && !coverageRanges_[index].empty()
                ? 1U
                : index < productDays_.size()
                && !productDays_[index].empty()
                ? productDays_[index].size()
                : indexed || (index < coverageRanges_.size()
                    && !coverageRanges_[index].empty())
                    ? static_cast<std::size_t>(std::max(
                        0, products_[index].availableDays))
                    : 1U;
            if (days > kMaximumDownloadTasks
                || estimatedTasks > kMaximumDownloadTasks - days) {
                estimatedTasks = kMaximumDownloadTasks + 1U;
                break;
            }
            estimatedTasks += days;
        }
        if (queued != nullptr) {
            hasSelection = true;
            estimatedTasks = queued->payload.days.empty()
                ? static_cast<std::size_t>(std::max(
                    0, queued->payload.product.availableDays))
                : queued->payload.days.size();
        }
        if (!hasSelection) {
            NativeUiLanguage::ShowMessageBox(window_, L"请先选择至少一个精密产品。", L"GNSS 数据下载器", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (estimatedTasks > kMaximumDownloadTasks) {
            NativeUiLanguage::ShowMessageBox(window_,
                L"所选范围预计需要处理超过 25 万个产品日期，已为避免卡顿和内存耗尽而停止。\n"
                L"请缩短日期范围，或减少已选产品后再下载。",
                L"下载清单过大", MB_OK | MB_ICONWARNING);
            return;
        }
        std::vector<SelectedPreciseProduct> selected;
        try {
            selected.reserve(queued != nullptr ? 1U
                : std::min<std::size_t>(products_.size(), 64U));
            if (queued != nullptr) selected.push_back(queued->payload);
            for (std::size_t index = 0; queued == nullptr
                 && index < products_.size(); ++index) {
                if (!products_[index].selected || products_[index].localInventoryOnly) {
                    continue;
                }
                selected.push_back(SnapshotSelectedProduct(index));
            }
        } catch (...) {
            NativeUiLanguage::ShowMessageBox(window_,
                L"无法生成下载清单：所选范围需要的内存超过当前进程可用容量。\n"
                L"请缩短日期范围或减少已选产品后重试。",
                L"下载清单过大", MB_OK | MB_ICONWARNING);
            return;
        }
        if (selected.empty()) {
            NativeUiLanguage::ShowMessageBox(window_, L"请先选择至少一个精密产品。", L"GNSS 数据下载器", MB_OK | MB_ICONINFORMATION);
            return;
        }
        const auto selectedFolder = queued != nullptr
            ? queued->folder : WindowText(folder_);
        const std::filesystem::path folder = std::filesystem::path(selectedFolder);
        if (folder.empty()) {
            NativeUiLanguage::ShowMessageBox(window_, L"请选择保存目录。", L"GNSS 数据下载器", MB_OK | MB_ICONWARNING);
            return;
        }
        const HWND target = window_;
        // Hydrating routes creates one small DTO per product/date before the
        // transfer workers start.  Allowing an unrestricted "all products ×
        // all years" selection would allocate millions of DTOs and mirrors
        // and was a common source of apparent hangs and out-of-memory
        // termination.  Keep ordinary multi-year downloads unrestricted,
        // but fail early with an actionable message for an unmanageable
        // manifest instead of letting the background thread exhaust memory.
        std::size_t totalWork = 0;
        std::size_t manifestWork = 0;
        for (const auto& item : selected) {
            if (item.quickPlanRoutesDeferred) {
                const auto unresolved = std::ranges::count_if(item.days,
                    [](const auto& day) { return day.mirrors.empty(); });
                totalWork += item.days.size();
                manifestWork += static_cast<std::size_t>(unresolved);
            } else if (item.days.empty() && (!item.indexedFamilyKey.empty()
                || !item.coverageRanges.empty())) {
                const auto days = static_cast<std::size_t>(
                    std::max(0, item.product.availableDays));
                totalWork = days > kMaximumDownloadTasks - std::min(
                    totalWork, kMaximumDownloadTasks)
                    ? kMaximumDownloadTasks + 1
                    : totalWork + days;
                manifestWork = days > kMaximumDownloadTasks - std::min(
                    manifestWork, kMaximumDownloadTasks)
                    ? kMaximumDownloadTasks + 1
                    : manifestWork + days;
            } else {
                const auto days = item.days.size();
                totalWork = days > kMaximumDownloadTasks - std::min(
                    totalWork, kMaximumDownloadTasks)
                    ? kMaximumDownloadTasks + 1
                    : totalWork + days;
            }
        }
        if (totalWork > kMaximumDownloadTasks) {
            NativeUiLanguage::ShowMessageBox(window_,
                L"所选范围预计需要处理超过 25 万个产品日期，已为避免卡顿和内存耗尽而停止。\n"
                L"请缩短日期范围，或减少已选产品后再下载。",
                L"下载清单过大", MB_OK | MB_ICONWARNING);
            return;
        }
        if (totalWork == 0) {
            NativeUiLanguage::ShowMessageBox(window_, L"所选产品在当前范围内没有可下载日期。",
                L"GNSS 数据下载器", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (!downloading_) {
            if (downloadThread_.joinable()) downloadThread_.join();
            downloadControl_ = std::make_shared<DownloadControl>();
            downloadCancelRequested_ = false;
        }
        downloading_ = true;
        lastParentProgressTick_ = 0;
        SetBusy(true);
        NotifyParentBusyChanged();
        SendMessageW(progress_, PBM_SETPOS, 0, 0);
        SetWindowTextW(status_, manifestWork > 0
            ? std::format(L"正在生成下载清单：0/{} 个产品日期…",
                manifestWork).c_str()
            : L"下载清单已就绪，正在开始下载…");
        const auto queueId = queued == nullptr ? 0 : queued->id;
        auto work = [target, queueId, folder, datedFolders = DownloadSettings::Load().datedFolders,
             control = downloadControl_, manifestWork,
             selected = std::move(selected)](
                 const std::stop_token stopToken) mutable {
                auto finished = std::make_unique<PreciseFinished>();
                finished->journal = std::make_shared<DownloadJournal>(folder, L"products");
                std::size_t manifestCompleted = 0;
                // One shared gate covers manifest, per-file and byte-level
                // progress.  Without it, twelve workers can enqueue dozens
                // of WM_PRECISE_PROGRESS messages per second; the native
                // window then spends more time draining its queue than
                // painting or handling cancellation.
                std::atomic<ULONGLONG> lastUiProgressTick{0};
                const auto allowUiProgress =
                    [&lastUiProgressTick](const bool force) {
                        const auto now = GetTickCount64();
                        auto previous = lastUiProgressTick.load(
                            std::memory_order_relaxed);
                        for (;;) {
                            if (!force && now - previous < 100) {
                                return false;
                            }
                            if (lastUiProgressTick.compare_exchange_weak(
                                    previous, now,
                                    std::memory_order_relaxed,
                                    std::memory_order_relaxed)) {
                                return true;
                            }
                        }
                    };

                try {

                // Indexed product rows are intentionally hydrated once, before
                // starting transfers.  The resulting date tasks can then be
                // dispatched independently just like observation files.
                for (auto& item : selected) {
                    if (stopToken.stop_requested()) {
                        break;
                    }
                    if (item.quickPlanRoutesDeferred) {
                        std::unordered_map<std::string, std::vector<std::size_t>>
                            daysByFamily;
                        for (std::size_t index = 0; index < item.days.size();
                             ++index) {
                            if (!item.days[index].mirrors.empty()) continue;
                            if (item.days[index].logicalKey.empty()
                                || !IsPreciseSearchSummaryCategory(
                                    item.indexedCategory)) {
                                ++finished->failed;
                                ++manifestCompleted;
                                AppendFinishedError(*finished,
                                    item.product.id + L"：缺少可解析的产品族信息");
                                continue;
                            }
                            daysByFamily[item.days[index].logicalKey]
                                .push_back(index);
                        }
                        for (const auto& [familyKey, indexes] : daysByFamily) {
                            if (stopToken.stop_requested()) break;
                            QueryDate first = item.days[indexes.front()].date;
                            QueryDate last = first;
                            for (const auto index : indexes) {
                                const auto& date = item.days[index].date;
                                if (BundleDateCode(date) < BundleDateCode(first))
                                    first = date;
                                if (BundleDateCode(date) > BundleDateCode(last))
                                    last = date;
                            }
                            const auto manifestBase = manifestCompleted;
                            const auto reportManifest =
                                [target, queueId, manifestBase, manifestWork,
                                 &allowUiProgress,
                                 id = item.product.id,
                                 selectedDays = indexes.size()](
                                    const std::size_t completed,
                                    const std::size_t total) {
                                    if (!allowUiProgress(total > 0
                                            && completed >= total)) return;
                                    auto progress = std::make_unique<PreciseProgress>();
                                    progress->phase = PreciseProgress::Phase::Manifest;
                                    progress->completed = std::min(manifestWork,
                                        manifestBase + (total == 0 ? 0
                                            : completed * selectedDays / total));
                                    progress->total = manifestWork;
                                    progress->productId = id;
                                    progress->message = L"正在读取已选日期的下载路线";
                                    if (PostMessageW(target, WM_PRECISE_PROGRESS,
                                            static_cast<WPARAM>(queueId), reinterpret_cast<LPARAM>(
                                                progress.get()))) {
                                        progress.release();
                                    }
                                };
                            auto routes = LoadPreciseSearchFamilyRoutes(
                                item.indexedCategory, familyKey,
                                first, last, stopToken, reportManifest);
                            manifestCompleted += indexes.size();
                            if (stopToken.stop_requested()) break;
                            if (!routes.error.empty()) {
                                finished->failed += indexes.size();
                                AppendFinishedError(*finished,
                                    item.product.id + L"：" + routes.error);
                                continue;
                            }
                            std::unordered_map<int, std::size_t> routesByDate;
                            routesByDate.reserve(routes.records.size());
                            for (std::size_t index = 0;
                                 index < routes.records.size(); ++index) {
                                routesByDate.emplace(BundleDateCode(
                                    routes.records[index].date), index);
                            }
                            std::size_t missing = 0;
                            for (const auto index : indexes) {
                                auto& day = item.days[index];
                                const auto found = routesByDate.find(
                                    BundleDateCode(day.date));
                                if (found == routesByDate.end()
                                    || routes.records[found->second].mirrors.empty()) {
                                    ++missing;
                                    continue;
                                }
                                auto& record = routes.records[found->second];
                                day.logicalKey = std::move(record.logicalKey);
                                day.mirrors = std::move(record.mirrors);
                            }
                            if (missing > 0) {
                                finished->failed += missing;
                                AppendFinishedError(*finished, std::format(
                                    L"{}：{} 个已选日期没有可用下载路线",
                                    item.product.id, missing));
                            }
                        }
                        item.quickPlanRoutesDeferred = false;
                    }
                    if (item.days.empty() && !item.coverageRanges.empty()) {
                        item.days.reserve(static_cast<std::size_t>(
                            std::max(0, item.product.availableDays)));
                        for (const auto& interval : item.coverageRanges) {
                            for (auto day = interval.first;; day = AddDays(day, 1)) {
                                if (stopToken.stop_requested()) break;
                                item.days.push_back({day, {}, item.product.mirrors});
                                if (SameQueryDate(day, interval.last)) break;
                            }
                            if (stopToken.stop_requested()) break;
                        }
                    }
                    if (item.days.empty() && !item.indexedFamilyKey.empty()) {
                        Sp3ProductFamilyRoutesLookup routes;
                        const auto itemManifestWork = static_cast<std::size_t>(
                            std::max(0, item.product.availableDays));
                        const auto manifestBase = manifestCompleted;
                            const auto reportManifest =
                            [target, queueId, manifestBase, manifestWork,
                             &allowUiProgress,
                             id = item.product.id](
                                const std::size_t completed,
                                const std::size_t total) {
                                if (!allowUiProgress(
                                        total > 0 && completed >= total)) {
                                    return;
                                }
                                auto progress = std::make_unique<PreciseProgress>();
                                progress->phase = PreciseProgress::Phase::Manifest;
                                progress->completed = std::min(
                                    manifestWork, manifestBase + completed);
                                progress->total = manifestWork;
                                progress->productId = id;
                                progress->message = std::format(
                                    L"正在读取下载路线（当前产品 {}/{} 天）",
                                    completed, total);
                                if (PostMessageW(target, WM_PRECISE_PROGRESS, static_cast<WPARAM>(queueId),
                                        reinterpret_cast<LPARAM>(progress.get()))) {
                                    progress.release();
                                }
                            };
                        if (IsPreciseSearchSummaryCategory(
                                item.indexedCategory)) {
                            const auto compactRoutes =
                                LoadPreciseSearchFamilyRoutes(
                                    item.indexedCategory, item.indexedFamilyKey,
                                    item.startDate, item.endDate,
                                    stopToken, reportManifest);
                            manifestCompleted += itemManifestWork;
                            if (!compactRoutes.error.empty()) {
                                ++finished->failed;
                                AppendFinishedError(*finished,
                                    item.product.id + L"：" + compactRoutes.error);
                                continue;
                            }
                            item.days.reserve(compactRoutes.records.size());
                            for (const auto& dated : compactRoutes.records) {
                                item.days.push_back({dated.date, dated.logicalKey,
                                    dated.mirrors});
                            }
                            if (item.days.empty()) {
                                ++finished->failed;
                                AppendFinishedError(*finished,
                                    item.product.id + L"：所选范围没有可用下载路线");
                            }
                            continue;
                        }
                        if (item.indexedCategory
                            == static_cast<int>(PreciseProductCategory::VmfGrid) + 1) {
                            const auto vmfRoutes = LoadVmfFamilyRoutes(
                                item.indexedFamilyKey, item.startDate,
                                item.endDate, stopToken);
                            manifestCompleted += itemManifestWork;
                            if (!vmfRoutes.error.empty()) {
                                ++finished->failed;
                                AppendFinishedError(*finished,
                                    item.product.id + L"：" + vmfRoutes.error);
                                continue;
                            }
                            item.days.reserve(vmfRoutes.records.size());
                            for (const auto& dated : vmfRoutes.records) {
                                item.days.push_back({dated.date, dated.logicalKey,
                                    {dated.mirror}});
                            }
                            if (item.days.empty()) {
                                ++finished->failed;
                                AppendFinishedError(*finished,
                                    item.product.id + L"：所选范围没有可用下载路线");
                            }
                            continue;
                        }
                        if (item.indexedCategory
                            == static_cast<int>(PreciseProductCategory::Ionosphere) + 1) {
                            const auto ionexRoutes = LoadIonexProductFamilyRoutes(
                                item.indexedFamilyKey, item.startDate,
                                item.endDate, stopToken, reportManifest);
                            manifestCompleted += itemManifestWork;
                            if (!ionexRoutes.error.empty()) {
                                ++finished->failed;
                                AppendFinishedError(*finished,
                                    item.product.id + L"：" + ionexRoutes.error);
                                continue;
                            }
                            item.days.reserve(ionexRoutes.records.size());
                            for (const auto& dated : ionexRoutes.records) {
                                std::vector<PreciseProductMirror> mirrors;
                                mirrors.reserve(dated.record.routes.size());
                                for (const auto& route : dated.record.routes) {
                                    mirrors.push_back({
                                        Utf8ToWide(route.sourceKey),
                                        route.downloadUrl,
                                        Utf8ToWide(route.fileName)});
                                }
                                item.days.push_back({dated.date,
                                    dated.record.logicalKey,
                                    std::move(mirrors)});
                            }
                            if (item.days.empty()) {
                                ++finished->failed;
                                AppendFinishedError(*finished,
                                    item.product.id + L"：所选范围没有可用下载路线");
                            }
                            continue;
                        }
                        if (item.indexedCategory == 1) {
                            routes = LoadSp3ProductFamilyRoutes(
                                item.indexedFamilyKey, item.startDate,
                                item.endDate, stopToken, reportManifest);
                        } else if (item.indexedCategory == 2) {
                            routes = LoadClkProductFamilyRoutes(
                                item.indexedFamilyKey, item.startDate,
                                item.endDate, stopToken, reportManifest);
                        } else if (item.indexedCategory == 5) {
                            routes = LoadAttProductFamilyRoutes(
                                item.indexedFamilyKey, item.startDate,
                                item.endDate, stopToken, reportManifest);
                        }
                        manifestCompleted += itemManifestWork;
                        if (!routes.error.empty()) {
                            ++finished->failed;
                            AppendFinishedError(*finished,
                                item.product.id + L"：" + routes.error);
                            continue;
                        }
                        item.days.reserve(routes.records.size());
                        for (const auto& dated : routes.records) {
                            PreciseProductFile hydrated = item.product;
                            hydrated.mirrors.clear();
                            for (const auto& route : dated.record.routes) {
                                hydrated.mirrors.push_back({
                                    Utf8ToWide(route.sourceKey),
                                    route.downloadUrl,
                                    Utf8ToWide(route.fileName)});
                            }
                            item.days.push_back({dated.date, dated.record.logicalKey,
                                std::move(hydrated.mirrors)});
                        }
                        if (item.days.empty()) {
                            ++finished->failed;
                            AppendFinishedError(*finished,
                                item.product.id + L"：所选范围没有可用下载路线");
                        }
                    }
                }

                struct ProductDayTask {
                    std::size_t productIndex = 0;
                    std::size_t dayIndex = 0;
                };
                std::vector<ProductDayTask> tasks;
                for (std::size_t productIndex = 0;
                     productIndex < selected.size(); ++productIndex) {
                    for (std::size_t dayIndex = 0;
                         dayIndex < selected[productIndex].days.size();
                         ++dayIndex) {
                        if (!selected[productIndex].days[dayIndex].mirrors.empty()) {
                            tasks.push_back({productIndex, dayIndex});
                        }
                    }
                }
                const std::size_t totalWork = tasks.size();
                if (totalWork > 0 && !stopToken.stop_requested()) {
                    auto initial = std::make_unique<PreciseProgress>();
                    initial->phase = PreciseProgress::Phase::Download;
                    initial->completed = 0;
                    initial->total = totalWork;
                    initial->message = L"下载清单已就绪，自动并发最多 12 线程，按服务器动态调节";
                    if (PostMessageW(target, WM_PRECISE_PROGRESS, static_cast<WPARAM>(queueId),
                            reinterpret_cast<LPARAM>(initial.get()))) {
                        initial.release();
                    }

                    auto& automaticConcurrency =
                        SharedAutomaticDownloadConcurrency();
                    std::atomic_size_t nextTask{0};
                    std::atomic_size_t completedTasks{0};
                    std::mutex finishedMutex;
                    // De-duplicate by the actual destination path, rather
                    // than by URL.  The same archive URL can legitimately be
                    // selected for two dates (or categories), in which case
                    // skipping the second URL would leave its date folder
                    // empty.  Conversely, two mirror URLs yielding the same
                    // physical destination should only produce one file.
                    std::mutex completedDestinationsMutex;
                    std::unordered_set<std::wstring> completedDestinations;
                    std::condition_variable destinationCondition;
                    std::unordered_set<std::wstring> inFlightDestinations;

                    enum class DestinationClaim {
                        Claimed,
                        AlreadyComplete,
                        Cancelled,
                    };
                    const auto claimDestination =
                        [&inFlightDestinations, &completedDestinations,
                         &completedDestinationsMutex, &destinationCondition,
                         stopToken](const std::wstring& key) {
                            std::unique_lock lock(completedDestinationsMutex);
                            while (inFlightDestinations.contains(key)
                                   && !stopToken.stop_requested()) {
                                destinationCondition.wait_for(
                                    lock, std::chrono::milliseconds(100));
                            }
                            if (stopToken.stop_requested()) {
                                return DestinationClaim::Cancelled;
                            }
                            if (completedDestinations.contains(key)) {
                                return DestinationClaim::AlreadyComplete;
                            }
                            inFlightDestinations.insert(key);
                            return DestinationClaim::Claimed;
                        };
                    const auto releaseDestination =
                        [&inFlightDestinations, &completedDestinations,
                         &completedDestinationsMutex, &destinationCondition](
                            const std::wstring& key, const bool success) {
                            {
                                std::scoped_lock lock(completedDestinationsMutex);
                                inFlightDestinations.erase(key);
                                if (success) {
                                    completedDestinations.insert(key);
                                }
                            }
                            destinationCondition.notify_all();
                        };

                    const auto postProgress =
                        [target, queueId, &completedTasks, totalWork,
                         &allowUiProgress](
                            const std::wstring& id,
                            const std::wstring& message,
                            const std::uint64_t bytes = 0,
                            const std::uint64_t totalBytes = 0) {
                            if (!allowUiProgress(false)) {
                                return;
                            }
                            auto progress = std::make_unique<PreciseProgress>();
                            progress->phase = PreciseProgress::Phase::Download;
                            progress->completed = completedTasks.load();
                            progress->total = totalWork;
                            progress->currentBytes = bytes;
                            progress->currentTotal = totalBytes;
                            progress->productId = id;
                            progress->message = message;
                            if (PostMessageW(target, WM_PRECISE_PROGRESS, static_cast<WPARAM>(queueId),
                                    reinterpret_cast<LPARAM>(progress.get()))) {
                                progress.release();
                            }
                        };

                    const auto worker = [&] {
                        std::wstring claimedDestination;
                        bool destinationClaimed = false;
                        // If an exception occurs after claiming a
                        // destination, release it before this worker exits.
                        // Otherwise another worker could wait forever in
                        // claimDestination(), making a failed transfer look
                        // like a hung download.
                        try {
                        while (!stopToken.stop_requested()) {
                            if (control != nullptr
                                && !control->waitIfPaused(stopToken)) {
                                return;
                            }
                            const auto taskIndex = nextTask.fetch_add(1);
                            if (taskIndex >= tasks.size()) {
                                return;
                            }
                            const auto task = tasks[taskIndex];
                            const auto& item = selected[task.productIndex];
                            const auto& day = item.days[task.dayIndex];
                            const auto& product = item.product;
                            bool completed = false;
                            std::wstring lastError;
                            std::vector<std::size_t> pendingRoutes;
                            pendingRoutes.reserve(day.mirrors.size());
                            bool sawInvalidRoute = false;
                            for (std::size_t index = 0;
                                 index < day.mirrors.size(); ++index) {
                                const auto& route = day.mirrors[index];
                                if (route.url.empty()
                                    || !IsSafeProductFileName(route.fileName)) {
                                    sawInvalidRoute = true;
                                    continue;
                                }
                                pendingRoutes.push_back(index);
                            }
                            if (sawInvalidRoute && pendingRoutes.empty()) {
                                lastError = L"下载路线包含无效文件名或地址";
                            }
                            while (!pendingRoutes.empty()
                                   && !stopToken.stop_requested()) {
                                if (control != nullptr
                                    && !control->waitIfPaused(stopToken)) {
                                    return;
                                }
                                std::vector<std::wstring_view> candidates;
                                candidates.reserve(pendingRoutes.size());
                                for (const auto routeIndex : pendingRoutes) {
                                    candidates.push_back(
                                        day.mirrors[routeIndex].url);
                                }
                                auto acquired = automaticConcurrency.acquireAny(
                                    candidates, stopToken, control);
                                if (!acquired.has_value()) {
                                    break;
                                }
                                const auto pendingIndex = acquired->candidateIndex;
                                const auto routeIndex = pendingRoutes[pendingIndex];
                                pendingRoutes.erase(pendingRoutes.begin() + pendingIndex);
                                const auto& mirror = day.mirrors[routeIndex];
                                const auto categoryFolder = !datedFolders ? folder :
                                    product.category
                                        == PreciseProductCategory::Antenna
                                    ? folder / CategoryFolder(product.category)
                                        / (product.center == L"IGS"
                                            || product.center == L"EPN"
                                            || product.center == L"CODE"
                                            || product.center == L"NGS"
                                                ? product.center : L"OTHER")
                                    : folder
                                        / std::format(L"{:04}", day.date.year)
                                        / std::format(L"{:03}", day.date.dayOfYear)
                                        / CategoryFolder(product.category);
                                const auto productFolder = product.category == PreciseProductCategory::VmfGrid
                                    ? categoryFolder / VmfVariantDirectory(product.id) : categoryFolder;
                                const auto destination = productFolder / mirror.fileName;
                                // Windows paths are case-insensitive.  Keep
                                // one canonical textual key for the in-batch
                                // duplicate check so differently cased route
                                // records cannot schedule the same file twice.
                                const auto destinationKey = Lower(
                                    destination.lexically_normal().wstring());
                                // Do not hold a server-concurrency lease while
                                // waiting for another worker that targets the
                                // same file.  A worker may need that server
                                // for its fallback route; releasing here
                                // avoids a destination/host lock inversion.
                                {
                                    auto initialLease =
                                        std::move(acquired->lease);
                                }
                                const DownloadDestinationLock destinationLock(destination, stopToken);
                                if (!destinationLock) break;
                                // Prepare the guard before touching the
                                // shared in-flight set; if this copy itself
                                // fails, no claim has been made yet.
                                claimedDestination = destinationKey;
                                const auto claim = claimDestination(destinationKey);
                                if (claim == DestinationClaim::AlreadyComplete) {
                                    finished->journal->Record(L"ALREADY_PRESENT", product.id + L" " + FormatDate(day.date),
                                        destination, mirror.url, 0, L"Completed by another item in this task");
                                    {
                                        std::scoped_lock lock(finishedMutex);
                                        ++finished->skipped;
                                    }
                                    completed = true;
                                    break;
                                }
                                if (claim == DestinationClaim::Cancelled) {
                                    break;
                                }
                                destinationClaimed = true;
                                auto reacquired = automaticConcurrency.acquire(
                                    mirror.url, stopToken, control);
                                if (!reacquired.has_value()) {
                                    releaseDestination(destinationKey, false);
                                    destinationClaimed = false;
                                    claimedDestination.clear();
                                    break;
                                }
                                auto lease = std::move(*reacquired);
                                std::error_code error;
                                std::filesystem::create_directories(productFolder, error);
                                if (error) {
                                    lastError = L"无法创建目录：" + productFolder.wstring();
                                    HttpDownloadResult failedResult;
                                    failedResult.error = lastError;
                                    lease.finish(failedResult);
                                    releaseDestination(destinationKey, false);
                                    destinationClaimed = false;
                                    claimedDestination.clear();
                                    break;
                                }
                                error.clear();
                                const auto size = std::filesystem::file_size(destination, error);
                                if (!error && size > 0 && DownloadFileLooksValid(destination)) {
                                    finished->journal->Record(L"ALREADY_PRESENT", product.id + L" " + FormatDate(day.date),
                                        destination, mirror.url, size);
                                    HttpDownloadResult skippedResult;
                                    skippedResult.success = true;
                                    lease.finish(skippedResult);
                                    releaseDestination(destinationKey, true);
                                    destinationClaimed = false;
                                    claimedDestination.clear();
                                    {
                                        std::scoped_lock lock(finishedMutex);
                                        ++finished->skipped;
                                    }
                                    completed = true;
                                    break;
                                }

                                if (!error && size > 0) std::filesystem::remove(destination, error);
                                const auto snapshot = automaticConcurrency.snapshot(mirror.url);
                                postProgress(
                                    product.id,
                                    std::format(
                                        L"{}：尝试 {}：{}（自动并发 {}/{}）",
                                        FormatDate(day.date), mirror.source,
                                        mirror.fileName, snapshot.active,
                                        snapshot.limit));
                                const auto result = HttpDownloadFile(
                                    mirror.url,
                                    destination,
                                    stopToken,
                                    [target, queueId, &completedTasks, &allowUiProgress,
                                     id = product.id, source = mirror.source,
                                     date = day.date, totalWork](
                                        const std::uint64_t bytes,
                                        const std::uint64_t totalBytes) {
                                        if (!allowUiProgress(
                                                totalBytes > 0
                                                && bytes >= totalBytes)) {
                                            return;
                                        }
                                        auto progress = std::make_unique<PreciseProgress>();
                                        progress->phase = PreciseProgress::Phase::Download;
                                        progress->completed = completedTasks.load();
                                        progress->total = totalWork;
                                        progress->currentBytes = bytes;
                                        progress->currentTotal = totalBytes;
                                        progress->productId = id;
                                        progress->message = std::format(
                                            L"{}：正在从 {} 下载", FormatDate(date), source);
                                        if (PostMessageW(target, WM_PRECISE_PROGRESS, static_cast<WPARAM>(queueId),
                                                reinterpret_cast<LPARAM>(progress.get()))) {
                                            progress.release();
                                        }
                                    },
                                    false,
                                    control,
                                     600,
                                     2);
                                finished->journal->Record(result.success ? L"SUCCEEDED"
                                    : result.cancelled ? L"CANCELLED" : L"ATTEMPT_FAILED",
                                    product.id + L" " + FormatDate(day.date), destination,
                                    mirror.url, result.bytesWritten, result.error);
                                lease.finish(result);
                                releaseDestination(destinationKey, result.success);
                                destinationClaimed = false;
                                claimedDestination.clear();
                                if (result.success) {
                                    {
                                        std::scoped_lock lock(finishedMutex);
                                        ++finished->succeeded;
                                    }
                                    completed = true;
                                    break;
                                }
                                if (result.cancelled || stopToken.stop_requested()) {
                                    break;
                                }
                                lastError = mirror.source + L"：" + result.error;
                            }
                            if (!completed && !stopToken.stop_requested()) {
                                std::scoped_lock lock(finishedMutex);
                                ++finished->failed;
                                AppendFinishedError(*finished, product.id + L" "
                                    + FormatDate(day.date) + L" — " + lastError);
                            }
                            const auto completedCount = completedTasks.fetch_add(1) + 1;
                            postProgress(
                                product.id,
                                std::format(
                                    L"{}：{}", FormatDate(day.date),
                                    completed ? L"处理完成" : L"所有候选镜像均未成功"));
                            (void)completedCount;
                        }
                        } catch (...) {
                            if (destinationClaimed) {
                                releaseDestination(claimedDestination, false);
                                destinationClaimed = false;
                            }
                            std::scoped_lock lock(finishedMutex);
                            ++finished->failed;
                            AppendFinishedError(*finished,
                                DescribeCurrentException());
                        }
                    };

                    const auto workerCount = std::min<std::size_t>(
                        kAutomaticDownloadGlobalWorkerLimit, tasks.size());
                    std::vector<std::jthread> workers;
                    workers.reserve(workerCount);
                    for (std::size_t index = 0; index < workerCount; ++index) {
                        workers.emplace_back(worker);
                    }
                    workers.clear();
                }
                } catch (...) {
                    ++finished->failed;
                    AppendFinishedError(*finished, DescribeCurrentException());
                }
                // The only stop source for this jthread is the UI/destructor
                // cancellation path.  Set this after all workers have joined
                // so the flag is race-free even when a transfer is cancelled
                // while another worker is finishing its current file.
                if (stopToken.stop_requested()) {
                    finished->cancelled = true;
                }
                if (PostMessageW(target, WM_PRECISE_FINISHED, static_cast<WPARAM>(queueId),
                        reinterpret_cast<LPARAM>(finished.get()))) {
                    finished.release();
                }
            };
        if (queueId != 0) planWorkers_.Start(queueId, std::move(work));
        else downloadThread_ = std::jthread(std::move(work));
    }

    void HandleProgress(std::unique_ptr<PreciseProgress> progress, const std::uint64_t queueId = 0) {
        if (queueId != 0 && !planWorkers_.Contains(queueId)) return;
        if (!progress) {
            return;
        }
        // Progress messages posted before the stop request may still be in
        // the window queue.  Do not let them overwrite the explicit
        // "cancelling" status while the worker is winding down.
        if (!downloading_ || downloadCancelRequested_) {
            return;
        }
        double fraction = progress->total == 0
            ? 0.0
            : static_cast<double>(progress->completed)
                / static_cast<double>(progress->total);
        if (progress->currentTotal > 0 && progress->completed < progress->total) {
            fraction = (static_cast<double>(progress->completed)
                + static_cast<double>(progress->currentBytes)
                    / static_cast<double>(progress->currentTotal))
                / static_cast<double>(progress->total);
        }
        fraction = std::clamp(fraction, 0.0, 1.0);
        const double displayedFraction = queueId == 0 ? fraction
            : progress->phase == PreciseProgress::Phase::Manifest
                ? fraction * 0.2 : 0.2 + fraction * 0.8;
        if (queueId != 0) {
            const auto active = std::ranges::find_if(downloadQueue_,
                [&](const auto& item) { return item.id == queueId; });
            if (active != downloadQueue_.end()) {
                active->percent = std::clamp(static_cast<int>(
                    std::lround(displayedFraction * 100.0)), 0, 100);
            }
        }
        SendMessageW(progress_, PBM_SETPOS,
            static_cast<WPARAM>(displayedFraction * 1000.0), 0);
        if (progress->phase == PreciseProgress::Phase::Manifest) {
            SetWindowTextW(
                status_,
                std::format(
                    L"生成下载清单 [{}/{}] {}：{}",
                    progress->completed,
                    progress->total,
                    progress->productId,
                    progress->message).c_str());
            NotifyParentProgressChanged();
            return;
        }
        SetWindowTextW(
            status_,
            std::format(
                L"[{}/{}] {}：{}",
                progress->completed,
                progress->total,
                progress->productId,
                progress->message).c_str());
        NotifyParentProgressChanged();
    }

    void CompleteActiveQueueItem(const PreciseFinished* finished, const std::uint64_t queueId) {
        if (queueId == 0) return;
        const auto active = std::ranges::find_if(downloadQueue_,
            [&](const auto& item) { return item.id == queueId; });
        if (active != downloadQueue_.end()) {
            active->state = finished == nullptr ? L"failed"
                : finished->cancelled ? L"cancelled"
                : finished->failed > 0 ? L"failed" : L"done";
            active->percent = finished != nullptr && !finished->cancelled
                ? 100 : 0;
            active->selected = false;
            if (active->removeAfterCancel) downloadQueue_.erase(active);
        }
        activeQueueId_ = planWorkers_.First();
        NotifyParentBusyChanged();
        if (queueRunning_) DispatchNextQueuedDownload();
    }

    void HandleFinished(std::unique_ptr<PreciseFinished> finished, const std::uint64_t queueId = 0) {
        if (queueId != 0 && !planWorkers_.Contains(queueId)) return;
        if (queueId != 0) planWorkers_.Finish(queueId);
        downloading_ = planWorkers_.Size() != 0;
        if (!downloading_) {
            downloadCancelRequested_ = false;
            if (downloadControl_) { downloadControl_->resume(); downloadControl_.reset(); }
        }
        SetBusy(downloading_);
        if (!finished) {
            SendMessageW(progress_, PBM_SETPOS, 0, 0);
            SetWindowTextW(status_, L"等待下载任务。");
            NotifyParentBusyChanged();
            CompleteActiveQueueItem(nullptr, queueId);
            return;
        }
        std::wstring logNotice;
        if (finished->journal) {
            finished->journal->Finish(finished->succeeded, finished->skipped,
                finished->failed, finished->cancelled, finished->errors);
            logNotice = finished->journal->Path().empty()
                ? L" Download log could not be created in the save folder."
                : L" Download log: " + finished->journal->Path().wstring();
        }
        if (finished->cancelled) {
            SendMessageW(progress_, PBM_SETPOS, 0, 0);
            SetWindowTextW(status_, (NativeUiLanguage::Text(L"下载任务已取消，等待重新开始。") + logNotice).c_str());
            NotifyParentBusyChanged();
            CompleteActiveQueueItem(finished.get(), queueId);
            return;
        }
        if (!downloading_) SendMessageW(progress_, PBM_SETPOS, 1000, 0);
        const std::wstring summary = std::format(
                L"精密产品处理完成：成功 {}，已存在 {}，失败 {}。",
                finished->succeeded,
                finished->skipped,
                finished->failed) + logNotice;
        SetWindowTextW(status_, summary.c_str());
        NotifyParentBusyChanged();
        if (finished->failed > 0 && queueId == 0) {
            std::wstring details = summary + L"\n\n部分失败项目：\n";
            const std::size_t count = std::min<std::size_t>(8, finished->errors.size());
            for (std::size_t index = 0; index < count; ++index) {
                details += L"• " + finished->errors[index] + L"\n";
            }
            NativeUiLanguage::ShowMessageBox(window_, details.c_str(), L"GNSS 数据下载器", MB_OK | MB_ICONWARNING);
        }
        CompleteActiveQueueItem(finished.get(), queueId);
    }

private:
    void NotifyParentProgressChanged() noexcept {
        // The parent serializes the visible product list and both queues for
        // WebView.  File-level callbacks can arrive far faster than the UI
        // can paint, so publish at a human-visible cadence.
        const ULONGLONG now = GetTickCount64();
        if (lastParentProgressTick_ == 0
            || now - lastParentProgressTick_ >= 150) {
            lastParentProgressTick_ = now;
            NotifyParentBusyChanged();
        }
    }

    void NotifyParentBusyChanged() const noexcept {
        const HWND parent = GetParent(window_);
        if (parent != nullptr) {
            PostMessageW(parent, WM_PRECISE_BUSY_CHANGED, 0, 0);
        }
    }

    HWND Static(const wchar_t* text, const DWORD style) const {
        return CreateWindowExW(
            0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
            0, 0, 100, 24, window_, nullptr, instance_, nullptr);
    }

    HWND Button(const wchar_t* text, const int id, const DWORD style = 0) const {
        const DWORD buttonStyle = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
        return CreateWindowExW(
            0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | buttonStyle,
            0, 0, 100, 30, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance_, nullptr);
    }

    HWND Combo(const int id) const {
        const HWND combo = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST
                | WS_VSCROLL,
            0, 0, 100, 200, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance_, nullptr);
        if (combo != nullptr) {
            // Keep a useful drop-down viewport even when the closed combo is
            // compressed to a 31 px row by the responsive layout.
            SendMessageW(combo, CB_SETMINVISIBLE, 8, 0);
        }
        return combo;
    }

    void AddColumn(const int index, const wchar_t* text, const int width) const {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.iSubItem = index;
        column.cx = width;
        column.pszText = const_cast<wchar_t*>(text);
        ListView_InsertColumn(list_, index, &column);
    }

    void ResizeListColumns(const int availableWidth) const {
        // Keep the high-value identity columns visible on laptop widths and
        // let the filename/status columns absorb the remaining space.  The
        // list keeps its native horizontal scroll if a user opens an even
        // narrower transient window, but normal sizes no longer start with
        // a 1.4k-pixel table inside a 900-pixel card.
        constexpr std::array<int, 10> minimum{
            120, 86, 72, 58, 58, 48, 120, 48, 90, 78};
        constexpr std::array<int, 10> preferred{
            200, 132, 120, 94, 110, 68, 330, 70, 170, 105};
        constexpr int minimumTotal = [] {
            int total = 0;
            for (const int value : minimum) total += value;
            return total;
        }();
        constexpr int preferredTotal = [] {
            int total = 0;
            for (const int value : preferred) total += value;
            return total;
        }();
        const int target = std::max(1, availableWidth - 18);
        std::array<int, 10> widths = minimum;
        if (target >= preferredTotal) {
            widths = preferred;
        } else if (target > minimumTotal) {
            const int extra = target - minimumTotal;
            const int span = preferredTotal - minimumTotal;
            int assigned = 0;
            for (std::size_t index = 0; index < widths.size(); ++index) {
                widths[index] = minimum[index]
                    + (preferred[index] - minimum[index]) * extra / span;
                assigned += widths[index];
            }
            for (std::size_t index = 0;
                 assigned < target && index < widths.size(); ++index) {
                if (widths[index] < preferred[index]) {
                    ++widths[index];
                    ++assigned;
                }
            }
        }
        for (std::size_t index = 0; index < widths.size(); ++index) {
            ListView_SetColumnWidth(
                list_, static_cast<int>(index), widths[index]);
        }
    }

    void UpdateSummary() const {
        const auto selected = std::ranges::count_if(products_, [](const auto& product) {
            return product.selected;
        });
        const bool singleDay = periodStart_.year == periodEnd_.year
            && periodStart_.month == periodEnd_.month
            && periodStart_.day == periodEnd_.day;
        const std::wstring summary = singleDay
            ? std::format(
                L"日期 {}（DOY {}）：共 {} 个逻辑产品；当前显示 {} 个，已选择 {} 个。",
                FormatDate(periodStart_), FormatDoy(periodStart_.dayOfYear),
                products_.size(), visible_.size(), selected)
            : std::format(
                L"范围 {} 至 {}：共 {} 个逻辑产品；当前显示 {} 个，已选择 {} 个。",
                FormatDate(periodStart_), FormatDate(periodEnd_),
                products_.size(), visible_.size(), selected);
        SetWindowTextW(summary_, summary.c_str());
        EnableWindow(download_, selected > 0 || downloading_);
        EnableWindow(pause_, downloading_ && !downloadCancelRequested_);
    }

    void InvalidateQuickBundle() noexcept {
        quickBundleReady_ = false;
        quickBundleSelectionKeys_.clear();
        quickBundleDisplayedKeys_.clear();
        quickBundleRestoreSelection_ = false;
    }

    void SetBusy(const bool busy) const {
        if (busy) {
            // Do not let a search/category timer that was queued immediately
            // before a download rebuild the list in the middle of a transfer.
            KillTimer(window_, kPrecisionSearchTimer);
            KillTimer(window_, kPrecisionCatalogTimer);
        }
        for (const HWND control : {
                 date_, endDate_, generate_, quickPreset_, quickApply_, category_,
                 latency_, center_, completeness_,
                 troStation_, search_, list_,
                 selectAll_, clear_, folder_, browse_}) {
            EnableWindow(control, !catalogLoading_);
        }
        const bool hasSelection = std::ranges::any_of(
            products_, [](const auto& product) { return product.selected; });
        // Outside a transfer the primary action is only meaningful when a
        // product is selected.  During a transfer the same button becomes
        // the cancel action, so it must stay enabled in that state.
        EnableWindow(download_, busy || hasSelection);
        SetWindowTextW(download_, busy ? L"取消下载" : L"开始下载");
        EnableWindow(pause_, busy && !downloadCancelRequested_);
        SetWindowTextW(
            pause_,
            busy && downloadControl_ != nullptr && downloadControl_->isPaused()
                ? L"继续下载"
                : L"暂停下载");
    }

    void SetCatalogControlsEnabled(const bool enabled) const {
        for (const HWND control : {
                 date_, endDate_, generate_, quickPreset_, quickApply_,
                 category_, latency_, center_, completeness_, troStation_,
                 search_, list_, selectAll_, clear_, folder_, browse_}) {
            if (control != nullptr) EnableWindow(control, enabled);
        }
        const bool hasSelection = std::ranges::any_of(
            products_, [](const auto& product) { return product.selected; });
        if (download_ != nullptr) {
            EnableWindow(download_, downloading_ || (enabled && hasSelection));
        }
    }

public:
    [[nodiscard]] bool IsBusy() const noexcept {
        return downloading_ || catalogLoading_;
    }

    [[nodiscard]] bool IsCatalogLoading() const noexcept {
        return catalogLoading_;
    }

    void FocusFirstControl() const noexcept {
        for (HWND control = GetWindow(window_, GW_CHILD);
             control != nullptr;
             control = GetWindow(control, GW_HWNDNEXT)) {
            const LONG_PTR style = GetWindowLongPtrW(control, GWL_STYLE);
            if (IsWindowVisible(control) && IsWindowEnabled(control)
                && (style & WS_TABSTOP) != 0) {
                SetFocus(control);
                return;
            }
        }
        SetFocus(window_);
    }

    void MarkQuickBundleStale() noexcept {
        InvalidateQuickBundle();
    }

    void DownloadSelectedFromUi() {
        StartOrCancelDownload();
    }

    void CancelDownloadFromUi() {
        queueRunning_ = false;
        if (downloading_) {
            StartOrCancelDownload();
        }
        if (catalogLoading_
            && (quickBundleDownloadPending_ || quickBundleQueuePending_)) {
            quickBundleDownloadPending_ = false;
            quickBundleQueuePending_ = false;
            if (catalogThread_.joinable()) catalogThread_.request_stop();
            SetWindowTextW(status_, L"正在取消精密产品下载准备…");
        }
    }

    // The concept page mirrors the native product table.  Keep its row
    // checkboxes connected to the same selection vector used by the real
    // downloader so the package button never operates on a different set of
    // products than the one the user can see.
    void SetProductSelection(const std::wstring_view productId, const bool selected) {
        if (catalogLoading_ || productId.empty()) return;
        const std::wstring wanted = Lower(std::wstring(productId));
        bool changed = false;
        for (std::size_t index = 0; index < products_.size(); ++index) {
            auto& product = products_[index];
            if (Lower(product.id) == wanted) {
                if (product.selected != selected) {
                    product.selected = selected;
                    changed = true;
                    const auto row = std::find(visible_.begin(), visible_.end(), index);
                    if (row != visible_.end()) {
                        const auto item = static_cast<int>(row - visible_.begin());
                        ListView_RedrawItems(list_, item, item);
                    }
                }
                break;
            }
        }
        if (changed) {
            UpdateSummary();
            NotifyParentBusyChanged();
        }
    }

    void SetDateRange(const SYSTEMTIME& start, const SYSTEMTIME& end) {
        if (date_ == nullptr || endDate_ == nullptr) return;
        DateTime_SetSystemtime(date_, GDT_VALID, &start);
        DateTime_SetSystemtime(endDate_, GDT_VALID, &end);
        MarkQuickBundleStale();
    }

    void SetAnalysisCenter(const std::wstring_view center) {
        if (center_ == nullptr) return;
        const std::wstring wanted = Lower(std::wstring(center));
        int index = 0;
        static constexpr std::array<std::wstring_view, 57> values{
            L"all analysis centers", L"igs", L"code", L"emr/nrcan", L"esa",
            L"gfz", L"grg", L"jpl", L"mit", L"ngs", L"sio", L"usno",
            L"whu/wum", L"jgx", L"hust", L"iers", L"iac", L"jaxa",
            L"shao", L"bkg", L"ign", L"dlr", L"cas", L"upc",
            L"optimap", L"other", L"tu wien", L"wcc", L"asi", L"aus", L"bek", L"bev", L"csr", L"deo", L"eur", L"gop", L"gsi", L"ige", L"lpt", L"mut", L"ncl", L"nkg", L"olg", L"pdr", L"pgc", L"rga", L"rob", L"sgo", L"sut", L"tug", L"tum", L"ulr", L"upa", L"wut", L"gia", L"mcc", L"epn"};
        std::wstring normalized = wanted;
        if (normalized == L"igs (igso)") normalized = L"igs";
        for (std::size_t candidate = 1; candidate < values.size(); ++candidate) {
            if (normalized == values[candidate]) {
                index = static_cast<int>(candidate);
                break;
            }
        }
        SendMessageW(center_, CB_SETCURSEL, index, 0);
        MarkQuickBundleStale();
        RebuildList();
    }

    void SetProductFamily(const std::wstring_view family) {
        if (category_ == nullptr || family.empty()) return;
        customRules_.clear();
        customPresetName_.clear();
        const std::wstring value = Lower(std::wstring(family));
        int index = 0;
        if (value == L"sp3" || value.find(L"orbit") != std::wstring::npos) index = 1;
        if (value == L"clk") index = 2;
        else if (value == L"erp/eop") index = 3;
        else if (value == L"dcb") index = 4;
        else if (value == L"orbex" || value == L"attitude") index = 5;
        else if (value == L"ionex") index = 6;
        else if (value == L"tro" || value == L"troposphere") index = 7;
        else if (value == L"sinex") index = 8;
        else if (value == L"antex") index = 9;
        else if (value == L"broadcast" || value == L"broadcast ephemeris") index = 10;
        else if (value == L"osb") index = 11;
        else if (value == L"upd") index = 12;
        else if (value == L"vmf") index = 13;
        SendMessageW(category_, CB_SETCURSEL, index, 0);
        MarkQuickBundleStale();
        SetWindowTextW(
            status_,
            std::format(L"已选择 {} 产品类型；点击“查询产品”读取本地知识库。",
                        std::wstring(family)).c_str());
        NotifyParentBusyChanged();
    }

    void SetFilters(const SetPreciseFiltersRequest& request) {
        if (catalogLoading_) return;
        customRules_.clear();
        customPresetName_.clear();
        const auto lookupIndex = [](const std::wstring_view value,
                                    const std::initializer_list<std::wstring_view> options) {
            const std::wstring wanted = Lower(std::wstring(value));
            int index = 0;
            for (const auto option : options) {
                if (wanted == Lower(std::wstring(option))) return index;
                ++index;
            }
            return 0;
        };
        const int latencyIndex = lookupIndex(request.latency, {
            L"all", L"final", L"rapid", L"ultra-rapid", L"near real-time",
            L"real-time", L"predicted", L"daily", L"weekly", L"static", L"other",
            L"cumulative", L"multi-day", L"solution-other"});
        const int completenessIndex = lookupIndex(request.completeness, {
            L"any", L"100%", L"95%", L"90%", L"80%", L"50%",
            L"300 days", L"100 days", L"30 days", L"7 days", L"1 day"});
        SendMessageW(latency_, CB_SETCURSEL, latencyIndex, 0);
        SendMessageW(completeness_, CB_SETCURSEL, completenessIndex, 0);
        if (!request.completenessMode.empty()) {
            const auto mode = Lower(request.completenessMode);
            completenessMode_ = mode == L"ratio"
                ? PreciseCompletenessMode::Ratio
                : mode == L"days" ? PreciseCompletenessMode::Days
                                    : PreciseCompletenessMode::Ignore;
            completenessValue_ = completenessMode_ == PreciseCompletenessMode::Ratio
                ? std::clamp(request.completenessValue, 0.0, 100.0)
                : std::max(0.0, request.completenessValue);
        } else {
            constexpr std::array<double, 6> ratio{0, 100, 95, 90, 80, 50};
            constexpr std::array<double, 5> days{300, 100, 30, 7, 1};
            if (completenessIndex > 0
                && completenessIndex < static_cast<int>(ratio.size())) {
                completenessMode_ = PreciseCompletenessMode::Ratio;
                completenessValue_ = ratio[completenessIndex];
            } else if (completenessIndex >= static_cast<int>(ratio.size())) {
                completenessMode_ = PreciseCompletenessMode::Days;
                completenessValue_ = days[completenessIndex - ratio.size()];
            } else {
                completenessMode_ = PreciseCompletenessMode::Ignore;
                completenessValue_ = 0.0;
            }
        }
        SetWindowTextW(troStation_, request.troStation.c_str());
        SetWindowTextW(search_, request.search.c_str());
        MarkQuickBundleStale();
        RebuildList();
    }

    void SetQuickPreset(const int preset) {
        if (preset < 0 || preset > 6) return;
        customRules_.clear();
        customPresetName_.clear();
        SendMessageW(quickPreset_, CB_SETCURSEL, preset, 0);
        MarkQuickBundleStale();
        NotifyParentBusyChanged();
    }

    void SetCustomPreset(const SetPreciseCustomPresetRequest& request) {
        if (catalogLoading_) return;
        std::vector<PreciseCustomRule> parsed;
        std::size_t start = 0;
        while (start <= request.rules.size()) {
            const auto end = request.rules.find(L';', start);
            const auto text = request.rules.substr(
                start, end == std::wstring::npos ? std::wstring::npos : end - start);
            const auto first = text.find(L'~');
            const auto second = first == std::wstring::npos
                ? std::wstring::npos : text.find(L'~', first + 1);
            if (first != std::wstring::npos && second != std::wstring::npos) {
                PreciseCustomRule rule;
                rule.categoryIndex = CustomCategoryIndex(text.substr(0, first));
                rule.latencyIndex = CustomLatencyIndex(
                    text.substr(first + 1, second - first - 1));
                const auto third = text.find(L'~', second + 1);
                rule.center = text.substr(second + 1, third == std::wstring::npos
                    ? std::wstring::npos : third - second - 1);
                if (third != std::wstring::npos) rule.interval = text.substr(third + 1);
                if (rule.categoryIndex > 0) parsed.push_back(std::move(rule));
            }
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        if (parsed.empty()) {
            NativeUiLanguage::ShowMessageBox(window_, L"自定义方案至少需要一个有效产品规则。",
                L"自定义快速方案", MB_OK | MB_ICONWARNING);
            return;
        }
        customRules_ = std::move(parsed);
        customPresetName_ = request.name.empty() ? L"自定义方案" : request.name;
        SendMessageW(quickPreset_, CB_SETCURSEL, 0, 0);
        SendMessageW(category_, CB_SETCURSEL, 0, 0);
        SendMessageW(latency_, CB_SETCURSEL, 0, 0);
        SendMessageW(center_, CB_SETCURSEL, 0, 0);
        completenessMode_ = PreciseCompletenessMode::Ignore;
        completenessValue_ = 0.0;
        SetWindowTextW(quickInfo_, std::format(
            L"自定义方案“{}”：{} 条产品/等级/机构/采样间隔规则；查询结果只显示符合任一规则的产品。",
            customPresetName_, customRules_.size()).c_str());
        GenerateCatalog();
    }

    void FillUiCoverage(PreciseUiCoverageSnapshot& snapshot) const {
        const auto found = std::ranges::find_if(products_, [&](const auto& product) {
            return product.id == snapshot.id && (snapshot.family.empty()
                || PreciseProductCategoryName(product.category) == snapshot.family);
        });
        if (found == products_.end()) { snapshot.error = L"该查询结果已更新"; return; }
        const auto index = static_cast<std::size_t>(found - products_.begin());
        const int first = CalendarOrdinal(periodStart_.year, periodStart_.month, periodStart_.day);
        const int last = CalendarOrdinal(periodEnd_.year, periodEnd_.month, periodEnd_.day);
        std::set<int> available, unknown, beforeHistory;
        std::wstring historyStart;
        if (index < coverageRanges_.size()) {
            for (const auto& range : coverageRanges_[index]) {
                const int begin = CalendarOrdinal(range.first.year, range.first.month, range.first.day);
                const int end = CalendarOrdinal(range.last.year, range.last.month, range.last.day);
                for (int day = std::max(first, begin); day <= std::min(last, end); ++day) available.insert(day);
            }
        }
        if (index < productDays_.size())
            for (const auto& day : productDays_[index])
                available.insert(CalendarOrdinal(day.date.year, day.date.month, day.date.day));
        if (index < indexedFamilyKeys_.size() && !indexedFamilyKeys_[index].empty()) {
            const auto& key = indexedFamilyKeys_[index];
            if (found->category == PreciseProductCategory::VmfGrid) {
                const auto routes = LoadVmfFamilyRoutes(key, periodStart_, periodEnd_);
                snapshot.error = routes.error;
                for (const auto& day : routes.records)
                    available.insert(CalendarOrdinal(day.date.year, day.date.month, day.date.day));
            } else {
                const auto range = LoadPreciseSearchFamilyRange(
                    static_cast<int>(found->category) + 1, periodStart_, periodEnd_, true, key);
                snapshot.error = range.error;
                for (const auto& family : range.families)
                    for (const auto& day : family.availableDates)
                        available.insert(CalendarOrdinal(day.year, day.month, day.day));
                for (const auto& day : range.incompleteDates)
                    unknown.insert(CalendarOrdinal(day.year, day.month, day.day));
                for (const auto& day : range.beforeHistoryDates)
                    beforeHistory.insert(CalendarOrdinal(day.year, day.month, day.day));
                if (range.hasHistoryStart) historyStart = FormatDate(range.historyStart);
            }
        }
        if (found->category == PreciseProductCategory::Clock) {
            std::vector<QueryDate> availableDates;
            for (int ordinal : available) {
                const auto text = CalendarDateText(ordinal);
                int y=0,m=0,d=0;
                if (swscanf_s(text.c_str(), L"%d-%d-%d", &y,&m,&d)==3)
                    availableDates.push_back(MakeQueryDate(y,m,d));
            }
            const auto family = index < indexedFamilyKeys_.size()
                && !indexedFamilyKeys_[index].empty()
                ? indexedFamilyKeys_[index] : WideToUtf8(found->id);
            snapshot.antexJson = ClkHeaderDetailsJson(
                LoadClkHeaderRange(family, periodStart_, periodEnd_), availableDates);
        }
        if (found->category == PreciseProductCategory::UncalibratedPhaseDelay) {
            const auto inputs = LoadUpdDeclaredInputs(
                WideToUtf8(found->id), periodStart_, periodEnd_);
            snapshot.updDate = inputs.date;
            snapshot.updFile = inputs.fileName;
            snapshot.updInputFiles = inputs.files;
        }
        const bool exact = available.size() == static_cast<std::size_t>(found->availableDays);
        snapshot.json = CalendarCoverageJson(first, last, [&](int day) -> std::wstring {
            if (available.contains(day)) return L"available";
            if (snapshot.error.empty() && exact && beforeHistory.contains(day)) return L"historical";
            if (!snapshot.error.empty() || !exact || unknown.contains(day)) return L"unknown";
            return L"missing";
        });
        if (!historyStart.empty() && snapshot.json != L"null") {
            snapshot.json.pop_back();
            snapshot.json += L",\"historyStart\":\"" + historyStart + L"\"}";
        }
    }

    void PrepareRuleOptions() {
        if (gPreciseRuleOptionsReady.load(std::memory_order_acquire)
            || ruleOptionsThread_.joinable()) return;
        const HWND target = window_;
        ruleOptionsThread_ = std::jthread([target] {
            try { PrewarmPreciseRangeIndexes(); } catch (...) {}
            PostMessageW(target, WM_PRECISE_RULE_OPTIONS_READY, 0, 0);
        });
    }

    void FinishRuleOptions() {
        if (ruleOptionsThread_.joinable()) ruleOptionsThread_.join();
        PublishUiState();
    }

    void FillUiSnapshot(PreciseUiSnapshot& snapshot) const {
        snapshot.catalogRevision = catalogRevision_;
        snapshot.ruleOptionsReady = gPreciseRuleOptionsReady.load(std::memory_order_acquire);
        snapshot.ruleOptions.clear();
        if (snapshot.ruleOptionsReady) snapshot.ruleOptions = gPreciseRuleOptions;
        snapshot.busy = downloading_ || catalogLoading_;
        snapshot.transferring = downloading_;
        snapshot.paused = downloading_ && downloadControl_ != nullptr
            && downloadControl_->isPaused();
        snapshot.selected = static_cast<int>(std::ranges::count_if(
            products_, [](const auto& product) { return product.selected; }));
        snapshot.quickBundleReady = quickBundleReady_;
        snapshot.familyIndex = category_ == nullptr ? 0 : static_cast<int>(
            SendMessageW(category_, CB_GETCURSEL, 0, 0));
        snapshot.latencyIndex = latency_ == nullptr ? 0 : static_cast<int>(
            SendMessageW(latency_, CB_GETCURSEL, 0, 0));
        snapshot.centerIndex = center_ == nullptr ? 0 : static_cast<int>(
            SendMessageW(center_, CB_GETCURSEL, 0, 0));
        snapshot.completenessIndex = completeness_ == nullptr ? 0 : static_cast<int>(
            SendMessageW(completeness_, CB_GETCURSEL, 0, 0));
        snapshot.completenessMode = completenessMode_ == PreciseCompletenessMode::Ratio
            ? L"ratio" : completenessMode_ == PreciseCompletenessMode::Days
                ? L"days" : L"ignore";
        snapshot.completenessValue = completenessValue_;
        snapshot.quickPresetIndex = quickPreset_ == nullptr ? 0 : static_cast<int>(
            SendMessageW(quickPreset_, CB_GETCURSEL, 0, 0));
        snapshot.folder = WindowText(folder_);
        const int position = progress_ == nullptr
            ? 0 : static_cast<int>(SendMessageW(progress_, PBM_GETPOS, 0, 0));
        snapshot.progressPercent = std::clamp(position / 10, 0, 100);
        snapshot.status = WindowText(status_);
        snapshot.troStation = WindowText(troStation_);
        snapshot.search = WindowText(search_);
        const auto readDate = [](const HWND picker) {
            SYSTEMTIME value{};
            if (picker == nullptr
                || DateTime_GetSystemtime(picker, &value) != GDT_VALID) {
                return std::wstring{};
            }
            return std::format(
                L"{:04}-{:02}-{:02}", value.wYear, value.wMonth, value.wDay);
        };
        snapshot.startDate = readDate(date_);
        snapshot.endDate = readDate(endDate_);
        snapshot.products.clear();
        const std::size_t count = visible_.size();
        snapshot.products.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto& product = products_[visible_[index]];
            PreciseUiProductSnapshot item;
            item.id = product.id;
            item.name = product.name;
            item.family = PreciseProductCategoryName(product.category);
            item.center = PreciseProductDisplayCenter(product);
            item.latency = PreciseProductLatencyName(product.latency);
            item.solutionClass = PreciseProductSolutionClass(product);
            item.constellation = product.constellation;
            item.interval = PreciseProductSamplingInterval(product);
            item.formatVersion = product.formatVersion;
            item.preferredFileName = product.mirrors.empty()
                ? L"—" : product.mirrors.front().fileName;
            for (const auto& mirror : product.mirrors) {
                if (mirror.source.empty()) continue;
                if (!item.sources.empty()) item.sources += L"、";
                item.sources += mirror.source;
            }
            if (item.sources.empty()) item.sources = L"—";
            item.completeness = static_cast<int>(std::lround(std::clamp(
                product.completenessPercent, 0.0, 100.0)));
            item.availableDays = product.availableDays;
            item.expectedDays = product.expectedDays;
            item.availability = product.expectedDays > 0
                ? std::format(
                    L"{}/{} 天 ({:.1f}%)", product.availableDays,
                    product.expectedDays, product.completenessPercent)
                : std::format(L"{}%", item.completeness);
            item.status = product.status.empty() ? L"待下载" : product.status;
            item.selected = product.selected;
            item.localInventoryOnly = product.localInventoryOnly;
            snapshot.products.push_back(std::move(item));
        }
        snapshot.queueRunning = queueRunning_;
        snapshot.activeQueueId = activeQueueId_;
        snapshot.queue.clear();
        snapshot.queue.reserve(downloadQueue_.size());
        for (const auto& item : downloadQueue_) {
            snapshot.queue.push_back({item.id, item.label,
                FormatDate(item.start) + L"—" + FormatDate(item.end),
                item.state, item.percent, item.selected});
        }
    }

    void PublishUiState() const noexcept {
        NotifyParentBusyChanged();
    }

    void TogglePause() const {
        if (!downloading_ || downloadCancelRequested_
            || downloadControl_ == nullptr) {
            return;
        }
        if (downloadControl_->isPaused()) {
            downloadControl_->resume();
            SetWindowTextW(pause_, L"暂停下载");
            SetWindowTextW(status_, L"已继续精密产品下载。\u2026");
        } else {
            downloadControl_->pause();
            SetWindowTextW(pause_, L"继续下载");
            SetWindowTextW(status_, L"精密产品下载已暂停；点击“继续下载”恢复。\u2026");
        }
    }

private:
    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND heading_ = nullptr;
    HWND explanation_ = nullptr;
    HWND quickLabel_ = nullptr;
    HWND quickPreset_ = nullptr;
    HWND quickApply_ = nullptr;
    HWND quickInfo_ = nullptr;
    HWND dateLabel_ = nullptr;
    HWND date_ = nullptr;
    HWND endDateLabel_ = nullptr;
    HWND endDate_ = nullptr;
    HWND generate_ = nullptr;
    HWND categoryLabel_ = nullptr;
    HWND category_ = nullptr;
    HWND latencyLabel_ = nullptr;
    HWND latency_ = nullptr;
    HWND centerLabel_ = nullptr;
    HWND center_ = nullptr;
    HWND completenessLabel_ = nullptr;
    HWND completeness_ = nullptr;
    HWND troStationLabel_ = nullptr;
    HWND troStation_ = nullptr;
    HWND searchLabel_ = nullptr;
    HWND search_ = nullptr;
    HWND list_ = nullptr;
    HWND summary_ = nullptr;
    HWND selectAll_ = nullptr;
    HWND clear_ = nullptr;
    HWND folderLabel_ = nullptr;
    HWND folder_ = nullptr;
    HWND browse_ = nullptr;
    HWND download_ = nullptr;
    HWND pause_ = nullptr;
    HWND progress_ = nullptr;
    HWND status_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    QueryDate currentDate_{};
    QueryDate periodStart_{};
    QueryDate periodEnd_{};
    std::vector<PreciseProductFile> products_;
    std::uint64_t catalogRevision_ = 1;
    std::vector<std::vector<PreciseDayFiles>> productDays_;
    std::vector<std::vector<PreciseCoverageRange>> coverageRanges_;
    // Non-empty entries identify rows supplied by the SP3/CLK/ATT family
    // index.  Such rows keep their daily logical keys in the shared cache and
    // hydrate physical download routes only when the user starts a transfer.
    std::vector<std::string> indexedFamilyKeys_;
    std::vector<std::size_t> visible_;
    std::vector<std::wstring> centerFilters_;
    std::vector<PreciseCustomRule> customRules_;
    std::wstring customPresetName_;
    PreciseCompletenessMode completenessMode_ = PreciseCompletenessMode::Ignore;
    double completenessValue_ = 0.0;
    std::jthread catalogThread_;
    std::unique_ptr<PreciseQuickBundleResult> completedQuickBundle_;
    bool catalogLoading_ = false;
    std::jthread ruleOptionsThread_;
    std::jthread downloadThread_;
    DownloadPlanWorkers planWorkers_;
    DownloadControlPtr downloadControl_;
    std::vector<PreciseQueuedDownload> downloadQueue_;
    std::uint64_t nextQueueId_ = 0;
    std::uint64_t activeQueueId_ = 0;
    bool queueRunning_ = false;
    bool syncing_ = false;
    bool downloading_ = false;
    bool downloadCancelRequested_ = false;
    ULONGLONG lastParentProgressTick_ = 0;
    bool quickBundleReady_ = false;
    bool quickBundleDownloadPending_ = false;
    bool quickBundleQueuePending_ = false;
    int quickBundlePreset_ = -1;
    QueryDate quickBundleStart_{};
    QueryDate quickBundleEnd_{};
    PreciseBundleRequest quickBundleRequest_{};
    PreciseBundlePlanResult quickBundleResult_{};
    std::unordered_set<std::wstring> quickBundleSelectionKeys_;
    std::vector<std::wstring> quickBundleDisplayedKeys_;
    bool quickBundleRestoreSelection_ = false;
};

LRESULT CALLBACK Procedure(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    auto* state = reinterpret_cast<PreciseProductView*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = new PreciseProductView(window, create->hInstance);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    case WM_CREATE:
        return state != nullptr && state->CreateControls() ? 0 : -1;
    case WM_ERASEBKGND:
        // PaintBackground() owns the complete client surface.  Returning
        // non-zero avoids a second default-theme erase pass during resize.
        return 1;
    case WM_PAINT:
        if (state != nullptr) {
            PAINTSTRUCT paint{};
            const HDC deviceContext = BeginPaint(window, &paint);
            state->PaintBackground(deviceContext);
            EndPaint(window, &paint);
            return 0;
        }
        break;
    case WM_SIZE:
        if (state != nullptr && wParam != SIZE_MINIMIZED) {
            state->Layout();
        }
        return 0;
    case WM_PRECISE_TRIGGER_SEARCH:
        if (state != nullptr) {
            state->GenerateCatalog();
            state->PublishUiState();
        }
        return 0;
    case WM_PRECISE_TRIGGER_DOWNLOAD:
        if (state != nullptr) {
            state->DownloadSelectedFromUi();
            state->PublishUiState();
        }
        return 0;
    case WM_PRECISE_TRIGGER_PAUSE:
        if (state != nullptr) {
            state->TogglePause();
            state->PublishUiState();
        }
        return 0;
    case WM_PRECISE_TRIGGER_CANCEL:
        if (state != nullptr) {
            state->CancelDownloadFromUi();
            state->PublishUiState();
        }
        return 0;
    case WM_PRECISE_SELECT_PRODUCT:
        if (state != nullptr && wParam != 0) {
            const auto* request = reinterpret_cast<const struct SelectProductRequest*>(wParam);
            if (request != nullptr) {
                state->SetProductSelection(request->productId, request->selected);
            }
        }
        return 0;
    case WM_PRECISE_SET_DATES:
        if (state != nullptr && wParam != 0) {
            const auto* request = reinterpret_cast<const SetPreciseDatesRequest*>(wParam);
            if (request != nullptr) {
                state->SetDateRange(request->start, request->end);
            }
        }
        return 0;
    case WM_PRECISE_SET_CENTER:
        if (state != nullptr && wParam != 0) {
            const auto* request = reinterpret_cast<const SetPreciseCenterRequest*>(wParam);
            if (request != nullptr) {
                state->SetAnalysisCenter(request->center);
            }
        }
        return 0;
    case WM_PRECISE_QUERY_COVERAGE:
        if (state && wParam) state->FillUiCoverage(*reinterpret_cast<PreciseUiCoverageSnapshot*>(wParam));
        return 0;
    case WM_PRECISE_QUERY_SNAPSHOT:
        if (state != nullptr && wParam != 0) {
            auto* snapshot = reinterpret_cast<PreciseUiSnapshot*>(wParam);
            if (snapshot != nullptr) {
                state->FillUiSnapshot(*snapshot);
            }
        }
        return 0;
    case WM_PRECISE_SET_FAMILY:
        if (state != nullptr && wParam != 0) {
            const auto* request = reinterpret_cast<const SetPreciseFamilyRequest*>(wParam);
            if (request != nullptr) {
                state->SetProductFamily(request->family);
            }
        }
        return 0;
    case WM_PRECISE_SET_FILTERS:
        if (state != nullptr && wParam != 0) {
            const auto* request = reinterpret_cast<const SetPreciseFiltersRequest*>(wParam);
            if (request != nullptr) state->SetFilters(*request);
        }
        return 0;
    case WM_PRECISE_SELECT_VISIBLE:
        if (state != nullptr) state->SelectVisible(wParam != 0);
        return 0;
    case WM_PRECISE_BROWSE_FOLDER:
        if (state != nullptr) state->BrowseFolder();
        return 0;
    case WM_PRECISE_SET_QUICK_PRESET:
        if (state != nullptr) state->SetQuickPreset(static_cast<int>(wParam));
        return 0;
    case WM_PRECISE_APPLY_QUICK_PRESET:
        if (state != nullptr) {
            state->ApplyQuickPreset();
            state->PublishUiState();
        }
        return 0;
    case WM_PRECISE_SET_CUSTOM_PRESET:
        if (state != nullptr && wParam != 0) {
            const auto* request = reinterpret_cast<const SetPreciseCustomPresetRequest*>(wParam);
            if (request != nullptr) state->SetCustomPreset(*request);
        }
        return 0;
    case WM_PRECISE_ADD_DOWNLOAD_PLAN:
        if (state != nullptr) state->AddSelectedToDownloadPlan();
        return 0;
    case WM_PRECISE_START_DOWNLOAD_PLAN:
        if (state != nullptr) state->StartQueuedDownloadPlan();
        return 0;
    case WM_PRECISE_UPDATE_DOWNLOAD_PLAN_ITEM:
        if (state != nullptr && wParam != 0) {
            const auto* request = reinterpret_cast<const UpdatePreciseQueueRequest*>(wParam);
            state->UpdateQueuedDownloadItem(request->id, request->action);
        }
        return 0;
    case WM_COMMAND:
        if (state == nullptr) {
            break;
        }
        switch (LOWORD(wParam)) {
        case IdGenerate:
            if (HIWORD(wParam) == BN_CLICKED) state->GenerateCatalog();
            return 0;
        case IdQuickApply:
            if (HIWORD(wParam) == BN_CLICKED) state->ApplyQuickPreset();
            return 0;
        case IdQuickPreset:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                state->MarkQuickBundleStale();
            }
            return 0;
        case IdCategory:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                // A keyboard walk through the category list can emit several
                // selection changes in one gesture.  Coalesce those changes
                // so each choice does not synchronously rescan SQLite and
                // rebuild the table.
                SetTimer(window, kPrecisionCatalogTimer, 180, nullptr);
            }
            return 0;
        case IdLatency:
        case IdCenter:
        case IdCompleteness:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                state->MarkQuickBundleStale();
                state->RebuildList();
            }
            return 0;
        case IdSearch:
            if (HIWORD(wParam) == EN_CHANGE) {
                // Rebuilding the non-virtual product table for every key
                // stroke is expensive on multi-year catalogues.  Debounce
                // the search field so a normal word or paste produces one
                // rebuild instead of a burst of full table refreshes.
                SetTimer(window, kPrecisionSearchTimer, 180, nullptr);
            }
            return 0;
        case IdSelectAll:
            if (HIWORD(wParam) == BN_CLICKED) state->SelectVisible(true);
            return 0;
        case IdClear:
            if (HIWORD(wParam) == BN_CLICKED) state->SelectVisible(false);
            return 0;
        case IdBrowse:
            if (HIWORD(wParam) == BN_CLICKED) state->BrowseFolder();
            return 0;
        case IdFolder:
            if (HIWORD(wParam) == EN_KILLFOCUS) {
                state->SaveFolderPreference();
            }
            return 0;
        case IdDownload:
            if (HIWORD(wParam) == BN_CLICKED) state->DownloadSelectedFromUi();
            return 0;
        case IdPauseDownload:
            if (HIWORD(wParam) == BN_CLICKED) state->TogglePause();
            return 0;
        default:
            break;
        }
        break;
    case WM_TIMER:
        if (state != nullptr && wParam == kPrecisionSearchTimer) {
            KillTimer(window, kPrecisionSearchTimer);
            if (!IsWindowVisible(window) || state->IsCatalogLoading()) {
                return 0;
            }
            state->RebuildList();
            return 0;
        }
        if (state != nullptr && wParam == kPrecisionCatalogTimer) {
            KillTimer(window, kPrecisionCatalogTimer);
            if (!IsWindowVisible(window) || state->IsCatalogLoading()) {
                return 0;
            }
            state->GenerateCatalog();
            return 0;
        }
        break;
    case WM_DRAWITEM:
        if (state != nullptr && lParam != 0) {
            const auto& draw = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (draw.CtlType == ODT_BUTTON) {
                DrawPrecisionButton(draw);
                return TRUE;
            }
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
        if (state != nullptr) {
            const HDC deviceContext = reinterpret_cast<HDC>(wParam);
            return reinterpret_cast<LRESULT>(state->HandleControlColor(
                message, deviceContext,
                reinterpret_cast<HWND>(lParam)));
        }
        break;
    case WM_NOTIFY:
        if (state != nullptr) {
            const auto* header = reinterpret_cast<NMHDR*>(lParam);
            if ((header->idFrom == IdDate || header->idFrom == IdEndDate)
                && header->code == DTN_DATETIMECHANGE) {
                state->MarkQuickBundleStale();
                return 0;
            }
            if (header->idFrom == IdList) {
                if (header->code == LVN_GETDISPINFOW) {
                    state->HandleListDisplay(*reinterpret_cast<NMLVDISPINFOW*>(lParam));
                    return 0;
                }
                if (header->code == LVN_ITEMCHANGED) {
                    state->HandleItemChanged(*reinterpret_cast<NMLISTVIEW*>(lParam));
                    return 0;
                }
                if (header->code == NM_CUSTOMDRAW) {
                    return state->HandleListCustomDraw(
                        *reinterpret_cast<NMLVCUSTOMDRAW*>(lParam));
                }
            }
        }
        break;
    case WM_PRECISE_PROGRESS:
        {
            auto progress = std::unique_ptr<PreciseProgress>(
                reinterpret_cast<PreciseProgress*>(lParam));
            if (state != nullptr) {
                state->HandleProgress(std::move(progress), static_cast<std::uint64_t>(wParam));
            }
            return 0;
        }
    case WM_PRECISE_FINISHED:
        {
            auto finished = std::unique_ptr<PreciseFinished>(
                reinterpret_cast<PreciseFinished*>(lParam));
            if (state != nullptr) {
                state->HandleFinished(std::move(finished), static_cast<std::uint64_t>(wParam));
            }
            return 0;
        }
    case WM_PRECISE_CATALOG_FINISHED:
        {
            auto result = std::unique_ptr<PreciseCatalogResult>(
                reinterpret_cast<PreciseCatalogResult*>(lParam));
            if (state != nullptr) {
                state->HandleCatalogFinished(std::move(result));
            }
            return 0;
        }
    case WM_PRECISE_QUICK_BUNDLE_FINISHED:
        {
            auto result = std::unique_ptr<PreciseQuickBundleResult>(
                reinterpret_cast<PreciseQuickBundleResult*>(lParam));
            if (state != nullptr) {
                state->HandleQuickBundleFinished(std::move(result));
            }
            return 0;
        }
    case WM_PRECISE_QUERY_BUSY:
        return state != nullptr && state->IsBusy() ? 1 : 0;
    case WM_PRECISE_FOCUS_FIRST:
        if (state != nullptr) {
            state->FocusFirstControl();
        }
        return 0;
    case WM_PRECISE_RULE_OPTIONS_READY:
        if (state != nullptr) state->FinishRuleOptions();
        return 0;
    case WM_NCDESTROY:
        delete state;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

bool PreciseProductViewIsBusy(const HWND view) noexcept {
    // Keep this message private to the child implementation; the numeric
    // value is intentionally outside the progress/finished pair above.
    constexpr UINT kQueryBusyMessage = WM_APP + 42;
    return view != nullptr
        && IsWindow(view)
        && SendMessageW(view, kQueryBusyMessage, 0, 0) != 0;
}

void FocusPreciseProductView(const HWND view) noexcept {
    if (view == nullptr || !IsWindow(view)) {
        return;
    }
    constexpr UINT kFocusMessage = WM_APP + 44;
    SendMessageW(view, kFocusMessage, 0, 0);
}

void TriggerPreciseSearch(const HWND view) noexcept {
    if (view != nullptr && IsWindow(view)) {
        PostMessageW(view, WM_PRECISE_TRIGGER_SEARCH, 0, 0);
    }
}

void TriggerPreciseDownload(const HWND view) noexcept {
    if (view != nullptr && IsWindow(view)) {
        PostMessageW(view, WM_PRECISE_TRIGGER_DOWNLOAD, 0, 0);
    }
}

void AddSelectedPreciseProductsToPlan(const HWND view) noexcept {
    if (view != nullptr && IsWindow(view)) {
        PostMessageW(view, WM_PRECISE_ADD_DOWNLOAD_PLAN, 0, 0);
    }
}

void StartPreciseDownloadPlan(const HWND view) noexcept {
    if (view != nullptr && IsWindow(view)) {
        PostMessageW(view, WM_PRECISE_START_DOWNLOAD_PLAN, 0, 0);
    }
}

void UpdatePreciseDownloadPlanItem(
    const HWND view, const std::uint64_t id,
    const wchar_t* action) noexcept {
    if (view == nullptr || !IsWindow(view) || action == nullptr) return;
    try {
        UpdatePreciseQueueRequest request{id, action};
        SendMessageW(view, WM_PRECISE_UPDATE_DOWNLOAD_PLAN_ITEM,
            reinterpret_cast<WPARAM>(&request), 0);
    } catch (...) {
    }
}

void TriggerPrecisePause(const HWND view) noexcept {
    if (view != nullptr && IsWindow(view)) {
        PostMessageW(view, WM_PRECISE_TRIGGER_PAUSE, 0, 0);
    }
}

void TriggerPreciseCancel(const HWND view) noexcept {
    if (view != nullptr && IsWindow(view)) {
        PostMessageW(view, WM_PRECISE_TRIGGER_CANCEL, 0, 0);
    }
}

void SelectPreciseProduct(
    const HWND view, const wchar_t* productId, const bool selected) noexcept {
    if (view == nullptr || !IsWindow(view) || productId == nullptr
        || *productId == L'\0') {
        return;
    }
    try {
        SelectProductRequest request{productId, selected};
        SendMessageW(
            view, WM_PRECISE_SELECT_PRODUCT,
            reinterpret_cast<WPARAM>(&request), 0);
    } catch (...) {
        // The bridge is best-effort.  A failed allocation must not take down
        // the main window merely because the concept page was clicked.
    }
}

void SetPreciseDateRange(
    const HWND view, const SYSTEMTIME& start, const SYSTEMTIME& end) noexcept {
    if (view == nullptr || !IsWindow(view)) return;
    try {
        SetPreciseDatesRequest request{start, end};
        SendMessageW(
            view, WM_PRECISE_SET_DATES,
            reinterpret_cast<WPARAM>(&request), 0);
    } catch (...) {
        // Keep the visual shell non-fatal if the bridge cannot be delivered.
    }
}

void SetPreciseAnalysisCenter(
    const HWND view, const wchar_t* center) noexcept {
    if (view == nullptr || !IsWindow(view) || center == nullptr) return;
    try {
        SetPreciseCenterRequest request{center};
        SendMessageW(
            view, WM_PRECISE_SET_CENTER,
            reinterpret_cast<WPARAM>(&request), 0);
    } catch (...) {
        // Best-effort UI synchronization; the native filters remain usable.
    }
}

bool QueryPreciseProductCoverage(HWND view, PreciseUiCoverageSnapshot& snapshot) noexcept {
    if (!view || !IsWindow(view)) return false;
    try {
        SendMessageW(view, WM_PRECISE_QUERY_COVERAGE, reinterpret_cast<WPARAM>(&snapshot), 0);
        return true;
    } catch (...) { return false; }
}

bool QueryPreciseProductSnapshot(
    const HWND view, PreciseUiSnapshot& snapshot) noexcept {
    if (view == nullptr || !IsWindow(view)) return false;
    try {
        SendMessageW(
            view, WM_PRECISE_QUERY_SNAPSHOT,
            reinterpret_cast<WPARAM>(&snapshot), 0);
        return true;
    } catch (...) {
        return false;
    }
}

void SetPreciseProductFamily(
    const HWND view, const wchar_t* family) noexcept {
    if (view == nullptr || !IsWindow(view) || family == nullptr
        || *family == L'\0') {
        return;
    }
    try {
        SetPreciseFamilyRequest request{family};
        SendMessageW(
            view, WM_PRECISE_SET_FAMILY,
            reinterpret_cast<WPARAM>(&request), 0);
    } catch (...) {
        // The native category remains unchanged if synchronization fails.
    }
}

void SetPreciseFilters(
    const HWND view, const wchar_t* latency, const wchar_t* completeness,
    const wchar_t* troStation, const wchar_t* search,
    const wchar_t* completenessMode,
    const double completenessValue) noexcept {
    if (view == nullptr || !IsWindow(view)) return;
    try {
        SetPreciseFiltersRequest request{
            latency == nullptr ? L"" : latency,
            completeness == nullptr ? L"" : completeness,
            troStation == nullptr ? L"" : troStation,
            search == nullptr ? L"" : search,
            completenessMode == nullptr ? L"" : completenessMode,
            completenessValue};
        SendMessageW(view, WM_PRECISE_SET_FILTERS,
            reinterpret_cast<WPARAM>(&request), 0);
    } catch (...) {}
}

void SelectAllVisiblePreciseProducts(
    const HWND view, const bool selected) noexcept {
    if (view != nullptr && IsWindow(view)) {
        SendMessageW(view, WM_PRECISE_SELECT_VISIBLE, selected ? 1 : 0, 0);
    }
}

void BrowsePreciseProductFolder(const HWND view) noexcept {
    if (view != nullptr && IsWindow(view)) {
        SendMessageW(view, WM_PRECISE_BROWSE_FOLDER, 0, 0);
    }
}

void SetPreciseQuickPreset(const HWND view, const int preset) noexcept {
    if (view != nullptr && IsWindow(view)) {
        SendMessageW(view, WM_PRECISE_SET_QUICK_PRESET,
            static_cast<WPARAM>(preset), 0);
    }
}

void ApplyPreciseQuickPreset(const HWND view) noexcept {
    if (view != nullptr && IsWindow(view)) {
        PostMessageW(view, WM_PRECISE_APPLY_QUICK_PRESET, 0, 0);
    }
}

void SetPreciseCustomPreset(
    const HWND view, const wchar_t* name, const wchar_t* rules) noexcept {
    if (view == nullptr || !IsWindow(view)) return;
    try {
        const SetPreciseCustomPresetRequest request{
            name == nullptr ? L"" : name,
            rules == nullptr ? L"" : rules};
        SendMessageW(view, WM_PRECISE_SET_CUSTOM_PRESET,
            reinterpret_cast<WPARAM>(&request), 0);
    } catch (...) {
    }
}

bool RegisterPreciseProductViewClass(const HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = Procedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kClassName;
    return RegisterClassExW(&windowClass) != 0
        || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND CreatePreciseProductView(
    const HWND parent,
    const HINSTANCE instance,
    const int controlId) {
    return CreateWindowExW(
        WS_EX_CONTROLPARENT, kClassName, L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 100, 100, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
        instance, nullptr);
}
