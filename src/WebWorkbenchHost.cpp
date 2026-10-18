#include "WebWorkbenchHost.h"
#include "UiPreferencesScope.h"

#include <wrl.h>

#ifndef interface
// Some trimmed Windows SDK installations omit the compatibility macro that
// MIDL-generated COM headers expect.  WebView2.h uses `typedef interface ...`
// in its forward declarations, so provide the normal C++ spelling when it is
// absent rather than requiring a particular SDK layout on the build machine.
#define interface struct
#endif
#include <WebView2.h>

#include <filesystem>
#include <string>
#include <utility>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

std::filesystem::path ModuleDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD count = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (count == 0 || count >= buffer.size()) {
        return std::filesystem::current_path();
    }
    buffer.resize(count);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path ContentDirectory() {
    auto content = ModuleDirectory() / L"resources";
    if (std::filesystem::exists(content / L"ui" / L"index.html")) {
        return content;
    }
    content = std::filesystem::current_path() / L"resources";
    return content;
}

std::filesystem::path WebViewDataDirectory() {
    // Custom product plans and other Web workbench preferences live in
    // localStorage.  A temporary WebView profile can be purged by Windows,
    // so prefer the per-user application-data directory for durable state.
    std::wstring localAppData(32768, L'\0');
    const DWORD localCount = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData.data(),
        static_cast<DWORD>(localAppData.size()));
    if (localCount > 0 && localCount < localAppData.size()) {
        localAppData.resize(localCount);
        return std::filesystem::path(localAppData)
            / L"IGSDownloader" / L"WebView2";
    }
    wchar_t temporary[MAX_PATH]{};
    const DWORD count = GetTempPathW(MAX_PATH, temporary);
    if (count == 0 || count >= MAX_PATH) {
        return ModuleDirectory() / L"webview2-data";
    }
    return std::filesystem::path(temporary) / L"IGSDownloader-WebView2";
}

}  // namespace

struct WebWorkbenchHost::Impl {
    HWND parent = nullptr;
    MessageCallback onMessage;
    ReadyCallback onReady;
    FailureCallback onFailure;
    RECT bounds{};
    bool initializing = false;
    bool ready = false;
    bool hasMessageToken = false;
    bool hasNavigationToken = false;
    EventRegistrationToken messageToken{};
    EventRegistrationToken navigationToken{};
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webView;
};

WebWorkbenchHost::WebWorkbenchHost(
    const HWND parent,
    MessageCallback messageCallback,
    ReadyCallback readyCallback,
    FailureCallback failureCallback)
    : impl_(std::make_unique<Impl>()) {
    impl_->parent = parent;
    impl_->onMessage = std::move(messageCallback);
    impl_->onReady = std::move(readyCallback);
    impl_->onFailure = std::move(failureCallback);
}

WebWorkbenchHost::~WebWorkbenchHost() {
    if (!impl_) return;
    if (impl_->webView && impl_->hasMessageToken) {
        impl_->webView->remove_WebMessageReceived(impl_->messageToken);
    }
    if (impl_->webView && impl_->hasNavigationToken) {
        impl_->webView->remove_NavigationCompleted(impl_->navigationToken);
    }
    if (impl_->controller) {
        impl_->controller->Close();
    }
    impl_->webView.Reset();
    impl_->controller.Reset();
    impl_->environment.Reset();
}

