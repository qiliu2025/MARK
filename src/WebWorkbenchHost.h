#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

// Hosts the HTML/CSS workbench inside the existing Win32 process.  The
// native query/download controls remain alive behind it and continue to own
// all data access; the WebView sends small command messages to the existing
// MainWindow controller and receives JSON state updates in return.
class WebWorkbenchHost {
public:
    using MessageCallback = std::function<void(const std::wstring&)>;
    using ReadyCallback = std::function<void()>;
    using FailureCallback = std::function<void()>;

    WebWorkbenchHost(
        HWND parent,
        MessageCallback messageCallback,
        ReadyCallback readyCallback,
        FailureCallback failureCallback);
    ~WebWorkbenchHost();

    WebWorkbenchHost(const WebWorkbenchHost&) = delete;
    WebWorkbenchHost& operator=(const WebWorkbenchHost&) = delete;

    bool Initialize();
    void Resize(const RECT& bounds);
    void SetVisible(bool visible) const;
    void Focus() const;
    void PostJson(const std::wstring& json) const;
    [[nodiscard]] bool IsReady() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
