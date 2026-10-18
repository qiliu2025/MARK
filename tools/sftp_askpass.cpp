#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

void SecureClear(std::wstring& value) {
    if (!value.empty()) {
        SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
        value.clear();
    }
}

void SecureClear(std::string& value) {
    if (!value.empty()) {
        SecureZeroMemory(value.data(), value.size());
        value.clear();
    }
}

}  // namespace

// OpenSSH invokes SSH_ASKPASS as a child process.  The parent passes the
// password only in this child's inherited environment; it is never placed in
// a command line, batch file, log, database, or persistent settings file.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    constexpr wchar_t variable[] = L"GNSS_DOWNLOADER_SFTP_PASSWORD";
    const DWORD required = GetEnvironmentVariableW(variable, nullptr, 0);
    if (required <= 1 || required > 32'768) return 1;

    std::wstring password(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        variable, password.data(), required);
    if (written == 0 || written >= required) {
        SecureClear(password);
        return 1;
    }
    password.resize(written);

    const int utf8Bytes = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS,
        password.data(), static_cast<int>(password.size()),
        nullptr, 0, nullptr, nullptr);
    if (utf8Bytes <= 0) {
        SecureClear(password);
        return 1;
    }
    std::string output(static_cast<std::size_t>(utf8Bytes), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS,
            password.data(), static_cast<int>(password.size()),
            output.data(), utf8Bytes, nullptr, nullptr) != utf8Bytes) {
        SecureClear(password);
        SecureClear(output);
        return 1;
    }
    output.push_back('\n');

    DWORD outputBytes = 0;
    const HANDLE standardOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    const BOOL ok = standardOutput != nullptr
        && standardOutput != INVALID_HANDLE_VALUE
        && WriteFile(
            standardOutput,
            output.data(),
            static_cast<DWORD>(output.size()),
            &outputBytes,
            nullptr);
    SecureClear(password);
    SecureClear(output);
    return ok ? 0 : 1;
}
