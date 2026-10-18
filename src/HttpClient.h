#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

struct HttpTextResult {
    std::string body;
    std::wstring error;
    unsigned long statusCode = 0;
    // A curl transport exit code when curl handled the request.  HTTP errors
    // remain statusCode values and are deliberately not classified as proxy
    // failures by the index builder.
    long transportErrorCode = 0;

    [[nodiscard]] bool ok() const {
        return error.empty() && statusCode >= 200 && statusCode < 300;
    }
};

struct HttpDownloadResult {
    bool success = false;
    bool cancelled = false;
    std::uint64_t bytesWritten = 0;
    std::wstring error;
};

using DownloadProgressCallback =
    std::function<void(std::uint64_t downloaded, std::uint64_t total)>;

// A single control can be shared by every worker in one download batch.
// pause()/resume() are safe to call from the UI thread while transport
// workers are inside libcurl, WinINet, or an OpenSSH SFTP transfer.
// waitIfPaused() always remains responsive to the caller's stop_token.
//
// Existing callers do not need a control: passing nullptr (the default on
// HttpDownloadFile/HttpDownloadRgnaBundle) preserves the previous behaviour.
class DownloadControl {
public:
    DownloadControl() = default;
    DownloadControl(const DownloadControl&) = delete;
    DownloadControl& operator=(const DownloadControl&) = delete;

    void setPaused(bool paused) noexcept;
    void pause() noexcept;
    void resume() noexcept;
    [[nodiscard]] bool isPaused() const noexcept;

    // Returns false only when stopToken has been requested.  A false result
    // tells a transport callback to abort exactly like its existing cancel
    // path; resuming normally returns true and continues the same file.
    [[nodiscard]] bool waitIfPaused(std::stop_token stopToken) const noexcept;

private:
    std::atomic_bool paused_{false};
    mutable std::mutex pauseMutex_;
    mutable std::condition_variable pauseCondition_;
};

using DownloadControlPtr = std::shared_ptr<DownloadControl>;

HttpTextResult HttpGetText(
    const std::wstring& url,
    std::stop_token stopToken,
    std::size_t maximumBytes = 16U * 1024U * 1024U);

// Read a bounded HTTP byte range without downloading the complete object.
// byteRange uses curl/HTTP syntax, for example L"-65557" or L"100-199".
HttpTextResult HttpGetByteRange(
    const std::wstring& url,
    const std::wstring& byteRange,
    std::stop_token stopToken,
    std::size_t maximumBytes = 16U * 1024U * 1024U);

HttpTextResult HttpPostFormText(
    const std::wstring& url,
    const std::string& formBody,
    std::stop_token stopToken,
    std::size_t maximumBytes = 16U * 1024U * 1024U);

// INEGI's RGNA application returns HTTP 200 with an empty body unless all
// station/file requests reuse the JSESSIONID created by llam=init.  This
// helper owns one bounded temporary cookie jar for the process.
HttpTextResult HttpGetRgnaSessionText(
    const std::wstring& url,
    bool initializeSession,
    std::stop_token stopToken,
    std::size_t maximumBytes = 2U * 1024U * 1024U);

HttpTextResult HttpListFtpDirectory(
    const std::wstring& url,
    std::stop_token stopToken,
    std::size_t maximumBytes = 16U * 1024U * 1024U);

HttpTextResult HttpListPanamaSftpFiles(
    const std::wstring& remotePattern,
    std::stop_token stopToken,
    std::size_t maximumBytes = 4U * 1024U * 1024U);

// Lists the public-credential historical REGNA-ROU archive exposed by IGM
// Uruguay.  The path is deliberately constrained to sftpserver/YYYY/MM/...
// so callers cannot turn the embedded official credential into a generic
// SFTP client.
HttpTextResult HttpListRegnaRouSftpFiles(
    const std::wstring& remotePattern,
    std::stop_token stopToken,
    std::size_t maximumBytes = 16U * 1024U * 1024U);

// Opens one SFTP session and lists every daily directory in a historical
// month.  This avoids OpenSSH's very slow remote expansion of
// sftpserver/YYYY/MM/*/*/*.zip while preserving the complete station set.
HttpTextResult HttpListRegnaRouSftpMonth(
    int year,
    int month,
    std::stop_token stopToken,
    std::size_t maximumBytes = 16U * 1024U * 1024U);

// Lists the public INEGI RGNA daily archive with one SFTP login per month.
// INEGI publishes the read-only credential rgnasftp/rgnasftp for this
// service, so no user account is required.
HttpTextResult HttpListRgnaMexicoSftpMonth(
    int year,
    int month,
    std::stop_token stopToken,
    std::size_t maximumBytes = 16U * 1024U * 1024U);

// Lists fixed, read-only GEONET/Terras archive directories through the
// caller's own SFTP account.  Credentials are supplied in memory only.
HttpTextResult HttpListTerrasSftpDirectories(
    const std::wstring& username,
    const std::wstring& password,
    const std::vector<std::wstring>& remoteDirectories,
    std::stop_token stopToken,
    std::size_t maximumBytes = 64U * 1024U * 1024U);

// Session-only credentials used when a geonet-jp:// route is downloaded.
// They are never persisted; ClearTerrasSftpCredentials securely erases the
// in-process copy at the end of the batch.
bool SetTerrasSftpCredentials(
    const std::wstring& username,
    const std::wstring& password,
    std::wstring& error);
void ClearTerrasSftpCredentials();

HttpDownloadResult HttpDownloadFile(
    const std::wstring& url,
    const std::filesystem::path& destination,
    std::stop_token stopToken,
    const DownloadProgressCallback& progress,
    bool allowFederatedPublicHttp = false,
    DownloadControlPtr control = nullptr,
    long maximumSeconds = 0,
    int maximumAttempts = 0);

HttpDownloadResult HttpDownloadRgnaBundle(
    const std::vector<std::wstring>& remoteParts,
    const std::filesystem::path& destination,
    std::stop_token stopToken,
    const DownloadProgressCallback& progress,
    DownloadControlPtr control = nullptr);

std::wstring GetLastWin32ErrorMessage(unsigned long errorCode);
