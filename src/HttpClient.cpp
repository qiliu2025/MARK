#include "DownloadFileValidation.h"
#include "HttpClient.h"
#include "NativeUiLanguage.h"
#include "ArchiveMemberDownload.h"

#include <windows.h>
#include <wininet.h>
#include <bcrypt.h>
#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <format>
#include <future>
#include <cwctype>
#include <iterator>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <regex>
#include <semaphore>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

void DownloadControl::setPaused(const bool paused) noexcept {
    paused_.store(paused, std::memory_order_release);
    if (!paused) {
        // A resume must wake every transfer worker that may be waiting on the
        // same batch-wide control object.
        pauseCondition_.notify_all();
    }
}

void DownloadControl::pause() noexcept {
    setPaused(true);
}

void DownloadControl::resume() noexcept {
    setPaused(false);
}

bool DownloadControl::isPaused() const noexcept {
    return paused_.load(std::memory_order_acquire);
}

bool DownloadControl::waitIfPaused(
    const std::stop_token stopToken) const noexcept {
    if (stopToken.stop_requested()) return false;
    if (!isPaused()) return true;

    // Timed waits poll cancellation when stop_source is not attached to this
    // condition variable; resume() also signals the waiter.
    std::unique_lock lock(pauseMutex_);
    while (paused_.load(std::memory_order_acquire)
           && !stopToken.stop_requested()) {
        pauseCondition_.wait_for(lock, std::chrono::milliseconds(100));
    }
    return !stopToken.stop_requested();
}

namespace {

std::once_flag gCurlInitializationOnce;
CURLcode gCurlInitializationResult = CURLE_FAILED_INIT;

std::wstring Utf8ToWide(std::string_view text);

bool EnsureLibCurl(std::wstring& error) {
    std::call_once(gCurlInitializationOnce, [] {
        gCurlInitializationResult = curl_global_init(CURL_GLOBAL_DEFAULT);
    });
    if (gCurlInitializationResult == CURLE_OK) return true;
    error = L"无法初始化内置 libcurl："
        + Utf8ToWide(curl_easy_strerror(gCurlInitializationResult));
    return false;
}

// Date-level workers may run at high concurrency, but the public FTP
// services have much lower per-client connection limits.  Keep the fast
// sources parallel while preventing CDDIS/ESA from being overloaded.
std::counting_semaphore<64> gCddisFtpSlots{1};
std::counting_semaphore<64> gEsaFtpSlots{6};

class SemaphorePermit {
public:
    explicit SemaphorePermit(std::counting_semaphore<64>* semaphore)
        : semaphore_(semaphore) {
        if (semaphore_ != nullptr) {
            semaphore_->acquire();
        }
    }

    ~SemaphorePermit() {
        if (semaphore_ != nullptr) {
            semaphore_->release();
        }
    }

    SemaphorePermit(const SemaphorePermit&) = delete;
    SemaphorePermit& operator=(const SemaphorePermit&) = delete;

private:
    std::counting_semaphore<64>* semaphore_ = nullptr;
};

struct InternetHandleCloser {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr) {
            InternetCloseHandle(handle);
        }
    }
};

using UniqueInternetHandle = std::unique_ptr<void, InternetHandleCloser>;

struct KernelHandleCloser {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

using UniqueKernelHandle = std::unique_ptr<void, KernelHandleCloser>;

constexpr wchar_t kTerrasSemaphoreName[] =
    L"Local\\GNSSDownloader_Terras_GlobalSlots_v2";
constexpr LONG kTerrasMaximumConcurrentRequests = 6;
constexpr wchar_t kTerrasSftpSemaphoreName[] =
    L"Local\\GNSSDownloader_Terras_SftpSingleSession_v1";

bool IsTerrasUrl(const std::wstring_view url) {
    constexpr std::wstring_view prefix = L"https://terras.gsi.go.jp/";
    return url.starts_with(prefix);
}

// Terras's per-client limit is shared by the GUI and index builder.
// A named semaphore coordinates them; timed waits poll cancellation.
class TerrasNamedPermit {
public:
    TerrasNamedPermit(
        const std::stop_token stopToken,
        std::wstring& error,
        const wchar_t* semaphoreName = kTerrasSemaphoreName,
        const LONG maximumPermits = kTerrasMaximumConcurrentRequests) {
        semaphore_.reset(CreateSemaphoreW(
            nullptr,
            maximumPermits,
            maximumPermits,
            semaphoreName));
        if (!semaphore_) {
            error = L"无法创建 GEONET/Terras 跨进程并发控制器："
                + GetLastWin32ErrorMessage(GetLastError());
            return;
        }
        while (!stopToken.stop_requested()) {
            const DWORD waitResult = WaitForSingleObject(
                static_cast<HANDLE>(semaphore_.get()), 100);
            if (waitResult == WAIT_OBJECT_0) {
                acquired_ = true;
                return;
            }
            if (waitResult != WAIT_TIMEOUT) {
                error = L"等待 GEONET/Terras 下载名额失败："
                    + GetLastWin32ErrorMessage(GetLastError());
                return;
            }
        }
        cancelled_ = true;
        error = L"等待 GEONET/Terras 下载名额时已取消。";
    }

    ~TerrasNamedPermit() {
        if (acquired_) {
            ReleaseSemaphore(
                static_cast<HANDLE>(semaphore_.get()), 1, nullptr);
        }
    }

    TerrasNamedPermit(const TerrasNamedPermit&) = delete;
    TerrasNamedPermit& operator=(const TerrasNamedPermit&) = delete;

    [[nodiscard]] bool acquired() const noexcept { return acquired_; }
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }

private:
    UniqueKernelHandle semaphore_;
    bool acquired_ = false;
    bool cancelled_ = false;
};

