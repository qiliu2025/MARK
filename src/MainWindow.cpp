#include "DownloadPlan.h"
#include "DownloadFileValidation.h"
#include "DownloadJournal.h"
#include "NativeUiLanguage.h"
#include "MainWindow.h"

#include "DateUtil.h"
#include "DownloadFolderState.h"
#include "DownloadSettings.h"
#include "ObservationPostprocess.h"
#include "CountryFlags.h"
#include "../resources/resource.h"
#include "DownloadConcurrency.h"
#include "HttpClient.h"
#include "IgsAvailabilityDb.h"
#include "IgsData.h"
#include "MapView.h"
#include "Models.h"
#include "NetworkAvailabilityDb.h"
#include "ObservationSearchDb.h"
#include "PreciseProductView.h"
#include "SiteLogDb.h"
#include "SpaceWeather.h"
#include "StationSelection.h"
#include "StationCoordinateEvidence.h"
#include "WebWorkbenchHost.h"

#include <windows.h>
#include <commctrl.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kMainWindowClassName[] = L"IGSDownloaderMainWindow";
constexpr wchar_t kApplicationTitle[] = L"MARK";

// Neutral colours for native controls and navigation.
constexpr COLORREF kWindowBackground = RGB(246, 248, 250);
constexpr COLORREF kSurface = RGB(255, 255, 255);
constexpr COLORREF kMutedText = RGB(101, 109, 118);
constexpr COLORREF kAccent = RGB(36, 112, 190);
constexpr COLORREF kBorder = RGB(214, 221, 229);
constexpr COLORREF kSuccess = RGB(31, 142, 88);
constexpr COLORREF kWarning = RGB(190, 121, 26);

// Reuse one brush for transparent/static controls.  Returning a brush that
// matches the parent canvas prevents the small native labels from showing a
// different grey rectangle when Windows themes are enabled.
HBRUSH WindowBackgroundBrush() {
    static HBRUSH brush = CreateSolidBrush(kWindowBackground);
    return brush;
}

HBRUSH SurfaceBrush() {
    static HBRUSH brush = CreateSolidBrush(kSurface);
    return brush;
}

std::wstring JsonEscape(const std::wstring_view value) {
    std::wstring escaped;
    escaped.reserve(value.size() + 8);
    for (const wchar_t character : value) {
        switch (character) {
        case L'\\': escaped += L"\\\\"; break;
        case L'\"': escaped += L"\\\""; break;
        case L'\r': escaped += L"\\r"; break;
        case L'\n': escaped += L"\\n"; break;
        case L'\t': escaped += L"\\t"; break;
        default: escaped += character; break;
        }
    }
    return escaped;
}

enum class ReferenceFrameSelection : int {
    None = 0,
    Automatic = 1,
    Igs14 = 2,
    Igb14 = 3,
    Igs20 = 4,
    Igb20 = 5,
    Igc20 = 6
};

// Observation download presets combine the individual station/file filters
// into a task-oriented configuration.  Keep this separate from the
// reference-frame selector because a GIM query does not apply a frame roster.
enum class ObservationPresetSelection : int {
    None = 0,
    ReferenceFrameAutomatic = 1,
    GimTec = 2,
    GimEvaluation = 3,
    BeiDouPod = 4,
    NetworkRtk = 5
};

struct ReferenceFrameSpec {
    ReferenceFrameSelection id;
    const wchar_t* label;
    const wchar_t* start;
    const wchar_t* end;
    const wchar_t* rosterFile;
    bool fullApr;
};

constexpr std::array<ReferenceFrameSpec, 5> kReferenceFrames{{
    {ReferenceFrameSelection::Igs14, L"IGS14", L"2017-01-29", L"2020-05-16",
     L"IGS14_core.txt", false},
    {ReferenceFrameSelection::Igb14, L"IGb14", L"2020-05-17", L"2022-11-26",
     L"IGb14_core.txt", false},
    {ReferenceFrameSelection::Igs20, L"IGS20", L"2022-11-27", L"2025-02-01",
     L"IGS20_core.txt", false},
    {ReferenceFrameSelection::Igb20, L"IGb20", L"2025-02-02", L"2026-01-10",
     L"IGb20_core.txt", false},
    {ReferenceFrameSelection::Igc20, L"IGc20", L"2026-01-11", L"9999-12-31",
     L"IGc20_core.txt", false}
}};

const ReferenceFrameSpec* FindReferenceFrame(
        const ReferenceFrameSelection id) {
    for (const auto& spec : kReferenceFrames) {
        if (spec.id == id) return &spec;
    }
    return nullptr;
}

const ReferenceFrameSpec* ReferenceFrameForDate(const QueryDate& date) {
    const auto value = FormatDate(date);
    for (const auto& spec : kReferenceFrames) {
        if (value >= spec.start && value <= spec.end) return &spec;
    }
    return nullptr;
}

const wchar_t* ReferenceFrameSelectionValue(
        const ReferenceFrameSelection selection) {
    switch (selection) {
    case ReferenceFrameSelection::Automatic: return L"automatic";
    case ReferenceFrameSelection::Igs14: return L"igs14";
    case ReferenceFrameSelection::Igb14: return L"igb14";
    case ReferenceFrameSelection::Igs20: return L"igs20";
    case ReferenceFrameSelection::Igb20: return L"igb20";
    case ReferenceFrameSelection::Igc20: return L"igc20";
    case ReferenceFrameSelection::None:
    default: return L"ignore";
    }
}

ReferenceFrameSelection ParseReferenceFrameSelection(
        const std::wstring& value) {
    if (value == L"automatic") return ReferenceFrameSelection::Automatic;
    if (value == L"igs14") return ReferenceFrameSelection::Igs14;
    if (value == L"igb14") return ReferenceFrameSelection::Igb14;
    if (value == L"igs20") return ReferenceFrameSelection::Igs20;
    if (value == L"igb20") return ReferenceFrameSelection::Igb20;
    if (value == L"igc20") return ReferenceFrameSelection::Igc20;
    return ReferenceFrameSelection::None;
}

std::vector<std::filesystem::path> ReferenceFrameDataCandidates(
        const std::wstring_view name) {
    const auto cwd = std::filesystem::current_path();
    return {
        cwd / L"resources" / L"reference_frames" / name,
        cwd / name,
        cwd.parent_path() / L"resources" / L"reference_frames" / name,
        cwd.parent_path() / name,
        cwd.parent_path().parent_path().parent_path()
            / L"resources" / L"reference_frames" / name,
        cwd.parent_path().parent_path().parent_path().parent_path()
            / L"resources" / L"reference_frames" / name};
}

std::unordered_set<std::string> LoadReferenceFrameCodes(
        const ReferenceFrameSpec& spec) {
    std::unordered_set<std::string> result;
    for (const auto& path : ReferenceFrameDataCandidates(spec.rosterFile)) {
        std::ifstream input(path);
        if (!input) continue;
        std::string line;
        while (std::getline(input, line)) {
            std::istringstream fields(line);
            std::string token;
            if (!(fields >> token) || token.empty() || token[0] == '#') continue;
            if (spec.fullApr) {
                const auto underscore = token.find('_');
                if (underscore == std::string::npos || underscore < 4) continue;
                token.resize(4);
            } else {
                if (token.size() != 4) continue;
            }
            std::transform(token.begin(), token.end(), token.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            result.insert(std::move(token));
            if (!spec.fullApr) {
                while (fields >> token) {
                    if (token.size() != 4 || token[0] == '#') continue;
                    std::transform(token.begin(), token.end(), token.begin(),
                        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                    result.insert(std::move(token));
                }
            }
        }
        if (!result.empty()) break;
    }
    return result;
}

const std::unordered_set<std::string>& ReferenceFrameStationCodes(
        const ReferenceFrameSelection id) {
    static std::array<std::unordered_set<std::string>, kReferenceFrames.size()> cache;
    static std::array<bool, kReferenceFrames.size()> loaded{};
    for (std::size_t index = 0; index < kReferenceFrames.size(); ++index) {
        if (kReferenceFrames[index].id != id) continue;
        if (!loaded[index]) {
            cache[index] = LoadReferenceFrameCodes(kReferenceFrames[index]);
            loaded[index] = true;
        }
        return cache[index];
    }
    static const std::unordered_set<std::string> empty;
    return empty;
}

QueryDate ParseReferenceFrameDate(const wchar_t* value) {
    const std::wstring text(value == nullptr ? L"" : value);
    if (text.size() < 10) return {};
    try {
        return MakeQueryDate(
            std::stoi(text.substr(0, 4)),
            std::stoi(text.substr(5, 2)),
            std::stoi(text.substr(8, 2)));
    } catch (...) {
        return {};
    }
}

std::vector<ObservationStationDateWindow> BuildReferenceFrameWindows(
        const ReferenceFrameSelection selection,
        const QueryDate& start,
        const QueryDate& end) {
    std::vector<ObservationStationDateWindow> result;
    if (selection == ReferenceFrameSelection::None) return result;
    const auto startText = FormatDate(start);
    const auto endText = FormatDate(end);
    const auto append = [&](const ReferenceFrameSpec& spec) {
        if (endText < spec.start || startText > spec.end) return;
        const auto& codes = ReferenceFrameStationCodes(spec.id);
        ObservationStationDateWindow window;
        window.start = ParseReferenceFrameDate(spec.start);
        window.end = ParseReferenceFrameDate(spec.end);
        window.stationCodes.assign(codes.begin(), codes.end());
        result.push_back(std::move(window));
    };
    if (selection == ReferenceFrameSelection::Automatic) {
        for (const auto& spec : kReferenceFrames) append(spec);
    } else if (const auto* spec = FindReferenceFrame(selection)) {
        append(*spec);
    }
    return result;
}

void FillSolidRect(
    const HDC deviceContext,
    const RECT& rectangle,
    const COLORREF colour) {
    const HBRUSH brush = CreateSolidBrush(colour);
    if (brush == nullptr) {
        return;
    }
    FillRect(deviceContext, &rectangle, brush);
    DeleteObject(brush);
}

void StrokeRect(
    const HDC deviceContext,
    const RECT& rectangle,
    const COLORREF colour) {
    const HPEN pen = CreatePen(PS_SOLID, 1, colour);
    if (pen == nullptr) {
        return;
    }
    const HGDIOBJ oldPen = SelectObject(deviceContext, pen);
    const HGDIOBJ oldBrush = SelectObject(
        deviceContext,
        GetStockObject(NULL_BRUSH));
    Rectangle(
        deviceContext,
        rectangle.left,
        rectangle.top,
        rectangle.right,
        rectangle.bottom);
    SelectObject(deviceContext, oldBrush);
    SelectObject(deviceContext, oldPen);
    DeleteObject(pen);
}

void DrawSectionFrame(
    const HDC deviceContext,
    const RECT& rectangle,
    const std::wstring_view title,
    const HFONT font) {
    // Paint a quiet card instead of a hard rectangular frame.  The controls
    // remain native child windows, while the parent supplies a consistent
    // surface, border and corner radius behind each logical section.
    const HBRUSH brush = CreateSolidBrush(kSurface);
    const HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
    if (brush != nullptr && pen != nullptr) {
        const HGDIOBJ oldBrush = SelectObject(deviceContext, brush);
        const HGDIOBJ oldPen = SelectObject(deviceContext, pen);
        RoundRect(
            deviceContext,
            rectangle.left,
            rectangle.top,
            rectangle.right,
            rectangle.bottom,
            8,
            8);
        SelectObject(deviceContext, oldPen);
        SelectObject(deviceContext, oldBrush);
    }
    if (pen != nullptr) DeleteObject(pen);
    if (brush != nullptr) DeleteObject(brush);

    // A short accent rule gives each card a consistent visual anchor.  It is
    // deliberately painted inside the frame so it remains visible on resize
    // while the native child controls retain their normal hit-testing and
    // theme rendering.
    FillSolidRect(
        deviceContext,
        RECT{rectangle.left + 12, rectangle.top + 1,
             std::min(rectangle.left + 48, rectangle.right - 12),
             rectangle.top + 4},
        kAccent);
    if (title.empty()) return;

    // Keep the caption inside a dedicated band at the top of the card.  The
    // previous group-box-like placement straddled the first child control,
    // which made native EDIT/COMBO text overwrite the caption on some font
    // metrics.  Layout reserves this band before placing controls.
    RECT caption{
        rectangle.left + 10,
        rectangle.top + 5,
        rectangle.left + 10 + 180,
        rectangle.top + 25};
    FillSolidRect(deviceContext, caption, kSurface);
    const int oldMode = SetBkMode(deviceContext, TRANSPARENT);
    const COLORREF oldColour = SetTextColor(deviceContext, kMutedText);
    const HGDIOBJ oldFont = font == nullptr
        ? nullptr : SelectObject(deviceContext, font);
    DrawTextW(
        deviceContext,
        title.data(),
        static_cast<int>(title.size()),
        &caption,
        DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX);
    if (oldFont != nullptr) SelectObject(deviceContext, oldFont);
    SetTextColor(deviceContext, oldColour);
    SetBkMode(deviceContext, oldMode);
}

constexpr UINT WM_IGS_QUERY_FINISHED = WM_APP + 1;
constexpr UINT WM_IGS_DOWNLOAD_PROGRESS = WM_APP + 2;
constexpr UINT WM_IGS_DOWNLOAD_FINISHED = WM_APP + 3;
constexpr UINT WM_IGS_SITELOG_FINISHED = WM_APP + 4;
constexpr UINT WM_IGS_KB_STATUS_FINISHED = WM_APP + 5;
constexpr UINT WM_IGS_SHOW_WEB_LOADING = WM_APP + 6;
constexpr UINT WM_IGS_WEB_DETAIL_READY = WM_APP + 7;
constexpr UINT WM_IGS_SELECTION_READY = WM_APP + 8;
constexpr UINT WM_IGS_POSTPROCESS_READY = WM_APP + 9;
constexpr UINT WM_IGS_POSTPROCESS_FINISHED = WM_APP + 10;
constexpr UINT_PTR kObservationFilterTimer = 0x6A01;
constexpr UINT_PTR kWebUiLoadingTimer = 0x6A02;

struct SiteLogEnrichmentResult {
    std::uint64_t generation = 0;
    std::vector<StationFile> stations;
    // For a date-range/GPS-week query this carries the per-day records after
    // Site Log equipment history has been applied.  Keeping the vector on
    // the worker result prevents the UI thread from copying a potentially
    // very large period while metadata is being resolved.
    std::vector<DatedStationFile> periodFiles;
    bool temporal = false;
    int filteredPeriodFiles = 0;
    int matchedCount = 0;
    std::wstring error;
};

struct KnowledgeBaseStatusResult {
    std::uint64_t generation = 0;
    ObservationNetworkMask networks = 0;
};

ObservationNetworkMask ScanKnowledgeBaseNetworks(
    const std::vector<ObservationNetwork>& networks,
    const std::stop_token stopToken) {
    ObservationNetworkMask ready = 0;
    std::error_code filesystemError;
    const auto igsPath = GetIgsAvailabilityDatabasePath();
    if (!stopToken.stop_requested()
        && std::filesystem::is_regular_file(igsPath, filesystemError)) {
        const auto stats = GetIgsAvailabilityStats();
        if (stats.error.empty()
            && stats.completeDays + stats.provisionalDays > 0) {
            ready |= NetworkBit(ObservationNetwork::Igs);
            ready |= NetworkBit(ObservationNetwork::Mgex);
        }
    }

    for (const auto network : networks) {
        if (stopToken.stop_requested()) {
            break;
        }
        if (network == ObservationNetwork::Igs
            || network == ObservationNetwork::Mgex
            || !HasSingleSourceKnowledgeBase(network)) {
            continue;
        }
        filesystemError.clear();
        const auto path = GetNetworkAvailabilityDatabasePath(network);
        if (!path.empty()
            && std::filesystem::is_regular_file(path, filesystemError)) {
            const auto stats = GetNetworkAvailabilityStats(network);
            if (stats.error.empty()
                && stats.completeDays + stats.provisionalDays > 0) {
                ready |= NetworkBit(network);
            }
        }
    }
    return ready;
}

enum ControlId : int {
    IdDatePicker = 1001,
    IdDateRangeEndPicker,
    IdNetworkPanelToggle,
    IdIgsCheck,
    IdMgexCheck,
    IdNgsCheck,
    IdEpnCheck,
    IdHkCheck,
    IdGaGdcCheck,
    IdRgnaCheck,
    IdSonelCheck,
    IdTrigNetCheck,
    IdCutCheck,
    IdCsnCheck,
    IdEposIcelandCheck,
    IdRbmcCheck,
    IdRamsacCheck,
    IdRegnaRouCheck,
    IdCacsCheck,
    IdRgpCheck,
    IdSopacGarnerCheck,
    IdMagnaEcoCheck,
    IdPanamaCorsCheck,
    IdHondurasCorsCheck,
    IdEposGnssCheck,
    IdGrefCheck,
    IdOsNetCheck,
    IdErgnssCheck,
    IdRenepCheck,
    IdGeoNetNzCheck,
    IdRingCheck,
    IdAposCheck,
    IdRobBelgiumCheck,
    IdDpgaCheck,
    IdCatNetCheck,
    IdSpsLuxCheck,
    IdKasiNgiiCheck,
    IdRenagCheck,
    IdNsgiCheck,
    IdNoaGnssCheck,
    IdRedgaeCheck,
    IdGeoNetJapanCheck,
    IdDlabCheck,
    IdMagnetCheck,
    IdPangaCwuCheck,
    IdSelectKnowledgeBaseNetworks,
    IdQueryButton,
    IdAdvancedFilterButton,
    IdDateAdvancedFilterButton,
    IdQueryStatus,
    IdFilterEdit,
    IdScopeCountry,
    IdScopeLatitudeMinimum,
    IdScopeLatitudeMaximum,
    IdScopeLongitudeMinimum,
    IdScopeLongitudeMaximum,
    IdApplyScope,
    IdMapRectangleScope,
    IdClearScope,
    IdStationList,
    IdSelectAll,
    IdClearSelection,
    IdUniformCount,
    IdMinimumDistance,
    IdMaximumNeighborDistance,
    IdUniformSelect,
    IdRandomSelect,
    IdSelectionSummary,
    IdFolderEdit,
    IdBrowseButton,
    IdDownloadButton,
    IdPauseDownloadButton,
    IdProgress,
    IdOperationStatus,
    IdMap,
    IdObservationMode,
    IdPreciseMode,
    IdPreciseView,
    IdQuickScheme,
    IdQuickSchemeApply
};

// Main-window buttons use one owner-draw renderer so the application keeps a
// consistent visual language even when the host Windows theme changes.  The
// controls remain ordinary Win32 buttons (and therefore keep keyboard
// navigation, accessibility and command routing), but their paint is
// deliberately quiet: a single blue action colour, white secondary cards and
// a dark navigation rail.
void DrawModernButton(const DRAWITEMSTRUCT& draw) {
    if (draw.hDC == nullptr || draw.hwndItem == nullptr) return;
    RECT bounds = draw.rcItem;
    InflateRect(&bounds, -1, -1);
    const int id = GetDlgCtrlID(draw.hwndItem);
    const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool navigation = id == IdObservationMode || id == IdPreciseMode;
    const bool checked = navigation
        ? GetWindowLongPtrW(draw.hwndItem, GWLP_USERDATA) != 0
        : SendMessageW(draw.hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool primary = id == IdQueryButton || id == IdDownloadButton
        || id == IdUniformSelect || id == IdRandomSelect;

    COLORREF fill = kSurface;
    COLORREF border = kBorder;
    COLORREF text = RGB(38, 50, 64);
    if (navigation) {
        // The mode switch now lives in the header.  Treat it like a compact
        // segmented control instead of a full-height navigation rail.
        fill = checked ? kAccent : RGB(237, 242, 248);
        border = checked ? kAccent : kBorder;
        text = checked ? RGB(255, 255, 255) : RGB(38, 50, 64);
    } else if (primary) {
        fill = kAccent;
        border = kAccent;
        text = RGB(255, 255, 255);
    }
    if (disabled) {
        fill = RGB(235, 239, 244);
        border = RGB(218, 224, 231);
        text = RGB(156, 166, 177);
    } else if (pressed) {
        if (navigation || primary) {
            fill = RGB(27, 88, 154);
        } else {
            fill = RGB(229, 236, 244);
        }
    }

    const HBRUSH brush = CreateSolidBrush(fill);
    const HPEN pen = CreatePen(PS_SOLID, 1, border);
    if (brush != nullptr && pen != nullptr) {
        const HGDIOBJ oldBrush = SelectObject(draw.hDC, brush);
        const HGDIOBJ oldPen = SelectObject(draw.hDC, pen);
        RoundRect(draw.hDC, bounds.left, bounds.top, bounds.right,
            bounds.bottom, navigation ? 8 : 6, navigation ? 8 : 6);
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

struct MainLayoutMetrics {
    // The two product modes are a compact header switch, not a permanent
    // navigation rail. Keeping the content origin explicit makes it harder
    // for future controls to accidentally reserve sidebar space again.
    int sidebarWidth = 0;
    int contentLeft = 24;
    int contentRight = 0;
    int contentWidth = 0;
    int panelWidth = 0;
    int panelRight = 0;
    int mapLeft = 0;
    int mapWidth = 0;
    // Cards reserve a small title band above their native child controls.
    // Keeping the band in the metrics (rather than hiding text behind a
    // border) makes the layout deterministic at normal Windows font sizes.
    int contentTop = 120;
    int networkOverlayWidth = 0;
    int networkOverlayBottom = 0;
    int quickTop = 0;
    int dateTop = 0;
    int advancedTop = 0;
    int queryTop = 0;
    int queryStatusTop = 0;
    int queryStatusHeight = 64;
    int filterLabelTop = 0;
    int filterEditTop = 0;
    int listTop = 0;
    int listBottom = 0;
    int actionTop = 0;
    int mapTop = 0;
};

// Keep the compressed observation workspace active until the normal footer
// has enough vertical budget for its progress and status rows.  Without this
// buffer, resizing through the 700–800 px client-height range briefly puts
// the list back into the full layout while the download footer is still
// below the fold.
constexpr int kMainCompactHeight = 800;

MainLayoutMetrics ComputeMainLayoutMetrics(
    const int width,
    const int height,
    const bool networksExpanded) {
    MainLayoutMetrics metrics;
    const bool compact = height < kMainCompactHeight;
    metrics.sidebarWidth = 0;
    metrics.contentLeft = 24;
    const int availableContent = std::max(
        280, width - 48);
    metrics.contentWidth = availableContent;
    metrics.contentRight = metrics.contentLeft + metrics.contentWidth;
    const int preferredPanel = metrics.contentWidth * 46 / 100;
    const int maximumPanel = std::max(200, metrics.contentWidth - 180);
    const int panelLowerBound = std::min(460, maximumPanel);
    const int panelUpperBound = std::min(620, maximumPanel);
    metrics.panelWidth = std::clamp(
        preferredPanel, panelLowerBound, panelUpperBound);
    metrics.panelRight = metrics.contentLeft + metrics.panelWidth;
    metrics.mapLeft = metrics.panelRight + 20;
    metrics.mapWidth = std::max(180,
        metrics.contentRight - metrics.mapLeft);
    // Keep the expanded chooser inside the left work card.  The map is a
    // separate child window and would otherwise paint over the parent's
    // floating-card background when the two rectangles overlap.
    metrics.networkOverlayWidth = metrics.panelWidth;
    // The compact path keeps a little more room for the source controls so
    // their native buttons stay below the card caption band.  The expanded
    // chooser is still allowed to float over the form as a deliberate modal
    // surface; its own controls are laid out in the same coordinate system.
    metrics.contentTop = compact ? 112 : 120;
    metrics.networkOverlayBottom = metrics.contentTop
        + (networksExpanded ? 358 : compact ? 62 : 58);

    // Each group starts with a 24 px caption band, followed by its controls.
    // The resulting rhythm prevents native EDIT/COMBO windows from covering
    // the canvas-drawn section captions on high-DPI and themed systems.
    metrics.quickTop = metrics.contentTop + 72;
    metrics.dateTop = metrics.quickTop + (compact ? 62 : 66);
    metrics.advancedTop = metrics.dateTop + (compact ? 36 : 38);
    metrics.queryTop = metrics.advancedTop + 38;
    metrics.queryStatusTop = metrics.queryTop;
    metrics.queryStatusHeight = compact ? 34 : 64;
    metrics.filterLabelTop = metrics.queryStatusTop
        + metrics.queryStatusHeight + (compact ? 14 : 48);
    metrics.filterEditTop = metrics.filterLabelTop + 22;
    metrics.listTop = metrics.filterEditTop + (compact ? 32 : 35);
    const int bottomReserve = compact ? 166 : 218;
    const int minimumListHeight = compact ? 58 : 96;
    metrics.listBottom = std::max(
        metrics.listTop + minimumListHeight,
        height - bottomReserve);
    metrics.actionTop = metrics.listBottom + (compact ? 26 : 38);
    // On compact laptop widths the map toolbar gets two extra rows instead
    // of forcing three 40-pixel buttons into a single clipped line.
    metrics.mapTop = metrics.contentTop
        + (metrics.mapWidth < 360 ? 286 : 206);
    return metrics;
}

// The advanced observation filters are deliberately kept as a small value
// object in the window.  The same snapshot is passed to the range query
// worker and is also applied to the virtual list after Site Log enrichment.
// Keeping the values in UTF-8 makes the conversion to ObservationRangeOptions
// lossless and avoids storing HWNDs in the query thread.
struct AdvancedFilterState {
    enum class CompletenessMode {
        Ignore,
        RatioAtLeast,
        DaysAtLeast
    };

    CompletenessMode completenessMode = CompletenessMode::Ignore;
    double minimumCompletenessPercent = 90.0;
    int minimumAvailableDays = 0;
    DailyCoverageMode dailyCoverageMode = DailyCoverageMode::Ignore;
    int minimumDailyStations = 0;
    double minimumDailyStationPercent = 0.0;
    std::string sampleInterval = "30S";
    int rinexVersion = 0; // 0 = no restriction, 2 = RINEX 2, 3 = RINEX 3/4
    std::string systemsInclude;
    std::string systemsExclude;
    std::string receiverInclude;
    std::string receiverExclude;
    std::string antennaInclude;
    std::string antennaExclude;
    std::string signalCodesInclude;
    std::string signalCodesExclude;
    std::string gimStationMode = "ignore";
    std::string gimProviders;
    std::string gimProductGrades;
    bool observationCompletenessEnabled = false;
    double minimumObservationCompletenessPercent = 90.0;
    std::string observationCompletenessSystem;
    bool latencyEnabled = false;
    double maximumMeanLatencyHours = 24.0;
    bool positionTimeSeriesEnabled = false;
    double maximumMeanAbsoluteDetrendedPositionMillimeters = 10.0;
    bool residualsEnabled = false;
    double maximumMeanAbsoluteResidualMillimeters = 10.0;
    bool meanMultipathEnabled = false;
    double maximumMeanMultipathCentimeters = 1.0;
    std::string multipathSystem;
    bool cycleSlipsEnabled = false;
    double maximumMeanDailyCycleSlips = 1000.0;
    std::string cycleSlipsSystem;
};

// Date-level space-weather predicates are kept separate from station/site
// log filters.  They are evaluated against every day in the selected date
// range before station completeness is calculated.
struct DateAdvancedFilterState {
    SpaceWeatherFilter kpMaxFilter{};
    SpaceWeatherFilter dstMinFilter{};
    SpaceWeatherFilter f107Filter{};
    bool useAdjustedF107 = true;
};

enum AdvancedControlId : int {
    IdAdvancedCompletenessMode = 5101,
    IdAdvancedCompletenessValue,
    IdAdvancedDaysValue,
    IdAdvancedDailyCoverageMode,
    IdAdvancedDailyStations,
    IdAdvancedDailyPercent,
    IdAdvancedSampleInterval,
    IdAdvancedRinexVersion,
    IdAdvancedSystemsInclude,
    IdAdvancedSystemsExclude,
    IdAdvancedReceiverInclude,
    IdAdvancedReceiverExclude,
    IdAdvancedAntennaInclude,
    IdAdvancedAntennaExclude,
    IdAdvancedSignalCodesInclude,
    IdAdvancedSignalCodesExclude,
    IdAdvancedOk = IDOK,
    IdAdvancedCancel = IDCANCEL
};

enum DateAdvancedControlId : int {
    IdDateAdvancedKpMode = 5301,
    IdDateAdvancedKpValue,
    IdDateAdvancedDstMode,
    IdDateAdvancedDstValue,
    IdDateAdvancedF107Mode,
    IdDateAdvancedF107Value,
    IdDateAdvancedOk = IDOK,
    IdDateAdvancedCancel = IDCANCEL
};

// The three Site Log criteria are multi-valued.  Keep the actual matching
// values in UTF-8 (the same representation used by AdvancedFilterState), but
// expose them through a checklist instead of asking users to type opaque
// receiver/antenna/system strings.  The values are intentionally separate
// from the labels so a friendly label can be shown without changing the
// existing range-query API.
struct AdvancedChoiceOption {
    std::wstring label;
    std::string value;
};

enum AdvancedChoiceControlId : int {
    IdChoiceList = 5201,
    IdChoiceSelectAll,
    IdChoiceClear,
    IdChoiceOk,
    IdChoiceCancel
};

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (length <= 0) {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        length);
    return result;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (length <= 0) {
        std::string fallback;
        fallback.reserve(text.size());
        for (const wchar_t character : text) {
            fallback.push_back(character >= 0 && character <= 0x7F
                ? static_cast<char>(character)
                : '?');
        }
        return fallback;
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        length,
        nullptr,
        nullptr);
    return result;
}

std::wstring ToUpperWide(std::wstring value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const wchar_t character) {
            return static_cast<wchar_t>(std::towupper(character));
        });
    return value;
}

// The project intentionally has no .rc dialog resources.  A small native
// popup is therefore used for the advanced filters.  It keeps the main
// window's original compact layout while still exposing every optional
// criterion without resorting to a sequence of message boxes.
struct AdvancedDialogContext {
    AdvancedFilterState* target = nullptr;
    HFONT font = nullptr;
    HWND completenessMode = nullptr;
    HWND completenessPercent = nullptr;
    HWND minimumDays = nullptr;
    HWND dailyCoverageMode = nullptr;
    HWND minimumDailyStations = nullptr;
    HWND minimumDailyPercent = nullptr;
    HWND sampleInterval = nullptr;
    HWND rinexVersion = nullptr;
    HWND systemsInclude = nullptr;
    HWND systemsExclude = nullptr;
    HWND receiverInclude = nullptr;
    HWND receiverExclude = nullptr;
    HWND antennaInclude = nullptr;
    HWND antennaExclude = nullptr;
    HWND signalCodesInclude = nullptr;
    HWND signalCodesExclude = nullptr;
    std::vector<AdvancedChoiceOption> systemOptions;
    std::vector<AdvancedChoiceOption> receiverOptions;
    std::vector<AdvancedChoiceOption> antennaOptions;
    std::vector<AdvancedChoiceOption> signalOptions;
    // Working copies are only committed to target when the user clicks
    // Apply.  This keeps Cancel truly non-mutating.
    std::string systemsIncludeValue;
    std::string systemsExcludeValue;
    std::string receiverIncludeValue;
    std::string receiverExcludeValue;
    std::string antennaIncludeValue;
    std::string antennaExcludeValue;
    std::string signalCodesIncludeValue;
    std::string signalCodesExcludeValue;
    bool accepted = false;
};

struct DateAdvancedDialogContext {
    DateAdvancedFilterState* target = nullptr;
    HFONT font = nullptr;
    HWND kpMode = nullptr;
    HWND kpValue = nullptr;
    HWND dstMode = nullptr;
    HWND dstValue = nullptr;
    HWND f107Mode = nullptr;
    HWND f107Value = nullptr;
    bool accepted = false;
};

struct AdvancedChoiceDialogContext {
    HFONT font = nullptr;
    HWND list = nullptr;
    std::vector<AdvancedChoiceOption> options;
    std::string initialSelection;
    std::string resultSelection;
    bool accepted = false;
};

// Forward declarations: the shared child-control helpers are defined just
// below the dialog utility functions, while the checklist dialog procedure
// needs to call them during WM_CREATE.
HWND CreateAdvancedChild(
    HWND dialog,
    const wchar_t* className,
    const wchar_t* text,
    DWORD style,
    int id,
    int x,
    int y,
    int width,
    int height,
    HINSTANCE instance,
    HFONT font);
void AddAdvancedLabel(
    HWND dialog,
    const wchar_t* text,
    int x,
    int y,
    int width,
    int height,
    HINSTANCE instance,
    HFONT font);

constexpr wchar_t kAdvancedDialogClassName[] =
    L"IGSDownloaderAdvancedFilterDialog";
constexpr wchar_t kDateAdvancedDialogClassName[] =
    L"IGSDownloaderDateAdvancedFilterDialog";
constexpr wchar_t kAdvancedChoiceDialogClassName[] =
    L"IGSDownloaderAdvancedChoiceDialog";

std::wstring DialogControlText(const HWND control) {
    if (control == nullptr) return {};
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(std::max(0, length)) + 1U, L'\0');
    if (length > 0) GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(std::max(0, length)));
    return value;
}

void SetDialogControlText(const HWND control, const std::wstring& value) {
    if (control != nullptr) SetWindowTextW(control, value.c_str());
}

HWND CreateAdvancedChild(
    const HWND dialog,
    const wchar_t* className,
    const wchar_t* text,
    const DWORD style,
    const int id,
    const int x,
    const int y,
    const int width,
    const int height,
    const HINSTANCE instance,
    const HFONT font) {
    const bool focusable = std::wcscmp(className, L"STATIC") != 0;
    const HWND control = CreateWindowExW(
        std::wcscmp(className, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0,
        className,
        text,
        WS_CHILD | WS_VISIBLE | (focusable ? WS_TABSTOP : 0) | style,
        x,
        y,
        width,
        height,
        dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        instance,
        nullptr);
    if (control != nullptr && font != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    return control;
}

void AddAdvancedLabel(
    const HWND dialog,
    const wchar_t* text,
    const int x,
    const int y,
    const int width,
    const int height,
    const HINSTANCE instance,
    const HFONT font) {
    (void)CreateAdvancedChild(
        dialog,
        L"STATIC",
        text,
        SS_LEFT,
        0,
        x,
        y,
        width,
        height,
        instance,
        font);
}

bool ParseDialogDouble(
    const std::wstring& text,
    const double minimum,
    const double maximum,
    double& value) {
    if (text.empty()) return false;
    errno = 0;
    wchar_t* end = nullptr;
    const double parsed = std::wcstod(text.c_str(), &end);
    if (end == text.c_str() || *end != L'\0' || errno == ERANGE
        || !std::isfinite(parsed) || parsed < minimum || parsed > maximum) {
        return false;
    }
    value = parsed;
    return true;
}

bool ParseDialogInt(
    const std::wstring& text,
    const int minimum,
    const int maximum,
    int& value) {
    if (text.empty()) return false;
    errno = 0;
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != L'\0' || errno == ERANGE
        || parsed < minimum || parsed > maximum) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

// The observation indexes use a two-character seconds field (01S/05S),
// while the filter dialog intentionally displays 1S/5S.  Keep the UI-side
// post-enrichment predicate in the same canonical form as the range engine;
// otherwise a valid 1 Hz/5 s row would disappear after Site Log processing.
std::wstring NormalizeObservationInterval(std::wstring value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](const wchar_t character) {
            return static_cast<wchar_t>(std::towupper(character));
        });
    value.erase(
        std::remove_if(
            value.begin(), value.end(),
            [](const wchar_t character) {
                return std::iswspace(character) != 0;
            }),
        value.end());
    if (value == L"ANY" || value == L"*" || value == L"ALL") return {};
    if (value.ends_with(L"SEC")) {
        value.resize(value.size() - 3U);
        value += L'S';
    } else if (value.ends_with(L"HZ")) {
        value.resize(value.size() - 2U);
        value += L'S';
    }
    if (value.ends_with(L'S') && value.size() > 1U) {
        std::wstring digits = value.substr(0, value.size() - 1U);
        if (!digits.empty()
            && std::all_of(
                digits.begin(), digits.end(),
                [](const wchar_t character) {
                    return std::iswdigit(character) != 0;
                })) {
            const auto firstSignificant = digits.find_first_not_of(L'0');
            if (firstSignificant == std::wstring::npos) {
                digits = L"0";
            } else {
                digits.erase(0, firstSignificant);
            }
            if (digits.size() == 1U) digits.insert(digits.begin(), L'0');
            value = digits + L'S';
        }
    }
    return value;
}

// Values selected in the checklist are still serialized as the same
// comma-separated UTF-8 strings consumed by the range engine.  The helpers
// below deliberately use the same separators and case-insensitive matching
// rules as the existing predicate, so changing the editor to a checklist is
// purely a presentation change.
std::vector<std::string> SplitChoiceValues(const std::string& text) {
    std::vector<std::string> values;
    std::string current;
    const auto flush = [&]() {
        auto first = current.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            current.clear();
            return;
        }
        auto last = current.find_last_not_of(" \t\r\n");
        values.push_back(current.substr(first, last - first + 1U));
        current.clear();
    };
    for (const unsigned char character : text) {
        if (character == ',' || character == ';' || character == '/'
            || character == '|' || character == '+'
            || std::isspace(character)) {
            flush();
        } else {
            current.push_back(static_cast<char>(character));
        }
    }
    flush();
    return values;
}

// Selection values emitted by the checklist are comma-separated and may
// contain spaces (receiver/antenna model names).  Keep this lossless parser
// alongside SplitChoiceValues, whose whitespace splitting is intentionally
// used only for satellite-system catalog values and legacy free text.
std::vector<std::string> SplitChoiceSelections(const std::string& text) {
    std::vector<std::string> values;
    std::string current;
    const auto flush = [&]() {
        const auto first = current.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            const auto last = current.find_last_not_of(" \t\r\n");
            values.push_back(current.substr(first, last - first + 1U));
        }
        current.clear();
    };
    for (const char character : text) {
        if (character == ',' || character == ';' || character == '|') {
            flush();
        } else {
            current.push_back(character);
        }
    }
    flush();
    return values;
}

std::string ChoiceValueKey(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    return value;
}

bool IsObviouslyInvalidEquipmentChoice(const std::string& rawValue) {
    std::string value;
    value.reserve(rawValue.size());
    for (const unsigned char character : rawValue) {
        if (!std::isspace(character)) {
            value.push_back(static_cast<char>(std::toupper(character)));
        }
    }
    if (value.empty() || value == "-" || value == "--"
        || value == "UNKNOWN" || value == "-UNKNOWN-NONE"
        || value == "NONE" || value == "NULL" || value == "N/A"
        || value == "NA") return true;
    bool numericZero = true;
    bool sawDigit = false;
    for (const unsigned char character : value) {
        if (std::isdigit(character)) {
            sawDigit = true;
            if (character != '0') numericZero = false;
        } else if (character != '.' && character != ','
                   && character != '+' && character != '-') {
            numericZero = false;
        }
    }
    return sawDigit && numericZero;
}

bool ChoiceValueIsSelected(
    const std::string& value,
    const std::string& selection) {
    const std::string key = ChoiceValueKey(value);
    // First compare lossless checklist entries.  This is important for
    // values such as "TRIMBLE NETR9" and "LEIAR GR30" which contain spaces.
    for (const auto& selected : SplitChoiceSelections(selection)) {
        if (ChoiceValueKey(selected) == key) return true;
    }
    // A pre-checklist configuration may still contain whitespace-separated
    // legacy tokens.  Retain that compatibility fallback for simple values.
    for (const auto& selected : SplitChoiceValues(selection)) {
        if (ChoiceValueKey(selected) == key) return true;
    }
    return false;
}

std::wstring ChoiceSummaryText(const std::string& selection) {
    auto values = SplitChoiceSelections(selection);
    if (values.empty()) values = SplitChoiceValues(selection);
    if (values.empty()) return L"未选择（不限制）";
    std::wstring result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) result += L"、";
        result += Utf8ToWide(values[index]);
    }
    // A long receiver/antenna model list should not make the advanced dialog
    // unreadable.  The complete selection remains available by reopening the
    // checklist; the button only shows a compact summary.
    constexpr std::size_t kSummaryLimit = 42;
    if (result.size() > kSummaryLimit) {
        result.resize(kSummaryLimit);
        result += L"…";
    }
    return result;
}

std::string SelectedChoiceValues(
    const HWND list,
    const std::vector<AdvancedChoiceOption>& options) {
    std::string result;
    if (list == nullptr) return result;
    for (std::size_t index = 0; index < options.size(); ++index) {
        if (ListView_GetCheckState(list, static_cast<int>(index)) == 0) {
            continue;
        }
        if (!result.empty()) result += ',';
        result += options[index].value;
    }
    return result;
}

void SetChoiceListSelection(
    const HWND list,
    const std::vector<AdvancedChoiceOption>& options,
    const std::string& selection) {
    if (list == nullptr) return;
    for (std::size_t index = 0; index < options.size(); ++index) {
        ListView_SetCheckState(
            list,
            static_cast<int>(index),
            ChoiceValueIsSelected(options[index].value, selection)
                ? TRUE : FALSE);
    }
}

LRESULT CALLBACK AdvancedChoiceDialogProc(
    const HWND dialog,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    auto* context = reinterpret_cast<AdvancedChoiceDialogContext*>(
        GetWindowLongPtrW(dialog, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        context = static_cast<AdvancedChoiceDialogContext*>(create->lpCreateParams);
        SetWindowLongPtrW(
            dialog,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(context));
    }
    if (context == nullptr) return DefWindowProcW(dialog, message, wParam, lParam);

    switch (message) {
    case WM_CREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        const HINSTANCE instance = static_cast<HINSTANCE>(create->hInstance);
        AddAdvancedLabel(
            dialog,
            L"勾选需要的项目（可多选）；不勾选表示不限制。",
            16,
            12,
            510,
            26,
            instance,
            context->font);
        context->list = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_LISTVIEWW,
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL
                | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL,
            16,
            44,
            510,
            276,
            dialog,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdChoiceList)),
            instance,
            nullptr);
        if (context->list != nullptr) {
            if (context->font != nullptr) {
                SendMessageW(
                    context->list,
                    WM_SETFONT,
                    reinterpret_cast<WPARAM>(context->font),
                    TRUE);
            }
            ListView_SetExtendedListViewStyle(
                context->list,
                LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT
                    | LVS_EX_DOUBLEBUFFER);
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<wchar_t*>(L"选项");
            column.cx = 480;
            ListView_InsertColumn(context->list, 0, &column);
            int itemIndex = 0;
            for (const auto& option : context->options) {
                LVITEMW item{};
                item.mask = LVIF_TEXT;
                item.iItem = itemIndex;
                item.pszText = const_cast<wchar_t*>(option.label.c_str());
                ListView_InsertItem(context->list, &item);
                ++itemIndex;
            }
            SetChoiceListSelection(
                context->list,
                context->options,
                context->initialSelection);
            if (context->options.empty()) {
                LVITEMW item{};
                item.mask = LVIF_TEXT;
                item.iItem = 0;
                item.pszText = const_cast<wchar_t*>(
                    L"（当前查询没有可用的 Site Log 选项）");
                ListView_InsertItem(context->list, &item);
                EnableWindow(context->list, FALSE);
            }
        }
        (void)CreateAdvancedChild(
            dialog,
            L"BUTTON",
            L"全选",
            BS_PUSHBUTTON,
            IdChoiceSelectAll,
            16,
            332,
            82,
            30,
            instance,
            context->font);
        (void)CreateAdvancedChild(
            dialog,
            L"BUTTON",
            L"清空",
            BS_PUSHBUTTON,
            IdChoiceClear,
            104,
            332,
            82,
            30,
            instance,
            context->font);
        (void)CreateAdvancedChild(
            dialog,
            L"BUTTON",
            L"应用",
            BS_DEFPUSHBUTTON,
            IdChoiceOk,
            344,
            332,
            88,
            30,
            instance,
            context->font);
        (void)CreateAdvancedChild(
            dialog,
            L"BUTTON",
            L"取消",
            BS_PUSHBUTTON,
            IdChoiceCancel,
            438,
            332,
            88,
            30,
            instance,
            context->font);
        SetFocus(context->list);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IdChoiceSelectAll
            && HIWORD(wParam) == BN_CLICKED) {
            if (context->list != nullptr) {
                const int count = ListView_GetItemCount(context->list);
                for (int index = 0; index < count; ++index) {
                    ListView_SetCheckState(context->list, index, TRUE);
                }
            }
            return 0;
        }
        if (LOWORD(wParam) == IdChoiceClear
            && HIWORD(wParam) == BN_CLICKED) {
            if (context->list != nullptr) {
                const int count = ListView_GetItemCount(context->list);
                for (int index = 0; index < count; ++index) {
                    ListView_SetCheckState(context->list, index, FALSE);
                }
            }
            return 0;
        }
        if (LOWORD(wParam) == IdChoiceOk
            && HIWORD(wParam) == BN_CLICKED) {
            context->resultSelection = SelectedChoiceValues(
                context->list,
                context->options);
            context->accepted = true;
            DestroyWindow(dialog);
            return 0;
        }
        if (LOWORD(wParam) == IdChoiceCancel
            && HIWORD(wParam) == BN_CLICKED) {
            DestroyWindow(dialog);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(dialog);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(dialog, message, wParam, lParam);
}

void RegisterAdvancedChoiceDialogClass(const HINSTANCE instance) {
    static std::mutex registrationMutex;
    static std::unordered_set<HINSTANCE> registered;
    std::scoped_lock lock(registrationMutex);
    if (registered.contains(instance)) return;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = AdvancedChoiceDialogProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    windowClass.lpszClassName = kAdvancedChoiceDialogClassName;
    if (RegisterClassExW(&windowClass) != 0
        || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered.insert(instance);
    }
}

bool ShowAdvancedChoiceDialog(
    const HWND owner,
    const HINSTANCE instance,
    const HFONT font,
    const wchar_t* title,
    const std::vector<AdvancedChoiceOption>& options,
    const std::string& initialSelection,
    std::string& resultSelection) {
    RegisterAdvancedChoiceDialogClass(instance);
    AdvancedChoiceDialogContext context;
    context.font = font;
    context.options = options;
    context.initialSelection = initialSelection;

    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    constexpr int dialogWidth = 560;
    constexpr int dialogHeight = 405;
    const int ownerWidth = ownerRect.right - ownerRect.left;
    const int ownerHeight = ownerRect.bottom - ownerRect.top;
    const int left = ownerRect.left + std::max(0, (ownerWidth - dialogWidth) / 2);
    const int top = ownerRect.top + std::max(0, (ownerHeight - dialogHeight) / 2);
    const HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kAdvancedChoiceDialogClassName,
        title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        left,
        top,
        dialogWidth,
        dialogHeight,
        owner,
        nullptr,
        instance,
        &context);
    if (dialog == nullptr) return false;

    EnableWindow(owner, FALSE);
    SetActiveWindow(dialog);
    MSG message{};
    while (IsWindow(dialog)) {
        const int status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) {
                PostQuitMessage(static_cast<int>(message.wParam));
            }
            break;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (IsWindow(dialog)) DestroyWindow(dialog);
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    if (!context.accepted) return false;
    resultSelection = std::move(context.resultSelection);
    return true;
}

void SetChoiceButtonText(
    const HWND button,
    const std::string& selection) {
    SetDialogControlText(button, ChoiceSummaryText(selection));
}

void AddAdvancedChoiceOption(
    std::vector<AdvancedChoiceOption>& options,
    std::unordered_set<std::string>& seen,
    const std::string& rawValue,
    const std::wstring& label = {}) {
    std::string value = rawValue;
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return;
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1U);
    if (value.empty()) return;
    const std::string key = ChoiceValueKey(value);
    if (!seen.insert(key).second) return;
    options.push_back({
        label.empty() ? Utf8ToWide(value) : label,
        value});
}

std::wstring SatelliteSystemDisplayLabel(const std::string& value) {
    if (value == "GPS") return L"GPS";
    if (value == "GLO") return L"GLONASS (GLO)";
    if (value == "GAL") return L"Galileo (GAL)";
    if (value == "BDS") return L"BeiDou (BDS)";
    if (value == "QZS") return L"QZSS (QZS)";
    if (value == "SBS") return L"SBAS (SBS)";
    if (value == "IRN") return L"NavIC/IRNSS (IRN)";
    return Utf8ToWide(value);
}

std::wstring SignalChoiceDisplayLabel(const std::string& value) {
    const auto separator = value.find(':');
    if (separator == std::string::npos) return Utf8ToWide(value);
    const std::string system = value.substr(0, separator);
    const std::string code = value.substr(separator + 1U);
    const std::string canonical = system == "G" ? "GPS"
        : system == "R" ? "GLO"
        : system == "E" ? "GAL"
        : system == "C" ? "BDS"
        : system == "J" ? "QZS"
        : system == "S" ? "SBS"
        : system == "I" ? "IRN" : system;
    return SatelliteSystemDisplayLabel(canonical) + L" · " + Utf8ToWide(code);
}

std::wstring SignalChoiceSummaryText(const std::string& selection) {
    const auto values = SplitChoiceSelections(selection);
    if (values.empty()) return L"未选择（不限制）";
    std::wstring result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) result += L"、";
        result += SignalChoiceDisplayLabel(values[index]);
    }
    constexpr std::size_t kSummaryLimit = 42;
    if (result.size() > kSummaryLimit) {
        result.resize(kSummaryLimit);
        result += L"…";
    }
    return result;
}

void SetSignalChoiceButtonText(
    const HWND button,
    const std::string& selection) {
    SetDialogControlText(button, SignalChoiceSummaryText(selection));
}

void SortAdvancedChoiceOptions(
    std::vector<AdvancedChoiceOption>& options) {
    std::sort(
        options.begin(),
        options.end(),
        [](const AdvancedChoiceOption& left, const AdvancedChoiceOption& right) {
            return ChoiceValueKey(left.value) < ChoiceValueKey(right.value);
        });
}

void CollectAdvancedChoiceOptions(
    const std::vector<StationFile>& stations,
    const std::vector<DatedStationFile>& periodFiles,
    std::vector<AdvancedChoiceOption>& systems,
    std::vector<AdvancedChoiceOption>& receivers,
    std::vector<AdvancedChoiceOption>& antennas,
    std::vector<AdvancedChoiceOption>& signalOptions) {
    std::unordered_set<std::string> systemSeen;
    std::unordered_set<std::string> receiverSeen;
    std::unordered_set<std::string> antennaSeen;

    // Always expose the standard constellation codes.  This makes the
    // control useful before the first Site Log pass has populated a query,
    // while the actual values found in the local logs are added below.
    for (const auto& [value, label] : std::array{
             std::pair{"GPS", L"GPS"},
             std::pair{"GLO", L"GLONASS (GLO)"},
             std::pair{"GAL", L"Galileo (GAL)"},
             std::pair{"BDS", L"BeiDou (BDS)"},
             std::pair{"QZS", L"QZSS (QZS)"},
             std::pair{"SBS", L"SBAS (SBS)"},
             std::pair{"IRN", L"NavIC/IRNSS (IRN)"}}) {
        AddAdvancedChoiceOption(systems, systemSeen, value, label);
    }

    const auto collect = [&](const StationFile& station) {
        if (!station.siteLogSystems.empty()) {
            for (const auto& token :
                CanonicalSatelliteSystemTokens(station.siteLogSystems)) {
                AddAdvancedChoiceOption(
                    systems,
                    systemSeen,
                    token,
                    SatelliteSystemDisplayLabel(token));
            }
        }
        if (!station.siteLogReceiver.empty()
            && !IsObviouslyInvalidEquipmentChoice(station.siteLogReceiver)) {
            AddAdvancedChoiceOption(
                receivers,
                receiverSeen,
                station.siteLogReceiver);
        }
        if (!station.siteLogAntenna.empty()
            && !IsObviouslyInvalidEquipmentChoice(station.siteLogAntenna)) {
            AddAdvancedChoiceOption(
                antennas,
                antennaSeen,
                station.siteLogAntenna);
        }
    };
    for (const auto& station : stations) collect(station);
    // Range queries retain one dated copy per station/day.  Including these
    // rows exposes historical receiver/antenna models that are not the model
    // effective on the representative day shown in the station list.
    for (const auto& dated : periodFiles) collect(dated.station);

    SortAdvancedChoiceOptions(systems);
    SortAdvancedChoiceOptions(receivers);
    SortAdvancedChoiceOptions(antennas);
    // Site Log Plus choices are appended by ShowAdvancedFilterDialog after
    // the compact index has supplied the date-scoped signal catalog.
    (void)signalOptions;
}

LRESULT CALLBACK AdvancedFilterDialogProc(
    const HWND dialog,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    auto* context = reinterpret_cast<AdvancedDialogContext*>(
        GetWindowLongPtrW(dialog, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        context = static_cast<AdvancedDialogContext*>(create->lpCreateParams);
        SetWindowLongPtrW(
            dialog,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(context));
    }
    if (context == nullptr) return DefWindowProcW(dialog, message, wParam, lParam);

    switch (message) {
    case WM_CREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        const HINSTANCE instance = static_cast<HINSTANCE>(create->hInstance);
        const auto& state = *context->target;
        AddAdvancedLabel(dialog, L"完整性", 16, 16, 72, 24, instance, context->font);
        context->completenessMode = CreateAdvancedChild(
            dialog, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
            IdAdvancedCompletenessMode, 88, 12, 130, 180, instance, context->font);
        for (const wchar_t* item : {L"不限制", L"完整性比例 ≥", L"可用天数 ≥"}) {
            SendMessageW(context->completenessMode, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(item));
        }
        const int modeIndex = state.completenessMode == AdvancedFilterState::CompletenessMode::Ignore
            ? 0 : (state.completenessMode == AdvancedFilterState::CompletenessMode::RatioAtLeast ? 1 : 2);
        SendMessageW(context->completenessMode, CB_SETCURSEL, modeIndex, 0);
        context->completenessPercent = CreateAdvancedChild(
            dialog, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
            IdAdvancedCompletenessValue, 228, 12, 70, 180, instance, context->font);
        for (const wchar_t* item : {
                 L"50", L"60", L"70", L"80", L"85", L"90",
                 L"95", L"99", L"100"}) {
            SendMessageW(
                context->completenessPercent,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(item));
        }
        const std::wstring currentPercent = std::format(
            L"{:.0f}", state.minimumCompletenessPercent);
        int percentIndex = static_cast<int>(SendMessageW(
            context->completenessPercent,
            CB_FINDSTRINGEXACT,
            static_cast<WPARAM>(-1),
            reinterpret_cast<LPARAM>(currentPercent.c_str())));
        if (percentIndex < 0) {
            percentIndex = static_cast<int>(SendMessageW(
                context->completenessPercent,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(currentPercent.c_str())));
        }
        SendMessageW(
            context->completenessPercent,
            CB_SETCURSEL,
            percentIndex,
            0);
        context->minimumDays = CreateAdvancedChild(
            dialog, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
            IdAdvancedDaysValue, 304, 12, 70, 180, instance, context->font);
        for (const wchar_t* item : {
                 L"0", L"1", L"3", L"7", L"14", L"30", L"60",
                 L"90", L"180", L"270", L"300", L"365", L"730",
                 L"1095", L"1825", L"3650"}) {
            SendMessageW(
                context->minimumDays,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(item));
        }
        const std::wstring currentDays = std::to_wstring(state.minimumAvailableDays);
        int daysIndex = static_cast<int>(SendMessageW(
            context->minimumDays,
            CB_FINDSTRINGEXACT,
            static_cast<WPARAM>(-1),
            reinterpret_cast<LPARAM>(currentDays.c_str())));
        if (daysIndex < 0) {
            daysIndex = static_cast<int>(SendMessageW(
                context->minimumDays,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(currentDays.c_str())));
        }
        SendMessageW(context->minimumDays, CB_SETCURSEL, daysIndex, 0);
        AddAdvancedLabel(dialog, L"% / 天", 380, 16, 52, 24, instance, context->font);

        AddAdvancedLabel(dialog, L"每日覆盖", 16, 54, 72, 24, instance, context->font);
        context->dailyCoverageMode = CreateAdvancedChild(
            dialog, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
            IdAdvancedDailyCoverageMode, 88, 50, 130, 180, instance, context->font);
        for (const wchar_t* item : {L"不限制", L"测站数 ≥", L"测站比例 ≥"}) {
            SendMessageW(context->dailyCoverageMode, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(item));
        }
        const int dailyModeIndex = state.dailyCoverageMode == DailyCoverageMode::StationsAtLeast
            ? 1 : (state.dailyCoverageMode == DailyCoverageMode::PercentAtLeast ? 2 : 0);
        SendMessageW(context->dailyCoverageMode, CB_SETCURSEL, dailyModeIndex, 0);
        context->minimumDailyStations = CreateAdvancedChild(
            dialog, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
            IdAdvancedDailyStations, 228, 50, 70, 180, instance, context->font);
        for (const wchar_t* item : {L"0", L"1", L"5", L"10", L"20", L"50", L"100", L"150", L"200", L"250"})
            SendMessageW(context->minimumDailyStations, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        const std::wstring currentDailyStations = std::to_wstring(state.minimumDailyStations);
        int dailyStationsIndex = static_cast<int>(SendMessageW(context->minimumDailyStations, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(currentDailyStations.c_str())));
        if (dailyStationsIndex < 0) dailyStationsIndex = static_cast<int>(SendMessageW(context->minimumDailyStations, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(currentDailyStations.c_str())));
        SendMessageW(context->minimumDailyStations, CB_SETCURSEL, dailyStationsIndex, 0);
        context->minimumDailyPercent = CreateAdvancedChild(
            dialog, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
            IdAdvancedDailyPercent, 304, 50, 70, 180, instance, context->font);
        for (const wchar_t* item : {L"0", L"10", L"25", L"50", L"75", L"80", L"90", L"95", L"100"})
            SendMessageW(context->minimumDailyPercent, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        const std::wstring currentDailyPercent = std::format(L"{:.0f}", state.minimumDailyStationPercent);
        int dailyPercentIndex = static_cast<int>(SendMessageW(context->minimumDailyPercent, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(currentDailyPercent.c_str())));
        if (dailyPercentIndex < 0) dailyPercentIndex = static_cast<int>(SendMessageW(context->minimumDailyPercent, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(currentDailyPercent.c_str())));
        SendMessageW(context->minimumDailyPercent, CB_SETCURSEL, dailyPercentIndex, 0);
        AddAdvancedLabel(dialog, L"数量 / %", 380, 54, 70, 24, instance, context->font);

        AddAdvancedLabel(dialog, L"采样率", 16, 92, 72, 24, instance, context->font);
        context->sampleInterval = CreateAdvancedChild(
            dialog, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
            IdAdvancedSampleInterval, 88, 88, 130, 180, instance, context->font);
        for (const wchar_t* item : {L"不限制", L"30S", L"1S", L"5S", L"15S", L"60S"}) {
            SendMessageW(context->sampleInterval, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(item));
        }
        int sampleIndex = 0;
        if (!state.sampleInterval.empty()) {
            const std::wstring sample = ToUpperWide(Utf8ToWide(state.sampleInterval));
            for (int index = 1; index <= 5; ++index) {
                wchar_t item[16]{};
                SendMessageW(context->sampleInterval, CB_GETLBTEXT, index,
                             reinterpret_cast<LPARAM>(item));
                if (sample == NormalizeObservationInterval(item)) {
                    sampleIndex = index;
                    break;
                }
            }
        }
        SendMessageW(context->sampleInterval, CB_SETCURSEL, sampleIndex, 0);
        AddAdvancedLabel(dialog, L"RINEX", 236, 92, 54, 24, instance, context->font);
        context->rinexVersion = CreateAdvancedChild(
            dialog, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
            IdAdvancedRinexVersion, 292, 88, 150, 120, instance, context->font);
        for (const wchar_t* item : {L"不限制", L"RINEX 2", L"RINEX 3/4"}) {
            SendMessageW(context->rinexVersion, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(item));
        }
        SendMessageW(context->rinexVersion, CB_SETCURSEL,
                     state.rinexVersion == 2 ? 1 : (state.rinexVersion == 3 ? 2 : 0), 0);

        // Site Log metadata is selected from checklists.  The values are
        // kept in working strings so Cancel does not alter the active query.
        context->systemsIncludeValue = state.systemsInclude;
        context->systemsExcludeValue = state.systemsExclude;
        context->receiverIncludeValue = state.receiverInclude;
        context->receiverExcludeValue = state.receiverExclude;
        context->antennaIncludeValue = state.antennaInclude;
        context->antennaExcludeValue = state.antennaExclude;
        context->signalCodesIncludeValue = state.signalCodesInclude;
        context->signalCodesExcludeValue = state.signalCodesExclude;

        AddAdvancedLabel(dialog, L"卫星系统（可多选）", 16, 174, 180, 24, instance, context->font);
        AddAdvancedLabel(dialog, L"包含", 16, 204, 48, 24, instance, context->font);
        context->systemsInclude = CreateAdvancedChild(
            dialog, L"BUTTON", L"", BS_PUSHBUTTON | BS_LEFT,
            IdAdvancedSystemsInclude, 68, 200, 374, 28, instance, context->font);
        SetChoiceButtonText(context->systemsInclude, context->systemsIncludeValue);
        AddAdvancedLabel(dialog, L"排除", 16, 238, 48, 24, instance, context->font);
        context->systemsExclude = CreateAdvancedChild(
            dialog, L"BUTTON", L"", BS_PUSHBUTTON | BS_LEFT,
            IdAdvancedSystemsExclude, 68, 234, 374, 28, instance, context->font);
        SetChoiceButtonText(context->systemsExclude, context->systemsExcludeValue);

        AddAdvancedLabel(dialog, L"接收机（可多选）", 16, 270, 180, 24, instance, context->font);
        AddAdvancedLabel(dialog, L"包含", 16, 300, 48, 24, instance, context->font);
        context->receiverInclude = CreateAdvancedChild(
            dialog, L"BUTTON", L"", BS_PUSHBUTTON | BS_LEFT,
            IdAdvancedReceiverInclude, 68, 296, 374, 28, instance, context->font);
        SetChoiceButtonText(context->receiverInclude, context->receiverIncludeValue);
        AddAdvancedLabel(dialog, L"排除", 16, 334, 48, 24, instance, context->font);
        context->receiverExclude = CreateAdvancedChild(
            dialog, L"BUTTON", L"", BS_PUSHBUTTON | BS_LEFT,
            IdAdvancedReceiverExclude, 68, 330, 374, 28, instance, context->font);
        SetChoiceButtonText(context->receiverExclude, context->receiverExcludeValue);

        AddAdvancedLabel(dialog, L"天线（可多选）", 16, 366, 180, 24, instance, context->font);
        AddAdvancedLabel(dialog, L"包含", 16, 396, 48, 24, instance, context->font);
        context->antennaInclude = CreateAdvancedChild(
            dialog, L"BUTTON", L"", BS_PUSHBUTTON | BS_LEFT,
            IdAdvancedAntennaInclude, 68, 392, 374, 28, instance, context->font);
        SetChoiceButtonText(context->antennaInclude, context->antennaIncludeValue);
        AddAdvancedLabel(dialog, L"排除", 16, 430, 48, 24, instance, context->font);
        context->antennaExclude = CreateAdvancedChild(
            dialog, L"BUTTON", L"", BS_PUSHBUTTON | BS_LEFT,
            IdAdvancedAntennaExclude, 68, 426, 374, 28, instance, context->font);
        SetChoiceButtonText(context->antennaExclude, context->antennaExcludeValue);

        AddAdvancedLabel(
            dialog,
            L"频点（卫星系统 + 观测码，可多选）",
            16,
            462,
            330,
            24,
            instance,
            context->font);
        AddAdvancedLabel(dialog, L"包含", 16, 492, 48, 24, instance, context->font);
        context->signalCodesInclude = CreateAdvancedChild(
            dialog, L"BUTTON", L"", BS_PUSHBUTTON | BS_LEFT,
            IdAdvancedSignalCodesInclude, 68, 488, 374, 28, instance, context->font);
        SetSignalChoiceButtonText(
            context->signalCodesInclude, context->signalCodesIncludeValue);
        AddAdvancedLabel(dialog, L"排除", 16, 526, 48, 24, instance, context->font);
        context->signalCodesExclude = CreateAdvancedChild(
            dialog, L"BUTTON", L"", BS_PUSHBUTTON | BS_LEFT,
            IdAdvancedSignalCodesExclude, 68, 522, 374, 28, instance, context->font);
        SetSignalChoiceButtonText(
            context->signalCodesExclude, context->signalCodesExcludeValue);

        const HWND ok = CreateAdvancedChild(
            dialog, L"BUTTON", L"应用", BS_DEFPUSHBUTTON,
            IDOK, 252, 566, 90, 30, instance, context->font);
        const HWND cancel = CreateAdvancedChild(
            dialog, L"BUTTON", L"取消", BS_PUSHBUTTON,
            IDCANCEL, 350, 566, 90, 30, instance, context->font);
        (void)ok; (void)cancel;
        EnableWindow(context->completenessPercent, modeIndex == 1);
        EnableWindow(context->minimumDays, modeIndex == 2);
        EnableWindow(context->minimumDailyStations, dailyModeIndex == 1);
        EnableWindow(context->minimumDailyPercent, dailyModeIndex == 2);
        SetFocus(context->completenessMode);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IdAdvancedCompletenessMode
            && HIWORD(wParam) == CBN_SELCHANGE) {
            const int mode = static_cast<int>(SendMessageW(
                context->completenessMode, CB_GETCURSEL, 0, 0));
            EnableWindow(context->completenessPercent, mode == 1);
            EnableWindow(context->minimumDays, mode == 2);
            return 0;
        }
        if (LOWORD(wParam) == IdAdvancedDailyCoverageMode
            && HIWORD(wParam) == CBN_SELCHANGE) {
            const int mode = static_cast<int>(SendMessageW(
                context->dailyCoverageMode, CB_GETCURSEL, 0, 0));
            EnableWindow(context->minimumDailyStations, mode == 1);
            EnableWindow(context->minimumDailyPercent, mode == 2);
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED) {
            const int controlId = LOWORD(wParam);
            const std::vector<AdvancedChoiceOption>* options = nullptr;
            std::string* value = nullptr;
            const wchar_t* title = L"选择 Site Log 条件";
            HWND summaryButton = nullptr;
            switch (controlId) {
            case IdAdvancedSystemsInclude:
                options = &context->systemOptions;
                value = &context->systemsIncludeValue;
                title = L"选择卫星系统（包含）";
                summaryButton = context->systemsInclude;
                break;
            case IdAdvancedSystemsExclude:
                options = &context->systemOptions;
                value = &context->systemsExcludeValue;
                title = L"选择卫星系统（排除）";
                summaryButton = context->systemsExclude;
                break;
            case IdAdvancedReceiverInclude:
                options = &context->receiverOptions;
                value = &context->receiverIncludeValue;
                title = L"选择接收机型号（包含）";
                summaryButton = context->receiverInclude;
                break;
            case IdAdvancedReceiverExclude:
                options = &context->receiverOptions;
                value = &context->receiverExcludeValue;
                title = L"选择接收机型号（排除）";
                summaryButton = context->receiverExclude;
                break;
            case IdAdvancedAntennaInclude:
                options = &context->antennaOptions;
                value = &context->antennaIncludeValue;
                title = L"选择天线型号（包含）";
                summaryButton = context->antennaInclude;
                break;
            case IdAdvancedAntennaExclude:
                options = &context->antennaOptions;
                value = &context->antennaExcludeValue;
                title = L"选择天线型号（排除）";
                summaryButton = context->antennaExclude;
                break;
            case IdAdvancedSignalCodesInclude:
                options = &context->signalOptions;
                value = &context->signalCodesIncludeValue;
                title = L"选择频点（包含；同一天需同时具备所选观测码）";
                summaryButton = context->signalCodesInclude;
                break;
            case IdAdvancedSignalCodesExclude:
                options = &context->signalOptions;
                value = &context->signalCodesExcludeValue;
                title = L"选择频点（排除；当天出现任一观测码即排除）";
                summaryButton = context->signalCodesExclude;
                break;
            default:
                break;
            }
            if (options != nullptr && value != nullptr) {
                std::string result;
                const bool accepted = ShowAdvancedChoiceDialog(
                        dialog,
                        reinterpret_cast<HINSTANCE>(
                            GetWindowLongPtrW(dialog, GWLP_HINSTANCE)),
                        context->font,
                        title,
                        *options,
                        *value,
                        result);
                // The checklist is a nested modal window.  Return focus to
                // the summary button that opened it, even when the user
                // cancelled, so Tab/Enter navigation remains predictable.
                SetFocus(summaryButton);
                if (accepted) {
                    *value = std::move(result);
                    if (controlId == IdAdvancedSignalCodesInclude
                        || controlId == IdAdvancedSignalCodesExclude) {
                        SetSignalChoiceButtonText(summaryButton, *value);
                    } else {
                        SetChoiceButtonText(summaryButton, *value);
                    }
                }
                return 0;
            }
        }
        if (LOWORD(wParam) == IDOK && HIWORD(wParam) == BN_CLICKED) {
            AdvancedFilterState candidate = *context->target;
            const int mode = static_cast<int>(SendMessageW(
                context->completenessMode, CB_GETCURSEL, 0, 0));
            candidate.completenessMode = mode == 1
                ? AdvancedFilterState::CompletenessMode::RatioAtLeast
                : (mode == 2
                    ? AdvancedFilterState::CompletenessMode::DaysAtLeast
                    : AdvancedFilterState::CompletenessMode::Ignore);
            if (!ParseDialogDouble(
                    DialogControlText(context->completenessPercent),
                    0.0, 100.0, candidate.minimumCompletenessPercent)) {
                NativeUiLanguage::ShowMessageBox(dialog, L"完整性比例必须是 0 到 100 之间的数字。",
                             kApplicationTitle, MB_OK | MB_ICONWARNING);
                SetFocus(context->completenessPercent);
                return 0;
            }
            if (!ParseDialogInt(
                    DialogControlText(context->minimumDays),
                    0, 100000, candidate.minimumAvailableDays)) {
                NativeUiLanguage::ShowMessageBox(dialog, L"可用天数必须是非负整数。",
                             kApplicationTitle, MB_OK | MB_ICONWARNING);
                SetFocus(context->minimumDays);
                return 0;
            }
            const int dailyMode = static_cast<int>(SendMessageW(
                context->dailyCoverageMode, CB_GETCURSEL, 0, 0));
            candidate.dailyCoverageMode = dailyMode == 1
                ? DailyCoverageMode::StationsAtLeast
                : (dailyMode == 2 ? DailyCoverageMode::PercentAtLeast
                                  : DailyCoverageMode::Ignore);
            if (!ParseDialogInt(
                    DialogControlText(context->minimumDailyStations),
                    0, 1000000, candidate.minimumDailyStations)) {
                NativeUiLanguage::ShowMessageBox(dialog, L"每日测站数必须是非负整数。",
                             kApplicationTitle, MB_OK | MB_ICONWARNING);
                SetFocus(context->minimumDailyStations);
                return 0;
            }
            if (!ParseDialogDouble(
                    DialogControlText(context->minimumDailyPercent),
                    0.0, 100.0, candidate.minimumDailyStationPercent)) {
                NativeUiLanguage::ShowMessageBox(dialog, L"每日测站比例必须是 0 到 100 之间的数字。",
                             kApplicationTitle, MB_OK | MB_ICONWARNING);
                SetFocus(context->minimumDailyPercent);
                return 0;
            }
            const int sample = static_cast<int>(SendMessageW(
                context->sampleInterval, CB_GETCURSEL, 0, 0));
            candidate.sampleInterval.clear();
            if (sample > 0) {
                wchar_t value[32]{};
                SendMessageW(context->sampleInterval, CB_GETLBTEXT, sample,
                             reinterpret_cast<LPARAM>(value));
                candidate.sampleInterval = WideToUtf8(std::wstring(value));
            }
            const int rinex = static_cast<int>(SendMessageW(
                context->rinexVersion, CB_GETCURSEL, 0, 0));
            candidate.rinexVersion = rinex == 1 ? 2 : (rinex == 2 ? 3 : 0);
            candidate.systemsInclude = context->systemsIncludeValue;
            candidate.systemsExclude = context->systemsExcludeValue;
            candidate.receiverInclude = context->receiverIncludeValue;
            candidate.receiverExclude = context->receiverExcludeValue;
            candidate.antennaInclude = context->antennaIncludeValue;
            candidate.antennaExclude = context->antennaExcludeValue;
            candidate.signalCodesInclude = context->signalCodesIncludeValue;
            candidate.signalCodesExclude = context->signalCodesExcludeValue;
            *context->target = std::move(candidate);
            context->accepted = true;
            DestroyWindow(dialog);
            return 0;
        }
        if ((LOWORD(wParam) == IDCANCEL && HIWORD(wParam) == BN_CLICKED)) {
            DestroyWindow(dialog);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(dialog);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(dialog, message, wParam, lParam);
}

LRESULT CALLBACK DateAdvancedFilterDialogProc(
    const HWND dialog,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    auto* context = reinterpret_cast<DateAdvancedDialogContext*>(
        GetWindowLongPtrW(dialog, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        context = static_cast<DateAdvancedDialogContext*>(create->lpCreateParams);
        SetWindowLongPtrW(
            dialog,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(context));
    }
    if (context == nullptr) return DefWindowProcW(dialog, message, wParam, lParam);

    switch (message) {
    case WM_CREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        const HINSTANCE instance = static_cast<HINSTANCE>(create->hInstance);
        const auto& state = *context->target;
        AddAdvancedLabel(
            dialog,
            L"只作用于当前起止日期；应用后会在查询摘要中列出实际保留日期。",
            16,
            10,
            430,
            24,
            instance,
            context->font);

        const auto setupWeather = [&](HWND& modeControl, HWND& valueControl,
                                       const int modeId, const int valueId,
                                       const int y, const SpaceWeatherFilter& filter,
                                       const std::initializer_list<const wchar_t*>& modes,
                                       const std::initializer_list<const wchar_t*>& values) {
            modeControl = CreateAdvancedChild(
                dialog, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                modeId, 100, y, 145, 180, instance, context->font);
            for (const auto* item : modes) {
                SendMessageW(modeControl, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(item));
            }
            const int selectedMode = filter.mode == SpaceWeatherFilterMode::KeepAbove
                ? 1 : (filter.mode == SpaceWeatherFilterMode::ExcludeAbove ? 2 : 0);
            SendMessageW(modeControl, CB_SETCURSEL, selectedMode, 0);

            valueControl = CreateAdvancedChild(
                dialog, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                valueId, 252, y, 78, 180, instance, context->font);
            const std::wstring current = std::format(L"{:.2f}", filter.threshold);
            std::wstring compact = current;
            while (!compact.empty() && compact.back() == L'0') compact.pop_back();
            if (!compact.empty() && compact.back() == L'.') compact.pop_back();
            for (const auto* item : values) {
                SendMessageW(valueControl, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(item));
            }
            int found = static_cast<int>(SendMessageW(
                valueControl, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                reinterpret_cast<LPARAM>(compact.c_str())));
            if (found < 0) {
                found = static_cast<int>(SendMessageW(
                    valueControl, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(compact.c_str())));
            }
            SendMessageW(valueControl, CB_SETCURSEL, found >= 0 ? found : 0, 0);
            EnableWindow(valueControl, selectedMode != 0);
        };

        AddAdvancedLabel(dialog, L"Kp日峰值", 16, 48, 78, 24, instance, context->font);
        setupWeather(
            context->kpMode,
            context->kpValue,
            IdDateAdvancedKpMode,
            IdDateAdvancedKpValue,
            44,
            state.kpMaxFilter,
            {L"不限制", L"仅保留 >", L"排除 >"},
            {L"0", L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8", L"9"});
        AddAdvancedLabel(dialog, L"级", 336, 48, 40, 24, instance, context->font);

        AddAdvancedLabel(dialog, L"Dst日最低", 16, 86, 78, 24, instance, context->font);
        setupWeather(
            context->dstMode,
            context->dstValue,
            IdDateAdvancedDstMode,
            IdDateAdvancedDstValue,
            82,
            state.dstMinFilter,
            {L"不限制", L"仅保留 ≤", L"排除 ≤"},
            {L"0", L"-10", L"-20", L"-30", L"-50", L"-75", L"-100", L"-150", L"-200", L"-300", L"-400"});
        AddAdvancedLabel(dialog, L"nT", 336, 86, 40, 24, instance, context->font);

        AddAdvancedLabel(dialog, L"F10.7日值", 16, 124, 78, 24, instance, context->font);
        setupWeather(
            context->f107Mode,
            context->f107Value,
            IdDateAdvancedF107Mode,
            IdDateAdvancedF107Value,
            120,
            state.f107Filter,
            {L"不限制", L"仅保留 >", L"排除 >"},
            {L"70", L"80", L"90", L"100", L"110", L"120", L"130", L"150", L"200", L"250"});
        AddAdvancedLabel(dialog, L"sfu", 336, 124, 48, 24, instance, context->font);

        (void)CreateAdvancedChild(
            dialog, L"BUTTON", L"应用", BS_DEFPUSHBUTTON,
            IDOK, 252, 166, 90, 30, instance, context->font);
        (void)CreateAdvancedChild(
            dialog, L"BUTTON", L"取消", BS_PUSHBUTTON,
            IDCANCEL, 350, 166, 90, 30, instance, context->font);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wParam) == CBN_SELCHANGE) {
            const int controlId = LOWORD(wParam);
            const auto update = [&](HWND modeControl, HWND valueControl) {
                const int mode = static_cast<int>(SendMessageW(
                    modeControl, CB_GETCURSEL, 0, 0));
                EnableWindow(valueControl, mode != 0);
            };
            if (controlId == IdDateAdvancedKpMode) {
                update(context->kpMode, context->kpValue);
                return 0;
            }
            if (controlId == IdDateAdvancedDstMode) {
                update(context->dstMode, context->dstValue);
                return 0;
            }
            if (controlId == IdDateAdvancedF107Mode) {
                update(context->f107Mode, context->f107Value);
                return 0;
            }
        }
        if (LOWORD(wParam) == IDOK && HIWORD(wParam) == BN_CLICKED) {
            DateAdvancedFilterState candidate = *context->target;
            const auto readWeather = [&](HWND modeControl, HWND valueControl,
                                         SpaceWeatherFilter& target,
                                         const double minimum, const double maximum,
                                         const wchar_t* label) {
                const int modeIndex = static_cast<int>(SendMessageW(
                    modeControl, CB_GETCURSEL, 0, 0));
                target.mode = modeIndex == 1
                    ? SpaceWeatherFilterMode::KeepAbove
                    : (modeIndex == 2
                        ? SpaceWeatherFilterMode::ExcludeAbove
                        : SpaceWeatherFilterMode::Ignore);
                if (target.mode == SpaceWeatherFilterMode::Ignore) {
                    target.threshold = 0.0;
                    return true;
                }
                if (!ParseDialogDouble(
                        DialogControlText(valueControl), minimum, maximum,
                        target.threshold)) {
                    NativeUiLanguage::ShowMessageBox(
                        dialog,
                        std::format(L"{}阈值必须在 {} 到 {} 之间。",
                                    label, minimum, maximum).c_str(),
                        kApplicationTitle,
                        MB_OK | MB_ICONWARNING);
                    SetFocus(valueControl);
                    return false;
                }
                return true;
            };
            if (!readWeather(context->kpMode, context->kpValue,
                             candidate.kpMaxFilter, 0.0, 9.0, L"Kp日峰值")) return 0;
            if (!readWeather(context->dstMode, context->dstValue,
                             candidate.dstMinFilter, -2000.0, 200.0, L"Dst日最低")) return 0;
            if (!readWeather(context->f107Mode, context->f107Value,
                             candidate.f107Filter, 0.0, 1000.0, L"F10.7日值")) return 0;
            *context->target = std::move(candidate);
            context->accepted = true;
            DestroyWindow(dialog);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL && HIWORD(wParam) == BN_CLICKED) {
            DestroyWindow(dialog);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(dialog);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(dialog, message, wParam, lParam);
}

void RegisterDateAdvancedDialogClass(const HINSTANCE instance) {
    static std::mutex registrationMutex;
    static std::unordered_set<HINSTANCE> registered;
    std::scoped_lock lock(registrationMutex);
    if (registered.contains(instance)) return;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DateAdvancedFilterDialogProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    windowClass.lpszClassName = kDateAdvancedDialogClassName;
    if (RegisterClassExW(&windowClass) != 0
        || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered.insert(instance);
    }
}

bool ShowDateAdvancedFilterDialog(
    const HWND owner,
    const HINSTANCE instance,
    const HFONT font,
    DateAdvancedFilterState& target) {
    RegisterDateAdvancedDialogClass(instance);
    DateAdvancedDialogContext context;
    context.target = &target;
    context.font = font;

    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    constexpr int dialogWidth = 470;
    constexpr int dialogHeight = 235;
    const int ownerWidth = ownerRect.right - ownerRect.left;
    const int ownerHeight = ownerRect.bottom - ownerRect.top;
    const int left = ownerRect.left + std::max(0, (ownerWidth - dialogWidth) / 2);
    const int top = ownerRect.top + std::max(0, (ownerHeight - dialogHeight) / 2);
    const HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kDateAdvancedDialogClassName,
        L"日期高级筛选",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        left,
        top,
        dialogWidth,
        dialogHeight,
        owner,
        nullptr,
        instance,
        &context);
    if (dialog == nullptr) return false;

    EnableWindow(owner, FALSE);
    SetActiveWindow(dialog);
    MSG message{};
    while (IsWindow(dialog)) {
        const int status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) {
                PostQuitMessage(static_cast<int>(message.wParam));
            }
            break;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (IsWindow(dialog)) DestroyWindow(dialog);
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    return context.accepted;
}

bool DateToOrdinalForSummary(const QueryDate& date, int& ordinal) {
    const auto ymd = std::chrono::year{date.year}
        / std::chrono::month{static_cast<unsigned>(date.month)}
        / std::chrono::day{static_cast<unsigned>(date.day)};
    if (!ymd.ok()) return false;
    const auto value = std::chrono::sys_days{ymd}.time_since_epoch().count();
    if (value < std::numeric_limits<int>::min()
        || value > std::numeric_limits<int>::max()) {
        return false;
    }
    ordinal = static_cast<int>(value);
    return true;
}

std::wstring FormatDateAdvancedSelection(
    const DateAdvancedFilterState& state,
    const QueryDate& startDate,
    const QueryDate& endDate) {
    ObservationRangeOptions options;
    options.kpMaxFilter = state.kpMaxFilter;
    options.dstMinFilter = state.dstMinFilter;
    options.f107Filter = state.f107Filter;
    options.useAdjustedF107 = state.useAdjustedF107;
    if (!HasSpaceWeatherFilter(options)) return {};

    const auto selection = SelectSpaceWeatherDates(
        DefaultSpaceWeatherDatabasePath(), startDate, endDate, options);
    if (!selection.error.empty()) {
        return L"日期筛选无法读取空间天气知识库：" + selection.error;
    }

    int firstOrdinal = 0;
    if (!DateToOrdinalForSummary(startDate, firstOrdinal)) {
        return L"日期筛选：起始日期无效。";
    }
    std::wstring summary = std::format(
        L"日期筛选：在 {} 至 {} 中保留 {} / {} 天",
        FormatDate(startDate),
        FormatDate(endDate),
        selection.matchedDays,
        selection.requestedDays);
    if (selection.missingDays > 0) {
        summary += std::format(
            L"（空间天气指标缺失 {} 天）", selection.missingDays);
    }
    summary += L"\r\n保留日期：";
    if (selection.ordinals.empty()) {
        summary += L"无";
        return summary;
    }

    // Consecutive days are written as an explicit date range.  This keeps a
    // multi-year result readable while still identifying every retained day
    // without silently truncating the list.
    bool firstRange = true;
    for (std::size_t index = 0; index < selection.ordinals.size();) {
        const int runStart = selection.ordinals[index];
        int runEnd = runStart;
        ++index;
        while (index < selection.ordinals.size()
               && selection.ordinals[index] == runEnd + 1) {
            runEnd = selection.ordinals[index];
            ++index;
        }
        if (!firstRange) summary += L"、";
        firstRange = false;
        const auto start = AddDays(startDate, runStart - firstOrdinal);
        const auto end = AddDays(startDate, runEnd - firstOrdinal);
        summary += FormatDate(start);
        if (runEnd != runStart) {
            summary += L" 至 " + FormatDate(end);
        }
    }
    return summary;
}

void RegisterAdvancedDialogClass(const HINSTANCE instance) {
    static std::mutex registrationMutex;
    static std::unordered_set<HINSTANCE> registered;
    std::scoped_lock lock(registrationMutex);
    if (registered.contains(instance)) return;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = AdvancedFilterDialogProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    windowClass.lpszClassName = kAdvancedDialogClassName;
    if (RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered.insert(instance);
    }
}

void ApplySiteLogRecord(StationFile& station, const SiteLogRecord& log) {
    station.hasSiteLog = true;
    station.siteLogDomes = log.domes;
    station.siteLogName = log.siteName;
    station.siteLogCity = log.city;
    station.siteLogCountry = log.country;
    station.siteLogReceiver = log.receiverType;
    station.siteLogSystems = log.receiverSystems;
    station.siteLogReceiverSerial = log.receiverSerial;
    station.siteLogFirmware = log.receiverFirmware;
    station.siteLogReceiverDateInstalled = log.receiverDateInstalled;
    station.siteLogAntenna = log.antennaType;
    station.siteLogRadome = log.antennaRadome;
    station.siteLogAntennaSerial = log.antennaSerial;
    station.siteLogAntennaReferencePoint = log.antennaReferencePoint;
    station.siteLogAntennaDateInstalled = log.antennaDateInstalled;
    station.siteLogAgency = log.agency;
    station.siteLogOperator = log.operatorName;
    station.siteLogRevisionDate = log.revisionDate;
    station.siteLogSourceDirectory = log.sourceDirectory;
    station.siteLogUrl = log.sourceUrl;

    // A Site Log is an authoritative station metadata source.  Only replace
    // the daily catalog coordinates when the log actually contains them;
    // otherwise retain the coordinates already resolved from the station
    // catalog.  This keeps old/partial logs from making a station unmappable.
    // Never let malformed/stale Site Log coordinates displace a valid daily
    // catalog position.  In particular, older logs may contain compact
    // DDDMMSS values; the parser now normalizes them, but keep this bounds
    // check at the UI boundary as a second line of defense.
    if (log.hasCoordinate
        && std::isfinite(log.latitude)
        && std::isfinite(log.longitude)
        && std::abs(log.latitude) <= 90.0
        && std::abs(log.longitude) <= 180.0
        && !(std::abs(log.latitude) < 1.0e-12
            && std::abs(log.longitude) < 1.0e-12)) {
        station.latitude = log.latitude;
        station.longitude = log.longitude;
        station.hasCoordinate = true;
    }
    if (station.country.empty() && !log.country.empty()) {
        station.country = log.country;
    }
}

void CorrectKnownStationCoordinates(StationFile& station) {
    // The archived GERD row repeats its latitude in the longitude column.
    // Geralds Yard is on Montserrat: Nevada Geodetic Laboratory lists
    // 16.795 N, 62.194 W (station page /NGLStationPages/stations/GERD.sta).
    // Correct only this identified corrupt coordinate, leaving the index and
    // all other station positions untouched.
    if ((station.fourCharId == "GERD" || station.stationId == "GERD")
        && station.hasCoordinate
        && std::abs(station.latitude - 16.794817) < 0.02
        && std::abs(station.longitude - station.latitude) < 0.001) {
        station.longitude = -62.194;
        station.country = "MSR";
    }
    // This old SOPAC row likewise repeats the latitude as a negative
    // longitude.  The PANGA record for the same marker and NGL station page
    // /NGLStationPages/stations/CAMI.sta place CAMI in Washington State.
    if ((station.fourCharId == "CAMI" || station.stationId == "CAMI")
        && station.hasCoordinate
        && std::abs(station.latitude - 48.2153) < 0.01
        && std::abs(station.longitude + station.latitude) < 0.001) {
        station.longitude = -122.4786;
        station.country = "USA";
    }

    // These two archived latitude/longitude fields contradict the same
    // record's ECEF XYZ and the independently saved observation headers.
    // Correct only the identified corrupt values for the matching network.
    if (station.stationId == "LEIP" && (station.availableNetworks & 4194304ULL)
        && station.hasCoordinate
        && std::abs(station.latitude - 52.3539722222222) < 0.0001
        && std::abs(station.longitude - 12.3740833333333) < 0.0001) {
        station.latitude = 51.353977319017;
        station.longitude = 12.374094153937;
    }
    if (station.stationId == "YELM" && (station.availableNetworks & 2199023255552ULL)
        && station.hasCoordinate
        && std::abs(station.latitude - 46.95) < 0.0001
        && std::abs(station.longitude + 123.39) < 0.0001) {
        station.latitude = 46.948713350920;
        station.longitude = -122.605716875711;
    }

    if (station.hasCoordinate) return;
    const StationCoordinateEvidence::Entry* match = nullptr;
    for (const auto& candidate : StationCoordinateEvidence::kEntries) {
        if (candidate.stationId != station.stationId
            || (candidate.networkMask & station.availableNetworks) == 0
            || (!candidate.country.empty() && !station.country.empty()
                && station.country != "UNK" && candidate.country != station.country)) {
            continue;
        }
        if (match) {
            // A combined query can join archives with the same short marker.
            // Do not assign either position when the matches are distinct.
            const auto latitudeDifference = std::abs(match->latitude - candidate.latitude);
            const auto longitudeDifference = std::abs(match->longitude - candidate.longitude);
            if (latitudeDifference > 0.05 || longitudeDifference > 0.05) return;
        } else {
            match = &candidate;
        }
    }
    if (match) {
        station.latitude = match->latitude;
        station.longitude = match->longitude;
        station.hasCoordinate = true;
        if (!match->area.empty()) station.country.assign(match->area);
    }
}

// Copy only the fields populated by Site Log enrichment.  Availability,
// selection and geographic-scope fields belong to the live UI model and may
// have changed while the background Site Log pass was running, so replacing
// the whole StationFile would unexpectedly clear a user's selections.
void CopySiteLogMetadata(
    StationFile& target,
    const StationFile& enriched) {
    if (!enriched.hasSiteLog) {
        return;
    }
    target.hasSiteLog = true;
    target.siteLogDomes = enriched.siteLogDomes;
    target.siteLogName = enriched.siteLogName;
    target.siteLogCity = enriched.siteLogCity;
    target.siteLogCountry = enriched.siteLogCountry;
    target.siteLogReceiver = enriched.siteLogReceiver;
    target.siteLogSystems = enriched.siteLogSystems;
    target.siteLogReceiverSerial = enriched.siteLogReceiverSerial;
    target.siteLogFirmware = enriched.siteLogFirmware;
    target.siteLogReceiverDateInstalled = enriched.siteLogReceiverDateInstalled;
    target.siteLogAntenna = enriched.siteLogAntenna;
    target.siteLogRadome = enriched.siteLogRadome;
    target.siteLogAntennaSerial = enriched.siteLogAntennaSerial;
    target.siteLogAntennaReferencePoint = enriched.siteLogAntennaReferencePoint;
    target.siteLogAntennaDateInstalled = enriched.siteLogAntennaDateInstalled;
    target.siteLogAgency = enriched.siteLogAgency;
    target.siteLogOperator = enriched.siteLogOperator;
    target.siteLogRevisionDate = enriched.siteLogRevisionDate;
    target.siteLogSourceDirectory = enriched.siteLogSourceDirectory;
    target.siteLogUrl = enriched.siteLogUrl;
    if (enriched.hasCoordinate
        && std::isfinite(enriched.latitude)
        && std::isfinite(enriched.longitude)
        && std::abs(enriched.latitude) <= 90.0
        && std::abs(enriched.longitude) <= 180.0
        && !(std::abs(enriched.latitude) < 1.0e-12
            && std::abs(enriched.longitude) < 1.0e-12)) {
        target.latitude = enriched.latitude;
        target.longitude = enriched.longitude;
        target.hasCoordinate = true;
    }
    CorrectKnownStationCoordinates(target);
    if (target.country.empty() && !enriched.country.empty()) {
        target.country = enriched.country;
    }
}

// Site Log enrichment is performed after the daily availability query.  A
// single interactive query can touch many regional databases, and each of
// those databases may contain thousands of equipment-history rows.  Keep a
// small process-local cache of the already materialized, date-specific maps
// so changing the geographic/filter controls or querying the same day again
// does not reopen and rescan every SQLite file.  The database stamp includes
// the WAL sidecar because a builder can commit new Site Logs without changing
// the main database file's timestamp.
using SiteLogRecordMap =
    std::unordered_map<std::string, SiteLogRecord>;

struct SiteLogFileStamp {
    bool exists = false;
    std::uintmax_t size = 0;
    std::int64_t modified = 0;
    std::uintmax_t walSize = 0;
    std::int64_t walModified = 0;

    bool operator==(const SiteLogFileStamp& other) const {
        return exists == other.exists
            && size == other.size
            && modified == other.modified
            && walSize == other.walSize
            && walModified == other.walModified;
    }
};

SiteLogFileStamp GetSiteLogFileStamp(
    const std::filesystem::path& databasePath) {
    SiteLogFileStamp stamp;
    const auto readFile = [](
        const std::filesystem::path& path,
        bool& exists,
        std::uintmax_t& size,
        std::int64_t& modified) {
        std::error_code filesystemError;
        if (!std::filesystem::is_regular_file(path, filesystemError)) {
            return;
        }
        exists = true;
        filesystemError.clear();
        size = std::filesystem::file_size(path, filesystemError);
        if (filesystemError) size = 0;
        filesystemError.clear();
        const auto time = std::filesystem::last_write_time(
            path, filesystemError);
        if (!filesystemError) {
            modified = static_cast<std::int64_t>(
                time.time_since_epoch().count());
        }
    };
    readFile(
        databasePath,
        stamp.exists,
        stamp.size,
        stamp.modified);
    const auto walPath = std::filesystem::path(
        databasePath.wstring() + L"-wal");
    bool walExists = false;
    readFile(walPath, walExists, stamp.walSize, stamp.walModified);
    return stamp;
}

struct SiteLogCacheEntry {
    std::wstring path;
    std::string asOf;
    std::uint64_t networkMask = 0;
    SiteLogFileStamp stamp;
    std::shared_ptr<const SiteLogRecordMap> records;
    std::uint64_t useSerial = 0;
};

std::mutex gSiteLogCacheMutex;
std::vector<SiteLogCacheEntry> gSiteLogCache;
std::uint64_t gSiteLogCacheSerial = 0;
// There are currently up to 42 regional databases and two masks per regional
// database.  Keep one complete query's worth of maps so a second query for
// the same date is genuinely a cache hit instead of evicting the first
// networks while the later ones are loaded.  The cap still prevents a user
// browsing many historical dates from growing memory without bound.
constexpr std::size_t kSiteLogCacheLimit = 96;

std::wstring SiteLogCachePath(
    const std::filesystem::path& databasePath) {
    std::error_code filesystemError;
    const auto absolute = std::filesystem::absolute(
        databasePath, filesystemError);
    return (filesystemError ? databasePath : absolute)
        .lexically_normal().wstring();
}

std::shared_ptr<const SiteLogRecordMap> LoadSiteLogSummaryMapCached(
    const std::filesystem::path& databasePath,
    const std::string& asOf,
    const std::uint64_t networkMask,
    std::wstring& error) {
    error.clear();
    const std::wstring pathKey = SiteLogCachePath(databasePath);
    const SiteLogFileStamp stamp = GetSiteLogFileStamp(databasePath);
    {
        std::scoped_lock lock(gSiteLogCacheMutex);
        for (auto& entry : gSiteLogCache) {
            if (entry.path != pathKey
                || entry.asOf != asOf
                || entry.networkMask != networkMask
                || !(entry.stamp == stamp)) {
                continue;
            }
            entry.useSerial = ++gSiteLogCacheSerial;
            return entry.records;
        }
    }

    auto loaded = std::make_shared<SiteLogRecordMap>();
    if (!LoadSiteLogSummaryMap(
            databasePath, asOf, *loaded, error, networkMask)) {
        return {};
    }
    const std::shared_ptr<const SiteLogRecordMap> records = loaded;
    {
        std::scoped_lock lock(gSiteLogCacheMutex);
        // Replace a stale entry for this path/date rather than retaining two
        // full copies while a Site Log builder is updating the database.
        std::erase_if(
            gSiteLogCache,
            [&](const SiteLogCacheEntry& entry) {
                return entry.path == pathKey
                    && entry.asOf == asOf
                    && entry.networkMask == networkMask;
            });
        gSiteLogCache.push_back({
            pathKey,
            asOf,
            networkMask,
            stamp,
            records,
            ++gSiteLogCacheSerial});
        while (gSiteLogCache.size() > kSiteLogCacheLimit) {
            const auto oldest = std::min_element(
                gSiteLogCache.begin(),
                gSiteLogCache.end(),
                [](const SiteLogCacheEntry& left,
                   const SiteLogCacheEntry& right) {
                    return left.useSerial < right.useSerial;
                });
            if (oldest == gSiteLogCache.end()) break;
            gSiteLogCache.erase(oldest);
        }
    }
    return records;
}

int EnrichStationsWithSiteLogs(
    std::vector<StationFile>& stations,
    const QueryDate& asOfDate,
    std::wstring& error) {
    error.clear();
    if (stations.empty()) {
        return 0;
    }
    // Site Logs are versioned documents, but their equipment fields are
    // time-varying histories.  Pass the observation date so the database can
    // select the receiver/antenna whose Date Installed/Date Removed interval
    // contains that day.  A document's revision date is only provenance (and
    // a compatibility fallback when no equipment interval is available); it
    // is never treated as the equipment replacement date.
    const std::string asOf = std::format(
        "{:04}-{:02}-{:02}",
        asOfDate.year,
        asOfDate.month,
        asOfDate.day);
    struct SiteLogSource {
        ObservationNetwork network;
        std::shared_ptr<const SiteLogRecordMap> byId;
        bool native = false;
    };
    std::vector<SiteLogSource> sources;
    sources.reserve(96);

    // The shared database is the authoritative source for IGS and MGEX.
    // Regional databases receive only the matching rows from the same table
    // when station history changes, so station metadata remains tied to the
    // selected network instead of accidentally matching a four-character ID
    // from another country.
    const auto isGlobal = [](const StationFile& station) {
        return HasNetwork(station.availableNetworks, ObservationNetwork::Igs)
            || HasNetwork(station.availableNetworks, ObservationNetwork::Mgex);
    };
    bool needGlobal = false;
    for (const auto& station : stations) {
        if (isGlobal(station)) {
            needGlobal = true;
            break;
        }
    }
    if (needGlobal) {
        SiteLogSource source{ObservationNetwork::Igs, {}, false};
        std::wstring sourceError;
        source.byId = LoadSiteLogSummaryMapCached(
            GetIgsAvailabilityDatabasePath(), asOf, 0, sourceError);
        if (source.byId != nullptr && !source.byId->empty()) {
            sources.push_back(std::move(source));
        }
    }

    constexpr ObservationNetwork regionalNetworks[] = {
        ObservationNetwork::Ngs, ObservationNetwork::Epn,
        ObservationNetwork::Hk, ObservationNetwork::GaGdc,
        ObservationNetwork::Rgna, ObservationNetwork::Sonel,
        ObservationNetwork::TrigNet, ObservationNetwork::Cut,
        ObservationNetwork::Csn, ObservationNetwork::EposIceland,
        ObservationNetwork::Rbmc, ObservationNetwork::Ramsac,
        ObservationNetwork::RegnaRou, ObservationNetwork::Cacs,
        ObservationNetwork::Rgp, ObservationNetwork::SopacGarner,
        ObservationNetwork::MagnaEco, ObservationNetwork::PanamaCors,
        ObservationNetwork::HondurasCors, ObservationNetwork::EposGnss,
        ObservationNetwork::Gref, ObservationNetwork::OsNet,
        ObservationNetwork::Ergnss, ObservationNetwork::Renep,
        ObservationNetwork::GeoNetNz, ObservationNetwork::Ring,
        ObservationNetwork::Apos, ObservationNetwork::RobBelgium,
        ObservationNetwork::Dpga, ObservationNetwork::CatNet,
        ObservationNetwork::SpsLux, ObservationNetwork::KasiNgii,
        ObservationNetwork::Renag, ObservationNetwork::Nsgi,
        ObservationNetwork::NoaGnss, ObservationNetwork::Redgae,
        ObservationNetwork::GeoNetJapan, ObservationNetwork::Dlab,
        ObservationNetwork::Magnet, ObservationNetwork::PangaCwu
    };
    struct RegionalSiteLogRequest {
        ObservationNetwork network;
        std::filesystem::path path;
    };
    std::vector<RegionalSiteLogRequest> regionalRequests;
    regionalRequests.reserve(std::size(regionalNetworks));
    for (const auto network : regionalNetworks) {
        const bool selected = std::any_of(
            stations.begin(), stations.end(),
            [network](const StationFile& station) {
                return HasNetwork(station.availableNetworks, network);
            });
        if (!selected) continue;
        const auto path = GetNetworkAvailabilityDatabasePath(network);
        std::error_code filesystemError;
        if (!std::filesystem::is_regular_file(path, filesystemError)) continue;
        regionalRequests.push_back({network, path});
    }

    // Each regional database is independent.  Read their native and
    // inherited maps in a small bounded pool so a full-network query is not
    // serialized behind the slowest (often multi-thousand-station) archive.
    struct RegionalSiteLogResult {
        ObservationNetwork network;
        std::shared_ptr<const SiteLogRecordMap> native;
        std::shared_ptr<const SiteLogRecordMap> inherited;
    };
    std::vector<RegionalSiteLogResult> regionalResults(
        regionalRequests.size());
    std::atomic<std::size_t> nextRegional{0};
    const auto loadRegional = [&]() {
        while (true) {
            const std::size_t index = nextRegional.fetch_add(1);
            if (index >= regionalRequests.size()) return;
            const auto request = regionalRequests[index];
            RegionalSiteLogResult loaded;
            loaded.network = request.network;
            std::wstring sourceError;
            loaded.native = LoadSiteLogSummaryMapCached(
                request.path,
                asOf,
                NetworkBit(request.network),
                sourceError);
            sourceError.clear();
            loaded.inherited = LoadSiteLogSummaryMapCached(
                request.path,
                asOf,
                NetworkBit(ObservationNetwork::Igs)
                    | NetworkBit(ObservationNetwork::Mgex),
                sourceError);
            regionalResults[index] = std::move(loaded);
        }
    };
    const int regionalWorkerCount = std::max(
        1,
        std::min<int>(
            6,
            static_cast<int>(regionalRequests.size())));
    std::vector<std::jthread> regionalWorkers;
    regionalWorkers.reserve(static_cast<std::size_t>(regionalWorkerCount));
    for (int index = 0; index < regionalWorkerCount; ++index) {
        regionalWorkers.emplace_back(loadRegional);
    }
    regionalWorkers.clear();

    for (auto& loaded : regionalResults) {
        if (loaded.native != nullptr && !loaded.native->empty()) {
            sources.push_back({loaded.network, loaded.native, true});
        }
        if (loaded.inherited != nullptr && !loaded.inherited->empty()) {
            sources.push_back({loaded.network, loaded.inherited, false});
        }
    }

    if (sources.empty()) return 0;
    int enriched = 0;
    const auto canonicalId = [](const std::string& value) {
        std::string result = value;
        std::transform(
            result.begin(),
            result.end(),
            result.begin(),
            [](const unsigned char character) {
                return static_cast<char>(std::toupper(character));
            });
        return result;
    };
    const auto findRecord = [&](
        const SiteLogSource& source,
        const StationFile& station) -> const SiteLogRecord* {
        if (source.byId == nullptr) return nullptr;
        if (!station.stationId.empty()) {
            const auto exact = source.byId->find(
                canonicalId(station.stationId));
            if (exact != source.byId->end()) {
                return &exact->second;
            }
        }
        if (!station.fourCharId.empty()) {
            const auto four = source.byId->find(
                canonicalId(station.fourCharId));
            if (four != source.byId->end()) {
                return &four->second;
            }
        }
        return nullptr;
    };
    for (auto& station : stations) {
        const SiteLogRecord* found = nullptr;
        // Prefer the selected network's own/native log over a coincidental
        // IGS/MGEX copy.  Some regional stations (for example HKSL/HKWS)
        // are also IGS sites; using the shared source first would hide the
        // richer regional log and make the provenance misleading.  A second
        // pass keeps inherited regional rows as a fallback, and the final
        // global pass handles a station that has no regional row at all.
        for (const bool nativePass : {true, false}) {
            for (const auto& source : sources) {
                if (source.network == ObservationNetwork::Igs
                    || source.native != nativePass
                    || !HasNetwork(
                        station.availableNetworks, source.network)) {
                    continue;
                }
                found = findRecord(source, station);
                if (found != nullptr) break;
            }
            if (found != nullptr) break;
        }
        if (found == nullptr) {
            for (const auto& source : sources) {
                if (source.network != ObservationNetwork::Igs
                    || !isGlobal(station)) {
                    continue;
                }
                found = findRecord(source, station);
                if (found != nullptr) break;
            }
        }
        if (found == nullptr) {
            continue;
        }
        ApplySiteLogRecord(station, *found);
        ++enriched;
    }
    return enriched;
}

// A range query can contain many years of daily rows.  Loading a
// date-specific Site Log map for every day would repeatedly scan every
// regional SQLite file and make a local query look network-bound.  The
// temporal path below loads the complete, history-bearing summary maps once
// (asOf="") and applies the equipment intervals in memory for each dated row.
// The scalar station fields are intentionally taken from the newest summary;
// receiver/antenna/system fields are replaced by the exact installed/removed
// interval for the observation date through ApplySiteLogEquipmentAsOf().
struct TemporalSiteLogSource {
    ObservationNetwork network = ObservationNetwork::Igs;
    std::shared_ptr<const SiteLogRecordMap> byId;
    bool native = false;
};

struct TemporalSiteLogContext {
    std::vector<TemporalSiteLogSource> sources;
};

bool IsGlobalSiteLogStation(const StationFile& station) {
    return HasNetwork(station.availableNetworks, ObservationNetwork::Igs)
        || HasNetwork(station.availableNetworks, ObservationNetwork::Mgex);
}

bool BuildTemporalSiteLogContext(
    const std::vector<StationFile>& stations,
    TemporalSiteLogContext& context,
    std::wstring& error) {
    context.sources.clear();
    error.clear();
    if (stations.empty()) return true;

    bool needGlobal = false;
    for (const auto& station : stations) {
        if (IsGlobalSiteLogStation(station)) {
            needGlobal = true;
            break;
        }
    }
    if (needGlobal) {
        std::wstring sourceError;
        // Empty asOf loads all eligible Site Log documents and preserves the
        // complete equipmentHistory vector.  It is a single cached read.
        auto map = LoadSiteLogSummaryMapCached(
            GetIgsAvailabilityDatabasePath(), "", 0, sourceError);
        if (map != nullptr && !map->empty()) {
            context.sources.push_back({ObservationNetwork::Igs, std::move(map), false});
        }
        if (!sourceError.empty()) error = sourceError;
    }

    constexpr ObservationNetwork regionalNetworks[] = {
        ObservationNetwork::Ngs, ObservationNetwork::Epn,
        ObservationNetwork::Hk, ObservationNetwork::GaGdc,
        ObservationNetwork::Rgna, ObservationNetwork::Sonel,
        ObservationNetwork::TrigNet, ObservationNetwork::Cut,
        ObservationNetwork::Csn, ObservationNetwork::EposIceland,
        ObservationNetwork::Rbmc, ObservationNetwork::Ramsac,
        ObservationNetwork::RegnaRou, ObservationNetwork::Cacs,
        ObservationNetwork::Rgp, ObservationNetwork::SopacGarner,
        ObservationNetwork::MagnaEco, ObservationNetwork::PanamaCors,
        ObservationNetwork::HondurasCors, ObservationNetwork::EposGnss,
        ObservationNetwork::Gref, ObservationNetwork::OsNet,
        ObservationNetwork::Ergnss, ObservationNetwork::Renep,
        ObservationNetwork::GeoNetNz, ObservationNetwork::Ring,
        ObservationNetwork::Apos, ObservationNetwork::RobBelgium,
        ObservationNetwork::Dpga, ObservationNetwork::CatNet,
        ObservationNetwork::SpsLux, ObservationNetwork::KasiNgii,
        ObservationNetwork::Renag, ObservationNetwork::Nsgi,
        ObservationNetwork::NoaGnss, ObservationNetwork::Redgae,
        ObservationNetwork::GeoNetJapan, ObservationNetwork::Dlab,
        ObservationNetwork::Magnet, ObservationNetwork::PangaCwu
    };
    struct Request {
        ObservationNetwork network;
        std::filesystem::path path;
    };
    std::vector<Request> requests;
    requests.reserve(std::size(regionalNetworks));
    for (const auto network : regionalNetworks) {
        const bool selected = std::any_of(
            stations.begin(), stations.end(),
            [network](const StationFile& station) {
                return HasNetwork(station.availableNetworks, network);
            });
        if (!selected) continue;
        const auto path = GetNetworkAvailabilityDatabasePath(network);
        std::error_code filesystemError;
        if (std::filesystem::is_regular_file(path, filesystemError)) {
            requests.push_back({network, path});
        }
    }

    struct Loaded {
        ObservationNetwork network;
        std::shared_ptr<const SiteLogRecordMap> native;
        std::shared_ptr<const SiteLogRecordMap> inherited;
    };
    std::vector<Loaded> loaded(requests.size());
    std::atomic<std::size_t> next{0};
    const auto worker = [&]() {
        while (true) {
            const std::size_t index = next.fetch_add(1);
            if (index >= requests.size()) return;
            const auto request = requests[index];
            Loaded item;
            item.network = request.network;
            std::wstring ignored;
            item.native = LoadSiteLogSummaryMapCached(
                request.path, "", NetworkBit(request.network), ignored);
            ignored.clear();
            item.inherited = LoadSiteLogSummaryMapCached(
                request.path,
                "",
                NetworkBit(ObservationNetwork::Igs)
                    | NetworkBit(ObservationNetwork::Mgex),
                ignored);
            loaded[index] = std::move(item);
        }
    };
    const int workerCount = std::max(
        1,
        std::min<int>(6, static_cast<int>(requests.size())));
    std::vector<std::jthread> workers;
    workers.reserve(static_cast<std::size_t>(workerCount));
    for (int index = 0; index < workerCount; ++index) {
        workers.emplace_back(worker);
    }
    workers.clear();

    for (auto& item : loaded) {
        if (item.native != nullptr && !item.native->empty()) {
            context.sources.push_back({item.network, item.native, true});
        }
        if (item.inherited != nullptr && !item.inherited->empty()) {
            context.sources.push_back({item.network, item.inherited, false});
        }
    }
    return true;
}

const SiteLogRecord* FindTemporalSiteLogRecord(
    const TemporalSiteLogContext& context,
    const StationFile& station) {
    const auto canonical = [](const std::string& value) {
        std::string result = value;
        std::transform(
            result.begin(), result.end(), result.begin(),
            [](const unsigned char character) {
                return static_cast<char>(std::toupper(character));
            });
        return result;
    };
    const auto findIn = [&](const TemporalSiteLogSource& source)
        -> const SiteLogRecord* {
        if (source.byId == nullptr) return nullptr;
        if (!station.stationId.empty()) {
            const auto found = source.byId->find(canonical(station.stationId));
            if (found != source.byId->end()) return &found->second;
        }
        if (!station.fourCharId.empty()) {
            const auto found = source.byId->find(canonical(station.fourCharId));
            if (found != source.byId->end()) return &found->second;
        }
        return nullptr;
    };
    // Native regional logs have precedence over inherited IGS/MGEX rows.
    for (const bool nativePass : {true, false}) {
        for (const auto& source : context.sources) {
            if (source.network == ObservationNetwork::Igs
                || source.native != nativePass
                || !HasNetwork(station.availableNetworks, source.network)) {
                continue;
            }
            if (const auto* found = findIn(source); found != nullptr) {
                return found;
            }
        }
    }
    for (const auto& source : context.sources) {
        if (source.network == ObservationNetwork::Igs
            && IsGlobalSiteLogStation(station)) {
            if (const auto* found = findIn(source); found != nullptr) {
                return found;
            }
        }
    }
    return nullptr;
}

bool EnrichStationWithTemporalSiteLog(
    StationFile& station,
    const QueryDate& date,
    const TemporalSiteLogContext& context) {
    const auto* found = FindTemporalSiteLogRecord(context, station);
    if (found == nullptr) return false;
    const std::string asOf = std::format(
        "{:04}-{:02}-{:02}", date.year, date.month, date.day);
    // Use the lightweight overload: SiteLogRecord contains the complete
    // equipment history, and copying it for every dated file made a long
    // range query needlessly expensive.  The overload copies scalar metadata
    // and selects only the receiver/antenna epoch effective on this date.
    SiteLogRecord snapshot;
    ApplySiteLogEquipmentAsOf(*found, asOf, snapshot);
    ApplySiteLogRecord(station, snapshot);
    return true;
}

bool HasTemporalMetadataFilter(const AdvancedFilterState& filter) {
    return !filter.systemsInclude.empty()
        || !filter.systemsExclude.empty()
        || !filter.receiverInclude.empty()
        || !filter.receiverExclude.empty()
        || !filter.antennaInclude.empty()
        || !filter.antennaExclude.empty();
}

std::vector<std::string> SplitTemporalFilterTokens(
    const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;
    const auto flush = [&]() {
        if (current.empty()) return;
        std::transform(
            current.begin(), current.end(), current.begin(),
            [](const unsigned char character) {
                return static_cast<char>(std::toupper(character));
            });
        tokens.push_back(current);
        current.clear();
    };
    for (const unsigned char character : text) {
        if (character == ',' || character == ';' || character == '/'
            || character == '|' || character == '+'
            || std::isspace(character)) {
            flush();
        } else {
            current.push_back(static_cast<char>(character));
        }
    }
    flush();
    return tokens;
}

bool TemporalFieldMatches(
    const std::string& value,
    const std::string& include,
    const std::string& exclude) {
    std::string normalized = value;
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    const auto includeTokens = SplitTemporalFilterTokens(include);
    const auto excludeTokens = SplitTemporalFilterTokens(exclude);
    if ((!includeTokens.empty() || !excludeTokens.empty())
        && normalized.empty()) {
        return false;
    }
    for (const auto& token : includeTokens) {
        if (normalized.find(token) == std::string::npos) return false;
    }
    for (const auto& token : excludeTokens) {
        if (!token.empty()
            && normalized.find(token) != std::string::npos) {
            return false;
        }
    }
    return true;
}

bool MatchesTemporalMetadataFilter(
    const StationFile& station,
    const AdvancedFilterState& filter) {
    if (HasTemporalMetadataFilter(filter) && !station.hasSiteLog) {
        return false;
    }
    return TemporalFieldMatches(
               station.siteLogSystems,
               filter.systemsInclude,
               filter.systemsExclude)
        && TemporalFieldMatches(
               station.siteLogReceiver,
               filter.receiverInclude,
               filter.receiverExclude)
        && TemporalFieldMatches(
               station.siteLogAntenna,
               filter.antennaInclude,
               filter.antennaExclude);
}

std::string TemporalStationKey(const StationFile& station) {
    const auto canonical = [](std::string value) {
        std::transform(
            value.begin(), value.end(), value.begin(),
            [](const unsigned char character) {
                return static_cast<char>(std::toupper(character));
            });
        return value;
    };
    if (!station.stationId.empty()) {
        return "ID:" + canonical(station.stationId);
    }
    if (!station.fourCharId.empty()) {
        return "FOUR:" + canonical(station.fourCharId)
            + ":" + canonical(station.country);
    }
    return "FILE:" + canonical(station.fileName);
}

int TemporalDateKey(const QueryDate& date) {
    return date.year * 400 + date.dayOfYear;
}

int TemporalFilePreference(const StationFile& station) {
    int score = station.format == RinexFormat::Rinex34 ? 100 : 10;
    std::string interval = station.interval;
    std::transform(
        interval.begin(), interval.end(), interval.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    if (interval == "30S") score += 20;
    else if (interval == "15S") score += 15;
    else if (interval == "01S" || interval == "1S") score += 10;
    if (!station.downloadUrl.empty()) score += 5;
    if (!station.downloadRoutes.empty()) score += 3;
    return score;
}

std::wstring FormatAvailableRinexFamilies(const StationFile& station) {
    const bool hasRinex2 = HasRinexFormat(station, RinexFormat::Rinex2);
    const bool hasRinex34 = HasRinexFormat(station, RinexFormat::Rinex34);
    if (hasRinex2 && hasRinex34) return L"RINEX 2 + 3/4";
    if (hasRinex2) return L"RINEX 2";
    if (hasRinex34) return L"RINEX 3/4";
    return L"—";
}

struct TemporalSiteLogProcessResult {
    std::vector<StationFile> stations;
    std::vector<DatedStationFile> periodFiles;
    int matchedCount = 0;
    int filteredPeriodFiles = 0;
    std::wstring error;
};

TemporalSiteLogProcessResult ProcessTemporalSiteLogPeriod(
    const std::vector<StationFile>& inputStations,
    std::vector<DatedStationFile> inputFiles,
    const AdvancedFilterState& filter,
    const int expectedDays,
    const std::stop_token stopToken) {
    TemporalSiteLogProcessResult output;
    if (inputStations.empty()) return output;

    TemporalSiteLogContext context;
    BuildTemporalSiteLogContext(inputStations, context, output.error);

    std::unordered_map<std::string, std::size_t> inputByKey;
    inputByKey.reserve(inputStations.size() * 2U);
    for (std::size_t index = 0; index < inputStations.size(); ++index) {
        inputByKey.emplace(TemporalStationKey(inputStations[index]), index);
    }

    std::vector<std::vector<DatedStationFile>> filesByStation(
        inputStations.size());
    std::vector<bool> stationMatched(inputStations.size(), false);
    for (auto& dated : inputFiles) {
        if (stopToken.stop_requested()) {
            output.error = L"Site Log 日期履历筛选已取消。";
            return output;
        }
        std::size_t stationIndex = dated.stationIndex;
        if (stationIndex >= inputStations.size()) {
            const auto found = inputByKey.find(
                TemporalStationKey(dated.station));
            if (found == inputByKey.end()) {
                ++output.filteredPeriodFiles;
                continue;
            }
            stationIndex = found->second;
        }
        dated.stationIndex = stationIndex;
        if (EnrichStationWithTemporalSiteLog(
                dated.station, dated.date, context)) {
            stationMatched[stationIndex] = true;
        }
        if (!MatchesTemporalMetadataFilter(dated.station, filter)) {
            ++output.filteredPeriodFiles;
            continue;
        }
        filesByStation[stationIndex].push_back(std::move(dated));
    }

    const int normalizedExpectedDays = std::max(1, expectedDays);
    const double ratioThreshold = std::clamp(
        std::isfinite(filter.minimumCompletenessPercent)
            ? filter.minimumCompletenessPercent : 0.0,
        0.0,
        100.0);
    const int daysThreshold = std::max(0, filter.minimumAvailableDays);

    output.stations.reserve(inputStations.size());
    output.periodFiles.reserve(inputFiles.size());
    for (std::size_t stationIndex = 0;
         stationIndex < filesByStation.size();
         ++stationIndex) {
        auto& stationFiles = filesByStation[stationIndex];
        if (stationFiles.empty()) continue;

        // A knowledge-base row is normally already unique per station/day.
        // Keep this second guard because mixed archives can expose equivalent
        // RINEX names through more than one selected network.
        std::unordered_map<int, std::size_t> bestByDay;
        std::vector<std::size_t> chosen;
        chosen.reserve(stationFiles.size());
        for (std::size_t index = 0; index < stationFiles.size(); ++index) {
            const int dayKey = TemporalDateKey(stationFiles[index].date);
            const auto found = bestByDay.find(dayKey);
            if (found == bestByDay.end()) {
                bestByDay.emplace(dayKey, chosen.size());
                chosen.push_back(index);
            } else {
                auto& existingIndex = chosen[found->second];
                if (TemporalFilePreference(stationFiles[index].station)
                    > TemporalFilePreference(
                        stationFiles[existingIndex].station)) {
                    existingIndex = index;
                }
            }
        }
        std::sort(
            chosen.begin(), chosen.end(),
            [&](const std::size_t left, const std::size_t right) {
                return TemporalDateKey(stationFiles[left].date)
                    < TemporalDateKey(stationFiles[right].date);
            });

        const int availableDays = static_cast<int>(chosen.size());
        const double completeness = 100.0
            * static_cast<double>(availableDays)
            / static_cast<double>(normalizedExpectedDays);
        bool qualifies = availableDays > 0;
        if (filter.completenessMode
            == AdvancedFilterState::CompletenessMode::RatioAtLeast) {
            qualifies = qualifies
                && completeness + 1e-9 >= ratioThreshold;
        } else if (filter.completenessMode
                   == AdvancedFilterState::CompletenessMode::DaysAtLeast) {
            qualifies = qualifies && availableDays >= daysThreshold;
        }
        if (!qualifies) {
            output.filteredPeriodFiles += availableDays;
            continue;
        }

        // Build the visible representative from a real dated file row rather
        // than the lightweight lookup-only inputStations vector.  The latter
        // intentionally omits file names, formats and download routes to keep
        // the UI thread cheap; using it here would leave the range list with
        // blank file/routing fields after temporal enrichment.
        StationFile representative = stationFiles[chosen.front()].station;
        representative.selected = false;
        representative.inSelectionScope = true;
        representative.availableDays = availableDays;
        representative.expectedDays = normalizedExpectedDays;
        representative.completenessPercent = completeness;
        // The list has one row per station, so show the first qualifying
        // observation day's equipment.  Every period file below retains its
        // own exact dated receiver/antenna snapshot for filtering/download.
        CopySiteLogMetadata(
            representative,
            stationFiles[chosen.front()].station);
        for (const auto chosenIndex : chosen) {
            const auto& station = stationFiles[chosenIndex].station;
            representative.availableRinexFormats =
                static_cast<RinexFormatMask>(
                    EffectiveRinexFormatMask(representative)
                    | EffectiveRinexFormatMask(station));
            representative.availableNetworks =
                static_cast<ObservationNetworkMask>(
                    representative.availableNetworks
                    | station.availableNetworks);
            if (!representative.hasCoordinate && station.hasCoordinate) {
                representative.hasCoordinate = true;
                representative.latitude = station.latitude;
                representative.longitude = station.longitude;
            }
            if (representative.country.empty() && !station.country.empty()) {
                representative.country = station.country;
            }
            for (const auto& source : station.availabilitySources) {
                if (std::find(
                        representative.availabilitySources.begin(),
                        representative.availabilitySources.end(), source)
                    == representative.availabilitySources.end()) {
                    representative.availabilitySources.push_back(source);
                }
            }
        }

        const std::size_t outputIndex = output.stations.size();
        output.stations.push_back(std::move(representative));
        for (const auto chosenIndex : chosen) {
            auto dated = std::move(stationFiles[chosenIndex]);
            dated.stationIndex = outputIndex;
            dated.station.selected = false;
            dated.station.inSelectionScope = true;
            dated.station.availableDays = availableDays;
            dated.station.expectedDays = normalizedExpectedDays;
            dated.station.completenessPercent = completeness;
            output.periodFiles.push_back(std::move(dated));
        }
    }

    output.matchedCount = static_cast<int>(std::count(
        stationMatched.begin(), stationMatched.end(), true));
    std::sort(
        output.periodFiles.begin(), output.periodFiles.end(),
        [](const DatedStationFile& left, const DatedStationFile& right) {
            const int leftDate = TemporalDateKey(left.date);
            const int rightDate = TemporalDateKey(right.date);
            if (leftDate != rightDate) return leftDate < rightDate;
            return left.stationIndex < right.stationIndex;
        });
    return output;
}

std::wstring GetWindowTextString(const HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring value(
        static_cast<std::size_t>(std::max(0, length)) + 1U,
        L'\0');
    if (length > 0) {
        GetWindowTextW(window, value.data(), length + 1);
    }
    value.resize(static_cast<std::size_t>(std::max(0, length)));
    return value;
}

std::wstring FormatNetworkSelection(const NetworkSelection selection) {
    std::wstring result;
    const auto append = [&](const wchar_t* name) {
        if (!result.empty()) {
            result += L" + ";
        }
        result += name;
    };
    if (HasNetwork(selection.mask, ObservationNetwork::Igs)) {
        append(L"IGS");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::Mgex)) {
        append(L"MGEX");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::Ngs)) {
        append(L"NGS");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::Epn)) {
        append(L"EPN");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::Hk)) {
        append(L"HK");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::GaGdc)) {
        append(L"GA-GDC");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::Rgna)) {
        append(L"RGNA");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::Sonel)) {
        append(L"SONEL");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::TrigNet)) {
        append(L"TrigNet");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::Cut)) {
        append(L"CUT");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::Csn)) {
        append(L"CSN");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::EposIceland)) {
        append(L"EPOS-IS");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::Rbmc)) {
        append(L"IBGE RBMC");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::Ramsac)) {
        append(L"IGN RAMSAC");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::RegnaRou)) {
        append(L"REGNA-ROU");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::Cacs)) {
        append(L"CACS");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::Rgp)) {
        append(L"RGP");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::SopacGarner)) {
        append(L"SOPAC/GARNER");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::MagnaEco)) {
        append(L"MAGNA-ECO");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::PanamaCors)) {
        append(L"PAN CORS");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::HondurasCors)) {
        append(L"HND CORS");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::EposGnss)) {
        append(L"EPOS-GNSS");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::Gref)) append(L"GREF");
    if (HasNetwork(selection.mask, ObservationNetwork::OsNet)) append(L"OS Net");
    if (HasNetwork(selection.mask, ObservationNetwork::Ergnss)) append(L"ERGNSS");
    if (HasNetwork(selection.mask, ObservationNetwork::Renep)) append(L"RENEP");
    if (HasNetwork(selection.mask, ObservationNetwork::GeoNetNz)) {
        append(L"GeoNet/PositioNZ");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::Ring)) append(L"RING");
    if (HasNetwork(selection.mask, ObservationNetwork::Apos)) append(L"APOS");
    if (HasNetwork(selection.mask, ObservationNetwork::RobBelgium)) {
        append(L"ROB Belgium");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::Dpga)) append(L"DPGA");
    if (HasNetwork(selection.mask, ObservationNetwork::CatNet)) append(L"CatNet");
    if (HasNetwork(selection.mask, ObservationNetwork::SpsLux)) append(L"SPSLux");
    if (HasNetwork(selection.mask, ObservationNetwork::KasiNgii)) {
        append(L"KASI-NGII");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::Renag)) append(L"RENAG");
    if (HasNetwork(selection.mask, ObservationNetwork::Nsgi)) append(L"NSGI");
    if (HasNetwork(selection.mask, ObservationNetwork::NoaGnss)) {
        append(L"NOA GNSS");
    }
    if (HasNetwork(selection.mask, ObservationNetwork::Redgae)) append(L"REDGAE");
    if (HasNetwork(selection.mask, ObservationNetwork::GeoNetJapan)) append(L"GEONET JP");
    if (HasNetwork(selection.mask, ObservationNetwork::Dlab)) append(L"DLAB");
    if (HasNetwork(selection.mask, ObservationNetwork::Magnet)) append(L"MAGNET");
    if (HasNetwork(selection.mask, ObservationNetwork::PangaCwu)) append(L"PANGA/CWU");
    return result.empty() ? L"未选择" : result;
}

std::wstring FormatAvailableNetworks(const ObservationNetworkMask mask) {
    std::wstring result;
    const auto append = [&](const wchar_t* name) {
        if (!result.empty()) {
            result += L"+";
        }
        result += name;
    };
    if (HasNetwork(mask, ObservationNetwork::Igs)) {
        append(L"IGS");
    }
    if (HasNetwork(mask, ObservationNetwork::Mgex)) {
        append(L"MGEX");
    }
    if (HasNetwork(mask, ObservationNetwork::Ngs)) {
        append(L"NGS");
    }
    if (HasNetwork(mask, ObservationNetwork::Epn)) {
        append(L"EPN");
    }
    if (HasNetwork(mask, ObservationNetwork::Hk)) {
        append(L"HK");
    }
    if (HasNetwork(mask, ObservationNetwork::GaGdc)) {
        append(L"GA-GDC");
    }
    if (HasNetwork(mask, ObservationNetwork::Rgna)) {
        append(L"RGNA");
    }
    if (HasNetwork(mask, ObservationNetwork::Sonel)) {
        append(L"SONEL");
    }
    if (HasNetwork(mask, ObservationNetwork::TrigNet)) {
        append(L"TrigNet");
    }
    if (HasNetwork(mask, ObservationNetwork::Cut)) {
        append(L"CUT");
    }
    if (HasNetwork(mask, ObservationNetwork::Csn)) {
        append(L"CSN");
    }
    if (HasNetwork(mask, ObservationNetwork::EposIceland)) {
        append(L"EPOS-IS");
    }
    if (HasNetwork(mask, ObservationNetwork::Rbmc)) {
        append(L"IBGE RBMC");
    }
    if (HasNetwork(mask, ObservationNetwork::Ramsac)) {
        append(L"IGN RAMSAC");
    }
    if (HasNetwork(mask, ObservationNetwork::RegnaRou)) {
        append(L"REGNA-ROU");
    }
    if (HasNetwork(mask, ObservationNetwork::Cacs)) {
        append(L"CACS");
    }
    if (HasNetwork(mask, ObservationNetwork::Rgp)) {
        append(L"RGP");
    }
    if (HasNetwork(mask, ObservationNetwork::SopacGarner)) {
        append(L"SOPAC/GARNER");
    }
    if (HasNetwork(mask, ObservationNetwork::MagnaEco)) {
        append(L"MAGNA-ECO");
    }
    if (HasNetwork(mask, ObservationNetwork::PanamaCors)) {
        append(L"PAN CORS");
    }
    if (HasNetwork(mask, ObservationNetwork::HondurasCors)) {
        append(L"HND CORS");
    }
    if (HasNetwork(mask, ObservationNetwork::EposGnss)) {
        append(L"EPOS-GNSS");
    }
    if (HasNetwork(mask, ObservationNetwork::Gref)) append(L"GREF");
    if (HasNetwork(mask, ObservationNetwork::OsNet)) append(L"OS Net");
    if (HasNetwork(mask, ObservationNetwork::Ergnss)) append(L"ERGNSS");
    if (HasNetwork(mask, ObservationNetwork::Renep)) append(L"RENEP");
    if (HasNetwork(mask, ObservationNetwork::GeoNetNz)) {
        append(L"GeoNet/PositioNZ");
    }
    if (HasNetwork(mask, ObservationNetwork::Ring)) append(L"RING");
    if (HasNetwork(mask, ObservationNetwork::Apos)) append(L"APOS");
    if (HasNetwork(mask, ObservationNetwork::RobBelgium)) append(L"ROB Belgium");
    if (HasNetwork(mask, ObservationNetwork::Dpga)) append(L"DPGA");
    if (HasNetwork(mask, ObservationNetwork::CatNet)) append(L"CatNet");
    if (HasNetwork(mask, ObservationNetwork::SpsLux)) append(L"SPSLux");
    if (HasNetwork(mask, ObservationNetwork::KasiNgii)) append(L"KASI-NGII");
    if (HasNetwork(mask, ObservationNetwork::Renag)) append(L"RENAG");
    if (HasNetwork(mask, ObservationNetwork::Nsgi)) append(L"NSGI");
    if (HasNetwork(mask, ObservationNetwork::NoaGnss)) append(L"NOA GNSS");
    if (HasNetwork(mask, ObservationNetwork::Redgae)) append(L"REDGAE");
    if (HasNetwork(mask, ObservationNetwork::GeoNetJapan)) append(L"GEONET JP");
    if (HasNetwork(mask, ObservationNetwork::Dlab)) append(L"DLAB");
    if (HasNetwork(mask, ObservationNetwork::Magnet)) append(L"MAGNET");
    if (HasNetwork(mask, ObservationNetwork::PangaCwu)) append(L"PANGA/CWU");
    return result;
}

std::wstring FormatBytes(const std::uint64_t bytes) {
    constexpr double kib = 1024.0;
    constexpr double mib = kib * 1024.0;
    constexpr double gib = mib * 1024.0;
    if (bytes >= static_cast<std::uint64_t>(gib)) {
        return std::format(L"{:.2f} GiB", bytes / gib);
    }
    if (bytes >= static_cast<std::uint64_t>(mib)) {
        return std::format(L"{:.1f} MiB", bytes / mib);
    }
    if (bytes >= static_cast<std::uint64_t>(kib)) {
        return std::format(L"{:.1f} KiB", bytes / kib);
    }
    return std::to_wstring(bytes) + L" B";
}

std::filesystem::path DefaultDownloadFolder() {
    PWSTR downloads = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(
            FOLDERID_Downloads,
            KF_FLAG_DEFAULT,
            nullptr,
            &downloads))) {
        const std::filesystem::path path =
            std::filesystem::path(downloads) / L"IGSDownloader";
        CoTaskMemFree(downloads);
        return path;
    }
    wchar_t current[MAX_PATH]{};
    GetCurrentDirectoryW(static_cast<DWORD>(std::size(current)), current);
    return std::filesystem::path(current) / L"IGSDownloader";
}

void SetDatePickerToTwoDaysAgo(const HWND picker) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    FILETIME fileTime{};
    if (!SystemTimeToFileTime(&now, &fileTime)) {
        DateTime_SetSystemtime(picker, GDT_VALID, &now);
        return;
    }
    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    constexpr ULONGLONG twoDays = 2ULL * 24ULL * 60ULL * 60ULL * 10'000'000ULL;
    if (value.QuadPart > twoDays) {
        value.QuadPart -= twoDays;
    }
    fileTime.dwLowDateTime = value.LowPart;
    fileTime.dwHighDateTime = value.HighPart;
    SYSTEMTIME recent{};
    FileTimeToSystemTime(&fileTime, &recent);
    DateTime_SetSystemtime(picker, GDT_VALID, &recent);
}

std::filesystem::path UiStateFilePath() {
    return DownloadFolderState::FilePath();
}

std::wstring FormatSystemDate(const SYSTEMTIME& date) {
    return std::format(
        L"{:04}-{:02}-{:02}", date.wYear, date.wMonth, date.wDay);
}

bool ParseSystemDate(const std::wstring& text, SYSTEMTIME& date) {
    unsigned int year = 0;
    unsigned int month = 0;
    unsigned int day = 0;
    if (swscanf_s(text.c_str(), L"%u-%u-%u", &year, &month, &day) != 3
        || year < 1900 || year > 9999 || month < 1 || month > 12
        || day < 1 || day > 31) {
        return false;
    }
    SYSTEMTIME candidate{};
    candidate.wYear = static_cast<WORD>(year);
    candidate.wMonth = static_cast<WORD>(month);
    candidate.wDay = static_cast<WORD>(day);
    FILETIME fileTime{};
    if (!SystemTimeToFileTime(&candidate, &fileTime)) return false;
    date = candidate;
    return true;
}

bool LoadStoredDateRange(
    const wchar_t* section, SYSTEMTIME& start, SYSTEMTIME& end) {
    const auto path = UiStateFilePath();
    const auto pathText = path.wstring();
    wchar_t startText[32]{};
    wchar_t endText[32]{};
    GetPrivateProfileStringW(
        section, L"start", L"", startText, static_cast<DWORD>(std::size(startText)),
        pathText.c_str());
    GetPrivateProfileStringW(
        section, L"end", L"", endText, static_cast<DWORD>(std::size(endText)),
        pathText.c_str());
    return ParseSystemDate(startText, start)
        && ParseSystemDate(endText, end);
}

void SaveStoredDateRange(
    const wchar_t* section, const SYSTEMTIME& start, const SYSTEMTIME& end) {
    const auto path = UiStateFilePath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const auto pathText = path.wstring();
    const auto startText = FormatSystemDate(start);
    const auto endText = FormatSystemDate(end);
    WritePrivateProfileStringW(
        section, L"start", startText.c_str(), pathText.c_str());
    WritePrivateProfileStringW(
        section, L"end", endText.c_str(), pathText.c_str());
}

void LoadDatePickerState(const HWND startPicker, const HWND endPicker) {
    SYSTEMTIME start{};
    SYSTEMTIME end{};
    if (!LoadStoredDateRange(L"dates", start, end)) return;
    DateTime_SetSystemtime(startPicker, GDT_VALID, &start);
    DateTime_SetSystemtime(endPicker, GDT_VALID, &end);
}

void SaveDatePickerState(const HWND startPicker, const HWND endPicker) {
    SYSTEMTIME start{};
    SYSTEMTIME end{};
    if (DateTime_GetSystemtime(startPicker, &start) != GDT_VALID
        || DateTime_GetSystemtime(endPicker, &end) != GDT_VALID) {
        return;
    }
    SaveStoredDateRange(L"dates", start, end);
}

struct ObservationQueuedDownload {
    std::uint64_t id = 0;
    std::wstring label;
    std::wstring key;
    std::wstring folder;
    std::wstring state = L"pending";
    int percent = 0;
    bool selected = true;
    bool removeAfterCancel = false;
    bool range = false;
    bool deferred = false;
    bool verifyTemporalMetadata = false;
    QueryDate start{};
    QueryDate end{};
    NetworkSelection selection{};
    ObservationRangeOptions options{};
    std::vector<DatedStationFile> files;
    std::vector<StationFile> deferredStations;
};

struct StationSelectionWorkerResult {
    StationSelectionPlan plan;
    std::uint64_t signature = 0;
    std::uint64_t request = 0;
    bool candidateGeometry = false;
};

class MainWindow {
public:
    explicit MainWindow(
        const HWND window,
        const HINSTANCE instance,
        const int initialShowCommand)
        : window_(window),
          instance_(instance),
          initialShowCommand_(initialShowCommand) {
    }

    ~MainWindow() {
        // Stop the browser surface first so no asynchronous WebView callback
        // can arrive while the native controller is being torn down.
        webUi_.reset();
        if (stationSelectionThread_.joinable()) {
            stationSelectionThread_.request_stop();
            stationSelectionThread_.join();
        }
        if (queryThread_.joinable()) {
            queryThread_.request_stop();
            queryThread_.join();
        }
        if (siteLogThread_.joinable()) {
            siteLogThread_.request_stop();
            siteLogThread_.join();
        }
        if (postprocessThread_.joinable()) {
            postprocessThread_.request_stop(); postprocessThread_.join();
        }
        planWorkers_.JoinAll();
        if (downloadThread_.joinable()) {
            downloadThread_.request_stop();
            if (downloadControl_ != nullptr) {
                downloadControl_->resume();
            }
            downloadThread_.join();
        }
        if (knowledgeBaseStatusThread_.joinable()) {
            knowledgeBaseStatusThread_.request_stop();
            knowledgeBaseStatusThread_.join();
        }
        if (observationSearchPreloadThread_.joinable()) {
            observationSearchPreloadThread_.request_stop();
            observationSearchPreloadThread_.join();
        }
        downloadControl_.reset();
        if (countryFlags_ != nullptr) {
            ImageList_Destroy(countryFlags_);
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
        if (sectionFont_ != nullptr) {
            DeleteObject(sectionFont_);
        }
    }

    bool CreateControls() {
        RegisterAdvancedDialogClass(instance_);
        RegisterDateAdvancedDialogClass(instance_);
        titleFont_ = CreateFontW(
            -28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
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
        sectionFont_ = CreateFontW(
            -16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");

        title_ = CreateStatic(L"MARK", SS_LEFT);
        subtitle_ = CreateStatic(
            L"GNSS knowledge system · metadata · rules · evidence",
            SS_LEFT);
        observationModeButton_ = CreateButton(
            L"观测数据",
            IdObservationMode,
            WS_GROUP);
        preciseModeButton_ = CreateButton(
            L"精密产品",
            IdPreciseMode);
        SetWindowLongPtrW(observationModeButton_, GWLP_USERDATA, 1);
        SetWindowLongPtrW(preciseModeButton_, GWLP_USERDATA, 0);
        networkLabel_ = CreateStatic(
            L"数据源 / 站网（红色表示本地索引不可用）",
            SS_LEFT);
        networkPanelToggleButton_ = CreateButton(
            L"选择数据源", IdNetworkPanelToggle);
        selectKnowledgeBaseNetworksButton_ = CreateButton(
            L"选择可用站网",
            IdSelectKnowledgeBaseNetworks);
        igsCheck_ = CreateCheckBox(L"IGS", IdIgsCheck);
        mgexCheck_ = CreateCheckBox(L"MGEX", IdMgexCheck);
        ngsCheck_ = CreateCheckBox(L"NGS", IdNgsCheck);
        epnCheck_ = CreateCheckBox(L"EPN", IdEpnCheck);
        hkCheck_ = CreateCheckBox(L"HK", IdHkCheck);
        gaGdcCheck_ = CreateCheckBox(L"GA-GDC", IdGaGdcCheck);
        rgnaCheck_ = CreateCheckBox(L"RGNA", IdRgnaCheck);
        sonelCheck_ = CreateCheckBox(L"SONEL", IdSonelCheck);
        trigNetCheck_ = CreateCheckBox(L"TrigNet", IdTrigNetCheck);
        cutCheck_ = CreateCheckBox(L"CUT", IdCutCheck);
        csnCheck_ = CreateCheckBox(L"CSN", IdCsnCheck);
        eposIcelandCheck_ = CreateCheckBox(L"EPOS-IS", IdEposIcelandCheck);
        rbmcCheck_ = CreateCheckBox(L"IBGE RBMC", IdRbmcCheck);
        ramsacCheck_ = CreateCheckBox(L"IGN RAMSAC", IdRamsacCheck);
        regnaRouCheck_ = CreateCheckBox(L"REGNA-ROU", IdRegnaRouCheck);
        cacsCheck_ = CreateCheckBox(L"CACS", IdCacsCheck);
        rgpCheck_ = CreateCheckBox(L"RGP", IdRgpCheck);
        sopacGarnerCheck_ =
            CreateCheckBox(L"SOPAC/GARNER", IdSopacGarnerCheck);
        magnaEcoCheck_ =
            CreateCheckBox(L"MAGNA-ECO", IdMagnaEcoCheck);
        panamaCorsCheck_ =
            CreateCheckBox(L"PAN CORS", IdPanamaCorsCheck);
        hondurasCorsCheck_ =
            CreateCheckBox(L"HND CORS", IdHondurasCorsCheck);
        eposGnssCheck_ =
            CreateCheckBox(L"EPOS-GNSS", IdEposGnssCheck);
        grefCheck_ = CreateCheckBox(L"GREF", IdGrefCheck);
        osNetCheck_ = CreateCheckBox(L"OS Net", IdOsNetCheck);
        ergnssCheck_ = CreateCheckBox(L"ERGNSS", IdErgnssCheck);
        renepCheck_ = CreateCheckBox(L"RENEP", IdRenepCheck);
        geoNetNzCheck_ =
            CreateCheckBox(L"GeoNet NZ", IdGeoNetNzCheck);
        ringCheck_ = CreateCheckBox(L"RING", IdRingCheck);
        aposCheck_ = CreateCheckBox(L"APOS", IdAposCheck);
        robBelgiumCheck_ = CreateCheckBox(L"ROB Belgium", IdRobBelgiumCheck);
        dpgaCheck_ = CreateCheckBox(L"DPGA", IdDpgaCheck);
        catNetCheck_ = CreateCheckBox(L"CatNet", IdCatNetCheck);
        spsLuxCheck_ = CreateCheckBox(L"SPSLux", IdSpsLuxCheck);
        kasiNgiiCheck_ = CreateCheckBox(L"KASI-NGII", IdKasiNgiiCheck);
        renagCheck_ = CreateCheckBox(L"RENAG", IdRenagCheck);
        nsgiCheck_ = CreateCheckBox(L"NSGI", IdNsgiCheck);
        noaGnssCheck_ = CreateCheckBox(L"NOA GNSS", IdNoaGnssCheck);
        redgaeCheck_ = CreateCheckBox(L"REDGAE", IdRedgaeCheck);
        geoNetJapanCheck_ = CreateCheckBox(L"GEONET JP", IdGeoNetJapanCheck);
        dlabCheck_ = CreateCheckBox(L"DLAB", IdDlabCheck);
        magnetCheck_ = CreateCheckBox(L"MAGNET", IdMagnetCheck);
        pangaCwuCheck_ = CreateCheckBox(L"PANGA/CWU", IdPangaCwuCheck);
        SendMessageW(igsCheck_, BM_SETCHECK, BST_CHECKED, 0);
        dateLabel_ = CreateStatic(L"起始日期（UTC）", SS_LEFT);
        datePicker_ = CreateWindowExW(
            0,
            DATETIMEPICK_CLASSW,
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | DTS_SHORTDATEFORMAT,
            0, 0, 100, 30,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdDatePicker)),
            instance_,
            nullptr);
        DateTime_SetFormat(datePicker_, L"yyyy-MM-dd");
        SetDatePickerToTwoDaysAgo(datePicker_);
        dateRangeToLabel_ = CreateStatic(L"至", SS_CENTER);
        endDatePicker_ = CreateWindowExW(
            0,
            DATETIMEPICK_CLASSW,
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | DTS_SHORTDATEFORMAT,
            0, 0, 140, 30,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdDateRangeEndPicker)),
            instance_,
            nullptr);
        DateTime_SetFormat(endDatePicker_, L"yyyy-MM-dd");
        // A range query defaults to one day, preserving the original daily
        // workflow until the user explicitly changes the end date.
        SYSTEMTIME startForEnd{};
        if (DateTime_GetSystemtime(datePicker_, &startForEnd) == GDT_VALID) {
            DateTime_SetSystemtime(endDatePicker_, GDT_VALID, &startForEnd);
        }
        LoadDatePickerState(datePicker_, endDatePicker_);
        DateTime_GetSystemtime(datePicker_, &queryStartDate_);
        DateTime_GetSystemtime(endDatePicker_, &queryEndDate_);

        queryButton_ = CreateButton(L"查询测站", IdQueryButton);
        quickSchemeLabel_ = CreateStatic(L"查询预设", SS_LEFT);
        quickScheme_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
            0, 0, 280, 300, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdQuickScheme)),
            instance_, nullptr);
        if (quickScheme_ != nullptr) {
            SendMessageW(quickScheme_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(L"普通查询（按当前条件）"));
            SendMessageW(quickScheme_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(L"IGS/ITRF 参考框架（按日期自动选择）"));
            SendMessageW(quickScheme_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(L"GIM/TEC 全球电离层建模"));
            SendMessageW(quickScheme_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(L"GIM 评估（全数据源·100站·≥50 km）"));
            SendMessageW(quickScheme_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(L"北斗 POD（全球均匀 60 站）"));
            SendMessageW(quickScheme_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(L"Network RTK 候选网（区域组网）"));
            SendMessageW(quickScheme_, CB_SETCURSEL, 0, 0);
            SendMessageW(quickScheme_, CB_SETMINVISIBLE, 8, 0);
        }
        advancedFilterButton_ = CreateButton(L"高级筛选…", IdAdvancedFilterButton);
        dateAdvancedFilterButton_ = CreateButton(
            L"日期高级筛选…", IdDateAdvancedFilterButton);
        // The query summary can contain one line per network and a long list
        // of retained dates.  A read-only multiline EDIT gives it native
        // vertical/horizontal scrolling instead of clipping the text in a
        // STATIC control when the summary grows.
        queryStatus_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"选择数据源和日期后，点击“查询测站”。",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY
                | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL
                | ES_NOHIDESEL,
            0, 0, 100, 80,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdQueryStatus)),
            instance_,
            nullptr);
        filterLabel_ = CreateStatic(
            L"快速筛选（站号、站网、国家或设备信息）",
            SS_LEFT);
        filterEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 100, 30,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdFilterEdit)),
            instance_,
            nullptr);
        SendMessageW(
            filterEdit_,
            EM_SETCUEBANNER,
            TRUE,
            reinterpret_cast<LPARAM>(
                L"例如 ZIM2、MGEX、CHN、2025079、DOMES、POLARX5、GAL"));

        scopeCountryLabel_ = CreateStatic(L"国家/地区", SS_LEFT);
        scopeCountry_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_COMBOBOXW,
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL
                | CBS_DROPDOWNLIST | CBS_HASSTRINGS | CBS_OWNERDRAWFIXED,
            0, 0, 130, 300,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdScopeCountry)),
            instance_,
            nullptr);
        SendMessageW(scopeCountry_, CB_SETDROPPEDWIDTH, 108, 0);
        SendMessageW(scopeCountry_, CB_SETMINVISIBLE, 8, 0);
        const auto allCountries = static_cast<int>(SendMessageW(
            scopeCountry_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"全部国家/地区")));
        SendMessageW(scopeCountry_, CB_SETITEMDATA, allCountries, -1);
        SendMessageW(scopeCountry_, CB_SETCURSEL, 0, 0);
        scopeLatitudeLabel_ = CreateStatic(L"纬度", SS_LEFT);
        scopeLatitudeMinimum_ = CreateScopeEdit(
            IdScopeLatitudeMinimum, L"最小");
        scopeLatitudeToLabel_ = CreateStatic(L"至", SS_CENTER);
        scopeLatitudeMaximum_ = CreateScopeEdit(
            IdScopeLatitudeMaximum, L"最大");
        scopeLongitudeLabel_ = CreateStatic(L"经度", SS_LEFT);
        scopeLongitudeMinimum_ = CreateScopeEdit(
            IdScopeLongitudeMinimum, L"最小");
        scopeLongitudeToLabel_ = CreateStatic(L"至", SS_CENTER);
        scopeLongitudeMaximum_ = CreateScopeEdit(
            IdScopeLongitudeMaximum, L"最大");
        applyScopeButton_ = CreateButton(L"应用范围", IdApplyScope);
        mapRectangleScopeButton_ = CreateButton(
            L"地图框选", IdMapRectangleScope);
        clearScopeButton_ = CreateButton(L"清除范围", IdClearScope);
        scopeStatus_ = CreateStatic(
            L"候选范围：请先查询当天测站。",
            SS_LEFT | SS_ENDELLIPSIS);

        stationList_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_LISTVIEWW,
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT
                | LVS_SINGLESEL | LVS_OWNERDATA,
            0, 0, 100, 100,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdStationList)),
            instance_,
            nullptr);
        ListView_SetExtendedListViewStyle(
            stationList_,
            LVS_EX_CHECKBOXES
                | LVS_EX_DOUBLEBUFFER
                | LVS_EX_FULLROWSELECT
                | LVS_EX_LABELTIP);
        SetWindowTheme(stationList_, L"Explorer", nullptr);
        AddListColumn(0, L"测站", 110);
        AddListColumn(1, L"站网", 110);
        AddListColumn(2, L"国家/地区", 92);
        AddListColumn(3, L"格式", 100);
        AddListColumn(4, L"间隔", 55);
        AddListColumn(5, L"接收机 / 系统", 190);
        AddListColumn(6, L"天线", 170);
        AddListColumn(7, L"范围完整性", 125);

        countryFlags_ = ImageList_Create(40, 24, ILC_COLOR24 | ILC_MASK, 250, 0);
        if (countryFlags_ != nullptr) {
            const auto bitmap = static_cast<HBITMAP>(LoadImageW(
                instance_, MAKEINTRESOURCEW(IDB_COUNTRY_FLAGS), IMAGE_BITMAP,
                0, 0, LR_CREATEDIBSECTION));
            if (bitmap != nullptr) {
                ImageList_AddMasked(countryFlags_, bitmap, RGB(255, 0, 255));
                DeleteObject(bitmap);
            }
        }

        selectionSummary_ = CreateStatic(L"已选择 0 个测站", SS_LEFT);
        selectAllButton_ = CreateButton(L"全选当前列表", IdSelectAll);
        clearSelectionButton_ = CreateButton(L"清空选择", IdClearSelection);
        uniformCountLabel_ = CreateStatic(L"数量", SS_LEFT);
        uniformCountEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"100",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP
                | ES_NUMBER | ES_CENTER | ES_AUTOHSCROLL,
            0, 0, 70, 30,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdUniformCount)),
            instance_,
            nullptr);
        SendMessageW(uniformCountEdit_, EM_SETLIMITTEXT, 6, 0);
        minimumDistanceLabel_ = CreateStatic(L"最小 km", SS_LEFT);
        minimumDistanceEdit_ = CreateScopeEdit(IdMinimumDistance, L"0=不限");
        SetWindowTextW(minimumDistanceEdit_, L"0");
        maximumNeighborDistanceLabel_ = CreateStatic(
            L"最大 km", SS_LEFT);
        maximumNeighborDistanceEdit_ = CreateScopeEdit(
            IdMaximumNeighborDistance, L"0=不限");
        SetWindowTextW(maximumNeighborDistanceEdit_, L"0");
        uniformSelectButton_ = CreateButton(
            L"在候选范围内均匀选站",
            IdUniformSelect);
        randomSelectButton_ = CreateButton(
            L"随机选站",
            IdRandomSelect);
        EnableWindow(uniformCountEdit_, FALSE);
        EnableWindow(minimumDistanceEdit_, FALSE);
        EnableWindow(maximumNeighborDistanceEdit_, FALSE);
        EnableWindow(uniformSelectButton_, FALSE);
        EnableWindow(randomSelectButton_, FALSE);
        for (const HWND control : {
                 scopeCountry_, scopeLatitudeMinimum_, scopeLatitudeMaximum_,
                 scopeLongitudeMinimum_, scopeLongitudeMaximum_,
                 applyScopeButton_, mapRectangleScopeButton_, clearScopeButton_}) {
            EnableWindow(control, FALSE);
        }
        folderLabel_ = CreateStatic(L"保存目录", SS_LEFT);
        folderEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            DownloadFolderState::Load(
                L"observation", DefaultDownloadFolder()).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 100, 30,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdFolderEdit)),
            instance_,
            nullptr);
        browseButton_ = CreateButton(L"浏览…", IdBrowseButton);
        downloadButton_ = CreateButton(L"开始下载", IdDownloadButton);
        EnableWindow(downloadButton_, FALSE);
        pauseDownloadButton_ = CreateButton(L"暂停下载", IdPauseDownloadButton);
        EnableWindow(pauseDownloadButton_, FALSE);

        progress_ = CreateWindowExW(
            0,
            PROGRESS_CLASSW,
            L"",
            WS_CHILD | WS_VISIBLE,
            0, 0, 100, 18,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdProgress)),
            instance_,
            nullptr);
        SendMessageW(progress_, PBM_SETRANGE32, 0, 1000);
        SetWindowTheme(progress_, L"Explorer", nullptr);
        operationStatus_ = CreateStatic(
            L"仅显示官方目录中该日期实际存在的文件。",
            SS_LEFT | SS_ENDELLIPSIS);
        querySummaryText_ = GetWindowTextString(queryStatus_);
        filterText_ = GetWindowTextString(filterEdit_);
        downloadFolder_ = GetWindowTextString(folderEdit_);
        operationStatusText_ = GetWindowTextString(operationStatus_);

        // The query summary is a read-only status surface, not an input
        // field.  Removing the legacy client edge lets the surrounding card
        // provide the visual boundary while retaining native scrolling for
        // long date/source diagnostics.
        if (queryStatus_ != nullptr) {
            SetWindowLongPtrW(queryStatus_, GWL_EXSTYLE, 0);
            SetWindowPos(queryStatus_, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                    | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }

        map_ = CreateMapView(window_, instance_, IdMap);
        preciseProductView_ = CreatePreciseProductView(
            window_, instance_, IdPreciseView);
        SYSTEMTIME storedPreciseStart{};
        SYSTEMTIME storedPreciseEnd{};
        if (preciseProductView_ != nullptr
            && LoadStoredDateRange(
                L"precise_dates", storedPreciseStart, storedPreciseEnd)) {
            SetPreciseDateRange(
                preciseProductView_, storedPreciseStart, storedPreciseEnd);
        }
        ShowWindow(preciseProductView_, SW_HIDE);

        if (title_ == nullptr || subtitle_ == nullptr
            || observationModeButton_ == nullptr
            || preciseModeButton_ == nullptr
            || selectKnowledgeBaseNetworksButton_ == nullptr
            || igsCheck_ == nullptr || mgexCheck_ == nullptr
            || ngsCheck_ == nullptr || epnCheck_ == nullptr
            || hkCheck_ == nullptr || gaGdcCheck_ == nullptr
            || rgnaCheck_ == nullptr || sonelCheck_ == nullptr
            || trigNetCheck_ == nullptr || cutCheck_ == nullptr
            || csnCheck_ == nullptr || eposIcelandCheck_ == nullptr
            || rbmcCheck_ == nullptr || ramsacCheck_ == nullptr
            || regnaRouCheck_ == nullptr || cacsCheck_ == nullptr
            || rgpCheck_ == nullptr || sopacGarnerCheck_ == nullptr
            || magnaEcoCheck_ == nullptr || panamaCorsCheck_ == nullptr
            || hondurasCorsCheck_ == nullptr || eposGnssCheck_ == nullptr
            || grefCheck_ == nullptr || osNetCheck_ == nullptr
            || ergnssCheck_ == nullptr || renepCheck_ == nullptr
            || geoNetNzCheck_ == nullptr
            || ringCheck_ == nullptr || aposCheck_ == nullptr
            || robBelgiumCheck_ == nullptr || dpgaCheck_ == nullptr
            || catNetCheck_ == nullptr || spsLuxCheck_ == nullptr
            || kasiNgiiCheck_ == nullptr || renagCheck_ == nullptr
            || nsgiCheck_ == nullptr || noaGnssCheck_ == nullptr
            || redgaeCheck_ == nullptr || geoNetJapanCheck_ == nullptr
            || dlabCheck_ == nullptr || magnetCheck_ == nullptr
            || pangaCwuCheck_ == nullptr
            || datePicker_ == nullptr || dateRangeToLabel_ == nullptr
            || endDatePicker_ == nullptr
            || quickSchemeLabel_ == nullptr || quickScheme_ == nullptr
            || networkPanelToggleButton_ == nullptr
            || queryButton_ == nullptr || advancedFilterButton_ == nullptr
            || dateAdvancedFilterButton_ == nullptr
            || stationList_ == nullptr
            || scopeCountry_ == nullptr
            || scopeLatitudeMinimum_ == nullptr
            || scopeLatitudeMaximum_ == nullptr
            || scopeLongitudeMinimum_ == nullptr
            || scopeLongitudeMaximum_ == nullptr
            || applyScopeButton_ == nullptr
            || mapRectangleScopeButton_ == nullptr
            || clearScopeButton_ == nullptr
            || uniformCountEdit_ == nullptr
            || minimumDistanceEdit_ == nullptr
            || maximumNeighborDistanceEdit_ == nullptr
            || uniformSelectButton_ == nullptr
            || randomSelectButton_ == nullptr
            || downloadButton_ == nullptr || pauseDownloadButton_ == nullptr
            || map_ == nullptr
            || preciseProductView_ == nullptr) {
            return false;
        }

        // Use the current Windows visual style for common controls.  This is
        // deliberately limited to controls whose rendering is entirely
        // native; the network checkboxes and country combo have custom colour
        // or owner-draw handlers and therefore keep their existing painting.
        const auto applyNativeTheme = [](const HWND control) {
            if (control != nullptr) {
                SetWindowTheme(control, L"Explorer", nullptr);
            }
        };
        for (const HWND control : {
                 observationModeButton_, preciseModeButton_,
                 networkPanelToggleButton_, selectKnowledgeBaseNetworksButton_,
                 datePicker_, endDatePicker_, quickScheme_, queryButton_,
                 advancedFilterButton_, dateAdvancedFilterButton_,
                 queryStatus_, filterEdit_, stationList_,
                 scopeLatitudeMinimum_, scopeLatitudeMaximum_,
                 scopeLongitudeMinimum_, scopeLongitudeMaximum_,
                 applyScopeButton_, mapRectangleScopeButton_, clearScopeButton_,
                 uniformCountEdit_, minimumDistanceEdit_,
                 maximumNeighborDistanceEdit_, uniformSelectButton_,
                 randomSelectButton_, folderEdit_, browseButton_,
                 downloadButton_, pauseDownloadButton_, progress_}) {
            applyNativeTheme(control);
        }

        ApplyFont(title_, titleFont_);
        ApplyFont(subtitle_, smallFont_);
        for (const HWND control : {
                 observationModeButton_, preciseModeButton_,
                 networkLabel_, networkPanelToggleButton_,
                 selectKnowledgeBaseNetworksButton_,
                 igsCheck_, mgexCheck_, ngsCheck_,
                 epnCheck_, hkCheck_, gaGdcCheck_, rgnaCheck_,
                 sonelCheck_, trigNetCheck_, cutCheck_, csnCheck_,
                 eposIcelandCheck_, rbmcCheck_, ramsacCheck_,
                 regnaRouCheck_, cacsCheck_, rgpCheck_,
                 sopacGarnerCheck_, magnaEcoCheck_,
                 panamaCorsCheck_, hondurasCorsCheck_, eposGnssCheck_,
                 grefCheck_, osNetCheck_, ergnssCheck_, renepCheck_,
                 geoNetNzCheck_,
                 ringCheck_, aposCheck_, robBelgiumCheck_, dpgaCheck_,
                 catNetCheck_, spsLuxCheck_, kasiNgiiCheck_, renagCheck_,
                 nsgiCheck_, noaGnssCheck_, redgaeCheck_, geoNetJapanCheck_,
                  dlabCheck_, magnetCheck_, pangaCwuCheck_,
                 dateLabel_, datePicker_, dateRangeToLabel_,
                 endDatePicker_, queryButton_, advancedFilterButton_,
                 quickSchemeLabel_, quickScheme_,
                 dateAdvancedFilterButton_, queryStatus_,
                 filterLabel_, filterEdit_, stationList_, selectionSummary_,
                 selectAllButton_, clearSelectionButton_,
                 scopeCountryLabel_, scopeCountry_, scopeLatitudeLabel_,
                 scopeLatitudeMinimum_, scopeLatitudeToLabel_,
                 scopeLatitudeMaximum_, scopeLongitudeLabel_,
                 scopeLongitudeMinimum_, scopeLongitudeToLabel_,
                 scopeLongitudeMaximum_, applyScopeButton_,
                 mapRectangleScopeButton_, clearScopeButton_, scopeStatus_,
                 uniformCountLabel_, uniformCountEdit_,
                 minimumDistanceLabel_, minimumDistanceEdit_,
                  maximumNeighborDistanceLabel_, maximumNeighborDistanceEdit_,
                  uniformSelectButton_, randomSelectButton_,
                 folderLabel_,
                 folderEdit_, browseButton_, downloadButton_,
                 pauseDownloadButton_, operationStatus_}) {
            ApplyFont(control, bodyFont_);
        }
        ApplyFont(subtitle_, smallFont_);
        ApplyFont(queryStatus_, smallFont_);
        ApplyFont(dateAdvancedFilterButton_, bodyFont_);
        ApplyFont(scopeStatus_, smallFont_);
        ApplyFont(operationStatus_, smallFont_);
        // Keep the network grid at the same readable size as the original
        // interface.  Long names are handled by their fixed grid cells.
        for (const auto& [control, network] : NetworkControls()) {
            (void)network;
            ApplyFont(control, bodyFont_);
        }
        for (const HWND control : {
                 networkLabel_, dateLabel_, dateRangeToLabel_, filterLabel_, folderLabel_,
                 scopeCountryLabel_, scopeLatitudeLabel_, scopeLatitudeToLabel_,
                 scopeLongitudeLabel_, scopeLongitudeToLabel_,
                 selectionSummary_}) {
            ApplyFont(control, bodyFont_);
        }

        RefreshKnowledgeBaseStatus();
        PreloadObservationSearchIndex();
        UpdateObservationPeriodMode();
        UpdateAdvancedFilterButtonLabel();
        UpdateDateAdvancedFilterButtonLabel();
        ApplyNetworkPanelVisibility();
        UpdateNetworkPanelToggleLabel();

        // Capture every native child before WebView2 creates its own child
        // hierarchy.  These controls remain alive as the proven query and
        // download backend, but must never paint on top of the web workbench.
        nativeControls_.clear();
        EnumChildWindows(
            window_,
            [](const HWND child, const LPARAM parameter) -> BOOL {
                auto* controls = reinterpret_cast<std::vector<HWND>*>(parameter);
                if (controls != nullptr) controls->push_back(child);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&nativeControls_));

        // The WebView2 workbench is the sole product UI.  The remaining
        // native children are headless compatibility adapters while their
        // business state is migrated; they are never presented as a fallback.
        webUiStarting_ = true;
        // CreateWindow returns before the main window is shown.  Hide the
        // captured children now, while the parent is still off-screen, so no
        // legacy frame can be exposed during asynchronous WebView creation.
        for (const HWND control : nativeControls_) {
            if (control != nullptr && IsWindow(control)) {
                ShowWindow(control, SW_HIDE);
            }
        }
        webUi_ = std::make_unique<WebWorkbenchHost>(
            window_,
            [this](const std::wstring& message) {
                HandleWebUiMessage(message);
            },
            [this]() {
                Layout();
                PostWebUiState();
                // The first styled frame can arrive before NavigationCompleted.
                // Remember it so startup can finish when both signals are ready.
                if (webUiFirstFrameReady_) CompleteWebStartup();
            },
            [this]() {
                // The Web workbench is now the only user interface.  Do not
                // revive the retired Win32 control tree when WebView2 fails:
                // that would bring back the startup flash and leave users in
                // a different application surface with different behavior.
                webUiStarting_ = false;
                KillTimer(window_, kWebUiLoadingTimer);
                NativeUiLanguage::ShowMessageBox(
                    window_,
                    L"新版工作台初始化失败。请确认 Microsoft Edge WebView2 "
                    L"Runtime 与程序 resources 目录完整。",
                    kApplicationTitle,
                    MB_OK | MB_ICONERROR);
                PostMessageW(window_, WM_CLOSE, 0, 0);
            });
        if (!webUi_->Initialize()) {
            webUiStarting_ = false;
            webUi_.reset();
            NativeUiLanguage::ShowMessageBox(
                window_,
                L"无法启动新版工作台。请确认 Microsoft Edge WebView2 "
                L"Runtime 已安装。",
                kApplicationTitle,
                MB_OK | MB_ICONERROR);
            return false;
        }
        // Show a lightweight native startup surface on the next message-loop
        // turn.  It covers the unavoidable interval before WebView2 paints
        // its first frame without exposing a blank white host or legacy UI.
        PostMessageW(window_, WM_IGS_SHOW_WEB_LOADING, 0, 0);

        return true;
    }

    void RevealWebWindow() {
        if (windowRevealed_) return;
        windowRevealed_ = true;
        ShowWindow(window_, initialShowCommand_);
        UpdateWindow(window_);
    }

    void BeginWebLoadingPresentation() {
        if (!webUiStarting_) {
            RevealWebWindow();
            return;
        }
        loadingAnimationTick_ = 0;
        SetTimer(window_, kWebUiLoadingTimer, 80, nullptr);
        RevealWebWindow();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void AdvanceWebLoadingAnimation() {
        if (!webUiStarting_) {
            KillTimer(window_, kWebUiLoadingTimer);
            return;
        }
        ++loadingAnimationTick_;
        // Only the moving progress segment changes between frames. Repainting
        // the entire native card at 12.5 Hz makes its white surface flash on
        // some Windows compositors during WebView2 initialization.
        RECT client{};
        GetClientRect(window_, &client);
        const int width = std::max(1, static_cast<int>(client.right));
        const int height = std::max(1, static_cast<int>(client.bottom));
        const int cardWidth = std::min(480, std::max(320, width - 48));
        const int cardTop = std::max(24, (height - 300) / 2);
        const int trackWidth = std::min(260, cardWidth - 90);
        const int trackLeft = (width - trackWidth) / 2;
        const int trackTop = cardTop + 253;
        const RECT progressRect{trackLeft - 2, trackTop - 2,
            trackLeft + trackWidth + 2, trackTop + 8};
        InvalidateRect(window_, &progressRect, FALSE);
        // Defensive fallback: a damaged JavaScript bridge must not strand the
        // user on the loader forever after the document itself is ready.
        if (loadingAnimationTick_ >= 150 && webUi_ && webUi_->IsReady()) {
            CompleteWebStartup();
        }
    }

    void CompleteWebStartup() {
        if (!webUiStarting_ || !webUi_ || !webUi_->IsReady()) return;
        webUiStarting_ = false;
        KillTimer(window_, kWebUiLoadingTimer);
        Layout();
        // Clear the native startup pixels before the WebView covers them.
        // Its compositor can briefly expose the parent surface during resize
        // or a page switch; that surface must no longer contain the card.
        RedrawWindow(window_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_NOCHILDREN);
        webUi_->SetVisible(true);
        RevealWebWindow();
        webUi_->Focus();
    }

    [[nodiscard]] bool IsStartupLoading() const noexcept {
        return webUiStarting_;
    }

    void SetOperationStatusText(const std::wstring& text) {
        operationStatusText_ = text;
        if (operationStatus_ != nullptr) {
            SetWindowTextW(operationStatus_, text.c_str());
        }
    }

    void SetQuerySummaryText(const std::wstring& text) {
        querySummaryText_ = text;
        if (queryStatus_ != nullptr) {
            SetWindowTextW(queryStatus_, text.c_str());
        }
    }

    void SetFilterText(const std::wstring& text) {
        filterText_ = text;
        if (filterEdit_ != nullptr) {
            SetWindowTextW(filterEdit_, text.c_str());
        }
    }

    void SetDownloadFolder(const std::wstring& folder) {
        downloadFolder_ = folder;
        if (folderEdit_ != nullptr) {
            SetWindowTextW(folderEdit_, folder.c_str());
        }
        if (!DownloadFolderState::Save(L"observation", folder)) {
            NativeUiLanguage::ShowMessageBox(window_, L"保存目录已更改，但无法记住到下次启动。",
                kApplicationTitle, MB_OK | MB_ICONWARNING);
        }
        PostWebUiState();
    }

    void HandleDownloadFolderEdit(const bool commit) {
        downloadFolder_ = GetWindowTextString(folderEdit_);
        if (commit && !downloadFolder_.empty()) {
            DownloadFolderState::Save(L"observation", downloadFolder_);
        }
        PostWebUiState();
    }

    void OpenDownloadFolder(const bool precise) {
        std::wstring folder = downloadFolder_;
        if (precise) {
            PreciseUiSnapshot snapshot{};
            if (!QueryPreciseProductSnapshot(preciseProductView_, snapshot)) {
                NativeUiLanguage::ShowMessageBox(window_, L"无法读取精密产品保存目录。",
                    kApplicationTitle, MB_OK | MB_ICONERROR);
                return;
            }
            folder = std::move(snapshot.folder);
        }
        if (folder.empty()) {
            NativeUiLanguage::ShowMessageBox(window_, L"请先选择保存目录。", kApplicationTitle,
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        const std::filesystem::path path(folder);
        std::error_code error;
        std::filesystem::create_directories(path, error);
        if (error || !std::filesystem::is_directory(path, error) || error) {
            NativeUiLanguage::ShowMessageBox(window_, L"无法创建或打开保存目录。", kApplicationTitle,
                MB_OK | MB_ICONERROR);
            return;
        }
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open",
                path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
            NativeUiLanguage::ShowMessageBox(window_, L"无法在文件资源管理器中打开保存目录。",
                kApplicationTitle, MB_OK | MB_ICONERROR);
        }
    }

    void SetQueryDates(const SYSTEMTIME& start, const SYSTEMTIME& end) {
        queryStartDate_ = start;
        queryEndDate_ = end;
        if (datePicker_ != nullptr) {
            DateTime_SetSystemtime(datePicker_, GDT_VALID, &queryStartDate_);
        }
        if (endDatePicker_ != nullptr) {
            DateTime_SetSystemtime(endDatePicker_, GDT_VALID, &queryEndDate_);
        }
    }

    void HandleWebUiMessage(const std::wstring& message) {
        const auto getJsonString = [&message](const std::wstring_view key) {
            const std::wstring marker = L"\"" + std::wstring(key) + L"\":\"";
            const auto start = message.find(marker);
            if (start == std::wstring::npos) return std::wstring{};
            const auto valueStart = start + marker.size();
            const auto valueEnd = message.find(L'\"', valueStart);
            return valueEnd == std::wstring::npos
                ? std::wstring{}
                : message.substr(valueStart, valueEnd - valueStart);
        };
        if (message.find(L"\"type\":\"download-settings\"") != std::wstring::npos) {
            PreciseUiSnapshot snapshot{};
            QueryPreciseProductSnapshot(preciseProductView_, snapshot);
            if (downloading_ || observationQueueRunning_ || postprocessBusy_ || snapshot.busy) {
                PostWebUiState(); return;
            }
            const auto layout = getJsonString(L"layout");
            const auto ask = getJsonString(L"postprocess");
            if ((layout != L"date" && layout != L"flat") || (ask != L"ask" && ask != L"off")) return;
            const DownloadSettings::Options options{layout == L"date", ask == L"ask"};
            if (!DownloadSettings::Save(options)) {
                NativeUiLanguage::ShowMessageBox(window_, L"无法保存下载设置。请检查用户配置目录的写入权限。", kApplicationTitle, MB_OK | MB_ICONWARNING);
            }
            PostWebUiState(); return;
        }
        if (message.find(L"\"type\":\"ui-language\"") != std::wstring::npos
            || message.find(L"\"type\":\"ready\"") != std::wstring::npos) {
            NativeUiLanguage::SetLanguage(getJsonString(L"language"));
        }
        if (message.find(L"\"type\":\"ui-language\"") != std::wstring::npos) {
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"first-frame-ready\"")
            != std::wstring::npos) {
            webUiFirstFrameReady_ = true;
            CompleteWebStartup();
            return;
        }
        if (message.find(L"\"type\":\"ready\"") != std::wstring::npos) {
            // The native index/status workers may have changed while the Web
            // document was loading. Run one definitive layout pass and send
            // the authoritative state into the fresh JavaScript document.
            Layout();
            // A navigation creates a fresh JavaScript document.  Force the
            // large station layer to be sent once even when the native data
            // itself has not changed since the previous document.
            webLastMapDataRevisionSent_ = 0;
            webLastMapSelectionSignatureSent_ = 0;
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"mode\"") != std::wstring::npos) {
            const bool precise = message.find(L"\"mode\":\"precise\"")
                != std::wstring::npos;
            SetPreciseMode(precise);
            if (webUi_) webUi_->Focus();
            return;
        }
        if (message.find(L"\"type\":\"query-dates\"")
            != std::wstring::npos) {
            SYSTEMTIME startDate{};
            SYSTEMTIME endDate{};
            if (ParseSystemDate(getJsonString(L"start"), startDate)
                && ParseSystemDate(getJsonString(L"end"), endDate)) {
                SetQueryDates(startDate, endDate);
            }
            return;
        }
        if (message.find(L"\"type\":\"query\"") != std::wstring::npos) {
            webRowOffset_ = 0;
            SYSTEMTIME startDate{};
            SYSTEMTIME endDate{};
            const auto startText = getJsonString(L"start");
            const auto endText = getJsonString(L"end");
            if (!ParseSystemDate(startText, startDate)
                || !ParseSystemDate(endText, endDate)) {
                NativeUiLanguage::ShowMessageBox(window_, L"请选择有效的起止日期。",
                    kApplicationTitle, MB_OK | MB_ICONWARNING);
                return;
            }
            SetQueryDates(startDate, endDate);
            StartQuery();
            return;
        }
        if (message.find(L"\"type\":\"observation-preset\"")
            != std::wstring::npos) {
            webRowOffset_ = 0;
            const auto value = getJsonString(L"value");
            const int preset = value == L"reference" ? 1
                : value == L"gim" ? 2
                : value == L"gim-evaluation" ? 3
                : value == L"pod" ? 4
                : value == L"network-rtk" ? 5 : 0;
            SendMessageW(quickScheme_, CB_SETCURSEL, preset, 0);
            ApplyQuickScheme();
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"products\"") != std::wstring::npos) {
            // The web query button is the single commit point for all
            // precision-product filters.  Keep the filter values in the
            // same message as the date range so changing a control never
            // triggers a partial/stale search.
            SetPreciseQuickPreset(preciseProductView_, 0);
            if (message.find(L"\"family\"") != std::wstring::npos) {
                const auto family = getJsonString(L"family");
                SetPreciseProductFamily(preciseProductView_, family.c_str());
            }
            if (message.find(L"\"center\"") != std::wstring::npos) {
                const auto center = getJsonString(L"center");
                SetPreciseAnalysisCenter(preciseProductView_, center.c_str());
            }
            if (message.find(L"\"latency\"") != std::wstring::npos
                || message.find(L"\"completenessMode\"")
                    != std::wstring::npos
                || message.find(L"\"search\"")
                    != std::wstring::npos) {
                const auto latency = getJsonString(L"latency");
                const auto completeness = getJsonString(L"completeness");
                const auto completenessMode =
                    getJsonString(L"completenessMode");
                double completenessValue = 0.0;
                ParseDialogDouble(
                    getJsonString(L"completenessValue"), 0.0, 100000.0,
                    completenessValue);
                const auto search = getJsonString(L"search");
                SetPreciseFilters(
                    preciseProductView_, latency.c_str(),
                    completeness.c_str(), L"", search.c_str(),
                    completenessMode.c_str(),
                    completenessValue);
            }
            SYSTEMTIME startDate{};
            SYSTEMTIME endDate{};
            const auto startText = getJsonString(L"start");
            const auto endText = getJsonString(L"end");
            if (ParseSystemDate(startText, startDate)
                && ParseSystemDate(endText, endDate)) {
                SetPreciseDateRange(preciseProductView_, startDate, endDate);
                SaveStoredDateRange(
                    L"precise_dates", startDate, endDate);
            }
            TriggerPreciseSearch(preciseProductView_);
            return;
        }
        if (message.find(L"\"type\":\"download-products\"")
            != std::wstring::npos) {
            TriggerPreciseDownload(preciseProductView_);
            return;
        }
        if (message.find(L"\"type\":\"select-product\"")
            != std::wstring::npos) {
            const auto productId = getJsonString(L"product");
            const bool checked = message.find(L"\"checked\":true")
                != std::wstring::npos;
            SelectPreciseProduct(
                preciseProductView_, productId.c_str(), checked);
            return;
        }
        if (message.find(L"\"type\":\"center\"") != std::wstring::npos) {
            const auto center = getJsonString(L"value");
            SetPreciseAnalysisCenter(preciseProductView_, center.c_str());
            return;
        }
        if (message.find(L"\"type\":\"product-family\"")
            != std::wstring::npos) {
            const auto family = getJsonString(L"value");
            SetPreciseProductFamily(preciseProductView_, family.c_str());
            return;
        }
        if (message.find(L"\"type\":\"precise-dates\"")
            != std::wstring::npos) {
            SYSTEMTIME startDate{};
            SYSTEMTIME endDate{};
            if (ParseSystemDate(getJsonString(L"start"), startDate)
                && ParseSystemDate(getJsonString(L"end"), endDate)) {
                SetPreciseDateRange(preciseProductView_, startDate, endDate);
                SaveStoredDateRange(
                    L"precise_dates", startDate, endDate);
                PostWebUiState();
            }
            return;
        }
        if (message.find(L"\"type\":\"precise-filters\"")
            != std::wstring::npos) {
            const auto latency = getJsonString(L"latency");
            const auto completeness = getJsonString(L"completeness");
            const auto completenessMode = getJsonString(L"completenessMode");
            double completenessValue = 0.0;
            ParseDialogDouble(
                getJsonString(L"completenessValue"), 0.0, 100000.0,
                completenessValue);
            const auto troStation = getJsonString(L"troStation");
            const auto search = getJsonString(L"search");
            SetPreciseFilters(
                preciseProductView_, latency.c_str(), completeness.c_str(),
                troStation.c_str(), search.c_str(),
                completenessMode.c_str(), completenessValue);
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"precise-select-all\"")
            != std::wstring::npos) {
            SelectAllVisiblePreciseProducts(preciseProductView_, true);
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"precise-clear\"")
            != std::wstring::npos) {
            SelectAllVisiblePreciseProducts(preciseProductView_, false);
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"precise-browse\"")
            != std::wstring::npos) {
            BrowsePreciseProductFolder(preciseProductView_);
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"open-project-repository\"") != std::wstring::npos) {
            ShellExecuteW(window_, L"open", L"https://github.com/qiliu2025/MARK",
                nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        if (message.find(L"\"type\":\"open-folder\"")
            != std::wstring::npos) {
            OpenDownloadFolder(getJsonString(L"mode") == L"precise");
            return;
        }
        if (message.find(L"\"type\":\"precise-preset\"")
            != std::wstring::npos) {
            const auto value = getJsonString(L"value");
            const int preset = value == L"wum" ? 1
                : value == L"upd" ? 2
                : value == L"network-rtk" ? 3
                : value == L"auto-ppp" ? 4 : 0;
            SetPreciseQuickPreset(preciseProductView_, preset);
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"precise-custom-preset\"")
            != std::wstring::npos) {
            const auto name = getJsonString(L"name");
            const auto rules = getJsonString(L"rules");
            SetPreciseCustomPreset(
                preciseProductView_, name.c_str(), rules.c_str());
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"precise-apply-preset\"")
            != std::wstring::npos) {
            ApplyPreciseQuickPreset(preciseProductView_);
            return;
        }
        if (message.find(L"\"type\":\"add-download-plan\"")
            != std::wstring::npos) {
            if (getJsonString(L"mode") == L"precise") {
                AddSelectedPreciseProductsToPlan(preciseProductView_);
            } else {
                AddSelectedStationsToDownloadPlan();
            }
            return;
        }
        if (message.find(L"\"type\":\"start-download-plan\"")
            != std::wstring::npos) {
            if (postprocessBusy_) return;
            if (getJsonString(L"mode") == L"precise") {
                StartPreciseDownloadPlan(preciseProductView_);
            } else {
                StartObservationDownloadPlan();
            }
            return;
        }
        if (message.find(L"\"type\":\"download-plan-item\"")
            != std::wstring::npos) {
            const auto idText = getJsonString(L"id");
            const auto id = _wcstoui64(idText.c_str(), nullptr, 10);
            const auto action = getJsonString(L"action");
            if (getJsonString(L"mode") == L"precise") {
                UpdatePreciseDownloadPlanItem(
                    preciseProductView_, id, action.c_str());
            } else {
                UpdateObservationDownloadPlanItem(id, action);
            }
            return;
        }
        if (message.find(L"\"type\":\"download\"") != std::wstring::npos) {
            if (preciseMode_) {
                TriggerPreciseDownload(preciseProductView_);
            } else {
                StartDownload();
            }
            return;
        }
        if (message.find(L"\"type\":\"request-advanced-choices\"")
            != std::wstring::npos) {
            PostWebAdvancedChoices();
            return;
        }
        if (message.find(L"\"type\":\"advanced\"") != std::wstring::npos) {
            // Compatibility hook for older HTML resources.  Scheme C edits
            // and submits the same state through apply-advanced so the
            // legacy Win32 dialog never has to cover the new workbench.
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"apply-advanced\"")
            != std::wstring::npos) {
            if (querying_ || siteLogLoading_) return;
            AdvancedFilterState candidate = advancedFilter_;
            const auto completenessMode = getJsonString(L"completenessMode");
            candidate.completenessMode = completenessMode == L"ratio"
                ? AdvancedFilterState::CompletenessMode::RatioAtLeast
                : completenessMode == L"days"
                    ? AdvancedFilterState::CompletenessMode::DaysAtLeast
                    : AdvancedFilterState::CompletenessMode::Ignore;
            const auto dailyMode = getJsonString(L"dailyMode");
            candidate.dailyCoverageMode = dailyMode == L"stations"
                ? DailyCoverageMode::StationsAtLeast
                : dailyMode == L"percent"
                    ? DailyCoverageMode::PercentAtLeast
                    : DailyCoverageMode::Ignore;
            candidate.observationCompletenessEnabled =
                getJsonString(L"observationCompletenessMode") == L"minimum";
            candidate.latencyEnabled =
                getJsonString(L"latencyMode") == L"maximum";
            candidate.positionTimeSeriesEnabled =
                getJsonString(L"positionTimeSeriesMode") == L"maximum";
            candidate.residualsEnabled =
                getJsonString(L"residualsMode") == L"maximum";
            candidate.meanMultipathEnabled =
                getJsonString(L"multipathMode") == L"maximum";
            candidate.cycleSlipsEnabled =
                getJsonString(L"cycleSlipsMode") == L"maximum";
            candidate.observationCompletenessSystem = WideToUtf8(getJsonString(L"observationCompletenessSystem"));
            candidate.multipathSystem = WideToUtf8(getJsonString(L"multipathSystem"));
            candidate.cycleSlipsSystem = WideToUtf8(getJsonString(L"cycleSlipsSystem"));
            if (!ParseDialogDouble(
                    getJsonString(L"completenessPercent"), 0.0, 100.0,
                    candidate.minimumCompletenessPercent)
                || !ParseDialogInt(
                    getJsonString(L"minimumDays"), 0, 100000,
                    candidate.minimumAvailableDays)
                || !ParseDialogInt(
                    getJsonString(L"dailyStations"), 0, 1000000,
                    candidate.minimumDailyStations)
                || !ParseDialogDouble(
                    getJsonString(L"dailyPercent"), 0.0, 100.0,
                    candidate.minimumDailyStationPercent)
                || !ParseDialogDouble(
                    getJsonString(L"observationCompletenessPercent"),
                    0.0, 100.0,
                    candidate.minimumObservationCompletenessPercent)
                || !ParseDialogDouble(
                    getJsonString(L"maximumMeanLatency"), 0.0, 100000.0,
                    candidate.maximumMeanLatencyHours)
                || !ParseDialogDouble(
                    getJsonString(L"maximumMeanDetrendedPosition"),
                    0.0, 1000000.0,
                    candidate.maximumMeanAbsoluteDetrendedPositionMillimeters)
                || !ParseDialogDouble(
                    getJsonString(L"maximumMeanResidual"),
                    0.0, 1000000.0,
                    candidate.maximumMeanAbsoluteResidualMillimeters)
                || !ParseDialogDouble(
                    getJsonString(L"maximumMeanMultipath"), 0.0, 100.0,
                    candidate.maximumMeanMultipathCentimeters)
                || !ParseDialogDouble(
                    getJsonString(L"maximumCycleSlips"), 0.0, 1000000.0,
                    candidate.maximumMeanDailyCycleSlips)) {
                SetOperationStatusText(
                    L"高级筛选未应用：请检查可用性与观测质量阈值。"
                    L"比例应为0–100，其余六类质量上限不得为负数。");
                PostWebUiState();
                return;
            }
            const auto sample = ToUpperWide(getJsonString(L"sampleInterval"));
            candidate.sampleInterval = sample == L"30S" || sample == L"1S"
                    || sample == L"5S" || sample == L"15S"
                    || sample == L"60S"
                ? WideToUtf8(sample) : std::string{};
            const auto rinex = getJsonString(L"rinexVersion");
            candidate.rinexVersion = rinex == L"2" ? 2
                : rinex == L"3" ? 3 : 0;
            candidate.systemsInclude = WideToUtf8(
                getJsonString(L"systemsInclude"));
            candidate.systemsExclude = WideToUtf8(
                getJsonString(L"systemsExclude"));
            candidate.receiverInclude = WideToUtf8(
                getJsonString(L"receiverInclude"));
            candidate.receiverExclude = WideToUtf8(
                getJsonString(L"receiverExclude"));
            candidate.antennaInclude = WideToUtf8(
                getJsonString(L"antennaInclude"));
            candidate.antennaExclude = WideToUtf8(
                getJsonString(L"antennaExclude"));
            candidate.signalCodesInclude = WideToUtf8(
                getJsonString(L"signalCodesInclude"));
            candidate.signalCodesExclude = WideToUtf8(
                getJsonString(L"signalCodesExclude"));
            const auto gimMode = getJsonString(L"gimStationMode");
            candidate.gimStationMode = gimMode == L"keep"
                ? "keep" : gimMode == L"exclude" ? "exclude" : "ignore";
            candidate.gimProviders = WideToUtf8(
                getJsonString(L"gimProviders"));
            candidate.gimProductGrades = WideToUtf8(
                getJsonString(L"gimProductGrades"));
            const auto referenceFrame = ParseReferenceFrameSelection(
                getJsonString(L"referenceFrame"));
            advancedFilter_ = std::move(candidate);
            quickFrameSelection_ = referenceFrame;
            webRowOffset_ = 0;
            UpdateAdvancedFilterButtonLabel();
            SetOperationStatusText(
                L"高级测站筛选已应用，正在按新条件刷新结果…");
            if (!stations_.empty()) {
                StartQuery();
            } else {
                ApplyAdvancedFilterToScope();
                PopulateStationList();
                MapViewRefresh(map_);
                PostWebUiState();
            }
            return;
        }
        if (message.find(L"\"type\":\"source\"") != std::wstring::npos) {
            webRowOffset_ = 0;
            const auto source = getJsonString(L"value");
            HWND matched = nullptr;
            ObservationNetwork matchedNetwork = ObservationNetwork::Igs;
            for (const auto& [control, network] : NetworkControls()) {
                const auto label = GetWindowTextString(control);
                if (!source.empty()
                    && (ToUpperWide(label) == ToUpperWide(source)
                        || (source == L"IGS" && network == ObservationNetwork::Igs))) {
                    matched = control;
                    matchedNetwork = network;
                    break;
                }
            }
            if (matched != nullptr) {
                const auto bit = NetworkBit(matchedNetwork);
                selectedNetworkMask_ ^= bit;
                SendMessageW(
                    matched,
                    BM_SETCHECK,
                    HasNetwork(selectedNetworkMask_, matchedNetwork)
                        ? BST_CHECKED : BST_UNCHECKED,
                    0);
                networkSelectionTouched_ = true;
                if (knowledgeBaseTogglePending_) {
                    selectionChangedWhileTogglePending_ = true;
                }
                UpdateKnowledgeBaseToggleLabel();
                PostWebUiState();
            } else {
                SetOperationStatusText(L"未找到对应的数据源。");
                PostWebUiState();
            }
            return;
        }
        if (message.find(L"\"type\":\"source-toggle-available\"")
            != std::wstring::npos) {
            webRowOffset_ = 0;
            SelectAllKnowledgeBaseNetworks();
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"filter\"") != std::wstring::npos) {
            if (!querying_) {
                webRowOffset_ = 0;
                SetFilterText(getJsonString(L"value"));
                ApplyFilter();
                PostWebUiState();
            }
            return;
        }
        if (message.find(L"\"type\":\"station-detail\"") != std::wstring::npos) {
            PostWebStationDetail(getJsonString(L"station"), getJsonString(L"request"));
            return;
        }
        if (message.find(L"\"type\":\"product-coverage\"") != std::wstring::npos) {
            PreciseUiCoverageSnapshot detail;
            detail.id = getJsonString(L"product");
            detail.family = getJsonString(L"family");
            QueryPreciseProductCoverage(preciseProductView_, detail);
            std::wstring updJson = L"null";
            if (detail.updInputFiles.size() == 5) {
                constexpr const wchar_t* types[] = {L"SP3", L"CLK", L"DCB", L"ANTEX", L"SINEX"};
                std::wstring inputs = L"[";
                for (std::size_t i = 0; i < 5; ++i) {
                    if (i) inputs += L',';
                    inputs += std::format(L"{{\"type\":\"{}\",\"fileName\":\"{}\"}}",
                        types[i], JsonEscape(detail.updInputFiles[i]));
                }
                inputs += L']';
                updJson = std::format(L"{{\"date\":\"{}\",\"fileName\":\"{}\",\"inputs\":{}}}",
                    JsonEscape(detail.updDate), JsonEscape(detail.updFile), inputs);
            }
            std::wstring filesJson = L"[";
            for (const auto& file : detail.files) {
                if (filesJson.size() > 1) filesJson += L',';
                filesJson += L"\"" + JsonEscape(file) + L"\"";
            }
            filesJson += L']';
            std::wstring recordsJson=L"[";
            for(const auto& record:detail.updRecords){
                if(recordsJson.size()>1)recordsJson+=L',';
                recordsJson+=std::format(L"{{\"date\":\"{}\",\"id\":\"{}\",\"fileName\":\"{}\",\"dcbFileName\":\"{}\",\"flagged\":{},\"selected\":{}}}",
                    FormatDate(record.date),JsonEscape(record.logicalKey),JsonEscape(record.fileName),JsonEscape(record.dcbFileName),
                    record.flagged?L"true":L"false",record.selected?L"true":L"false");
            }
            recordsJson+=L']';
            if (webUi_) webUi_->PostJson(std::format(
                L"{{\"type\":\"product-coverage\",\"product\":\"{}\",\"request\":\"{}\",\"coverage\":{},\"antex\":{},\"upd\":{},\"files\":{},\"updRecords\":{},\"error\":\"{}\"}}",
                JsonEscape(detail.id), JsonEscape(getJsonString(L"request")), detail.json, detail.antexJson, updJson, filesJson, recordsJson, JsonEscape(detail.error)));
            return;
        }
        if (message.find(L"\"type\":\"sort-stations\"")
            != std::wstring::npos) {
            webSortAscending_ = getJsonString(L"direction") != L"desc";
            webSortByCompleteness_ = getJsonString(L"column") == L"completeness";
            webRowOffset_ = 0;
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"rows-selected-only\"")
            != std::wstring::npos) {
            webRowsSelectedOnly_ = message.find(L"\"enabled\":true")
                != std::wstring::npos;
            webRowOffset_ = 0;
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"countries\"") != std::wstring::npos) {
            webRowOffset_ = 0;
            webSelectedCountryCodes_.clear();
            const auto value = getJsonString(L"value");
            std::size_t start = 0;
            while (start <= value.size()) {
                const auto end = value.find(L',', start);
                auto token = value.substr(
                    start, end == std::wstring::npos
                        ? std::wstring::npos : end - start);
                token.erase(std::remove_if(
                    token.begin(), token.end(),
                    [](const wchar_t character) {
                        return std::iswspace(character) != 0;
                    }), token.end());
                const auto area = CountryFlags::NormalizeArea(
                    WideToUtf8(ToUpperWide(token)));
                const auto code = CountryPolicy::SelectableRegionArea(area)
                    ? area : CountryFlags::Normalize(area);
                if (code.size() == 3 && CountryFlags::IsKnownArea(code)) {
                    webSelectedCountryCodes_.insert(code);
                }
                if (end == std::wstring::npos) break;
                start = end + 1;
            }
            SendMessageW(scopeCountry_, CB_SETCURSEL, 0, 0);
            ApplyScopeFromControls();
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"country\"") != std::wstring::npos) {
            webRowOffset_ = 0;
            webSelectedCountryCodes_.clear();
            const auto country = getJsonString(L"value");
            if (country == L"All countries") {
                ClearScope();
                PostWebUiState();
                return;
            }
            int item = 0;
            if (country == L"China") {
                item = static_cast<int>(SendMessageW(
                    scopeCountry_, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                    reinterpret_cast<LPARAM>(L"CHN")));
            } else if (country == L"Europe") {
                // Europe is an aggregate concept option in the WebView.  The
                // native scope engine already supports rectangular bounds,
                // so express the option through the same validated path.
                SendMessageW(scopeCountry_, CB_SETCURSEL, 0, 0);
                SetWindowTextW(scopeLatitudeMinimum_, L"35");
                SetWindowTextW(scopeLatitudeMaximum_, L"72");
                SetWindowTextW(scopeLongitudeMinimum_, L"-25");
                SetWindowTextW(scopeLongitudeMaximum_, L"45");
                ApplyScopeFromControls();
                PostWebUiState();
                return;
            } else {
                item = static_cast<int>(SendMessageW(
                    scopeCountry_, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                    reinterpret_cast<LPARAM>(country.c_str())));
            }
            SendMessageW(scopeCountry_, CB_SETCURSEL, item < 0 ? 0 : item, 0);
            ApplyScopeFromControls();
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"select-station\"")
            != std::wstring::npos) {
            const auto stationId = ToUpperWide(getJsonString(L"station"));
            const bool checked = message.find(L"\"checked\":true")
                != std::wstring::npos;
            for (std::size_t index = 0; index < stations_.size(); ++index) {
                const auto& station = stations_[index];
                if (ToUpperWide(Utf8ToWide(station.fourCharId)) == stationId
                    || ToUpperWide(Utf8ToWide(station.stationId)) == stationId) {
                    stations_[index].selected = checked;
                    syncingList_ = true;
                    const auto visible = std::find(
                        visibleStationIndices_.begin(),
                        visibleStationIndices_.end(), index);
                    if (visible != visibleStationIndices_.end()) {
                        ListView_SetCheckState(
                            stationList_,
                            static_cast<int>(std::distance(
                                visibleStationIndices_.begin(), visible)),
                            checked ? TRUE : FALSE);
                    }
                    syncingList_ = false;
                    MapViewRefresh(map_);
                    UpdateSelectionSummary();
                    break;
                }
            }
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"map-hover-detail\"")
            != std::wstring::npos) {
            PostWebMapHoverDetail(getJsonString(L"station"));
            return;
        }
        if (message.find(L"\"type\":\"date-advanced\"")
            != std::wstring::npos) {
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"apply-date-advanced\"") != std::wstring::npos
            || message.find(L"\"type\":\"space-weather-preview\"") != std::wstring::npos) {
            const bool preview = message.find(L"\"type\":\"space-weather-preview\"") != std::wstring::npos;
            if (!preview && (querying_ || siteLogLoading_)) return;
            DateAdvancedFilterState candidate = dateAdvancedFilter_;
            const auto f107Basis = getJsonString(L"f107Basis");
            if (f107Basis == L"adjusted" || f107Basis == L"observed")
                candidate.useAdjustedF107 = f107Basis == L"adjusted";
            const auto readWeather = [&](const std::wstring_view modeKey,
                                         const std::wstring_view valueKey,
                                         const std::wstring_view upperKey,
                                         SpaceWeatherFilter& target,
                                         const double minimum,
                                         const double maximum) {
                const auto mode = getJsonString(modeKey);
                target.mode = mode == L"keep"
                    ? SpaceWeatherFilterMode::KeepAbove
                    : mode == L"exclude"
                        ? SpaceWeatherFilterMode::ExcludeAbove
                        : mode == L"range"
                            ? SpaceWeatherFilterMode::KeepRange
                        : SpaceWeatherFilterMode::Ignore;
                if (target.mode == SpaceWeatherFilterMode::Ignore) {
                    target.threshold = 0.0;
                    target.upperThreshold = 0.0;
                    return true;
                }
                if (!ParseDialogDouble(
                    getJsonString(valueKey), minimum, maximum,
                    target.threshold)) return false;
                if (target.mode == SpaceWeatherFilterMode::KeepRange) {
                    return ParseDialogDouble(
                        getJsonString(upperKey), minimum, maximum,
                        target.upperThreshold)
                        && target.threshold <= target.upperThreshold;
                }
                target.upperThreshold = target.threshold;
                return true;
            };
            if (!readWeather(L"kpMode", L"kpValue", L"kpUpper", candidate.kpMaxFilter,
                             0.0, 9.0)
                || !readWeather(L"dstMode", L"dstValue", L"dstUpper",
                                candidate.dstMinFilter, -2000.0, 200.0)
                || !readWeather(L"f107Mode", L"f107Value", L"f107Upper",
                                candidate.f107Filter, 0.0, 1000.0)) {
                if (preview) {
                    webUi_->PostJson(std::format(L"{{\"type\":\"space-weather-preview\",\"request\":\"{}\",\"error\":\"invalid-conditions\"}}", JsonEscape(getJsonString(L"request"))));
                    return;
                }
                SetOperationStatusText(
                    L"日期高级筛选未应用：请检查 Kp、Dst 和 F10.7 阈值。");
                PostWebUiState();
                return;
            }
            if (preview) {
                PostWebSpaceWeatherPreview(getJsonString(L"start"), getJsonString(L"end"),
                    candidate, getJsonString(L"request"));
                return;
            }
            dateAdvancedFilter_ = std::move(candidate);
            webRowOffset_ = 0;
            UpdateDateAdvancedFilterButtonLabel();
            SetOperationStatusText(
                L"日期高级筛选已应用，正在按空间天气条件刷新结果…");
            if (!stations_.empty()) {
                StartQuery();
            }
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"select-all\"")
            != std::wstring::npos) {
            SelectVisible(true);
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"clear-selection\"")
            != std::wstring::npos) {
            ClearAllSelection();
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"clear-scope\"")
            != std::wstring::npos) {
            webRowOffset_ = 0;
            ClearScope();
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"apply-bounds\"")
            != std::wstring::npos) {
            webRowOffset_ = 0;
            SetWindowTextW(scopeLatitudeMinimum_, getJsonString(L"latMin").c_str());
            SetWindowTextW(scopeLatitudeMaximum_, getJsonString(L"latMax").c_str());
            SetWindowTextW(scopeLongitudeMinimum_, getJsonString(L"lonMin").c_str());
            SetWindowTextW(scopeLongitudeMaximum_, getJsonString(L"lonMax").c_str());
            ApplyScopeFromControls();
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"rows-page\"")
            != std::wstring::npos) {
            constexpr std::size_t pageSize = 50U;
            const std::size_t rowCount = webRowsSelectedOnly_
                ? static_cast<std::size_t>(std::count_if(
                    visibleStationIndices_.begin(),
                    visibleStationIndices_.end(),
                    [this](const std::size_t stationIndex) {
                        return stationIndex < stations_.size()
                            && stations_[stationIndex].selected;
                    }))
                : visibleStationIndices_.size();
            const std::size_t lastPage = rowCount == 0
                ? 0U
                : ((rowCount - 1U) / pageSize) * pageSize;
            const auto direction = getJsonString(L"direction");
            if (direction == L"next") {
                webRowOffset_ = std::min(webRowOffset_ + pageSize, lastPage);
            } else if (direction == L"previous") {
                webRowOffset_ = webRowOffset_ >= pageSize
                    ? webRowOffset_ - pageSize : 0U;
            }
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"station-selection-report\"") != std::wstring::npos) {
            PostStationSelectionReport(); return;
        }
        if (message.find(L"\"type\":\"station-selection-day\"") != std::wstring::npos) {
            PostStationSelectionDay(getJsonString(L"date")); return;
        }
        if (message.find(L"\"type\":\"cancel-station-selection\"") != std::wstring::npos) {
            CancelStationSelection(); return;
        }
        if (message.find(L"\"type\":\"selection-settings\"") != std::wstring::npos
            || message.find(L"\"type\":\"check-station-selection\"") != std::wstring::npos) {
            if (!getJsonString(L"count").empty()) SetWindowTextW(uniformCountEdit_, getJsonString(L"count").c_str());
            if (!getJsonString(L"minDistance").empty()) SetWindowTextW(minimumDistanceEdit_, getJsonString(L"minDistance").c_str());
            if (!getJsonString(L"maxNeighbor").empty()) SetWindowTextW(maximumNeighborDistanceEdit_, getJsonString(L"maxNeighbor").c_str());
            const auto mode = getJsonString(L"dailyMode");
            selectionDailyMode_ = mode == L"stations" || mode == L"percent" ? mode : L"ignore";
            ParseDialogInt(getJsonString(L"dailyStations"), 0, 100000, selectionDailyStations_);
            ParseDialogDouble(getJsonString(L"dailyPercent"), 0, 100, selectionDailyPercent_);
            if (!getJsonString(L"randomSeed").empty()) selectionRandomSeed_ = getJsonString(L"randomSeed");
            if (message.find(L"\"type\":\"check-station-selection\"") != std::wstring::npos) BeginStationSelection(false);
            PostWebUiState(); return;
        }
        if (message.find(L"\"type\":\"uniform-select\"")
                != std::wstring::npos
            || message.find(L"\"type\":\"random-select\"")
                != std::wstring::npos) {
            SetWindowTextW(uniformCountEdit_, getJsonString(L"count").c_str());
            SetWindowTextW(minimumDistanceEdit_, getJsonString(L"minDistance").c_str());
            SetWindowTextW(maximumNeighborDistanceEdit_, getJsonString(L"maxNeighbor").c_str());
            const auto mode = getJsonString(L"dailyMode");
            selectionDailyMode_ = mode == L"stations" || mode == L"percent" ? mode : L"ignore";
            ParseDialogInt(getJsonString(L"dailyStations"), 0, 100000, selectionDailyStations_);
            ParseDialogDouble(getJsonString(L"dailyPercent"), 0, 100, selectionDailyPercent_);
            if (!getJsonString(L"randomSeed").empty()) selectionRandomSeed_ = getJsonString(L"randomSeed");
            const bool random = message.find(L"\"type\":\"random-select\"")
                != std::wstring::npos;
            SelectGloballyDistributed(random);
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"browse-folder\"")
            != std::wstring::npos) {
            BrowseFolder();
            PostWebUiState();
            return;
        }
        if (message.find(L"\"type\":\"pause\"") != std::wstring::npos) {
            if (preciseMode_) {
                TriggerPrecisePause(preciseProductView_);
            } else {
                ToggleDownloadPause();
            }
            return;
        }
        if (message.find(L"\"type\":\"cancel\"") != std::wstring::npos) {
            if (preciseMode_) {
                TriggerPreciseCancel(preciseProductView_);
            } else {
                if (CancelDownload()) {
                    observationQueueRunning_ = false;
                    PostWebUiState();
                }
            }
        }
    }

    void PostWebAdvancedChoices() const {
        if (!webUi_ || !webUi_->IsReady()) return;
        std::vector<std::string> systems{
            "GPS", "GLO", "GAL", "BDS", "QZS", "SBS", "IRN"};
        std::vector<std::string> receivers;
        std::vector<std::string> antennas;
        std::vector<std::string> signals;
        const std::vector<std::string> gimProviders{
            "IGS", "CODE", "ESA", "JPL", "UPC", "CAS", "EMR", "WHU"};
        std::wstring error;

        const auto appendUnique = [](
            std::vector<std::string>& target,
            const std::vector<std::string>& values) {
            std::unordered_set<std::string> seen;
            seen.reserve(target.size() + values.size());
            for (const auto& value : target) seen.insert(ChoiceValueKey(value));
            for (const auto& value : values) {
                if (value.empty()) continue;
                if (seen.insert(ChoiceValueKey(value)).second) {
                    target.push_back(value);
                }
            }
        };
        const auto appendSelection = [&appendUnique](
            std::vector<std::string>& target,
            const std::string& selection) {
            appendUnique(target, SplitChoiceSelections(selection));
        };

        const SYSTEMTIME startSystem = queryStartDate_;
        const SYSTEMTIME endSystem = queryEndDate_;
        std::vector<std::string> gimProductGrades;
        std::wstring gimGradesError;
        if (startSystem.wYear > 0 && endSystem.wYear > 0) {
            const auto choices = QueryGimProductGradeChoices(
                MakeQueryDate(startSystem.wYear, startSystem.wMonth, startSystem.wDay),
                MakeQueryDate(endSystem.wYear, endSystem.wMonth, endSystem.wDay));
            gimProductGrades = choices.values;
            gimGradesError = choices.error;
        }
        appendSelection(gimProductGrades, advancedFilter_.gimProductGrades);
        const NetworkSelection selection = GetNetworkSelection();
        if (!selection.Any()) {
            error = L"请至少选择一个站网。";
        } else if (MakeQueryDate(startSystem.wYear, startSystem.wMonth,
                       startSystem.wDay).dayOfYear <= 0
            || MakeQueryDate(endSystem.wYear, endSystem.wMonth,
                       endSystem.wDay).dayOfYear <= 0) {
            error = L"日期范围无效。";
        } else {
            const QueryDate start = MakeQueryDate(
                startSystem.wYear, startSystem.wMonth, startSystem.wDay);
            const QueryDate end = MakeQueryDate(
                endSystem.wYear, endSystem.wMonth, endSystem.wDay);
            const auto choices = QueryObservationSearchMetadataChoices(
                DefaultObservationSearchIndexPath(),
                start, end, selection, true);
            error = choices.error;
            appendUnique(systems, choices.systems);
            appendUnique(receivers, choices.receivers);
            appendUnique(antennas, choices.antennas);
            appendUnique(signals, choices.signals);
        }

        // A saved selection remains visible even when the current date range
        // no longer contains that equipment segment, so reopening the dialog
        // can never silently discard an active criterion.
        appendSelection(systems, advancedFilter_.systemsInclude);
        appendSelection(systems, advancedFilter_.systemsExclude);
        appendSelection(receivers, advancedFilter_.receiverInclude);
        appendSelection(receivers, advancedFilter_.receiverExclude);
        appendSelection(antennas, advancedFilter_.antennaInclude);
        appendSelection(antennas, advancedFilter_.antennaExclude);
        appendSelection(signals, advancedFilter_.signalCodesInclude);
        appendSelection(signals, advancedFilter_.signalCodesExclude);
        if (!gimGradesError.empty()) {
            if (!error.empty()) error += L" ";
            error += L"GIM product grades: " + gimGradesError;
        }

        std::erase_if(receivers, IsObviouslyInvalidEquipmentChoice);
        std::erase_if(antennas, IsObviouslyInvalidEquipmentChoice);

        const auto sortValues = [](std::vector<std::string>& values) {
            std::sort(values.begin(), values.end(),
                [](const std::string& left, const std::string& right) {
                    return ChoiceValueKey(left) < ChoiceValueKey(right);
                });
        };
        sortValues(systems);
        sortValues(receivers);
        sortValues(antennas);
        sortValues(signals);
        const auto jsonArray = [](const std::vector<std::string>& values) {
            std::wstring result = L"[";
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (index != 0) result += L',';
                result += L"\"" + JsonEscape(Utf8ToWide(values[index]))
                    + L"\"";
            }
            result += L']';
            return result;
        };
        webUi_->PostJson(std::format(
            L"{{\"type\":\"advanced-choices\",\"systems\":{},"
            L"\"receivers\":{},\"antennas\":{},\"signals\":{},"
            L"\"gimProviders\":{},\"gimProductGrades\":{},\"error\":\"{}\"}}",
            jsonArray(systems), jsonArray(receivers), jsonArray(antennas),
            jsonArray(signals), jsonArray(gimProviders), jsonArray(gimProductGrades), JsonEscape(error)));
    }

    void HandleWebDetailReady(std::unique_ptr<std::wstring> result) {
        if (result && webUi_ && webUi_->IsReady()) webUi_->PostJson(*result);
    }

    void PostWebSpaceWeatherPreview(const std::wstring& first, const std::wstring& last,
                                   const DateAdvancedFilterState& filters, const std::wstring& request) {
        SYSTEMTIME start{}, end{};
        if (!ParseSystemDate(first, start) || !ParseSystemDate(last, end) || first > last) {
            webUi_->PostJson(std::format(L"{{\"type\":\"space-weather-preview\",\"request\":\"{}\",\"error\":\"invalid-dates\"}}", JsonEscape(request)));
            return;
        }
        ObservationRangeOptions options;
        options.kpMaxFilter = filters.kpMaxFilter;
        options.dstMinFilter = filters.dstMinFilter;
        options.f107Filter = filters.f107Filter;
        options.useAdjustedF107 = filters.useAdjustedF107;
        const auto startDate = MakeQueryDate(start.wYear, start.wMonth, start.wDay);
        const auto endDate = MakeQueryDate(end.wYear, end.wMonth, end.wDay);
        const auto path = DefaultSpaceWeatherDatabasePath();
        const auto target = window_;
        const auto done = std::make_shared<std::atomic<bool>>(false);
        std::erase_if(webDetailWorkers_, [](const auto& worker) { return worker.finished->load(); });
        webDetailWorkers_.push_back({std::jthread([=](std::stop_token stop) {
            std::wstring json;
            try {
                const auto preview = PreviewSpaceWeatherDates(path, startDate, endDate, options, stop);
                const auto number = [](double value) {
                    return std::isfinite(value) && value > -1.0e8 && value < 1.0e8
                        ? std::format(L"{:.6f}", value) : std::wstring(L"null");
                };
                std::wstring rows = L"[";
                for (const auto& day : preview.days) {
                    if (rows.size() > 1) rows += L',';
                    rows += std::format(L"[\"{}\",{},{},{},{},{},{},\"{}\"]",
                        CalendarDateText(day.ordinal), number(day.values.kpMax), number(day.values.dstMin),
                        number(day.values.f107), day.retained ? L"true" : L"false",
                        day.failedMetrics, day.missingMetrics, JsonEscape(Utf8ToWide(day.values.source)));
                }
                rows += L']';
                json = std::format(L"{{\"type\":\"space-weather-preview\",\"request\":\"{}\",\"start\":\"{}\",\"end\":\"{}\",\"requestedDays\":{},\"retainedDays\":{},\"missingDays\":{},\"rows\":{},\"error\":\"{}\"}}",
                    JsonEscape(request), JsonEscape(first), JsonEscape(last), preview.requestedDays,
                    preview.retainedDays, preview.missingDays, rows, JsonEscape(preview.error));
            } catch (...) {
                json = std::format(L"{{\"type\":\"space-weather-preview\",\"request\":\"{}\",\"error\":\"load-failed\"}}", JsonEscape(request));
            }
            auto result = std::make_unique<std::wstring>(std::move(json));
            if (PostMessageW(target, WM_IGS_WEB_DETAIL_READY, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
            done->store(true);
        }), done});
    }

    void PostWebStationDetail(const std::wstring& requested, const std::wstring& request) {
        const auto upper = ToUpperWide(requested);
        for (const auto stationIndex : visibleStationIndices_) {
            if (stationIndex >= stations_.size()) continue;
            const auto station = stations_[stationIndex];
            const auto display = Utf8ToWide(station.fourCharId.empty() ? station.stationId : station.fourCharId);
            if (ToUpperWide(display) != upper && ToUpperWide(Utf8ToWide(station.stationId)) != upper) continue;
            std::erase_if(webDetailWorkers_, [](const auto& worker) { return worker.finished->load(); });
            const auto path = DefaultObservationSearchIndexPath();
            const auto start = currentDateRangeQuery_ ? currentPeriodStart_ : currentDate_;
            const auto end = currentDateRangeQuery_ ? currentPeriodEnd_ : currentDate_;
            const auto target = window_;
            const auto done = std::make_shared<std::atomic<bool>>(false);
            webDetailWorkers_.push_back({std::jthread([station, path, start, end, target, requested, request, done](std::stop_token) {
                std::wstring json;
                try {
                    const auto detail = LoadObservationStationDetail(path, station, start, end);
                    const auto& s = detail.station;
                    std::wstring fields = L"[";
                    const auto add = [&](std::wstring_view label, const std::string& value) {
                        if (fields.size() > 1) fields += L',';
                        fields += std::format(L"[\"{}\",\"{}\"]", JsonEscape(label), JsonEscape(Utf8ToWide(value)));
                    };
                    add(L"测站完整名称", s.siteLogName); add(L"DOMES", s.siteLogDomes);
                    add(L"负责机构", s.siteLogAgency); add(L"运行单位", s.siteLogOperator);
                    add(L"城市", s.siteLogCity);
                    const std::string stationArea = DisplayStationAreaCode(s);
                    add(L"国家/地区", stationArea);
                    fields += L']';
                    std::wstring periods = L"[";
                    for (const auto& item : detail.periods) {
                        if (periods.size() > 1) periods += L',';
                        std::wstring codes = L"[";
                        for (const auto& code : item.signals) {
                            if (codes.size() > 1) codes += L',';
                            codes += L'"' + JsonEscape(Utf8ToWide(code)) + L'"';
                        }
                        codes += L']';
                        periods += std::format(L"{{\"start\":\"{}\",\"end\":\"{}\",\"receiver\":\"{}\",\"receiverSerial\":\"{}\",\"firmware\":\"{}\",\"antenna\":\"{}\",\"antennaSerial\":\"{}\",\"radome\":\"{}\",\"referencePoint\":\"{}\",\"signals\":{}}}",
                            CalendarDateText(item.firstOrdinal), CalendarDateText(item.lastOrdinal),
                            JsonEscape(Utf8ToWide(item.receiver)), JsonEscape(Utf8ToWide(item.receiverSerial)),
                            JsonEscape(Utf8ToWide(item.firmware)), JsonEscape(Utf8ToWide(item.antenna)),
                            JsonEscape(Utf8ToWide(item.antennaSerial)), JsonEscape(Utf8ToWide(item.radome)),
                            JsonEscape(Utf8ToWide(item.referencePoint)), codes);
                    }
                    periods += L']';
                    const auto coverage = station.calendarCoverage ? CalendarCoverageJson(*station.calendarCoverage) : L"null";
                    json = std::format(L"{{\"type\":\"station-detail\",\"station\":\"{}\",\"request\":\"{}\",\"coverage\":{},\"fields\":{},\"periods\":{},\"error\":\"{}\"}}",
                        JsonEscape(requested), JsonEscape(request), coverage, fields, periods, JsonEscape(detail.error));
                } catch (...) {
                    json = std::format(L"{{\"type\":\"station-detail\",\"station\":\"{}\",\"request\":\"{}\",\"error\":\"测站详情读取失败\"}}", JsonEscape(requested), JsonEscape(request));
                }
                auto result = std::make_unique<std::wstring>(std::move(json));
                if (PostMessageW(target, WM_IGS_WEB_DETAIL_READY, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
                done->store(true);
            }), done});
            return;
        }
    }

    void PostWebMapHoverDetail(const std::wstring_view requestedStation) const {
        if (!webUi_ || !webUi_->IsReady()) return;
        const std::wstring requestedUpper = ToUpperWide(
            std::wstring(requestedStation));
        for (const auto stationIndex : visibleStationIndices_) {
            if (stationIndex >= stations_.size()) continue;
            const auto& station = stations_[stationIndex];
            const std::wstring stationId = Utf8ToWide(
                station.fourCharId.empty()
                    ? station.stationId : station.fourCharId);
            if (ToUpperWide(stationId) != requestedUpper
                && ToUpperWide(Utf8ToWide(station.stationId))
                    != requestedUpper) {
                continue;
            }
            const std::wstring network =
                FormatAvailableNetworks(station.availableNetworks);
            const std::wstring country = Utf8ToWide(
                DisplayStationCountryCode(station));
            const std::wstring countryArea = Utf8ToWide(DisplayStationAreaCode(station));
            const std::wstring interval = Utf8ToWide(station.interval);
            const std::wstring receiver = Utf8ToWide(
                station.siteLogReceiver.empty()
                    ? std::string("—") : station.siteLogReceiver);
            const std::wstring systems = Utf8ToWide(
                station.siteLogSystems.empty()
                    ? std::string("—") : station.siteLogSystems);
            const std::wstring antenna = Utf8ToWide(
                station.siteLogAntenna.empty()
                    ? std::string("—") : station.siteLogAntenna);
            const std::wstring format = station.interval == "RAW"
                ? L"接收机 ZIP" : FormatAvailableRinexFamilies(station);
            webUi_->PostJson(std::format(
                L"{{\"type\":\"map-hover-detail\",\"found\":true,"
                L"\"station\":\"{}\",\"network\":\"{}\","
                L"\"country\":\"{}\",\"countryArea\":\"{}\",\"interval\":\"{}\","
                L"\"receiver\":\"{}\",\"system\":\"{}\","
                L"\"antenna\":\"{}\",\"format\":\"{}\","
                L"\"selected\":{},\"latitude\":{:.6f},"
                L"\"longitude\":{:.6f}}}",
                JsonEscape(stationId), JsonEscape(network),
                JsonEscape(country), JsonEscape(countryArea), JsonEscape(interval),
                JsonEscape(receiver), JsonEscape(systems),
                JsonEscape(antenna), JsonEscape(format),
                station.selected ? L"true" : L"false",
                station.latitude, station.longitude));
            return;
        }
        webUi_->PostJson(std::format(
            L"{{\"type\":\"map-hover-detail\",\"found\":false,"
            L"\"station\":\"{}\"}}", JsonEscape(requestedStation)));
    }

    void PostWebUiState() const {
        if (!webUi_ || !webUi_->IsReady()) return;
        const auto selected = std::count_if(
            stations_.begin(), stations_.end(),
            [](const StationFile& station) { return station.selected; });
        const auto networkControls = NetworkControls();
        const auto sourceCount = std::count_if(
            networkControls.begin(), networkControls.end(),
            [this](const NetworkControl& item) {
                return HasNetwork(selectedNetworkMask_, item.second);
            });
        const auto availableSourceCount = std::count_if(
            networkControls.begin(), networkControls.end(),
            [this](const NetworkControl& item) {
                return item.first != nullptr
                    && HasNetwork(knowledgeBaseNetworks_, item.second);
            });
        std::wstring networksJson = L"[";
        for (std::size_t index = 0; index < networkControls.size(); ++index) {
            if (index != 0) networksJson += L',';
            const auto& item = networkControls[index];
            const std::wstring label = item.first == nullptr
                ? L"" : GetWindowTextString(item.first);
            const bool checked = HasNetwork(
                selectedNetworkMask_, item.second);
            const bool available = item.first != nullptr
                && HasNetwork(knowledgeBaseNetworks_, item.second);
            networksJson += std::format(
                L"{{\"name\":\"{}\",\"checked\":{},\"available\":{}}}",
                JsonEscape(label), checked ? L"true" : L"false",
                available ? L"true" : L"false");
        }
        networksJson += L']';
        std::wstring countriesJson = L"[";
        bool firstCountry = true;
        if (scopeCountry_ != nullptr) {
            const int count = static_cast<int>(SendMessageW(
                scopeCountry_, CB_GETCOUNT, 0, 0));
            for (int index = 1; index < count; ++index) {
                const int length = static_cast<int>(SendMessageW(
                    scopeCountry_, CB_GETLBTEXTLEN, index, 0));
                if (length <= 0) continue;
                std::wstring label(static_cast<std::size_t>(length) + 1U, L'\0');
                SendMessageW(scopeCountry_, CB_GETLBTEXT, index,
                    reinterpret_cast<LPARAM>(label.data()));
                label.resize(static_cast<std::size_t>(length));
                const auto flagIndex = static_cast<int>(SendMessageW(
                    scopeCountry_, CB_GETITEMDATA,
                    static_cast<WPARAM>(index), 0));
                if (!firstCountry) countriesJson += L',';
                firstCountry = false;
                countriesJson += std::format(
                    L"{{\"code\":\"{}\",\"flagIndex\":{}}}",
                    JsonEscape(label), flagIndex);
            }
        }
        countriesJson += L']';
        const std::wstring& status = operationStatusText_;
        const std::wstring& querySummary = querySummaryText_;
        const std::wstring& folder = downloadFolder_;
        const bool canDownload = !querying_ && !siteLogBlocksInteraction_
            && !MetadataFilterPending() && selected > 0;
        PreciseUiSnapshot preciseSnapshot{};
        const bool havePreciseSnapshot = QueryPreciseProductSnapshot(
            preciseProductView_, preciseSnapshot);
        std::wstring preciseRuleOptionsJson = L"[";
        for (const auto& option : preciseSnapshot.ruleOptions) {
            if (preciseRuleOptionsJson.size() > 1) preciseRuleOptionsJson += L',';
            preciseRuleOptionsJson += std::format(
                L"{{\"family\":\"{}\",\"latency\":\"{}\",\"center\":\"{}\",\"interval\":\"{}\"}}",
                JsonEscape(option.family), JsonEscape(option.latency),
                JsonEscape(option.center), JsonEscape(option.interval));
        }
        preciseRuleOptionsJson += L']';
        std::wstring preciseProductsJson = L"[]";
        if (havePreciseSnapshot && !preciseSnapshot.products.empty()) {
            preciseProductsJson = L"[";
            for (std::size_t index = 0;
                 index < preciseSnapshot.products.size(); ++index) {
                if (index != 0) preciseProductsJson += L',';
                const auto& product = preciseSnapshot.products[index];
                preciseProductsJson += std::format(
                    L"{{\"id\":\"{}\",\"name\":\"{}\","
                    L"\"family\":\"{}\",\"center\":\"{}\","
                    L"\"latency\":\"{}\",\"solutionClass\":\"{}\",\"constellation\":\"{}\","
                    L"\"interval\":\"{}\",\"formatVersion\":\"{}\",\"preferredFileName\":\"{}\","
                    L"\"sources\":\"{}\",\"availability\":\"{}\","
                    L"\"availableDays\":{},\"expectedDays\":{},"
                    L"\"completeness\":{},\"status\":\"{}\","
                    L"\"localInventoryOnly\":{},\"selected\":{}}}",
                    JsonEscape(product.id), JsonEscape(product.name),
                    JsonEscape(product.family), JsonEscape(product.center),
                    JsonEscape(product.latency), JsonEscape(product.solutionClass),
                    JsonEscape(product.constellation),
                    JsonEscape(product.interval),
                    JsonEscape(product.formatVersion),
                    JsonEscape(product.preferredFileName),
                    JsonEscape(product.sources),
                    JsonEscape(product.availability),
                    product.availableDays, product.expectedDays,
                    product.completeness, JsonEscape(product.status),
                    product.localInventoryOnly ? L"true" : L"false",
                    product.selected ? L"true" : L"false");
                if(product.recordCount>0){
                    preciseProductsJson.pop_back();
                    preciseProductsJson+=std::format(L",\"recordCount\":{},\"inventoryDays\":{},\"flaggedRecords\":{},\"flaggedDays\":{}}}",
                        product.recordCount,product.inventoryDays,product.flaggedRecords,product.flaggedDays);
                }
                if (product.missingRequired) {
                    preciseProductsJson.pop_back();
                    preciseProductsJson += L",\"missingRequired\":true}";
                }
            }
            preciseProductsJson += L']';
        }
        std::wstring rowsJson = L"[]";
        if (webSortedStationRevision_ != webStationDataRevision_
            || webSortedAscending_ != webSortAscending_
            || webSortedCompleteness_ != webSortByCompleteness_) {
            webSortedVisibleStationIndices_.assign(
                visibleStationIndices_.begin(), visibleStationIndices_.end());
            std::stable_sort(
                webSortedVisibleStationIndices_.begin(),
                webSortedVisibleStationIndices_.end(),
                [this](const std::size_t left, const std::size_t right) {
                    const auto stationId = [this](const std::size_t index) {
                        if (index >= stations_.size()) return std::string{};
                        const auto& station = stations_[index];
                        return station.fourCharId.empty()
                            ? station.stationId : station.fourCharId;
                    };
                    if (webSortByCompleteness_ && left < stations_.size() && right < stations_.size()) {
                        const double a = stations_[left].completenessPercent;
                        const double b = stations_[right].completenessPercent;
                        if (a != b) return webSortAscending_ ? a < b : a > b;
                    }
                    const auto leftId = stationId(left);
                    const auto rightId = stationId(right);
                    return webSortAscending_
                        ? leftId < rightId : leftId > rightId;
                });
            webSortedStationRevision_ = webStationDataRevision_;
            webSortedAscending_ = webSortAscending_;
            webSortedCompleteness_ = webSortByCompleteness_;
        }
        std::vector<std::size_t> webSelectedStationIndices;
        const std::vector<std::size_t>* webVisibleStationIndicesPointer =
            &webSortedVisibleStationIndices_;
        if (webRowsSelectedOnly_) {
            webSelectedStationIndices.reserve(
                webSortedVisibleStationIndices_.size());
            std::copy_if(
                webSortedVisibleStationIndices_.begin(),
                webSortedVisibleStationIndices_.end(),
                std::back_inserter(webSelectedStationIndices),
                [this](const std::size_t stationIndex) {
                    return stationIndex < stations_.size()
                        && stations_[stationIndex].selected;
                });
            webVisibleStationIndicesPointer = &webSelectedStationIndices;
        }
        const auto& webVisibleStationIndices =
            *webVisibleStationIndicesPointer;
        constexpr std::size_t webRowLimit = 50U;
        const std::size_t lastWebRowOffset = webVisibleStationIndices.empty()
            ? 0U
            : ((webVisibleStationIndices.size() - 1U) / webRowLimit)
                * webRowLimit;
        const std::size_t webRowBegin = std::min(
            webRowOffset_, lastWebRowOffset);
        const std::size_t webRowEnd = std::min(
            webVisibleStationIndices.size(), webRowBegin + webRowLimit);
        if (!stations_.empty()) {
            rowsJson = L"[";
            // The visible page is capped at 50 rows. Reserve its small
            // payload up front so each UI state publication avoids repeated
            // wide-string reallocations on the window thread.
            rowsJson.reserve((webRowEnd - webRowBegin) * 320U + 2U);
            for (std::size_t index = webRowBegin; index < webRowEnd; ++index) {
                if (index != webRowBegin) rowsJson += L',';
                const std::size_t stationIndex = webVisibleStationIndices[index];
                const auto& station = stations_[stationIndex];
                std::string sources;
                for (const auto& source : station.availabilitySources) {
                    if (!sources.empty()) sources += ", ";
                    sources += source;
                }
                if (sources.empty()) sources = station.preferredSource;
                const std::wstring stationId = Utf8ToWide(
                    station.fourCharId.empty() ? station.stationId : station.fourCharId);
                const std::wstring domes = Utf8ToWide(station.siteLogDomes);
                const std::string countryCode = DisplayStationCountryCode(station);
                const std::wstring country = Utf8ToWide(countryCode);
                const int countryFlagIndex = DisplayFlagIndex(station);
                const std::wstring receiver = Utf8ToWide(station.siteLogReceiver);
                const std::wstring receiverSystems = Utf8ToWide(
                    station.siteLogSystems);
                const std::wstring antenna = Utf8ToWide(station.siteLogAntenna);
                const std::wstring radome = Utf8ToWide(station.siteLogRadome);
                const std::wstring network =
                    FormatAvailableNetworks(station.availableNetworks);
                const std::wstring format = station.interval == "RAW"
                    ? L"接收机 ZIP"
                    : FormatAvailableRinexFamilies(station);
                const std::wstring interval = Utf8ToWide(station.interval);
                std::wstring receiverSystem = receiver;
                if (!station.siteLogSystems.empty()) {
                    if (!receiverSystem.empty()) receiverSystem += L" / ";
                    receiverSystem += Utf8ToWide(station.siteLogSystems);
                }
                std::wstring antennaRadome = antenna;
                if (!station.siteLogRadome.empty()) {
                    if (!antennaRadome.empty()) antennaRadome += L" / ";
                    antennaRadome += Utf8ToWide(station.siteLogRadome);
                }
                const int completeness = static_cast<int>(std::lround(
                    std::clamp(station.completenessPercent, 0.0, 100.0)));
                const std::wstring rowStatus = station.unverifiedDays > 0 ? L"Pending verification" : completeness >= 99
                    ? L"Available" : completeness > 0 ? L"Partial" : L"Missing";
                const std::wstring coverage = station.unverifiedDays > 0
                    ? std::format(L"预估 {:.1f}% · {}天待确认", station.completenessPercent, station.unverifiedDays)
                    : station.expectedDays > 0
                    ? std::format(
                        L"{:.1f}% ({}/{})", station.completenessPercent,
                        station.availableDays, station.expectedDays)
                    : L"—";
                std::format_to(std::back_inserter(rowsJson),
                    L"[\"{}\",\"{}\",\"{}\",\"{}\",\"{}\",\"{}\",\"{}%\",\"{}\",{},{:.6f},{:.6f},{},"
                    L"\"{}\",\"{}\",\"{}\",\"{}\",\"{}\",\"{}\",{},\"{}\",{:.8f},\"{}\",\"{}\"]",
                    JsonEscape(stationId), JsonEscape(domes),
                    JsonEscape(country), JsonEscape(receiver),
                    JsonEscape(antenna), JsonEscape(Utf8ToWide(sources)),
                    completeness, JsonEscape(rowStatus),
                    station.selected ? L"true" : L"false",
                    station.latitude, station.longitude,
                    station.hasCoordinate ? L"true" : L"false",
                    JsonEscape(network), JsonEscape(format),
                    JsonEscape(interval), JsonEscape(receiverSystem),
                    JsonEscape(antennaRadome), JsonEscape(coverage),
                    countryFlagIndex, JsonEscape(receiverSystems), station.completenessPercent,
                    JsonEscape(Utf8ToWide(DisplayStationAreaCode(station))),
                    JsonEscape(radome));
            }
            rowsJson += L']';
        }
        // The map payload can contain many thousands of points.  Most UI
        // state publications only change a button, status label or progress
        // value, so re-sending and re-parsing that payload on every update is
        // pure work.  A compact signature tracks the data revision plus the
        // selected set; unchanged publications use JSON null and the web UI
        // retains its existing Canvas layer.
        std::uint64_t mapSignature =
            (webStationDataRevision_ + 1U) * 1099511628211ULL;
        for (const auto stationIndex : visibleStationIndices_) {
            if (stationIndex >= stations_.size()
                || !stations_[stationIndex].selected) {
                continue;
            }
            mapSignature ^= static_cast<std::uint64_t>(stationIndex) +
                0x9e3779b97f4a7c15ULL + (mapSignature << 6U)
                + (mapSignature >> 2U);
        }
        if (mapSignature == 0) mapSignature = 1;
        std::wstring mapPointsJson = L"null";
        const bool mapDataChanged =
            webStationDataRevision_ != webLastMapDataRevisionSent_;
        if (mapDataChanged) {
            mapPointsJson = L"[";
            // Most queries publish several thousand points.  Reserve a
            // conservative compact payload and append directly into it; the
            // previous `+= std::format(...)` created one temporary wide
            // string per station before copying it into the payload.
            mapPointsJson.reserve(visibleStationIndices_.size() * 48U + 2U);
            bool firstMapPoint = true;
            for (const auto stationIndex : visibleStationIndices_) {
                if (stationIndex >= stations_.size()) continue;
                const auto& station = stations_[stationIndex];
                if (!station.hasCoordinate) continue;
                if (!firstMapPoint) mapPointsJson += L',';
                firstMapPoint = false;
                const std::wstring stationId = Utf8ToWide(
                    station.fourCharId.empty()
                        ? station.stationId : station.fourCharId);
                std::format_to(std::back_inserter(mapPointsJson),
                    L"[\"{}\",{:.6f},{:.6f},{}]",
                    JsonEscape(stationId), station.latitude,
                    station.longitude,
                    station.selected ? L"true" : L"false");
            }
            mapPointsJson += L']';
            webLastMapDataRevisionSent_ = webStationDataRevision_;
        }
        std::wstring mapSelectionJson = L"null";
        if (mapDataChanged
            || mapSignature != webLastMapSelectionSignatureSent_) {
            mapSelectionJson = L"[";
            bool firstSelected = true;
            for (const auto stationIndex : visibleStationIndices_) {
                if (stationIndex >= stations_.size()) continue;
                const auto& station = stations_[stationIndex];
                if (!station.selected || !station.hasCoordinate) continue;
                if (!firstSelected) mapSelectionJson += L',';
                firstSelected = false;
                const std::wstring stationId = Utf8ToWide(
                    station.fourCharId.empty()
                        ? station.stationId : station.fourCharId);
                mapSelectionJson += L"\"" + JsonEscape(stationId) + L"\"";
            }
            mapSelectionJson += L']';
            webLastMapSelectionSignatureSent_ = mapSignature;
        }
        const auto readDate = [](const SYSTEMTIME& value) {
            return std::format(
                L"{:04}-{:02}-{:02}", value.wYear, value.wMonth, value.wDay);
        };
        const wchar_t* completenessMode = advancedFilter_.completenessMode
                == AdvancedFilterState::CompletenessMode::RatioAtLeast
            ? L"ratio"
            : advancedFilter_.completenessMode
                    == AdvancedFilterState::CompletenessMode::DaysAtLeast
                ? L"days" : L"ignore";
        const wchar_t* dailyMode = advancedFilter_.dailyCoverageMode
                == DailyCoverageMode::StationsAtLeast
            ? L"stations"
            : advancedFilter_.dailyCoverageMode
                    == DailyCoverageMode::PercentAtLeast
                ? L"percent" : L"ignore";
        const std::wstring advancedJson = std::format(
            L"{{\"completenessMode\":\"{}\",\"completenessPercent\":{:.6f},"
            L"\"minimumDays\":{},\"dailyMode\":\"{}\","
            L"\"dailyStations\":{},\"dailyPercent\":{:.6f},"
            L"\"sampleInterval\":\"{}\",\"rinexVersion\":{},"
            L"\"systemsInclude\":\"{}\",\"systemsExclude\":\"{}\","
            L"\"receiverInclude\":\"{}\",\"receiverExclude\":\"{}\","
            L"\"antennaInclude\":\"{}\",\"antennaExclude\":\"{}\","
            L"\"signalCodesInclude\":\"{}\",\"signalCodesExclude\":\"{}\","
            L"\"gimStationMode\":\"{}\",\"gimProviders\":\"{}\",\"gimProductGrades\":\"{}\","
            L"\"referenceFrame\":\"{}\","
            L"\"observationCompletenessMode\":\"{}\","
            L"\"observationCompletenessPercent\":{:.6f},"
            L"\"latencyMode\":\"{}\",\"maximumMeanLatency\":{:.6f},"
            L"\"positionTimeSeriesMode\":\"{}\","
            L"\"maximumMeanDetrendedPosition\":{:.6f},"
            L"\"residualsMode\":\"{}\",\"maximumMeanResidual\":{:.6f},"
            L"\"multipathMode\":\"{}\",\"maximumMeanMultipath\":{:.6f},"
            L"\"cycleSlipsMode\":\"{}\",\"maximumCycleSlips\":{:.6f},"
            L"\"observationCompletenessSystem\":\"{}\","
            L"\"multipathSystem\":\"{}\",\"cycleSlipsSystem\":\"{}\"}}",
            completenessMode, advancedFilter_.minimumCompletenessPercent,
            advancedFilter_.minimumAvailableDays, dailyMode,
            advancedFilter_.minimumDailyStations,
            advancedFilter_.minimumDailyStationPercent,
            JsonEscape(Utf8ToWide(advancedFilter_.sampleInterval)),
            advancedFilter_.rinexVersion,
            JsonEscape(Utf8ToWide(advancedFilter_.systemsInclude)),
            JsonEscape(Utf8ToWide(advancedFilter_.systemsExclude)),
            JsonEscape(Utf8ToWide(advancedFilter_.receiverInclude)),
            JsonEscape(Utf8ToWide(advancedFilter_.receiverExclude)),
            JsonEscape(Utf8ToWide(advancedFilter_.antennaInclude)),
            JsonEscape(Utf8ToWide(advancedFilter_.antennaExclude)),
            JsonEscape(Utf8ToWide(advancedFilter_.signalCodesInclude)),
            JsonEscape(Utf8ToWide(advancedFilter_.signalCodesExclude)),
            JsonEscape(Utf8ToWide(advancedFilter_.gimStationMode)),
            JsonEscape(Utf8ToWide(advancedFilter_.gimProviders)),
            JsonEscape(Utf8ToWide(advancedFilter_.gimProductGrades)),
            ReferenceFrameSelectionValue(quickFrameSelection_),
            advancedFilter_.observationCompletenessEnabled ? L"minimum" : L"ignore",
            advancedFilter_.minimumObservationCompletenessPercent,
            advancedFilter_.latencyEnabled ? L"maximum" : L"ignore",
            advancedFilter_.maximumMeanLatencyHours,
            advancedFilter_.positionTimeSeriesEnabled ? L"maximum" : L"ignore",
            advancedFilter_.maximumMeanAbsoluteDetrendedPositionMillimeters,
            advancedFilter_.residualsEnabled ? L"maximum" : L"ignore",
            advancedFilter_.maximumMeanAbsoluteResidualMillimeters,
            advancedFilter_.meanMultipathEnabled ? L"maximum" : L"ignore",
            advancedFilter_.maximumMeanMultipathCentimeters,
            advancedFilter_.cycleSlipsEnabled ? L"maximum" : L"ignore",
            advancedFilter_.maximumMeanDailyCycleSlips,
            JsonEscape(Utf8ToWide(advancedFilter_.observationCompletenessSystem)),
            JsonEscape(Utf8ToWide(advancedFilter_.multipathSystem)),
            JsonEscape(Utf8ToWide(advancedFilter_.cycleSlipsSystem)));
        const auto weatherMode = [](const SpaceWeatherFilter& filter) {
            return filter.mode == SpaceWeatherFilterMode::KeepAbove
                ? L"keep"
                : filter.mode == SpaceWeatherFilterMode::ExcludeAbove
                    ? L"exclude"
                    : filter.mode == SpaceWeatherFilterMode::KeepRange
                        ? L"range" : L"ignore";
        };
        const std::wstring dateAdvancedJson = std::format(
            L"{{\"kpMode\":\"{}\",\"kpValue\":{:.6f},\"kpUpper\":{:.6f},"
            L"\"dstMode\":\"{}\",\"dstValue\":{:.6f},\"dstUpper\":{:.6f},"
            L"\"f107Mode\":\"{}\",\"f107Value\":{:.6f},\"f107Upper\":{:.6f},\"f107Basis\":\"{}\"}}",
            weatherMode(dateAdvancedFilter_.kpMaxFilter),
            dateAdvancedFilter_.kpMaxFilter.threshold,
            dateAdvancedFilter_.kpMaxFilter.upperThreshold,
            weatherMode(dateAdvancedFilter_.dstMinFilter),
            dateAdvancedFilter_.dstMinFilter.threshold,
            dateAdvancedFilter_.dstMinFilter.upperThreshold,
            weatherMode(dateAdvancedFilter_.f107Filter),
            dateAdvancedFilter_.f107Filter.threshold,
            dateAdvancedFilter_.f107Filter.upperThreshold,
            dateAdvancedFilter_.useAdjustedF107 ? L"adjusted" : L"observed");
        const wchar_t* observationPresetValue =
            observationPreset_ == ObservationPresetSelection::ReferenceFrameAutomatic
                ? L"reference"
                : observationPreset_ == ObservationPresetSelection::GimTec
                    ? L"gim"
                    : observationPreset_ == ObservationPresetSelection::GimEvaluation
                        ? L"gim-evaluation"
                    : observationPreset_
                            == ObservationPresetSelection::BeiDouPod
                        ? L"pod"
                        : observationPreset_
                                == ObservationPresetSelection::NetworkRtk
                            ? L"network-rtk" : L"normal";
        std::wstring scopeCountryValue;
        if (!webSelectedCountryCodes_.empty()) {
            for (const auto& code : webSelectedCountryCodes_) {
                if (!scopeCountryValue.empty()) scopeCountryValue += L',';
                scopeCountryValue += Utf8ToWide(code);
            }
        } else {
            scopeCountryValue = GetWindowTextString(scopeCountry_);
            if (scopeCountryValue == L"全部国家/地区") {
                scopeCountryValue = L"All countries";
            }
        }
        std::wstring json = std::format(
            L"{{\"type\":\"state\",\"stations\":{},\"visible\":{},\"selected\":{},\"sources\":{},\"availableSources\":{},\"networks\":{},\"countries\":{},"
            L"\"rowOffset\":{},\"rowLimit\":{},\"rowTotal\":{},"
            L"\"rowsSelectedOnly\":{},"
            L"\"filter\":\"{}\",\"sortAscending\":{},\"sortColumn\":\"{}\",\"stationRevision\":{},"
            L"\"querying\":{},\"downloading\":{},\"canDownload\":{},"
            L"\"paused\":{},\"canPause\":{},"
            L"\"folder\":\"{}\",\"country\":\"{}\"," 
            L"\"selectionCount\":\"{}\",\"minDistance\":\"{}\"," 
            L"\"maxNeighbor\":\"{}\",\"latMin\":\"{}\",\"latMax\":\"{}\"," 
            L"\"lonMin\":\"{}\",\"lonMax\":\"{}\"," 
            L"\"startDate\":\"{}\",\"endDate\":\"{}\"," 
            L"\"status\":\"{}\",\"querySummary\":\"{}\","
            L"\"rows\":{},\"mapPoints\":{},\"mapSelection\":{},"
            L"\"observationPreset\":\"{}\","
            L"\"advanced\":{},\"dateAdvanced\":{},"
            L"\"preciseBusy\":{},\"preciseProgress\":{},\"precisePaused\":{},"
            L"\"preciseSelected\":{},\"preciseQuickReady\":{},"
            L"\"preciseFamily\":{},\"preciseLatency\":{},"
            L"\"preciseCenter\":{},\"preciseCompleteness\":{},"
            L"\"preciseCompletenessMode\":\"{}\",\"preciseCompletenessValue\":{:.6f},"
            L"\"preciseQuickPreset\":{},"
            L"\"preciseFolder\":\"{}\",\"preciseStatus\":\"{}\"," 
            L"\"preciseStartDate\":\"{}\",\"preciseEndDate\":\"{}\"," 
            L"\"preciseTroStation\":\"{}\",\"preciseSearch\":\"{}\"," 
            L"\"preciseRuleOptionsReady\":{},\"preciseRuleOptions\":{},"
            L"\"preciseProducts\":{},\"preciseRevision\":{}}}",
            stations_.size(), visibleStationIndices_.size(), selected,
            sourceCount, availableSourceCount, networksJson, countriesJson,
            webRowBegin, webRowLimit, webVisibleStationIndices.size(),
            webRowsSelectedOnly_ ? L"true" : L"false",
            JsonEscape(filterText_),
            webSortAscending_ ? L"true" : L"false",
            webSortByCompleteness_ ? L"completeness" : L"station", webStationDataRevision_,
            querying_ ? L"true" : L"false",
            downloading_ ? L"true" : L"false",
            canDownload ? L"true" : L"false",
            downloading_ && downloadControl_ != nullptr
                    && downloadControl_->isPaused() ? L"true" : L"false",
            downloading_ && downloadTransferStarted_
                    && !downloadCancelRequested_ ? L"true" : L"false",
            JsonEscape(folder),
            JsonEscape(scopeCountryValue),
            JsonEscape(GetWindowTextString(uniformCountEdit_)),
            JsonEscape(GetWindowTextString(minimumDistanceEdit_)),
            JsonEscape(GetWindowTextString(maximumNeighborDistanceEdit_)),
            JsonEscape(GetWindowTextString(scopeLatitudeMinimum_)),
            JsonEscape(GetWindowTextString(scopeLatitudeMaximum_)),
            JsonEscape(GetWindowTextString(scopeLongitudeMinimum_)),
            JsonEscape(GetWindowTextString(scopeLongitudeMaximum_)),
            JsonEscape(readDate(queryStartDate_)),
            JsonEscape(readDate(queryEndDate_)),
            JsonEscape(status), JsonEscape(querySummary), rowsJson,
            mapPointsJson, mapSelectionJson,
            observationPresetValue,
            advancedJson, dateAdvancedJson,
            havePreciseSnapshot && preciseSnapshot.busy ? L"true" : L"false",
            havePreciseSnapshot ? preciseSnapshot.progressPercent : 0,
            havePreciseSnapshot && preciseSnapshot.paused ? L"true" : L"false",
            havePreciseSnapshot ? preciseSnapshot.selected : 0,
            havePreciseSnapshot && preciseSnapshot.quickBundleReady
                ? L"true" : L"false",
            havePreciseSnapshot ? preciseSnapshot.familyIndex : 1,
            havePreciseSnapshot ? preciseSnapshot.latencyIndex : 0,
            havePreciseSnapshot ? preciseSnapshot.centerIndex : 0,
            havePreciseSnapshot ? preciseSnapshot.completenessIndex : 0,
            havePreciseSnapshot
                ? JsonEscape(preciseSnapshot.completenessMode) : L"ignore",
            havePreciseSnapshot ? preciseSnapshot.completenessValue : 0.0,
            havePreciseSnapshot ? preciseSnapshot.quickPresetIndex : 0,
            havePreciseSnapshot ? JsonEscape(preciseSnapshot.folder) : L"",
            havePreciseSnapshot ? JsonEscape(preciseSnapshot.status) : L"",
            havePreciseSnapshot ? JsonEscape(preciseSnapshot.startDate) : L"",
            havePreciseSnapshot ? JsonEscape(preciseSnapshot.endDate) : L"",
            havePreciseSnapshot ? JsonEscape(preciseSnapshot.troStation) : L"",
            havePreciseSnapshot ? JsonEscape(preciseSnapshot.search) : L"",
            preciseSnapshot.ruleOptionsReady ? L"true" : L"false", preciseRuleOptionsJson,
            preciseProductsJson, preciseSnapshot.catalogRevision);
        std::wstring observationQueueJson = L"[";
        for (const auto& item : observationQueue_) {
            if (observationQueueJson.size() > 1) observationQueueJson += L',';
            observationQueueJson += std::format(
                L"{{\"id\":\"{}\",\"label\":\"{}\",\"range\":\"{}\","
                L"\"status\":\"{}\",\"percent\":{},\"selected\":{}}}",
                item.id, JsonEscape(item.label),
                JsonEscape(FormatDate(item.start) + L"—" + FormatDate(item.end)),
                item.state, item.percent,
                item.selected ? L"true" : L"false");
        }
        observationQueueJson += L']';
        std::wstring preciseQueueJson = L"[";
        if (havePreciseSnapshot) {
            for (const auto& item : preciseSnapshot.queue) {
                if (preciseQueueJson.size() > 1) preciseQueueJson += L',';
                preciseQueueJson += std::format(
                    L"{{\"id\":\"{}\",\"label\":\"{}\",\"range\":\"{}\","
                    L"\"status\":\"{}\",\"percent\":{},\"selected\":{},\"fileCount\":{}}}",
                    item.id, JsonEscape(item.label), JsonEscape(item.range),
                    item.status, item.percent,
                    item.selected ? L"true" : L"false",item.fileCount);
            }
        }
        preciseQueueJson += L']';
        json.pop_back();
        const auto downloadOptions = DownloadSettings::Load();
        json += std::format(L",\"downloadSettings\":{{\"layout\":\"{}\",\"postprocess\":\"{}\"}},\"postprocessing\":{}",
            downloadOptions.datedFolders ? L"date" : L"flat",
            downloadOptions.askObservationPostprocess ? L"ask" : L"off",
            postprocessBusy_ ? L"true" : L"false");
        json += std::format(L",\"stationSelection\":{{\"busy\":{},\"metadataLoading\":{},\"stale\":{},\"dailyMode\":\"{}\","
            L"\"dailyStations\":{},\"dailyPercent\":{:.3f},\"randomSeed\":\"{}\",\"summary\":{}}}",
            stationSelectionBusy_ ? L"true" : L"false",
            siteLogLoading_ || (!siteLogEnrichmentComplete_ && !stations_.empty()) ? L"true" : L"false",
            lastStationSelection_.status.empty() || stationSelectionSignature_ != StationSelectionSignature() ? L"true" : L"false",
            JsonEscape(selectionDailyMode_), selectionDailyStations_, selectionDailyPercent_,
            JsonEscape(selectionRandomSeed_), StationSelectionSummaryJson());
        json += L",\"observationQueue\":" + observationQueueJson
            + L",\"observationQueueRunning\":"
            + (observationQueueRunning_ ? L"true" : L"false")
            + L",\"activeObservationQueueId\":\""
            + std::to_wstring(activeObservationQueueId_) + L"\""
            + L",\"preciseQueue\":" + preciseQueueJson
            + L",\"preciseQueueRunning\":"
            + (havePreciseSnapshot && preciseSnapshot.queueRunning
                ? L"true" : L"false")
            + L",\"activePreciseQueueId\":\""
            + std::to_wstring(havePreciseSnapshot
                ? preciseSnapshot.activeQueueId : 0) + L"\""
            + L",\"preciseDownloading\":"
            + (havePreciseSnapshot && preciseSnapshot.transferring
                ? L"true" : L"false") + L"}";
        webUi_->PostJson(json);
    }

    void PostWebUiProgress(
        const int percent,
        const std::wstring_view message) const {
        if (!webUi_ || !webUi_->IsReady()) return;
        const std::wstring json = std::format(
            L"{{\"type\":\"progress\",\"percent\":{},\"message\":\"{}\"}}",
            std::clamp(percent, 0, 100), JsonEscape(message));
        webUi_->PostJson(json);
    }

    void Layout() const {
        RECT client{};
        GetClientRect(window_, &client);
        const int width = std::max(
            1, static_cast<int>(client.right - client.left));
        const int height = std::max(
            1, static_cast<int>(client.bottom - client.top));
        const auto metrics = ComputeMainLayoutMetrics(
            width, height, networksExpanded_);
        const bool compact = height < kMainCompactHeight;

        // A single redraw transaction prevents the dozens of child controls
        // from repainting one by one while the user resizes the window.
        SendMessageW(window_, WM_SETREDRAW, FALSE, 0);

        constexpr int modeWidth = 124;
        constexpr int modeGap = 8;
        const int modeLeft = std::max(
            metrics.contentLeft + 260,
            metrics.contentRight - modeWidth * 2 - modeGap);
        const int titleWidth = std::max(
            220, modeLeft - metrics.contentLeft - 18);
        MoveWindow(title_, metrics.contentLeft, 16,
            titleWidth, 38, FALSE);
        MoveWindow(subtitle_, metrics.contentLeft, 56,
            titleWidth, 24, FALSE);
        MoveWindow(observationModeButton_, modeLeft, 18,
            modeWidth, 34, FALSE);
        MoveWindow(preciseModeButton_, modeLeft + modeWidth + modeGap, 18,
            modeWidth, 34, FALSE);

        MoveWindow(preciseProductView_, metrics.contentLeft, metrics.contentTop,
            metrics.contentWidth,
            std::max(100, height - metrics.contentTop - 18), FALSE);

        const int networkButtonWidth = std::max(
            160, metrics.networkOverlayWidth - 172);
        const int sourceControlTop = metrics.contentTop
            + (compact ? 28 : 26);
        MoveWindow(networkPanelToggleButton_, metrics.contentLeft + 8,
            sourceControlTop, networkButtonWidth, 30, FALSE);
        MoveWindow(selectKnowledgeBaseNetworksButton_,
            metrics.contentLeft + metrics.networkOverlayWidth - 156,
            sourceControlTop, 148, 30, FALSE);

        // The full network catalogue is a floating card.  It no longer pushes
        // the results table below the fold when expanded, so users can inspect
        // all 42 sources without sacrificing the map or download bar.
        constexpr int networkRowHeight = 26;
        const int networkGridTop = metrics.contentTop + 66;
        const int networkColumnWidth = metrics.networkOverlayWidth / 4;
        const auto networkControls = NetworkControls();
        for (std::size_t index = 0; index < networkControls.size(); ++index) {
            const HWND control = networkControls[index].first;
            const int column = static_cast<int>(index % 4U);
            const int row = static_cast<int>(index / 4U);
            const int left = metrics.contentLeft + column * networkColumnWidth + 8;
            const int right = column == 3
                ? metrics.contentLeft + metrics.networkOverlayWidth - 8
                : metrics.contentLeft + (column + 1) * networkColumnWidth - 4;
            if (networksExpanded_ && !preciseMode_) {
                SetWindowPos(control, HWND_TOP, left,
                    networkGridTop + row * networkRowHeight,
                    std::max(80, right - left), networkRowHeight,
                    SWP_SHOWWINDOW);
            } else {
                SetWindowPos(control, HWND_BOTTOM, left,
                    networkGridTop + row * networkRowHeight,
                    std::max(80, right - left), networkRowHeight,
                    SWP_NOACTIVATE | SWP_HIDEWINDOW);
            }
        }
        if (networksExpanded_ && !preciseMode_) {
            SetWindowPos(networkPanelToggleButton_, HWND_TOP,
                metrics.contentLeft + 8, sourceControlTop,
                networkButtonWidth, 30, SWP_SHOWWINDOW);
            SetWindowPos(selectKnowledgeBaseNetworksButton_, HWND_TOP,
                metrics.contentLeft + metrics.networkOverlayWidth - 156,
                sourceControlTop, 148, 30, SWP_SHOWWINDOW);
        }

        // Query controls use a three-row rhythm: preset, date/action and
        // optional filters.  This keeps the primary action visible at every
        // supported window width.
        const int panelLeft = metrics.contentLeft;
        const int panelWidth = metrics.panelWidth;
        const int panelRight = metrics.panelRight;
        MoveWindow(quickSchemeLabel_, panelLeft, metrics.quickTop + 27,
            72, 24, FALSE);
        MoveWindow(quickScheme_, panelLeft + 76, metrics.quickTop + 22,
            std::max(160, panelWidth - 76), 31, FALSE);
        MoveWindow(datePicker_, panelLeft, metrics.dateTop, 124, 31, FALSE);
        MoveWindow(dateRangeToLabel_, panelLeft + 128,
            metrics.dateTop + 4, 16, 24, FALSE);
        MoveWindow(endDatePicker_, panelLeft + 148,
            metrics.dateTop, 124, 31, FALSE);
        MoveWindow(queryButton_, panelRight - 124, metrics.dateTop,
            124, 31, FALSE);
        const int advancedWidth = std::max(120, (panelWidth - 8) / 2);
        MoveWindow(advancedFilterButton_, panelLeft, metrics.advancedTop,
            advancedWidth, 31, FALSE);
        MoveWindow(dateAdvancedFilterButton_, panelLeft + advancedWidth + 8,
            metrics.advancedTop, panelWidth - advancedWidth - 8, 31, FALSE);
        MoveWindow(queryStatus_, panelLeft, metrics.queryStatusTop,
            panelWidth, metrics.queryStatusHeight, FALSE);
        // The compact status surface is intentionally a single quiet line;
        // scrollbars would consume most of its 34 px height and look like a
        // second border.  The full multiline diagnostics return as soon as
        // the window grows back to the normal layout.
        ShowScrollBar(queryStatus_, SB_HORZ, compact ? FALSE : TRUE);
        ShowScrollBar(queryStatus_, SB_VERT, compact ? FALSE : TRUE);
        MoveWindow(filterLabel_, panelLeft, metrics.filterLabelTop,
            panelWidth, 20, FALSE);
        MoveWindow(filterEdit_, panelLeft, metrics.filterEditTop,
            panelWidth, 31, FALSE);
        MoveWindow(stationList_, panelLeft, metrics.listTop,
            panelWidth,
            std::max(compact ? 58 : 34,
                metrics.listBottom - metrics.listTop), FALSE);
        ResizeStationColumns(panelWidth);

        int y = metrics.actionTop;
        if (compact) {
            // At the minimum track height the list and download controls
            // share a deliberately compressed footer.  Keeping this as a
            // separate rhythm prevents the status line from falling below
            // the client area (the old normal spacing did exactly that).
            constexpr int compactGap = 6;
            constexpr int compactClearWidth = 92;
            constexpr int compactSelectWidth = 112;
            MoveWindow(selectionSummary_, panelLeft, y,
                std::max(100, panelWidth - compactSelectWidth
                    - compactClearWidth - compactGap * 2),
                22, FALSE);
            MoveWindow(selectAllButton_, panelRight - compactClearWidth
                    - compactSelectWidth - compactGap,
                y - 2, compactSelectWidth, 26, FALSE);
            MoveWindow(clearSelectionButton_, panelRight - compactClearWidth,
                y - 2, compactClearWidth, 26, FALSE);
            y += 24;
            constexpr int compactFolderLabel = 58;
            constexpr int compactBrowse = 70;
            MoveWindow(folderLabel_, panelLeft, y + 2,
                compactFolderLabel, 22, FALSE);
            MoveWindow(folderEdit_, panelLeft + compactFolderLabel + compactGap,
                y, std::max(100, panelWidth - compactFolderLabel
                    - compactBrowse - compactGap * 2),
                26, FALSE);
            MoveWindow(browseButton_, panelRight - compactBrowse, y,
                compactBrowse, 26, FALSE);
            y += 30;
            constexpr int compactPause = 96;
            MoveWindow(downloadButton_, panelLeft, y,
                std::max(110, panelWidth - compactPause - compactGap),
                30, FALSE);
            MoveWindow(pauseDownloadButton_, panelRight - compactPause, y,
                compactPause, 30, FALSE);
            y += 34;
            MoveWindow(progress_, panelLeft, y, panelWidth, 10, FALSE);
            y += 14;
            MoveWindow(operationStatus_, panelLeft, y, panelWidth, 18, FALSE);
        } else {
            constexpr int actionButtonWidth = 96;
            const int actionSummaryWidth = std::max(
                120, panelWidth - actionButtonWidth * 2 - 12);
            MoveWindow(selectionSummary_, panelLeft, y,
                actionSummaryWidth, 24, FALSE);
            MoveWindow(selectAllButton_, panelRight - actionButtonWidth * 2 - 8,
                y - 3, actionButtonWidth, 30, FALSE);
            MoveWindow(clearSelectionButton_, panelRight - actionButtonWidth,
                y - 3, actionButtonWidth, 30, FALSE);
            y += 32;
            MoveWindow(folderLabel_, panelLeft, y + 4, 68, 24, FALSE);
            MoveWindow(folderEdit_, panelLeft + 72, y,
                std::max(100, panelWidth - 150), 31, FALSE);
            MoveWindow(browseButton_, panelRight - 74, y, 74, 31, FALSE);
            y += 38;
            constexpr int pauseButtonWidth = 112;
            MoveWindow(downloadButton_, panelLeft, y,
                std::max(120, panelWidth - pauseButtonWidth - 8), 35, FALSE);
            MoveWindow(pauseDownloadButton_, panelRight - pauseButtonWidth, y,
                pauseButtonWidth, 35, FALSE);
            y += 40;
            MoveWindow(progress_, panelLeft, y, panelWidth, 14, FALSE);
            y += 19;
            MoveWindow(operationStatus_, panelLeft, y, panelWidth, 22, FALSE);
        }

        // Map controls are intentionally stacked into compact rows.  The old
        // single-row layout clipped at normal laptop widths; this one remains
        // readable from 1180 px upward and leaves the map as the dominant
        // visual element.
        const int mapLeft = metrics.mapLeft;
        const int mapWidth = metrics.mapWidth;
        const int mapRow1 = metrics.contentTop + 26;
        const bool compactMap = mapWidth < 360;
        if (compactMap) {
            constexpr int compactGap = 6;
            constexpr int compactLabelWidth = 34;
            // Short captions keep the three map actions readable in the
            // narrow toolbar; the full labels are restored in the normal
            // layout below.
            SetWindowTextW(applyScopeButton_, L"应用");
            SetWindowTextW(mapRectangleScopeButton_, L"框选");
            SetWindowTextW(clearScopeButton_, L"清除");
            SetWindowTextW(scopeLatitudeLabel_, L"纬");
            SetWindowTextW(scopeLongitudeLabel_, L"经");
            SetWindowTextW(minimumDistanceLabel_, L"最小");
            SetWindowTextW(maximumNeighborDistanceLabel_, L"最大");
            const int compactCountryWidth = std::max(
                1, mapWidth - compactLabelWidth);
            MoveWindow(scopeCountryLabel_, mapLeft, mapRow1 + 5,
                compactLabelWidth, 24, FALSE);
            MoveWindow(scopeCountry_, mapLeft + compactLabelWidth,
                mapRow1, compactCountryWidth, 240, FALSE);

            const int compactActionTop = mapRow1 + 38;
            const int compactActionWidth = std::max(
                1, (mapWidth - compactGap * 2) / 3);
            int actionX = mapLeft;
            for (const HWND button : {
                     applyScopeButton_, mapRectangleScopeButton_,
                     clearScopeButton_}) {
                MoveWindow(button, actionX, compactActionTop,
                    compactActionWidth, 31, FALSE);
                actionX += compactActionWidth + compactGap;
            }

            const int compactRangeTop = mapRow1 + 76;
            constexpr int compactRangeLabelWidth = 28;
            constexpr int compactRangeEditWidth = 36;
            constexpr int compactRangeToWidth = 12;
            const int compactGroupWidth = compactRangeLabelWidth
                + compactRangeEditWidth * 2 + compactRangeToWidth;
            auto placeCompactRange = [&](const HWND label,
                                         const HWND minimum,
                                         const HWND to,
                                         const HWND maximum,
                                         const int left) {
                MoveWindow(label, left, compactRangeTop + 5,
                    compactRangeLabelWidth, 24, FALSE);
                MoveWindow(minimum, left + compactRangeLabelWidth,
                    compactRangeTop, compactRangeEditWidth, 31, FALSE);
                MoveWindow(to, left + compactRangeLabelWidth
                        + compactRangeEditWidth,
                    compactRangeTop + 5, compactRangeToWidth, 24, FALSE);
                MoveWindow(maximum, left + compactRangeLabelWidth
                        + compactRangeEditWidth + compactRangeToWidth,
                    compactRangeTop, compactRangeEditWidth, 31, FALSE);
            };
            placeCompactRange(scopeLatitudeLabel_, scopeLatitudeMinimum_,
                scopeLatitudeToLabel_, scopeLatitudeMaximum_, mapLeft);
            placeCompactRange(scopeLongitudeLabel_, scopeLongitudeMinimum_,
                scopeLongitudeToLabel_, scopeLongitudeMaximum_,
                mapLeft + std::max(0, mapWidth - compactGroupWidth));
            const int compactStatusTop = mapRow1 + 114;
            MoveWindow(scopeStatus_, mapLeft, compactStatusTop,
                mapWidth, 24, FALSE);

            const int compactSelectionTop = mapRow1 + 144;
            MoveWindow(uniformCountLabel_, mapLeft,
                compactSelectionTop + 5, 34, 24, FALSE);
            MoveWindow(uniformCountEdit_, mapLeft + 34,
                compactSelectionTop, 40, 31, FALSE);
            MoveWindow(minimumDistanceLabel_, mapLeft + 82,
                compactSelectionTop + 5, 40, 24, FALSE);
            MoveWindow(minimumDistanceEdit_, mapLeft + 122,
                compactSelectionTop, 40, 31, FALSE);
            const int compactSelectionSecondTop = compactSelectionTop + 38;
            MoveWindow(maximumNeighborDistanceLabel_, mapLeft,
                compactSelectionSecondTop + 5, 44, 24, FALSE);
            MoveWindow(maximumNeighborDistanceEdit_, mapLeft + 44,
                compactSelectionSecondTop, 40, 31, FALSE);
            const int compactSelectionButtonsTop = mapRow1 + 220;
            const int compactSelectionButtonWidth = std::max(
                1, (mapWidth - compactGap) / 2);
            MoveWindow(uniformSelectButton_, mapLeft,
                compactSelectionButtonsTop, compactSelectionButtonWidth,
                31, FALSE);
            MoveWindow(randomSelectButton_,
                mapLeft + compactSelectionButtonWidth + compactGap,
                compactSelectionButtonsTop,
                std::max(1, mapWidth - compactSelectionButtonWidth - compactGap),
                31, FALSE);
        } else {
            SetWindowTextW(applyScopeButton_, L"应用范围");
            SetWindowTextW(mapRectangleScopeButton_, L"地图框选");
            SetWindowTextW(clearScopeButton_, L"清除范围");
            SetWindowTextW(scopeLatitudeLabel_, L"纬度");
            SetWindowTextW(scopeLongitudeLabel_, L"经度");
            SetWindowTextW(minimumDistanceLabel_, L"最小 km");
            SetWindowTextW(maximumNeighborDistanceLabel_, L"最大 km");
            const int mapGap = 6;
            const int countryLabelWidth = 34;
            const int countryWidth = std::clamp(mapWidth / 3, 96, 124);
            const int mapActionWidth = std::max(
                58, (mapWidth - countryLabelWidth - countryWidth - mapGap * 3) / 3);
            MoveWindow(scopeCountryLabel_, mapLeft, mapRow1 + 5,
                countryLabelWidth, 24, FALSE);
            MoveWindow(scopeCountry_, mapLeft + countryLabelWidth,
                mapRow1, countryWidth, 240, FALSE);
            int mapX = mapLeft + countryLabelWidth + countryWidth + mapGap;
            MoveWindow(applyScopeButton_, mapX, mapRow1,
                mapActionWidth, 31, FALSE);
            mapX += mapActionWidth + mapGap;
            MoveWindow(mapRectangleScopeButton_, mapX, mapRow1,
                mapActionWidth, 31, FALSE);
            mapX += mapActionWidth + mapGap;
            MoveWindow(clearScopeButton_, mapX, mapRow1,
                mapActionWidth, 31, FALSE);

            const int mapRow2 = mapRow1 + 38;
            int rangeX = mapLeft;
            constexpr int rangeLabelWidth = 28;
            constexpr int rangeEditWidth = 48;
            MoveWindow(scopeLatitudeLabel_, rangeX, mapRow2 + 5,
                rangeLabelWidth, 24, FALSE);
            rangeX += rangeLabelWidth;
            MoveWindow(scopeLatitudeMinimum_, rangeX, mapRow2,
                rangeEditWidth, 31, FALSE);
            rangeX += rangeEditWidth;
            MoveWindow(scopeLatitudeToLabel_, rangeX, mapRow2 + 5,
                16, 24, FALSE);
            rangeX += 16;
            MoveWindow(scopeLatitudeMaximum_, rangeX, mapRow2,
                rangeEditWidth, 31, FALSE);
            rangeX += rangeEditWidth + 12;
            MoveWindow(scopeLongitudeLabel_, rangeX, mapRow2 + 5,
                rangeLabelWidth, 24, FALSE);
            rangeX += rangeLabelWidth;
            MoveWindow(scopeLongitudeMinimum_, rangeX, mapRow2,
                rangeEditWidth, 31, FALSE);
            rangeX += rangeEditWidth;
            MoveWindow(scopeLongitudeToLabel_, rangeX, mapRow2 + 5,
                16, 24, FALSE);
            rangeX += 16;
            MoveWindow(scopeLongitudeMaximum_, rangeX, mapRow2,
                rangeEditWidth, 31, FALSE);
            MoveWindow(scopeStatus_, mapLeft, mapRow2 + 36,
                mapWidth, 24, FALSE);

            const int selectionRow = mapRow1 + 96;
            int selectionX = mapLeft;
            MoveWindow(uniformCountLabel_, selectionX, selectionRow + 5,
                34, 24, FALSE);
            selectionX += 34;
            MoveWindow(uniformCountEdit_, selectionX, selectionRow,
                46, 31, FALSE);
            selectionX += 52;
            MoveWindow(minimumDistanceLabel_, selectionX, selectionRow + 5,
                40, 24, FALSE);
            selectionX += 40;
            MoveWindow(minimumDistanceEdit_, selectionX, selectionRow,
                46, 31, FALSE);
            selectionX += 52;
            MoveWindow(maximumNeighborDistanceLabel_, selectionX,
                selectionRow + 5, 44, 24, FALSE);
            selectionX += 44;
            MoveWindow(maximumNeighborDistanceEdit_, selectionX, selectionRow,
                46, 31, FALSE);
            const int selectionButtonsTop = selectionRow + 38;
            const int selectionButtonWidth = std::max(100, (mapWidth - 6) / 2);
            MoveWindow(uniformSelectButton_, mapLeft, selectionButtonsTop,
                selectionButtonWidth, 31, FALSE);
            MoveWindow(randomSelectButton_, mapLeft + selectionButtonWidth + 6,
                selectionButtonsTop, std::max(100, mapWidth - selectionButtonWidth - 6),
                31, FALSE);
        }

        MoveWindow(map_, mapLeft, metrics.mapTop, mapWidth,
            std::max(100, height - metrics.mapTop - 18), FALSE);

        // MoveWindow/SetWindowPos can restore WS_VISIBLE on a child that was
        // hidden before a resize (notably when the child owns native common
        // controls). Re-assert the page visibility after every layout pass so
        // the inactive precision page can never paint over observation data.
        ShowWindow(preciseProductView_, preciseMode_ ? SW_SHOW : SW_HIDE);

        if (webUi_ && (webUiStarting_ || webUi_->IsReady())) {
            // HWND-based Win32 controls are always composited above an HWND
            // WebView2 controller on some systems, regardless of creation
            // order.  Hiding the captured native hierarchy is therefore the
            // only reliable way to avoid two complete interfaces overlapping.
            for (const HWND control : nativeControls_) {
                if (control != nullptr && IsWindow(control)) {
                    ShowWindow(control, SW_HIDE);
                }
            }
            webUi_->Resize(RECT{0, 0, width, height});
        }

        SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(window_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    void PaintBackground(const HDC deviceContext) const {
        RECT client{};
        GetClientRect(window_, &client);
        FillSolidRect(deviceContext, client, kWindowBackground);
        if (webUiStarting_) {
            // The first native frame uses the same embedded MARK artwork as
            // the title bar; it needs neither WebView2 nor external image files.
            const int savedContext = SaveDC(deviceContext);
            const int width = std::max(1, static_cast<int>(client.right));
            const int height = std::max(1, static_cast<int>(client.bottom));
            FillSolidRect(deviceContext, client, RGB(241, 244, 245));

            const int cardWidth = std::min(480, std::max(320, width - 48));
            const int cardHeight = 300;
            const int cardLeft = (width - cardWidth) / 2;
            const int cardTop = std::max(24, (height - cardHeight) / 2);
            const HBRUSH shadowBrush = CreateSolidBrush(RGB(223, 231, 236));
            const HGDIOBJ oldBrush = SelectObject(deviceContext, shadowBrush);
            const HGDIOBJ oldPen = SelectObject(
                deviceContext, GetStockObject(NULL_PEN));
            RoundRect(deviceContext, cardLeft + 3, cardTop + 5,
                cardLeft + cardWidth + 3, cardTop + cardHeight + 5, 24, 24);
            SelectObject(deviceContext, oldBrush);
            DeleteObject(shadowBrush);

            const HBRUSH cardBrush = CreateSolidBrush(RGB(255, 255, 255));
            const HPEN cardPen = CreatePen(PS_SOLID, 1, RGB(220, 228, 232));
            SelectObject(deviceContext, cardBrush);
            SelectObject(deviceContext, cardPen);
            RoundRect(deviceContext, cardLeft, cardTop,
                cardLeft + cardWidth, cardTop + cardHeight, 24, 24);
            SelectObject(deviceContext, oldBrush);
            SelectObject(deviceContext, oldPen);
            DeleteObject(cardBrush);
            DeleteObject(cardPen);

            constexpr int iconSize = 96;
            static const HICON markIcon = static_cast<HICON>(LoadImageW(
                instance_, MAKEINTRESOURCEW(IDI_MARK_APP), IMAGE_ICON,
                iconSize, iconSize, LR_SHARED));
            if (markIcon != nullptr) {
                DrawIconEx(deviceContext, (width - iconSize) / 2,
                    cardTop + 24, markIcon, iconSize, iconSize,
                    0, nullptr, DI_NORMAL);
            }

            SetBkMode(deviceContext, TRANSPARENT);
            SetTextColor(deviceContext, RGB(23, 54, 79));
            SelectObject(deviceContext, titleFont_ != nullptr
                ? titleFont_ : GetStockObject(DEFAULT_GUI_FONT));
            RECT titleRect{cardLeft + 24, cardTop + 129,
                cardLeft + cardWidth - 24, cardTop + 168};
            DrawTextW(deviceContext, L"MARK", -1, &titleRect,
                DT_CENTER | DT_SINGLELINE | DT_VCENTER);

            SelectObject(deviceContext, bodyFont_ != nullptr
                ? bodyFont_ : GetStockObject(DEFAULT_GUI_FONT));
            SetTextColor(deviceContext, RGB(80, 106, 123));
            RECT taglineRect{cardLeft + 24, cardTop + 173,
                cardLeft + cardWidth - 24, cardTop + 200};
            DrawTextW(deviceContext, L"Metadata and rules for GNSS data", -1,
                &taglineRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

            SelectObject(deviceContext, smallFont_ != nullptr
                ? smallFont_ : GetStockObject(DEFAULT_GUI_FONT));
            SetTextColor(deviceContext, RGB(109, 128, 141));
            RECT statusRect{cardLeft + 24, cardTop + 209,
                cardLeft + cardWidth - 24, cardTop + 235};
            DrawTextW(deviceContext, L"Initializing the workbench...", -1,
                &statusRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

            const int trackWidth = std::min(260, cardWidth - 90);
            const int trackLeft = (width - trackWidth) / 2;
            const int trackTop = cardTop + 253;
            const HBRUSH trackBrush = CreateSolidBrush(RGB(228, 235, 238));
            SelectObject(deviceContext, GetStockObject(NULL_PEN));
            SelectObject(deviceContext, trackBrush);
            RoundRect(deviceContext, trackLeft, trackTop,
                trackLeft + trackWidth, trackTop + 5, 5, 5);
            SelectObject(deviceContext, oldBrush);
            DeleteObject(trackBrush);

            const int segmentWidth = std::max(46, trackWidth / 4);
            const int travel = trackWidth - segmentWidth;
            const int phase = static_cast<int>(loadingAnimationTick_ % 54);
            const int mirroredPhase = phase <= 27 ? phase : 54 - phase;
            const int segmentLeft = trackLeft + (travel * mirroredPhase) / 27;
            const HBRUSH segmentBrush = CreateSolidBrush(RGB(233, 162, 59));
            SelectObject(deviceContext, segmentBrush);
            RoundRect(deviceContext, segmentLeft, trackTop,
                segmentLeft + segmentWidth, trackTop + 5, 5, 5);
            SelectObject(deviceContext, oldBrush);
            DeleteObject(segmentBrush);
            RestoreDC(deviceContext, savedContext);
            return;
        }
        // WebView2 owns the visible workbench after its first frame. Keep the
        // native surface plain when the compositor temporarily uncovers it.
        if (webUi_) return;
        const int width = std::max(
            1, static_cast<int>(client.right - client.left));
        const int height = std::max(
            1, static_cast<int>(client.bottom - client.top));
        const auto metrics = ComputeMainLayoutMetrics(
            width, height, networksExpanded_);
        const bool compact = height < kMainCompactHeight;

        // The mode switch is part of the header now, so the whole client area
        // remains available to the query surface.  A shallow white header
        // keeps the title and segmented switch visually separate from the
        // denser cards below without reserving a permanent sidebar.
        FillSolidRect(deviceContext,
            RECT{0, 0, width, 108}, kSurface);
        SetBkMode(deviceContext, TRANSPARENT);
        // Header status is intentionally derived from the same knowledge-base
        // state used by the query logic.
        FillSolidRect(deviceContext,
            RECT{metrics.contentLeft, 104, metrics.contentRight, 106}, kAccent);
        const COLORREF statusColour = knowledgeBaseStatusLoading_
            ? kWarning
            : knowledgeBaseNetworks_ != 0 ? kSuccess : kWarning;
        // Keep the status badge below the subtitle and away from the header
        // mode switch; placing it at the far right made the two controls
        // overlap on wide windows.
        const int statusX = metrics.contentLeft + 4;
        const HBRUSH statusBrush = CreateSolidBrush(statusColour);
        if (statusBrush != nullptr) {
            const HGDIOBJ oldStatusBrush = SelectObject(
                deviceContext, statusBrush);
            Ellipse(deviceContext, statusX, 82, statusX + 10, 92);
            SelectObject(deviceContext, oldStatusBrush);
            DeleteObject(statusBrush);
        }
        SetTextColor(deviceContext, kMutedText);
        RECT statusRect{statusX + 16, 76, statusX + 190, 100};
        const wchar_t* statusText = knowledgeBaseStatusLoading_
            ? L"正在读取本地索引"
            : knowledgeBaseNetworks_ != 0 ? L"本地索引已就绪" : L"等待索引状态";
        DrawTextW(deviceContext, statusText, -1, &statusRect,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

        if (preciseMode_) return;

        const int panelLeft = metrics.contentLeft;
        const int panelRight = metrics.panelRight;
        const int mapLeft = metrics.mapLeft;
        const int mapRight = metrics.mapLeft + metrics.mapWidth;
        DrawSectionFrame(deviceContext,
            RECT{panelLeft - 8, metrics.contentTop,
                 panelRight + 8,
                 metrics.contentTop + (compact ? 62 : 58)},
            L"数据源", smallFont_);
        if (compact) {
            // The compact view uses one continuous query workspace.  Separate
            // canvas captions would be only a few pixels apart at the
            // minimum track height and would compete with the native labels.
            DrawSectionFrame(deviceContext,
                RECT{panelLeft - 8, metrics.quickTop - 4,
                     panelRight + 8, metrics.listBottom + 8},
                L"", smallFont_);
            DrawSectionFrame(deviceContext,
                RECT{panelLeft - 8, metrics.actionTop - 4,
                     panelRight + 8,
                     std::max(metrics.actionTop + 128, height - 12)},
                L"", smallFont_);
        } else {
            DrawSectionFrame(deviceContext,
                RECT{panelLeft - 8, metrics.quickTop - 4,
                     panelRight + 8,
                     metrics.queryStatusTop + metrics.queryStatusHeight + 10},
                L"查询条件", smallFont_);
            DrawSectionFrame(deviceContext,
                RECT{panelLeft - 8, metrics.filterLabelTop - 26,
                     panelRight + 8, metrics.listBottom + 8},
                L"测站结果", smallFont_);
            DrawSectionFrame(deviceContext,
                RECT{panelLeft - 8, metrics.actionTop - 26,
                     panelRight + 8, metrics.actionTop + 176},
                L"下载任务", smallFont_);
        }
        DrawSectionFrame(deviceContext,
            RECT{mapLeft - 8, metrics.contentTop,
                 mapRight + 8, metrics.mapTop - 4},
            L"地图范围", smallFont_);
        DrawSectionFrame(deviceContext,
            RECT{mapLeft - 8, metrics.mapTop - 4,
                 mapRight + 8, height - 12},
            L"", smallFont_);
        FillSolidRect(deviceContext,
            RECT{mapLeft - 14, metrics.contentTop, mapLeft - 13,
                 std::max(metrics.contentTop + 100, height - 12)},
            RGB(225, 231, 238));

        if (networksExpanded_) {
            // Paint the expanded source chooser last so it reads as a modal
            // card floating above the query form.  The checkbox HWNDs are
            // promoted to the same z-order in Layout().
            DrawSectionFrame(deviceContext,
                RECT{panelLeft, metrics.contentTop,
                     panelLeft + metrics.networkOverlayWidth,
                     metrics.networkOverlayBottom},
                L"选择数据源（42 个站网）", smallFont_);
        }
    }

    void SetPreciseMode(const bool precise) {
        // Transfers keep running in their own panes.  The task dock follows
        // the selected pane, so pause and cancel remain reachable on return.
        if (precise != preciseMode_
            && (querying_ || siteLogLoading_)) {
            SetWindowLongPtrW(
                observationModeButton_, GWLP_USERDATA,
                preciseMode_ ? 0 : 1);
            SetWindowLongPtrW(
                preciseModeButton_, GWLP_USERDATA,
                preciseMode_ ? 1 : 0);
            InvalidateRect(observationModeButton_, nullptr, TRUE);
            InvalidateRect(preciseModeButton_, nullptr, TRUE);
            SetOperationStatusText(
                querying_
                    ? L"测站查询进行中；请先等待查询完成，再切换页面。"
                    : L"正在补充 Site Log 元数据；请稍候再切换页面。");
            SetBusyControls();
            return;
        }
        if (precise) {
            SetRectangleSelectionMode(false);
        }
        preciseMode_ = precise;
        // A pending filter refresh belongs to the observation page.  Cancel
        // it when switching panes so a delayed callback cannot wake a hidden
        // list while the precision page is being shown.
        KillTimer(window_, kObservationFilterTimer);
        SetWindowLongPtrW(
            observationModeButton_, GWLP_USERDATA, precise ? 0 : 1);
        SetWindowLongPtrW(
            preciseModeButton_, GWLP_USERDATA, precise ? 1 : 0);
        InvalidateRect(observationModeButton_, nullptr, TRUE);
        InvalidateRect(preciseModeButton_, nullptr, TRUE);
        const int observationShow = precise ? SW_HIDE : SW_SHOW;
        for (const HWND control : {
                 networkLabel_, networkPanelToggleButton_,
                 selectKnowledgeBaseNetworksButton_,
                 igsCheck_, mgexCheck_, ngsCheck_, epnCheck_,
                 hkCheck_, gaGdcCheck_, rgnaCheck_, sonelCheck_, trigNetCheck_,
                 cutCheck_, csnCheck_, eposIcelandCheck_, rbmcCheck_,
                 ramsacCheck_, regnaRouCheck_, cacsCheck_, rgpCheck_,
                 sopacGarnerCheck_, magnaEcoCheck_, panamaCorsCheck_,
                 hondurasCorsCheck_, eposGnssCheck_, grefCheck_, osNetCheck_,
                 ergnssCheck_, renepCheck_, dateLabel_,
                 geoNetNzCheck_,
                 ringCheck_, aposCheck_, robBelgiumCheck_, dpgaCheck_,
                 catNetCheck_, spsLuxCheck_, kasiNgiiCheck_, renagCheck_,
                  nsgiCheck_, noaGnssCheck_, redgaeCheck_, geoNetJapanCheck_,
                 dlabCheck_, magnetCheck_, pangaCwuCheck_,
                 datePicker_, dateRangeToLabel_, endDatePicker_,
                 quickSchemeLabel_, quickScheme_, queryButton_,
                 advancedFilterButton_, dateAdvancedFilterButton_,
                 queryStatus_, filterLabel_, filterEdit_, stationList_,
                 selectionSummary_, selectAllButton_, clearSelectionButton_,
                 scopeCountryLabel_, scopeCountry_, scopeLatitudeLabel_,
                 scopeLatitudeMinimum_, scopeLatitudeToLabel_,
                 scopeLatitudeMaximum_, scopeLongitudeLabel_,
                 scopeLongitudeMinimum_, scopeLongitudeToLabel_,
                 scopeLongitudeMaximum_, applyScopeButton_,
                 mapRectangleScopeButton_, clearScopeButton_, scopeStatus_,
                 uniformCountLabel_, uniformCountEdit_,
                 minimumDistanceLabel_, minimumDistanceEdit_,
                 maximumNeighborDistanceLabel_, maximumNeighborDistanceEdit_,
                  uniformSelectButton_, randomSelectButton_,
                 folderLabel_, folderEdit_, browseButton_, downloadButton_,
                 pauseDownloadButton_,
                 progress_, operationStatus_, map_}) {
            ShowWindow(control, observationShow);
        }
        ApplyNetworkPanelVisibility();
        ShowWindow(preciseProductView_, precise ? SW_SHOW : SW_HIDE);
        SetWindowTextW(
            subtitle_,
            precise
                ? L"精密轨道、钟差、ERP/EOP、偏差、姿态、电离层、对流层、SINEX 与 ANTEX 一站式下载"
                : L"42 个站网/数据源可多选；可按任意起止日期、完整性和空间天气条件筛选测站");
        Layout();
        if (precise) {
            FocusPreciseProductView(preciseProductView_);
        } else {
            SetFocus(networksExpanded_
                ? networkPanelToggleButton_ : datePicker_);
        }
    }

    // Called by the precise-product child after a transfer starts/ends so the
    // parent mode buttons immediately reflect the child-owned busy state.
    void HandlePreciseBusyChanged() {
        SetBusyControls();
        PostWebUiState();
    }

    void UpdateObservationPeriodMode() {
        EnableWindow(
            endDatePicker_,
            !querying_ && !siteLogLoading_);
        SetWindowTextW(queryButton_, L"查询测站");
        SetOperationStatusText(
            L"按起止 UTC 日期读取本地知识库，并按日期、完整性与测站条件筛选。");
    }

    // Loading the compact bitmap database is intentionally done before the
    // first user query.  GetObservationSearchIndexStatus() uses the same
    // process-wide immutable cache as QueryObservationSearchIndex(), so this
    // background read turns the first interactive range query into a warm
    // in-memory operation without delaying window creation.
    void PreloadObservationSearchIndex() {
        if (observationSearchPreloadThread_.joinable()) return;
        const auto indexPath = DefaultObservationSearchIndexPath();
        observationSearchPreloadThread_ = std::jthread(
            [indexPath](const std::stop_token stopToken) {
                if (stopToken.stop_requested()) return;
                (void)GetObservationSearchIndexStatus(indexPath, false, true);
            });
    }

    void UpdateAdvancedFilterButtonLabel() const {
        const bool active = advancedFilter_.completenessMode
                != AdvancedFilterState::CompletenessMode::Ignore
            || advancedFilter_.dailyCoverageMode != DailyCoverageMode::Ignore
            || !advancedFilter_.sampleInterval.empty()
            || advancedFilter_.rinexVersion != 0
            || quickFrameSelection_ != ReferenceFrameSelection::None
            || !advancedFilter_.systemsInclude.empty()
            || !advancedFilter_.systemsExclude.empty()
            || !advancedFilter_.receiverInclude.empty()
            || !advancedFilter_.receiverExclude.empty()
            || !advancedFilter_.antennaInclude.empty()
            || !advancedFilter_.antennaExclude.empty()
            || !advancedFilter_.signalCodesInclude.empty()
            || !advancedFilter_.signalCodesExclude.empty()
            || (advancedFilter_.gimStationMode != "ignore"
                && !advancedFilter_.gimProviders.empty())
            || advancedFilter_.observationCompletenessEnabled
            || advancedFilter_.latencyEnabled
            || advancedFilter_.positionTimeSeriesEnabled
            || advancedFilter_.residualsEnabled
            || advancedFilter_.meanMultipathEnabled
            || advancedFilter_.cycleSlipsEnabled;
        SetWindowTextW(
            advancedFilterButton_,
            active ? L"测站高级筛选*" : L"测站高级筛选…");
    }

    void UpdateDateAdvancedFilterButtonLabel() const {
        const bool active = dateAdvancedFilter_.kpMaxFilter.mode
                != SpaceWeatherFilterMode::Ignore
            || dateAdvancedFilter_.dstMinFilter.mode
                != SpaceWeatherFilterMode::Ignore
            || dateAdvancedFilter_.f107Filter.mode
                != SpaceWeatherFilterMode::Ignore;
        SetWindowTextW(
            dateAdvancedFilterButton_,
            active ? L"日期筛选*" : L"日期高级筛选…");
    }

    void UpdateNetworkPanelToggleLabel() const {
        if (networkPanelToggleButton_ == nullptr) return;
        if (knowledgeBaseStatusLoading_ || knowledgeBaseTogglePending_) {
            SetWindowTextW(networkPanelToggleButton_, L"正在读取站网知识库…");
            return;
        }
        std::size_t available = 0;
        std::size_t checked = 0;
        for (const auto& [control, network] : NetworkControls()) {
            if (control == nullptr || !HasNetwork(knowledgeBaseNetworks_, network)) {
                continue;
            }
            ++available;
            if (HasNetwork(selectedNetworkMask_, network)) {
                ++checked;
            }
        }
        const wchar_t* prefix = networksExpanded_
            ? L"收起数据源"
            : L"选择数据源";
        SetWindowTextW(
            networkPanelToggleButton_,
            std::format(
                L"{} · {}/{} 可用", prefix, checked, available).c_str());
    }

    void ApplyNetworkPanelVisibility() const {
        if (networkPanelToggleButton_ == nullptr) return;
        // The explanatory label is replaced by the compact toggle button and
        // the painted section caption.  Hiding it avoids a second line that
        // could overlap the checkbox grid when the panel is expanded.
        ShowWindow(networkLabel_, SW_HIDE);
        ShowWindow(dateLabel_, SW_HIDE);
        const int networkShow =
            (!preciseMode_ && networksExpanded_) ? SW_SHOW : SW_HIDE;
        for (const auto& [control, network] : NetworkControls()) {
            (void)network;
            ShowWindow(control, networkShow);
        }
        // The source chooser is a modal-looking floating card.  Hide the
        // observation form underneath it while it is open; otherwise the
        // gaps between checkbox cells expose clickable date/filter controls
        // below the card and make the z-order feel broken.
        const int formShow =
            (!preciseMode_ && !networksExpanded_) ? SW_SHOW : SW_HIDE;
        for (const HWND control : {
                 datePicker_, dateRangeToLabel_, endDatePicker_,
                 quickSchemeLabel_, quickScheme_, queryButton_,
                 advancedFilterButton_, dateAdvancedFilterButton_,
                 queryStatus_, filterLabel_, filterEdit_, stationList_,
                 selectionSummary_, selectAllButton_, clearSelectionButton_,
                 scopeCountryLabel_, scopeCountry_, scopeLatitudeLabel_,
                 scopeLatitudeMinimum_, scopeLatitudeToLabel_,
                 scopeLatitudeMaximum_, scopeLongitudeLabel_,
                 scopeLongitudeMinimum_, scopeLongitudeToLabel_,
                 scopeLongitudeMaximum_, applyScopeButton_,
                 mapRectangleScopeButton_, clearScopeButton_, scopeStatus_,
                 uniformCountLabel_, uniformCountEdit_,
                 minimumDistanceLabel_, minimumDistanceEdit_,
                 maximumNeighborDistanceLabel_, maximumNeighborDistanceEdit_,
                 uniformSelectButton_, randomSelectButton_, folderLabel_,
                 folderEdit_, browseButton_, downloadButton_,
                 pauseDownloadButton_, progress_, operationStatus_}) {
            ShowWindow(control, formShow);
        }
        ShowWindow(
            networkPanelToggleButton_, preciseMode_ ? SW_HIDE : SW_SHOW);
        ShowWindow(
            selectKnowledgeBaseNetworksButton_, preciseMode_ ? SW_HIDE : SW_SHOW);
        if (webUi_ && (webUiStarting_ || webUi_->IsReady())) {
            for (const HWND control : nativeControls_) {
                if (control != nullptr && IsWindow(control)) {
                    ShowWindow(control, SW_HIDE);
                }
            }
        }
    }

    void ToggleNetworkPanel() {
        if (preciseMode_ || !IsWindowVisible(networkPanelToggleButton_)
            || querying_ || siteLogLoading_) return;
        // A pending filter refresh belongs to the visible station list.  Do
        // not let a delayed timer rebuild that list while the source chooser
        // is covering it.
        KillTimer(window_, kObservationFilterTimer);
        networksExpanded_ = !networksExpanded_;
        ApplyNetworkPanelVisibility();
        UpdateNetworkPanelToggleLabel();
        Layout();
        SetFocus(networkPanelToggleButton_);
        InvalidateRect(window_, nullptr, TRUE);
    }

    bool HasMetadataAdvancedFilter() const {
        return !advancedFilter_.systemsInclude.empty()
            || !advancedFilter_.systemsExclude.empty()
            || !advancedFilter_.receiverInclude.empty()
            || !advancedFilter_.receiverExclude.empty()
            || !advancedFilter_.antennaInclude.empty()
            || !advancedFilter_.antennaExclude.empty()
            || !advancedFilter_.signalCodesInclude.empty()
            || !advancedFilter_.signalCodesExclude.empty();
    }

    // Site Log records are enriched on a worker after the local availability
    // query has been published.  While a metadata criterion is active, the
    // visible station list is intentionally provisional: stations without a
    // log are kept visible until the worker finishes, then removed if they do
    // not match.  Do not let a range download or a bulk-select action race
    // that pass and accidentally include a station which will be filtered
    // out moments later.
    bool MetadataFilterPending() const {
        return HasMetadataAdvancedFilter() && !siteLogEnrichmentComplete_;
    }

    void ApplyAdvancedFilterToScope() {
        if (geographicScope_.size() != stations_.size()) {
            geographicScope_.assign(stations_.size(), true);
        }
        for (std::size_t index = 0; index < stations_.size(); ++index) {
            // Metadata is loaded asynchronously.  Keep the geographic
            // candidates visible while that pass is pending; once complete,
            // a non-empty metadata criterion requires an actual Site Log.
            const bool metadataPending = HasMetadataAdvancedFilter()
                && !siteLogEnrichmentComplete_
                && !stations_[index].hasSiteLog;
            stations_[index].inSelectionScope = geographicScope_[index]
                && (metadataPending || MatchesAdvancedFilter(stations_[index]));
            if (!stations_[index].inSelectionScope) {
                stations_[index].selected = false;
            }
        }
    }

    void ShowAdvancedFilterDialog() {
        if (querying_ || siteLogLoading_) return;
        RegisterAdvancedDialogClass(instance_);
        AdvancedDialogContext context;
        context.target = &advancedFilter_;
        context.font = bodyFont_;
        CollectAdvancedChoiceOptions(
            stations_,
            currentPeriodFiles_,
            context.systemOptions,
            context.receiverOptions,
            context.antennaOptions,
            context.signalOptions);
        // The compact index contains every historical equipment segment,
        // including models that are not effective on the representative day
        // currently shown in the table.  Populate the checklist from that
        // local history so the user never has to type a receiver/antenna name
        // and can select equipment installed midway through a long range.
        const NetworkSelection choiceSelection = GetNetworkSelection();
        const SYSTEMTIME choiceStartSystem = queryStartDate_;
        const SYSTEMTIME choiceEndSystem = queryEndDate_;
        if (choiceSelection.Any()) {
            QueryDate choiceStart = MakeQueryDate(
                choiceStartSystem.wYear,
                choiceStartSystem.wMonth,
                choiceStartSystem.wDay);
            const QueryDate choiceEnd = MakeQueryDate(
                choiceEndSystem.wYear,
                choiceEndSystem.wMonth,
                choiceEndSystem.wDay);
            const auto indexedChoices = QueryObservationSearchMetadataChoices(
                DefaultObservationSearchIndexPath(),
                choiceStart,
                choiceEnd,
                choiceSelection,
                true);
            const auto appendIndexed = [](
                std::vector<AdvancedChoiceOption>& target,
                const std::vector<std::string>& values) {
                std::unordered_set<std::string> seen;
                seen.reserve(target.size() + values.size());
                for (const auto& option : target) {
                    seen.insert(ChoiceValueKey(option.value));
                }
                for (const auto& value : values) {
                    AddAdvancedChoiceOption(target, seen, value);
                }
                SortAdvancedChoiceOptions(target);
            };
            const auto appendIndexedSystems = [](
                std::vector<AdvancedChoiceOption>& target,
                const std::vector<std::string>& values) {
                std::unordered_set<std::string> seen;
                seen.reserve(target.size() + values.size());
                for (const auto& option : target) {
                    seen.insert(ChoiceValueKey(option.value));
                }
                for (const auto& value : values) {
                    for (const auto& token :
                        CanonicalSatelliteSystemTokens(value)) {
                        AddAdvancedChoiceOption(
                            target,
                            seen,
                            token,
                            SatelliteSystemDisplayLabel(token));
                    }
                }
                SortAdvancedChoiceOptions(target);
            };
            if (indexedChoices.error.empty()) {
                appendIndexedSystems(
                    context.systemOptions, indexedChoices.systems);
                appendIndexed(
                    context.receiverOptions, indexedChoices.receivers);
                appendIndexed(
                    context.antennaOptions, indexedChoices.antennas);
                std::unordered_set<std::string> seenSignals;
                seenSignals.reserve(context.signalOptions.size()
                    + indexedChoices.signals.size());
                for (const auto& option : context.signalOptions) {
                    seenSignals.insert(ChoiceValueKey(option.value));
                }
                for (const auto& value : indexedChoices.signals) {
                    AddAdvancedChoiceOption(
                        context.signalOptions,
                        seenSignals,
                        value,
                        SignalChoiceDisplayLabel(value));
                }
                SortAdvancedChoiceOptions(context.signalOptions);
            }
        }
        RECT ownerRect{};
        GetWindowRect(window_, &ownerRect);
        constexpr int dialogWidth = 470;
        // The signal include/exclude row extends the dialog by one section;
        // leave a client-area margin below the buttons so the controls are
        // not clipped by the caption/border on smaller Windows themes.
        constexpr int dialogHeight = 650;
        const int ownerWidth = ownerRect.right - ownerRect.left;
        const int ownerHeight = ownerRect.bottom - ownerRect.top;
        const int left = ownerRect.left + std::max(0, (ownerWidth - dialogWidth) / 2);
        const int top = ownerRect.top + std::max(0, (ownerHeight - dialogHeight) / 2);
        const HWND dialog = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
            kAdvancedDialogClassName,
            L"高级测站筛选",
            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
            left,
            top,
            dialogWidth,
            dialogHeight,
            window_,
            nullptr,
            instance_,
            &context);
        if (dialog == nullptr) {
            NativeUiLanguage::ShowMessageBox(window_, L"无法打开高级筛选窗口。", kApplicationTitle,
                        MB_OK | MB_ICONERROR);
            return;
        }
        EnableWindow(window_, FALSE);
        SetActiveWindow(dialog);
        MSG message{};
        while (IsWindow(dialog)) {
            const int status = GetMessageW(&message, nullptr, 0, 0);
            if (status <= 0) {
                if (status == 0) PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            if (!IsDialogMessageW(dialog, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (IsWindow(dialog)) DestroyWindow(dialog);
        EnableWindow(window_, TRUE);
        SetActiveWindow(window_);
        SetFocus(advancedFilterButton_);
        if (context.accepted) {
            UpdateAdvancedFilterButtonLabel();
            if (!stations_.empty()) {
                // The availability result may have been filtered with the
                // previous completeness/sample/RINEX settings.  Re-run the
                // local query so changing any advanced condition can recover
                // rows that were excluded by the previous condition.  This
                // remains offline; no archive/network request is made.
                SetOperationStatusText(
                    L"高级筛选已应用，正在重新读取本地日期索引并按新条件筛选…");
                StartQuery();
            } else {
                ApplyAdvancedFilterToScope();
                PopulateStationList();
                MapViewRefresh(map_);
                SetOperationStatusText(
                    L"高级筛选已保存；下一次查询会使用这些条件。");
            }
        }
    }

    void ShowDateAdvancedFilterDialog() {
        if (querying_ || siteLogLoading_) return;
        DateAdvancedFilterState candidate = dateAdvancedFilter_;
        const bool accepted = ::ShowDateAdvancedFilterDialog(
            window_, instance_, bodyFont_, candidate);
        // The modal helper restores activation to the main window; explicitly
        // return focus to the control that opened it so keyboard users do not
        // land on a disabled/hidden child after closing the dialog.
        SetFocus(dateAdvancedFilterButton_);
        if (!accepted) {
            return;
        }
        dateAdvancedFilter_ = std::move(candidate);
        UpdateDateAdvancedFilterButtonLabel();

        // Show a preview immediately when the user applies the dialog, even
        // before the first station query has been run.  A subsequent query
        // replaces this text with the final station summary and the same
        // retained-date list.
        const SYSTEMTIME previewStartSystem = queryStartDate_;
        const SYSTEMTIME previewEndSystem = queryEndDate_;
        {
            QueryDate previewStart = MakeQueryDate(
                previewStartSystem.wYear,
                previewStartSystem.wMonth,
                previewStartSystem.wDay);
            QueryDate previewEnd = MakeQueryDate(
                previewEndSystem.wYear,
                previewEndSystem.wMonth,
                previewEndSystem.wDay);
            if (previewStart.dayOfYear > 0 && previewEnd.dayOfYear > 0
                && FormatDate(previewEnd) >= FormatDate(previewStart)) {
                const auto preview = FormatDateAdvancedSelection(
                    dateAdvancedFilter_, previewStart, previewEnd);
                if (!preview.empty()) SetQuerySummaryText(preview);
            }
        }

        // Re-run the current query immediately, just like the station
        // advanced filter.  The query worker applies the date predicate before
        // counting station completeness, so the resulting station list and
        // download rows use exactly the retained dates.
        if (!stations_.empty()) {
            SetOperationStatusText(
                L"日期高级筛选已应用，正在按空间天气指标重新计算保留日期和测站…");
            StartQuery();
        } else {
            SetOperationStatusText(
                L"日期高级筛选已保存；下一次查询会按空间天气指标筛选日期。 ");
        }
    }

    void ApplyKnowledgeBaseStatus(
        const KnowledgeBaseStatusResult& result) {
        // A result from an older scan must never overwrite a newer snapshot.
        if (result.generation != knowledgeBaseStatusGeneration_) {
            return;
        }
        knowledgeBaseStatusLoading_ = false;
        knowledgeBaseNetworks_ = result.networks;
        knowledgeBaseStatusTime_ = std::chrono::steady_clock::now();
        for (const auto& [control, network] : NetworkControls()) {
            if (control != nullptr) InvalidateRect(control, nullptr, TRUE);
        }
        if (knowledgeBaseTogglePending_) {
            // The explicit toggle waits for the same asynchronous snapshot
            // used by startup/activation.  Applying it here keeps the click
            // handler non-blocking while preserving the "first click selects,
            // second click clears" semantics against the freshly scanned set.
            knowledgeBaseTogglePending_ = false;
            const bool overriddenByUser = selectionChangedWhileTogglePending_;
            selectionChangedWhileTogglePending_ = false;
            if (overriddenByUser) {
                // 扫描期间用户已经手动增删站网：用户意图优先，跳过这次重放，
                // 避免把刚点的切换覆盖掉。
                UpdateKnowledgeBaseToggleLabel();
                SetOperationStatusText(
                    L"已保留你手动调整的站网选择；本次全选/取消全选已跳过。");
            } else {
                // 勾选在点击时就已经应用过了；这里只在可用集合刷新后按同一意图重放。
                ApplyKnowledgeBaseToggleSelection(knowledgeBaseToggleClearIntent_);
            }
            initialNetworkSelectionApplied_ = true;
        } else if (!initialNetworkSelectionApplied_
                   && !networkSelectionTouched_) {
            // On first launch, make the normal query immediately useful:
            // select every network whose local SQLite index is available and
            // leave unavailable (red) networks unchecked.  Do this only once
            // and only when the user has not touched a checkbox while the
            // asynchronous status scan was running.
            std::size_t selected = 0;
            for (const auto& [control, network] : NetworkControls()) {
                if (control == nullptr) continue;
                const bool available = HasNetwork(
                    knowledgeBaseNetworks_, network);
                SendMessageW(
                    control,
                    BM_SETCHECK,
                    available ? BST_CHECKED : BST_UNCHECKED,
                    0);
                if (available) ++selected;
            }
            selectedNetworkMask_ = knowledgeBaseNetworks_;
            initialNetworkSelectionApplied_ = true;
            SetOperationStatusText(
                std::format(
                    L"已自动选择 {} 个具有本地知识库的站网；红色站网保持未选中。",
                    selected));
            UpdateKnowledgeBaseToggleLabel();
        } else {
            UpdateKnowledgeBaseToggleLabel();
        }
        // Publish the refreshed availability snapshot after it is applied.
        PostWebUiState();
    }

    void RefreshKnowledgeBaseStatus(const bool force = false) {
        if (igsCheck_ == nullptr) return;
        // WM_ACTIVATE can be delivered repeatedly while the user changes
        // focus between the list, map and another window.  Reopening every
        // SQLite file on the UI thread made the window appear to pause.  A
        // normal refresh and the explicit toggle action both scan in the
        // background and post one compact mask back to the window.  Keeping
        // the force path asynchronous is important: the large IGS database
        // can otherwise make a button click block the message loop.
        const auto now = std::chrono::steady_clock::now();
        if (!force
            && knowledgeBaseStatusTime_.time_since_epoch().count() != 0
            && now - knowledgeBaseStatusTime_ < std::chrono::seconds{10}) {
            return;
        }

        std::vector<ObservationNetwork> networks;
        networks.reserve(NetworkControls().size());
        for (const auto& [control, network] : NetworkControls()) {
            (void)control;
            networks.push_back(network);
        }

        if (knowledgeBaseStatusLoading_) {
            // A scan is already in flight.  A pending explicit toggle will be
            // applied by ApplyKnowledgeBaseStatus when that scan completes;
            // starting a second scan would only add contention and could
            // reintroduce a join on the UI thread.
            return;
        }

        knowledgeBaseStatusLoading_ = true;
        const auto generation = ++knowledgeBaseStatusGeneration_;
        const HWND target = window_;
        if (knowledgeBaseStatusThread_.joinable()) {
            // If loading_ is false the previous worker has already posted its
            // result and is normally finished; joining is therefore
            // instantaneous and makes ownership of the jthread explicit.
            knowledgeBaseStatusThread_.request_stop();
            knowledgeBaseStatusThread_.join();
        }
        UpdateKnowledgeBaseToggleLabel();
        knowledgeBaseStatusThread_ = std::jthread(
            [target, generation, networks = std::move(networks)](
                const std::stop_token stopToken) mutable {
                if (stopToken.stop_requested()) return;
                auto* result = new KnowledgeBaseStatusResult();
                result->generation = generation;
                result->networks = ScanKnowledgeBaseNetworks(
                    networks, stopToken);
                if (stopToken.stop_requested()
                    || !IsWindow(target)
                    || !PostMessageW(
                        target,
                        WM_IGS_KB_STATUS_FINISHED,
                        0,
                        reinterpret_cast<LPARAM>(result))) {
                    delete result;
                }
            });
    }

    // Keep the knowledge-base action genuinely toggleable.  The first click
    // checks every network whose local index is available; a second click
    // clears those same checkboxes.  Networks without a local index are
    // always left unchecked (and remain coloured red), so the button never
    // gives the impression that an unavailable source can be queried.
    void UpdateKnowledgeBaseToggleLabel() const {
        UpdateNetworkPanelToggleLabel();
        if (selectKnowledgeBaseNetworksButton_ == nullptr) return;
        const bool waitingForInitialAvailability = knowledgeBaseStatusLoading_
            && knowledgeBaseStatusTime_.time_since_epoch().count() == 0;
        if (waitingForInitialAvailability || knowledgeBaseTogglePending_) {
            SetWindowTextW(
                selectKnowledgeBaseNetworksButton_,
                L"正在读取知识库…");
            EnableWindow(selectKnowledgeBaseNetworksButton_, FALSE);
            return;
        }
        std::size_t available = 0;
        std::size_t checked = 0;
        for (const auto& [control, network] : NetworkControls()) {
            if (control == nullptr
                || !HasNetwork(knowledgeBaseNetworks_, network)) {
                continue;
            }
            ++available;
            if (HasNetwork(selectedNetworkMask_, network)) {
                ++checked;
            }
        }
        const bool allSelected = available > 0 && checked == available;
        SetWindowTextW(
            selectKnowledgeBaseNetworksButton_,
            allSelected ? L"取消全选所有站网" : L"全选所有站网");
        const BOOL canToggle = !querying_ && !siteLogLoading_ && !downloading_;
        EnableWindow(selectKnowledgeBaseNetworksButton_, canToggle);
    }

    void ApplyKnowledgeBaseToggleSelection(const bool clearSelection) {
        std::size_t available = 0;
        for (const auto& [control, network] : NetworkControls()) {
            if (control == nullptr
                || !HasNetwork(knowledgeBaseNetworks_, network)) {
                continue;
            }
            ++available;
        }
        int selected = 0;
        for (const auto& [control, network] : NetworkControls()) {
            const bool hasKnowledgeBase =
                HasNetwork(knowledgeBaseNetworks_, network);
            SendMessageW(
                control,
                BM_SETCHECK,
                hasKnowledgeBase && !clearSelection
                    ? BST_CHECKED
                    : BST_UNCHECKED,
                0);
            if (hasKnowledgeBase && !clearSelection) ++selected;
        }
        selectedNetworkMask_ = clearSelection ? 0 : knowledgeBaseNetworks_;
        UpdateKnowledgeBaseToggleLabel();
        const std::wstring message = available == 0
            ? L"当前没有可用的本地知识库；红色站网暂不能参与本地查询。"
            : clearSelection
                ? std::format(
                    L"已取消选择 {} 个具有可用本地知识库的站网；"
                    L"红色站网保持未选中。",
                    available)
                : std::format(
                    L"已选中 {} 个具有可用本地知识库的站网；"
                    L"红色站网已自动取消选择。",
                    selected);
        SetOperationStatusText(message);
    }

    void SelectAllKnowledgeBaseNetworks() {
        if (querying_ || siteLogLoading_) return;
        const bool waitingForInitialAvailability = knowledgeBaseStatusLoading_
            && knowledgeBaseStatusTime_.time_since_epoch().count() == 0;
        if (waitingForInitialAvailability || knowledgeBaseTogglePending_) {
            return;
        }
        // Availability is refreshed at startup/activation. Bulk selection only
        // changes checkboxes against that snapshot; it must not rescan every
        // database or block the next click behind a pending availability scan.
        const bool clearSelection = KnowledgeBaseNetworksAllSelected();
        ApplyKnowledgeBaseToggleSelection(clearSelection);
        networkSelectionTouched_ = true;
    }

    bool KnowledgeBaseNetworksAllSelected() const {
        return knowledgeBaseNetworks_ != 0
            && (selectedNetworkMask_ & knowledgeBaseNetworks_)
                == knowledgeBaseNetworks_;
    }

    void MarkNetworkSelectionTouched() {
        SyncNetworkSelectionFromLegacyControls();
        networkSelectionTouched_ = true;
        if (knowledgeBaseTogglePending_) {
            selectionChangedWhileTogglePending_ = true;
        }
    }

    HBRUSH HandleNetworkControlColor(
        const HDC deviceContext,
        const HWND control) const {
        if (control == observationModeButton_ || control == preciseModeButton_) {
            SetBkMode(deviceContext, TRANSPARENT);
            SetBkColor(deviceContext, kWindowBackground);
            SetTextColor(deviceContext, RGB(38, 50, 64));
            return WindowBackgroundBrush();
        }
        if (control == queryStatus_ || control == filterEdit_
            || control == scopeLatitudeMinimum_
            || control == scopeLatitudeMaximum_
            || control == scopeLongitudeMinimum_
            || control == scopeLongitudeMaximum_
            || control == uniformCountEdit_
            || control == minimumDistanceEdit_
            || control == maximumNeighborDistanceEdit_
            || control == folderEdit_) {
            SetBkMode(deviceContext, OPAQUE);
            SetBkColor(deviceContext, kSurface);
            SetTextColor(deviceContext, RGB(38, 50, 64));
            return SurfaceBrush();
        }
        for (const auto& [networkControl, network] : NetworkControls()) {
            if (networkControl != control) continue;
            SetBkMode(deviceContext, TRANSPARENT);
            SetBkColor(deviceContext, RGB(255, 255, 255));
            SetTextColor(
                deviceContext,
                HasNetwork(knowledgeBaseNetworks_, network)
                    ? RGB(0, 0, 0)
                    : RGB(205, 30, 30));
            return GetSysColorBrush(COLOR_WINDOW);
        }
        // Static labels are transparent over the parent-painted canvas.  A
        // matching brush is still returned for controls/themes that ask the
        // parent to erase their background, avoiding a legacy grey patch.
        SetBkMode(deviceContext, TRANSPARENT);
        SetBkColor(deviceContext, kWindowBackground);
        return WindowBackgroundBrush();
    }

    ObservationRangeOptions BuildRangeOptions() const {
        ObservationRangeOptions options;
        switch (advancedFilter_.completenessMode) {
        case AdvancedFilterState::CompletenessMode::RatioAtLeast:
            options.completenessMode = CompletenessMode::RatioAtLeast;
            break;
        case AdvancedFilterState::CompletenessMode::DaysAtLeast:
            options.completenessMode = CompletenessMode::DaysAtLeast;
            break;
        case AdvancedFilterState::CompletenessMode::Ignore:
        default:
            options.completenessMode = CompletenessMode::Ignore;
            break;
        }
        options.minimumCompletenessPercent =
            advancedFilter_.minimumCompletenessPercent;
        options.minimumAvailableDays = advancedFilter_.minimumAvailableDays;
        // Daily availability is a constraint on the selected set. It must
        // never filter dates or change the single-station denominator.
        options.dailyCoverageMode = DailyCoverageMode::Ignore;
        options.minimumDailyStations = 0;
        options.minimumDailyStationPercent = 0;
        options.sampleInterval = advancedFilter_.sampleInterval;
        options.rinexVersion = advancedFilter_.rinexVersion;
        options.systemsInclude = advancedFilter_.systemsInclude;
        options.systemsExclude = advancedFilter_.systemsExclude;
        options.receiverInclude = advancedFilter_.receiverInclude;
        options.receiverExclude = advancedFilter_.receiverExclude;
        options.antennaInclude = advancedFilter_.antennaInclude;
        options.antennaExclude = advancedFilter_.antennaExclude;
        options.signalCodesInclude = advancedFilter_.signalCodesInclude;
        options.signalCodesExclude = advancedFilter_.signalCodesExclude;
        options.gimStationMode = advancedFilter_.gimStationMode;
        options.gimProviders = advancedFilter_.gimProviders;
        options.gimProductGrades = advancedFilter_.gimProductGrades;
        options.observationCompletenessEnabled =
            advancedFilter_.observationCompletenessEnabled;
        options.minimumObservationCompletenessPercent =
            advancedFilter_.minimumObservationCompletenessPercent;
        options.latencyEnabled = advancedFilter_.latencyEnabled;
        options.maximumMeanLatencyHours =
            advancedFilter_.maximumMeanLatencyHours;
        options.positionTimeSeriesEnabled =
            advancedFilter_.positionTimeSeriesEnabled;
        options.maximumMeanAbsoluteDetrendedPositionMillimeters =
            advancedFilter_.maximumMeanAbsoluteDetrendedPositionMillimeters;
        options.residualsEnabled = advancedFilter_.residualsEnabled;
        options.maximumMeanAbsoluteResidualMillimeters =
            advancedFilter_.maximumMeanAbsoluteResidualMillimeters;
        options.meanMultipathEnabled = advancedFilter_.meanMultipathEnabled;
        options.maximumMeanMultipathCentimeters =
            advancedFilter_.maximumMeanMultipathCentimeters;
        options.cycleSlipsEnabled = advancedFilter_.cycleSlipsEnabled;
        options.maximumMeanDailyCycleSlips =
            advancedFilter_.maximumMeanDailyCycleSlips;
        options.observationCompletenessSystem = advancedFilter_.observationCompletenessSystem;
        options.multipathSystem = advancedFilter_.multipathSystem;
        options.cycleSlipsSystem = advancedFilter_.cycleSlipsSystem;
        options.kpMaxFilter = dateAdvancedFilter_.kpMaxFilter;
        options.dstMinFilter = dateAdvancedFilter_.dstMinFilter;
        options.f107Filter = dateAdvancedFilter_.f107Filter;
        options.useAdjustedF107 = dateAdvancedFilter_.useAdjustedF107;
        // Keep both range-engine interval switches enabled unless the user
        // explicitly selected a RINEX family.  The GUI's single combo maps
        // directly to the public rinexVersion field.
        options.includeRinex2 = advancedFilter_.rinexVersion != 3;
        options.includeRinex34 = advancedFilter_.rinexVersion != 2;
        return options;
    }

    void ApplyQuickScheme() {
        if (quickScheme_ == nullptr) return;
        const auto index = static_cast<int>(SendMessageW(quickScheme_, CB_GETCURSEL, 0, 0));
        ObservationPresetSelection preset = ObservationPresetSelection::None;
        switch (index) {
        case 1: preset = ObservationPresetSelection::ReferenceFrameAutomatic; break;
        case 2: preset = ObservationPresetSelection::GimTec; break;
        case 3: preset = ObservationPresetSelection::GimEvaluation; break;
        case 4: preset = ObservationPresetSelection::BeiDouPod; break;
        case 5: preset = ObservationPresetSelection::NetworkRtk; break;
        default: break;
        }
        observationPreset_ = preset;
        quickFrameSelection_ = preset == ObservationPresetSelection::ReferenceFrameAutomatic
            ? ReferenceFrameSelection::Automatic : ReferenceFrameSelection::None;
        if (preset == ObservationPresetSelection::None) {
            SetOperationStatusText(L"已切换到普通观测查询方案。请按需选择站网和筛选条件。");
            return;
        }

        // Each task starts from a complete filter snapshot. User dates remain unchanged.
        advancedFilter_ = {};
        dateAdvancedFilter_ = {};
        advancedFilter_.sampleInterval = "30S";
        advancedFilter_.rinexVersion = 3;
        advancedFilter_.completenessMode = AdvancedFilterState::CompletenessMode::RatioAtLeast;
        advancedFilter_.minimumCompletenessPercent = 95.0;
        advancedFilter_.systemsInclude = "GPS";
        selectionDailyMode_ = L"ignore";
        selectionDailyStations_ = 0;
        selectionDailyPercent_ = 0.0;
        SetWindowTextW(minimumDistanceEdit_, L"0");
        SetWindowTextW(maximumNeighborDistanceEdit_, L"0");

        const bool regional = preset == ObservationPresetSelection::NetworkRtk;
        const bool allSources = regional || preset == ObservationPresetSelection::GimEvaluation;
        const bool reference = preset == ObservationPresetSelection::ReferenceFrameAutomatic;
        for (const auto& [control, network] : NetworkControls()) {
            const bool enabled = allSources ? HasNetwork(knowledgeBaseNetworks_, network)
                : network == ObservationNetwork::Igs || (!reference && network == ObservationNetwork::Mgex);
            SendMessageW(control, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        MarkNetworkSelectionTouched();
        if (!regional) {
            webSelectedCountryCodes_.clear();
            SendMessageW(scopeCountry_, CB_SETCURSEL, 0, 0);
            for (const HWND control : {scopeLatitudeMinimum_, scopeLatitudeMaximum_,
                     scopeLongitudeMinimum_, scopeLongitudeMaximum_}) SetWindowTextW(control, L"");
            geographicScope_.assign(stations_.size(), true);
        }

        std::wstring message;
        if (reference) {
            message = L"已应用参考框架预设：官方核心站、GPS、30S、RINEX 3/4、日期覆盖至少95%；不固定测站数量。";
        } else if (preset == ObservationPresetSelection::GimTec) {
            advancedFilter_.gimStationMode = "keep";
            advancedFilter_.gimProviders = "IGS,CODE,ESA,JPL,UPC,CAS,EMR,WHU";
            selectionDailyMode_ = L"stations";
            selectionDailyStations_ = 245;
            SetWindowTextW(uniformCountEdit_, L"250");
            SetWindowTextW(minimumDistanceEdit_, L"10");
            message = L"已应用 GIM 相关测站预设：IGS + MGEX、GPS、30S、RINEX 3/4、日期覆盖至少95%；均匀选择250站，间距至少10 km，每天至少245站可用。";
        } else if (preset == ObservationPresetSelection::GimEvaluation) {
            advancedFilter_.minimumCompletenessPercent = 90.0;
            advancedFilter_.gimStationMode = "exclude";
            advancedFilter_.gimProviders = "IGS,CODE,ESA,JPL,UPC,CAS,EMR,WHU";
            selectionDailyMode_ = L"stations";
            selectionDailyStations_ = 35;
            SetWindowTextW(uniformCountEdit_, L"50");
            SetWindowTextW(minimumDistanceEdit_, L"50");
            message = L"已应用 GIM 独立评估预设：GPS、30S、RINEX 3/4、日期覆盖至少90%；排除所选机构在整个区间使用过的测站，均匀选择50站，间距至少50 km，每天至少35站可用。建议评估区间为三至四年。";
        } else if (preset == ObservationPresetSelection::BeiDouPod) {
            advancedFilter_.minimumCompletenessPercent = 100.0;
            advancedFilter_.systemsInclude = "BDS";
            advancedFilter_.signalCodesInclude = "BDS:C2I;BDS:L2I;BDS:C6I;BDS:L6I";
            advancedFilter_.observationCompletenessEnabled = true;
            advancedFilter_.minimumObservationCompletenessPercent = 90.0;
            advancedFilter_.observationCompletenessSystem = "BDS";
            advancedFilter_.meanMultipathEnabled = true;
            advancedFilter_.maximumMeanMultipathCentimeters = 30.0;
            advancedFilter_.multipathSystem = "BDS";
            selectionDailyMode_ = L"stations";
            selectionDailyStations_ = 60;
            SetWindowTextW(uniformCountEdit_, L"60");
            SetWindowTextW(minimumDistanceEdit_, L"300");
            message = L"已应用北斗 POD 预设：IGS + MGEX、30S、RINEX 3/4、BDS B1I/B3I双频伪距和相位；北斗每日完整率至少90%、平均 RMS 多路径不超过30 cm，合格日期覆盖100%；均匀选择60站，间距至少300 km。建议先使用7至10天区间。";
        } else if (regional) {
            advancedFilter_.minimumCompletenessPercent = 90.0;
            advancedFilter_.rinexVersion = 0;
            advancedFilter_.systemsInclude.clear();
            selectionDailyMode_ = L"percent";
            selectionDailyPercent_ = 80.0;
            SetWindowTextW(uniformCountEdit_, L"30");
            SetWindowTextW(minimumDistanceEdit_, L"20");
            SetWindowTextW(maximumNeighborDistanceEdit_, L"75");
            message = L"已应用区域 RTK 候选预设：全部本地数据源、30S、日期覆盖至少90%；默认不限版本和系统。请选择区域；默认30站、间距至少20 km、最大连边75 km，每天至少80%的目标站数可用。系统元数据缺失不代表满足指定系统要求。";
        }
        ApplyAdvancedFilterToScope();
        if (regional && !stations_.empty()) ApplyScopeFromControls(false);
        UpdateAdvancedFilterButtonLabel();
        UpdateDateAdvancedFilterButtonLabel();
        UpdateKnowledgeBaseToggleLabel();
        SetOperationStatusText(message);
        if (!stations_.empty()) {
            PopulateStationList();
            MapViewRefresh(map_);
        }
    }

    void StartQuery() {
        CancelStationSelection();
        ++stationQueryRevision_;
        if (querying_ || siteLogLoading_) {
            return;
        }
        // The knowledge-base status is refreshed when the window is created
        // or activated and when the user presses "全选有知识库".  Re-scanning
        // every database here made each query pay the cost of aggregating
        // tens of thousands of daily-scan rows (especially the large IGS
        // database).  The query itself performs read-only date checks and
        // will report a missing/incomplete database, so there is no need to
        // block the click with a second full status pass.
        const NetworkSelection selection = GetNetworkSelection();
        if (!selection.Any()) {
            NativeUiLanguage::ShowMessageBox(
                window_,
                L"请至少选择一个站网。",
                kApplicationTitle,
                MB_OK | MB_ICONWARNING);
            return;
        }
        const SYSTEMTIME selected = queryStartDate_;
        const QueryDate date = MakeQueryDate(
            selected.wYear,
            selected.wMonth,
            selected.wDay);
        if (date.dayOfYear <= 0) {
            NativeUiLanguage::ShowMessageBox(window_, L"日期无效。", kApplicationTitle, MB_OK | MB_ICONWARNING);
            return;
        }
        const SYSTEMTIME selectedEnd = queryEndDate_;
        const QueryDate rangeEnd = MakeQueryDate(
            selectedEnd.wYear,
            selectedEnd.wMonth,
            selectedEnd.wDay);
        if (rangeEnd.dayOfYear <= 0) {
            NativeUiLanguage::ShowMessageBox(window_, L"结束日期无效。", kApplicationTitle,
                        MB_OK | MB_ICONWARNING);
            return;
        }
        if (FormatDate(rangeEnd) < FormatDate(date)) {
            NativeUiLanguage::ShowMessageBox(window_, L"结束日期不能早于起始日期。", kApplicationTitle,
                        MB_OK | MB_ICONWARNING);
            SetFocus(endDatePicker_);
            return;
        }
        auto rangeOptions = BuildRangeOptions();
        rangeOptions.onlineSnapshots = ObservationSearchOnlineSnapshots(
            DefaultObservationSearchIndexPath(), selection, rangeEnd);
        if (quickFrameSelection_ != ReferenceFrameSelection::None) {
            rangeOptions.restrictStationDates = true;
            rangeOptions.stationDateWindows = BuildReferenceFrameWindows(
                quickFrameSelection_, date, rangeEnd);
        }
        if (queryThread_.joinable()) {
            queryThread_.join();
        }
        SetRectangleSelectionMode(false);
        querying_ = true;
        currentDate_ = date;
        currentDateRangeQuery_ = true;
        currentSelection_ = selection;
        currentRangeOptions_ = rangeOptions;
        SetBusyControls();
        const std::wstring queryMessage = std::format(
            L"正在查询 {} 至 {} 的 {} 日期范围…",
            FormatDate(date),
            FormatDate(rangeEnd),
            FormatNetworkSelection(selection));
        SetQuerySummaryText(queryMessage);
        SetOperationStatusText(
            L"正在读取起止日期内的本地知识库，并按日期、完整性与测站条件计算结果。"
            L"超出知识库的日期沿用最后已知信息，下载时在线确认。");
        SendMessageW(progress_, PBM_SETPOS, 0, 0);
        PostWebUiState();

        const HWND target = window_;
        const auto frameSelection = quickFrameSelection_;
        // Display equipment comes from the dated index in the same query.
        // Reading the source archives afterward delayed otherwise complete rows.
        queryThread_ = std::jthread([target, date, rangeEnd, selection,
                                     rangeOptions, frameSelection](
                                        const std::stop_token stopToken) {
            auto result = std::make_unique<QueryResult>();
            // Every interactive range query, including time-aware receiver,
            // antenna and constellation predicates, uses the compact bitmap
            // index.  Never silently fall back to scanning tens of millions
            // of authoritative daily rows: a missing/incomplete derived
            // index is reported immediately and can be rebuilt offline.
            const auto fast = QueryObservationSearchIndex(
                DefaultObservationSearchIndexPath(),
                date,
                rangeEnd,
                selection,
                rangeOptions,
                true,
                stopToken,
                true);
            if (fast.complete) {
                result->date = date;
                result->selection = selection;
                result->directoryUrl =
                    L"本地 GNSS 观测高速索引（日期位图）";
                result->stations = fast.stations;
                result->selectionDatesKnown = true;
                result->selectionEligibleOrdinals = fast.selectionEligibleOrdinals;
                bool referenceRosterMissing = false;
                bool referenceFrameOutsideTimeline = false;
                if (frameSelection != ReferenceFrameSelection::None) {
                    std::unordered_set<std::string> roster;
                    if (frameSelection == ReferenceFrameSelection::Automatic) {
                        // A range can cross a reference-frame switch.  Use
                        // the union of the official core rosters for every
                        // frame that overlaps the requested UTC interval.
                        const auto startText = FormatDate(date);
                        const auto endText = FormatDate(rangeEnd);
                        bool overlapsTimeline = false;
                        for (const auto& spec : kReferenceFrames) {
                            if (endText < spec.start || startText > spec.end) continue;
                            overlapsTimeline = true;
                            const auto& codes = ReferenceFrameStationCodes(spec.id);
                            roster.insert(codes.begin(), codes.end());
                        }
                        referenceFrameOutsideTimeline = !overlapsTimeline;
                    } else {
                        const auto& codes = ReferenceFrameStationCodes(frameSelection);
                        roster.insert(codes.begin(), codes.end());
                    }
                    referenceRosterMissing = roster.empty();
                    if (!roster.empty()) {
                        result->stations.erase(
                            std::remove_if(result->stations.begin(),
                                result->stations.end(),
                                [&roster](const StationFile& station) {
                                    std::string code = station.fourCharId;
                                    if (code.empty()) code = station.stationId;
                                    if (code.size() > 4) code.resize(4);
                                    std::transform(code.begin(), code.end(), code.begin(),
                                        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                                    return !roster.contains(code);
                                }),
                            result->stations.end());
                    }
                }
                result->periodStart = date;
                result->periodEnd = rangeEnd;
                result->coversFullGpsWeek = false;
                result->coversDateRange = true;
                result->gpsWeek = -1;
                result->periodFilesDeferred = true;
                result->siteLogMetadataIncluded = fast.siteLogMetadataIncluded;
                result->usedLocalKnowledgeBase = true;
                result->usedIgsLocalIndex =
                    HasNetwork(selection.mask, ObservationNetwork::Igs)
                    || HasNetwork(selection.mask, ObservationNetwork::Mgex);
                result->catalogSource = L"统一日期位图与时间化Site Log索引";
                result->warning = fast.warning;
                if (frameSelection != ReferenceFrameSelection::None) {
                    if (referenceFrameOutsideTimeline) {
                        result->warning += L"\n所选日期早于当前内置的 IGS14 参考框架时间线（2017-01-29）；旧版 ITRF/IGS 框架暂未纳入自动筛选。";
                    } else if (referenceRosterMissing) {
                        result->warning += L"\n未找到参考框架核心测站名册，未执行测站筛选；请确认程序目录下 resources/reference_frames 文件夹完整。";
                    }
                    if (frameSelection == ReferenceFrameSelection::Automatic) {
                        result->warning += L"\nIGS/ITRF 自动方案按日期使用 IGS14、IGb14、IGS20、IGb20、IGc20 官方核心测站名册；"
                                           L"跨框架日期范围使用重叠框架的测站并集。";
                    } else if (const auto* spec = FindReferenceFrame(frameSelection)) {
                        result->warning += std::format(
                            L"\n{} 方案使用官方核心测站名册（有效期 {} 至 {}）。",
                            spec->label,
                            spec->start,
                            std::wstring_view(spec->end) == L"9999-12-31"
                                ? L"今" : spec->end);
                        const auto startText = FormatDate(date);
                        const auto endText = FormatDate(rangeEnd);
                        if (startText < spec->start || endText > spec->end) {
                            result->warning += L" 查询日期超出该框架有效期，请使用‘按日期自动选择’方案。";
                        }
                    }
                }
                result->filesWithoutCoordinates = static_cast<int>(
                    std::count_if(
                        result->stations.begin(),
                        result->stations.end(),
                        [](const StationFile& station) {
                            return !station.hasCoordinate;
                        }));
                result->emptyResultIsComplete = true;
            } else {
                result->error = fast.error.empty()
                    ? L"观测数据索引不可用，请重新安装官方知识库后重试。"
                    : fast.error
                        + L"\n\n请重新安装官方知识库后重试。";
            }
            if (!PostMessageW(
                    target,
                    WM_IGS_QUERY_FINISHED,
                    0,
                    reinterpret_cast<LPARAM>(result.get()))) {
                return;
            }
            result.release();
        });
    }

    void StartSiteLogEnrichment(
        std::vector<StationFile> stations,
        const QueryDate date,
        std::vector<DatedStationFile> periodFiles = {},
        const bool temporal = false,
        const int expectedDays = 0,
        const AdvancedFilterState filter = {}) {
        if (stations.empty()) {
            siteLogLoading_ = false;
            siteLogBlocksInteraction_ = false;
            siteLogEnrichmentComplete_ = true;
            currentPeriodFiles_ = std::move(periodFiles);
            SetBusyControls();
            return;
        }
        // A previous Site Log task can only be finished-but-not-yet-joined
        // when its completion message is being handled.  Joining here is
        // therefore normally instantaneous and keeps the jthread lifetime
        // unambiguous when a new query starts.
        if (siteLogThread_.joinable()) {
            siteLogThread_.join();
        }
        siteLogLoading_ = true;
        // Representative Site Log enrichment only augments display fields;
        // it never changes availability or station indices.  Keep map/list,
        // scope, selection and download usable in that common case.  A
        // temporal metadata predicate can change the result set and must
        // retain the strict interaction lock until its atomic replacement.
        siteLogBlocksInteraction_ = temporal
            || HasTemporalMetadataFilter(filter);
        siteLogEnrichmentComplete_ = false;
        const std::uint64_t generation = ++siteLogGeneration_;
        const HWND target = window_;
        siteLogThread_ = std::jthread(
            [target,
             generation,
              date,
              temporal,
              expectedDays,
              filter,
              stations = std::move(stations),
              periodFiles = std::move(periodFiles)](
                 const std::stop_token stopToken) mutable {
                auto result = std::make_unique<SiteLogEnrichmentResult>();
                result->generation = generation;
                result->temporal = temporal;
                if (temporal && !periodFiles.empty()) {
                    auto processed = ProcessTemporalSiteLogPeriod(
                        stations,
                        std::move(periodFiles),
                        filter,
                        expectedDays,
                        stopToken);
                    result->stations = std::move(processed.stations);
                    result->periodFiles = std::move(processed.periodFiles);
                    result->matchedCount = processed.matchedCount;
                    result->filteredPeriodFiles =
                        processed.filteredPeriodFiles;
                    result->error = std::move(processed.error);
                } else {
                    result->stations = std::move(stations);
                    result->periodFiles = std::move(periodFiles);
                    std::wstring error;
                    result->matchedCount = EnrichStationsWithSiteLogs(
                        result->stations,
                        date,
                        error);
                    result->error = std::move(error);
                }
                if (!PostMessageW(
                        target,
                        WM_IGS_SITELOG_FINISHED,
                        0,
                        reinterpret_cast<LPARAM>(result.get()))) {
                    return;
                }
                result.release();
            });
        SetBusyControls();
    }

    void HandleSiteLogFinished(
        std::unique_ptr<SiteLogEnrichmentResult> result) {
        if (result == nullptr || result->generation != siteLogGeneration_) {
            return;
        }
        // The worker posts only after the expensive reads have completed, so
        // this join does not block the UI.  It also guarantees that the
        // temporary StationFile copies are destroyed before the next query.
        if (siteLogThread_.joinable()) {
            siteLogThread_.join();
        }
        siteLogLoading_ = false;
        siteLogBlocksInteraction_ = false;
        siteLogEnrichmentComplete_ = true;

        if (result->temporal) {
            // The temporal worker rebuilt both the station representatives and
            // the per-day download rows.  Replace them atomically on the UI
            // thread so stationIndex values in periodFiles always refer to
            // the current vector.  The controls were disabled while this pass
            // was running, therefore no user selection or geographic scope
            // can be lost here.
            stations_ = std::move(result->stations);
            for (auto& station : stations_) CorrectKnownStationCoordinates(station);
            currentPeriodFiles_ = std::move(result->periodFiles);
            ResetScopeControls();
            PopulateCountryChoices();
            PopulateStationList();
            MapViewSetStations(map_, &stations_);
            MapViewRefresh(map_);

            // MapView only paints stations that remain inside the current
            // geographic/metadata scope.  Count that same predicate here so
            // the status line reports the number of actual map markers,
            // rather than counting coordinates that a scope filter removed.
            const int mapped = static_cast<int>(std::count_if(
                stations_.begin(), stations_.end(),
                [](const StationFile& station) {
                    return station.inSelectionScope && station.hasCoordinate;
                }));
            std::wstring summary = std::format(
                L"{} 至 {} · {}：{} 个去重测站",
                FormatDate(currentPeriodStart_),
                FormatDate(currentPeriodEnd_),
                FormatNetworkSelection(currentSelection_),
                stations_.size());
            summary += std::format(
                L"（Site Log按观测日期应用设备履历；地图显示 {} 个，{} 个日文件因元数据条件被排除）",
                mapped,
                result->filteredPeriodFiles);
            if (const auto dateSummary = FormatDateAdvancedSelection(
                    dateAdvancedFilter_,
                    currentPeriodStart_,
                    currentPeriodEnd_);
                !dateSummary.empty()) {
                summary += L"\r\n" + dateSummary;
            }
            SetQuerySummaryText(summary);

            std::wstring status = std::format(
                L"Site Log 已按每个观测日匹配设备履历，匹配 {} 个测站；"
                L"当前列表已按日期对应的接收机、天线和卫星系统信息完成筛选。",
                result->matchedCount);
            if (!result->error.empty()) {
                status += L" Site Log提示：" + result->error;
            }
            SetOperationStatusText(status);
            SetBusyControls();
            return;
        }

        const auto canonical = [](const std::string& value) {
            std::string result = value;
            std::transform(
                result.begin(),
                result.end(),
                result.begin(),
                [](const unsigned char character) {
                    return static_cast<char>(std::toupper(character));
                });
            return result;
        };
        const auto sameStation = [&](
            const StationFile& left,
            const StationFile& right) {
            if (!left.stationId.empty() && !right.stationId.empty()
                && canonical(left.stationId) == canonical(right.stationId)) {
                return true;
            }
            if (!left.fileName.empty() && !right.fileName.empty()
                && left.fileName == right.fileName) {
                return true;
            }
            return !left.fourCharId.empty()
                && !right.fourCharId.empty()
                && canonical(left.fourCharId) == canonical(right.fourCharId)
                && left.country == right.country;
        };

        // The query vector normally keeps exactly the same order while the
        // worker runs.  Handle that fast path first; the keyed fallback keeps
        // the merge correct if a future UI operation reorders the rows.
        std::vector<bool> merged(stations_.size(), false);
        const std::size_t common = std::min(
            stations_.size(), result->stations.size());
        for (std::size_t index = 0; index < common; ++index) {
            if (!sameStation(stations_[index], result->stations[index])) {
                continue;
            }
            CopySiteLogMetadata(stations_[index], result->stations[index]);
            merged[index] = true;
        }
        if (std::count(merged.begin(), merged.end(), false) > 0) {
            std::unordered_map<std::string, std::size_t> byKey;
            byKey.reserve(stations_.size() * 2U);
            for (std::size_t index = 0; index < stations_.size(); ++index) {
                if (!stations_[index].stationId.empty()) {
                    byKey.emplace(
                        canonical(stations_[index].stationId), index);
                }
                if (!stations_[index].fourCharId.empty()) {
                    byKey.emplace(
                        canonical(stations_[index].fourCharId), index);
                }
            }
            for (const auto& enriched : result->stations) {
                std::size_t index = stations_.size();
                if (!enriched.stationId.empty()) {
                    const auto found = byKey.find(canonical(enriched.stationId));
                    if (found != byKey.end()) index = found->second;
                }
                if (index >= stations_.size() && !enriched.fourCharId.empty()) {
                    const auto found = byKey.find(canonical(enriched.fourCharId));
                    if (found != byKey.end()) index = found->second;
                }
                if (index < stations_.size()) {
                    CopySiteLogMetadata(stations_[index], enriched);
                }
            }
        }

        ApplyAdvancedFilterToScope();
        // Site Log fields are part of the filter columns.  Rebuild the
        // virtual list once after the background pass, preserving all checks,
        // scope flags and selections already made by the user.
        PopulateStationList();
        MapViewRefresh(map_);
        // Site Log enrichment can add coordinates after the initial query.
        // Refresh the reported map and missing-coordinate counts as well.
        const int mapped = static_cast<int>(std::count_if(
            stations_.begin(), stations_.end(),
            [](const StationFile& station) {
                return station.inSelectionScope && station.hasCoordinate;
            }));
        std::wstring summary = std::format(
            L"查询完成：{} 至 {}；{} 个测站，地图显示 {} 个。",
            FormatDate(currentPeriodStart_), FormatDate(currentPeriodEnd_),
            stations_.size(), mapped);
        const int unresolved = static_cast<int>(std::count_if(
            stations_.begin(), stations_.end(),
            [](const StationFile& station) {
                return station.inSelectionScope && !station.hasCoordinate;
            }));
        if (unresolved > 0) summary += std::format(L" {} 个暂缺坐标。", unresolved);
        if (const auto dateSummary = FormatDateAdvancedSelection(
                dateAdvancedFilter_, currentPeriodStart_, currentPeriodEnd_);
            !dateSummary.empty()) summary += L"\r\n" + dateSummary;
        SetQuerySummaryText(summary);
        if (!result->error.empty())
            SetOperationStatusText(L"测站详细信息读取失败：" + result->error);
        else SetOperationStatusText(querySummaryText_);
        SetBusyControls();
    }

    void HandleQueryFinished(std::unique_ptr<QueryResult> result) {
        querying_ = false;
        SetBusyControls();
        if (result == nullptr) {
            return;
        }
        if (!result->error.empty()) {
            stations_.clear();
            siteLogEnrichmentComplete_ = true;
            currentDateRangeQuery_ = false;
            currentPeriodFilesDeferred_ = false;
            currentPeriodFiles_.clear();
            ResetScopeControls();
            PopulateStationList();
            MapViewSetStations(map_, &stations_);
            SetQuerySummaryText(result->error);
            SetOperationStatusText(L"查询失败；请检查网络、日期或稍后重试。");
            NativeUiLanguage::ShowMessageBox(
                window_,
                result->error.c_str(),
                kApplicationTitle,
                MB_OK | MB_ICONWARNING);
            return;
        }

        currentDate_ = result->date;
        currentSelection_ = result->selection;
        currentDateRangeQuery_ = result->coversDateRange;
        currentPeriodStart_ = result->periodStart;
        currentPeriodEnd_ = result->periodEnd;
        currentPeriodFilesDeferred_ = result->periodFilesDeferred;
        currentSelectionDatesKnown_ = result->selectionDatesKnown;
        currentSelectionEligibleDates_ = std::move(result->selectionEligibleOrdinals);
        currentPeriodFiles_ = std::move(result->periodFiles);
        const bool indexedSiteLog = result->siteLogMetadataIncluded;
        // A full date-range result can contain millions of daily rows.  The
        // exact Site Log equipment epoch is only needed when a metadata
        // predicate is active; otherwise enriching every row would spend
        // most of the query time copying identical receiver/antenna data.
        // Keep the lightweight representative enrichment below for display,
        // and run the temporal per-day pass only when it can affect the
        // result.  If the user later enables a metadata filter, accepting the
        // dialog starts a fresh query with this flag enabled.
        const bool temporalSiteLog = !indexedSiteLog
            && result->coversDateRange
            && !currentPeriodFiles_.empty()
            && HasMetadataAdvancedFilter();
        int expectedPeriodDays = 0;
        for (const auto& station : result->stations) {
            expectedPeriodDays = std::max(
                expectedPeriodDays,
                station.expectedDays);
        }
        if (expectedPeriodDays <= 0) {
            expectedPeriodDays = 1;
        }
        siteLogEnrichmentComplete_ = indexedSiteLog;
        // Publish the availability result immediately.  Site Log enrichment
        // can touch many independent SQLite archives and is now performed in
        // a background pass below; waiting for it here made a local query
        // appear to access the network even though the daily index itself was
        // already complete.
        // The enrichment worker needs only station identity and network
        // membership.  Do not copy downloadRoutes/remoteParts (RGNA can have
        // many hourly parts per station) onto the UI thread just to perform a
        // metadata lookup.
        std::vector<StationFile> siteLogInput;
        if (!indexedSiteLog) {
            siteLogInput.reserve(result->stations.size());
            for (const auto& station : result->stations) {
                StationFile lookup;
                lookup.stationId = station.stationId;
                lookup.fourCharId = station.fourCharId;
                lookup.country = station.country;
                lookup.availableNetworks = station.availableNetworks;
                siteLogInput.push_back(std::move(lookup));
            }
        }
        stations_ = std::move(result->stations);
        for (auto& station : stations_) CorrectKnownStationCoordinates(station);
        webSortAscending_ = false;
        webSortByCompleteness_ = true;
        if (observationPreset_ != ObservationPresetSelection::NetworkRtk) ResetScopeControls();
        else geographicScope_.assign(stations_.size(), true);
        ApplyAdvancedFilterToScope();
        PopulateCountryChoices();
        if (observationPreset_ == ObservationPresetSelection::NetworkRtk) ApplyScopeFromControls(false);
        PopulateStationList();
        MapViewSetStations(map_, &stations_);
        MapViewResetView(map_);

        // Keep the summary consistent with MapView: out-of-scope rows remain
        // available in the list but are not drawn on the map.  The unresolved
        // count is therefore limited to rows that could otherwise be drawn.
        const int mapped = static_cast<int>(std::count_if(
            stations_.begin(), stations_.end(),
            [](const StationFile& station) {
                return station.inSelectionScope && station.hasCoordinate;
            }));
        const int unresolved = static_cast<int>(std::count_if(
            stations_.begin(), stations_.end(),
            [](const StationFile& station) {
                return station.inSelectionScope && !station.hasCoordinate;
            }));
        std::wstring resultSummary = std::format(
            L"查询完成：{} 至 {}；{} 个测站，地图显示 {} 个。",
            FormatDate(currentPeriodStart_), FormatDate(currentPeriodEnd_),
            stations_.size(), mapped);
        if (unresolved > 0) resultSummary += std::format(L" {} 个暂缺坐标。", unresolved);
        if (const auto dateSummary = FormatDateAdvancedSelection(
                dateAdvancedFilter_,
                currentPeriodStart_,
                currentPeriodEnd_);
            !dateSummary.empty()) {
            resultSummary += L"\r\n" + dateSummary;
        }
        SetQuerySummaryText(resultSummary);
        SetWindowTextW(downloadButton_, currentDateRangeQuery_
            ? L"下载已选测站的范围文件" : L"下载已选观测文件");
        if (!result->warning.empty())
            SetOperationStatusText(L"查询提示：" + result->warning);
        else SetOperationStatusText(resultSummary);
        UpdateSelectionSummary();
        // Global task-oriented presets also apply their station-selection
        // policy.  The regional Network RTK preset intentionally stops at
        // candidate filtering because its service area and target count must
        // be chosen by the user before the connected-network selector runs.
        if ((observationPreset_ == ObservationPresetSelection::GimTec
                || observationPreset_ == ObservationPresetSelection::GimEvaluation
                || observationPreset_
                    == ObservationPresetSelection::BeiDouPod)
            && !stations_.empty()) {
            SelectGloballyDistributed(false);
        }
        // Move the potentially large per-day vector into the worker.  It is
        // restored on completion (or unchanged if the temporal pass cannot
        // load a Site Log), avoiding an additional multi-gigabyte copy for a
        // long full-year query.
        if (indexedSiteLog) {
            siteLogLoading_ = false;
            siteLogBlocksInteraction_ = false;
            SetBusyControls();
        } else {
            StartSiteLogEnrichment(
                std::move(siteLogInput),
                result->date,
                std::move(currentPeriodFiles_),
                temporalSiteLog,
                expectedPeriodDays,
                advancedFilter_);
        }
    }

    void PopulateCountryChoices() const {
        SendMessageW(scopeCountry_, CB_RESETCONTENT, 0, 0);
        const auto allCountries = static_cast<int>(SendMessageW(
            scopeCountry_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"全部国家/地区")));
        SendMessageW(scopeCountry_, CB_SETITEMDATA, allCountries, -1);
        std::set<std::string> countries;
        for (const auto& station : stations_) {
            if (!station.country.empty()) {
                const auto code = DisplayCountryCode(station);
                // The combo is deliberately a compact ISO-alpha-3 list.  A
                // malformed provider value must never leak its long display
                // name into the owner-drawn list.
                if (code.size() == 3 && CountryFlags::IsKnownArea(code)) countries.insert(code);
            }
        }
        for (const auto region : CountryPolicy::kSelectableRegionAreas) {
            countries.insert(std::string(region));
        }
        for (const auto& country : countries) {
            const auto display = Utf8ToWide(country);
            const auto item = static_cast<int>(SendMessageW(
                scopeCountry_,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(display.c_str())));
            if (item >= 0) {
                SendMessageW(
                    scopeCountry_, CB_SETITEMDATA,
                    static_cast<WPARAM>(item),
                    static_cast<LPARAM>(CountryChoiceFlagIndex(country)));
            }
        }
        SendMessageW(scopeCountry_, CB_SETDROPPEDWIDTH, 108, 0);
        SendMessageW(scopeCountry_, CB_SETCURSEL, 0, 0);
    }

    void ResetScopeControls() {
        mapRectangleSelecting_ = false;
        MapViewSetRectangleSelectionMode(map_, false);
        SetWindowTextW(mapRectangleScopeButton_, L"地图框选");
        webSelectedCountryCodes_.clear();
        SendMessageW(scopeCountry_, CB_SETCURSEL, 0, 0);
        for (const HWND edit : {
                 scopeLatitudeMinimum_, scopeLatitudeMaximum_,
                 scopeLongitudeMinimum_, scopeLongitudeMaximum_}) {
            SetWindowTextW(edit, L"");
        }
        for (auto& station : stations_) {
            station.inSelectionScope = true;
        }
        geographicScope_.assign(stations_.size(), true);
        ApplyAdvancedFilterToScope();
        SetWindowTextW(
            scopeStatus_,
            stations_.empty()
                ? L"候选范围：请先查询当天测站。"
                : std::format(
                    L"候选范围：全部 {} 个测站。",
                    stations_.size()).c_str());
    }

    bool ReadOptionalNumber(
        const HWND edit,
        const wchar_t* label,
        const double minimum,
        const double maximum,
        std::optional<double>& value) const {
        const std::wstring text = GetWindowTextString(edit);
        if (text.empty()) {
            value.reset();
            return true;
        }
        errno = 0;
        wchar_t* end = nullptr;
        const double parsed = std::wcstod(text.c_str(), &end);
        if (end == text.c_str() || *end != L'\0' || errno == ERANGE
            || !std::isfinite(parsed)
            || parsed < minimum || parsed > maximum) {
            NativeUiLanguage::ShowMessageBox(
                window_,
                std::format(
                    L"{}必须是 {} 到 {} 之间的数字。",
                    label,
                    minimum,
                    maximum).c_str(),
                kApplicationTitle,
                MB_OK | MB_ICONWARNING);
            SetFocus(edit);
            return false;
        }
        value = parsed;
        return true;
    }

    void SetRectangleSelectionMode(const bool enabled) {
        mapRectangleSelecting_ = enabled && !stations_.empty();
        MapViewSetRectangleSelectionMode(map_, mapRectangleSelecting_);
        SetWindowTextW(
            mapRectangleScopeButton_,
            mapRectangleSelecting_ ? L"取消框选" : L"地图框选");
        if (mapRectangleSelecting_) {
            SetWindowTextW(
                scopeStatus_,
                L"地图框选：在地图内按住左键拖出矩形候选范围。" );
            SetFocus(map_);
        }
    }

    void ToggleMapRectangleScope() {
        if (querying_ || siteLogBlocksInteraction_
            || stations_.empty()) {
            return;
        }
        SetRectangleSelectionMode(!mapRectangleSelecting_);
    }

    bool ApplyScopeFromControls(const bool announce = true) {
        if (querying_ || siteLogBlocksInteraction_
            || stations_.empty()) {
            return false;
        }
        std::optional<double> minimumLatitude;
        std::optional<double> maximumLatitude;
        std::optional<double> minimumLongitude;
        std::optional<double> maximumLongitude;
        if (!ReadOptionalNumber(
                scopeLatitudeMinimum_, L"最小纬度", -90.0, 90.0,
                minimumLatitude)
            || !ReadOptionalNumber(
                scopeLatitudeMaximum_, L"最大纬度", -90.0, 90.0,
                maximumLatitude)
            || !ReadOptionalNumber(
                scopeLongitudeMinimum_, L"最小经度", -180.0, 180.0,
                minimumLongitude)
            || !ReadOptionalNumber(
                scopeLongitudeMaximum_, L"最大经度", -180.0, 180.0,
                maximumLongitude)) {
            return false;
        }
        if (minimumLatitude && maximumLatitude
            && *minimumLatitude > *maximumLatitude) {
            NativeUiLanguage::ShowMessageBox(
                window_,
                L"最小纬度不能大于最大纬度。",
                kApplicationTitle,
                MB_OK | MB_ICONWARNING);
            SetFocus(scopeLatitudeMinimum_);
            return false;
        }

        std::wstring selectedCountry;
        const int countryIndex = static_cast<int>(
            SendMessageW(scopeCountry_, CB_GETCURSEL, 0, 0));
        if (countryIndex > 0) {
            const int length = static_cast<int>(SendMessageW(
                scopeCountry_, CB_GETLBTEXTLEN, countryIndex, 0));
            if (length > 0) {
                selectedCountry.resize(static_cast<std::size_t>(length) + 1U);
                SendMessageW(
                    scopeCountry_,
                    CB_GETLBTEXT,
                    countryIndex,
                    reinterpret_cast<LPARAM>(selectedCountry.data()));
                selectedCountry.resize(static_cast<std::size_t>(length));
            }
        }

        const bool usesCoordinates = minimumLatitude || maximumLatitude
            || minimumLongitude || maximumLongitude;
        std::size_t candidateCount = 0;
        std::size_t coordinateCount = 0;
        std::size_t deselectedOutside = 0;
        for (auto& station : stations_) {
            bool candidate = !webSelectedCountryCodes_.empty()
                ? std::any_of(webSelectedCountryCodes_.begin(),
                    webSelectedCountryCodes_.end(),
                    [&station](const auto& code) {
                        return MatchesCountryChoice(station, code);
                    })
                : selectedCountry.empty()
                    || MatchesCountryChoice(station,
                        WideToUtf8(selectedCountry));
            if (candidate && usesCoordinates) {
                candidate = station.hasCoordinate
                    && std::isfinite(station.latitude)
                    && std::isfinite(station.longitude);
            }
            if (candidate && minimumLatitude) {
                candidate = station.latitude >= *minimumLatitude;
            }
            if (candidate && maximumLatitude) {
                candidate = station.latitude <= *maximumLatitude;
            }
            if (candidate && minimumLongitude && maximumLongitude) {
                candidate = *minimumLongitude <= *maximumLongitude
                    ? station.longitude >= *minimumLongitude
                        && station.longitude <= *maximumLongitude
                    : station.longitude >= *minimumLongitude
                        || station.longitude <= *maximumLongitude;
            } else {
                if (candidate && minimumLongitude) {
                    candidate = station.longitude >= *minimumLongitude;
                }
                if (candidate && maximumLongitude) {
                    candidate = station.longitude <= *maximumLongitude;
                }
            }
            const std::size_t stationIndex = static_cast<std::size_t>(
                &station - stations_.data());
            if (stationIndex >= geographicScope_.size()) {
                geographicScope_.resize(stations_.size(), true);
            }
            geographicScope_[stationIndex] = candidate;
            const bool metadataPending = HasMetadataAdvancedFilter()
                && !siteLogEnrichmentComplete_
                && !station.hasSiteLog;
            station.inSelectionScope = candidate
                && (metadataPending || MatchesAdvancedFilter(station));
            if (candidate) {
                ++candidateCount;
                if (station.hasCoordinate) {
                    ++coordinateCount;
                }
            } else if (station.selected) {
                station.selected = false;
                ++deselectedOutside;
            }
        }

        SetRectangleSelectionMode(false);
        PopulateStationList();
        MapViewRefresh(map_);
        std::wstring status = std::format(
            L"候选范围：{} / {} 个测站（{} 个有坐标）",
            candidateCount,
            stations_.size(),
            coordinateCount);
        if (deselectedOutside > 0) {
            status += std::format(
                L"；已取消范围外 {} 个选择",
                deselectedOutside);
        }
        status += L"。";
        SetWindowTextW(scopeStatus_, status.c_str());
        if (announce) {
            SetOperationStatusText(status);
        }
        return true;
    }

    void ClearScope() {
        if (querying_ || siteLogBlocksInteraction_) {
            return;
        }
        ResetScopeControls();
        PopulateStationList();
        MapViewRefresh(map_);
        UpdateSelectionSummary();
    }

    void HandleMapAreaSelected(const MapSelectionBounds& bounds) {
        if (querying_ || siteLogBlocksInteraction_) {
            return;
        }
        SetWindowTextW(
            scopeLatitudeMinimum_,
            std::format(L"{:.5f}", bounds.minimumLatitude).c_str());
        SetWindowTextW(
            scopeLatitudeMaximum_,
            std::format(L"{:.5f}", bounds.maximumLatitude).c_str());
        SetWindowTextW(
            scopeLongitudeMinimum_,
            std::format(L"{:.5f}", bounds.minimumLongitude).c_str());
        SetWindowTextW(
            scopeLongitudeMaximum_,
            std::format(L"{:.5f}", bounds.maximumLongitude).c_str());
        ApplyScopeFromControls();
    }

    void ApplyFilter() {
        PopulateStationList();
    }

    void HandleListItemChanged(const NMLISTVIEW& notification) {
        if (syncingList_ || querying_
            || siteLogBlocksInteraction_) {
            return;
        }
        const UINT oldState =
            (notification.uOldState & LVIS_STATEIMAGEMASK) >> 12;
        const UINT newState =
            (notification.uNewState & LVIS_STATEIMAGEMASK) >> 12;
        if (oldState == newState || newState == 0) {
            return;
        }

        if (notification.iItem < 0
            || static_cast<std::size_t>(notification.iItem)
                >= visibleStationIndices_.size()) {
            return;
        }
        const auto stationIndex = visibleStationIndices_[notification.iItem];
        if (stationIndex >= stations_.size()) {
            return;
        }
        stations_[stationIndex].selected = newState == 2;
        MapViewRefresh(map_);
        UpdateSelectionSummary();
    }

    void ToggleStationFromMap(const std::size_t stationIndex) {
        if (stationIndex >= stations_.size()
            || querying_ || siteLogBlocksInteraction_) {
            return;
        }
        stations_[stationIndex].selected = !stations_[stationIndex].selected;
        syncingList_ = true;
        const auto visible = std::find(
            visibleStationIndices_.begin(),
            visibleStationIndices_.end(),
            stationIndex);
        if (visible != visibleStationIndices_.end()) {
            const int row = static_cast<int>(
                std::distance(visibleStationIndices_.begin(), visible));
            ListView_SetCheckState(
                stationList_,
                row,
                stations_[stationIndex].selected ? TRUE : FALSE);
            ListView_EnsureVisible(stationList_, row, FALSE);
        }
        syncingList_ = false;
        MapViewRefresh(map_);
        UpdateSelectionSummary();
    }

    void SelectVisible(const bool selected) {
        if (querying_ || siteLogBlocksInteraction_
            || (selected && MetadataFilterPending())) {
            if (selected && MetadataFilterPending()) {
                SetOperationStatusText(
                    L"Site Log 正在加载；元数据筛选完成后才能全选当前列表。" );
            }
            return;
        }
        syncingList_ = true;
        for (const auto index : visibleStationIndices_) {
            if (index < stations_.size()) {
                stations_[index].selected = selected;
            }
        }
        ListView_SetItemState(
            stationList_,
            -1,
            INDEXTOSTATEIMAGEMASK(selected ? 2 : 1),
            LVIS_STATEIMAGEMASK);
        syncingList_ = false;
        MapViewRefresh(map_);
        UpdateSelectionSummary();
    }

    void ClearAllSelection() {
        if (querying_ || siteLogBlocksInteraction_) {
            return;
        }
        for (auto& station : stations_) {
            station.selected = false;
        }
        PopulateStationList();
        MapViewRefresh(map_);
        UpdateSelectionSummary();
    }

    std::uint64_t StationSelectionSignature(const bool candidateGeometry = false) const {
        std::uint64_t hash = 1469598103934665603ULL;
        const auto add = [&](std::uint64_t value) { hash ^= value; hash *= 1099511628211ULL; };
        const auto text = [&](const std::wstring& value) { for (auto c : value) add(c); add(0xff); };
        add(stationQueryRevision_);
        add(GetNetworkSelection().mask);
        add(queryStartDate_.wYear); add(queryStartDate_.wMonth); add(queryStartDate_.wDay);
        add(queryEndDate_.wYear); add(queryEndDate_.wMonth); add(queryEndDate_.wDay);
        text(filterText_); text(selectionDailyMode_); text(selectionRandomSeed_);
        text(GetWindowTextString(uniformCountEdit_));
        text(GetWindowTextString(minimumDistanceEdit_));
        text(GetWindowTextString(maximumNeighborDistanceEdit_));
        add(selectionDailyStations_); add(static_cast<std::uint64_t>(selectionDailyPercent_ * 1000));
        for (auto index : visibleStationIndices_) add(index + 1);
        for (std::size_t index = 0; index < stations_.size(); ++index) {
            const auto& station = stations_[index];
            if (station.selected) add(index + stations_.size() + 1);
            add(station.inSelectionScope ? 1 : 0);
            if ((candidateGeometry || station.selected) && station.hasCoordinate
                && std::isfinite(station.latitude) && std::isfinite(station.longitude)) {
                add(static_cast<std::uint64_t>(static_cast<std::int64_t>(station.latitude * 1e6)));
                add(static_cast<std::uint64_t>(static_cast<std::int64_t>(station.longitude * 1e6)));
            }
        }
        return hash;
    }

    std::wstring StationSelectionSummaryJson() const {
        const auto& r = lastStationSelection_;
        return std::format(
            L"{{\"status\":\"{}\",\"message\":\"{}\",\"success\":{},\"generated\":{},\"applied\":{},"
            L"\"provenInfeasible\":{},\"requested\":{},\"selected\":{},\"candidates\":{},"
            L"\"withCoordinates\":{},\"missingCalendars\":{},\"threshold\":{},\"activeDays\":{},\"excludedDays\":{},"
            L"\"failingDays\":{},\"unknownDays\":{},\"minimumAvailable\":{},\"averageAvailable\":{:.3f},"
            L"\"minimumDistanceKm\":{:.3f},\"maximumNearestKm\":{:.3f},"
            L"\"spacingSatisfied\":{},\"neighborSatisfied\":{},\"randomSeed\":\"{}\"}}",
            JsonEscape(r.status), JsonEscape(r.message), r.success ? L"true" : L"false",
            r.generated ? L"true" : L"false", stationSelectionApplied_ ? L"true" : L"false",
            r.provenInfeasible ? L"true" : L"false", r.requested, r.selected, r.candidates,
            r.withCoordinates, r.missingCalendars, r.minimumDailyStations, r.activeDays, r.excludedDays,
            r.failingDays, r.unknownDays, r.minimumAvailable, r.averageAvailable,
            r.minimumDistanceKm, r.maximumNearestKm, r.spacingSatisfied ? L"true" : L"false",
            r.neighborSatisfied ? L"true" : L"false", r.randomSeed);
    }

    void PostStationSelectionReport() const {
        if (!webUi_ || !webUi_->IsReady() || lastStationSelection_.status.empty()) return;
        std::wstring days = L"[";
        for (const auto& day : lastStationSelection_.days) {
            if (days.size() > 1) days += L',';
            days += std::format(L"[\"{}\",{},{},{},{},{}]", CalendarDateText(day.ordinal),
                day.available, day.unknown, day.candidateAvailable,
                day.eligible ? L"true" : L"false", day.candidateUnknown);
        }
        days += L']';
        std::wstring stationIds = L"[";
        for (const auto& id : stationSelectionIds_) {
            if (stationIds.size() > 1) stationIds += L',';
            stationIds += L"\"" + JsonEscape(id) + L"\"";
        }
        stationIds += L']';
        webUi_->PostJson(L"{\"type\":\"station-selection-report\",\"stale\":"
            + std::wstring(stationSelectionSignature_ == StationSelectionSignature() ? L"false" : L"true")
            + L",\"summary\":" + StationSelectionSummaryJson() + L",\"days\":" + days
            + L",\"stationIds\":" + stationIds + L"}");
    }

    void PostStationSelectionDay(const std::wstring& date) const {
        if (!webUi_ || !webUi_->IsReady()
            || stationSelectionSignature_ != StationSelectionSignature()) return;
        const auto found = std::find_if(lastStationSelection_.days.begin(), lastStationSelection_.days.end(),
            [&](const auto& day) { return CalendarDateText(day.ordinal) == date && day.eligible; });
        if (found == lastStationSelection_.days.end()) return;
        std::wstring points = L"[", unknown = L"[";
        for (auto index : lastStationSelection_.selectedIndices) {
            if (index >= stations_.size()) continue;
            const auto& station = stations_[index];
            if (!station.hasCoordinate || !station.calendarCoverage) continue;
            const auto& coverage = *station.calendarCoverage;
            const bool provisional = CalendarHasDay(coverage.unverified, found->ordinal);
            const bool available = !provisional && CalendarHasDay(coverage.available, found->ordinal);
            if (!available && !provisional) continue;
            auto& target = provisional ? unknown : points;
            if (target.size() > 1) target += L',';
            target += std::format(L"[\"{}\",{:.6f},{:.6f}]", JsonEscape(Utf8ToWide(
                station.fourCharId.empty() ? station.stationId : station.fourCharId)),
                station.latitude, station.longitude);
        }
        points += L']'; unknown += L']';
        webUi_->PostJson(std::format(L"{{\"type\":\"station-selection-day\",\"date\":\"{}\","
            L"\"available\":{},\"unknown\":{},\"candidatePlan\":{},\"points\":{},\"unknownPoints\":{}}}",
            JsonEscape(date), found->available, found->unknown,
            lastStationSelection_.generated && !stationSelectionApplied_ ? L"true" : L"false", points, unknown));
    }

    void CancelStationSelection() {
        if (stationSelectionThread_.joinable()) stationSelectionThread_.request_stop();
    }

    void BeginStationSelection(const bool generate, const bool randomize = false) {
        if (stationSelectionBusy_ || querying_ || siteLogBlocksInteraction_ || MetadataFilterPending()) return;
        // Representative Site Log enrichment can replace coordinates. Wait
        // for that existing worker before certifying any distance constraint.
        if (siteLogLoading_ || (!siteLogEnrichmentComplete_ && !stations_.empty())) {
            SetOperationStatusText(L"测站详细信息正在加载，完成后可生成或检查选站方案。");
            PostWebUiState(); return;
        }
        StationSelectionPlanOptions options;
        std::wstring error;
        wchar_t* end = nullptr;
        const auto countText = GetWindowTextString(uniformCountEdit_);
        const auto count = wcstoull(countText.c_str(), &end, 10);
        if (countText.empty() || end == countText.c_str() || *end || count == 0 || count > 100'000
            || countText.front() == L'-') error = L"目标测站数量应为 1 至 100000 的整数。";
        double minimum = 0, maximum = 0;
        if (!ParseDialogDouble(GetWindowTextString(minimumDistanceEdit_), 0, 20'050, minimum)
            || !ParseDialogDouble(GetWindowTextString(maximumNeighborDistanceEdit_), 0, 20'050, maximum))
            error = L"站间距离应为 0 至 20050 km；0 表示不限制。";
        if (minimum > 0 && maximum > 0 && minimum > maximum)
            error = L"最小站间距不能大于区域组网的最大连边距离。";
        errno = 0;
        const auto seed = wcstoull(selectionRandomSeed_.c_str(), &end, 10);
        if (selectionRandomSeed_.empty() || end == selectionRandomSeed_.c_str() || *end
            || selectionRandomSeed_.front() == L'-' || seed == 0 || errno == ERANGE)
            error = L"随机种子应为大于 0 的整数。";
        const auto uiStart = std::format(L"{:04}-{:02}-{:02}", queryStartDate_.wYear, queryStartDate_.wMonth, queryStartDate_.wDay);
        const auto uiEnd = std::format(L"{:04}-{:02}-{:02}", queryEndDate_.wYear, queryEndDate_.wMonth, queryEndDate_.wDay);
        if (stations_.empty()) error = L"请先查询候选测站。";
        else if (uiStart != FormatDate(currentPeriodStart_) || uiEnd != FormatDate(currentPeriodEnd_)
            || GetNetworkSelection().mask != currentSelection_.mask)
            error = L"时间范围或数据源已变化，请先重新查询候选测站。";
        options.spatial = {static_cast<std::size_t>(count), minimum, maximum, randomize, seed};
        options.firstOrdinal = CalendarOrdinal(currentPeriodStart_.year, currentPeriodStart_.month, currentPeriodStart_.day);
        options.lastOrdinal = CalendarOrdinal(currentPeriodEnd_.year, currentPeriodEnd_.month, currentPeriodEnd_.day);
        options.eligibleDatesKnown = currentSelectionDatesKnown_;
        options.eligibleOrdinals = currentSelectionEligibleDates_;
        options.minimumDailyStations = selectionDailyMode_ == L"stations" ? selectionDailyStations_
            : selectionDailyMode_ == L"percent" ? static_cast<int>(std::ceil(selectionDailyPercent_ * count / 100.0 - 1e-9)) : 0;
        if (options.minimumDailyStations > static_cast<int>(count)) error = L"每日最低可用站数不能大于目标测站数量。";
        if (!error.empty()) {
            lastStationSelection_ = {}; lastStationSelection_.status = L"invalid"; lastStationSelection_.message = error;
            stationSelectionIds_.clear();
            stationSelectionApplied_ = false; stationSelectionSignature_ = StationSelectionSignature();
            SetOperationStatusText(error); PostStationSelectionReport(); PostWebUiState(); return;
        }
        if (stationSelectionThread_.joinable()) stationSelectionThread_.join();
        std::vector<StationFile> snapshot;
        snapshot.reserve(stations_.size());
        std::vector<std::size_t> selected;
        for (std::size_t index = 0; index < stations_.size(); ++index) {
            const auto& original = stations_[index];
            StationFile copy;
            copy.stationId = original.stationId; copy.fourCharId = original.fourCharId;
            copy.hasCoordinate = original.hasCoordinate; copy.latitude = original.latitude; copy.longitude = original.longitude;
            copy.calendarCoverage = original.calendarCoverage;
            snapshot.push_back(std::move(copy));
            if (original.selected) selected.push_back(index);
        }
        auto candidates = visibleStationIndices_;
        const auto signature = StationSelectionSignature(generate);
        const auto request = ++stationSelectionRequest_;
        const HWND target = window_;
        stationSelectionBusy_ = true;
        SetOperationStatusText(generate ? L"正在后台生成选站方案，逐日可用性与空间约束将一起检查…" : L"正在检查当前所选测站的逐日可用性…");
        PostWebUiState();
        stationSelectionThread_ = std::jthread([target, snapshot = std::move(snapshot), candidates = std::move(candidates),
            selected = std::move(selected), options = std::move(options), signature, request, generate](std::stop_token stop) {
            auto result = std::make_unique<StationSelectionWorkerResult>();
            result->signature = signature; result->request = request;
            result->candidateGeometry = generate;
            try {
                result->plan = generate ? GenerateStationSelectionPlan(snapshot, candidates, options, stop)
                    : EvaluateStationSelectionPlan(snapshot, candidates, selected, options, stop);
            } catch (const std::exception&) {
                result->plan.status = L"error"; result->plan.message = L"选站计算未完成，请缩短日期范围或减少候选数量后重试。";
            }
            if (PostMessageW(target, WM_IGS_SELECTION_READY, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
        });
    }

    void HandleStationSelectionReady(std::unique_ptr<StationSelectionWorkerResult> result) {
        if (!result || result->request != stationSelectionRequest_) return;
        stationSelectionBusy_ = false;
        if (result->signature != StationSelectionSignature(result->candidateGeometry)) {
            SetOperationStatusText(L"查询条件或所选测站已变化，本次选站结果已作废，请重新生成或检查。");
            PostWebUiState(); return;
        }
        stationSelectionApplied_ = result->plan.generated && result->plan.success;
        lastStationSelection_ = std::move(result->plan);
        // The report is a snapshot. A later query changes row indices, but
        // must never change the station identities in an earlier CSV/report.
        stationSelectionIds_.clear();
        for (auto index : lastStationSelection_.selectedIndices)
            if (index < stations_.size()) stationSelectionIds_.push_back(Utf8ToWide(stations_[index].stationId));
        if (stationSelectionApplied_) {
            for (auto& station : stations_) station.selected = false;
            for (auto index : lastStationSelection_.selectedIndices) if (index < stations_.size()) stations_[index].selected = true;
            PopulateStationList(); MapViewRefresh(map_);
        }
        stationSelectionSignature_ = StationSelectionSignature();
        if (lastStationSelection_.success) {
            SetOperationStatusText(std::format(L"{}：{} 个测站，{} 个评估日期，每日最少 {} 个已确认可用站；最小站间距 {:.1f} km。",
                lastStationSelection_.generated ? L"已应用选站方案" : L"当前选站检查通过",
                lastStationSelection_.selected, lastStationSelection_.activeDays,
                lastStationSelection_.minimumAvailable, lastStationSelection_.minimumDistanceKm));
        } else SetOperationStatusText(lastStationSelection_.message);
        UpdateSelectionSummary(); PostStationSelectionReport(); PostWebUiState();
    }

    void SelectGloballyDistributed(const bool randomize = false) {
        BeginStationSelection(true, randomize);
    }


    void BrowseFolder() {
        IFileDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(
                CLSID_FileOpenDialog,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&dialog)))) {
            NativeUiLanguage::ShowMessageBox(window_, L"无法打开文件夹选择器。", kApplicationTitle, MB_OK | MB_ICONERROR);
            return;
        }
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(NativeUiLanguage::Text(L"选择 GNSS 数据保存目录").c_str());
        if (SUCCEEDED(dialog->Show(window_))) {
            IShellItem* selected = nullptr;
            if (SUCCEEDED(dialog->GetResult(&selected))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    SetDownloadFolder(path);
                    CoTaskMemFree(path);
                }
                selected->Release();
            }
        }
        dialog->Release();
    }

    void AddSelectedStationsToDownloadPlan() {
        if (querying_ || siteLogBlocksInteraction_ || MetadataFilterPending()) {
            return;
        }
        const bool range = currentDateRangeQuery_;
        // A representative Site Log pass temporarily owns the daily rows.
        // Keep the visible station selectable and resolve its exact files
        // from the read-only date index when this queued item starts.
        const bool deferred = range && (currentPeriodFilesDeferred_
            || (siteLogLoading_ && currentPeriodFiles_.empty()));
        const QueryDate start = range ? currentPeriodStart_ : currentDate_;
        const QueryDate end = range ? currentPeriodEnd_ : currentDate_;
        if (range && !deferred) {
            constexpr std::size_t kMaximumStagedDailyFiles = 100'000;
            std::size_t selectedFiles = 0;
            for (const auto& dated : currentPeriodFiles_) {
                if (dated.stationIndex < stations_.size()
                    && stations_[dated.stationIndex].selected
                    && ++selectedFiles > kMaximumStagedDailyFiles) {
                    SetOperationStatusText(
                        L"所选范围超过 10 万个日文件；请缩短范围或分批添加下载计划。");
                    PostWebUiState();
                    return;
                }
            }
        }
        std::size_t added = 0;
        try {
        std::vector<ObservationQueuedDownload> additions;
        std::unordered_map<std::size_t, std::size_t> byStation;
        std::unordered_set<std::wstring> queuedKeys;
        queuedKeys.reserve(observationQueue_.size() + stations_.size());
        for (const auto& queued : observationQueue_) {
            if (queued.state != L"cancelled") queuedKeys.insert(queued.key);
        }
        additions.reserve(std::min<std::size_t>(stations_.size(), 256));
        for (std::size_t index = 0; index < stations_.size(); ++index) {
            const auto& station = stations_[index];
            if (!station.selected) continue;
            ObservationQueuedDownload item;
            item.id = ++nextObservationQueueId_;
            item.label = Utf8ToWide(station.stationId);
            item.key = std::format(L"{}|{}|{}|{}|{}|{}|{}|{}", item.label,
                FormatDate(start), FormatDate(end),
                Utf8ToWide(station.fileName), downloadFolder_,
                currentSelection_.mask,
                Utf8ToWide(station.preferredSource),
                station.downloadUrl);
            if (!queuedKeys.insert(item.key).second) {
                continue;
            }
            item.folder = downloadFolder_;
            item.range = range;
            item.deferred = deferred;
            item.verifyTemporalMetadata = HasMetadataAdvancedFilter()
                || advancedFilter_.observationCompletenessEnabled
                || advancedFilter_.latencyEnabled
                || advancedFilter_.positionTimeSeriesEnabled
                || advancedFilter_.residualsEnabled
                || advancedFilter_.meanMultipathEnabled
                || advancedFilter_.cycleSlipsEnabled;
            item.start = start;
            item.end = end;
            item.selection = currentSelection_;
            item.options = currentRangeOptions_;
            if (deferred) {
                item.deferredStations.push_back(station);
            } else if (!range) {
                item.files.push_back({currentDate_, index, station});
            }
            byStation.emplace(index, additions.size());
            additions.push_back(std::move(item));
        }
        if (range && !deferred) {
            for (const auto& dated : currentPeriodFiles_) {
                const auto found = byStation.find(dated.stationIndex);
                if (found != byStation.end()) {
                    additions[found->second].files.push_back(dated);
                }
            }
        }
        observationQueue_.reserve(observationQueue_.size()
            + additions.size());
        for (auto& item : additions) {
            if (item.files.empty() && item.deferredStations.empty()) continue;
            observationQueue_.push_back(std::move(item));
            ++added;
        }
        } catch (const std::bad_alloc&) {
            SetOperationStatusText(
                L"内存不足，未能加入下载计划；请缩短范围或减少测站。");
            PostWebUiState();
            return;
        }
        SetOperationStatusText(std::format(
            L"已将 {} 个测站添加到下载计划。", added));
        PostWebUiState();
        if (observationQueueRunning_) {
            DispatchNextObservationDownload();
        }
    }

    void DispatchNextObservationDownload() {
        if (!observationQueueRunning_ || downloadCancelRequested_ || postprocessBusy_) return;
        for (auto& item : observationQueue_) {
            if (planWorkers_.Size() >= DownloadPlanWorkers::limit) break;
            if (item.state != L"pending" || !item.selected) continue;
            item.state = L"running";
            item.percent = 0;
            activeObservationQueueId_ = item.id;
            StartDownload(true);
            if (!planWorkers_.Contains(item.id)) {
                item.state = L"failed";
                item.selected = false;
            }
        }
        activeObservationQueueId_ = planWorkers_.First();
        if (planWorkers_.Size() == 0) {
            observationQueueRunning_ = false;
            OfferObservationPostprocess();
        }
        PostWebUiState();
    }

    void StartObservationDownloadPlan() {
        if (downloading_ || postprocessBusy_) return;
        batchDownloadOptions_ = DownloadSettings::Load();
        batchPostprocessFiles_.clear();
        batchPostprocessCancelled_ = false;
        for (auto& item : observationQueue_) {
            if (item.selected && item.state != L"running") {
                item.state = L"pending";
                item.percent = 0;
            }
        }
        observationQueueRunning_ = true;
        DispatchNextObservationDownload();
    }

    void UpdateObservationDownloadPlanItem(
        const std::uint64_t id, const std::wstring_view action) {
        for (auto& item : observationQueue_) {
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
        std::erase_if(observationQueue_, [](const auto& item) {
            return item.removeAfterCancel && item.state != L"running";
        });
        PostWebUiState();
    }

    void StartDownload(const bool fromPlan = false) {
        if (postprocessBusy_) return;
        if (downloading_ && !fromPlan) {
            CancelDownload();
            return;
        }
        const auto queuedIt = std::ranges::find_if(observationQueue_,
            [&](const auto& item) {
                return fromPlan && item.id == activeObservationQueueId_;
            });
        const ObservationQueuedDownload* queued = queuedIt
            == observationQueue_.end() ? nullptr : &*queuedIt;
        if (queued == nullptr && (querying_ || siteLogBlocksInteraction_)) {
            return;
        }
        if (queued == nullptr && MetadataFilterPending()) {
            NativeUiLanguage::ShowMessageBox(
                window_,
                L"Site Log 元数据仍在加载，当前列表尚未完成接收机、天线或卫星系统筛选。"
                L"请等待加载完成后再下载。",
                kApplicationTitle,
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        std::vector<DatedStationFile> selected;
        std::vector<StationFile> deferredStations;
        const bool isRange = queued != nullptr
            ? queued->range : currentDateRangeQuery_;
        const bool hydrateDeferredRange = queued != nullptr
            ? queued->deferred
            : currentDateRangeQuery_ && currentPeriodFilesDeferred_;
        if (queued != nullptr) {
            selected = queued->files;
            deferredStations = queued->deferredStations;
        } else if (hydrateDeferredRange) {
            deferredStations.reserve(stations_.size());
            for (const auto& station : stations_) {
                if (station.selected) deferredStations.push_back(station);
            }
        } else if (isRange) {
            selected.reserve(currentPeriodFiles_.size());
            for (const auto& dated : currentPeriodFiles_) {
                if (dated.stationIndex < stations_.size()
                    && stations_[dated.stationIndex].selected) {
                    selected.push_back(dated);
                }
            }
        } else {
            selected.reserve(stations_.size());
            for (std::size_t index = 0; index < stations_.size(); ++index) {
                if (stations_[index].selected) {
                    selected.push_back({currentDate_, index, stations_[index]});
                }
            }
        }
        if (selected.empty() && deferredStations.empty()) {
            NativeUiLanguage::ShowMessageBox(window_, L"请先在地图或列表中选择至少一个测站。", kApplicationTitle, MB_OK | MB_ICONINFORMATION);
            return;
        }

        const std::filesystem::path root(queued != nullptr
            ? queued->folder : downloadFolder_);
        if (root.empty()) {
            NativeUiLanguage::ShowMessageBox(window_, L"请选择保存目录。", kApplicationTitle, MB_OK | MB_ICONWARNING);
            return;
        }

        if (queued == nullptr) {
            batchDownloadOptions_ = DownloadSettings::Load();
            batchPostprocessFiles_.clear();
            batchPostprocessCancelled_ = false;
        }
        // GEONET account credentials are used only by the offline knowledge-
        // base builder.  The public downloader deliberately keeps using the
        // anonymous Terras HTTPS workflow so released builds never request or
        // retain a user's Terras account.
        ClearTerrasSftpCredentials();

        if (!downloading_) {
            if (downloadThread_.joinable()) downloadThread_.join();
            downloadControl_ = std::make_shared<DownloadControl>();
            downloadCancelRequested_ = false;
            downloadTransferStarted_ = false;
        }
        downloading_ = true;
        SetBusyControls();
        SendMessageW(progress_, PBM_SETPOS, 0, 0);
        const std::size_t pendingCount = hydrateDeferredRange
            ? deferredStations.size() : selected.size();
        std::wstring preparationStatus;
        const QueryDate selectedStart = queued != nullptr
            ? queued->start : currentPeriodStart_;
        const QueryDate selectedEnd = queued != nullptr
            ? queued->end : currentPeriodEnd_;
        if (isRange && hydrateDeferredRange) {
            preparationStatus = std::format(
                L"正在为 {} 至 {} 的 {} 个已选测站生成下载清单…",
                FormatDate(selectedStart),
                FormatDate(selectedEnd),
                pendingCount);
        } else if (isRange) {
            preparationStatus = std::format(
                L"准备下载 {} 至 {} 的 {} 个日文件…",
                FormatDate(selectedStart),
                FormatDate(selectedEnd),
                pendingCount);
        } else {
            preparationStatus = std::format(
                L"准备下载 {} 个文件…", pendingCount);
        }
        SetOperationStatusText(preparationStatus);

        const HWND target = window_;
        const QueryDate deferredStart = selectedStart;
        const QueryDate deferredEnd = selectedEnd;
        const NetworkSelection deferredSelection = queued != nullptr
            ? queued->selection : currentSelection_;
        const ObservationRangeOptions deferredOptions = queued != nullptr
            ? queued->options : currentRangeOptions_;
        const bool verifyTemporalMetadata = queued != nullptr
            ? queued->verifyTemporalMetadata : (HasMetadataAdvancedFilter()
                || advancedFilter_.observationCompletenessEnabled
                || advancedFilter_.latencyEnabled
                || advancedFilter_.positionTimeSeriesEnabled
                || advancedFilter_.residualsEnabled
                || advancedFilter_.meanMultipathEnabled
                || advancedFilter_.cycleSlipsEnabled);
        const auto queueId = queued == nullptr ? 0 : queued->id;
        auto work = [target, queueId, root, downloadOptions = batchDownloadOptions_, control = downloadControl_,
             hydrateDeferredRange,
             deferredStart,
             deferredEnd,
             deferredSelection,
             deferredOptions,
             verifyTemporalMetadata,
             deferredStations = std::move(deferredStations),
             selected = std::move(selected)](
                const std::stop_token stopToken) mutable {
                auto finished = std::make_unique<DownloadFinished>();
                finished->datedFolders = downloadOptions.datedFolders;
                finished->journal = std::make_shared<DownloadJournal>(root, L"observations");
                if (hydrateDeferredRange) {
                    // The authoritative daily-file databases may contain a
                    // representative/current Site Log summary.  Do not let
                    // that legacy snapshot discard rows before the central
                    // time-aware index can evaluate the equipment actually
                    // installed on each observation date.
                    auto hydrationOptions = deferredOptions;
                    hydrationOptions.systemsInclude.clear();
                    hydrationOptions.systemsExclude.clear();
                    hydrationOptions.receiverInclude.clear();
                    hydrationOptions.receiverExclude.clear();
                    hydrationOptions.antennaInclude.clear();
                    hydrationOptions.antennaExclude.clear();
                    hydrationOptions.includeSystems.clear();
                    hydrationOptions.excludeSystems.clear();
                    hydrationOptions.includeReceiver.clear();
                    hydrationOptions.excludeReceiver.clear();
                    hydrationOptions.includeAntenna.clear();
                    hydrationOptions.excludeAntenna.clear();
                    ULONGLONG lastManifestProgressPost = 0;
                    const ObservationManifestProgressCallback manifestProgress =
                        [&](const ObservationManifestProgress& value) {
                            const ULONGLONG now = GetTickCount64();
                            const bool complete = value.totalUnits == 0
                                || value.completedUnits >= value.totalUnits;
                            if (!complete
                                && now - lastManifestProgressPost < 120) {
                                return;
                            }
                            lastManifestProgressPost = now;
                            const double fraction = value.totalUnits == 0
                                ? 1.0
                                : std::clamp(
                                    static_cast<double>(value.completedUnits)
                                        / static_cast<double>(value.totalUnits),
                                    0.0,
                                    1.0);
                            const double manifestWeight =
                                verifyTemporalMetadata ? 0.85 : 1.0;
                            auto progress = std::make_unique<DownloadProgress>();
                            progress->phase =
                                DownloadProgressPhase::PreparingManifest;
                            progress->completedFiles = static_cast<std::size_t>(
                                std::lround(fraction * manifestWeight * 1000.0));
                            progress->totalFiles = 1000;
                            progress->message = std::format(
                                L"生成下载清单：{}  {}/{} 个知识库，"
                                L"{}/{} 个日期查询块，已找到 {} 个文件",
                                value.currentSource.empty()
                                    ? L"准备中"
                                    : value.currentSource,
                                value.completedSources,
                                value.totalSources,
                                value.completedUnits,
                                value.totalUnits,
                                value.discoveredFiles);
                            if (PostMessageW(
                                    target,
                                    WM_IGS_DOWNLOAD_PROGRESS,
                                    static_cast<WPARAM>(queueId),
                                    reinterpret_cast<LPARAM>(progress.get()))) {
                                progress.release();
                            }
                        };
                    auto hydrated = LoadObservationDateRangeFilesForStations(
                        deferredStart,
                        deferredEnd,
                        deferredSelection,
                        hydrationOptions,
                        deferredStations,
                        stopToken,
                        manifestProgress);
                    if (!hydrated.error.empty() || stopToken.stop_requested()) {
                        if (stopToken.stop_requested()) {
                            finished->cancelled = true;
                        } else {
                            finished->failed = 1;
                            finished->errors.push_back(
                                L"无法生成已选测站下载清单："
                                + hydrated.error);
                        }
                        if (PostMessageW(
                                target,
                                WM_IGS_DOWNLOAD_FINISHED,
                                static_cast<WPARAM>(queueId),
                                reinterpret_cast<LPARAM>(finished.get()))) {
                            finished.release();
                        }
                        return;
                    }
                    if (!hydrated.warning.empty()) {
                        finished->warnings.push_back(
                            L"下载清单提示：" + hydrated.warning);
                    }
                    if (verifyTemporalMetadata) {
                        ULONGLONG lastMetadataProgressPost = 0;
                        const ObservationSearchDatedFilterProgressCallback
                            metadataProgress =
                                [&](const std::size_t processed,
                                    const std::size_t total) {
                                    const ULONGLONG now = GetTickCount64();
                                    const bool complete = total == 0
                                        || processed >= total;
                                    if (!complete
                                        && now - lastMetadataProgressPost < 120) {
                                        return;
                                    }
                                    lastMetadataProgressPost = now;
                                    const double fraction = total == 0
                                        ? 1.0
                                        : std::clamp(
                                            static_cast<double>(processed)
                                                / static_cast<double>(total),
                                            0.0,
                                            1.0);
                                    auto progress =
                                        std::make_unique<DownloadProgress>();
                                    progress->phase =
                                        DownloadProgressPhase::VerifyingMetadata;
                                    progress->completedFiles =
                                        static_cast<std::size_t>(std::lround(
                                            (0.85 + fraction * 0.15) * 1000.0));
                                    progress->totalFiles = 1000;
                                    progress->message = std::format(
                                        L"生成下载清单：正在按日期复核 Site Log 设备履历  {}/{}",
                                        processed,
                                        total);
                                    if (PostMessageW(
                                            target,
                                            WM_IGS_DOWNLOAD_PROGRESS,
                                            static_cast<WPARAM>(queueId),
                                            reinterpret_cast<LPARAM>(
                                                progress.get()))) {
                                        progress.release();
                                    }
                                };
                        auto verified = FilterObservationSearchDatedFiles(
                            DefaultObservationSearchIndexPath(),
                            deferredSelection,
                            deferredOptions,
                            hydrated.periodFiles,
                            true,
                            stopToken,
                            metadataProgress);
                        if (!verified.complete
                            || !verified.error.empty()
                            || stopToken.stop_requested()) {
                            if (stopToken.stop_requested()) {
                                finished->cancelled = true;
                            } else {
                                finished->failed = 1;
                                finished->errors.push_back(
                                    L"无法按每日设备履历复核下载清单："
                                    + verified.error);
                            }
                            if (PostMessageW(
                                    target,
                                    WM_IGS_DOWNLOAD_FINISHED,
                                    static_cast<WPARAM>(queueId),
                                    reinterpret_cast<LPARAM>(finished.get()))) {
                                finished.release();
                            }
                            return;
                        }
                    }
                    selected = std::move(hydrated.periodFiles);
                    if (selected.empty()) {
                        finished->failed = 1;
                        finished->errors.push_back(
                            L"所选测站在这个日期范围内没有符合条件的下载文件。");
                        if (PostMessageW(
                                target,
                                WM_IGS_DOWNLOAD_FINISHED,
                                static_cast<WPARAM>(queueId),
                                reinterpret_cast<LPARAM>(finished.get()))) {
                            finished.release();
                        }
                        return;
                    }
                }
                {
                    auto progress = std::make_unique<DownloadProgress>();
                    progress->phase = DownloadProgressPhase::Transfer;
                    progress->completedFiles = 0;
                    progress->totalFiles = selected.size();
                    progress->message = std::format(
                        L"下载清单已生成，共 {} 个日文件；"
                        L"自动并发已启用（全局最多 {}，按服务器独立调节）…",
                        selected.size(),
                        kAutomaticDownloadGlobalWorkerLimit);
                    if (PostMessageW(
                            target,
                            WM_IGS_DOWNLOAD_PROGRESS,
                            static_cast<WPARAM>(queueId),
                            reinterpret_cast<LPARAM>(progress.get()))) {
                        progress.release();
                    }
                }
                const auto dispatchOrder =
                    BuildAutomaticDownloadDispatchOrder(selected);
                auto& automaticConcurrency =
                    SharedAutomaticDownloadConcurrency();
                std::atomic_size_t nextIndex{0};
                std::atomic_size_t completedFiles{0};
                std::atomic<ULONGLONG> lastTransferProgressPost{0};
                std::mutex finishedMutex;

                const auto worker = [&]() {
                    while (!stopToken.stop_requested()) {
                    if (control != nullptr
                        && !control->waitIfPaused(stopToken)) {
                        return;
                    }
                    const std::size_t dispatchPosition = nextIndex.fetch_add(
                        1, std::memory_order_relaxed);
                    if (dispatchPosition >= dispatchOrder.size()) return;
                    const std::size_t index = dispatchOrder[dispatchPosition];
                    const auto& item = selected[index];
                    const auto& station = item.station;
                    const auto outputDirectory = downloadOptions.datedFolders
                        ? root / std::to_wstring(item.date.year) / FormatDoy(item.date.dayOfYear)
                        : root;
                    const std::wstring progressStation =
                        FormatDate(item.date) + L" / "
                        + Utf8ToWide(station.stationId);

                    const auto destinationFor = [&](const DownloadRoute* route) {
                        std::wstring fileName = Utf8ToWide(
                            route == nullptr
                                ? station.fileName
                                : route->fileName);
                        if ((route == nullptr ? station.downloadUrl : route->url).starts_with(L"geonet-jp://")
                            && (fileName.ends_with(L"d.gz")
                                || fileName.ends_with(L"D.GZ"))) {
                            // Existing SFTP-built databases may still contain
                            // the archive's *.d.gz name.  Anonymous HTTPS
                            // expands it and returns ordinary RINEX *.o.gz.
                            fileName[fileName.size() - 4U] = L'o';
                        }
                        return outputDirectory / fileName;
                    };
                    const DownloadDestinationLock destinationLock(
                        outputDirectory / Utf8ToWide(station.stationId), stopToken);
                    if (!destinationLock) return;
                    std::error_code fileError;
                    std::filesystem::path completedPath;
                    const auto existingPath = [&](const std::filesystem::path& path) {
                        std::error_code error;
                        if (DownloadFileLooksValid(path)) return path;
                        const auto standard = ObservationPostprocess::StandardRinexPath(path);
                        if (!standard.empty() && DownloadFileLooksValid(standard)) return standard;
                        return std::filesystem::path{};
                    };
                    bool alreadyExists = false;
                    if (station.downloadRoutes.empty()) {
                        const auto destination = destinationFor(nullptr);
                        completedPath = existingPath(destination);
                        alreadyExists = !completedPath.empty();
                    } else {
                        for (const auto& route : station.downloadRoutes) {
                            const auto destination = destinationFor(&route);
                            completedPath = existingPath(destination);
                            if (!completedPath.empty()) {
                                alreadyExists = true;
                                break;
                            }
                        }
                    }
                    if (alreadyExists) {
                        finished->journal->Record(L"ALREADY_PRESENT", progressStation, completedPath,
                            station.downloadUrl, 0);
                        {
                            std::scoped_lock lock(finishedMutex);
                            ++finished->skipped;
                            finished->completedPaths.push_back(completedPath.wstring());
                        }
                        const std::size_t completed =
                            completedFiles.fetch_add(
                                1, std::memory_order_relaxed) + 1;
                        auto progress = std::make_unique<DownloadProgress>();
                        progress->selectedIndex = index;
                        progress->completedFiles = completed;
                        progress->totalFiles = selected.size();
                        progress->stationId = progressStation;
                        progress->message = L"已存在，跳过";
                        if (PostMessageW(
                                target,
                                WM_IGS_DOWNLOAD_PROGRESS,
                                static_cast<WPARAM>(queueId),
                                reinterpret_cast<LPARAM>(progress.get()))) {
                            progress.release();
                        }
                        continue;
                    }

                    const DownloadProgressCallback progressCallback =
                        [&](const std::uint64_t bytes, const std::uint64_t total) {
                            const ULONGLONG now = GetTickCount64();
                            const bool completedTransfer =
                                total > 0 && bytes >= total;
                            if (!completedTransfer) {
                                ULONGLONG previous =
                                    lastTransferProgressPost.load(
                                        std::memory_order_relaxed);
                                if (now - previous < 80
                                    || !lastTransferProgressPost
                                            .compare_exchange_strong(
                                                previous,
                                                now,
                                                std::memory_order_relaxed)) {
                                    return;
                                }
                            } else {
                                lastTransferProgressPost.store(
                                    now, std::memory_order_relaxed);
                            }
                            auto progress = std::make_unique<DownloadProgress>();
                            progress->selectedIndex = index;
                            progress->completedFiles =
                                completedFiles.load(std::memory_order_relaxed);
                            progress->totalFiles = selected.size();
                            progress->downloadedBytes = bytes;
                            progress->totalBytes = total;
                            progress->stationId = progressStation;
                            if (PostMessageW(
                                    target,
                                    WM_IGS_DOWNLOAD_PROGRESS,
                                    static_cast<WPARAM>(queueId),
                                    reinterpret_cast<LPARAM>(progress.get()))) {
                                progress.release();
                            }
                        };
                    const bool allowFederatedPublicHttp =
                        HasNetwork(station.availableNetworks,
                            ObservationNetwork::EposGnss)
                        || HasNetwork(station.availableNetworks,
                            ObservationNetwork::OsNet)
                        || HasNetwork(station.availableNetworks,
                            ObservationNetwork::Ergnss)
                        || HasNetwork(station.availableNetworks,
                            ObservationNetwork::Renep)
                        || HasNetwork(station.availableNetworks,
                            ObservationNetwork::Redgae);
                    HttpDownloadResult download;
                    std::wstring effectiveSource = station.downloadUrl;
                    completedPath = destinationFor(nullptr);
                    std::wstring routeErrors;
                    if (!station.remoteParts.empty()) {
                        auto lease = automaticConcurrency.acquire(
                            L"rgna-bundle://official", stopToken, control);
                        if (!lease.has_value()) {
                            download.cancelled = true;
                            download.error = L"等待自动并发下载名额时已取消。";
                        } else {
                            download = HttpDownloadRgnaBundle(
                                station.remoteParts,
                                destinationFor(nullptr),
                                stopToken,
                                progressCallback,
                                control);
                            lease->finish(download);
                        }
                    } else if (station.downloadRoutes.empty()) {
                        const auto destination = destinationFor(nullptr);
                        if (std::filesystem::exists(destination, fileError)) {
                            std::filesystem::remove(destination, fileError);
                        }
                        auto lease = automaticConcurrency.acquire(
                            station.downloadUrl, stopToken, control);
                        if (!lease.has_value()) {
                            download.cancelled = true;
                            download.error = L"等待自动并发下载名额时已取消。";
                        } else {
                            download = HttpDownloadFile(
                                station.downloadUrl,
                                destination,
                                stopToken,
                                progressCallback,
                                allowFederatedPublicHttp,
                                control);
                            lease->finish(download);
                        }
                    } else {
                        std::vector<std::size_t> pendingRoutes(
                            station.downloadRoutes.size());
                        std::iota(
                            pendingRoutes.begin(), pendingRoutes.end(), 0U);
                        while (!pendingRoutes.empty()
                               && !download.success
                               && !download.cancelled) {
                            std::vector<std::wstring_view> candidateUrls;
                            candidateUrls.reserve(pendingRoutes.size());
                            for (const auto routeIndex : pendingRoutes) {
                                candidateUrls.push_back(
                                    station.downloadRoutes[routeIndex].url);
                            }
                            auto acquired = automaticConcurrency.acquireAny(
                                candidateUrls, stopToken, control);
                            if (!acquired.has_value()) {
                                download.cancelled = true;
                                download.error =
                                    L"等待自动并发下载名额时已取消。";
                                break;
                            }
                            const std::size_t pendingPosition =
                                acquired->candidateIndex;
                            const std::size_t routeIndex =
                                pendingRoutes[pendingPosition];
                            pendingRoutes.erase(
                                pendingRoutes.begin()
                                    + static_cast<std::ptrdiff_t>(
                                        pendingPosition));
                            const auto& route =
                                station.downloadRoutes[routeIndex];
                            const auto destination = destinationFor(&route);
                            if (std::filesystem::exists(destination, fileError)) {
                                std::filesystem::remove(destination, fileError);
                            }
                            effectiveSource = route.url;
                            download = HttpDownloadFile(
                                route.url,
                                destination,
                                stopToken,
                                progressCallback,
                                allowFederatedPublicHttp,
                                control);
                            acquired->lease.finish(download);
                            if (download.success) completedPath = destination;
                            if (download.success || download.cancelled) break;
                            if (!routeErrors.empty()) routeErrors += L"；";
                            routeErrors += Utf8ToWide(route.source)
                                + L"：" + download.error;
                        }
                        if (!download.success && !download.cancelled
                            && download.error.empty()) {
                            download.error =
                                L"本批任务中的所有 IGS 镜像均已检测为不可用。";
                        }
                    }

                    finished->journal->Record(download.success ? L"SUCCEEDED"
                        : download.cancelled ? L"CANCELLED" : L"FAILED", progressStation,
                        completedPath, effectiveSource, download.bytesWritten,
                        routeErrors.empty() ? download.error : routeErrors);
                    {
                        std::scoped_lock lock(finishedMutex);
                        if (download.success) {
                            ++finished->succeeded;
                            finished->completedPaths.push_back(completedPath.wstring());
                        } else if (download.cancelled) {
                            finished->cancelled = true;
                        } else {
                            ++finished->failed;
                            finished->errors.push_back(
                                progressStation + L"："
                                + (routeErrors.empty()
                                    ? download.error
                                    : routeErrors));
                        }
                    }

                    const std::size_t completed =
                        completedFiles.fetch_add(
                            1, std::memory_order_relaxed) + 1;
                    auto progress = std::make_unique<DownloadProgress>();
                    progress->selectedIndex = index;
                    progress->completedFiles = completed;
                    progress->totalFiles = selected.size();
                    progress->stationId = progressStation;
                    progress->message = download.success
                        ? L"完成"
                        : (download.cancelled ? L"已取消" : L"失败");
                    if (PostMessageW(
                            target,
                            WM_IGS_DOWNLOAD_PROGRESS,
                            static_cast<WPARAM>(queueId),
                            reinterpret_cast<LPARAM>(progress.get()))) {
                        progress.release();
                    }
                }
                };

                // The global pool is intentionally larger than any fragile
                // archive's allowance.  AutomaticDownloadConcurrency applies
                // independent host limits (Terras/FTP/SFTP stay conservative)
                // and adjusts only the server that reports pressure.
                const std::size_t workerCount = std::min<std::size_t>(
                    kAutomaticDownloadGlobalWorkerLimit,
                    dispatchOrder.size());
                std::vector<std::jthread> workers;
                workers.reserve(workerCount);
                for (std::size_t workerIndex = 0;
                     workerIndex < workerCount;
                     ++workerIndex) {
                    workers.emplace_back(worker);
                }
                workers.clear();

                if (stopToken.stop_requested()
                    && completedFiles.load(std::memory_order_relaxed)
                        < selected.size()) {
                    finished->cancelled = true;
                }

                if (PostMessageW(
                        target,
                        WM_IGS_DOWNLOAD_FINISHED,
                        static_cast<WPARAM>(queueId),
                        reinterpret_cast<LPARAM>(finished.get()))) {
                    finished.release();
                }
            };
        if (queueId != 0) planWorkers_.Start(queueId, std::move(work));
        else downloadThread_ = std::jthread(std::move(work));
    }

    void ToggleDownloadPause() {
        if (!downloading_ || !downloadTransferStarted_
            || downloadCancelRequested_
            || downloadControl_ == nullptr) {
            return;
        }
        if (downloadControl_->isPaused()) {
            downloadControl_->resume();
            SetWindowTextW(pauseDownloadButton_, L"暂停下载");
            SetOperationStatusText(L"已继续观测文件下载。下载线程正在恢复。…");
        } else {
            downloadControl_->pause();
            SetWindowTextW(pauseDownloadButton_, L"继续下载");
            SetOperationStatusText(
                L"观测文件下载已暂停；可点击“继续下载”恢复，"
                L"也可点击“取消下载”结束本次任务。");
        }
        PostWebUiState();
    }

    bool CancelDownload(const bool confirm = true) {
        if (postprocessBusy_) {
            if (confirm && NativeUiLanguage::ShowMessageBox(window_, L"取消解压与 RINEX 转换吗？已完成的结果会保留，未完成文件保留原件。",
                    kApplicationTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return false;
            postprocessThread_.request_stop();
            SetOperationStatusText(L"正在安全取消解压与 RINEX 转换…");
            PostWebUiState(); return true;
        }
        if (!downloading_) return false;
        if (downloadCancelRequested_) return true;
        if (confirm && NativeUiLanguage::ShowMessageBox(
                window_,
                L"确定取消本次下载吗？\n"
                L"已经下载完成的文件会保留，未完成的临时文件会由下载器清理。",
                kApplicationTitle,
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            return false;
        }
        observationQueueRunning_ = false;
        downloadCancelRequested_ = true;
        planWorkers_.StopAll();
        if (downloadThread_.joinable()) {
            // Publish the stop request before waking paused workers so none
            // of them can claim one more file in the small wake-up window.
            downloadThread_.request_stop();
        }
        if (downloadControl_ != nullptr) {
            // A paused worker must be woken after stop is visible.
            downloadControl_->resume();
        }
        SetBusyControls();
        SetOperationStatusText(
            L"正在取消下载…正在安全结束清单生成或传输任务。");
        PostWebUiState();
        return true;
    }

    void HandleDownloadProgress(std::unique_ptr<DownloadProgress> progress, const std::uint64_t queueId = 0) {
        if (queueId != 0 && !planWorkers_.Contains(queueId)) return;
        if (progress == nullptr || progress->totalFiles == 0) {
            return;
        }
        if (downloadCancelRequested_) {
            // Late progress messages can already be queued when the user
            // cancels.  Keep the explicit "cancelling" status visible until
            // the worker posts its final result.
            return;
        }
        const bool transfer = progress->phase
            == DownloadProgressPhase::Transfer;
        const double fileFraction = transfer && progress->totalBytes > 0
            ? std::clamp(static_cast<double>(progress->downloadedBytes)
                / static_cast<double>(progress->totalBytes), 0.0, 1.0)
            : 0.0;
        const double fraction = std::clamp(
            (static_cast<double>(progress->completedFiles) + fileFraction)
                / static_cast<double>(progress->totalFiles), 0.0, 1.0);
        // A queued range first builds its manifest, then transfers files.
        // Reserve the first 20% for preparation so the displayed per-item
        // progress does not jump backwards when transfer begins.
        const bool queued = queueId != 0;
        const int position = static_cast<int>(std::lround(
            (queued ? (transfer ? 0.2 + fraction * 0.8
                                : fraction * 0.2)
                    : fraction) * 1000.0));
        if (queued) {
            const auto found = std::ranges::find_if(observationQueue_,
                [&](const auto& item) {
                    return item.id == queueId;
                });
            if (found != observationQueue_.end()) {
                found->percent = std::clamp(position / 10, 0, 100);
            }
        }
        if (transfer && !downloadTransferStarted_) {
            downloadTransferStarted_ = true;
            SetBusyControls();
        }
        if (!transfer) {
            SendMessageW(progress_, PBM_SETPOS, position, 0);
            if (!progress->message.empty()) {
                SetOperationStatusText(progress->message);
            }
            PostWebUiProgress(
                position / 10,
                progress->message.empty()
                    ? L"Preparing download manifest…"
                    : progress->message);
            return;
        }
        SendMessageW(progress_, PBM_SETPOS, std::clamp(position, 0, 1000), 0);

        if (progress->stationId.empty() && !progress->message.empty()) {
            SetOperationStatusText(progress->message);
            PostWebUiProgress(position / 10, progress->message);
            return;
        }

        const std::size_t displayedFile = progress->message.empty()
            ? progress->completedFiles + 1
            : progress->completedFiles;
        std::wstring status = std::format(
            L"[{}/{}] {}",
            std::clamp<std::size_t>(
                displayedFile,
                1,
                progress->totalFiles),
            progress->totalFiles,
            progress->stationId);
        if (progress->totalBytes > 0) {
            status += L"  " + FormatBytes(progress->downloadedBytes)
                + L" / " + FormatBytes(progress->totalBytes);
        } else if (progress->downloadedBytes > 0) {
            status += L"  已下载 " + FormatBytes(progress->downloadedBytes)
                + L"（服务器未提供总大小，请继续等待）";
        }
        if (!progress->message.empty()) {
            status += L" — " + progress->message;
        }
        SetOperationStatusText(status);
        PostWebUiProgress(position / 10, status);
    }

    void HandleDownloadFinished(std::unique_ptr<DownloadFinished> finished, const std::uint64_t queueId = 0) {
        const auto activeQueueId = queueId;
        if (queueId != 0 && !planWorkers_.Contains(queueId)) return;
        if (queueId != 0) planWorkers_.Finish(queueId);
        const auto activeQueueItem = std::ranges::find_if(observationQueue_,
            [&](const auto& item) { return item.id == activeQueueId; });
        const bool queued = activeQueueId != 0
            && activeQueueItem != observationQueue_.end();
        const bool completedRange = queued
            ? activeQueueItem->range : currentDateRangeQuery_;
        const QueryDate completedStart = queued
            ? activeQueueItem->start : currentPeriodStart_;
        const QueryDate completedEnd = queued
            ? activeQueueItem->end : currentPeriodEnd_;
        const std::wstring completedFolder = queued
            ? activeQueueItem->folder : downloadFolder_;
        downloading_ = planWorkers_.Size() != 0;
        if (!downloading_) {
            downloadCancelRequested_ = false;
            downloadTransferStarted_ = false;
            if (downloadControl_) { downloadControl_->resume(); downloadControl_.reset(); }
            ClearTerrasSftpCredentials();
        }
        SetBusyControls();
        if (finished == nullptr) {
            if (queued) {
                activeQueueItem->state = L"failed";
                activeQueueItem->selected = false;
                activeObservationQueueId_ = planWorkers_.First();
                PostWebUiState();
                DispatchNextObservationDownload();
            }
            return;
        }
        batchPostprocessCancelled_ = batchPostprocessCancelled_ || finished->cancelled;
        for (const auto& path : finished->completedPaths) batchPostprocessFiles_.emplace_back(path);
        if (!downloading_ && !finished->cancelled) {
            SendMessageW(progress_, PBM_SETPOS, 1000, 0);
        }
        std::wstring summary = finished->cancelled
            ? std::format(
                L"下载已取消：成功 {}，已存在跳过 {}，失败 {}。"
                L"已完成的文件保留在 {}。",
                finished->succeeded,
                finished->skipped,
                finished->failed,
                 completedFolder)
            : !finished->datedFolders
            ? std::format(L"下载结束：成功 {}，已存在跳过 {}，失败 {}。统一保存到 {}。",
                finished->succeeded, finished->skipped, finished->failed, completedFolder)
            : completedRange
            ? std::format(
                L"日期范围 {} 至 {} 下载结束：成功 {}，已存在跳过 {}，失败 {}。保存到 {} 下各年/DOY目录。",
                 FormatDate(completedStart),
                 FormatDate(completedEnd),
                finished->succeeded,
                finished->skipped,
                finished->failed,
                 completedFolder)
            : std::format(
                L"下载结束：成功 {}，已存在跳过 {}，失败 {}。保存到 {}\\{}\\{}",
                finished->succeeded,
                finished->skipped,
                finished->failed,
                 completedFolder,
                 queued ? activeQueueItem->start.year : currentDate_.year,
                 FormatDoy(queued ? activeQueueItem->start.dayOfYear
                                  : currentDate_.dayOfYear));
        if (finished->journal) {
            finished->journal->Finish(finished->succeeded, finished->skipped,
                finished->failed, finished->cancelled, finished->errors, finished->warnings);
            summary += finished->journal->Path().empty()
                ? L" Download log could not be created in the save folder."
                : L" Download log: " + finished->journal->Path().wstring();
        }
        SetOperationStatusText(summary);

        if (!queued && (finished->failed > 0
            || !finished->warnings.empty())) {
            std::wstring details = summary + L"\n\n";
            const std::size_t limit = std::min<std::size_t>(finished->errors.size(), 8);
            for (std::size_t index = 0; index < limit; ++index) {
                details += L"• " + finished->errors[index] + L"\n";
            }
            if (!finished->errors.empty() && !finished->warnings.empty()) {
                details += L"\n";
            }
            const std::size_t warningLimit = std::min<std::size_t>(
                finished->warnings.size(), 8);
            for (std::size_t index = 0; index < warningLimit; ++index) {
                details += L"• " + finished->warnings[index] + L"\n";
            }
            NativeUiLanguage::ShowMessageBox(window_, details.c_str(), kApplicationTitle, MB_OK | MB_ICONWARNING);
        }
        if (!downloading_) PostWebUiProgress(finished->cancelled ? 0 : 100, summary);
        if (queued) {
            activeQueueItem->state = finished->cancelled ? L"cancelled"
                : finished->failed > 0 ? L"failed" : L"done";
            activeQueueItem->percent = finished->cancelled ? 0 : 100;
            activeQueueItem->selected = false;
            if (activeQueueItem->removeAfterCancel) {
                observationQueue_.erase(activeQueueItem);
            }
            activeObservationQueueId_ = planWorkers_.First();
        }
        PostWebUiState();
        if (queued && observationQueueRunning_) DispatchNextObservationDownload();
        else OfferObservationPostprocess();
    }

    void OfferObservationPostprocess() {
        if (postprocessBusy_ || observationQueueRunning_ || downloading_) return;
        auto files = std::move(batchPostprocessFiles_);
        batchPostprocessFiles_.clear();
        if (!batchDownloadOptions_.askObservationPostprocess || batchPostprocessCancelled_ || files.empty()) return;
        postprocessSucceeded_ = 0; postprocessFailed_ = 0;
        postprocessErrors_.clear();
        InspectObservationPostprocess(std::move(files));
    }

    void InspectObservationPostprocess(std::vector<std::filesystem::path> files) {
        if (postprocessThread_.joinable()) postprocessThread_.join();
        postprocessBusy_ = true;
        SetOperationStatusText(L"下载已结束，正在估算解压与 RINEX 转换所需空间…");
        PostWebUiProgress(0, L"正在检查文件和剩余磁盘空间…");
        PostWebUiState();
        postprocessThread_ = std::jthread([target = window_, files = std::move(files)](const std::stop_token stop) {
            auto estimate = std::make_unique<ObservationPostprocess::Estimate>(
                ObservationPostprocess::Inspect(files, ObservationPostprocess::ToolsDirectory()));
            if (PostMessageW(target, WM_IGS_POSTPROCESS_READY, 0,
                    reinterpret_cast<LPARAM>(estimate.get()))) estimate.release();
        });
    }

    void HandlePostprocessReady(std::unique_ptr<ObservationPostprocess::Estimate> estimate) {
        if (!estimate) return;
        if (postprocessThread_.get_stop_token().stop_requested()) {
            if (postprocessThread_.joinable()) postprocessThread_.join();
            postprocessBusy_ = false; SetOperationStatusText(L"已取消后处理，下载文件保留。"); PostWebUiState(); return;
        }
        if (postprocessThread_.joinable()) postprocessThread_.join();
        if (!estimate->error.empty()) {
            NativeUiLanguage::ShowMessageBox(window_, estimate->error.c_str(), kApplicationTitle, MB_OK | MB_ICONWARNING);
            postprocessBusy_ = false; SetOperationStatusText(estimate->error); PostWebUiState(); return;
        }
        if (estimate->files.empty()) {
            postprocessBusy_ = false;
            SetOperationStatusText(L"本次文件已是标准 RINEX，无需解压或转换。");
            PostWebUiState(); return;
        }
        std::wstring prompt = L"解压与 RINEX 转换\n\n本次待处理 " + std::to_wstring(estimate->files.size())
            + L" 个文件。解压后，将 Compact RINEX（d / crx）转为标准 RINEX（o / rnx）。\n"
              L"每个文件处理成功后删除对应压缩包及中间 Compact RINEX；失败或取消保留原文件。\n\n";
        for (const auto& volume : estimate->volumes) {
            prompt += volume.volume.wstring() + L"  建议额外预留：" + FormatBytes(volume.required)
                + L"；当前可用：" + FormatBytes(volume.available) + L"\n";
        }
        prompt += L"\n这是包含临时文件的保守估算，实际大小取决于文件内容；处理中仍会检查剩余空间。\n\n";
        if (!estimate->sufficient) {
            prompt += L"空间不足。请先清理上述磁盘，再点击“重试”重新检查；点击“取消”保留下载文件。";
            if (NativeUiLanguage::ShowMessageBox(window_, prompt.c_str(), kApplicationTitle, MB_RETRYCANCEL | MB_ICONWARNING) == IDRETRY) {
                InspectObservationPostprocess(std::move(estimate->files)); return;
            }
            postprocessBusy_ = false; SetOperationStatusText(L"已取消后处理，下载文件保留。"); PostWebUiState(); return;
        }
        prompt += L"现在开始处理吗？";
        if (NativeUiLanguage::ShowMessageBox(window_, prompt.c_str(), kApplicationTitle,
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            postprocessBusy_ = false; SetOperationStatusText(L"已跳过后处理，下载文件保留。"); PostWebUiState(); return;
        }
        postprocessThread_ = std::jthread([target = window_, estimate = std::move(*estimate)](const std::stop_token stop) {
            auto result = std::make_unique<ObservationPostprocess::Result>(ObservationPostprocess::Run(
                estimate, ObservationPostprocess::ToolsDirectory(), stop,
                [target](std::size_t completed, std::size_t total, const std::wstring& file) {
                    auto progress = std::make_unique<DownloadProgress>();
                    progress->completedFiles = completed; progress->totalFiles = total;
                    progress->stationId = file; progress->message = L"解压与 RINEX 转换";
                    if (PostMessageW(target, WM_IGS_DOWNLOAD_PROGRESS, 0,
                            reinterpret_cast<LPARAM>(progress.get()))) progress.release();
                }));
            if (PostMessageW(target, WM_IGS_POSTPROCESS_FINISHED, 0,
                    reinterpret_cast<LPARAM>(result.get()))) result.release();
        });
        SetOperationStatusText(L"正在解压与 RINEX 转换…"); PostWebUiState();
    }

    void HandlePostprocessFinished(std::unique_ptr<ObservationPostprocess::Result> result) {
        if (!result) return;
        if (postprocessThread_.joinable()) postprocessThread_.join();
        postprocessSucceeded_ += result->succeeded; postprocessFailed_ += result->failed;
        postprocessErrors_.insert(postprocessErrors_.end(), result->errors.begin(), result->errors.end());
        std::wstring summary = std::format(L"解压与 RINEX 转换{}：成功 {}，失败 {}。",
            result->cancelled ? L"已取消" : result->needsSpace ? L"因空间不足暂停" : L"完成",
            postprocessSucceeded_, postprocessFailed_);
        if (!postprocessErrors_.empty()) summary += L"失败文件保留原件。";
        SetOperationStatusText(summary);
        PostWebUiProgress(result->cancelled || result->needsSpace ? 0 : 100, summary);
        if (result->needsSpace) {
            InspectObservationPostprocess(std::move(result->remaining)); return;
        }
        postprocessBusy_ = false; SetBusyControls(); PostWebUiState();
        if (!postprocessErrors_.empty()) {
            auto details = summary + L"\n\n";
            for (std::size_t i = 0; i < std::min<std::size_t>(8, postprocessErrors_.size()); ++i) details += postprocessErrors_[i] + L"\n";
            NativeUiLanguage::ShowMessageBox(window_, details.c_str(), kApplicationTitle, MB_OK | MB_ICONWARNING);
        }
    }

    void PrepareForClose() {
        KillTimer(window_, kObservationFilterTimer);
        KillTimer(window_, kWebUiLoadingTimer);
        SaveDatePickerState(datePicker_, endDatePicker_);
        if (!downloadFolder_.empty()) {
            DownloadFolderState::Save(L"observation", downloadFolder_);
        }
        PreciseUiSnapshot preciseSnapshot{};
        if (QueryPreciseProductSnapshot(
                preciseProductView_, preciseSnapshot)) {
            if (!preciseSnapshot.folder.empty()) {
                DownloadFolderState::Save(L"precise", preciseSnapshot.folder);
            }
            SYSTEMTIME preciseStart{};
            SYSTEMTIME preciseEnd{};
            if (ParseSystemDate(preciseSnapshot.startDate, preciseStart)
                && ParseSystemDate(preciseSnapshot.endDate, preciseEnd)) {
                SaveStoredDateRange(
                    L"precise_dates", preciseStart, preciseEnd);
            }
        }
        EnableWindow(window_, FALSE);
        if (queryThread_.joinable()) {
            queryThread_.request_stop();
            queryThread_.join();
        }
        if (siteLogThread_.joinable()) {
            siteLogThread_.request_stop();
            siteLogThread_.join();
            siteLogLoading_ = false;
            siteLogBlocksInteraction_ = false;
        }
        if (postprocessThread_.joinable()) {
            postprocessThread_.request_stop(); postprocessThread_.join();
        }
        planWorkers_.JoinAll();
        if (downloadThread_.joinable()) {
            downloadThread_.request_stop();
            if (downloadControl_ != nullptr) {
                downloadControl_->resume();
            }
            downloadThread_.join();
        }
        if (knowledgeBaseStatusThread_.joinable()) {
            knowledgeBaseStatusThread_.request_stop();
            knowledgeBaseStatusThread_.join();
            knowledgeBaseStatusLoading_ = false;
        }
        if (observationSearchPreloadThread_.joinable()) {
            observationSearchPreloadThread_.request_stop();
            observationSearchPreloadThread_.join();
        }
        downloadControl_.reset();
        ClearTerrasSftpCredentials();
    }

private:
    using NetworkControl = std::pair<HWND, ObservationNetwork>;

    std::array<NetworkControl, 42> NetworkControls() const {
        return {{
            {igsCheck_, ObservationNetwork::Igs},
            {mgexCheck_, ObservationNetwork::Mgex},
            {ngsCheck_, ObservationNetwork::Ngs},
            {epnCheck_, ObservationNetwork::Epn},
            {hkCheck_, ObservationNetwork::Hk},
            {gaGdcCheck_, ObservationNetwork::GaGdc},
            {rgnaCheck_, ObservationNetwork::Rgna},
            {sonelCheck_, ObservationNetwork::Sonel},
            {trigNetCheck_, ObservationNetwork::TrigNet},
            {cutCheck_, ObservationNetwork::Cut},
            {csnCheck_, ObservationNetwork::Csn},
            {eposIcelandCheck_, ObservationNetwork::EposIceland},
            {rbmcCheck_, ObservationNetwork::Rbmc},
            {ramsacCheck_, ObservationNetwork::Ramsac},
            {regnaRouCheck_, ObservationNetwork::RegnaRou},
            {cacsCheck_, ObservationNetwork::Cacs},
            {rgpCheck_, ObservationNetwork::Rgp},
            {sopacGarnerCheck_, ObservationNetwork::SopacGarner},
            {magnaEcoCheck_, ObservationNetwork::MagnaEco},
            {panamaCorsCheck_, ObservationNetwork::PanamaCors},
            {hondurasCorsCheck_, ObservationNetwork::HondurasCors},
            {eposGnssCheck_, ObservationNetwork::EposGnss},
            {grefCheck_, ObservationNetwork::Gref},
            {osNetCheck_, ObservationNetwork::OsNet},
            {ergnssCheck_, ObservationNetwork::Ergnss},
            {renepCheck_, ObservationNetwork::Renep},
            {geoNetNzCheck_, ObservationNetwork::GeoNetNz},
            {ringCheck_, ObservationNetwork::Ring},
            {aposCheck_, ObservationNetwork::Apos},
            {robBelgiumCheck_, ObservationNetwork::RobBelgium},
            {dpgaCheck_, ObservationNetwork::Dpga},
            {catNetCheck_, ObservationNetwork::CatNet},
            {spsLuxCheck_, ObservationNetwork::SpsLux},
            {kasiNgiiCheck_, ObservationNetwork::KasiNgii},
            {renagCheck_, ObservationNetwork::Renag},
            {nsgiCheck_, ObservationNetwork::Nsgi},
            {noaGnssCheck_, ObservationNetwork::NoaGnss},
            {redgaeCheck_, ObservationNetwork::Redgae},
            {geoNetJapanCheck_, ObservationNetwork::GeoNetJapan},
            {dlabCheck_, ObservationNetwork::Dlab},
            {magnetCheck_, ObservationNetwork::Magnet},
            {pangaCwuCheck_, ObservationNetwork::PangaCwu}
        }};
    }

    HWND CreateStatic(const wchar_t* text, const DWORD style) const {
        return CreateWindowExW(
            0,
            L"STATIC",
            text,
            WS_CHILD | WS_VISIBLE | style,
            0, 0, 100, 24,
            window_,
            nullptr,
            instance_,
            nullptr);
    }

    HWND CreateButton(
        const wchar_t* text,
        const int id,
        const DWORD extraStyle = 0) const {
        const DWORD requestedType = extraStyle & BS_TYPEMASK;
        const bool keepNativeType = requestedType == BS_RADIOBUTTON
            || requestedType == BS_AUTORADIOBUTTON
            || requestedType == BS_CHECKBOX
            || requestedType == BS_AUTOCHECKBOX;
        const DWORD buttonStyle = (extraStyle & ~BS_TYPEMASK)
            | (keepNativeType ? requestedType : BS_OWNERDRAW);
        return CreateWindowExW(
            0,
            L"BUTTON",
            text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP
                | buttonStyle,
            0, 0, 100, 30,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance_,
            nullptr);
    }

    HWND CreateCheckBox(const wchar_t* text, const int id) const {
        return CreateWindowExW(
            0,
            L"BUTTON",
            text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 100, 28,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance_,
            nullptr);
    }

    HWND CreateScopeEdit(const int id, const wchar_t* cue) const {
        const HWND edit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP
                | ES_CENTER | ES_AUTOHSCROLL,
            0, 0, 60, 31,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance_,
            nullptr);
        if (edit != nullptr) {
            SendMessageW(edit, EM_SETLIMITTEXT, 12, 0);
            SendMessageW(
                edit,
                EM_SETCUEBANNER,
                TRUE,
                reinterpret_cast<LPARAM>(cue));
        }
        return edit;
    }

    void SyncNetworkSelectionFromLegacyControls() {
        selectedNetworkMask_ = GetLegacyNetworkSelection().mask;
    }

    NetworkSelection GetNetworkSelection() const {
        NetworkSelection selection;
        selection.mask = selectedNetworkMask_;
        return selection;
    }

    NetworkSelection GetLegacyNetworkSelection() const {
        NetworkSelection selection;
        selection.mask = 0;
        if (SendMessageW(igsCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Igs);
        }
        if (SendMessageW(mgexCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Mgex);
        }
        if (SendMessageW(ngsCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Ngs);
        }
        if (SendMessageW(epnCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Epn);
        }
        if (SendMessageW(hkCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Hk);
        }
        if (SendMessageW(gaGdcCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::GaGdc);
        }
        if (SendMessageW(rgnaCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Rgna);
        }
        if (SendMessageW(sonelCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Sonel);
        }
        if (SendMessageW(trigNetCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::TrigNet);
        }
        if (SendMessageW(cutCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Cut);
        }
        if (SendMessageW(csnCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Csn);
        }
        if (SendMessageW(eposIcelandCheck_, BM_GETCHECK, 0, 0)
            == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::EposIceland);
        }
        if (SendMessageW(rbmcCheck_, BM_GETCHECK, 0, 0)
            == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Rbmc);
        }
        if (SendMessageW(ramsacCheck_, BM_GETCHECK, 0, 0)
            == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Ramsac);
        }
        if (SendMessageW(regnaRouCheck_, BM_GETCHECK, 0, 0)
            == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::RegnaRou);
        }
        if (SendMessageW(cacsCheck_, BM_GETCHECK, 0, 0)
            == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Cacs);
        }
        if (SendMessageW(rgpCheck_, BM_GETCHECK, 0, 0)
            == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Rgp);
        }
        if (SendMessageW(sopacGarnerCheck_, BM_GETCHECK, 0, 0)
            == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::SopacGarner);
        }
        if (SendMessageW(magnaEcoCheck_, BM_GETCHECK, 0, 0)
            == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::MagnaEco);
        }
        if (SendMessageW(panamaCorsCheck_, BM_GETCHECK, 0, 0)
            == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::PanamaCors);
        }
        if (SendMessageW(hondurasCorsCheck_, BM_GETCHECK, 0, 0)
            == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::HondurasCors);
        }
        if (SendMessageW(eposGnssCheck_, BM_GETCHECK, 0, 0)
            == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::EposGnss);
        }
        if (SendMessageW(grefCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Gref);
        }
        if (SendMessageW(osNetCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::OsNet);
        }
        if (SendMessageW(ergnssCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Ergnss);
        }
        if (SendMessageW(renepCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Renep);
        }
        if (SendMessageW(geoNetNzCheck_, BM_GETCHECK, 0, 0)
            == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::GeoNetNz);
        }
        if (SendMessageW(ringCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Ring);
        }
        if (SendMessageW(aposCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Apos);
        }
        if (SendMessageW(robBelgiumCheck_, BM_GETCHECK, 0, 0)
            == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::RobBelgium);
        }
        if (SendMessageW(dpgaCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Dpga);
        }
        if (SendMessageW(catNetCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::CatNet);
        }
        if (SendMessageW(spsLuxCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::SpsLux);
        }
        if (SendMessageW(kasiNgiiCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::KasiNgii);
        }
        if (SendMessageW(renagCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Renag);
        }
        if (SendMessageW(nsgiCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Nsgi);
        }
        if (SendMessageW(noaGnssCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::NoaGnss);
        }
        if (SendMessageW(redgaeCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Redgae);
        }
        if (SendMessageW(geoNetJapanCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::GeoNetJapan);
        }
        if (SendMessageW(dlabCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Dlab);
        }
        if (SendMessageW(magnetCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::Magnet);
        }
        if (SendMessageW(pangaCwuCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selection.mask |= NetworkBit(ObservationNetwork::PangaCwu);
        }
        return selection;
    }

    static void ApplyFont(const HWND control, const HFONT font) {
        if (control != nullptr && font != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
    }

    void AddListColumn(
        const int index,
        const wchar_t* title,
        const int width) const {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(title);
        column.cx = width;
        column.iSubItem = index;
        ListView_InsertColumn(stationList_, index, &column);
    }

    void ResizeStationColumns(const int availableWidth) const {
        // The observation card is intentionally allowed to be compact next
        // to the map.  Scale secondary metadata columns down first so the
        // station code, network and completeness columns remain visible on a
        // laptop instead of opening with an almost-one-kilopixel horizontal
        // scroll range.
        constexpr std::array<int, 8> minimum{
            60, 56, 42, 56, 34, 60, 54, 62};
        constexpr std::array<int, 8> preferred{
            110, 110, 92, 100, 55, 190, 170, 125};
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
        std::array<int, 8> widths = minimum;
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
                stationList_, static_cast<int>(index), widths[index]);
        }
    }

    LRESULT HandleListHeaderNotify(const NMHEADERW& notification) const {
        if (notification.pitem == nullptr
            || !(notification.pitem->mask & HDI_WIDTH)
            || notification.iItem < 0
            || notification.iItem >= 8) {
            return 0;
        }
        constexpr int minimumWidths[] = {
            60, 56, 42, 56, 34, 60, 54, 62};
        if (notification.iItem == 7) {
            return notification.hdr.code == HDN_ITEMCHANGINGW ? TRUE : 0;
        }
        const int minimum = minimumWidths[notification.iItem];
        if (notification.pitem->cxy < minimum) {
            if (notification.hdr.code == HDN_ITEMCHANGEDW) {
                ListView_SetColumnWidth(
                    stationList_, notification.iItem, minimum);
            }
            return notification.hdr.code == HDN_ITEMCHANGINGW ? TRUE : 0;
        }
        return 0;
    }

    static std::vector<std::wstring> SplitAdvancedTokens(const std::string& value) {
        const std::wstring text = ToUpperWide(Utf8ToWide(value));
        std::vector<std::wstring> tokens;
        std::wstring current;
        const auto flush = [&]() {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        };
        for (const wchar_t character : text) {
            if (character == L',' || character == L';' || character == L'/'
                || character == L'|' || std::iswspace(character)) {
                flush();
            } else {
                current += character;
            }
        }
        flush();
        return tokens;
    }

    static bool AdvancedTokensIncluded(
        const std::string& haystackValue,
        const std::string& include,
        const std::string& exclude) {
        const std::wstring haystack = ToUpperWide(Utf8ToWide(haystackValue));
        for (const auto& token : SplitAdvancedTokens(include)) {
            if (haystack.find(token) == std::wstring::npos) return false;
        }
        for (const auto& token : SplitAdvancedTokens(exclude)) {
            if (!token.empty() && haystack.find(token) != std::wstring::npos) {
                return false;
            }
        }
        // An explicit metadata criterion must never silently pass a station
        // for which no Site Log was available at the query epoch.
        if ((!include.empty() || !exclude.empty()) && haystack.empty()) return false;
        return true;
    }

    static bool AdvancedSystemsIncluded(
        const std::string& value,
        const std::string& include,
        const std::string& exclude) {
        const auto actual = CanonicalSatelliteSystemTokens(value);
        const auto included = CanonicalSatelliteSystemTokens(include);
        const auto excluded = CanonicalSatelliteSystemTokens(exclude);
        if ((!include.empty() || !exclude.empty()) && actual.empty()) {
            return false;
        }
        for (const auto& token : included) {
            if (std::find(actual.begin(), actual.end(), token) == actual.end()) {
                return false;
            }
        }
        for (const auto& token : excluded) {
            if (std::find(actual.begin(), actual.end(), token) != actual.end()) {
                return false;
            }
        }
        return true;
    }

    static bool AdvancedModelsIncluded(
        const std::string& modelValue,
        const std::string& include,
        const std::string& exclude) {
        const std::string model = ChoiceValueKey(modelValue);
        const auto includes = SplitChoiceSelections(include);
        const auto excludes = SplitChoiceSelections(exclude);
        if ((!includes.empty() || !excludes.empty()) && model.empty()) {
            return false;
        }
        // Receiver and antenna checklist entries are complete model names.
        // Multiple included models mean OR, not an impossible requirement
        // that one station contain every selected name at once.
        if (!includes.empty()
            && std::none_of(
                includes.begin(), includes.end(),
                [&](const std::string& value) {
                    return ChoiceValueKey(value) == model;
                })) {
            return false;
        }
        return std::none_of(
            excludes.begin(), excludes.end(),
            [&](const std::string& value) {
                return ChoiceValueKey(value) == model;
            });
    }

    static bool MatchesAdvancedMetadataFilter(
        const StationFile& station,
        const AdvancedFilterState& filter) {
        if (filter.rinexVersion == 2
            && !HasRinexFormat(station, RinexFormat::Rinex2)) {
            return false;
        }
        if (filter.rinexVersion == 3
            && !HasRinexFormat(station, RinexFormat::Rinex34)) {
            return false;
        }
        if (!filter.sampleInterval.empty()) {
            std::wstring expected = ToUpperWide(
                Utf8ToWide(filter.sampleInterval));
            std::wstring actual = ToUpperWide(Utf8ToWide(station.interval));
            if (NormalizeObservationInterval(actual)
                != NormalizeObservationInterval(expected)) {
                return false;
            }
        }
        if (!AdvancedSystemsIncluded(
                station.siteLogSystems,
                filter.systemsInclude,
                filter.systemsExclude)) {
            return false;
        }
        if (!AdvancedModelsIncluded(
                station.siteLogReceiver,
                filter.receiverInclude,
                filter.receiverExclude)) {
            return false;
        }
        if (!AdvancedModelsIncluded(
                station.siteLogAntenna,
                filter.antennaInclude,
                filter.antennaExclude)) {
            return false;
        }
        return true;
    }

    static bool MatchesAdvancedCompletenessFilter(
        const StationFile& station,
        const AdvancedFilterState& filter) {
        if (filter.completenessMode
            == AdvancedFilterState::CompletenessMode::RatioAtLeast) {
            if (station.expectedDays <= 0
                || station.completenessPercent
                    < filter.minimumCompletenessPercent) {
                return false;
            }
        } else if (filter.completenessMode
                   == AdvancedFilterState::CompletenessMode::DaysAtLeast) {
            if (station.availableDays < filter.minimumAvailableDays) {
                return false;
            }
        }
        return true;
    }

    bool MatchesAdvancedFilter(const StationFile& station) const {
        return MatchesAdvancedMetadataFilter(station, advancedFilter_)
            && MatchesAdvancedCompletenessFilter(station, advancedFilter_);
    }

    static std::string DisplayAreaCode(const StationFile& station) {
        if (station.hasCoordinate) {
            return CountryFlags::ResolveStationArea(station.country, station.siteLogCountry,
                station.stationId, station.preferredSource == "NGS",
                station.latitude, station.longitude, station.fourCharId);
        }
        return CountryFlags::ResolveArea(station.country, station.siteLogCountry,
            station.stationId, station.preferredSource == "NGS");
    }

    static std::string DisplayCountryCode(const StationFile& station) {
        return CountryFlags::Normalize(DisplayAreaCode(station));
    }

    static std::string DisplayStationAreaCode(const StationFile& station) {
        return DisplayAreaCode(station);
    }

    static std::string DisplayStationCountryCode(const StationFile& station) {
        return CountryFlags::Normalize(DisplayStationAreaCode(station));
    }

    static std::string DisplayStationFlagLabelCode(const StationFile& station) {
        const auto area = DisplayStationAreaCode(station);
        return CountryPolicy::OwnFlagArea(area)
            ? area : CountryFlags::Normalize(area);
    }

    static int DisplayFlagIndex(const StationFile& station) {
        const auto area = DisplayStationAreaCode(station);
        if (CountryPolicy::OwnFlagArea(area)) return CountryFlags::Index(area);
        return CountryFlags::Index(CountryFlags::Normalize(area));
    }

    static int CountryChoiceFlagIndex(const std::string_view code) {
        if (CountryPolicy::OwnFlagArea(code)) return CountryFlags::Index(code);
        return CountryFlags::Index(CountryFlags::Normalize(code));
    }

    static bool MatchesCountryChoice(
        const StationFile& station, const std::string_view chosenCode) {
        const auto area = DisplayAreaCode(station);
        if (CountryPolicy::SelectableRegionArea(chosenCode)) return area == chosenCode;
        return CountryFlags::Normalize(area) == chosenCode;
    }

    bool MatchesFilter(const StationFile& station, const std::wstring& filter) const {
        if (!station.inSelectionScope) {
            return false;
        }
        if (filter.empty()) {
            return true;
        }
        const std::wstring haystack = ToUpperWide(
            Utf8ToWide(station.stationId)
            + L" "
            + FormatAvailableNetworks(station.availableNetworks)
            + L" "
            + Utf8ToWide(station.country)
            + L" "
            + Utf8ToWide(DisplayStationCountryCode(station))
            + L" "
            + Utf8ToWide(station.siteLogDomes)
            + L" "
            + Utf8ToWide(station.siteLogName)
            + L" "
            + Utf8ToWide(station.siteLogCity)
            + L" "
            + Utf8ToWide(station.siteLogCountry)
            + L" "
            + Utf8ToWide(station.siteLogReceiver)
            + L" "
            + Utf8ToWide(station.siteLogSystems)
            + L" "
            + Utf8ToWide(station.siteLogReceiverSerial)
            + L" "
            + Utf8ToWide(station.siteLogFirmware)
            + L" "
            + Utf8ToWide(station.siteLogReceiverDateInstalled)
            + L" "
            + Utf8ToWide(station.siteLogAntenna)
            + L" "
            + Utf8ToWide(station.siteLogRadome)
            + L" "
            + Utf8ToWide(station.siteLogAntennaSerial)
            + L" "
            + Utf8ToWide(station.siteLogAntennaReferencePoint)
            + L" "
            + Utf8ToWide(station.siteLogAntennaDateInstalled)
            + L" "
            + Utf8ToWide(station.siteLogAgency)
            + L" "
            + Utf8ToWide(station.siteLogOperator)
            + L" "
            + Utf8ToWide(station.siteLogRevisionDate)
            + L" "
            + Utf8ToWide(station.siteLogSourceDirectory)
            + L" "
            + Utf8ToWide(station.siteLogUrl));
        return haystack.find(filter) != std::wstring::npos;
    }

    void PopulateStationList() {
        const std::wstring filter = ToUpperWide(filterText_);
        syncingList_ = true;
        SendMessageW(stationList_, WM_SETREDRAW, FALSE, 0);
        visibleStationIndices_.clear();
        visibleStationIndices_.reserve(stations_.size());
        for (std::size_t index = 0; index < stations_.size(); ++index) {
            const auto& station = stations_[index];
            if (MatchesFilter(station, filter)) {
                visibleStationIndices_.push_back(index);
            }
        }
        ++webStationDataRevision_;
        if (webStationDataRevision_ == 0) ++webStationDataRevision_;
        ListView_SetItemCountEx(
            stationList_,
            static_cast<int>(visibleStationIndices_.size()),
            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
        // The checkbox state is the business selection. Clear the native
        // current-row selection so a filter refresh cannot leave an old row
        // painted as if it were selected for download.
        ListView_SetItemState(
            stationList_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetSelectionMark(stationList_, -1);
        ListView_SetItemState(
            stationList_,
            -1,
            INDEXTOSTATEIMAGEMASK(1),
            LVIS_STATEIMAGEMASK);
        for (std::size_t row = 0; row < visibleStationIndices_.size(); ++row) {
            if (stations_[visibleStationIndices_[row]].selected) {
                ListView_SetCheckState(
                    stationList_, static_cast<int>(row), TRUE);
            }
        }

        SendMessageW(stationList_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(stationList_, nullptr, TRUE);
        syncingList_ = false;
        UpdateSelectionSummary();
    }

public:
    HWND StationListHandle() const {
        return stationList_;
    }

    [[nodiscard]] bool IsPreciseMode() const noexcept {
        return preciseMode_;
    }

    [[nodiscard]] bool IsObservationOperationBusy() const noexcept {
        return querying_ || downloading_ || postprocessBusy_;
    }

    LRESULT OnStationListHeaderNotify(const NMHEADERW& notification) const {
        return HandleListHeaderNotify(notification);
    }

    void HandleCountryComboMeasureItem(MEASUREITEMSTRUCT& measure) const {
        if (measure.CtlID == IdScopeCountry) {
            measure.itemHeight = 24;
            measure.itemWidth = 108;
        }
    }

    void HandleCountryComboDrawItem(const DRAWITEMSTRUCT& draw) const {
        if (draw.CtlID != IdScopeCountry) {
            return;
        }
        const int item = draw.itemID == static_cast<UINT>(-1)
            ? static_cast<int>(SendMessageW(scopeCountry_, CB_GETCURSEL, 0, 0))
            : static_cast<int>(draw.itemID);
        if (item < 0) return;
        const int length = static_cast<int>(SendMessageW(
            scopeCountry_, CB_GETLBTEXTLEN, item, 0));
        std::wstring text;
        if (length > 0) {
            text.resize(static_cast<std::size_t>(length) + 1U);
            SendMessageW(
                scopeCountry_, CB_GETLBTEXT, item,
                reinterpret_cast<LPARAM>(text.data()));
            text.resize(static_cast<std::size_t>(length));
        }
        const auto flag = static_cast<int>(SendMessageW(
            scopeCountry_, CB_GETITEMDATA, item, 0));
        if (flag >= 0 && text.size() > 3) text.resize(3);
        const bool selected = (draw.itemState & ODS_SELECTED) != 0;
        const COLORREF background = selected
            ? GetSysColor(COLOR_HIGHLIGHT)
            : GetSysColor(COLOR_WINDOW);
        const COLORREF foreground = selected
            ? GetSysColor(COLOR_HIGHLIGHTTEXT)
            : GetSysColor(COLOR_WINDOWTEXT);
        const auto brush = CreateSolidBrush(background);
        if (brush != nullptr) {
            FillRect(draw.hDC, &draw.rcItem, brush);
            DeleteObject(brush);
        }
        SetBkMode(draw.hDC, TRANSPARENT);
        SetTextColor(draw.hDC, foreground);
        RECT textRect = draw.rcItem;
        textRect.left += 4;
        textRect.right = flag >= 0
            ? std::min(textRect.right, draw.rcItem.left + 52)
            : textRect.right - 4;
        DrawTextW(
            draw.hDC, text.c_str(), static_cast<int>(text.size()), &textRect,
            DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        if (countryFlags_ != nullptr && flag >= 0) {
            ImageList_Draw(
                countryFlags_, flag, draw.hDC,
                draw.rcItem.left + 56, draw.rcItem.top, ILD_NORMAL);
        }
        if ((draw.itemState & ODS_FOCUS) != 0) {
            DrawFocusRect(draw.hDC, &draw.rcItem);
        }
    }

    void HandleListGetDisplayInfo(NMLVDISPINFOW& notification) const {
        const int row = notification.item.iItem;
        if (row < 0
            || static_cast<std::size_t>(row) >= visibleStationIndices_.size()
            || !(notification.item.mask & LVIF_TEXT)
            || notification.item.pszText == nullptr
            || notification.item.cchTextMax <= 0) {
            return;
        }
        const auto stationIndex = visibleStationIndices_[row];
        if (stationIndex >= stations_.size()) {
            return;
        }
        const auto& station = stations_[stationIndex];
        std::wstring text;
        switch (notification.item.iSubItem) {
            case 0: text = Utf8ToWide(station.stationId); break;
            case 1: text = FormatAvailableNetworks(station.availableNetworks); break;
            case 2: {
                const auto code = DisplayStationFlagLabelCode(station);
                text = Utf8ToWide(code);
                break;
            }
            case 3:
                text = station.interval == "RAW"
                    ? L"接收机 ZIP"
                    : FormatAvailableRinexFamilies(station);
                break;
            case 4: text = Utf8ToWide(station.interval); break;
            case 5:
                text = Utf8ToWide(station.siteLogReceiver);
                if (!station.siteLogSystems.empty()) {
                    if (!text.empty()) text += L" / ";
                    text += Utf8ToWide(station.siteLogSystems);
                }
                break;
            case 6:
                text = Utf8ToWide(station.siteLogAntenna);
                if (!station.siteLogRadome.empty()) {
                    if (!text.empty()) text += L" / ";
                    text += Utf8ToWide(station.siteLogRadome);
                }
                break;
            case 7:
                if (station.unverifiedDays > 0) {
                    text = std::format(L"预估 {:.1f}% · {}天待确认", station.completenessPercent, station.unverifiedDays);
                } else if (station.expectedDays > 0) {
                    text = std::format(
                        L"{:.1f}% ({}/{})",
                        station.completenessPercent,
                        station.availableDays,
                        station.expectedDays);
                } else {
                    text = L"—";
                }
                break;
            default: break;
        }
        wcsncpy_s(
            notification.item.pszText,
            static_cast<std::size_t>(notification.item.cchTextMax),
            text.c_str(),
            _TRUNCATE);
    }

    LRESULT HandleListCustomDraw(NMLVCUSTOMDRAW& draw) const {
        if (draw.nmcd.dwDrawStage == CDDS_PREPAINT) {
            return CDRF_NOTIFYITEMDRAW;
        }
        if (draw.nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            const bool selected = (draw.nmcd.uItemState & CDIS_SELECTED) != 0
                && GetFocus() == stationList_;
            draw.clrText = RGB(31, 41, 55);
            // Keep the table calm and close to the original white surface.
            // Only the focused native row gets a very light cue; the
            // checkbox remains the persistent download-selection signal.
            draw.clrTextBk = selected
                ? RGB(239, 246, 255)
                : RGB(255, 255, 255);
            return CDRF_NOTIFYSUBITEMDRAW;
        }
        if (draw.nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)
            && draw.iSubItem == 2) {
            const bool roomForFlag = ListView_GetColumnWidth(
                stationList_, 2) >= 78;
            return countryFlags_ != nullptr && roomForFlag
                ? CDRF_NOTIFYPOSTPAINT : CDRF_DODEFAULT;
        }
        if (draw.nmcd.dwDrawStage == (CDDS_ITEMPOSTPAINT | CDDS_SUBITEM)
            && draw.iSubItem == 2 && countryFlags_ != nullptr) {
            if (ListView_GetColumnWidth(stationList_, 2) < 78) {
                return CDRF_DODEFAULT;
            }
            const auto row = static_cast<int>(draw.nmcd.dwItemSpec);
            if (row >= 0 && static_cast<std::size_t>(row) < visibleStationIndices_.size()) {
                const auto& station = stations_[visibleStationIndices_[row]];
                const int flag = DisplayFlagIndex(station);
                if (flag >= 0) {
                    RECT cell{};
                    if (ListView_GetSubItemRect(stationList_, row, 2, LVIR_LABEL, &cell)) {
                        ImageList_Draw(countryFlags_, flag, draw.nmcd.hdc, cell.left + 34, cell.top, ILD_NORMAL);
                    }
                }
            }
        }
        return CDRF_DODEFAULT;
    }

private:

    void UpdateSelectionSummary() {
        const auto selected = std::count_if(
            stations_.begin(),
            stations_.end(),
            [](const StationFile& station) {
                return station.selected;
            });
        const int visible = ListView_GetItemCount(stationList_);
        const auto candidates = std::count_if(
            stations_.begin(),
            stations_.end(),
            [](const StationFile& station) {
                return station.inSelectionScope;
            });
        SetWindowTextW(
            selectionSummary_,
            std::format(
                L"已选择 {} · 候选范围 {} · 当前列表 {} · 总计 {}",
                selected,
                candidates,
                visible,
                stations_.size()).c_str());
        const bool metadataPending = MetadataFilterPending();
        if (downloading_) {
            SetWindowTextW(
                downloadButton_,
                downloadCancelRequested_ ? L"正在取消…" : L"取消下载");
        } else if (currentDateRangeQuery_) {
            SetWindowTextW(downloadButton_, L"下载已选测站的范围文件");
        } else {
            SetWindowTextW(downloadButton_, L"下载已选观测文件");
        }
        EnableWindow(
            downloadButton_,
            downloading_
                ? !downloadCancelRequested_
                : (!querying_ && !siteLogBlocksInteraction_
                    && !metadataPending && selected > 0));
        EnableWindow(
            pauseDownloadButton_,
            downloading_ && downloadTransferStarted_
                && !downloadCancelRequested_
                && !querying_ && downloadControl_ != nullptr);
        SetWindowTextW(
            pauseDownloadButton_,
            downloading_ && downloadTransferStarted_
                && !downloadCancelRequested_
                && downloadControl_ != nullptr
                && downloadControl_->isPaused()
                ? L"继续下载"
                : L"暂停下载");
        const BOOL canSelect = !querying_
            && !siteLogBlocksInteraction_
            && !metadataPending && candidates > 0;
        const BOOL canScope = !querying_
            && !siteLogBlocksInteraction_
            && !stations_.empty();
        for (const HWND control : {
                 scopeCountry_, scopeLatitudeMinimum_, scopeLatitudeMaximum_,
                 scopeLongitudeMinimum_, scopeLongitudeMaximum_,
                 applyScopeButton_, mapRectangleScopeButton_, clearScopeButton_}) {
            EnableWindow(control, canScope);
        }
        EnableWindow(uniformCountEdit_, canSelect);
        EnableWindow(minimumDistanceEdit_, canSelect);
        EnableWindow(maximumNeighborDistanceEdit_, canSelect);
        EnableWindow(uniformSelectButton_, canSelect);
        PostWebUiState();
    }

    void SetBusyControls() {
        // Each pane owns its controls and worker.  A busy precise pane must
        // not disable observation controls, and either transfer may run while
        // the user switches to the other pane.
        const BOOL enableInteractive = !querying_;
        const BOOL enablePaneSwitch = !querying_ && !siteLogLoading_;
        EnableWindow(observationModeButton_, enablePaneSwitch);
        EnableWindow(preciseModeButton_, enablePaneSwitch);
        // Query inputs remain locked while either Site Log pass is active so
        // a new query cannot replace its owning jthread.  Ordinary
        // representative enrichment does not replace station indices, so
        // map/list/scope/select/download remain interactive below.  Only a
        // temporal metadata pass keeps those controls locked.
        const BOOL enableQueryInputs = enableInteractive && !siteLogLoading_;
        EnableWindow(
            selectKnowledgeBaseNetworksButton_,
            enableQueryInputs
                && (!knowledgeBaseStatusLoading_
                    || knowledgeBaseStatusTime_.time_since_epoch().count() != 0)
                && !knowledgeBaseTogglePending_);
        EnableWindow(networkPanelToggleButton_, enableQueryInputs);
        EnableWindow(igsCheck_, enableQueryInputs);
        EnableWindow(mgexCheck_, enableQueryInputs);
        EnableWindow(ngsCheck_, enableQueryInputs);
        EnableWindow(epnCheck_, enableQueryInputs);
        EnableWindow(hkCheck_, enableQueryInputs);
        EnableWindow(gaGdcCheck_, enableQueryInputs);
        EnableWindow(rgnaCheck_, enableQueryInputs);
        EnableWindow(sonelCheck_, enableQueryInputs);
        EnableWindow(trigNetCheck_, enableQueryInputs);
        EnableWindow(cutCheck_, enableQueryInputs);
        EnableWindow(csnCheck_, enableQueryInputs);
        EnableWindow(eposIcelandCheck_, enableQueryInputs);
        EnableWindow(rbmcCheck_, enableQueryInputs);
        EnableWindow(ramsacCheck_, enableQueryInputs);
        EnableWindow(regnaRouCheck_, enableQueryInputs);
        EnableWindow(cacsCheck_, enableQueryInputs);
        EnableWindow(rgpCheck_, enableQueryInputs);
        EnableWindow(sopacGarnerCheck_, enableQueryInputs);
        EnableWindow(magnaEcoCheck_, enableQueryInputs);
        EnableWindow(panamaCorsCheck_, enableQueryInputs);
        EnableWindow(hondurasCorsCheck_, enableQueryInputs);
        EnableWindow(eposGnssCheck_, enableQueryInputs);
        EnableWindow(grefCheck_, enableQueryInputs);
        EnableWindow(osNetCheck_, enableQueryInputs);
        EnableWindow(ergnssCheck_, enableQueryInputs);
        EnableWindow(renepCheck_, enableQueryInputs);
        EnableWindow(geoNetNzCheck_, enableQueryInputs);
        EnableWindow(ringCheck_, enableQueryInputs);
        EnableWindow(aposCheck_, enableQueryInputs);
        EnableWindow(robBelgiumCheck_, enableQueryInputs);
        EnableWindow(dpgaCheck_, enableQueryInputs);
        EnableWindow(catNetCheck_, enableQueryInputs);
        EnableWindow(spsLuxCheck_, enableQueryInputs);
        EnableWindow(kasiNgiiCheck_, enableQueryInputs);
        EnableWindow(renagCheck_, enableQueryInputs);
        EnableWindow(nsgiCheck_, enableQueryInputs);
        EnableWindow(noaGnssCheck_, enableQueryInputs);
        EnableWindow(redgaeCheck_, enableQueryInputs);
        EnableWindow(geoNetJapanCheck_, enableQueryInputs);
        EnableWindow(dlabCheck_, enableQueryInputs);
        EnableWindow(magnetCheck_, enableQueryInputs);
        EnableWindow(pangaCwuCheck_, enableQueryInputs);
        EnableWindow(datePicker_, enableQueryInputs);
        EnableWindow(endDatePicker_, enableQueryInputs);
        EnableWindow(dateRangeToLabel_, enableQueryInputs);
        EnableWindow(quickScheme_, enableQueryInputs);
        EnableWindow(queryButton_, enableQueryInputs);
        EnableWindow(advancedFilterButton_, enableQueryInputs);
        EnableWindow(dateAdvancedFilterButton_, enableQueryInputs);
        EnableWindow(
            filterEdit_, enableInteractive && !siteLogBlocksInteraction_);
        const BOOL enableScope = enableInteractive
            && !siteLogBlocksInteraction_
            && !stations_.empty();
        EnableWindow(scopeCountry_, enableScope);
        EnableWindow(scopeLatitudeMinimum_, enableScope);
        EnableWindow(scopeLatitudeMaximum_, enableScope);
        EnableWindow(scopeLongitudeMinimum_, enableScope);
        EnableWindow(scopeLongitudeMaximum_, enableScope);
        EnableWindow(applyScopeButton_, enableScope);
        EnableWindow(mapRectangleScopeButton_, enableScope);
        EnableWindow(clearScopeButton_, enableScope);
        EnableWindow(
            stationList_, enableInteractive && !siteLogBlocksInteraction_);
        EnableWindow(
            map_, enableInteractive && !siteLogBlocksInteraction_);
        const BOOL metadataPending = MetadataFilterPending();
        EnableWindow(
            selectAllButton_,
            enableInteractive && !siteLogBlocksInteraction_
                && !metadataPending);
        EnableWindow(
            clearSelectionButton_,
            enableInteractive && !siteLogBlocksInteraction_);
        EnableWindow(
            uniformCountEdit_,
            enableInteractive && !siteLogBlocksInteraction_
                && !stations_.empty());
        EnableWindow(
            minimumDistanceEdit_,
            enableInteractive && !siteLogBlocksInteraction_
                && !stations_.empty());
        EnableWindow(
            maximumNeighborDistanceEdit_,
            enableInteractive && !siteLogBlocksInteraction_
                && !stations_.empty());
        EnableWindow(
            uniformSelectButton_,
            enableInteractive && !siteLogBlocksInteraction_ && !metadataPending
                && !stations_.empty());
        EnableWindow(
            randomSelectButton_,
            enableInteractive && !siteLogBlocksInteraction_ && !metadataPending
                && !stations_.empty());
        EnableWindow(folderEdit_, enableInteractive);
        EnableWindow(browseButton_, enableInteractive);
        UpdateSelectionSummary();
        EnableWindow(
            pauseDownloadButton_,
            downloading_ && downloadTransferStarted_
                && !downloadCancelRequested_
                && downloadControl_ != nullptr);
        SetWindowTextW(
            pauseDownloadButton_,
            downloading_ && downloadTransferStarted_
                && !downloadCancelRequested_
                && downloadControl_ != nullptr
                && downloadControl_->isPaused()
                ? L"继续下载"
                : L"暂停下载");
    }

    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    int initialShowCommand_ = SW_SHOWNORMAL;
    bool windowRevealed_ = false;

    HWND title_ = nullptr;
    HWND subtitle_ = nullptr;
    HWND observationModeButton_ = nullptr;
    HWND preciseModeButton_ = nullptr;
    HWND networkLabel_ = nullptr;
    HWND networkPanelToggleButton_ = nullptr;
    HWND selectKnowledgeBaseNetworksButton_ = nullptr;
    HWND igsCheck_ = nullptr;
    HWND mgexCheck_ = nullptr;
    HWND ngsCheck_ = nullptr;
    HWND epnCheck_ = nullptr;
    HWND hkCheck_ = nullptr;
    HWND gaGdcCheck_ = nullptr;
    HWND rgnaCheck_ = nullptr;
    HWND sonelCheck_ = nullptr;
    HWND trigNetCheck_ = nullptr;
    HWND cutCheck_ = nullptr;
    HWND csnCheck_ = nullptr;
    HWND eposIcelandCheck_ = nullptr;
    HWND rbmcCheck_ = nullptr;
    HWND ramsacCheck_ = nullptr;
    HWND regnaRouCheck_ = nullptr;
    HWND cacsCheck_ = nullptr;
    HWND rgpCheck_ = nullptr;
    HWND sopacGarnerCheck_ = nullptr;
    HWND magnaEcoCheck_ = nullptr;
    HWND panamaCorsCheck_ = nullptr;
    HWND hondurasCorsCheck_ = nullptr;
    HWND eposGnssCheck_ = nullptr;
    HWND grefCheck_ = nullptr;
    HWND osNetCheck_ = nullptr;
    HWND ergnssCheck_ = nullptr;
    HWND renepCheck_ = nullptr;
    HWND geoNetNzCheck_ = nullptr;
    HWND ringCheck_ = nullptr;
    HWND aposCheck_ = nullptr;
    HWND robBelgiumCheck_ = nullptr;
    HWND dpgaCheck_ = nullptr;
    HWND catNetCheck_ = nullptr;
    HWND spsLuxCheck_ = nullptr;
    HWND kasiNgiiCheck_ = nullptr;
    HWND renagCheck_ = nullptr;
    HWND nsgiCheck_ = nullptr;
    HWND noaGnssCheck_ = nullptr;
    HWND redgaeCheck_ = nullptr;
    HWND geoNetJapanCheck_ = nullptr;
    HWND dlabCheck_ = nullptr;
    HWND magnetCheck_ = nullptr;
    HWND pangaCwuCheck_ = nullptr;
    HWND dateLabel_ = nullptr;
    HWND datePicker_ = nullptr;
    HWND dateRangeToLabel_ = nullptr;
    HWND endDatePicker_ = nullptr;
    HWND quickSchemeLabel_ = nullptr;
    HWND quickScheme_ = nullptr;
    HWND queryButton_ = nullptr;
    HWND advancedFilterButton_ = nullptr;
    HWND dateAdvancedFilterButton_ = nullptr;
    HWND queryStatus_ = nullptr;
    HWND filterLabel_ = nullptr;
    HWND filterEdit_ = nullptr;
    HWND scopeCountryLabel_ = nullptr;
    HWND scopeCountry_ = nullptr;
    HWND scopeLatitudeLabel_ = nullptr;
    HWND scopeLatitudeMinimum_ = nullptr;
    HWND scopeLatitudeToLabel_ = nullptr;
    HWND scopeLatitudeMaximum_ = nullptr;
    HWND scopeLongitudeLabel_ = nullptr;
    HWND scopeLongitudeMinimum_ = nullptr;
    HWND scopeLongitudeToLabel_ = nullptr;
    HWND scopeLongitudeMaximum_ = nullptr;
    HWND applyScopeButton_ = nullptr;
    HWND mapRectangleScopeButton_ = nullptr;
    HWND clearScopeButton_ = nullptr;
    HWND scopeStatus_ = nullptr;
    HWND stationList_ = nullptr;
    HIMAGELIST countryFlags_ = nullptr;
    HWND selectionSummary_ = nullptr;
    HWND selectAllButton_ = nullptr;
    HWND clearSelectionButton_ = nullptr;
    HWND uniformCountLabel_ = nullptr;
    HWND uniformCountEdit_ = nullptr;
    HWND minimumDistanceLabel_ = nullptr;
    HWND minimumDistanceEdit_ = nullptr;
    HWND maximumNeighborDistanceLabel_ = nullptr;
    HWND maximumNeighborDistanceEdit_ = nullptr;
    HWND uniformSelectButton_ = nullptr;
    HWND randomSelectButton_ = nullptr;
    HWND folderLabel_ = nullptr;
    HWND folderEdit_ = nullptr;
    HWND browseButton_ = nullptr;
    HWND downloadButton_ = nullptr;
    HWND pauseDownloadButton_ = nullptr;
    HWND progress_ = nullptr;
    HWND operationStatus_ = nullptr;
    HWND map_ = nullptr;
    HWND preciseProductView_ = nullptr;
    std::vector<HWND> nativeControls_;
    std::unique_ptr<WebWorkbenchHost> webUi_;
    std::size_t webRowOffset_ = 0;
    bool webSortAscending_ = false;
    bool webSortByCompleteness_ = true;
    bool webRowsSelectedOnly_ = false;
    std::set<std::string> webSelectedCountryCodes_;
    std::uint64_t webStationDataRevision_ = 1;
    mutable std::uint64_t webLastMapDataRevisionSent_ = 0;
    mutable std::uint64_t webLastMapSelectionSignatureSent_ = 0;
    mutable std::uint64_t webSortedStationRevision_ = 0;
    mutable bool webSortedAscending_ = true;
    mutable bool webSortedCompleteness_ = false;
    mutable std::vector<std::size_t> webSortedVisibleStationIndices_;
    bool webUiStarting_ = false;
    bool webUiFirstFrameReady_ = false;
    std::uint32_t loadingAnimationTick_ = 0;

    HFONT titleFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HFONT sectionFont_ = nullptr;

    std::vector<StationFile> stations_;
    std::vector<std::size_t> visibleStationIndices_;
    std::vector<bool> geographicScope_;
    // State rendered by the Web workbench.  These values, rather than the
    // retired Win32 controls, are the source of truth for user input and
    // status output.  The HWNDs are mirrored only while the remaining native
    // business adapters are migrated.
    SYSTEMTIME queryStartDate_{};
    SYSTEMTIME queryEndDate_{};
    std::wstring filterText_;
    std::wstring downloadFolder_;
    std::wstring operationStatusText_;
    std::wstring querySummaryText_;
    QueryDate currentDate_{};
    NetworkSelection currentSelection_{};
    QueryDate currentPeriodStart_{};
    QueryDate currentPeriodEnd_{};
    std::vector<DatedStationFile> currentPeriodFiles_;
    bool currentDateRangeQuery_ = false;
    bool currentPeriodFilesDeferred_ = false;
    ObservationRangeOptions currentRangeOptions_{};
    AdvancedFilterState advancedFilter_{};
    DateAdvancedFilterState dateAdvancedFilter_{};
    struct WebDetailWorker {
        std::jthread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    std::vector<WebDetailWorker> webDetailWorkers_;
    std::uint64_t stationQueryRevision_ = 0;
    std::uint64_t stationSelectionRequest_ = 0;
    std::uint64_t stationSelectionSignature_ = 0;
    bool stationSelectionBusy_ = false;
    bool stationSelectionApplied_ = false;
    bool currentSelectionDatesKnown_ = false;
    std::vector<int> currentSelectionEligibleDates_;
    std::wstring selectionDailyMode_ = L"ignore";
    int selectionDailyStations_ = 35;
    double selectionDailyPercent_ = 70;
    std::wstring selectionRandomSeed_ = L"20260929";
    StationSelectionPlan lastStationSelection_;
    std::vector<std::wstring> stationSelectionIds_;
    std::jthread stationSelectionThread_;
    std::jthread queryThread_;
    std::jthread siteLogThread_;
    std::jthread downloadThread_;
    DownloadPlanWorkers planWorkers_;
    std::jthread postprocessThread_;
    bool postprocessBusy_ = false;
    std::size_t postprocessSucceeded_ = 0;
    std::size_t postprocessFailed_ = 0;
    std::vector<std::wstring> postprocessErrors_;
    DownloadSettings::Options batchDownloadOptions_{};
    std::vector<std::filesystem::path> batchPostprocessFiles_;
    bool batchPostprocessCancelled_ = false;
    std::jthread knowledgeBaseStatusThread_;
    std::jthread observationSearchPreloadThread_;
    DownloadControlPtr downloadControl_;
    std::vector<ObservationQueuedDownload> observationQueue_;
    std::uint64_t nextObservationQueueId_ = 0;
    std::uint64_t activeObservationQueueId_ = 0;
    bool observationQueueRunning_ = false;
    bool querying_ = false;
    bool siteLogLoading_ = false;
    bool siteLogBlocksInteraction_ = false;
    bool siteLogEnrichmentComplete_ = true;
    bool downloading_ = false;
    bool downloadCancelRequested_ = false;
    bool downloadTransferStarted_ = false;
    bool syncingList_ = false;
    bool mapRectangleSelecting_ = false;
    bool preciseMode_ = false;
    bool networksExpanded_ = false;
    ObservationNetworkMask knowledgeBaseNetworks_ = 0;
    // Authoritative source selection for the Web workbench.  Native checkbox
    // handles are now only a temporary compatibility adapter and no longer
    // own application state.
    ObservationNetworkMask selectedNetworkMask_ =
        NetworkBit(ObservationNetwork::Igs);
    std::chrono::steady_clock::time_point knowledgeBaseStatusTime_{};
    std::uint64_t knowledgeBaseStatusGeneration_ = 0;
    bool knowledgeBaseStatusLoading_ = false;
    bool knowledgeBaseTogglePending_ = false;
    bool initialNetworkSelectionApplied_ = false;
    bool networkSelectionTouched_ = false;
    // 用户在「全选/取消全选」的异步扫描期间手动改过站网选择。
    // 扫描完成时必须以用户操作为准、丢弃这次全选/清空；否则会把刚点的单个
    // 站网切换无声覆盖掉（表现为「点 IGS 取消，结果却变成只剩 IGS」）。
    bool selectionChangedWhileTogglePending_ = false;
    // 「全选/取消全选」在点击时立即应用，这个标记记住当时的意图（true = 清空），
    // 供异步可用性扫描回来后按同一意图重放。
    bool knowledgeBaseToggleClearIntent_ = false;
    ObservationPresetSelection observationPreset_ = ObservationPresetSelection::None;
    ReferenceFrameSelection quickFrameSelection_ = ReferenceFrameSelection::None;
    std::uint64_t siteLogGeneration_ = 0;
};

LRESULT CALLBACK MainWindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    auto* state = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        const auto instance = static_cast<HINSTANCE>(create->hInstance);
        const auto* requestedShowCommand = static_cast<const int*>(
            create->lpCreateParams);
        auto object = std::make_unique<MainWindow>(
            window,
            instance,
            requestedShowCommand == nullptr
                ? SW_SHOWNORMAL : *requestedShowCommand);
        state = object.release();
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    case WM_CREATE:
        if (state == nullptr || !state->CreateControls()) {
            return -1;
        }
        state->Layout();
        return 0;
    case WM_SIZE:
        if (state != nullptr && wParam != SIZE_MINIMIZED) {
            state->Layout();
        }
        return 0;
    case WM_ERASEBKGND: {
        // The startup card is painted atomically in WM_PAINT. Erasing its
        // surface first exposes a brief grey frame on each animation tick.
        if (state != nullptr && state->IsStartupLoading()) return 1;
        const HDC deviceContext = reinterpret_cast<HDC>(wParam);
        RECT client{};
        GetClientRect(window, &client);
        FillSolidRect(deviceContext, client, kWindowBackground);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC deviceContext = BeginPaint(window, &paint);
        if (state != nullptr && state->IsStartupLoading()) {
            const int width = paint.rcPaint.right - paint.rcPaint.left;
            const int height = paint.rcPaint.bottom - paint.rcPaint.top;
            const HDC buffer = CreateCompatibleDC(deviceContext);
            const HBITMAP bitmap = buffer != nullptr && width > 0 && height > 0
                ? CreateCompatibleBitmap(deviceContext, width, height) : nullptr;
            if (buffer != nullptr && bitmap != nullptr) {
                const HGDIOBJ previous = SelectObject(buffer, bitmap);
                SetViewportOrgEx(buffer, -paint.rcPaint.left,
                    -paint.rcPaint.top, nullptr);
                state->PaintBackground(buffer);
                BitBlt(deviceContext, paint.rcPaint.left, paint.rcPaint.top,
                    width, height, buffer, 0, 0, SRCCOPY);
                SelectObject(buffer, previous);
            } else {
                state->PaintBackground(deviceContext);
            }
            if (bitmap != nullptr) DeleteObject(bitmap);
            if (buffer != nullptr) DeleteDC(buffer);
        } else if (state != nullptr) {
            state->PaintBackground(deviceContext);
        } else {
            RECT client{};
            GetClientRect(window, &client);
            FillSolidRect(deviceContext, client, kWindowBackground);
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ACTIVATE:
        if (state != nullptr && LOWORD(wParam) != WA_INACTIVE) {
            state->RefreshKnowledgeBaseStatus();
        }
        return 0;
    case WM_PRECISE_BUSY_CHANGED:
        if (state != nullptr) {
            state->HandlePreciseBusyChanged();
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* information = reinterpret_cast<MINMAXINFO*>(lParam);
        MONITORINFO monitor{sizeof(monitor)};
        const HMONITOR monitorHandle = MonitorFromWindow(
            window, MONITOR_DEFAULTTONEAREST);
        const bool haveWorkArea = monitorHandle != nullptr
            && GetMonitorInfoW(monitorHandle, &monitor) != FALSE;
        const int workWidth = haveWorkArea
            ? monitor.rcWork.right - monitor.rcWork.left : 0;
        const int workHeight = haveWorkArea
            ? monitor.rcWork.bottom - monitor.rcWork.top : 0;
        const int widthBudget = workWidth > 0
            ? std::max(1, workWidth - 32) : 1180;
        const int heightBudget = workHeight > 0
            ? std::max(1, workHeight - 24) : 900;
        information->ptMinTrackSize.x = std::min(960, widthBudget);
        // The network chooser is now a floating card, so the main window no
        // longer needs a 1000 px minimum just to make room for its rows.  A
        // compact workspace is used below 800 px, leaving the download bar
        // usable even when the taskbar leaves a short work area.
        information->ptMinTrackSize.y = std::min(680, heightBudget);
        return 0;
    }
    case WM_COMMAND:
        if (state == nullptr) {
            break;
        }
        // Commands from observation controls can remain queued for one turn
        // while a page switch is being processed.  Once the precision page is
        // active, discard those stale commands instead of starting a hidden
        // observation query/download behind it.
        if (state->IsPreciseMode()
            && LOWORD(wParam) != IdObservationMode
            && LOWORD(wParam) != IdPreciseMode) {
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED
            && LOWORD(wParam) >= IdIgsCheck
            && LOWORD(wParam) <= IdPangaCwuCheck) {
            // A manual checkbox click must win over the asynchronous
            // first-launch default selection.
            state->MarkNetworkSelectionTouched();
            state->UpdateKnowledgeBaseToggleLabel();
        }
        switch (LOWORD(wParam)) {
        case IdObservationMode:
            if (HIWORD(wParam) == BN_CLICKED) {
                state->SetPreciseMode(false);
            }
            return 0;
        case IdPreciseMode:
            if (HIWORD(wParam) == BN_CLICKED) {
                state->SetPreciseMode(true);
            }
            return 0;
        case IdQueryButton:
            if (HIWORD(wParam) == BN_CLICKED) {
                state->StartQuery();
            }
            return 0;
        case IdQuickScheme:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                state->ApplyQuickScheme();
            }
            return 0;
        case IdAdvancedFilterButton:
            if (HIWORD(wParam) == BN_CLICKED) {
                state->ShowAdvancedFilterDialog();
            }
            return 0;
        case IdDateAdvancedFilterButton:
            if (HIWORD(wParam) == BN_CLICKED) {
                state->ShowDateAdvancedFilterDialog();
            }
            return 0;
        case IdNetworkPanelToggle:
            if (HIWORD(wParam) == BN_CLICKED) {
                state->ToggleNetworkPanel();
            }
            return 0;
        case IdSelectKnowledgeBaseNetworks:
            if (HIWORD(wParam) == BN_CLICKED) {
                state->SelectAllKnowledgeBaseNetworks();
            }
            return 0;
        case IdFilterEdit:
            if (HIWORD(wParam) == EN_CHANGE) {
                // Filtering a large virtual station list on every keystroke
                // makes the edit box feel sticky.  Wait for a short pause so
                // a pasted station code or a normal word is processed once.
                SetTimer(window, kObservationFilterTimer, 180, nullptr);
            }
            return 0;
        case IdApplyScope:
            if (HIWORD(wParam) == BN_CLICKED) {
                state->ApplyScopeFromControls();
            }
            return 0;
        case IdMapRectangleScope:
            if (HIWORD(wParam) == BN_CLICKED) {
                state->ToggleMapRectangleScope();
            }
            return 0;
        case IdClearScope:
            if (HIWORD(wParam) == BN_CLICKED) {
                state->ClearScope();
            }
            return 0;
        case IdSelectAll:
            if (HIWORD(wParam) == BN_CLICKED) {
                state->SelectVisible(true);
            }
            return 0;
        case IdClearSelection:
            if (HIWORD(wParam) == BN_CLICKED) {
                state->ClearAllSelection();
            }
            return 0;
        case IdUniformSelect:
            if (HIWORD(wParam) == BN_CLICKED) {
                state->SelectGloballyDistributed();
            }
            return 0;
        case IdRandomSelect:
            if (HIWORD(wParam) == BN_CLICKED) {
                state->SelectGloballyDistributed(true);
            }
            return 0;
        case IdFolderEdit:
            if (HIWORD(wParam) == EN_CHANGE) {
                state->HandleDownloadFolderEdit(false);
            } else if (HIWORD(wParam) == EN_KILLFOCUS) {
                state->HandleDownloadFolderEdit(true);
            }
            return 0;
        case IdBrowseButton:
            if (HIWORD(wParam) == BN_CLICKED) {
                state->BrowseFolder();
            }
            return 0;
        case IdDownloadButton:
            if (HIWORD(wParam) == BN_CLICKED) {
                state->StartDownload();
            }
            return 0;
        case IdPauseDownloadButton:
            if (HIWORD(wParam) == BN_CLICKED) {
                state->ToggleDownloadPause();
            }
            return 0;
        default:
            break;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
        if (state != nullptr) {
            const auto brush = state->HandleNetworkControlColor(
                reinterpret_cast<HDC>(wParam),
                reinterpret_cast<HWND>(lParam));
            if (brush != nullptr) {
                return reinterpret_cast<LRESULT>(brush);
            }
        }
        break;
    case WM_TIMER:
        if (state != nullptr && wParam == kWebUiLoadingTimer) {
            state->AdvanceWebLoadingAnimation();
            return 0;
        }
        if (state != nullptr && wParam == kObservationFilterTimer) {
            KillTimer(window, kObservationFilterTimer);
            if (state->IsPreciseMode()
                || state->IsObservationOperationBusy()
                || !IsWindowVisible(state->StationListHandle())) {
                return 0;
            }
            // A timer message can already be queued when the user switches
            // pages or opens the source chooser.  Avoid rebuilding a hidden
            // virtual list in either case; the next visible edit will schedule
            // a fresh refresh.
            state->ApplyFilter();
            return 0;
        }
        break;
    case WM_IGS_SHOW_WEB_LOADING:
        if (state != nullptr) {
            state->BeginWebLoadingPresentation();
        }
        return 0;
    case WM_MEASUREITEM:
        if (state != nullptr && lParam != 0) {
            auto& measure = *reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (measure.CtlID == IdScopeCountry) {
                state->HandleCountryComboMeasureItem(measure);
                return TRUE;
            }
        }
        break;
    case WM_DRAWITEM:
        if (state != nullptr && lParam != 0) {
            const auto& draw = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (draw.CtlType == ODT_BUTTON) {
                DrawModernButton(draw);
                return TRUE;
            }
            if (draw.CtlID == IdScopeCountry) {
                state->HandleCountryComboDrawItem(draw);
                return TRUE;
            }
        }
        break;
    case WM_NOTIFY:
        if (state != nullptr) {
            const auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->hwndFrom != nullptr
                && state->StationListHandle() != nullptr
                && header->hwndFrom
                    == ListView_GetHeader(state->StationListHandle())
                && (header->code == HDN_ITEMCHANGINGW
                    || header->code == HDN_ITEMCHANGEDW)) {
                return state->OnStationListHeaderNotify(
                    *reinterpret_cast<NMHEADERW*>(lParam));
            }
            if (header->idFrom == IdStationList) {
                if (header->code == LVN_GETDISPINFOW) {
                    state->HandleListGetDisplayInfo(
                        *reinterpret_cast<NMLVDISPINFOW*>(lParam));
                    return 0;
                }
                if (header->code == LVN_ITEMCHANGED) {
                    state->HandleListItemChanged(
                        *reinterpret_cast<NMLISTVIEW*>(lParam));
                    return 0;
                }
                if (header->code == NM_CUSTOMDRAW) {
                    return state->HandleListCustomDraw(
                        *reinterpret_cast<NMLVCUSTOMDRAW*>(lParam));
                }
            }
        }
        break;
    case WM_IGS_MAP_TOGGLE_STATION:
        if (state != nullptr) {
            state->ToggleStationFromMap(static_cast<std::size_t>(wParam));
        }
        return 0;
    case WM_IGS_MAP_AREA_SELECTED:
        if (state != nullptr && lParam != 0) {
            state->HandleMapAreaSelected(
                *reinterpret_cast<const MapSelectionBounds*>(lParam));
        }
        return 0;
    case WM_IGS_WEB_DETAIL_READY: {
        auto detail = std::unique_ptr<std::wstring>(reinterpret_cast<std::wstring*>(lParam));
        if (state) state->HandleWebDetailReady(std::move(detail));
        return 0;
    }
    case WM_IGS_SELECTION_READY: {
        auto result = std::unique_ptr<StationSelectionWorkerResult>(reinterpret_cast<StationSelectionWorkerResult*>(lParam));
        if (state) state->HandleStationSelectionReady(std::move(result));
        return 0;
    }
    case WM_IGS_QUERY_FINISHED:
        {
            auto result = std::unique_ptr<QueryResult>(
                reinterpret_cast<QueryResult*>(lParam));
            if (state != nullptr) {
                state->HandleQueryFinished(std::move(result));
            }
            return 0;
        }
    case WM_IGS_SITELOG_FINISHED:
        {
            auto result = std::unique_ptr<SiteLogEnrichmentResult>(
                reinterpret_cast<SiteLogEnrichmentResult*>(lParam));
            if (state != nullptr) {
                state->HandleSiteLogFinished(std::move(result));
            }
            return 0;
        }
    case WM_IGS_KB_STATUS_FINISHED:
        if (lParam == 0) {
            return 0;
        }
        if (state != nullptr) {
            state->ApplyKnowledgeBaseStatus(
                *reinterpret_cast<const KnowledgeBaseStatusResult*>(lParam));
        }
        delete reinterpret_cast<KnowledgeBaseStatusResult*>(lParam);
        return 0;
    case WM_IGS_DOWNLOAD_PROGRESS:
        {
            auto progress = std::unique_ptr<DownloadProgress>(
                reinterpret_cast<DownloadProgress*>(lParam));
            if (state != nullptr) {
                state->HandleDownloadProgress(std::move(progress), static_cast<std::uint64_t>(wParam));
            }
            return 0;
        }
    case WM_IGS_DOWNLOAD_FINISHED:
        {
            auto finished = std::unique_ptr<DownloadFinished>(
                reinterpret_cast<DownloadFinished*>(lParam));
            if (state != nullptr) {
                state->HandleDownloadFinished(std::move(finished), static_cast<std::uint64_t>(wParam));
            }
            return 0;
        }
    case WM_IGS_POSTPROCESS_READY: {
        auto estimate = std::unique_ptr<ObservationPostprocess::Estimate>(reinterpret_cast<ObservationPostprocess::Estimate*>(lParam));
        if (state) state->HandlePostprocessReady(std::move(estimate));
        return 0;
    }
    case WM_IGS_POSTPROCESS_FINISHED: {
        auto result = std::unique_ptr<ObservationPostprocess::Result>(reinterpret_cast<ObservationPostprocess::Result*>(lParam));
        if (state) state->HandlePostprocessFinished(std::move(result));
        return 0;
    }
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(
            window,
            nullptr,
            suggested->left,
            suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
        if (state != nullptr) {
            state->PrepareForClose();
        }
        DestroyWindow(window);
        return 0;
    case WM_NCDESTROY:
        delete state;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

bool RegisterMainWindowClass(const HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = MainWindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_MARK_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    windowClass.lpszClassName = kMainWindowClassName;
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_MARK_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    return RegisterClassExW(&windowClass) != 0
        || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND CreateMainWindow(const HINSTANCE instance, const int showCommand) {
    RECT workArea{};
    const bool haveWorkArea = SystemParametersInfoW(
        SPI_GETWORKAREA, 0, &workArea, 0) != FALSE;
    const int workWidth = haveWorkArea
        ? workArea.right - workArea.left : 1480;
    const int workHeight = haveWorkArea
        ? workArea.bottom - workArea.top : 1000;
    const int width = std::min(1480, std::max(1, workWidth - 32));
    const int height = std::min(1000, std::max(1, workHeight - 24));
    const int left = haveWorkArea
        ? workArea.left + std::max(0, (workWidth - width) / 2)
        : CW_USEDEFAULT;
    const int top = haveWorkArea
        ? workArea.top + std::max(0, (workHeight - height) / 2)
        : CW_USEDEFAULT;
    int requestedShowCommand = showCommand;
    const HWND window = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kMainWindowClassName,
        kApplicationTitle,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        left,
        top,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        &requestedShowCommand);
    if (window != nullptr) {
        SetWindowTextW(window, kApplicationTitle);
    }
    return window;
}
