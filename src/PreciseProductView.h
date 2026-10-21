#pragma once

#include <windows.h>
#include "PreciseProducts.h"

#include <cstdint>
#include <string>
#include <vector>

// Parent notification emitted when the child product-transfer state changes.
// It is kept separate from the progress/finished messages used internally by
// the child view so the parent can keep the pane-switch buttons in sync.
inline constexpr UINT WM_PRECISE_BUSY_CHANGED = WM_APP + 43;
// Keep these outside the messages used by the map view and the precise
// product child itself (WM_APP + 40..45).
inline constexpr UINT WM_PRECISE_TRIGGER_SEARCH = WM_APP + 50;
inline constexpr UINT WM_PRECISE_TRIGGER_DOWNLOAD = WM_APP + 51;
inline constexpr UINT WM_PRECISE_TRIGGER_PAUSE = WM_APP + 52;
inline constexpr UINT WM_PRECISE_TRIGGER_CANCEL = WM_APP + 53;
inline constexpr UINT WM_PRECISE_SELECT_PRODUCT = WM_APP + 54;
inline constexpr UINT WM_PRECISE_SET_DATES = WM_APP + 55;
inline constexpr UINT WM_PRECISE_SET_CENTER = WM_APP + 56;
inline constexpr UINT WM_PRECISE_QUERY_SNAPSHOT = WM_APP + 57;
inline constexpr UINT WM_PRECISE_SET_FAMILY = WM_APP + 58;
inline constexpr UINT WM_PRECISE_SET_FILTERS = WM_APP + 59;
inline constexpr UINT WM_PRECISE_SELECT_VISIBLE = WM_APP + 60;
inline constexpr UINT WM_PRECISE_BROWSE_FOLDER = WM_APP + 61;
inline constexpr UINT WM_PRECISE_SET_QUICK_PRESET = WM_APP + 62;
inline constexpr UINT WM_PRECISE_APPLY_QUICK_PRESET = WM_APP + 63;
inline constexpr UINT WM_PRECISE_SET_CUSTOM_PRESET = WM_APP + 65;
inline constexpr UINT WM_PRECISE_ADD_DOWNLOAD_PLAN = WM_APP + 66;
inline constexpr UINT WM_PRECISE_START_DOWNLOAD_PLAN = WM_APP + 67;
inline constexpr UINT WM_PRECISE_UPDATE_DOWNLOAD_PLAN_ITEM = WM_APP + 68;
inline constexpr UINT WM_PRECISE_QUERY_COVERAGE = WM_APP + 69;
struct PreciseUiCoverageSnapshot {
    std::wstring id;
    std::wstring json = L"null";
    std::wstring family;
    std::wstring antexJson = L"null";
    std::wstring updDate;
    std::wstring updFile;
    std::vector<std::wstring> updInputFiles;
    std::vector<std::wstring> files;
    std::vector<PreciseUpdFileRecord> updRecords;
    std::wstring error;
};
bool QueryPreciseProductCoverage(HWND view, PreciseUiCoverageSnapshot& snapshot) noexcept;

struct PreciseUiQueueItemSnapshot {
    std::uint64_t id = 0;
    std::wstring label;
    std::wstring range;
    std::wstring status;
    int percent = 0;
    bool selected = false;
    int fileCount = 0;
};

struct PreciseUiProductSnapshot {
    std::wstring id;
    std::wstring name;
    std::wstring family;
    std::wstring center;
    std::wstring latency;
    std::wstring solutionClass;
    std::wstring constellation;
    std::wstring interval;
    std::wstring formatVersion;
    std::wstring preferredFileName;
    std::wstring sources;
    std::wstring availability;
    std::wstring status;
    int completeness = 0;
    int availableDays = 0;
    int expectedDays = 0;
    int recordCount = 0;
    int inventoryDays = 0;
    int flaggedRecords = 0;
    int flaggedDays = 0;
    bool selected = false;
    bool localInventoryOnly = false;
    bool missingRequired = false;
};

struct PreciseUiRuleOption {
    std::wstring family;
    std::wstring latency;
    std::wstring center;
    std::wstring interval;
};

struct PreciseUiSnapshot {
    std::uint64_t catalogRevision = 0;
    bool busy = false;
    bool transferring = false;
    bool paused = false;
    int progressPercent = 0;
    int selected = 0;
    int familyIndex = 0;
    int latencyIndex = 0;
    int centerIndex = 0;
    int completenessIndex = 0;
    std::wstring completenessMode = L"ignore";
    double completenessValue = 0.0;
    int quickPresetIndex = 0;
    bool quickBundleReady = false;
    std::wstring folder;
    std::wstring status;
    std::wstring startDate;
    std::wstring endDate;
    std::wstring troStation;
    std::wstring search;
    bool ruleOptionsReady = false;
    std::vector<PreciseUiRuleOption> ruleOptions;
    std::vector<PreciseUiProductSnapshot> products;
    std::vector<PreciseUiQueueItemSnapshot> queue;
    bool queueRunning = false;
    std::uint64_t activeQueueId = 0;
};

bool RegisterPreciseProductViewClass(HINSTANCE instance);
HWND CreatePreciseProductView(
    HWND parent,
    HINSTANCE instance,
    int controlId);

// Lightweight, synchronous query of the child pane's busy state.  It never
// touches the database or network and is safe when the child is null/destroyed.
[[nodiscard]] bool PreciseProductViewIsBusy(HWND view) noexcept;

// Put keyboard focus on the first actionable control when the parent switches
// to the precision page.  The page is a child window rather than a dialog, so
// exposing this tiny focus bridge keeps Tab/Enter behavior predictable.
void FocusPreciseProductView(HWND view) noexcept;

// Message bridges used by the WebView2 visual shell.  They keep the product
// page's command IDs private while allowing the HTML buttons to invoke the
// same validated native workflows.
void TriggerPreciseSearch(HWND view) noexcept;
void TriggerPreciseDownload(HWND view) noexcept;
void AddSelectedPreciseProductsToPlan(HWND view) noexcept;
void StartPreciseDownloadPlan(HWND view) noexcept;
void UpdatePreciseDownloadPlanItem(
    HWND view, std::uint64_t id, const wchar_t* action) noexcept;
void TriggerPrecisePause(HWND view) noexcept;
void TriggerPreciseCancel(HWND view) noexcept;
void SelectPreciseProduct(HWND view, const wchar_t* productId, bool selected) noexcept;
void SetPreciseDateRange(HWND view, const SYSTEMTIME& start, const SYSTEMTIME& end) noexcept;
void SetPreciseAnalysisCenter(HWND view, const wchar_t* center) noexcept;
bool QueryPreciseProductSnapshot(HWND view, PreciseUiSnapshot& snapshot) noexcept;
void SetPreciseProductFamily(HWND view, const wchar_t* family) noexcept;
void SetPreciseFilters(
    HWND view, const wchar_t* latency, const wchar_t* completeness,
    const wchar_t* troStation, const wchar_t* search,
    const wchar_t* completenessMode = nullptr,
    double completenessValue = 0.0) noexcept;
void SelectAllVisiblePreciseProducts(HWND view, bool selected) noexcept;
void BrowsePreciseProductFolder(HWND view) noexcept;
void SetPreciseQuickPreset(HWND view, int preset) noexcept;
void ApplyPreciseQuickPreset(HWND view) noexcept;
// Semicolon-separated family~latency~center~interval rules. The fourth
// field is optional; legacy three-field rules impose no sampling filter.
void SetPreciseCustomPreset(
    HWND view, const wchar_t* name, const wchar_t* rules) noexcept;