std::string WideToUtf8(const std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int count = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (count <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        count,
        nullptr,
        nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (count <= 0) {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        count);
    return result;
}

std::wstring TrimWide(std::wstring value) {
    const auto isSpace = [](const wchar_t c) {
        return std::iswspace(c) != 0;
    };
    value.erase(value.begin(), std::find_if_not(
        value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(
        value.rbegin(), value.rend(), isSpace).base(), value.end());
    return value;
}

std::wstring CurrentSystemProxyForUrl(const std::wstring_view url) {
    // Read the registry value first.  On some Windows builds
    // InternetQueryOption(INTERNET_OPTION_PROXY) returns an
    // INTERNET_PROXY_INFO whose string pointers are not self-contained in
    // the caller's buffer; treating those pointers as a null-terminated
    // string can yield a stray CR/LF block and ultimately a malformed
    // `--proxy http://` argument (curl error 3).  Internet Settings is the
    // same source used by the user's global proxy toggle and gives us a
    // bounded, length-aware value.
    constexpr wchar_t kInternetSettings[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
    const auto readRegistryProxy = [&]() -> std::wstring {
        DWORD enabled = 0;
        DWORD enabledBytes = sizeof(enabled);
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                kInternetSettings,
                L"ProxyEnable",
                RRF_RT_REG_DWORD,
                nullptr,
                &enabled,
                &enabledBytes) != ERROR_SUCCESS
            || enabled == 0) {
            return {};
        }
        DWORD proxyBytes = 0;
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                kInternetSettings,
                L"ProxyServer",
                RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                nullptr,
                nullptr,
                &proxyBytes) != ERROR_SUCCESS
            || proxyBytes <= sizeof(wchar_t)
            || proxyBytes > 64U * 1024U) {
            return {};
        }
        std::vector<wchar_t> proxy(
            proxyBytes / sizeof(wchar_t) + 1U, L'\0');
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                kInternetSettings,
                L"ProxyServer",
                RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                nullptr,
                proxy.data(),
                &proxyBytes) != ERROR_SUCCESS) {
            return {};
        }
        const std::size_t characterCount = std::min<std::size_t>(
            proxy.size(), proxyBytes / sizeof(wchar_t));
        std::wstring value(proxy.data(), characterCount);
        while (!value.empty() && (value.back() == L'\0'
                                  || value.back() == L'\r'
                                  || value.back() == L'\n')) {
            value.pop_back();
        }
        return TrimWide(std::move(value));
    };

    std::wstring list = readRegistryProxy();

    // If no explicit Internet Settings proxy is enabled, retain the
    // WinINet/PAC fallback, but only accept printable bounded text.  This
    // preserves PAC support without trusting an invalid pointer payload.
    if (list.empty()) {
        UniqueInternetHandle session(InternetOpenW(
            L"GNSSDownloader/libcurl-proxy",
            INTERNET_OPEN_TYPE_PRECONFIG,
            nullptr,
            nullptr,
            0));
        if (session) {
            DWORD bytes = 0;
            InternetQueryOptionW(
                static_cast<HINTERNET>(session.get()),
                INTERNET_OPTION_PROXY,
                nullptr,
                &bytes);
            if (bytes >= sizeof(INTERNET_PROXY_INFO)
                && bytes <= 64U * 1024U) {
                std::vector<std::byte> storage(bytes);
                if (InternetQueryOptionW(
                        static_cast<HINTERNET>(session.get()),
                        INTERNET_OPTION_PROXY,
                        storage.data(),
                        &bytes)) {
                    const auto* information =
                        reinterpret_cast<const INTERNET_PROXY_INFO*>(
                            storage.data());
                    if (information->dwAccessType == INTERNET_OPEN_TYPE_PROXY
                        && information->lpszProxy != nullptr) {
                        // Do not read past a conservative bound.  The
                        // registry path above is preferred precisely because
                        // this pointer is not guaranteed to be self-contained.
                        for (std::size_t length = 0; length < 4096U;
                             ++length) {
                            const wchar_t character =
                                information->lpszProxy[length];
                            if (character == L'\0') {
                                list.assign(
                                    information->lpszProxy, length);
                                break;
                            }
                            if (character < 0x20 || character == L'\"') {
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    // Remove control characters that can be returned by malformed PAC or
    // proxy values before constructing a command line or passing the value to
    // libcurl.
    list.erase(std::remove_if(list.begin(), list.end(), [](const wchar_t c) {
        return c == L'\0' || c == L'\r' || c == L'\n';
    }), list.end());
    list = TrimWide(std::move(list));
    if (list.empty()) return {};

    const bool https = url.starts_with(L"https://");
    const bool ftp = url.starts_with(L"ftp://")
        || url.starts_with(L"ftps://");
    std::wstring fallback;
    std::size_t start = 0;
    while (start <= list.size()) {
        const std::size_t separator = list.find(L';', start);
        std::wstring entry = TrimWide(list.substr(
            start,
            separator == std::wstring::npos
                ? std::wstring::npos
                : separator - start));
        const std::size_t equals = entry.find(L'=');
        if (equals == std::wstring::npos) {
            if (fallback.empty()) fallback = entry;
        } else {
            std::wstring protocol = entry.substr(0, equals);
            std::transform(
                protocol.begin(), protocol.end(), protocol.begin(),
                [](const wchar_t c) {
                    return static_cast<wchar_t>(std::towlower(c));
                });
            const std::wstring endpoint = TrimWide(entry.substr(equals + 1));
            if ((https && protocol == L"https")
                || (ftp && protocol == L"ftp")
                || (!https && !ftp && protocol == L"http")) {
                return endpoint;
            }
            if (fallback.empty()
                && (protocol == L"socks" || protocol == L"proxy")) {
                fallback = endpoint;
            }
        }
        if (separator == std::wstring::npos) break;
        start = separator + 1;
    }
    return fallback;
}

// Honour an explicitly supplied proxy environment before consulting the
// Windows Internet Settings value.  This is important for SOCKS5 clients:
// the registry may contain a bare endpoint while ALL_PROXY carries the
// actual `socks5h://` scheme.  Only accept scheme-qualified values from the
// environment so an accidental command-line fragment cannot become a proxy
// argument.
std::wstring ConfiguredProxyEnvironment() {
    constexpr std::array<const wchar_t*, 2> names{
        L"GNSS_DOWNLOADER_PROXY", L"ALL_PROXY"};
    constexpr std::array<std::wstring_view, 4> schemes{
        L"socks5h://", L"socks5://", L"http://", L"https://"};
    for (const auto* name : names) {
        const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required <= 1 || required >= 2048) continue;
        std::wstring value(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(
            name, value.data(), required);
        value.resize(written);
        value = TrimWide(std::move(value));
        if (value.empty()
            || value.find_first_of(L"\r\n\"") != std::wstring::npos) {
            continue;
        }
        if (std::any_of(schemes.begin(), schemes.end(),
                        [&](const std::wstring_view scheme) {
                            return value.starts_with(scheme);
                        })) {
            return value;
        }
    }
    return {};
}

struct ResolvedProxy {
    bool forceDirect = false;
    std::string value;
    // Windows Internet Settings permits a bare `host:port` value, but that
    // value does not identify HTTP versus SOCKS.  For FTP/FTPS we try SOCKS
    // first and retain the endpoint so the next retry can try HTTP.  This is
    // protocol auto-detection, not a machine-specific port assumption.
    bool ambiguousSystemEndpoint = false;
    std::string ambiguousEndpoint;
};

ResolvedProxy ResolveProxyForUrl(const std::wstring_view url) {
    // Network routing is deliberately controlled outside the application.
    // A Windows/WinINet proxy configured by the user takes precedence.  When
    // none is configured, leaving CURLOPT_PROXY untouched lets libcurl honor
    // ALL_PROXY/HTTPS_PROXY/HTTP_PROXY; without either, the connection is
    // direct (or is transparently routed by an external TUN/global-mode VPN).
    std::wstring proxy = ConfiguredProxyEnvironment();
    const bool explicitProxy = !proxy.empty();
    if (proxy.empty()) proxy = CurrentSystemProxyForUrl(url);
    if (proxy.empty()) return {false, {}};
    ResolvedProxy resolved;
    if (proxy.find(L"://") == std::wstring::npos) {
        const bool ftp = url.starts_with(L"ftp://")
            || url.starts_with(L"ftps://");
        if (!explicitProxy && ftp) {
            resolved.ambiguousSystemEndpoint = true;
            resolved.ambiguousEndpoint = WideToUtf8(proxy);
            // A bare system proxy is ambiguous.  SOCKS is attempted first
            // because it supports FTP's separate control/data connections;
            // the retry path below automatically tries HTTP as well.
            proxy.insert(0, L"socks5h://");
        } else {
            proxy.insert(0, L"http://");
        }
    }
    resolved.value = WideToUtf8(proxy);
    return resolved;
}

void ApplyProxySettings(CURL* easy, const std::wstring_view url) {
    const auto proxy = ResolveProxyForUrl(url);
    if (proxy.forceDirect) {
        curl_easy_setopt(easy, CURLOPT_PROXY, "");
        curl_easy_setopt(easy, CURLOPT_NOPROXY, "*");
    } else if (!proxy.value.empty()) {
        curl_easy_setopt(easy, CURLOPT_PROXY, proxy.value.c_str());
        curl_easy_setopt(easy, CURLOPT_NOPROXY, "");
    }
}

struct CurlEasyCloser {
    void operator()(CURL* easy) const noexcept {
        if (easy != nullptr) curl_easy_cleanup(easy);
    }
};

using UniqueCurlEasy = std::unique_ptr<CURL, CurlEasyCloser>;

struct CurlTransferContext {
    std::FILE* output = nullptr;
    std::string* memory = nullptr;
    std::size_t maximumBytes = 0;
    std::uint64_t bytesWritten = 0;
    std::stop_token stopToken;
    DownloadControlPtr control;
    DownloadProgressCallback progress;
    bool exceededLimit = false;
};

bool WaitForDownloadControl(
    const DownloadControlPtr& control,
    const std::stop_token stopToken) {
    return !control || control->waitIfPaused(stopToken);
}

std::size_t LibCurlWrite(
    char* data,
    const std::size_t size,
    const std::size_t count,
    void* userData) {
    auto* context = static_cast<CurlTransferContext*>(userData);
    const std::size_t bytes = size * count;
    if (context == nullptr || context->stopToken.stop_requested()) return 0;
    if (!WaitForDownloadControl(context->control, context->stopToken)) {
        return 0;
    }
    if (context->maximumBytes > 0
        && context->bytesWritten + bytes > context->maximumBytes) {
        context->exceededLimit = true;
        return 0;
    }
    if (context->output != nullptr) {
        const std::size_t written = std::fwrite(data, 1, bytes, context->output);
        context->bytesWritten += written;
        return written;
    }
    if (context->memory != nullptr) {
        context->memory->append(data, bytes);
        context->bytesWritten += bytes;
        return bytes;
    }
    return 0;
}

int LibCurlProgress(
    void* userData,
    const curl_off_t downloadTotal,
    const curl_off_t downloaded,
    curl_off_t,
    curl_off_t) {
    auto* context = static_cast<CurlTransferContext*>(userData);
    if (context == nullptr || context->stopToken.stop_requested()) return 1;
    if (!WaitForDownloadControl(context->control, context->stopToken)) {
        return 1;
    }
    if (context->progress) {
        context->progress(
            downloaded > 0 ? static_cast<std::uint64_t>(downloaded) : 0,
            downloadTotal > 0
                ? static_cast<std::uint64_t>(downloadTotal)
                : 0);
    }
    return 0;
}

std::filesystem::path BundledCaCertificatePath() {
    std::array<wchar_t, 32768> modulePath{};
    const DWORD length = GetModuleFileNameW(
        nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size()) return {};
    return std::filesystem::path(
        std::wstring_view(modulePath.data(), length)).parent_path()
        / L"curl-ca-bundle.crt";
}

void ConfigureLibCurlCommon(
    CURL* easy,
    const std::wstring_view url,
    const char* protocols,
    CurlTransferContext& context) {
    const std::string narrowUrl = WideToUtf8(url);
    curl_easy_setopt(easy, CURLOPT_URL, narrowUrl.c_str());
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(easy, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 32L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, protocols);
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
    curl_easy_setopt(
        easy,
        CURLOPT_USERAGENT,
        "GNSSDownloader/0.28 libcurl");
    const auto caCertificate = BundledCaCertificatePath();
    std::error_code certificateError;
    if (!caCertificate.empty()
        && std::filesystem::is_regular_file(
            caCertificate, certificateError)) {
        // LibreSSL opens narrow paths on Windows. Read the bundle through a
        // filesystem path so installations in Unicode folders can verify TLS.
        static const std::string certificateData = [] {
            std::ifstream input(BundledCaCertificatePath(), std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), {});
        }();
        curl_blob certificateBlob{
            const_cast<char*>(certificateData.data()), certificateData.size(), CURL_BLOB_COPY};
        curl_easy_setopt(easy, CURLOPT_CAINFO_BLOB, &certificateBlob);
    } else {
        curl_easy_setopt(
            easy,
            CURLOPT_SSL_OPTIONS,
            static_cast<long>(CURLSSLOPT_NATIVE_CA));
    }
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, LibCurlWrite);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, LibCurlProgress);
    curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &context);
    ApplyProxySettings(easy, url);
}

bool IsHttpsUrl(const std::wstring& url, std::wstring& error) {
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    if (!InternetCrackUrlW(
            url.c_str(),
            static_cast<DWORD>(url.size()),
            0,
            &components)) {
        error = L"无法解析下载地址：" + GetLastWin32ErrorMessage(GetLastError());
        return false;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTPS) {
        error = L"出于安全考虑，只允许 HTTPS 地址。";
        return false;
    }
    return true;
}

bool IsAllowedPublicFtpUrl(const std::wstring& url) {
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    if (!InternetCrackUrlW(
            url.c_str(),
            static_cast<DWORD>(url.size()),
            0,
            &components)
        || components.nScheme != INTERNET_SCHEME_FTP) {
        return false;
    }
    const std::wstring host(
        components.lpszHostName,
        components.dwHostNameLength);
    return _wcsicmp(host.c_str(), L"ftp.sonel.org") == 0
        || _wcsicmp(host.c_str(), L"ftp.trignet.co.za") == 0
        || _wcsicmp(host.c_str(), L"ftp.ga.gov.au") == 0
        || _wcsicmp(host.c_str(), L"rgpdata.ign.fr") == 0
        || _wcsicmp(host.c_str(), L"rgpdata.ensg.eu") == 0
        || _wcsicmp(host.c_str(), L"www.ngs.noaa.gov") == 0
        || _wcsicmp(host.c_str(), L"epncb.oma.be") == 0
        || _wcsicmp(host.c_str(), L"igs-ftp.bkg.bund.de") == 0
        || _wcsicmp(host.c_str(), L"rtopsdata1.geod.nrcan.gc.ca") == 0
        || _wcsicmp(host.c_str(), L"cacsa.nrcan.gc.ca") == 0
        || _wcsicmp(host.c_str(), L"cacsb.nrcan.gc.ca") == 0
        || _wcsicmp(host.c_str(), L"garner.ucsd.edu") == 0
        || _wcsicmp(host.c_str(), L"ftp.geonet.org.nz") == 0
        || _wcsicmp(host.c_str(), L"ftp.sirgas.org") == 0
        || _wcsicmp(host.c_str(), L"pp.igm.gub.uy") == 0
        || _wcsicmp(host.c_str(), L"rgpdata.ensg.eu") == 0
        || _wcsicmp(host.c_str(), L"srvstorage.igac.gov.co") == 0
        || _wcsicmp(host.c_str(), L"geodesia.igac.gov.co") == 0
        || _wcsicmp(host.c_str(), L"gdc.cddis.eosdis.nasa.gov") == 0
        || _wcsicmp(host.c_str(), L"igs.gnsswhu.cn") == 0
        || _wcsicmp(host.c_str(), L"nfs.kasi.re.kr") == 0
        || _wcsicmp(host.c_str(), L"gnss-ftp.kasi.re.kr") == 0
        || _wcsicmp(host.c_str(), L"igs.ign.fr") == 0
        || _wcsicmp(host.c_str(), L"igs.ensg.ign.fr") == 0
        || _wcsicmp(host.c_str(), L"gssc.esa.int") == 0;
}

bool IsBdsPrideFtpsUrl(const std::wstring_view url) {
    // WCC publishes its combined products through implicit FTPS and its
    // current certificate chain is not trusted by Windows Schannel. Keep the
    // exception pinned to the official read-only archive path.
    constexpr std::wstring_view prefix = L"ftps://bdspride.com/";
    if (!url.starts_with(prefix) || url.size() > 500
        || url.find(L"..") != std::wstring_view::npos) return false;
    return std::all_of(url.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
        url.end(), [](const wchar_t character) {
            return (character >= L'a' && character <= L'z')
                || (character >= L'A' && character <= L'Z')
                || (character >= L'0' && character <= L'9')
                || character == L'/' || character == L'_'
                || character == L'-' || character == L'.';
        });
}

bool UrlHostEquals(
    const std::wstring& url,
    const wchar_t* expectedHost) {
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    if (!InternetCrackUrlW(
            url.c_str(),
            static_cast<DWORD>(url.size()),
            0,
            &components)) {
        return false;
    }
    const std::wstring host(
        components.lpszHostName,
        components.dwHostNameLength);
    return _wcsicmp(host.c_str(), expectedHost) == 0;
}

bool IsCddisExplicitTlsFtp(const std::wstring& url) {
    return UrlHostEquals(url, L"gdc.cddis.eosdis.nasa.gov");
}

bool NeedsExplicitAnonymousFtpLogin(const std::wstring& url) {
    return IsCddisExplicitTlsFtp(url)
        || UrlHostEquals(url, L"igs.ign.fr")
        || UrlHostEquals(url, L"igs.ensg.ign.fr")
        || UrlHostEquals(url, L"gssc.esa.int")
        // TrigNet accepts anonymous post-processing access, but its FTP
        // gateway is stricter than most public archives about the USER/PASS
        // exchange.  Supplying the anonymous identity explicitly also makes
        // behavior consistent across Windows curl builds.
        || UrlHostEquals(url, L"ftp.trignet.co.za");
}

bool IsAllowedPublicHttpUrl(const std::wstring& url) {
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    if (!InternetCrackUrlW(
            url.c_str(),
            static_cast<DWORD>(url.size()),
            0,
            &components)
        || components.nScheme != INTERNET_SCHEME_HTTP) {
        return false;
    }
    const std::wstring host(
        components.lpszHostName,
        components.dwHostNameLength);
    // These legacy scientific archives currently expose only working HTTP
    // endpoints.  Keep the exception host-scoped and never allow arbitrary
    // plain HTTP download URLs.
    return _wcsicmp(host.c_str(), L"saegnss2.curtin.edu.au") == 0
        || _wcsicmp(host.c_str(), L"garner.ucsd.edu") == 0
        // GA's public anonymous S3 bucket. Some Windows Schannel/proxy
        // combinations cannot establish TLS to the bucket, while the same
        // read-only objects remain available over S3 HTTP. Keep this
        // exception pinned to the exact official bucket host.
        || _wcsicmp(
            host.c_str(),
            L"ga-gnss-data-rinex-v1.s3.amazonaws.com") == 0;
}

bool IsSafeFederatedPublicHttpUrl(const std::wstring& url) {
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    if (url.size() > 2048
        || url.find(L"..") != std::wstring::npos
        || url.find(L'@') != std::wstring::npos
        || !InternetCrackUrlW(
            url.c_str(),
            static_cast<DWORD>(url.size()),
            0,
            &components)
        || components.nScheme != INTERNET_SCHEME_HTTP) {
        return false;
    }
    std::wstring host(
        components.lpszHostName,
        components.dwHostNameLength);
    std::transform(host.begin(), host.end(), host.begin(),
        [](const wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    if (host.empty() || host == L"localhost"
        || host.ends_with(L".localhost")
        || host.ends_with(L".local")
        || host.find(L'.') == std::wstring::npos) {
        return false;
    }
    // Do not let a locally modified knowledge base turn this opt-in route
    // into a request to a private numeric address.  EPOS nodes use DNS names.
    const bool numericAddress = std::all_of(
        host.begin(),
        host.end(),
        [](const wchar_t character) {
            return (character >= L'0' && character <= L'9')
                || character == L'.';
        });
    return !numericAddress;
}

bool IsCsnUrl(const std::wstring& url) {
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    if (!InternetCrackUrlW(
            url.c_str(),
            static_cast<DWORD>(url.size()),
            0,
            &components)
        || components.nScheme != INTERNET_SCHEME_HTTPS) {
        return false;
    }
    const std::wstring host(
        components.lpszHostName,
        components.dwHostNameLength);
    return _wcsicmp(host.c_str(), L"gps.csn.uchile.cl") == 0
        || _wcsicmp(host.c_str(), L"ops.csn.uchile.cl") == 0;
}

bool IsGaPublicS3Url(const std::wstring& url) {
    return UrlHostEquals(
        url,
        L"ga-gnss-data-rinex-v1.s3.ap-southeast-2.amazonaws.com")
        || UrlHostEquals(
            url,
            L"ga-gnss-data-rinex-v1.s3.amazonaws.com");
}

bool IsNoaaCorsS3Url(const std::wstring& url) {
    return UrlHostEquals(url, L"noaa-cors-pds.s3.amazonaws.com");
}

bool IsGeoNetMetadataS3Url(const std::wstring& url) {
    // WinINet can stall on stale proxies for this GeoNet Site Log bucket.
    // Use libcurl directly for these read-only metadata requests.
    return UrlHostEquals(
        url, L"geonet-meta.s3-ap-southeast-2.amazonaws.com");
}

bool IsIersDataCenterUrl(const std::wstring& url) {
    return UrlHostEquals(url, L"datacenter.iers.org");
}

std::mutex gCsnChallengeMutex;
std::wstring gCsnChallengeToken;

bool OpenUrl(
    const std::wstring& url,
    UniqueInternetHandle& session,
    UniqueInternetHandle& request,
    std::wstring& error) {
    std::wstring schemeError;
    const bool secure = IsHttpsUrl(url, schemeError);
    if (!secure && !IsAllowedPublicHttpUrl(url)) {
        error = std::move(schemeError);
        return false;
    }

    DWORD connectTimeout = 10'000;
    DWORD sendTimeout = 20'000;
    DWORD receiveTimeout = 30'000;
    std::wstring headers =
        L"Accept: */*\r\n"
        L"Accept-Language: zh-CN,zh;q=0.9,en;q=0.8\r\n"
        L"Cache-Control: no-cache\r\n";
    if (IsCsnUrl(url)) {
        std::scoped_lock lock(gCsnChallengeMutex);
        if (!gCsnChallengeToken.empty()) {
            headers += L"Cookie: challenge_token="
                + gCsnChallengeToken
                + L"\r\n";
        }
    }
    DWORD flags =
        INTERNET_FLAG_RELOAD
        | INTERNET_FLAG_NO_CACHE_WRITE
        | INTERNET_FLAG_NO_UI
        | INTERNET_FLAG_KEEP_CONNECTION;
    if (secure) {
        flags |= INTERNET_FLAG_SECURE;
    }

    const auto openWithAccessType = [&](const DWORD accessType) {
        const wchar_t* userAgent = IsCsnUrl(url)
            ? L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
              L"AppleWebKit/537.36 (KHTML, like Gecko) "
              L"Chrome/138.0.0.0 Safari/537.36"
            : L"GNSSDownloader/0.8 (Windows; public-GNSS-archive-client)";
        session.reset(InternetOpenW(
            userAgent,
            accessType,
            nullptr,
            nullptr,
            0));
        if (!session) {
            return false;
        }
        InternetSetOptionW(
            static_cast<HINTERNET>(session.get()),
            INTERNET_OPTION_CONNECT_TIMEOUT,
            &connectTimeout,
            sizeof(connectTimeout));
        InternetSetOptionW(
            static_cast<HINTERNET>(session.get()),
            INTERNET_OPTION_SEND_TIMEOUT,
            &sendTimeout,
            sizeof(sendTimeout));
        InternetSetOptionW(
            static_cast<HINTERNET>(session.get()),
            INTERNET_OPTION_RECEIVE_TIMEOUT,
            &receiveTimeout,
            sizeof(receiveTimeout));
        request.reset(InternetOpenUrlW(
            static_cast<HINTERNET>(session.get()),
            url.c_str(),
            headers.c_str(),
            static_cast<DWORD>(-1),
            flags,
            0));
        return request != nullptr;
    };

    // Respect the user's Windows proxy first. If it is stale or unavailable,
    // retry directly; all configured public archives support ordinary HTTPS.
    if (!openWithAccessType(INTERNET_OPEN_TYPE_PRECONFIG)) {
        request.reset();
        session.reset();
        openWithAccessType(INTERNET_OPEN_TYPE_DIRECT);
    }
    if (!request) {
        error = L"发送 HTTPS 请求失败：" + GetLastWin32ErrorMessage(GetLastError());
        return false;
    }
    return true;
}

bool ReadInternetText(
    HINTERNET request,
    const std::stop_token stopToken,
    const std::size_t maximumBytes,
    std::string& body,
    std::wstring& error) {
    std::array<char, 64U * 1024U> buffer{};
    body.clear();
    while (!stopToken.stop_requested()) {
        DWORD bytesRead = 0;
        if (!InternetReadFile(
                request,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytesRead)) {
            error = L"读取网络数据失败："
                + GetLastWin32ErrorMessage(GetLastError());
            body.clear();
            return false;
        }
        if (bytesRead == 0) {
            return true;
        }
        if (body.size() + bytesRead > maximumBytes) {
            error = L"服务器响应超过安全大小限制。";
            body.clear();
            return false;
        }
        body.append(buffer.data(), bytesRead);
    }
    error = L"操作已取消。";
    body.clear();
    return false;
}

bool SolveCsnChallenge(
    const std::string& body,
    const std::stop_token stopToken,
    std::wstring& error) {
    if (body.find("Browser Verification") == std::string::npos
        || body.find("challenge_token=") == std::string::npos) {
        error = L"CSN 返回了无法识别的 HTTP 503 页面。";
        return false;
    }

    const auto tokenMarker = body.find("challenge_token=");
    std::string seed;
    std::size_t position = 0;
    while (position + 40 <= tokenMarker) {
        bool hexadecimal = true;
        for (std::size_t index = 0; index < 40; ++index) {
            const unsigned char character = static_cast<unsigned char>(
                body[position + index]);
            if (!std::isxdigit(character)) {
                hexadecimal = false;
                break;
            }
        }
        if (hexadecimal
            && (position == 0
                || !std::isxdigit(
                    static_cast<unsigned char>(body[position - 1])))
            && (position + 40 == body.size()
                || !std::isxdigit(
                    static_cast<unsigned char>(body[position + 40])))) {
            seed = body.substr(position, 40);
        }
        ++position;
    }
    if (seed.empty()) {
        error = L"CSN 浏览器验证页中没有找到挑战种子。";
        return false;
    }

    const auto hexDigit = [](const char character) {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        return character - 'A' + 10;
    };
    const int byteIndex = hexDigit(seed.front());

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    const NTSTATUS openStatus = BCryptOpenAlgorithmProvider(
        &algorithm,
        BCRYPT_SHA1_ALGORITHM,
        nullptr,
        0);
    if (openStatus < 0 || algorithm == nullptr) {
        error = L"Windows SHA-1 服务不可用，无法完成 CSN 浏览器验证。";
        return false;
    }

    std::array<UCHAR, 20> digest{};
    std::string token;
    bool solved = false;
    for (std::uint32_t nonce = 0;
         nonce < 10'000'000U && !stopToken.stop_requested();
         ++nonce) {
        token = seed + std::to_string(nonce);
        const NTSTATUS hashStatus = BCryptHash(
            algorithm,
            nullptr,
            0,
            reinterpret_cast<PUCHAR>(token.data()),
            static_cast<ULONG>(token.size()),
            digest.data(),
            static_cast<ULONG>(digest.size()));
        if (hashStatus < 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            error = L"计算 CSN 浏览器验证令牌失败。";
            return false;
        }
        if (digest[static_cast<std::size_t>(byteIndex)] == 0xb0
            && digest[static_cast<std::size_t>(byteIndex + 1)] == 0x0b) {
            solved = true;
            break;
        }
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (stopToken.stop_requested()) {
        error = L"操作已取消。";
        return false;
    }
    if (!solved) {
        error = L"未能在安全迭代上限内完成 CSN 浏览器验证。";
        return false;
    }

    std::scoped_lock lock(gCsnChallengeMutex);
    gCsnChallengeToken.assign(token.begin(), token.end());
    InternetSetCookieW(
        L"https://gps.csn.uchile.cl/",
        L"challenge_token",
        gCsnChallengeToken.c_str());
    InternetSetCookieW(
        L"https://ops.csn.uchile.cl/",
        L"challenge_token",
        gCsnChallengeToken.c_str());
    return true;
}

bool QueryStatusCode(HINTERNET request, DWORD& statusCode, std::wstring& error) {
    DWORD size = sizeof(statusCode);
    if (!HttpQueryInfoW(
            request,
            HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
            &statusCode,
            &size,
            nullptr)) {
        error = L"无法读取 HTTP 状态：" + GetLastWin32ErrorMessage(GetLastError());
        return false;
    }
    return true;
}

std::uint64_t QueryContentLength(HINTERNET request) {
    wchar_t buffer[64]{};
    DWORD size = sizeof(buffer);
    if (!HttpQueryInfoW(
            request,
            HTTP_QUERY_CONTENT_LENGTH,
            buffer,
            &size,
            nullptr)) {
        return 0;
    }
    return _wcstoui64(buffer, nullptr, 10);
}

std::filesystem::path SystemCurlPath() {
    // Terras POST can fail with Schannel's SEC_E_NO_CREDENTIALS.
    // Try bundled LibreSSL curl first, then system curl.
    std::wstring executable(32'768, L'\0');
    const DWORD executableLength = GetModuleFileNameW(
        nullptr,
        executable.data(),
        static_cast<DWORD>(executable.size()));
    if (executableLength > 0 && executableLength < executable.size()) {
        executable.resize(executableLength);
        const auto portable = std::filesystem::path(executable).parent_path()
            / L"curl-libressl.exe";
        std::error_code portableError;
        if (std::filesystem::is_regular_file(portable, portableError)) {
            return portable;
        }
    }
    std::wstring directory(32'768, L'\0');
    const UINT length = GetSystemDirectoryW(
        directory.data(),
        static_cast<UINT>(directory.size()));
    if (length == 0 || length >= directory.size()) {
        return {};
    }
    directory.resize(length);
    const auto curl = std::filesystem::path(directory) / L"curl.exe";
    std::error_code error;
    return std::filesystem::is_regular_file(curl, error)
        ? curl
        : std::filesystem::path{};
}

std::filesystem::path SystemSftpPath() {
    std::wstring directory(32'768, L'\0');
    const UINT length = GetWindowsDirectoryW(
        directory.data(),
        static_cast<UINT>(directory.size()));
    if (length == 0 || length >= directory.size()) {
        return {};
    }
    directory.resize(length);
    const auto executable =
        std::filesystem::path(directory)
        / L"System32"
        / L"OpenSSH"
        / L"sftp.exe";
    std::error_code error;
    return std::filesystem::is_regular_file(executable, error)
        ? executable
        : std::filesystem::path{};
}

std::filesystem::path SiblingSocksRelayPath() {
    std::wstring executable(32'768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr,
        executable.data(),
        static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) return {};
    executable.resize(length);
    const auto candidate = std::filesystem::path(executable).parent_path()
        / L"GnssSocksRelay.exe";
    std::error_code error;
    if (!std::filesystem::is_regular_file(candidate, error)) return {};
    std::wstring shortPath(32'768, L'\0');
    const DWORD shortLength = GetShortPathNameW(
        candidate.c_str(),
        shortPath.data(),
        static_cast<DWORD>(shortPath.size()));
    if (shortLength > 0 && shortLength < shortPath.size()) {
        shortPath.resize(shortLength);
        if (shortPath.find(L' ') == std::wstring::npos
            && shortPath.find(L'"') == std::wstring::npos) {
            return std::filesystem::path(shortPath);
        }
    }
    return candidate.wstring().find(L' ') == std::wstring::npos
        && candidate.wstring().find(L'"') == std::wstring::npos
        ? candidate
        : std::filesystem::path{};
}

std::filesystem::path SiblingSftpAskPassPath() {
    std::wstring executable(32'768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr,
        executable.data(),
        static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) return {};
    executable.resize(length);
    const auto candidate = std::filesystem::path(executable).parent_path()
        / L"GnssSftpAskPass.exe";
    std::error_code error;
    return std::filesystem::is_regular_file(candidate, error)
        ? candidate
        : std::filesystem::path{};
}

std::vector<wchar_t> ChildEnvironmentWithSftpPassword(
    const std::filesystem::path& askPassPath,
    const std::wstring_view password) {
    std::vector<std::wstring> entries;
    const LPWCH environment = GetEnvironmentStringsW();
    if (environment == nullptr) return {};
    for (const wchar_t* item = environment; *item != L'\0';) {
        std::wstring entry(item);
        item += entry.size() + 1U;
        const std::size_t separator = entry.find(
            L'=', entry.starts_with(L'=') ? 1U : 0U);
        const std::wstring key = separator == std::wstring::npos
            ? entry
            : entry.substr(0, separator);
        if (_wcsicmp(key.c_str(), L"SSH_ASKPASS") == 0
            || _wcsicmp(key.c_str(), L"SSH_ASKPASS_REQUIRE") == 0
            || _wcsicmp(key.c_str(), L"DISPLAY") == 0
            || _wcsicmp(
                key.c_str(), L"GNSS_DOWNLOADER_SFTP_PASSWORD") == 0) {
            continue;
        }
        entries.push_back(std::move(entry));
    }
    FreeEnvironmentStringsW(environment);
    entries.push_back(L"SSH_ASKPASS=" + askPassPath.wstring());
    entries.push_back(L"SSH_ASKPASS_REQUIRE=force");
    entries.push_back(L"DISPLAY=GNSSDownloader");
    entries.push_back(
        L"GNSS_DOWNLOADER_SFTP_PASSWORD=" + std::wstring(password));
    std::sort(entries.begin(), entries.end(),
        [](const std::wstring& left, const std::wstring& right) {
            return _wcsicmp(left.c_str(), right.c_str()) < 0;
        });

    std::size_t total = 1U;
    for (const auto& entry : entries) total += entry.size() + 1U;
    std::vector<wchar_t> block;
    block.reserve(total);
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

std::optional<int> ConfiguredLocalSocksPort() {
    constexpr std::array<const wchar_t*, 2> names{
        L"GNSS_DOWNLOADER_PROXY", L"ALL_PROXY"};
    for (const auto* name : names) {
        const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required <= 1 || required >= 2048) continue;
        std::wstring value(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(
            name, value.data(), required);
        value.resize(written);
        constexpr std::array<std::wstring_view, 2> prefixes{
            L"socks5h://127.0.0.1:", L"socks5://127.0.0.1:"};
        for (const auto prefix : prefixes) {
            if (!value.starts_with(prefix)) continue;
            const std::wstring portText(value.substr(prefix.size()));
            if (portText.empty()
                || !std::all_of(portText.begin(), portText.end(),
                    [](const wchar_t character) {
                        return character >= L'0' && character <= L'9';
                    })) {
                continue;
            }
            try {
                const int port = std::stoi(portText);
                if (port >= 1 && port <= 65535) return port;
            } catch (...) {
            }
        }
    }
    return std::nullopt;
}

std::wstring QuoteArgument(const std::wstring& value) {
    if (value.find(L'"') != std::wstring::npos) {
        return {};
    }
    return L"\"" + value + L"\"";
}

std::wstring SafeSftpDiagnostic(const std::string& output) {
    if (output.empty()) return {};
    const std::size_t first = output.size() > 2000U
        ? output.size() - 2000U
        : 0U;
    std::wstring value = Utf8ToWide(
        std::string_view(output).substr(first));
    for (auto& c : value) {
        if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
        if (c < 0x20) c = L' ';
    }
    while (value.find(L"  ") != std::wstring::npos) {
        value.replace(value.find(L"  "), 2U, L" ");
    }
    if (value.size() > 1200U) value.erase(0, value.size() - 1200U);
    return value;
}

struct CurlProcessResult {
    bool started = false;
    bool cancelled = false;
    DWORD exitCode = static_cast<DWORD>(-1);
    std::wstring error;
};

CurlProcessResult RunCurl(
    const std::vector<std::wstring>& arguments,
    const std::filesystem::path& monitoredFile,
    const std::stop_token stopToken,
    const DownloadProgressCallback& progress,
    const std::size_t maximumBytes = 0,
    DownloadControlPtr control = nullptr) {
    CurlProcessResult result;
    const auto executable = SystemCurlPath();
    if (executable.empty()) {
        result.error = L"Windows 系统 curl.exe 不可用。";
        return result;
    }

    std::wstring commandLine = QuoteArgument(executable.wstring());
    if (_wcsicmp(executable.filename().c_str(), L"curl-libressl.exe") == 0) {
        const auto certificateBundle = executable.parent_path()
            / L"curl-ca-bundle.crt";
        std::error_code certificateError;
        if (std::filesystem::is_regular_file(
                certificateBundle, certificateError)) {
            commandLine += L" --cacert "
                + QuoteArgument(certificateBundle.wstring());
        }
    }

    const bool hasExplicitProxyOption = std::any_of(
        arguments.begin(),
        arguments.end(),
        [](const std::wstring& argument) {
            return argument == L"--proxy" || argument == L"-x"
                || argument == L"--noproxy";
        });
    if (!hasExplicitProxyOption) {
        wchar_t directValue[8]{};
        const DWORD directLength = GetEnvironmentVariableW(
            L"GNSS_DOWNLOADER_FORCE_DIRECT",
            directValue,
            static_cast<DWORD>(std::size(directValue)));
        const bool forceDirect = directLength > 0
            && (directValue[0] == L'1' || directValue[0] == L'y'
                || directValue[0] == L'Y' || directValue[0] == L't'
                || directValue[0] == L'T');
        if (forceDirect) {
            commandLine += L" --noproxy \"*\"";
        }

        std::wstring requestUrl;
        for (auto argument = arguments.rbegin();
             argument != arguments.rend(); ++argument) {
            if (argument->starts_with(L"https://")
                || argument->starts_with(L"http://")
                || argument->starts_with(L"ftp://")
                || argument->starts_with(L"ftps://")) {
                requestUrl = *argument;
                break;
            }
        }

        std::wstring externalProxy = ConfiguredProxyEnvironment();
        if (!forceDirect && externalProxy.empty() && !requestUrl.empty()) {
            externalProxy = CurrentSystemProxyForUrl(requestUrl);
            if (!externalProxy.empty()
                && externalProxy.find(L"://") == std::wstring::npos) {
                externalProxy.insert(0, L"http://");
            }
        }
        if (!forceDirect && !externalProxy.empty()
            && externalProxy.find_first_of(L"\r\n\"")
                == std::wstring::npos) {
            commandLine += L" --proxy " + QuoteArgument(externalProxy)
                + L" --noproxy \"\"";
        }
    }
    for (const auto& argument : arguments) {
        const auto quoted = QuoteArgument(argument);
        if (quoted.empty() && !argument.empty()) {
            result.error = L"curl 参数包含不允许的字符。";
            return result;
        }
        commandLine += L" " + quoted;
    }
    std::vector<wchar_t> mutableCommand(
        commandLine.begin(),
        commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    // Kill-on-close jobs terminate curl children when the parent exits,
    // releasing FTP sessions even after Ctrl+C.
    UniqueKernelHandle jobHandle(CreateJobObjectW(nullptr, nullptr));
    if (!jobHandle) {
        result.error = L"Unable to create a lifetime job for Windows curl.exe: "
            + GetLastWin32ErrorMessage(GetLastError());
        return result;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInformation{};
    jobInformation.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            static_cast<HANDLE>(jobHandle.get()),
            JobObjectExtendedLimitInformation,
            &jobInformation,
            sizeof(jobInformation))) {
        result.error = L"Unable to configure the Windows curl.exe lifetime job: "
            + GetLastWin32ErrorMessage(GetLastError());
        return result;
    }

    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            executable.c_str(),
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        result.error = L"无法启动 Windows curl.exe："
            + GetLastWin32ErrorMessage(GetLastError());
        return result;
    }
    result.started = true;
    UniqueKernelHandle processHandle(process.hProcess);
    UniqueKernelHandle threadHandle(process.hThread);

    if (!AssignProcessToJobObject(
            static_cast<HANDLE>(jobHandle.get()),
            static_cast<HANDLE>(processHandle.get()))) {
        const DWORD assignError = GetLastError();
        TerminateProcess(
            static_cast<HANDLE>(processHandle.get()),
            assignError);
        WaitForSingleObject(
            static_cast<HANDLE>(processHandle.get()),
            5'000);
        result.error = L"Unable to bind Windows curl.exe to its lifetime job: "
            + GetLastWin32ErrorMessage(assignError);
        return result;
    }
    if (ResumeThread(static_cast<HANDLE>(threadHandle.get()))
        == static_cast<DWORD>(-1)) {
        const DWORD resumeError = GetLastError();
        TerminateProcess(
            static_cast<HANDLE>(processHandle.get()),
            resumeError);
        WaitForSingleObject(
            static_cast<HANDLE>(processHandle.get()),
            5'000);
        result.error = L"Unable to start Windows curl.exe: "
            + GetLastWin32ErrorMessage(resumeError);
        return result;
    }

    bool childSuspended = false;
    while (true) {
        // RunCurl is retained for a few legacy directory/session requests
        // that still use the system curl.exe child.  Suspending its primary
        // thread gives those requests the same pause semantics as the
        // in-process libcurl path.  The child remains in the job object, so
        // stop_token cancellation still terminates the complete process tree.
        if (control != nullptr && control->isPaused()) {
            if (!childSuspended) {
                if (SuspendThread(static_cast<HANDLE>(threadHandle.get()))
                    == static_cast<DWORD>(-1)) {
                    TerminateProcess(
                        static_cast<HANDLE>(processHandle.get()),
                        ERROR_CANCELLED);
                    result.error = L"无法暂停 curl.exe 下载进程。";
                    break;
                }
                childSuspended = true;
            }
            while (control->isPaused() && !stopToken.stop_requested()) {
                if (WaitForSingleObject(
                        static_cast<HANDLE>(processHandle.get()), 0)
                    == WAIT_OBJECT_0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (childSuspended) {
                // ResumeThread is safe after process exit and clears the
                // suspension count before the handle is released.
                ResumeThread(static_cast<HANDLE>(threadHandle.get()));
                childSuspended = false;
            }
        }
        const DWORD wait = WaitForSingleObject(
            static_cast<HANDLE>(processHandle.get()),
            150);
        if (!monitoredFile.empty()) {
            std::error_code sizeError;
            const auto currentSize = std::filesystem::file_size(
                monitoredFile,
                sizeError);
            if (!sizeError) {
                if (maximumBytes > 0 && currentSize > maximumBytes) {
                    TerminateProcess(
                        static_cast<HANDLE>(processHandle.get()),
                        ERROR_FILE_TOO_LARGE);
                    result.error = L"服务器响应超过安全大小限制。";
                    break;
                }
                if (progress) {
                    progress(currentSize, 0);
                }
            }
        }
        if (stopToken.stop_requested()) {
            TerminateProcess(
                static_cast<HANDLE>(processHandle.get()),
                ERROR_CANCELLED);
            result.cancelled = true;
            result.error = L"操作已取消。";
            break;
        }
        if (wait == WAIT_OBJECT_0) {
            GetExitCodeProcess(
                static_cast<HANDLE>(processHandle.get()),
                &result.exitCode);
            break;
        }
        if (wait == WAIT_FAILED) {
            result.error = L"等待 curl.exe 失败："
                + GetLastWin32ErrorMessage(GetLastError());
            break;
        }
    }
    return result;
}

std::filesystem::path CreateCurlTemporaryPath() {
    std::wstring directory(32'768, L'\0');
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(directory.size()),
        directory.data());
    if (length == 0 || length >= directory.size()) {
        return {};
    }
    directory.resize(length);
    const auto folder = std::filesystem::path(directory) / L"IGSDownloader";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        return {};
    }
    static std::atomic_uint64_t counter{0};
    return folder / std::format(
        L"http-{}-{}.tmp",
        GetCurrentProcessId(),
        counter.fetch_add(1, std::memory_order_relaxed));
}

struct RgnaCurlSession {
    std::mutex mutex;
    std::filesystem::path cookiePath;
    std::string initialBody;

    ~RgnaCurlSession() {
        std::error_code error;
        if (!cookiePath.empty()) {
            std::filesystem::remove(cookiePath, error);
        }
    }
};

RgnaCurlSession gRgnaCurlSession;

bool IsSafePanamaRemotePattern(const std::wstring_view value) {
    constexpr std::wstring_view prefix = L"RefData.";
    if (!value.starts_with(prefix)
        || value.size() > 180
        || value.find(L"..") != std::wstring_view::npos) {
        return false;
    }
    return std::all_of(
        value.begin(),
        value.end(),
        [](const wchar_t character) {
            return (character >= L'a' && character <= L'z')
                || (character >= L'A' && character <= L'Z')
                || (character >= L'0' && character <= L'9')
                || character == L'/'
                || character == L'_'
                || character == L'-'
                || character == L'.'
                || character == L'*';
        });
}

bool IsSafePanamaRemoteFile(const std::wstring_view value) {
    return IsSafePanamaRemotePattern(value)
        && value.find(L'*') == std::wstring_view::npos
        && value.ends_with(L".zip");
}

constexpr std::wstring_view kPanamaSftpUrlPrefix =
    L"sftp://corsigntg@cors.anati.gob.pa/";

constexpr std::wstring_view kRegnaRouSftpUrlPrefix =
    L"sftp://regna@sftp.igm.gub.uy:2222/";

constexpr std::wstring_view kRgnaMexicoSftpUrlPrefix =
    L"sftp://rgnasftp@geodesia2.inegi.org.mx/";

bool IsPanamaSftpUrl(
    const std::wstring_view url,
    std::wstring& remotePath) {
    if (!url.starts_with(kPanamaSftpUrlPrefix)) {
        return false;
    }
    remotePath.assign(url.substr(kPanamaSftpUrlPrefix.size()));
    return IsSafePanamaRemoteFile(remotePath);
}

bool IsSafeRegnaRouRemotePattern(const std::wstring_view value) {
    if (!value.starts_with(L"sftpserver/")
        || value.size() > 220
        || value.find(L"..") != std::wstring_view::npos) {
        return false;
    }
    return std::all_of(
        value.begin(),
        value.end(),
        [](const wchar_t character) {
            return (character >= L'a' && character <= L'z')
                || (character >= L'A' && character <= L'Z')
                || (character >= L'0' && character <= L'9')
                || character == L'/'
                || character == L'_'
                || character == L'-'
                || character == L'.'
                || character == L'*';
        });
}

bool IsSafeRegnaRouRemoteFile(const std::wstring_view value) {
    return IsSafeRegnaRouRemotePattern(value)
        && value.find(L'*') == std::wstring_view::npos
        && value.ends_with(L".zip");
}

bool IsRegnaRouSftpUrl(
    const std::wstring_view url,
    std::wstring& remotePath) {
    if (!url.starts_with(kRegnaRouSftpUrlPrefix)) {
        return false;
    }
    remotePath.assign(url.substr(kRegnaRouSftpUrlPrefix.size()));
    return IsSafeRegnaRouRemoteFile(remotePath);
}

bool IsSafeRgnaMexicoRemoteFile(const std::wstring_view value) {
    // The public account starts in /home/rgna.  Keep download routes limited
    // to its documented YYYY/DDD compact daily observation files.
    if (value.size() < 20 || value.size() > 160
        || value.find(L"..") != std::wstring_view::npos
        || value[4] != L'/' || value[8] != L'/') {
        return false;
    }
    if (!std::all_of(value.begin(), value.begin() + 4, ::iswdigit)
        || !std::all_of(value.begin() + 5, value.begin() + 8, ::iswdigit)) {
        return false;
    }
    const auto name = value.substr(9);
    if (name.size() < 12 || !name.ends_with(L"d.gz")) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const wchar_t c) {
        return (c >= L'a' && c <= L'z')
            || (c >= L'A' && c <= L'Z')
            || (c >= L'0' && c <= L'9')
            || c == L'/' || c == L'.' || c == L'_' || c == L'-';
    });
}

bool IsRgnaMexicoSftpUrl(
    const std::wstring_view url,
    std::wstring& remotePath) {
    if (!url.starts_with(kRgnaMexicoSftpUrlPrefix)) {
        return false;
    }
    remotePath.assign(url.substr(kRgnaMexicoSftpUrlPrefix.size()));
    return IsSafeRgnaMexicoRemoteFile(remotePath);
}

struct SftpProcessResult {
    bool started = false;
    bool cancelled = false;
    bool inactivityTimedOut = false;
    DWORD exitCode = static_cast<DWORD>(-1);
    std::string output;
    std::wstring error;
};

bool IsSafeTerrasUsername(const std::wstring_view value) {
    if (value.empty() || value.size() > 128 || value.front() == L'-') {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const wchar_t c) {
        return (c >= L'a' && c <= L'z')
            || (c >= L'A' && c <= L'Z')
            || (c >= L'0' && c <= L'9')
            || c == L'.' || c == L'_' || c == L'-';
    });
}

bool IsSafeTerrasRemoteDirectory(const std::wstring_view value) {
    constexpr std::array<std::wstring_view, 2> prefixes{
        L"/data/GRJE_3.02/",
        L"/data/GRJE_4.01/"
    };
    const auto prefix = std::find_if(
        prefixes.begin(), prefixes.end(),
        [&](const std::wstring_view candidate) {
            return value.starts_with(candidate);
        });
    if (prefix == prefixes.end()
        || value.size() != prefix->size() + 8U
        || value.find(L"..") != std::wstring_view::npos) {
        return false;
    }
    const auto year = value.substr(prefix->size(), 4U);
    const auto slash = value[prefix->size() + 4U];
    const auto doy = value.substr(prefix->size() + 5U, 3U);
    const auto digits = [](const std::wstring_view text) {
        return std::all_of(text.begin(), text.end(), [](const wchar_t c) {
            return c >= L'0' && c <= L'9';
        });
    };
    if (slash != L'/' || !digits(year) || !digits(doy)) return false;
    const int numericYear = std::stoi(std::wstring(year));
    const int numericDoy = std::stoi(std::wstring(doy));
    return numericYear >= 1990 && numericYear <= 2200
        && numericDoy >= 1 && numericDoy <= 366;
}

bool IsSafeTerrasRemoteFile(const std::wstring_view value) {
    const auto slash = value.find_last_of(L'/');
    if (slash == std::wstring_view::npos
        || !IsSafeTerrasRemoteDirectory(value.substr(0, slash))) {
        return false;
    }
    const auto name = value.substr(slash + 1U);
    return !name.empty() && name.size() <= 128
        && name.find(L"..") == std::wstring_view::npos
        && std::all_of(name.begin(), name.end(), [](const wchar_t c) {
            return (c >= L'a' && c <= L'z')
                || (c >= L'A' && c <= L'Z')
                || (c >= L'0' && c <= L'9')
                || c == L'.' || c == L'_' || c == L'-';
        });
}

void SecureClear(std::wstring& value) {
    if (!value.empty()) {
        SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
        value.clear();
    }
}

struct TerrasCredentialState {
    std::mutex mutex;
    std::wstring username;
    std::wstring password;

    ~TerrasCredentialState() {
        SecureClear(username);
        SecureClear(password);
    }
};

TerrasCredentialState gTerrasCredentials;

SftpProcessResult RunKnownSftp(
    const std::vector<std::wstring>& commands,
    const std::filesystem::path& monitoredFile,
    const std::stop_token stopToken,
    const DownloadProgressCallback& progress,
    const std::size_t maximumOutputBytes,
    const std::wstring_view loginHost,
    const std::wstring_view password,
    const int port,
    const std::wstring_view serviceLabel,
    DownloadControlPtr control = nullptr) {
    SftpProcessResult result;
    const auto executable = SystemSftpPath();
    if (executable.empty()) {
        result.error =
            L"Windows OpenSSH sftp.exe 不可用；请在“可选功能”中安装 OpenSSH 客户端。";
        return result;
    }
    const auto askPassExecutable = SiblingSftpAskPassPath();
    if (askPassExecutable.empty()) {
        result.error = L"缺少 GnssSftpAskPass.exe，无法安全输入 SFTP 密码。";
        return result;
    }
    if (commands.empty()
        || !std::all_of(
            commands.begin(),
            commands.end(),
            [](const std::wstring& command) {
                return !command.empty()
                    && command.size() <= 700
                    && command.find(L'\r') == std::wstring::npos
                    && command.find(L'\n') == std::wstring::npos;
            })) {
        result.error = std::wstring(serviceLabel) + L" SFTP 命令无效。";
        return result;
    }

    const auto temporary = CreateCurlTemporaryPath();
    if (temporary.empty()) {
        result.error = L"无法创建 " + std::wstring(serviceLabel)
            + L" SFTP 临时文件。";
        return result;
    }
    const auto batchPath =
        std::filesystem::path(temporary.wstring() + L"-batch.txt");
    const auto outputPath =
        std::filesystem::path(temporary.wstring() + L"-output.txt");
    const auto cleanup = [&]() {
        std::error_code error;
        std::filesystem::remove(batchPath, error);
        std::filesystem::remove(outputPath, error);
    };
    cleanup();

    {
        std::ofstream batch(batchPath, std::ios::binary | std::ios::trunc);
        if (!batch) {
            result.error = L"无法写入 " + std::wstring(serviceLabel)
                + L" SFTP 临时文件。";
            cleanup();
            return result;
        }
        for (const auto& command : commands) {
            std::string narrow = WideToUtf8(command);
            if (narrow.empty()) {
                result.error = std::wstring(serviceLabel)
                    + L" SFTP 命令无法转换为 UTF-8。";
                cleanup();
                return result;
            }
            // A leading '-' is OpenSSH batch syntax meaning "continue after
            // this command fails".  We feed commands through stdin instead
            // of -b so password authentication remains available; ordinary
            // interactive SFTP already continues after a failed ls.
            if (narrow.starts_with('-')) narrow.erase(narrow.begin());
            batch << narrow << "\r\n";
        }
        batch << "quit\r\n";
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    UniqueKernelHandle outputHandle(CreateFileW(
        outputPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr));
    UniqueKernelHandle commandInput(CreateFileW(
        batchPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr));
    if (!outputHandle || !commandInput) {
        result.error = L"无法创建 " + std::wstring(serviceLabel)
            + L" SFTP 输出管道。";
        cleanup();
        return result;
    }

    std::wstring commandLine = QuoteArgument(executable.wstring())
        + L" -o \"BatchMode=no\""
        + L" -o \"PreferredAuthentications=keyboard-interactive,password\""
        + L" -o \"PubkeyAuthentication=no\""
        + L" -o \"ConnectTimeout=15\""
        + L" -o \"ConnectionAttempts=1\""
        + L" -o \"StrictHostKeyChecking=accept-new\""
        + (serviceLabel == L"GEONET/Terras"
            ? L" -o \"LogLevel=VERBOSE\""
            : L" -o \"LogLevel=ERROR\"")
        + (port > 0 ? L" -P " + std::to_wstring(port) : std::wstring{});
    if (const auto proxyPort = ConfiguredLocalSocksPort()) {
        const auto relay = SiblingSocksRelayPath();
        if (!relay.empty()) {
            const std::wstring proxyCommand = L"ProxyCommand="
                + relay.wstring() + L" %h %p "
                + std::to_wstring(*proxyPort);
            commandLine += L" -o " + QuoteArgument(proxyCommand);
        }
    }
    commandLine += L" " + QuoteArgument(std::wstring(loginHost));
    std::vector<wchar_t> mutableCommand(
        commandLine.begin(),
        commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = static_cast<HANDLE>(commandInput.get());
    startup.hStdOutput = static_cast<HANDLE>(outputHandle.get());
    startup.hStdError = static_cast<HANDLE>(outputHandle.get());

    // Keep sftp.exe and its ProxyCommand relay in one kill-on-close job.
    // Closing the builder with Ctrl+C otherwise terminates only the parent
    // process and can leave both children consuming a Terras connection.
    UniqueKernelHandle jobHandle(CreateJobObjectW(nullptr, nullptr));
    if (!jobHandle) {
        result.error = L"无法创建 " + std::wstring(serviceLabel)
            + L" SFTP 子进程生命周期控制器："
            + GetLastWin32ErrorMessage(GetLastError());
        cleanup();
        return result;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInformation{};
    jobInformation.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            static_cast<HANDLE>(jobHandle.get()),
            JobObjectExtendedLimitInformation,
            &jobInformation,
            sizeof(jobInformation))) {
        result.error = L"无法配置 " + std::wstring(serviceLabel)
            + L" SFTP 子进程生命周期控制器："
            + GetLastWin32ErrorMessage(GetLastError());
        cleanup();
        return result;
    }
    PROCESS_INFORMATION process{};

    auto childEnvironment = ChildEnvironmentWithSftpPassword(
        askPassExecutable, password);
    if (childEnvironment.empty()) {
        result.error = L"无法创建安全的 SFTP 子进程环境。";
        cleanup();
        return result;
    }
    const BOOL created = CreateProcessW(
        executable.c_str(),
        mutableCommand.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
        childEnvironment.data(),
        nullptr,
        &startup,
        &process);
    SecureZeroMemory(
        childEnvironment.data(),
        childEnvironment.size() * sizeof(wchar_t));
    childEnvironment.clear();
    outputHandle.reset();
    commandInput.reset();

    if (!created) {
        result.error = L"无法启动 Windows sftp.exe："
            + GetLastWin32ErrorMessage(GetLastError());
        cleanup();
        return result;
    }
    result.started = true;
    UniqueKernelHandle processHandle(process.hProcess);
    UniqueKernelHandle threadHandle(process.hThread);
    if (!AssignProcessToJobObject(
            static_cast<HANDLE>(jobHandle.get()),
            static_cast<HANDLE>(processHandle.get()))) {
        const DWORD assignError = GetLastError();
        TerminateProcess(
            static_cast<HANDLE>(processHandle.get()), assignError);
        WaitForSingleObject(
            static_cast<HANDLE>(processHandle.get()), 5'000);
        result.error = L"无法绑定 " + std::wstring(serviceLabel)
            + L" SFTP 子进程生命周期："
            + GetLastWin32ErrorMessage(assignError);
        cleanup();
        return result;
    }
    if (ResumeThread(static_cast<HANDLE>(threadHandle.get()))
        == static_cast<DWORD>(-1)) {
        const DWORD resumeError = GetLastError();
        TerminateJobObject(
            static_cast<HANDLE>(jobHandle.get()), resumeError);
        WaitForSingleObject(
            static_cast<HANDLE>(processHandle.get()), 5'000);
        result.error = L"无法启动 " + std::wstring(serviceLabel)
            + L" SFTP 子进程："
            + GetLastWin32ErrorMessage(resumeError);
        cleanup();
        return result;
    }
    // IGM Uruguay's SFTP has high per-directory latency.  REGNA-ROU lists a
    // whole month in one authenticated session so the cost is amortized by
    // every day in that month; allow that bounded batch to finish.
    const auto timeout = (serviceLabel == L"REGNA-ROU"
            || serviceLabel == L"RGNA-MX")
        ? std::chrono::seconds(240)
        : (serviceLabel == L"GEONET/Terras"
            ? std::chrono::seconds(1800)
            : std::chrono::seconds(90));
    auto hardDeadline = std::chrono::steady_clock::now() + timeout;
    const bool monitorTerrasDirectoryFlow =
        serviceLabel == L"GEONET/Terras" && monitoredFile.empty();
    auto lastOutputProgress = std::chrono::steady_clock::now();
    std::uintmax_t lastOutputSize = 0;
    bool childSuspended = false;
    while (true) {
        // OpenSSH sftp.exe is an external child, so a condition-variable wait
        // alone would allow the child to keep transferring data.  Suspend its
        // primary thread while paused, and remove the paused duration from
        // both watchdog clocks when it resumes.
        if (control != nullptr && control->isPaused()) {
            const auto pauseStarted = std::chrono::steady_clock::now();
            if (!childSuspended) {
                if (SuspendThread(static_cast<HANDLE>(threadHandle.get()))
                    == static_cast<DWORD>(-1)) {
                    TerminateJobObject(
                        static_cast<HANDLE>(jobHandle.get()),
                        ERROR_CANCELLED);
                    result.error = L"无法暂停 sftp.exe 下载进程。";
                    break;
                }
                childSuspended = true;
            }
            while (control->isPaused() && !stopToken.stop_requested()) {
                if (WaitForSingleObject(
                        static_cast<HANDLE>(processHandle.get()), 0)
                    == WAIT_OBJECT_0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (childSuspended) {
                ResumeThread(static_cast<HANDLE>(threadHandle.get()));
                childSuspended = false;
            }
            const auto pausedDuration =
                std::chrono::steady_clock::now() - pauseStarted;
            hardDeadline += pausedDuration;
            lastOutputProgress += pausedDuration;
        }
        const DWORD wait = WaitForSingleObject(
            static_cast<HANDLE>(processHandle.get()),
            150);
        if (!monitoredFile.empty()) {
            std::error_code sizeError;
            const auto size = std::filesystem::file_size(
                monitoredFile,
                sizeError);
            if (!sizeError && progress
                && WaitForDownloadControl(control, stopToken)) {
                progress(size, 0);
            }
        }
        if (stopToken.stop_requested()) {
            TerminateJobObject(
                static_cast<HANDLE>(jobHandle.get()),
                ERROR_CANCELLED);
            result.cancelled = true;
            result.error = L"操作已取消。";
            break;
        }
        if (wait == WAIT_OBJECT_0) {
            GetExitCodeProcess(
                static_cast<HANDLE>(processHandle.get()),
                &result.exitCode);
            break;
        }
        if (wait == WAIT_FAILED) {
            const DWORD waitError = GetLastError();
            TerminateJobObject(
                static_cast<HANDLE>(jobHandle.get()),
                waitError);
            result.error = L"等待 sftp.exe 失败："
                + GetLastWin32ErrorMessage(waitError);
            break;
        }
        std::error_code sizeError;
        const auto outputSize = std::filesystem::file_size(
            outputPath,
            sizeError);
        if (!sizeError && outputSize > lastOutputSize) {
            lastOutputSize = outputSize;
            lastOutputProgress = std::chrono::steady_clock::now();
        }
        if (monitorTerrasDirectoryFlow
            && std::chrono::steady_clock::now() - lastOutputProgress
                >= std::chrono::seconds(45)) {
            TerminateJobObject(
                static_cast<HANDLE>(jobHandle.get()),
                ERROR_TIMEOUT);
            result.exitCode = ERROR_TIMEOUT;
            result.inactivityTimedOut = true;
            result.error = L"GEONET/Terras SFTP 目录传输连续45秒没有新数据，"
                           L"已终止该半开连接。";
            break;
        }
        if (!sizeError && maximumOutputBytes > 0
            && outputSize > maximumOutputBytes) {
            TerminateJobObject(
                static_cast<HANDLE>(jobHandle.get()),
                ERROR_FILE_TOO_LARGE);
            result.error = L"SFTP 输出超过安全大小限制。";
            break;
        }
        if (std::chrono::steady_clock::now() >= hardDeadline) {
            TerminateJobObject(
                static_cast<HANDLE>(jobHandle.get()),
                ERROR_TIMEOUT);
            result.exitCode = ERROR_TIMEOUT;
            result.error = std::wstring(serviceLabel)
                + L" SFTP 操作超过 "
                + std::to_wstring(timeout.count())
                + L" 秒，已安全终止。";
            break;
        }
    }

    // Ensure the terminated process tree has released the output file before
    // reading diagnostics or starting a replacement SFTP session.
    WaitForSingleObject(
        static_cast<HANDLE>(processHandle.get()), 5'000);

    std::ifstream output(outputPath, std::ios::binary);
    result.output.assign(
        std::istreambuf_iterator<char>(output),
        std::istreambuf_iterator<char>());
    output.close();
    if (maximumOutputBytes > 0
        && result.output.size() > maximumOutputBytes) {
        result.output.clear();
        result.error = L"SFTP 输出超过安全大小限制。";
    }
    cleanup();
    return result;
}

SftpProcessResult RunPanamaSftp(
    const std::vector<std::wstring>& commands,
    const std::filesystem::path& monitoredFile,
    const std::stop_token stopToken,
    const DownloadProgressCallback& progress,
    const std::size_t maximumOutputBytes,
    DownloadControlPtr control = nullptr) {
    return RunKnownSftp(
        commands,
        monitoredFile,
        stopToken,
        progress,
        maximumOutputBytes,
        L"corsigntg@cors.anati.gob.pa",
        L"corsigntg",
        0,
        L"巴拿马",
        std::move(control));
}

SftpProcessResult RunRegnaRouSftp(
    const std::vector<std::wstring>& commands,
    const std::filesystem::path& monitoredFile,
    const std::stop_token stopToken,
    const DownloadProgressCallback& progress,
    const std::size_t maximumOutputBytes,
    DownloadControlPtr control = nullptr) {
    return RunKnownSftp(
        commands,
        monitoredFile,
        stopToken,
        progress,
        maximumOutputBytes,
        L"regna@sftp.igm.gub.uy",
        L"historico",
        2222,
        L"REGNA-ROU",
        std::move(control));
}

SftpProcessResult RunRgnaMexicoSftp(
    const std::vector<std::wstring>& commands,
    const std::filesystem::path& monitoredFile,
    const std::stop_token stopToken,
    const DownloadProgressCallback& progress,
    const std::size_t maximumOutputBytes,
    DownloadControlPtr control = nullptr) {
    return RunKnownSftp(
        commands,
        monitoredFile,
        stopToken,
        progress,
        maximumOutputBytes,
        L"rgnasftp@geodesia2.inegi.org.mx",
        L"rgnasftp",
        22,
        L"RGNA-MX",
        std::move(control));
}

SftpProcessResult RunTerrasSftp(
    const std::vector<std::wstring>& commands,
    const std::filesystem::path& monitoredFile,
    const std::stop_token stopToken,
    const DownloadProgressCallback& progress,
    const std::size_t maximumOutputBytes,
    const std::wstring_view username,
    const std::wstring_view password,
    DownloadControlPtr control = nullptr) {
    if (!IsSafeTerrasUsername(username) || password.empty()) {
        SftpProcessResult result;
        result.error = L"GEONET/Terras SFTP 用户名或密码为空/无效。";
        return result;
    }
    std::wstring permitError;
    TerrasNamedPermit permit(
        stopToken,
        permitError,
        kTerrasSftpSemaphoreName,
        1);
    if (!permit.acquired()) {
        SftpProcessResult result;
        result.cancelled = permit.cancelled();
        result.error = std::move(permitError);
        return result;
    }
    return RunKnownSftp(
        commands,
        monitoredFile,
        stopToken,
        progress,
        maximumOutputBytes,
        std::wstring(username) + L"@terras.gsi.go.jp",
        password,
        22,
        L"GEONET/Terras",
        std::move(control));
}

HttpTextResult CurlGetText(
    const std::wstring& url,
    const std::stop_token stopToken,
    const std::size_t maximumBytes,
    const std::wstring_view allowedProtocols = L"=https") {
    HttpTextResult result;
    const bool highrateHttp =
        (url.starts_with(L"https://") || url.starts_with(L"http://"))
        && (url.find(L"/highrate/") != std::wstring::npos
            || url.find(L"/highrate_v3/") != std::wstring::npos);
    if (highrateHttp) {
        // Keep the real HTTP status. The old curl.exe --fail path returned
        // only exit code 22, making a missing day (404) indistinguishable
        // from access denial (403) or server throttling (429).
        if (!EnsureLibCurl(result.error)) return result;
        CurlTransferContext context;
        context.memory = &result.body;
        context.maximumBytes = maximumBytes;
        context.stopToken = stopToken;
        UniqueCurlEasy easy(curl_easy_init());
        if (!easy) {
            result.error = L"无法创建高频目录读取会话。";
            return result;
        }
        ConfigureLibCurlCommon(
            easy.get(), url, allowedProtocols == L"=http" ? "http,https" : "https",
            context);
        curl_easy_setopt(easy.get(), CURLOPT_CONNECTTIMEOUT, 8L);
        curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT, 20L);
        std::array<char, CURL_ERROR_SIZE> errorBuffer{};
        curl_easy_setopt(easy.get(), CURLOPT_ERRORBUFFER, errorBuffer.data());
        const CURLcode code = curl_easy_perform(easy.get());
        long status = 0;
        curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status);
        result.statusCode = static_cast<unsigned long>(std::max(0L, status));
        if (code != CURLE_OK) {
            result.transportErrorCode = code == CURLE_HTTP_RETURNED_ERROR
                ? 0 : static_cast<long>(code);
            result.error = Utf8ToWide(errorBuffer[0] != '\0'
                ? std::string_view(errorBuffer.data())
                : std::string_view(curl_easy_strerror(code)));
            result.body.clear();
        }
        return result;
    }
    const auto temporary = CreateCurlTemporaryPath();
    if (temporary.empty()) {
        result.error = L"无法创建网络响应临时文件。";
        return result;
    }
    std::error_code fileError;
    std::filesystem::remove(temporary, fileError);

    // High-rate directory probes are issued in batches (up to twelve per
    // source and date).  Keeping the generic FTP timeout of two minutes for
    // these tiny listings makes a dead mirror stall an entire historical
    // backfill for hours.  Detect the high-rate trees and use a short,
    // bounded probe timeout; ordinary downloader/indexer FTP requests keep
    // their existing conservative settings below.
    const bool highrateListing =
        url.find(L"/highrate/") != std::wstring::npos
        || url.find(L"/highrate_v3/") != std::wstring::npos;
    const wchar_t* connectTimeout = highrateListing ? L"8" : L"15";
    const wchar_t* maxTime = highrateListing ? L"20" : L"120";
    std::vector<std::wstring> arguments{
        L"--fail",
        IsIersDataCenterUrl(url) ? L"-L" : L"--location",
        L"--silent",
        L"--show-error",
        L"--ssl-revoke-best-effort",
        L"--connect-timeout",
        connectTimeout,
        L"--max-time",
        maxTime,
        L"--proto",
        std::wstring(allowedProtocols)
    };
    if ((IsGaPublicS3Url(url) && IsAllowedPublicHttpUrl(url))
        || IsIersDataCenterUrl(url)) {
        arguments.push_back(L"--noproxy");
        arguments.push_back(L"*");
    }
    arguments.push_back(L"--output");
    arguments.push_back(temporary.wstring());
    arguments.push_back(url);
    const auto process = RunCurl(
        arguments,
        temporary,
        stopToken,
        {},
        maximumBytes);
    if (process.exitCode != 0) {
        result.transportErrorCode = static_cast<long>(process.exitCode);
        result.error = process.error.empty()
            ? L"Windows curl.exe 返回错误代码 "
                + std::to_wstring(process.exitCode)
                + L"。"
            : process.error;
        std::filesystem::remove(temporary, fileError);
        return result;
    }

    std::ifstream input(temporary, std::ios::binary);
    if (!input) {
        result.error = L"无法读取网络响应临时文件。";
        std::filesystem::remove(temporary, fileError);
        return result;
    }
    result.body.assign(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
    input.close();
    std::filesystem::remove(temporary, fileError);
    if (result.body.size() > maximumBytes) {
        result.body.clear();
        result.error = L"服务器响应超过安全大小限制。";
        return result;
    }
    result.statusCode = 200;
    return result;
}

HttpTextResult CurlGetRgnaSessionText(
    const std::wstring& url,
    const bool initializeSession,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    HttpTextResult result;
    std::filesystem::path cookiePath;
    if (initializeSession) {
        std::scoped_lock lock(gRgnaCurlSession.mutex);
        if (!gRgnaCurlSession.initialBody.empty()
            && !gRgnaCurlSession.cookiePath.empty()) {
            result.body = gRgnaCurlSession.initialBody;
            result.statusCode = 200;
            return result;
        }
        if (gRgnaCurlSession.cookiePath.empty()) {
            const auto temporary = CreateCurlTemporaryPath();
            if (temporary.empty()) {
                result.error = L"无法创建 RGNA 会话临时文件。";
                return result;
            }
            gRgnaCurlSession.cookiePath =
                std::filesystem::path(temporary.wstring() + L".cookie");
        }
        cookiePath = gRgnaCurlSession.cookiePath;
    } else {
        std::scoped_lock lock(gRgnaCurlSession.mutex);
        if (gRgnaCurlSession.cookiePath.empty()
            || gRgnaCurlSession.initialBody.empty()) {
            result.error = L"RGNA 会话尚未初始化。";
            return result;
        }
        cookiePath = gRgnaCurlSession.cookiePath;
    }

    const auto responsePath = CreateCurlTemporaryPath();
    if (responsePath.empty()) {
        result.error = L"无法创建 RGNA 响应临时文件。";
        return result;
    }
    std::error_code fileError;
    std::filesystem::remove(responsePath, fileError);
    std::vector<std::wstring> arguments{
        L"--fail",
        L"--location",
        L"--silent",
        L"--show-error",
        // INEGI's TLS chain intermittently makes Schannel's revocation
        // provider return SEC_E_NO_CREDENTIALS through SOCKS. Certificate
        // validation remains enabled; only the Windows revocation lookup is
        // disabled for this exact host-scoped RGNA request.
        L"--ssl-no-revoke",
        L"--connect-timeout",
        L"10",
        L"--max-time",
        L"90",
        L"--proto",
        L"=https"
    };
    // SsrWatchdog configures ALL_PROXY for the builder.  Pass the value
    // explicitly here because some Windows curl builds do not consistently
    // honor a SOCKS URL from the inherited environment when a cookie jar is
    // also enabled.
    {
        const wchar_t* proxyVariable =
            GetEnvironmentVariableW(L"GNSS_DOWNLOADER_PROXY", nullptr, 0) > 1
            ? L"GNSS_DOWNLOADER_PROXY"
            : L"ALL_PROXY";
        const DWORD required = GetEnvironmentVariableW(
            proxyVariable, nullptr, 0);
        if (required > 1 && required < 2048) {
            std::wstring proxy(required, L'\0');
            const DWORD written = GetEnvironmentVariableW(
                proxyVariable, proxy.data(), required);
            proxy.resize(written);
            if (proxy.starts_with(L"socks5h://127.0.0.1:")
                || proxy.starts_with(L"socks5://127.0.0.1:")
                || proxy.starts_with(L"http://127.0.0.1:")
                || proxy.starts_with(L"https://127.0.0.1:")) {
                arguments.push_back(L"--proxy");
                arguments.push_back(std::move(proxy));
            }
        }
    }
    arguments.push_back(initializeSession ? L"--cookie-jar" : L"--cookie");
    arguments.push_back(cookiePath.wstring());
    arguments.push_back(L"--output");
    arguments.push_back(responsePath.wstring());
    arguments.push_back(url);
    const auto process = RunCurl(
        arguments,
        responsePath,
        stopToken,
        {},
        maximumBytes);
    if (process.exitCode != 0) {
        result.transportErrorCode = static_cast<long>(process.exitCode);
        result.error = process.error.empty()
            ? L"Windows curl.exe 读取 RGNA 会话失败，错误代码 "
                + std::to_wstring(process.exitCode) + L"。"
            : process.error;
        std::filesystem::remove(responsePath, fileError);
        return result;
    }
    std::ifstream input(responsePath, std::ios::binary);
    result.body.assign(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
    input.close();
    std::filesystem::remove(responsePath, fileError);
    if (result.body.size() > maximumBytes) {
        result.body.clear();
        result.error = L"RGNA 服务器响应超过安全大小限制。";
        return result;
    }
    if (result.body.empty()) {
        result.error = L"RGNA 官方服务返回空响应（会话可能已失效）。";
        return result;
    }
    result.statusCode = 200;
    if (initializeSession) {
        std::scoped_lock lock(gRgnaCurlSession.mutex);
        gRgnaCurlSession.initialBody = result.body;
    }
    return result;
}

HttpTextResult CurlGetByteRange(
    const std::wstring& url,
    const std::wstring& byteRange,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    HttpTextResult result;
    if (byteRange.empty()
        || !std::all_of(
            byteRange.begin(), byteRange.end(),
            [](const wchar_t value) {
                return (value >= L'0' && value <= L'9') || value == L'-';
            })
        || std::count(byteRange.begin(), byteRange.end(), L'-') != 1) {
        result.error = L"HTTP 字节范围无效。";
        return result;
    }
    const auto temporary = CreateCurlTemporaryPath();
    if (temporary.empty()) {
        result.error = L"无法创建网络响应临时文件。";
        return result;
    }
    std::error_code fileError;
    std::filesystem::remove(temporary, fileError);
    const std::vector<std::wstring> arguments{
        L"--fail",
        L"--location",
        L"--silent",
        L"--show-error",
        L"--ssl-revoke-best-effort",
        L"--connect-timeout",
        L"10",
        L"--max-time",
        L"120",
        L"--proto",
        L"=https",
        L"--range",
        byteRange,
        L"--output",
        temporary.wstring(),
        url
    };
    const auto process = RunCurl(
        arguments, temporary, stopToken, {}, maximumBytes);
    if (process.exitCode != 0) {
        result.transportErrorCode = static_cast<long>(process.exitCode);
        result.error = process.error.empty()
            ? L"Windows curl.exe 字节范围请求失败，错误代码 "
                + std::to_wstring(process.exitCode) + L"。"
            : process.error;
        std::filesystem::remove(temporary, fileError);
        return result;
    }
    std::ifstream input(temporary, std::ios::binary);
    if (!input) {
        result.error = L"无法读取网络响应临时文件。";
        std::filesystem::remove(temporary, fileError);
        return result;
    }
    result.body.assign(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
    input.close();
    std::filesystem::remove(temporary, fileError);
    if (result.body.size() > maximumBytes) {
        result.body.clear();
        result.error = L"服务器字节范围响应超过安全大小限制。";
        return result;
    }
    result.statusCode = 206;
    return result;
}

HttpTextResult CurlGetCsnText(
    const std::wstring& url,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    HttpTextResult result;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto temporary = CreateCurlTemporaryPath();
        if (temporary.empty()) {
            result.error = L"无法创建 CSN 网络响应临时文件。";
            return result;
        }
        std::error_code fileError;
        std::filesystem::remove(temporary, fileError);

        std::vector<std::wstring> arguments{
            L"--fail-with-body",
            L"--location",
            L"--silent",
            L"--show-error",
            L"--user-agent",
            L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
            L"AppleWebKit/537.36 (KHTML, like Gecko) "
            L"Chrome/138.0.0.0 Safari/537.36",
            L"--ssl-revoke-best-effort",
            L"--connect-timeout",
            L"10",
            L"--max-time",
            L"120",
            L"--proto",
            L"=https"
        };
        {
            std::scoped_lock lock(gCsnChallengeMutex);
            if (!gCsnChallengeToken.empty()) {
                arguments.push_back(L"--header");
                arguments.push_back(
                    L"Cookie: challenge_token=" + gCsnChallengeToken);
            }
        }
        arguments.push_back(L"--output");
        arguments.push_back(temporary.wstring());
        arguments.push_back(url);
        const auto process = RunCurl(
            arguments,
            temporary,
            stopToken,
            {},
            maximumBytes);

        std::ifstream input(temporary, std::ios::binary);
        result.body.assign(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
        input.close();
        std::filesystem::remove(temporary, fileError);
        if (result.body.size() > maximumBytes) {
            result.body.clear();
            result.error = L"CSN 服务器响应超过安全大小限制。";
            return result;
        }
        if (result.body.find("Browser Verification") != std::string::npos
            && attempt == 0) {
            if (!SolveCsnChallenge(result.body, stopToken, result.error)) {
                result.body.clear();
                return result;
            }
            result.body.clear();
            continue;
        }
        // CSN's nginx rate limiter currently redirects blocked clients to
        // the literal location "=503", which curl reports as a redirect loop
        // (exit code 47).  Treat it like HTTP 429 so the CSN adapter applies
        // its cooldown instead of restarting the user's proxy.
        if (process.exitCode == 47) {
            result.transportErrorCode = 0;
            result.statusCode = 429;
            result.body.clear();
            result.error = L"CSN rate limit is active (redirect-loop guard).";
            return result;
        }
        if (process.exitCode != 0) {
            result.transportErrorCode = static_cast<long>(process.exitCode);
            result.body.clear();
            result.error = process.error.empty()
                ? L"Windows curl.exe 读取 CSN 失败，错误代码 "
                    + std::to_wstring(process.exitCode)
                    + L"。"
                : process.error;
            return result;
        }
        result.statusCode = 200;
        return result;
    }
    result.body.clear();
    result.error = L"CSN 浏览器验证失败。";
    return result;
}

HttpTextResult CurlPostFormText(
    const std::wstring& url,
    const std::string& formBody,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    HttpTextResult result;
    std::wstring schemeError;
    if (!IsHttpsUrl(url, schemeError)) {
        result.error = std::move(schemeError);
        return result;
    }
    if (!std::all_of(
            formBody.begin(),
            formBody.end(),
            [](const unsigned char character) {
                return character >= 0x20 && character <= 0x7e;
            })) {
        result.error = L"Form body contains non-ASCII characters.";
        return result;
    }
    const auto temporary = CreateCurlTemporaryPath();
    if (temporary.empty()) {
        result.error = L"Unable to create a temporary HTTP response file.";
        return result;
    }
    std::error_code fileError;
    std::filesystem::remove(temporary, fileError);
    const std::wstring wideBody(formBody.begin(), formBody.end());
    const auto process = RunCurl(
        {
            L"--fail",
            L"--location",
            L"--silent",
            L"--show-error",
            L"--ssl-revoke-best-effort",
            L"--connect-timeout",
            L"10",
            L"--max-time",
            L"90",
            L"--proto",
            L"=https",
            L"--header",
            L"Accept: application/json",
            L"--header",
            L"Content-Type: application/x-www-form-urlencoded",
            L"--data",
            wideBody,
            L"--output",
            temporary.wstring(),
            url
        },
        temporary,
        stopToken,
        {},
        maximumBytes);
    if (process.exitCode != 0) {
        result.transportErrorCode = static_cast<long>(process.exitCode);
        result.error = process.error.empty()
            ? L"Windows curl.exe returned error code "
                + std::to_wstring(process.exitCode) + L"."
            : process.error;
        std::filesystem::remove(temporary, fileError);
        return result;
    }
    std::ifstream input(temporary, std::ios::binary);
    if (!input) {
        result.error = L"Unable to read the temporary HTTP response file.";
        std::filesystem::remove(temporary, fileError);
        return result;
    }
    result.body.assign(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
    input.close();
    std::filesystem::remove(temporary, fileError);
    if (result.body.size() > maximumBytes) {
        result.body.clear();
        result.error = L"The server response exceeds the safety limit.";
        return result;
    }
    result.statusCode = 200;
    return result;
}

bool ShouldRetryCurlDownloadFailure(
    const CURLcode code,
    const long responseCode) {
    // Permanent absence/authentication errors should immediately fall back to
    // the next knowledge-base route.  Retrying them three times only delays a
    // batch and increases load on the public archive.
    if (responseCode == 400 || responseCode == 401
        || responseCode == 403 || responseCode == 404
        || responseCode == 410 || responseCode == 530
        || responseCode == 550) {
        return false;
    }
    if (responseCode == 408 || responseCode == 421
        || responseCode == 425 || responseCode == 426
        || responseCode == 429
        || (responseCode >= 450 && responseCode <= 452)
        || (responseCode >= 500 && responseCode <= 504)) {
        return true;
    }
    switch (code) {
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_WEIRD_SERVER_REPLY:
    case CURLE_PARTIAL_FILE:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_AGAIN:
    case CURLE_FTP_ACCEPT_FAILED:
    case CURLE_FTP_ACCEPT_TIMEOUT:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
        return true;
    default:
        return false;
    }
}

bool WaitForDownloadRetryDelay(
    const std::chrono::milliseconds delay,
    const std::stop_token stopToken,
    const DownloadControlPtr& control) {
    const auto deadline = std::chrono::steady_clock::now() + delay;
    while (!stopToken.stop_requested()
           && std::chrono::steady_clock::now() < deadline) {
        if (control != nullptr && !control->waitIfPaused(stopToken)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !stopToken.stop_requested();
}

std::string ReadBoundedFile(const std::filesystem::path& path, std::size_t maximumBytes);

HttpDownloadResult DownloadWithBundledCurl(
    const std::wstring& url, const std::filesystem::path& destination,
    std::stop_token stopToken, const DownloadProgressCallback& progress,
    std::wstring_view allowedProtocols, std::wstring_view extraHeader,
    DownloadControlPtr control, long maximumSeconds) {
    HttpDownloadResult result;
    const auto part = std::filesystem::path(destination.wstring() + L".part");
    const auto errors = CreateCurlTemporaryPath();
    if (errors.empty()) { result.error = L"Unable to create a transfer diagnostic file."; return result; }
    std::vector<std::wstring> arguments{
        L"--fail", L"--location", L"--silent", L"--show-error",
        L"--proto", std::wstring(allowedProtocols), L"--proto-redir", std::wstring(allowedProtocols),
        L"--connect-timeout", L"20", L"--max-time", std::to_wstring(maximumSeconds),
        L"--speed-limit", L"32", L"--speed-time", L"30",
        L"--output", part.wstring(), L"--stderr", errors.wstring()
    };
    if (IsCddisExplicitTlsFtp(url) || url.starts_with(L"ftps://")) arguments.push_back(L"--ssl-reqd");
    if (NeedsExplicitAnonymousFtpLogin(url)) {
        arguments.insert(arguments.end(), {L"--user", L"anonymous:gnss-downloader@example.invalid"});
    }
    if (!extraHeader.empty()) arguments.insert(arguments.end(), {L"--header", std::wstring(extraHeader)});
    arguments.push_back(url);
    const auto transfer = RunCurl(arguments, part, stopToken, progress, 0, control);
    std::error_code error;
    result.cancelled = transfer.cancelled || stopToken.stop_requested();
    if (transfer.exitCode != 0 || result.cancelled) {
        const auto detail = ReadBoundedFile(errors, 4096);
        result.error = result.cancelled ? L"Download cancelled."
            : L"Transfer failed: " + (detail.empty() ? transfer.error : Utf8ToWide(detail));
        std::filesystem::remove(part, error);
    } else {
        result.bytesWritten = std::filesystem::file_size(part, error);
        if (!error) std::filesystem::rename(part, destination, error);
        if (error) result.error = L"Unable to save the downloaded file.";
        else if (!DownloadFileLooksValid(destination)) {
            result.error = L"The server did not return a valid data file.";
            std::filesystem::remove(destination, error);
        } else result.success = true;
        if (!result.success) std::filesystem::remove(part, error);
    }
    std::filesystem::remove(errors, error);
    return result;
}

HttpDownloadResult CurlDownloadFile(
    const std::wstring& url,
    const std::filesystem::path& destination,
    const std::stop_token stopToken,
    const DownloadProgressCallback& progress,
    const std::wstring_view allowedProtocols = L"=https",
    const std::wstring_view extraHeader = {},
    DownloadControlPtr control = nullptr,
    const long maximumSeconds = 0,
    const int maximumAttempts = 0) {
    HttpDownloadResult result;
    if (!EnsureLibCurl(result.error)) return result;
    std::error_code fileError;
    std::filesystem::create_directories(destination.parent_path(), fileError);
    if (fileError) {
        result.error = L"无法创建下载目录。";
        return result;
    }
    const auto partPath = std::filesystem::path(destination.wstring() + L".part");
    std::filesystem::remove(partPath, fileError);

    const auto resolvedProxy = ResolveProxyForUrl(url);
    const bool canTryAlternateSystemProxy =
        resolvedProxy.ambiguousSystemEndpoint
        && (url.starts_with(L"ftp://") || url.starts_with(L"ftps://"));
    const std::string alternateHttpProxy = canTryAlternateSystemProxy
        ? "http://" + resolvedProxy.ambiguousEndpoint
        : std::string{};

    const char* protocols = "https";
    if (allowedProtocols == L"=ftp") protocols = "ftp";
    else if (allowedProtocols == L"=ftps") protocols = "ftps";
    else if (allowedProtocols == L"=http") protocols = "http,https";

    CURLcode code = CURLE_FAILED_INIT;
    long responseCode = 0;
    std::array<char, CURL_ERROR_SIZE> errorBuffer{};
    const long transferTimeout = maximumSeconds > 0 ? maximumSeconds : 3600L;
    const long lowSpeedTime = maximumSeconds > 0
        ? std::min<long>(30L, std::max<long>(5L, maximumSeconds))
        : 120L;
    const int attemptLimit = maximumAttempts > 0 ? maximumAttempts : 3;
    for (int attempt = 0;
         attempt < attemptLimit && !stopToken.stop_requested();
         ++attempt) {
        std::filesystem::remove(partPath, fileError);
        std::FILE* output = nullptr;
        if (_wfopen_s(&output, partPath.c_str(), L"wb") != 0
            || output == nullptr) {
            result.error = L"无法创建临时下载文件。";
            return result;
        }

        CurlTransferContext context;
        context.output = output;
        context.stopToken = stopToken;
        context.control = control;
        context.progress = progress;
        UniqueCurlEasy easy(curl_easy_init());
        if (!easy) {
            std::fclose(output);
            result.error = L"无法创建内置 libcurl 下载会话。";
            return result;
        }
        ConfigureLibCurlCommon(easy.get(), url, protocols, context);
        if (canTryAlternateSystemProxy && attempt > 0) {
            // The first attempt used SOCKS5 for an unqualified Windows
            // proxy endpoint.  If that fails, retry the same endpoint as an
            // HTTP proxy so ordinary corporate HTTP proxies remain supported.
            curl_easy_setopt(
                easy.get(), CURLOPT_PROXY, alternateHttpProxy.c_str());
            curl_easy_setopt(easy.get(), CURLOPT_NOPROXY, "");
        }
        curl_easy_setopt(easy.get(), CURLOPT_ERRORBUFFER, errorBuffer.data());
        curl_easy_setopt(easy.get(), CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt(easy.get(), CURLOPT_LOW_SPEED_LIMIT, 32L);
        curl_easy_setopt(easy.get(), CURLOPT_LOW_SPEED_TIME, lowSpeedTime);
        curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT, transferTimeout);

        if (UrlHostEquals(url, L"ftp.sonel.org")
            || UrlHostEquals(url, L"gnss-ftp.kasi.re.kr")) {
            curl_easy_setopt(easy.get(), CURLOPT_PROXY, "");
            curl_easy_setopt(easy.get(), CURLOPT_NOPROXY, "*");
        }
        if (IsCddisExplicitTlsFtp(url)) {
            curl_easy_setopt(
                easy.get(), CURLOPT_USE_SSL,
                static_cast<long>(CURLUSESSL_ALL));
        }
        if (IsBdsPrideFtpsUrl(url)) {
            curl_easy_setopt(easy.get(), CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(easy.get(), CURLOPT_SSL_VERIFYHOST, 0L);
        }
        if (NeedsExplicitAnonymousFtpLogin(url) || IsBdsPrideFtpsUrl(url)) {
            curl_easy_setopt(easy.get(), CURLOPT_USERNAME, "anonymous");
            curl_easy_setopt(
                easy.get(), CURLOPT_PASSWORD,
                "gnss-downloader@example.invalid");
        }

        curl_slist* headers = nullptr;
        const std::string header = WideToUtf8(extraHeader);
        if (!header.empty()) {
            headers = curl_slist_append(headers, header.c_str());
            curl_easy_setopt(easy.get(), CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(
                easy.get(), CURLOPT_USERAGENT,
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 Chrome/138.0.0.0 Safari/537.36");
        }

        errorBuffer.fill('\0');
        code = curl_easy_perform(easy.get());
        curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &responseCode);
        if (headers != nullptr) curl_slist_free_all(headers);
        std::fclose(output);
        result.bytesWritten = context.bytesWritten;

        if (code == CURLE_OK) {
            result.error.clear();
            break;
        }
        if (stopToken.stop_requested()
            || code == CURLE_ABORTED_BY_CALLBACK) {
            result.cancelled = true;
            result.error = L"下载已取消。";
            break;
        }
        // Some TLS handshakes fail in the embedded LibreSSL build. Retry with
        // the bundled client while retaining protocol restrictions and certificate validation.
        if (code == CURLE_SSL_CONNECT_ERROR && !IsBdsPrideFtpsUrl(url)) {
            std::filesystem::remove(partPath, fileError);
            return DownloadWithBundledCurl(url, destination, stopToken, progress,
                allowedProtocols, extraHeader, control, transferTimeout);
        }
        result.error = L"内置 libcurl 下载失败："
            + Utf8ToWide(
                errorBuffer[0] != '\0'
                    ? std::string_view(errorBuffer.data())
                    : std::string_view(curl_easy_strerror(code)));
        if (responseCode >= 400) {
            result.error += L"（HTTP " + std::to_wstring(responseCode) + L"）";
        }
        const bool alternateProxyRetry =
            canTryAlternateSystemProxy && attempt == 0;
        if (attempt + 1 >= attemptLimit
            || (!ShouldRetryCurlDownloadFailure(code, responseCode)
                && !alternateProxyRetry)) {
            break;
        }
        if (!WaitForDownloadRetryDelay(
                std::chrono::seconds(attempt + 1),
                stopToken,
                control)) {
            result.cancelled = true;
            result.error = L"下载已取消。";
            break;
        }
    }
    if (code != CURLE_OK) {
        std::filesystem::remove(partPath, fileError);
        return result;
    }

    result.bytesWritten = std::filesystem::file_size(partPath, fileError);
    if (fileError || result.bytesWritten == 0) {
        result.error = L"下载结果为空。";
        std::filesystem::remove(partPath, fileError);
        return result;
    }
    std::filesystem::rename(partPath, destination, fileError);
    if (fileError) {
        result.error = L"下载完成，但无法将临时文件改为最终文件名。";
        std::filesystem::remove(partPath, fileError);
        return result;
    }
    if (!DownloadFileLooksValid(destination)) {
        std::filesystem::remove(destination, fileError);
        result.error = L"The server did not return a valid data file.";
        return result;
    }
    result.success = true;
    return result;
}

HttpTextResult CurlListFtpDirectory(
    const std::wstring& url,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    HttpTextResult result;
    const bool bdsPride = IsBdsPrideFtpsUrl(url);
    const bool highrateListing =
        url.find(L"/highrate/") != std::wstring::npos
        || url.find(L"/highrate_v3/") != std::wstring::npos;
    const wchar_t* connectTimeout = highrateListing ? L"8" : L"15";
    const wchar_t* maxTime = highrateListing ? L"20" : L"120";
    if (!IsAllowedPublicFtpUrl(url) && !bdsPride) {
        result.error = L"FTP 地址不在公开 GNSS 数据源白名单中。";
        return result;
    }
    const bool isSonel = UrlHostEquals(url, L"ftp.sonel.org");
    if (isSonel) {
        // SONEL's FTP control connection is stable, but only a subset of its
        // advertised passive data ports is reachable from some networks.
        // Race a small number of read-only NLST requests and accept the first
        // complete result. Four bounded waves avoid multi-minute sequential
        // retries while keeping the load modest.
        constexpr int parallelAttempts = 3;
        constexpr int maximumWaves = 4;
        long lastExitCode = 28;
        for (int wave = 0;
             wave < maximumWaves && !stopToken.stop_requested();
             ++wave) {
            std::array<std::filesystem::path, parallelAttempts> paths;
            std::array<std::future<CurlProcessResult>, parallelAttempts> tasks;
            bool setupFailed = false;
            for (int attempt = 0; attempt < parallelAttempts; ++attempt) {
                paths[attempt] = CreateCurlTemporaryPath();
                if (paths[attempt].empty()) {
                    setupFailed = true;
                    break;
                }
                std::error_code removeError;
                std::filesystem::remove(paths[attempt], removeError);
                const auto outputPath = paths[attempt];
                tasks[attempt] = std::async(
                    std::launch::async,
                    [url, outputPath, stopToken, maximumBytes]() {
                        return RunCurl(
                            {
                                L"--fail",
                                L"--list-only",
                                L"--silent",
                                L"--show-error",
                                L"--noproxy", L"*",
                                L"--disable-epsv",
                                L"--connect-timeout", L"8",
                                L"--max-time", L"30",
                                L"--proto", L"=ftp",
                                L"--output", outputPath.wstring(),
                                url
                            },
                            outputPath,
                            stopToken,
                            {},
                            maximumBytes);
                    });
            }
            if (setupFailed) {
                for (auto& path : paths) {
                    std::error_code removeError;
                    if (!path.empty()) std::filesystem::remove(path, removeError);
                }
                result.error = L"Unable to create SONEL FTP temporary files.";
                return result;
            }

            int successful = -1;
            for (int attempt = 0; attempt < parallelAttempts; ++attempt) {
                const auto process = tasks[attempt].get();
                lastExitCode = static_cast<long>(process.exitCode);
                if (successful < 0 && process.exitCode == 0) {
                    successful = attempt;
                }
            }
            if (successful >= 0) {
                std::ifstream input(paths[successful], std::ios::binary);
                result.body.assign(
                    std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
                input.close();
            }
            for (const auto& path : paths) {
                std::error_code removeError;
                std::filesystem::remove(path, removeError);
            }
            if (successful >= 0 && !result.body.empty()
                && result.body.size() <= maximumBytes) {
                result.statusCode = 200;
                return result;
            }
            result.body.clear();
            const auto retryUntil = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(500);
            while (!stopToken.stop_requested()
                   && std::chrono::steady_clock::now() < retryUntil) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        result.transportErrorCode = lastExitCode;
        result.error = L"SONEL FTP passive data ports failed after 4 parallel retry waves (curl code "
            + std::to_wstring(lastExitCode) + L").";
        return result;
    }

    const auto temporary = CreateCurlTemporaryPath();
    if (temporary.empty()) {
        result.error = L"无法创建 FTP 目录临时文件。";
        return result;
    }
    std::error_code fileError;
    std::filesystem::remove(temporary, fileError);
    std::vector<std::wstring> arguments{
        L"--fail",
        L"--list-only",
        L"--silent",
        L"--show-error",
        L"--connect-timeout",
        connectTimeout,
        L"--max-time",
        maxTime,
        L"--proto",
        bdsPride ? L"=ftps" : L"=ftp"
    };
    if (highrateListing) {
        // High-rate probes use a complete directory URL for every request.
        // Keep libcurl on one CWD operation instead of walking each parent
        // component, which saves several FTP control-round trips per hour.
        arguments.push_back(L"--ftp-method");
        arguments.push_back(L"singlecwd");
    }
    if (UrlHostEquals(url, L"gnss-ftp.kasi.re.kr")) {
        // The KASI public FTP endpoint is directly reachable from China but
        // frequently refuses control connections relayed through SOCKS.
        arguments.push_back(L"--noproxy");
        arguments.push_back(L"*");
    }
    if (UrlHostEquals(url, L"ftp.trignet.co.za")) {
        // TrigNet's FTP service advertises an unroutable address in some
        // passive-mode replies.  These options make libcurl keep the control
        // connection's resolved address and use the traditional PASV
        // negotiation, which is important when the user is behind SOCKS/NAT.
        arguments.push_back(L"--disable-epsv");
        arguments.push_back(L"--ftp-skip-pasv-ip");
    }
    if (IsCddisExplicitTlsFtp(url)) {
        arguments.push_back(L"--ssl-reqd");
    }
    if (bdsPride) {
        arguments.push_back(L"--insecure");
    }
    if (NeedsExplicitAnonymousFtpLogin(url) || bdsPride) {
        arguments.push_back(L"--user");
        arguments.push_back(L"anonymous:gnss-downloader@example.invalid");
    }
    arguments.push_back(L"--output");
    arguments.push_back(temporary.wstring());
    arguments.push_back(url);
    std::counting_semaphore<64>* limiter = nullptr;
    if (IsCddisExplicitTlsFtp(url)) {
        limiter = &gCddisFtpSlots;
    } else if (UrlHostEquals(url, L"gssc.esa.int")) {
        limiter = &gEsaFtpSlots;
    }

    CurlProcessResult process;
    int attempts = 0;
    const int maximumAttempts = highrateListing ? 2 : 3;
    for (; attempts < maximumAttempts; ++attempts) {
        std::filesystem::remove(temporary, fileError);
        {
            SemaphorePermit permit(limiter);
            process = RunCurl(
                arguments,
                temporary,
                stopToken,
                {},
                maximumBytes);
        }
        // A few TrigNet FTP front ends accept the control connection but
        // close the passive data channel immediately (curl 52).  Give that
        // specific failure one bounded active-mode attempt before reporting
        // the directory as unavailable.  This is deliberately limited to
        // TrigNet and only to the empty-response symptom; other archives keep
        // their existing retry behavior and timing.
        if (process.exitCode == 52
            && UrlHostEquals(url, L"ftp.trignet.co.za")
            && !stopToken.stop_requested()) {
            auto activeArguments = arguments;
            activeArguments.push_back(L"--ftp-port");
            activeArguments.push_back(L"-");
            std::filesystem::remove(temporary, fileError);
            {
                SemaphorePermit permit(limiter);
                process = RunCurl(
                    activeArguments,
                    temporary,
                    stopToken,
                    {},
                    maximumBytes);
            }
        }
        if (process.exitCode == 0 || process.cancelled
            || stopToken.stop_requested()) {
            break;
        }
        const bool transient = process.exitCode == 7
            || process.exitCode == 28
            || process.exitCode == 35
            || process.exitCode == 56;
        if (!transient || attempts + 1 >= maximumAttempts) {
            break;
        }
        const auto retryDelay = std::chrono::milliseconds(
            500 * (attempts + 1));
        const auto retryUntil = std::chrono::steady_clock::now() + retryDelay;
        while (!stopToken.stop_requested()
               && std::chrono::steady_clock::now() < retryUntil) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    if (process.exitCode != 0) {
        result.transportErrorCode = static_cast<long>(process.exitCode);
        result.error = process.error.empty()
            ? L"Windows curl.exe 读取 FTP 目录失败，错误代码 "
                + std::to_wstring(process.exitCode) + L"（已尝试 "
                + std::to_wstring(attempts + 1) + L" 次）。"
            : process.error;
        std::filesystem::remove(temporary, fileError);
        return result;
    }
    std::ifstream input(temporary, std::ios::binary);
    if (!input) {
        result.error = L"无法读取 FTP 目录临时文件。";
        std::filesystem::remove(temporary, fileError);
        return result;
    }
    result.body.assign(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
    input.close();
    std::filesystem::remove(temporary, fileError);
    if (result.body.size() > maximumBytes) {
        result.body.clear();
        result.error = L"FTP 目录响应超过安全大小限制。";
        return result;
    }
    result.statusCode = 200;
    return result;
}

HttpTextResult ListPanamaSftpFiles(
    const std::wstring& remotePattern,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    HttpTextResult result;
    if (!IsSafePanamaRemotePattern(remotePattern)
        || remotePattern.find(L'*') == std::wstring::npos) {
        result.error = L"巴拿马 SFTP 目录模式无效。";
        return result;
    }
    const auto process = RunPanamaSftp(
        {L"ls -1 " + remotePattern},
        {},
        stopToken,
        {},
        maximumBytes);
    if (process.cancelled) {
        result.error = L"操作已取消。";
        return result;
    }
    if (process.exitCode != 0) {
        result.error = process.error.empty()
            ? L"读取巴拿马官方 SFTP 日文件目录失败，错误代码 "
                + std::to_wstring(process.exitCode) + L"。"
            : process.error;
        return result;
    }
    result.body = process.output;
    result.statusCode = 200;
    return result;
}

HttpTextResult ListRegnaRouSftpFiles(
    const std::wstring& remotePattern,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    HttpTextResult result;
    if (!IsSafeRegnaRouRemotePattern(remotePattern)
        || remotePattern.find(L'*') == std::wstring::npos) {
        result.error = L"REGNA-ROU SFTP 目录模式无效。";
        return result;
    }
    const auto process = RunRegnaRouSftp(
        {L"ls -1 " + remotePattern},
        {},
        stopToken,
        {},
        maximumBytes);
    if (process.cancelled) {
        result.error = L"操作已取消。";
        return result;
    }
    if (process.exitCode != 0) {
        result.transportErrorCode = static_cast<long>(process.exitCode);
        result.error = process.error.empty()
            ? L"读取 REGNA-ROU 官方历史 SFTP 目录失败，错误代码 "
                + std::to_wstring(process.exitCode) + L"。"
            : process.error;
        return result;
    }
    result.body = process.output;
    result.statusCode = 200;
    return result;
}

HttpTextResult ListRegnaRouSftpMonth(
    const int year,
    const int month,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    HttpTextResult result;
    if (year < 1990 || year > 2200 || month < 1 || month > 12) {
        result.error = L"REGNA-ROU SFTP 年月无效。";
        return result;
    }
    std::vector<std::wstring> commands;
    commands.reserve(31);
    for (int day = 1; day <= 31; ++day) {
        // A leading '-' tells OpenSSH batch mode to continue when an invalid
        // calendar day or an unpublished directory does not exist.
        commands.push_back(std::format(
            L"-ls -1 sftpserver/{:04}/{:02}/{:02}",
            year,
            month,
            day));
    }
    const auto process = RunRegnaRouSftp(
        commands,
        {},
        stopToken,
        {},
        maximumBytes);
    if (process.cancelled) {
        result.error = L"操作已取消。";
        return result;
    }
    if (process.exitCode != 0) {
        result.transportErrorCode = static_cast<long>(process.exitCode);
        result.error = process.error.empty()
            ? L"读取 REGNA-ROU 官方历史 SFTP 月站目录失败，错误代码 "
                + std::to_wstring(process.exitCode) + L"。"
            : process.error;
        return result;
    }
    result.body = process.output;
    result.statusCode = 200;
    return result;
}

HttpTextResult ListRgnaMexicoSftpMonth(
    const int year,
    const int month,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    HttpTextResult result;
    if (year < 2020 || year > 2200 || month < 1 || month > 12) {
        result.error = L"RGNA-MX SFTP 年月无效。";
        return result;
    }
    std::vector<std::wstring> commands;
    commands.reserve(31);
    const auto firstDay = std::chrono::sys_days{
        std::chrono::year{year} / std::chrono::month{1}
        / std::chrono::day{1}};
    for (int day = 1; day <= 31; ++day) {
        const auto queryDate = std::chrono::year{year}
            / std::chrono::month{static_cast<unsigned>(month)}
            / std::chrono::day{static_cast<unsigned>(day)};
        if (!queryDate.ok()) continue;
        const int doy = static_cast<int>(
            (std::chrono::sys_days{queryDate} - firstDay).count()) + 1;
        commands.push_back(std::format(
            L"-ls -1 {:04}/{:03}", year, doy));
    }
    const auto process = RunRgnaMexicoSftp(
        commands, {}, stopToken, {}, maximumBytes);
    if (process.cancelled) {
        result.error = L"操作已取消。";
        return result;
    }
    if (process.exitCode != 0) {
        // Translate OpenSSH's generic exit status into the transport classes
        // understood by the shared network-recovery circuit.
        result.transportErrorCode = process.exitCode == ERROR_TIMEOUT
            ? 28L
            : 7L;
        result.error = process.error.empty()
            ? L"读取 INEGI RGNA 官方公共 SFTP 月目录失败，错误代码 "
                + std::to_wstring(process.exitCode) + L"。"
            : process.error;
        return result;
    }
    result.body = process.output;
    result.statusCode = 200;
    return result;
}

HttpDownloadResult DownloadPanamaSftpFile(
    const std::wstring& remotePath,
    const std::filesystem::path& destination,
    const std::stop_token stopToken,
    const DownloadProgressCallback& progress,
    DownloadControlPtr control = nullptr) {
    HttpDownloadResult result;
    if (!IsSafePanamaRemoteFile(remotePath)
        || destination.wstring().find(L'"') != std::wstring::npos) {
        result.error = L"巴拿马 SFTP 文件路径无效。";
        return result;
    }
    std::error_code fileError;
    std::filesystem::create_directories(
        destination.parent_path(),
        fileError);
    if (fileError) {
        result.error = L"无法创建下载目录。";
        return result;
    }
    const auto partPath =
        std::filesystem::path(destination.wstring() + L".part");
    std::filesystem::remove(partPath, fileError);
    const auto process = RunPanamaSftp(
        {
            L"get \"" + remotePath + L"\" \""
                + partPath.wstring() + L"\""
        },
        partPath,
        stopToken,
        progress,
        2U * 1024U * 1024U,
        std::move(control));
    result.cancelled = process.cancelled;
    if (process.exitCode != 0) {
        result.error = process.error.empty()
            ? L"巴拿马 SFTP 下载失败，错误代码 "
                + std::to_wstring(process.exitCode) + L"。"
            : process.error;
        std::filesystem::remove(partPath, fileError);
        return result;
    }
    result.bytesWritten = std::filesystem::file_size(partPath, fileError);
    if (fileError || result.bytesWritten == 0) {
        result.error = L"巴拿马 SFTP 下载结果为空。";
        std::filesystem::remove(partPath, fileError);
        return result;
    }
    std::filesystem::rename(partPath, destination, fileError);
    if (fileError) {
        result.error = L"下载完成，但无法保存最终文件名。";
        std::filesystem::remove(partPath, fileError);
        return result;
    }
    result.success = true;
    return result;
}

HttpDownloadResult DownloadRegnaRouSftpFile(
    const std::wstring& remotePath,
    const std::filesystem::path& destination,
    const std::stop_token stopToken,
    const DownloadProgressCallback& progress,
    DownloadControlPtr control = nullptr) {
    HttpDownloadResult result;
    if (!IsSafeRegnaRouRemoteFile(remotePath)
        || destination.wstring().find(L'"') != std::wstring::npos) {
        result.error = L"REGNA-ROU SFTP 文件路径无效。";
        return result;
    }
    std::error_code fileError;
    std::filesystem::create_directories(destination.parent_path(), fileError);
    if (fileError) {
        result.error = L"无法创建下载目录。";
        return result;
    }
    const auto partPath =
        std::filesystem::path(destination.wstring() + L".part");
    std::filesystem::remove(partPath, fileError);
    const auto process = RunRegnaRouSftp(
        {L"get \"" + remotePath + L"\" \""
            + partPath.wstring() + L"\""},
        partPath,
        stopToken,
        progress,
        2U * 1024U * 1024U,
        std::move(control));
    result.cancelled = process.cancelled;
    if (process.exitCode != 0) {
        result.error = process.error.empty()
            ? L"REGNA-ROU SFTP 下载失败，错误代码 "
                + std::to_wstring(process.exitCode) + L"。"
            : process.error;
        std::filesystem::remove(partPath, fileError);
        return result;
    }
    result.bytesWritten = std::filesystem::file_size(partPath, fileError);
    if (fileError || result.bytesWritten == 0) {
        result.error = L"REGNA-ROU SFTP 下载结果为空。";
        std::filesystem::remove(partPath, fileError);
        return result;
    }
    std::filesystem::rename(partPath, destination, fileError);
    if (fileError) {
        result.error = L"下载完成，但无法保存最终文件名。";
        std::filesystem::remove(partPath, fileError);
        return result;
    }
    result.success = true;
    return result;
}

HttpDownloadResult DownloadRgnaMexicoSftpFile(
    const std::wstring& remotePath,
    const std::filesystem::path& destination,
    const std::stop_token stopToken,
    const DownloadProgressCallback& progress,
    DownloadControlPtr control = nullptr) {
    HttpDownloadResult result;
    if (!IsSafeRgnaMexicoRemoteFile(remotePath)
        || destination.wstring().find(L'"') != std::wstring::npos) {
        result.error = L"RGNA-MX SFTP 文件路径无效。";
        return result;
    }
    std::error_code fileError;
    std::filesystem::create_directories(destination.parent_path(), fileError);
    if (fileError) {
        result.error = L"无法创建下载目录。";
        return result;
    }
    const auto partPath =
        std::filesystem::path(destination.wstring() + L".part");
    std::filesystem::remove(partPath, fileError);
    const auto process = RunRgnaMexicoSftp(
        {L"get \"" + remotePath + L"\" \""
            + partPath.wstring() + L"\""},
        partPath,
        stopToken,
        progress,
        2U * 1024U * 1024U,
        std::move(control));
    result.cancelled = process.cancelled;
    if (process.exitCode != 0) {
        result.error = process.error.empty()
            ? L"RGNA-MX SFTP 下载失败，错误代码 "
                + std::to_wstring(process.exitCode) + L"。"
            : process.error;
        std::filesystem::remove(partPath, fileError);
        return result;
    }
    result.bytesWritten = std::filesystem::file_size(partPath, fileError);
    if (fileError || result.bytesWritten == 0) {
        result.error = L"RGNA-MX SFTP 下载结果为空。";
        std::filesystem::remove(partPath, fileError);
        return result;
    }
    std::filesystem::rename(partPath, destination, fileError);
    if (fileError) {
        result.error = L"下载完成，但无法保存最终文件名。";
        std::filesystem::remove(partPath, fileError);
        return result;
    }
    result.success = true;
    return result;
}

HttpDownloadResult DownloadTerrasSftpFile(
    const std::wstring_view username,
    const std::wstring_view password,
    const std::wstring& remotePath,
    const std::filesystem::path& destination,
    const std::stop_token stopToken,
    const DownloadProgressCallback& progress,
    DownloadControlPtr control = nullptr) {
    HttpDownloadResult result;
    if (!IsSafeTerrasUsername(username)
        || password.empty()
        || !IsSafeTerrasRemoteFile(remotePath)
        || destination.wstring().find(L'"') != std::wstring::npos) {
        result.error = L"GEONET/Terras SFTP 文件路径或凭据无效。";
        return result;
    }
    std::error_code fileError;
    std::filesystem::create_directories(destination.parent_path(), fileError);
    if (fileError) {
        result.error = L"无法创建下载目录。";
        return result;
    }
    const auto partPath =
        std::filesystem::path(destination.wstring() + L".part");
    std::filesystem::remove(partPath, fileError);
    const auto process = RunTerrasSftp(
        {L"get \"" + remotePath + L"\" \""
            + partPath.wstring() + L"\""},
        partPath,
        stopToken,
        progress,
        2U * 1024U * 1024U,
        username,
        password,
        std::move(control));
    result.cancelled = process.cancelled;
    if (process.exitCode != 0) {
        result.error = process.error.empty()
            ? L"GEONET/Terras SFTP 下载失败，错误代码 "
                + std::to_wstring(process.exitCode) + L"。"
            : process.error;
        if (process.output.find("Permission denied") != std::string::npos) {
            result.error = L"GEONET/Terras SFTP 拒绝登录；请检查用户名、密码，"
                           L"并确认注册已生效。";
        }
        std::filesystem::remove(partPath, fileError);
        return result;
    }
    result.bytesWritten = std::filesystem::file_size(partPath, fileError);
    if (fileError || result.bytesWritten == 0) {
        result.error = L"GEONET/Terras SFTP 下载结果为空。";
        std::filesystem::remove(partPath, fileError);
        return result;
    }
    std::filesystem::rename(partPath, destination, fileError);
    if (fileError) {
        result.error = L"下载完成，但无法保存最终文件名。";
        std::filesystem::remove(partPath, fileError);
        return result;
    }
    result.success = true;
    return result;
}

bool IsSafeRgnaRemotePart(const std::wstring& value) {
    constexpr std::wstring_view prefix = L"/home/rgna/";
    if (!value.starts_with(prefix)
        || value.size() > 180
        || !value.ends_with(L".zip")
        || value.find(L"..") != std::wstring::npos) {
        return false;
    }
    return std::all_of(
        value.begin(),
        value.end(),
        [](const wchar_t character) {
            return (character >= L'a' && character <= L'z')
                || (character >= L'A' && character <= L'Z')
                || (character >= L'0' && character <= L'9')
                || character == L'/'
                || character == L'_'
                || character == L'-'
                || character == L'.';
        });
}

struct GeonetRoute {
    std::wstring stationNumber;
    std::wstring observationDate;
    std::wstring fileName;
    int year = 0;
    int month = 0;
    int day = 0;
};

bool ParseGeonetRoute(const std::wstring_view url, GeonetRoute& route) {
    constexpr std::wstring_view prefix = L"geonet-jp://";
    if (!url.starts_with(prefix) || url.size() > 180
        || url.find(L"..") != std::wstring_view::npos) return false;
    const std::wstring_view remainder = url.substr(prefix.size());
    const auto firstSlash = remainder.find(L'/');
    const auto secondSlash = firstSlash == std::wstring_view::npos
        ? std::wstring_view::npos : remainder.find(L'/', firstSlash + 1);
    if ((firstSlash != 5 && firstSlash != 6)
        || secondSlash == std::wstring_view::npos) return false;
    route.stationNumber.assign(remainder.substr(0, firstSlash));
    route.observationDate.assign(remainder.substr(
        firstSlash + 1, secondSlash - firstSlash - 1));
    route.fileName.assign(remainder.substr(secondSlash + 1));
    const auto digitsOnly = [](const std::wstring_view value) {
        return std::all_of(value.begin(), value.end(), [](const wchar_t c) {
            return c >= L'0' && c <= L'9';
        });
    };
    if (route.observationDate.size() != 10 || route.fileName.empty()
        || route.fileName.size() > 100 || !route.fileName.ends_with(L".gz")
        || !digitsOnly(route.stationNumber)
        || route.observationDate[4] != L'-' || route.observationDate[7] != L'-'
        || !digitsOnly(std::wstring_view(route.observationDate).substr(0, 4))
        || !digitsOnly(std::wstring_view(route.observationDate).substr(5, 2))
        || !digitsOnly(std::wstring_view(route.observationDate).substr(8, 2))
        || !std::all_of(route.fileName.begin(), route.fileName.end(),
            [](const wchar_t c) {
                return (c >= L'a' && c <= L'z')
                    || (c >= L'A' && c <= L'Z')
                    || (c >= L'0' && c <= L'9')
                    || c == L'.' || c == L'_' || c == L'-';
            })) return false;
    route.year = std::stoi(route.observationDate.substr(0, 4));
    route.month = std::stoi(route.observationDate.substr(5, 2));
    route.day = std::stoi(route.observationDate.substr(8, 2));
    return route.year >= 2010 && route.year <= 2200
        && route.month >= 1 && route.month <= 12
        && route.day >= 1 && route.day <= 31;
}

std::string ReadBoundedFile(
    const std::filesystem::path& path,
    const std::size_t maximumBytes) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximumBytes) return {};
    std::ifstream input(path, std::ios::binary);
    return input
        ? std::string(std::istreambuf_iterator<char>(input), {})
        : std::string{};
}

std::wstring GeonetListingForm(const GeonetRoute& route) {
    return L"category=gis&sel_obs%5B0%5D=" + route.stationNumber
        + L"&day_time=UTC"
        + std::format(
            L"&day_st_year={:04}&day_st_mon={:02}&day_st_day={:02}"
            L"&day_ed_year={:04}&day_ed_mon={:02}&day_ed_day={:02}",
            route.year, route.month, route.day,
            route.year, route.month, route.day)
        + L"&day_satellite=GRJE&day_rinex_ver=3.02"
          L"&day_filename=normal&day_navfile=normal";
}

HttpDownloadResult DownloadGeonetJapanFileOnce(
    const GeonetRoute& route,
    const std::filesystem::path& destination,
    const std::stop_token stopToken,
    const DownloadProgressCallback& progress,
    DownloadControlPtr control = nullptr) {
    HttpDownloadResult result;
    std::wstring permitError;
    TerrasNamedPermit terrasPermit(stopToken, permitError);
    if (!terrasPermit.acquired()) {
        result.cancelled = terrasPermit.cancelled();
        result.error = std::move(permitError);
        return result;
    }
    const auto partPath = std::filesystem::path(destination.wstring() + L".part");
    std::error_code fileError;
    std::filesystem::remove(partPath, fileError);
    const auto cookiePath = CreateCurlTemporaryPath();
    const auto responsePath = CreateCurlTemporaryPath();
    const auto errorPath = CreateCurlTemporaryPath();
    if (cookiePath.empty() || responsePath.empty() || errorPath.empty()) {
        result.error = L"Unable to create a GEONET session file.";
        return result;
    }
    struct SessionFiles {
        std::filesystem::path cookies, response, errors;
        ~SessionFiles() {
            std::error_code ignored;
            for (const auto& path : {cookies, response, errors})
                std::filesystem::remove(path, ignored);
        }
    } sessionFiles{cookiePath, responsePath, errorPath};

    // Terras rejects the TLS handshake from the MSVC LibreSSL build.
    // The bundled curl client handles the same session with certificate checks enabled.
    const auto runPost = [&](const std::wstring& url, const std::wstring& form,
                             const std::filesystem::path& output,
                             const std::size_t limit, const int seconds,
                             const wchar_t* operation,
                             const DownloadProgressCallback& callback = {}) {
        const auto process = RunCurl({
            L"--fail", L"--location", L"--silent", L"--show-error",
            L"--proto", L"=https", L"--proto-redir", L"=https",
            L"--http1.1", L"--connect-timeout", L"20",
            L"--max-time", std::to_wstring(seconds),
            L"--cookie", cookiePath.wstring(), L"--cookie-jar", cookiePath.wstring(),
            L"--referer", L"https://terras.gsi.go.jp/data_service.php",
            L"--user-agent", L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/138.0.0.0 Safari/537.36",
            L"--header", L"Origin: https://terras.gsi.go.jp",
            L"--header", L"Content-Type: application/x-www-form-urlencoded",
            L"--data", form, L"--output", output.wstring(),
            L"--stderr", errorPath.wstring(), url
        }, output, stopToken, callback, limit, control);
        if (process.exitCode == 0 && !process.cancelled) return true;
        result.cancelled = process.cancelled || stopToken.stop_requested();
        const auto diagnostic = ReadBoundedFile(errorPath, 4096);
        result.error = result.cancelled ? L"Download cancelled."
            : std::wstring(operation) + L" failed: "
                + (diagnostic.empty() ? process.error : Utf8ToWide(diagnostic));
        return false;
    };
    if (!runPost(L"https://terras.gsi.go.jp/data_download.php",
            L"category=gis&sel_obs%5B%5D=" + route.stationNumber,
            responsePath, 2U * 1024U * 1024U, 180, L"GEONET session initialization")) return result;
    if (!runPost(L"https://terras.gsi.go.jp/day_download.php",
            GeonetListingForm(route), responsePath, 8U * 1024U * 1024U,
            180, L"GEONET file listing")) return result;
    const std::string listing = ReadBoundedFile(responsePath, 8U * 1024U * 1024U);
    const std::string station = WideToUtf8(route.stationNumber);
    const std::string date = WideToUtf8(route.observationDate);
    const std::regex linkPattern(
        "one_file_download\\('" + station
        + "','([^']+)','" + date + "','1','([^']+)'\\)");
    std::smatch match;
    if (!std::regex_search(listing, match, linkPattern)) {
        result.error = L"No GEONET observation file was listed for the selected station and date.";
        return result;
    }
    const std::string remoteNarrow = match[1].str();
    const std::string tokenNarrow = match[2].str();
    const std::wstring remotePath(remoteNarrow.begin(), remoteNarrow.end());
    const std::wstring temporaryToken(tokenNarrow.begin(), tokenNarrow.end());
    if (remotePath.find(L"..") != std::wstring::npos
        || !remotePath.starts_with(L"/")
        || temporaryToken.empty() || temporaryToken.size() > 100) {
        result.error = L"GEONET returned invalid download parameters.";
        return result;
    }

    std::filesystem::create_directories(destination.parent_path(), fileError);
    if (fileError) {
        result.error = L"Unable to create the download folder.";
        return result;
    }
    std::filesystem::remove(partPath, fileError);
    const std::wstring downloadForm =
        L"hi_download=1&satellite_type=GRJE&data_type=1"
        L"&observation_date=" + route.observationDate
        + L"&d_mode=%5Bday%5D+GRJE+3.02+"
        + std::format(L"{:04}%2F{:02}%2F{:02}", route.year, route.month, route.day)
        + L"&d_file=" + remotePath
        + L"&d_obs_cd=" + route.stationNumber
        + L"&d_satellite=GRJE&d_rv=3.02&d_category=gis"
        + L"&d_tmp_dir=" + temporaryToken
        + L"&category=gis&day_time=UTC"
        + std::format(
            L"&day_st_year={:04}&day_st_mon={:02}&day_st_day={:02}"
            L"&day_ed_year={:04}&day_ed_mon={:02}&day_ed_day={:02}",
            route.year, route.month, route.day,
            route.year, route.month, route.day)
        + L"&day_satellite=GRJE&day_rinex_ver=3.02"
          L"&day_filename=normal&day_navfile=normal"
          L"&filename=normal&navfile=normal&sel_obs%5B0%5D="
        + route.stationNumber;

    const bool downloaded = runPost(
        L"https://terras.gsi.go.jp/download.php", downloadForm, partPath,
        512U * 1024U * 1024U, 1200, L"GEONET file transfer", progress);
    if (!downloaded) {
        std::filesystem::remove(partPath, fileError);
        return result;
    }
    std::ifstream input(partPath, std::ios::binary);
    unsigned char magic[2]{};
    input.read(reinterpret_cast<char*>(magic), sizeof(magic));
    input.close();
    if (magic[0] != 0x1f || magic[1] != 0x8b) {
        const std::string response = ReadBoundedFile(
            partPath, 256U * 1024U);
        std::smatch titleMatch;
        const std::regex titlePattern(
            R"(<title[^>]*>\s*([^<]{1,200})\s*</title>)",
            std::regex_constants::icase);
        std::wstring title;
        if (std::regex_search(response, titleMatch, titlePattern)) {
            title = Utf8ToWide(titleMatch[1].str());
            std::replace(title.begin(), title.end(), L'\r', L' ');
            std::replace(title.begin(), title.end(), L'\n', L' ');
            while (title.find(L"  ") != std::wstring::npos) {
                title.replace(title.find(L"  "), 2, L" ");
            }
        }
        std::filesystem::remove(partPath, fileError);
        result.error = L"GEONET returned a server page instead of a valid gzip observation file";
        if (!title.empty() && std::all_of(title.begin(), title.end(), [](wchar_t c) { return c < 128; })) {
            result.error += L" (page title: " + title + L")";
        }
        result.error += L". Terras may be busy or limiting transfers; retry later.";
        return result;
    }
    result.bytesWritten = std::filesystem::file_size(partPath, fileError);
    if (fileError) {
        std::filesystem::remove(partPath, fileError);
        result.error = L"Unable to verify the GEONET download.";
        return result;
    }
    std::filesystem::rename(partPath, destination, fileError);
    if (fileError) {
        std::filesystem::remove(partPath, fileError);
        result.error = L"Download completed, but the file could not be saved under its final name.";
        return result;
    }
    result.success = true;
    return result;
}

HttpDownloadResult DownloadGeonetJapanFile(
    const std::wstring_view url,
    const std::filesystem::path& destination,
    const std::stop_token stopToken,
    const DownloadProgressCallback& progress,
    DownloadControlPtr control = nullptr) {
    GeonetRoute route;
    HttpDownloadResult result;
    if (!ParseGeonetRoute(url, route)) {
        result.error = L"Invalid GEONET download route.";
        return result;
    }
    std::wstring terrasUsername;
    std::wstring terrasPassword;
    {
        std::scoped_lock lock(gTerrasCredentials.mutex);
        terrasUsername = gTerrasCredentials.username;
        terrasPassword = gTerrasCredentials.password;
    }
    if (!terrasUsername.empty() && !terrasPassword.empty()) {
        const auto date = std::chrono::sys_days{
            std::chrono::year{route.year}
            / std::chrono::month{static_cast<unsigned>(route.month)}
            / std::chrono::day{static_cast<unsigned>(route.day)}};
        const auto yearStart = std::chrono::sys_days{
            std::chrono::year{route.year}/std::chrono::January/1};
        const int dayOfYear = static_cast<int>((date - yearStart).count()) + 1;
        const auto rinex4Start = std::chrono::sys_days{
            std::chrono::year{2024}/std::chrono::September/3};
        const wchar_t* archive = date >= rinex4Start
            ? L"GRJE_4.01"
            : L"GRJE_3.02";
        const std::wstring remotePath = std::format(
            L"/data/{}/{:04}/{:03}/{}",
            archive,
            route.year,
            dayOfYear,
            route.fileName);
        result = DownloadTerrasSftpFile(
            terrasUsername,
            terrasPassword,
            remotePath,
            destination,
            stopToken,
            progress,
            control);
        SecureClear(terrasUsername);
        SecureClear(terrasPassword);
        return result;
    }
    for (int attempt = 0; attempt < 3 && !stopToken.stop_requested(); ++attempt) {
        result = DownloadGeonetJapanFileOnce(
            route,
            destination,
            stopToken,
            progress,
            control);
        if (result.success || result.cancelled
            || result.error.starts_with(L"No GEONET observation file was listed")
            || result.error.starts_with(L"GEONET returned invalid download parameters")) return result;
        if (attempt < 2) {
            if (!WaitForDownloadRetryDelay(
                    std::chrono::seconds(attempt + 1),
                    stopToken,
                    control)) {
                result.cancelled = true;
                result.error = L"Download cancelled.";
                return result;
            }
        }
    }
    return result;
}

}  // namespace

HttpTextResult HttpGetText(
    const std::wstring& url,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    // Native Site Log archives are commonly published as individual files in
    // an anonymous FTP directory (RGP, GREF, SONEL, SOPAC and CACS among
    // others).  The directory listing already uses libcurl's FTP path, so
    // fetch the selected log with the same protocol instead of sending an
    // FTP URL through the HTTP/WinINet code path.  This also keeps the
    // protocol allow-list explicit and prevents an arbitrary FTP URL from
    // being requested by a malformed catalogue entry.
    if (url.starts_with(L"ftp://") || url.starts_with(L"ftps://")) {
        const bool secure = url.starts_with(L"ftps://");
        if ((!secure && !IsAllowedPublicFtpUrl(url))
            || (secure && !IsBdsPrideFtpsUrl(url))) {
            HttpTextResult result;
            result.error = L"FTP Site Log 地址不在公开 GNSS 数据源白名单中。";
            return result;
        }
        return CurlGetText(
            url,
            stopToken,
            maximumBytes,
            secure ? L"=ftps" : L"=ftp");
    }
    // WinINet can spend several seconds closing every paginated S3 request
    // through a SOCKS proxy. curl handles this particular public bucket much
    // faster and consistently, which matters for historical GA backfills.
    const bool highrateListing =
        url.find(L"/highrate/") != std::wstring::npos
        || url.find(L"/highrate_v3/") != std::wstring::npos;
    if (highrateListing || IsGaPublicS3Url(url) || IsNoaaCorsS3Url(url)
        || IsGeoNetMetadataS3Url(url)) {
        return CurlGetText(
            url,
            stopToken,
            maximumBytes,
            IsAllowedPublicHttpUrl(url) ? L"=http" : L"=https");
    }
    HttpTextResult result;
    for (int attempt = 0; attempt < 2; ++attempt) {
        UniqueInternetHandle session;
        UniqueInternetHandle request;
        result.error.clear();
        result.body.clear();
        if (!OpenUrl(url, session, request, result.error)) {
            if (IsCsnUrl(url)) {
                return CurlGetCsnText(url, stopToken, maximumBytes);
            }
            return CurlGetText(
                url,
                stopToken,
                maximumBytes,
                IsAllowedPublicHttpUrl(url) ? L"=http" : L"=https");
        }

        DWORD status = 0;
        if (!QueryStatusCode(
                static_cast<HINTERNET>(request.get()),
                status,
                result.error)) {
            return result;
        }
        result.statusCode = status;
        const std::size_t responseLimit =
            status >= 200 && status < 300
            ? maximumBytes
            : std::min<std::size_t>(maximumBytes, 512U * 1024U);
        if (!ReadInternetText(
                static_cast<HINTERNET>(request.get()),
                stopToken,
                responseLimit,
                result.body,
                result.error)) {
            return result;
        }
        if (status >= 200 && status < 300) {
            return result;
        }
        if (status == 503 && attempt == 0 && IsCsnUrl(url)) {
            if (!SolveCsnChallenge(result.body, stopToken, result.error)) {
                result.body.clear();
                return result;
            }
            continue;
        }
        result.body.clear();
        result.error = L"服务器返回 HTTP " + std::to_wstring(status) + L"。";
        return result;
    }
    result.body.clear();
    result.error = L"CSN 浏览器验证失败。";
    return result;
}

HttpTextResult HttpGetByteRange(
    const std::wstring& url,
    const std::wstring& byteRange,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    return CurlGetByteRange(
        url, byteRange, stopToken, maximumBytes);
}

HttpTextResult HttpPostFormText(
    const std::wstring& url,
    const std::string& formBody,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    if (IsTerrasUrl(url)) {
        HttpTextResult result;
        TerrasNamedPermit terrasPermit(stopToken, result.error);
        if (!terrasPermit.acquired()) return result;
        return CurlPostFormText(url, formBody, stopToken, maximumBytes);
    }
    return CurlPostFormText(url, formBody, stopToken, maximumBytes);
}

HttpTextResult HttpGetRgnaSessionText(
    const std::wstring& url,
    const bool initializeSession,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    constexpr std::wstring_view prefix =
        L"https://www.inegi.org.mx/app/geo2/rgna/";
    HttpTextResult result;
    if (!url.starts_with(prefix)
        || url.size() > 4096
        || url.find(L"..") != std::wstring::npos) {
        result.error = L"RGNA 会话地址无效。";
        return result;
    }
    return CurlGetRgnaSessionText(
        url,
        initializeSession,
        stopToken,
        maximumBytes);
}

HttpTextResult HttpListFtpDirectory(
    const std::wstring& url,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    return CurlListFtpDirectory(url, stopToken, maximumBytes);
}

HttpTextResult HttpListPanamaSftpFiles(
    const std::wstring& remotePattern,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    return ListPanamaSftpFiles(
        remotePattern,
        stopToken,
        maximumBytes);
}

HttpTextResult HttpListRegnaRouSftpFiles(
    const std::wstring& remotePattern,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    return ListRegnaRouSftpFiles(remotePattern, stopToken, maximumBytes);
}

HttpTextResult HttpListRegnaRouSftpMonth(
    const int year,
    const int month,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    return ListRegnaRouSftpMonth(year, month, stopToken, maximumBytes);
}

HttpTextResult HttpListRgnaMexicoSftpMonth(
    const int year,
    const int month,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    return ListRgnaMexicoSftpMonth(year, month, stopToken, maximumBytes);
}

HttpTextResult HttpListTerrasSftpDirectories(
    const std::wstring& username,
    const std::wstring& password,
    const std::vector<std::wstring>& remoteDirectories,
    const std::stop_token stopToken,
    const std::size_t maximumBytes) {
    HttpTextResult result;
    if (!IsSafeTerrasUsername(username) || password.empty()
        || remoteDirectories.empty() || remoteDirectories.size() > 62
        || !std::all_of(
            remoteDirectories.begin(), remoteDirectories.end(),
            [](const std::wstring& directory) {
                return IsSafeTerrasRemoteDirectory(directory);
            })) {
        result.error = L"GEONET/Terras SFTP 目录请求或凭据无效。";
        return result;
    }
    // The public documentation names /data/GRJE_3.02, but Terras accounts
    // can be placed behind different SFTP virtual roots.  Probe only four
    // fixed, read-only representations of that documented archive.  No
    // arbitrary path supplied by a caller is ever accepted.
    constexpr std::wstring_view canonicalPrefix = L"/data/";
    constexpr std::array<std::wstring_view, 4> layoutPrefixes{
        L"/data/",
        L"data/",
        L"/",
        L""
    };
    static std::atomic<int> rememberedLayout{-1};

    std::array<int, layoutPrefixes.size()> order{};
    std::iota(order.begin(), order.end(), 0);
    const int remembered = rememberedLayout.load(std::memory_order_relaxed);
    if (remembered >= 0
        && remembered < static_cast<int>(layoutPrefixes.size())) {
        const auto found = std::find(order.begin(), order.end(), remembered);
        std::rotate(order.begin(), found, found + 1);
    }

    static const std::regex observationPattern(
        R"((?:^|[/\s])[A-Za-z0-9]{4}[0-9]{3}0\.[0-9]{2}[dDoO](?:\.(?:gz|GZ|Z))?(?:$|\s))",
        std::regex::optimize);
    auto hasObservationFile = [&](const std::string& output) {
        return std::regex_search(output, observationPattern);
    };
    auto replaceAll = [](std::string& text,
                         const std::string_view from,
                         const std::string_view to) {
        if (from.empty() || from == to) return;
        std::size_t offset = 0;
        while ((offset = text.find(from, offset)) != std::string::npos) {
            text.replace(offset, from.size(), to);
            offset += to.size();
        }
    };
    auto lowercaseAscii = [](std::string text) {
        std::transform(
            text.begin(),
            text.end(),
            text.begin(),
            [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
        return text;
    };
    auto isAuthenticationFailure = [](const std::string_view lowerOutput) {
        return lowerOutput.find("permission denied") != std::string_view::npos
            || lowerOutput.find("authentication failed")
                != std::string_view::npos
            || lowerOutput.find("too many authentication failures")
                != std::string_view::npos;
    };

    std::string lastOutput;
    for (const int layout : order) {
        // Cold-state tests completed four consecutive daily listings per
        // login, while a fifth could leave the authenticated connection
        // directory-silent.  Never cross that observed safe boundary.  The
        // caller treats this group all-or-nothing and splits only a failed
        // group into single-day retry tasks after a global cooldown.
        constexpr std::size_t directoriesPerSession = 4;
        std::string layoutOutput;
        for (std::size_t chunkBegin = 0;
             chunkBegin < remoteDirectories.size();
             chunkBegin += directoriesPerSession) {
            const std::size_t chunkEnd = std::min(
                remoteDirectories.size(),
                chunkBegin + directoriesPerSession);
            std::vector<std::wstring> effectiveDirectories;
            std::vector<std::wstring> commands;
            effectiveDirectories.reserve(chunkEnd - chunkBegin);
            commands.reserve(chunkEnd - chunkBegin);
            for (std::size_t index = chunkBegin; index < chunkEnd; ++index) {
                const auto& directory = remoteDirectories[index];
                const std::wstring suffix =
                    directory.substr(canonicalPrefix.size());
                std::wstring effective(layoutPrefixes[layout]);
                effective += suffix;
                effectiveDirectories.push_back(effective);
                // Listing the directory itself avoids OpenSSH client-side
                // wildcard expansion and its per-object stat overhead.
                commands.push_back(L"-ls -1 " + effective);
            }

            const std::size_t remainingBytes = maximumBytes == 0
                ? 0
                : (layoutOutput.size() >= maximumBytes
                    ? 0
                    : maximumBytes - layoutOutput.size());
            if (maximumBytes > 0 && remainingBytes == 0) {
                result.error = L"SFTP 合并目录输出超过安全大小限制。";
                return result;
            }

            SftpProcessResult process;
            // The builder owns the visible per-day retry queue and global
            // cooldown.  Keep this transport call to one connection attempt;
            // hidden nested retries would multiply one failed date into as
            // many as twelve rapid sessions and defeat that circuit breaker.
            constexpr int maximumAttempts = 1;
            constexpr std::array<int, 1> retryDelaysSeconds{0};
            int completedAttempts = 0;
            for (int attempt = 0;
                 attempt < maximumAttempts && !stopToken.stop_requested();
                 ++attempt) {
                completedAttempts = attempt + 1;
                process = RunTerrasSftp(
                    commands,
                    {},
                    stopToken,
                    {},
                    remainingBytes,
                    username,
                    password);
                if (process.cancelled) {
                    result.error = L"操作已取消。";
                    return result;
                }
                const std::string lowerOutput =
                    lowercaseAscii(process.output);
                const bool authenticationFailure =
                    isAuthenticationFailure(lowerOutput);
                const bool transportFailure =
                    process.exitCode != 0 || !process.error.empty();
                if (!transportFailure || authenticationFailure) break;
                const bool transientConnectionFailure =
                    lowerOutput.find("connection timed out")
                        != std::string::npos
                    || lowerOutput.find("connection reset")
                        != std::string::npos
                    || lowerOutput.find("connection closed")
                        != std::string::npos
                    || lowerOutput.find("broken pipe") != std::string::npos
                    || lowerOutput.find("banner exchange")
                        != std::string::npos
                    || lowerOutput.find("connection refused")
                        != std::string::npos;
                if (!process.inactivityTimedOut
                    && !transientConnectionFailure) {
                    break;
                }
                if (attempt + 1 < maximumAttempts) {
                    // Terras may accept authentication while temporarily
                    // withholding directory replies after several sessions.
                    // Short 1/2-second retries remain inside that throttle
                    // window, so use bounded exponential-style backoff.
                    const auto retryDeadline =
                        std::chrono::steady_clock::now()
                        + std::chrono::seconds(
                            retryDelaysSeconds[attempt]);
                    while (!stopToken.stop_requested()
                           && std::chrono::steady_clock::now()
                               < retryDeadline) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(100));
                    }
                }
            }
            if (stopToken.stop_requested()) {
                result.error = L"操作已取消。";
                return result;
            }
            if (process.exitCode != 0 || !process.error.empty()) {
                result.transportErrorCode =
                    static_cast<long>(process.exitCode);
                const std::string lowerOutput =
                    lowercaseAscii(process.output);
                if (isAuthenticationFailure(lowerOutput)) {
                    result.error = L"GEONET/Terras SFTP 拒绝登录；请检查用户名、"
                                   L"密码，并确认注册已生效。";
                } else {
                    result.error = process.error.empty()
                        ? L"读取 GEONET/Terras SFTP 目录失败，错误代码 "
                            + std::to_wstring(process.exitCode) + L"。"
                        : process.error;
                    const std::wstring diagnostic =
                        SafeSftpDiagnostic(process.output);
                    if (!diagnostic.empty()) {
                        result.error += L" SFTP诊断：" + diagnostic;
                    }
                    if (completedAttempts > 1) {
                        result.error += L" 已自动建立新会话，累计尝试 "
                            + std::to_wstring(completedAttempts)
                            + L" 次。";
                    }
                }
                return result;
            }

            // Normalize only this chunk's paths before aggregation.
            for (std::size_t offset = 0;
                 offset < effectiveDirectories.size(); ++offset) {
                replaceAll(
                    process.output,
                    WideToUtf8(effectiveDirectories[offset]),
                    WideToUtf8(remoteDirectories[chunkBegin + offset]));
            }
            const std::size_t separatorBytes = layoutOutput.empty() ? 0U : 1U;
            if (maximumBytes > 0
                && process.output.size()
                    > maximumBytes - layoutOutput.size() - separatorBytes) {
                result.error = L"SFTP 合并目录输出超过安全大小限制。";
                return result;
            }
            if (!layoutOutput.empty()) layoutOutput.push_back('\n');
            layoutOutput += process.output;
        }

        lastOutput = layoutOutput;
        if (!hasObservationFile(layoutOutput)) continue;
        rememberedLayout.store(layout, std::memory_order_relaxed);
        result.body = std::move(layoutOutput);
        result.statusCode = 200;
        return result;
    }

    result.error = L"GEONET/Terras SFTP 已登录，但官方目录及受限虚拟根候选"
                   L"均未返回可识别的观测文件。";
    const std::wstring diagnostic = SafeSftpDiagnostic(lastOutput);
    if (!diagnostic.empty()) {
        result.error += L" SFTP诊断：" + diagnostic;
    }
    // If every documented/virtual-root form failed, collect a bounded,
    // read-only view of the account's actual SFTP root.  This is emitted only
    // in the interactive error message; it is never persisted in the
    // database or a log file and contains no password.
    const auto layoutProbe = RunTerrasSftp(
        {
            L"-ls -1 /data",
            L"pwd",
            L"-ls -1 /",
            L"-ls -1 .",
            L"-ls -ld /data/GRJE_3.02",
            L"-ls -ld /data/GRJE_4.01"
        },
        {},
        stopToken,
        {},
        1024U * 1024U,
        username,
        password);
    if (!layoutProbe.cancelled && !layoutProbe.output.empty()) {
        const std::wstring layoutDiagnostic =
            SafeSftpDiagnostic(layoutProbe.output);
        if (!layoutDiagnostic.empty()) {
            result.error += L" 账号根目录探测：" + layoutDiagnostic;
        }
    }
    result.statusCode = 200;
    return result;
}

bool SetTerrasSftpCredentials(
    const std::wstring& username,
    const std::wstring& password,
    std::wstring& error) {
    error.clear();
    if (!IsSafeTerrasUsername(username) || password.empty()
        || password.size() > 1024) {
        error = L"Terras SFTP 用户名或密码为空/无效。";
        return false;
    }
    std::scoped_lock lock(gTerrasCredentials.mutex);
    SecureClear(gTerrasCredentials.username);
    SecureClear(gTerrasCredentials.password);
    gTerrasCredentials.username = username;
    gTerrasCredentials.password = password;
    return true;
}

void ClearTerrasSftpCredentials() {
    std::scoped_lock lock(gTerrasCredentials.mutex);
    SecureClear(gTerrasCredentials.username);
    SecureClear(gTerrasCredentials.password);
}

HttpDownloadResult HttpDownloadFile(
    const std::wstring& url,
    const std::filesystem::path& destination,
    const std::stop_token stopToken,
    const DownloadProgressCallback& progress,
    const bool allowFederatedPublicHttp,
    DownloadControlPtr control,
    const long maximumSeconds,
    const int maximumAttempts) {
    if (!WaitForDownloadControl(control, stopToken)) {
        HttpDownloadResult result;
        result.cancelled = true;
        result.error = L"下载已取消。";
        return result;
    }
    if (url.find(L"#gnss-zip=") != std::wstring::npos) {
        return archive_member::Download(url, destination, stopToken, progress, control);
    }
    if (url.starts_with(L"pan-share://")) {
        const auto remote = url.substr(12);
        HttpDownloadResult result;
        if (!remote.starts_with(L"/sites/DatosPub/DatosCORS/GPSDATA/")
            || remote.find(L"..") != std::wstring::npos
            || remote.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/._-") != std::wstring::npos) {
            result.error = L"巴拿马公开分享文件路径无效。";
            return result;
        }
        // Use the official anonymous sharing link to establish its scoped
        // WinINet cookies. Static file URLs alone correctly return HTTP 403.
        const auto share = HttpGetText(
            L"https://anatipa.sharepoint.com/:f:/s/DatosPub/IgBcHVIcHMIkQ5Lx-qBDG5PEAS8Vn-uxsUIm1Wy_eTBU_Lc?e=pLIibD",
            stopToken, 2U * 1024U * 1024U);
        if (!share.ok() || stopToken.stop_requested()) {
            result.cancelled = stopToken.stop_requested();
            result.error = L"巴拿马官方匿名分享暂时不可访问，请稍后重试。";
            return result;
        }
        std::wstring encoded;
        for (const auto ch : remote) {
            if (ch == L'/') encoded += L"%2F";
            else encoded += ch;
        }
        const auto realUrl = L"https://anatipa.sharepoint.com/sites/DatosPub/_api/web/GetFileByServerRelativeUrl('"
            + encoded + L"')/$value";
        const auto temporary = std::filesystem::path(destination.wstring()
            + L".pan-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(GetCurrentThreadId()));
        result = HttpDownloadFile(realUrl, temporary, stopToken, progress,
            false, control, maximumSeconds, maximumAttempts);
        if (result.success) {
            std::ifstream input(temporary, std::ios::binary);
            std::string prefix(512, '\0');
            input.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
            prefix.resize(static_cast<std::size_t>(input.gcount())); input.close();
            const bool compressed = prefix.starts_with("PK\x03\x04")
                || prefix.starts_with("\x1f\x8b") || prefix.starts_with("\x1f\x9d");
            if ((!compressed && prefix.find("RINEX VERSION / TYPE") == std::string::npos
                 && prefix.find("CRINEX VERS") == std::string::npos)
                || !MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                result.success = false;
                result.error = L"巴拿马分享未返回有效观测文件或无法保存，请检查官方分享状态。";
            }
        }
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return result;
    }
    if (url.starts_with(L"pan-share://")) {
        const auto remote = url.substr(12);
        HttpDownloadResult result;
        if (!remote.starts_with(L"/sites/DatosPub/DatosCORS/GPSDATA/")
            || remote.find(L"..") != std::wstring::npos
            || remote.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/._-") != std::wstring::npos) {
            result.error = L"巴拿马公开分享文件路径无效。";
            return result;
        }
        // Use the official anonymous sharing link to establish its scoped
        // WinINet cookies. Static file URLs alone correctly return HTTP 403.
        const auto share = HttpGetText(
            L"https://anatipa.sharepoint.com/:f:/s/DatosPub/IgBcHVIcHMIkQ5Lx-qBDG5PEAS8Vn-uxsUIm1Wy_eTBU_Lc?e=pLIibD",
            stopToken, 2U * 1024U * 1024U);
        if (!share.ok() || stopToken.stop_requested()) {
            result.cancelled = stopToken.stop_requested();
            result.error = L"巴拿马官方匿名分享暂时不可访问，请稍后重试。";
            return result;
        }
        std::wstring encoded;
        for (const auto ch : remote) {
            if (ch == L'/') encoded += L"%2F";
            else encoded += ch;
        }
        const auto realUrl = L"https://anatipa.sharepoint.com/sites/DatosPub/_api/web/GetFileByServerRelativeUrl('"
            + encoded + L"')/$value";
        const auto temporary = std::filesystem::path(destination.wstring()
            + L".pan-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(GetCurrentThreadId()));
        result = HttpDownloadFile(realUrl, temporary, stopToken, progress,
            false, control, maximumSeconds, maximumAttempts);
        if (result.success) {
            std::ifstream input(temporary, std::ios::binary);
            std::string prefix(512, '\0');
            input.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
            prefix.resize(static_cast<std::size_t>(input.gcount())); input.close();
            const bool compressed = prefix.starts_with("PK\x03\x04")
                || prefix.starts_with("\x1f\x8b") || prefix.starts_with("\x1f\x9d");
            if ((!compressed && prefix.find("RINEX VERSION / TYPE") == std::string::npos
                 && prefix.find("CRINEX VERS") == std::string::npos)
                || !MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                result.success = false;
                result.error = L"巴拿马分享未返回有效观测文件或无法保存，请检查官方分享状态。";
            }
        }
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return result;
    }
    if (url.starts_with(L"magna-portal://")) {
        HttpDownloadResult result;
        result.error = L"MAGNA-ECO 观测文件需在官方门户登录后下载：https://ccg.igac.gov.co/";
        return result;
    }
    if (url.find(L"#gnss-zip=") != std::wstring::npos) {
        return archive_member::Download(url, destination, stopToken, progress, control);
    }
    if (url.starts_with(L"magna-portal://")) {
        HttpDownloadResult result;
        result.error = L"MAGNA-ECO 观测文件需在官方门户登录后下载：https://ccg.igac.gov.co/";
        return result;
    }
    if (url.starts_with(L"geonet-jp://")) {
        return DownloadGeonetJapanFile(
            url, destination, stopToken, progress, control);
    }
    std::wstring panamaRemotePath;
    if (IsPanamaSftpUrl(url, panamaRemotePath)) {
        return DownloadPanamaSftpFile(
            panamaRemotePath,
            destination,
            stopToken,
            progress,
            control);
    }
    std::wstring regnaRouRemotePath;
    if (IsRegnaRouSftpUrl(url, regnaRouRemotePath)) {
        return DownloadRegnaRouSftpFile(
            regnaRouRemotePath,
            destination,
            stopToken,
            progress,
            control);
    }
    std::wstring rgnaMexicoRemotePath;
    if (IsRgnaMexicoSftpUrl(url, rgnaMexicoRemotePath)) {
        return DownloadRgnaMexicoSftpFile(
            rgnaMexicoRemotePath,
            destination,
            stopToken,
            progress,
            control);
    }
    if (IsAllowedPublicFtpUrl(url)) {
        return CurlDownloadFile(
            url,
            destination,
            stopToken,
            progress,
            L"=ftp",
            {},
            control,
            maximumSeconds,
            maximumAttempts);
    }
    if (IsBdsPrideFtpsUrl(url)) {
        return CurlDownloadFile(
            url,
            destination,
            stopToken,
            progress,
            L"=ftps",
            {},
            control,
            maximumSeconds,
            maximumAttempts);
    }
    if (allowFederatedPublicHttp
        && IsSafeFederatedPublicHttpUrl(url)) {
        return CurlDownloadFile(
            url,
            destination,
            stopToken,
            progress,
            L"=http",
            {},
            control,
            maximumSeconds,
            maximumAttempts);
    }
    // Ordinary HTTPS downloads use WinINet.  It follows the user's Windows
    // proxy settings and native certificate store exactly like the browser.
    // Keep embedded libcurl for FTP/FTPS and explicit federated HTTP routes.
    // GEONET uses the bundled curl client for its multi-step POST session.
    HttpDownloadResult result;
    UniqueInternetHandle session;
    UniqueInternetHandle request;
    DWORD status = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        session.reset();
        request.reset();
        result.error.clear();
        if (!OpenUrl(url, session, request, result.error)) {
            if (IsCsnUrl(url)) {
                std::wstring cookie;
                {
                    std::scoped_lock lock(gCsnChallengeMutex);
                    if (!gCsnChallengeToken.empty()) {
                        cookie =
                            L"Cookie: challenge_token=" + gCsnChallengeToken;
                    }
                }
                return CurlDownloadFile(
                    url,
                    destination,
                    stopToken,
                    progress,
                    L"=https",
                    cookie,
                    control,
                    maximumSeconds,
                    maximumAttempts);
            }
            return CurlDownloadFile(
                url,
                destination,
                stopToken,
                progress,
                IsAllowedPublicHttpUrl(url) ? L"=http" : L"=https",
                {},
                control,
                maximumSeconds,
                maximumAttempts);
        }
        if (!QueryStatusCode(
                static_cast<HINTERNET>(request.get()),
                status,
                result.error)) {
            return result;
        }
        if (status >= 200 && status < 300) {
            break;
        }
        if (status == 503 && attempt == 0 && IsCsnUrl(url)) {
            std::string challengeBody;
            if (!ReadInternetText(
                    static_cast<HINTERNET>(request.get()),
                    stopToken,
                    512U * 1024U,
                    challengeBody,
                    result.error)
                || !SolveCsnChallenge(
                    challengeBody,
                    stopToken,
                    result.error)) {
                return result;
            }
            continue;
        }
        result.error = L"服务器返回 HTTP " + std::to_wstring(status) + L"。";
        return result;
    }
    if (status < 200 || status >= 300) {
        result.error = L"CSN 浏览器验证失败。";
        return result;
    }

    const std::uint64_t totalBytes =
        QueryContentLength(static_cast<HINTERNET>(request.get()));
    const auto partPath = std::filesystem::path(destination.wstring() + L".part");

    std::error_code fileError;
    std::filesystem::create_directories(destination.parent_path(), fileError);
    if (fileError) {
        result.error = L"无法创建下载目录。";
        return result;
    }
    std::filesystem::remove(partPath, fileError);

    std::ofstream output(partPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        result.error = L"无法创建临时下载文件。";
        return result;
    }

    std::array<char, 256U * 1024U> buffer{};
    while (!stopToken.stop_requested()) {
        if (!WaitForDownloadControl(control, stopToken)) {
            result.cancelled = true;
            result.error = L"下载已取消。";
            break;
        }
        DWORD bytesRead = 0;
        if (!InternetReadFile(
                static_cast<HINTERNET>(request.get()),
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytesRead)) {
            result.error = L"读取网络数据失败：" + GetLastWin32ErrorMessage(GetLastError());
            break;
        }
        if (bytesRead == 0) {
            output.close();
            if (totalBytes > 0 && result.bytesWritten != totalBytes) {
                result.error = L"Incomplete download: received byte count does not match Content-Length.";
                std::filesystem::remove(partPath, fileError);
                return result;
            }
            std::filesystem::rename(partPath, destination, fileError);
            if (fileError) {
                result.error = L"下载完成，但无法将临时文件改为最终文件名。";
                std::filesystem::remove(partPath, fileError);
                return result;
            }
            if (!DownloadFileLooksValid(destination)) {
                std::filesystem::remove(destination, fileError);
                result.error = L"The server did not return a valid data file.";
                return result;
            }
            result.success = true;
            return result;
        }

        output.write(buffer.data(), bytesRead);
        if (!output) {
            result.error = L"写入下载文件失败，可能是磁盘空间不足。";
            break;
        }
        result.bytesWritten += bytesRead;
        if (progress) {
            progress(result.bytesWritten, totalBytes);
        }
    }

    if (stopToken.stop_requested()) {
        result.cancelled = true;
        result.error = L"下载已取消。";
    }
    output.close();
    std::filesystem::remove(partPath, fileError);
    return result;
}

HttpDownloadResult HttpDownloadRgnaBundle(
    const std::vector<std::wstring>& remoteParts,
    const std::filesystem::path& destination,
    const std::stop_token stopToken,
    const DownloadProgressCallback& progress,
    DownloadControlPtr control) {
    HttpDownloadResult result;
    if (!WaitForDownloadControl(control, stopToken)) {
        result.cancelled = true;
        result.error = L"下载已取消。";
        return result;
    }
    if (remoteParts.empty() || remoteParts.size() > 24
        || !std::all_of(
            remoteParts.begin(),
            remoteParts.end(),
            IsSafeRgnaRemotePart)) {
        result.error = L"RGNA 远程文件清单无效。";
        return result;
    }

    const auto responsePath = CreateCurlTemporaryPath();
    if (responsePath.empty()) {
        result.error = L"无法创建 RGNA 打包响应临时文件。";
        return result;
    }
    std::error_code fileError;
    std::filesystem::remove(responsePath, fileError);
    std::vector<std::wstring> arguments{
        L"--fail",
        L"--location",
        L"--silent",
        L"--show-error",
        L"--ssl-revoke-best-effort",
        L"--connect-timeout",
        L"10",
        L"--max-time",
        L"180",
        L"--proto",
        L"=https"
    };
    for (const auto& part : remoteParts) {
        arguments.push_back(L"--data-urlencode");
        arguments.push_back(L"archivos[]=" + part);
    }
    arguments.push_back(L"--output");
    arguments.push_back(responsePath.wstring());
    arguments.push_back(
        L"https://www.inegi.org.mx/app/geo2/rgna/descargaTodos.do");

    const auto process = RunCurl(
        arguments,
        responsePath,
        stopToken,
        {},
        16U * 1024U,
        control);
    if (process.cancelled) {
        result.cancelled = true;
        result.error = L"下载已取消。";
        std::filesystem::remove(responsePath, fileError);
        return result;
    }
    if (process.exitCode != 0) {
        result.error = process.error.empty()
            ? L"RGNA 官方服务打包失败，curl 错误代码 "
                + std::to_wstring(process.exitCode) + L"。"
            : process.error;
        std::filesystem::remove(responsePath, fileError);
        return result;
    }

    std::ifstream responseInput(responsePath, std::ios::binary);
    const std::string response{
        std::istreambuf_iterator<char>(responseInput),
        std::istreambuf_iterator<char>()};
    responseInput.close();
    std::filesystem::remove(responsePath, fileError);
    constexpr std::string_view marker = "DescargaTodos_";
    const auto markerPosition = response.find(marker);
    if (markerPosition == std::string::npos) {
        result.error = L"RGNA 官方服务没有返回可下载的打包文件。";
        return result;
    }
    const auto tokenStart = markerPosition + marker.size();
    auto tokenEnd = tokenStart;
    while (tokenEnd < response.size()
           && ((response[tokenEnd] >= '0' && response[tokenEnd] <= '9')
               || response[tokenEnd] == '-'
               || response[tokenEnd] == '.')) {
        ++tokenEnd;
    }
    const std::string token = response.substr(
        tokenStart,
        tokenEnd - tokenStart);
    if (token.empty() || token.size() > 80) {
        result.error = L"RGNA 官方服务返回了无效的下载令牌。";
        return result;
    }
    const std::wstring finalUrl =
        L"https://www.inegi.org.mx/app/geo2/rgna/descargaFile.do?ruta="
        + std::wstring(token.begin(), token.end())
        + L"&nombre=RINEX_GNSSDownloader";
    return CurlDownloadFile(
        finalUrl,
        destination,
        stopToken,
        progress,
        L"=https",
        {},
        control);
}

std::wstring GetLastWin32ErrorMessage(const unsigned long errorCode) {
    const bool useEnglish = NativeUiLanguage::IsEnglish();
    const DWORD languageId = useEnglish
        ? MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)
        : MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);
    wchar_t* message = nullptr;
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER
            | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        languageId,
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);
    if (length == 0 && errorCode >= INTERNET_ERROR_BASE) {
        const HMODULE winInetModule = GetModuleHandleW(L"wininet.dll");
        if (winInetModule != nullptr) {
            length = FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER
                    | FORMAT_MESSAGE_FROM_HMODULE
                    | FORMAT_MESSAGE_IGNORE_INSERTS,
                winInetModule,
                errorCode,
                languageId,
                reinterpret_cast<wchar_t*>(&message),
                0,
                nullptr);
        }
    }
    const bool hasMessage = length > 0 && message != nullptr;
    std::wstring result = hasMessage
        ? std::wstring(message, length)
        : (useEnglish ? L"Windows error " : L"未知错误 ")
            + std::to_wstring(errorCode);
    if (message != nullptr) {
        LocalFree(message);
    }
    while (!result.empty()
           && (result.back() == L'\r'
               || result.back() == L'\n'
               || result.back() == L' ')) {
        result.pop_back();
    }
    if (useEnglish && hasMessage) {
        result += L" (Windows error " + std::to_wstring(errorCode) + L")";
    }
    return result;
}
