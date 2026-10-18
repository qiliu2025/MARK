#include "NativeUiLanguage.h"
#include "MainWindow.h"
#include "MapView.h"
#include "PreciseProductView.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>

#include <cwchar>
#include <iterator>

namespace {

bool ActivateFocusedButton(const HWND root) noexcept {
    const HWND focused = GetFocus();
    if (focused == nullptr || !IsChild(root, focused)
        || !IsWindowEnabled(focused)) {
        return false;
    }
    wchar_t className[32]{};
    if (GetClassNameW(focused, className,
                      static_cast<int>(std::size(className))) <= 0
        || std::wcscmp(className, L"Button") != 0) {
        return false;
    }
    // Owner-draw buttons do not carry BS_DEFPUSHBUTTON in their low style
    // nibble.  Routing Enter through BM_CLICK restores the native default
    // button behaviour without giving up the custom renderer.
    SendMessageW(focused, BM_CLICK, 0, 0);
    return true;
}

}  // namespace

int APIENTRY wWinMain(
    const HINSTANCE instance,
    HINSTANCE,
    wchar_t*,
    const int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_DATE_CLASSES
        | ICC_LISTVIEW_CLASSES
        | ICC_PROGRESS_CLASS
        | ICC_STANDARD_CLASSES;
    if (!InitCommonControlsEx(&controls)) {
        NativeUiLanguage::ShowMessageBox(
            nullptr,
            L"无法初始化 Windows 公共控件。",
            L"MARK",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    const HRESULT comResult = CoInitializeEx(
        nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitializeCom = SUCCEEDED(comResult);

    if (!RegisterMapViewClass(instance)
        || !RegisterPreciseProductViewClass(instance)
        || !RegisterMainWindowClass(instance)) {
        NativeUiLanguage::ShowMessageBox(
            nullptr,
            L"无法注册程序窗口类。",
            L"MARK",
            MB_OK | MB_ICONERROR);
        if (uninitializeCom) {
            CoUninitialize();
        }
        return 1;
    }

    const HWND mainWindow = CreateMainWindow(instance, showCommand);
    if (mainWindow == nullptr) {
        NativeUiLanguage::ShowMessageBox(
            nullptr,
            L"无法创建主窗口。",
            L"MARK",
            MB_OK | MB_ICONERROR);
        if (uninitializeCom) {
            CoUninitialize();
        }
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN
            && message.wParam == VK_RETURN
            && ActivateFocusedButton(mainWindow)) {
            continue;
        }
        // The redesigned window is a control parent rather than a dialog
        // resource.  IsDialogMessageW still gives native Tab/Shift+Tab
        // traversal across both pages without changing command handling.
        if (!IsDialogMessageW(mainWindow, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    if (uninitializeCom) {
        CoUninitialize();
    }
    return static_cast<int>(message.wParam);
}