bool WebWorkbenchHost::Initialize() {
    if (!impl_ || impl_->initializing || impl_->ready) return impl_ != nullptr;
    impl_->initializing = true;
    std::error_code error;
    const auto dataDirectory = WebViewDataDirectory();
    std::filesystem::create_directories(dataDirectory, error);

    const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        dataDirectory.c_str(),
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](const HRESULT result,
                   ICoreWebView2Environment* environment) -> HRESULT {
                if (!impl_ || FAILED(result) || environment == nullptr) {
                    if (impl_) {
                        impl_->initializing = false;
                        if (impl_->onFailure) impl_->onFailure();
                    }
                    return FAILED(result) ? result : E_FAIL;
                }
                impl_->environment = environment;
                return environment->CreateCoreWebView2Controller(
                    impl_->parent,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](const HRESULT controllerResult,
                               ICoreWebView2Controller* controller) -> HRESULT {
                            if (!impl_ || FAILED(controllerResult)
                                || controller == nullptr) {
                                if (impl_) {
                                    impl_->initializing = false;
                                    if (impl_->onFailure) impl_->onFailure();
                                }
                                return FAILED(controllerResult)
                                    ? controllerResult : E_FAIL;
                            }
                            impl_->controller = controller;
                            if (FAILED(controller->get_CoreWebView2(
                                    &impl_->webView)) || !impl_->webView) {
                                impl_->initializing = false;
                                if (impl_->onFailure) impl_->onFailure();
                                return E_FAIL;
                            }

                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(impl_->webView->get_Settings(&settings))
                                && settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(TRUE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                            }

                            ComPtr<ICoreWebView2_3> webView3;
                            if (FAILED(impl_->webView.As(&webView3)) || !webView3) {
                                impl_->initializing = false;
                                if (impl_->onFailure) impl_->onFailure();
                                return E_NOINTERFACE;
                            }
                            const auto content = ContentDirectory();
                            if (!std::filesystem::exists(
                                    content / L"ui" / L"index.html")) {
                                impl_->initializing = false;
                                if (impl_->onFailure) impl_->onFailure();
                                return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
                            }
                            const HRESULT mapping =
                                webView3->SetVirtualHostNameToFolderMapping(
                                    L"igs.local",
                                    content.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            if (FAILED(mapping)) {
                                impl_->initializing = false;
                                if (impl_->onFailure) impl_->onFailure();
                                return mapping;
                            }

                            const HRESULT subscribed =
                                impl_->webView->add_WebMessageReceived(
                                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                        [this](ICoreWebView2*,
                                               ICoreWebView2WebMessageReceivedEventArgs* args)
                                            -> HRESULT {
                                            if (!impl_ || args == nullptr) return S_OK;
                                            LPWSTR raw = nullptr;
                                            if (SUCCEEDED(args->TryGetWebMessageAsString(&raw))
                                                && raw != nullptr) {
                                                if (impl_->onMessage) {
                                                    impl_->onMessage(raw);
                                                }
                                                CoTaskMemFree(raw);
                                            }
                                            return S_OK;
                                        }).Get(),
                                    &impl_->messageToken);
                            impl_->hasMessageToken = SUCCEEDED(subscribed);
                            if (!impl_->hasMessageToken) {
                                impl_->initializing = false;
                                if (impl_->onFailure) impl_->onFailure();
                                return subscribed;
                            }

                            controller->put_Bounds(impl_->bounds);
                            // Keep the controller hidden until the complete
                            // document has rendered. Showing it here exposes
                            // a blank or partially styled frame before the
                            // product workbench is ready.
                            controller->put_IsVisible(FALSE);
                            const HRESULT navigationSubscribed =
                                impl_->webView->add_NavigationCompleted(
                                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                        [this](ICoreWebView2*,
                                               ICoreWebView2NavigationCompletedEventArgs* args)
                                            -> HRESULT {
                                            if (!impl_) return S_OK;
                                            BOOL succeeded = FALSE;
                                            if (args == nullptr
                                                || FAILED(args->get_IsSuccess(&succeeded))
                                                || succeeded == FALSE) {
                                                impl_->initializing = false;
                                                impl_->ready = false;
                                                if (impl_->controller) {
                                                    impl_->controller->put_IsVisible(FALSE);
                                                }
                                                if (impl_->onFailure) impl_->onFailure();
                                                return S_OK;
                                            }
                                            impl_->ready = true;
                                            impl_->initializing = false;
                                            if (impl_->onReady) impl_->onReady();
                                            return S_OK;
                                        }).Get(),
                                    &impl_->navigationToken);
                            impl_->hasNavigationToken =
                                SUCCEEDED(navigationSubscribed);
                            if (!impl_->hasNavigationToken) {
                                impl_->initializing = false;
                                if (impl_->onFailure) impl_->onFailure();
                                return navigationSubscribed;
                            }
                            // The virtual host shares a persistent WebView2 profile.
                            // Give each launch a fresh URL so updated local UI files
                            // cannot be shadowed by cached HTML, CSS, or JavaScript.
                            const std::wstring pageUrl =
                                L"https://igs.local/ui/index.html?v="
                                + std::to_wstring(GetCurrentProcessId())
                                + L"-" + std::to_wstring(GetTickCount64())
                                + L"&installation=" + UiPreferencesScope::ForInstallation(ModuleDirectory());
                            const HRESULT navigated = impl_->webView->Navigate(
                                pageUrl.c_str());
                            if (FAILED(navigated)) {
                                impl_->initializing = false;
                                if (impl_->onFailure) impl_->onFailure();
                                return navigated;
                            }
                            return S_OK;
                        }).Get());
            }).Get());
    if (FAILED(started)) {
        impl_->initializing = false;
        return false;
    }
    return true;
}

void WebWorkbenchHost::Resize(const RECT& bounds) {
    if (!impl_) return;
    if (impl_->bounds.left == bounds.left
        && impl_->bounds.top == bounds.top
        && impl_->bounds.right == bounds.right
        && impl_->bounds.bottom == bounds.bottom) {
        return;
    }
    impl_->bounds = bounds;
    if (impl_->controller) {
        impl_->controller->put_Bounds(bounds);
    }
}

void WebWorkbenchHost::SetVisible(const bool visible) const {
    if (impl_ && impl_->controller) {
        impl_->controller->put_IsVisible(visible ? TRUE : FALSE);
    }
}

void WebWorkbenchHost::Focus() const {
    if (impl_ && impl_->controller) {
        impl_->controller->MoveFocus(
            COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
}

void WebWorkbenchHost::PostJson(const std::wstring& json) const {
    if (impl_ && impl_->webView && !json.empty()) {
        impl_->webView->PostWebMessageAsJson(json.c_str());
    }
}

bool WebWorkbenchHost::IsReady() const noexcept {
    return impl_ && impl_->ready;
}
