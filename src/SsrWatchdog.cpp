#include "SsrWatchdog.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <format>
#include <future>
#include <iomanip>
#include <iterator>
#include <memory>
#include <regex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct HandleCloser {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

std::wstring Win32Error(const DWORD code) {
    wchar_t* raw = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER
            | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<wchar_t*>(&raw),
        0,
        nullptr);
    std::wstring text = length > 0 && raw != nullptr
        ? std::wstring(raw, length)
        : L"Windows error " + std::to_wstring(code);
    if (raw != nullptr) {
        LocalFree(raw);
    }
    while (!text.empty()
           && (text.back() == L'\r' || text.back() == L'\n')) {
        text.pop_back();
    }
    return text;
}

bool SamePath(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
    std::error_code leftError;
    std::error_code rightError;
    const auto normalizedLeft = std::filesystem::weakly_canonical(
        left,
        leftError).wstring();
    const auto normalizedRight = std::filesystem::weakly_canonical(
        right,
        rightError).wstring();
    return !leftError && !rightError
        && _wcsicmp(normalizedLeft.c_str(), normalizedRight.c_str()) == 0;
}

std::filesystem::path SystemCurlPath() {
    // Prefer the project's LibreSSL build: Windows Schannel can return
    // SEC_E_NO_CREDENTIALS even when the selected SSR node is healthy.
    std::wstring module(32768, L'\0');
    const DWORD moduleLength = GetModuleFileNameW(
        nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (moduleLength > 0 && moduleLength < module.size()) {
        module.resize(moduleLength);
        const auto bundled = std::filesystem::path(module).parent_path()
            / L"curl-libressl.exe";
        std::error_code error;
        if (std::filesystem::is_regular_file(bundled, error)) {
            return bundled;
        }
    }
    std::wstring directory(32768, L'\0');
    const UINT length = GetSystemDirectoryW(
        directory.data(),
        static_cast<UINT>(directory.size()));
    if (length == 0 || length >= directory.size()) {
        return {};
    }
    directory.resize(length);
    return std::filesystem::path(directory) / L"curl.exe";
}

std::wstring Quote(const std::wstring& value) {
    if (value.find(L'"') != std::wstring::npos) {
        return {};
    }
    return L"\"" + value + L"\"";
}

bool RunProbeCurl(
    const int localPort,
    const std::vector<std::wstring>& extraArguments,
    const std::wstring& url,
    const std::stop_token stopToken) {
    const auto curl = SystemCurlPath();
    std::error_code error;
    if (!std::filesystem::is_regular_file(curl, error)) {
        return false;
    }
    std::vector<std::wstring> arguments{
        L"--fail",
        L"--silent",
        L"--connect-timeout", L"8",
        L"--max-time", L"25",
        L"--proxy",
        L"socks5h://127.0.0.1:" + std::to_wstring(localPort),
        L"--output", L"NUL"
    };
    if (_wcsicmp(curl.filename().c_str(), L"curl-libressl.exe") == 0) {
        const auto bundle = curl.parent_path() / L"curl-ca-bundle.crt";
        std::error_code error;
        if (std::filesystem::is_regular_file(bundle, error)) {
            arguments.push_back(L"--cacert");
            arguments.push_back(bundle.wstring());
        }
    }
    arguments.insert(
        arguments.end(),
        extraArguments.begin(),
        extraArguments.end());
    arguments.push_back(url);

    std::wstring commandLine = Quote(curl.wstring());
    for (const auto& argument : arguments) {
        const auto quoted = Quote(argument);
        if (quoted.empty()) {
            return false;
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
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            curl.c_str(),
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        return false;
    }
    UniqueHandle processHandle(process.hProcess);
    UniqueHandle threadHandle(process.hThread);
    while (!stopToken.stop_requested()) {
        const DWORD wait = WaitForSingleObject(
            static_cast<HANDLE>(processHandle.get()),
            100);
        if (wait == WAIT_OBJECT_0) {
            DWORD exitCode = 1;
            GetExitCodeProcess(
                static_cast<HANDLE>(processHandle.get()),
                &exitCode);
            return exitCode == 0;
        }
        if (wait == WAIT_FAILED) {
            return false;
        }
    }
    TerminateProcess(
        static_cast<HANDLE>(processHandle.get()),
        ERROR_CANCELLED);
    WaitForSingleObject(
        static_cast<HANDLE>(processHandle.get()),
        5000);
    return false;
}

std::wstring Timestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    return std::format(
        L"{:04}{:02}{:02}-{:02}{:02}{:02}",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond);
}

void InterruptibleSleep(
    const std::chrono::seconds duration,
    const std::stop_token stopToken) {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (!stopToken.stop_requested()
           && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

int NodeRegionRank(const std::string& value) {
    std::string lower = value;
    std::transform(
        lower.begin(),
        lower.end(),
        lower.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    if (lower.find("jp") != std::string::npos
        || lower.find("japan") != std::string::npos
        || lower.find("tokyo") != std::string::npos) {
        return 0;
    }
    if (lower.find("kr") != std::string::npos
        || lower.find("korea") != std::string::npos
        || lower.find("seoul") != std::string::npos) {
        return 1;
    }
    if (lower.find("sg") != std::string::npos
        || lower.find("singapore") != std::string::npos) {
        return 2;
    }
    return 3;
}

std::vector<int> OrderedCandidateNodes(
    const SsrWatchdog::ConfigInfo& config,
    const bool skipCurrent,
    const std::vector<int>& preferred = {}) {
    std::vector<int> candidates;
    candidates.reserve(static_cast<std::size_t>(config.nodeCount));
    for (int index = 0; index < config.nodeCount; ++index) {
        if (skipCurrent && index == config.index) {
            continue;
        }
        candidates.push_back(index);
    }
    std::stable_sort(
        candidates.begin(),
        candidates.end(),
        [&](const int left, const int right) {
            if (!skipCurrent && left == config.index) return true;
            if (!skipCurrent && right == config.index) return false;
            const auto name = [&](const int index) -> const std::string& {
                static const std::string empty;
                return index >= 0
                    && index < static_cast<int>(config.nodeNames.size())
                    ? config.nodeNames[static_cast<std::size_t>(index)]
                    : empty;
            };
            const auto preferredRank = [&](const int value) {
                const auto found = std::find(preferred.begin(), preferred.end(), value);
                return found == preferred.end() ? static_cast<int>(preferred.size())
                    : static_cast<int>(std::distance(preferred.begin(), found));
            };
            const int leftPreferred = preferredRank(left), rightPreferred = preferredRank(right);
            if (leftPreferred != rightPreferred) return leftPreferred < rightPreferred;
            const int leftRank = NodeRegionRank(name(left));
            const int rightRank = NodeRegionRank(name(right));
            if (leftRank != rightRank) return leftRank < rightRank;
            return left < right;
        });
    return candidates;
}

}  // namespace

SsrWatchdog::SsrWatchdog(SsrWatchdogOptions options)
    : options_(std::move(options)) {
    if (options_.directory.empty()) {
        options_.directory = DiscoverDirectory();
    }
    executablePath_ = options_.directory / L"ShadowsocksR.exe";
    configPath_ = options_.directory / L"gui-config.json";
    ConfigInfo config;
    std::wstring ignored;
    if (ReadConfig(config, ignored)) {
        localPort_ = config.localPort;
    }
}

bool SsrWatchdog::available() const {
    std::error_code error;
    return std::filesystem::is_regular_file(executablePath_, error)
        && std::filesystem::is_regular_file(configPath_, error);
}

const std::filesystem::path& SsrWatchdog::executablePath() const {
    return executablePath_;
}

int SsrWatchdog::localPort() const {
    return localPort_;
}

std::filesystem::path SsrWatchdog::DiscoverDirectory() {
    wchar_t configured[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"GNSS_DOWNLOADER_SSR_DIR",
        configured,
        static_cast<DWORD>(std::size(configured)));
    if (length > 0 && length < std::size(configured)) {
        return configured;
    }
    UniqueHandle snapshot(CreateToolhelp32Snapshot(
        TH32CS_SNAPPROCESS,
        0));
    if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE) {
        return {};
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(
            static_cast<HANDLE>(snapshot.get()),
            &entry)) {
        return {};
    }
    do {
        if (_wcsicmp(entry.szExeFile, L"ShadowsocksR.exe") != 0) {
            continue;
        }
        UniqueHandle process(OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            entry.th32ProcessID));
        if (!process) {
            continue;
        }
        std::wstring path(32768, L'\0');
        DWORD pathLength = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(
                static_cast<HANDLE>(process.get()),
                0,
                path.data(),
                &pathLength)) {
            path.resize(pathLength);
            return std::filesystem::path(path).parent_path();
        }
    } while (Process32NextW(
        static_cast<HANDLE>(snapshot.get()),
        &entry));
    return {};
}

bool SsrWatchdog::ReadConfig(
    ConfigInfo& config,
    std::wstring& error) const {
    std::ifstream input(configPath_, std::ios::binary);
    if (!input) {
        error = L"无法读取 SSR 配置文件。";
        return false;
    }
    config.text.assign(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
    const std::regex indexPattern(R"("Index"\s*:\s*(-?\d+))");
    const std::regex portPattern(R"("LocalPort"\s*:\s*(\d+))");
    const std::regex nodePattern(R"("Id"\s*:)");
    const std::regex serverPattern(R"re("server"\s*:\s*"([^"]+)")re");
    std::smatch match;
    if (!std::regex_search(config.text, match, indexPattern)) {
        error = L"SSR 配置中没有找到当前节点 Index。";
        return false;
    }
    config.index = std::stoi(match[1].str());
    if (std::regex_search(config.text, match, portPattern)) {
        config.localPort = std::stoi(match[1].str());
    }
    config.nodeCount = static_cast<int>(std::distance(
        std::sregex_iterator(
            config.text.begin(),
            config.text.end(),
            nodePattern),
        std::sregex_iterator()));
    for (auto iterator = std::sregex_iterator(
             config.text.begin(),
             config.text.end(),
             serverPattern);
         iterator != std::sregex_iterator();
         ++iterator) {
        config.nodeNames.push_back((*iterator)[1].str());
    }
    if (config.index < 0 || config.nodeCount <= 0
        || config.index >= config.nodeCount
        || config.localPort < 1 || config.localPort > 65535) {
        error = L"SSR 配置中的节点索引或本地端口无效。";
        return false;
    }
    return true;
}

bool SsrWatchdog::WriteIndex(
    const ConfigInfo& config,
    const int index,
    std::wstring& error) const {
    if (index < 0 || index >= config.nodeCount) {
        error = L"候选 SSR 节点索引超出范围。";
        return false;
    }
    const std::regex pattern(R"("Index"\s*:\s*(-?\d+))");
    const std::string replacement = "\"Index\": "
        + std::to_string(index);
    const std::string updated = std::regex_replace(
        config.text,
        pattern,
        replacement,
        std::regex_constants::format_first_only);
    if (updated == config.text && index != config.index) {
        error = L"无法更新 SSR 当前节点索引。";
        return false;
    }

    const auto backup = std::filesystem::path(
        configPath_.wstring() + L".watchdog-" + Timestamp() + L".bak");
    std::error_code fileError;
    std::filesystem::copy_file(
        configPath_,
        backup,
        std::filesystem::copy_options::none,
        fileError);
    if (fileError) {
        error = L"无法备份 SSR 配置文件：" + backup.wstring();
        return false;
    }
    const auto temporary = std::filesystem::path(
        configPath_.wstring() + L".watchdog.tmp");
    {
        std::ofstream output(
            temporary,
            std::ios::binary | std::ios::trunc);
        if (!output) {
            error = L"无法创建 SSR 临时配置文件。";
            return false;
        }
        output.write(
            updated.data(),
            static_cast<std::streamsize>(updated.size()));
        output.flush();
        if (!output) {
            error = L"无法完整写入 SSR 临时配置文件。";
            return false;
        }
    }
    if (!MoveFileExW(
            temporary.c_str(),
            configPath_.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = L"无法原子替换 SSR 配置："
            + Win32Error(GetLastError());
        std::filesystem::remove(temporary, fileError);
        return false;
    }
    return true;
}

bool SsrWatchdog::StopExactProcess(std::wstring& error) const {
    UniqueHandle snapshot(CreateToolhelp32Snapshot(
        TH32CS_SNAPPROCESS,
        0));
    if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE) {
        error = L"无法枚举 SSR 进程：" + Win32Error(GetLastError());
        return false;
    }
    std::vector<DWORD> matched;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(
            static_cast<HANDLE>(snapshot.get()),
            &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"ShadowsocksR.exe") != 0) {
                continue;
            }
            UniqueHandle process(OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                entry.th32ProcessID));
            if (!process) {
                continue;
            }
            std::wstring path(32768, L'\0');
            DWORD length = static_cast<DWORD>(path.size());
            if (QueryFullProcessImageNameW(
                    static_cast<HANDLE>(process.get()),
                    0,
                    path.data(),
                    &length)) {
                path.resize(length);
                if (SamePath(path, executablePath_)) {
                    matched.push_back(entry.th32ProcessID);
                }
            }
        } while (Process32NextW(
            static_cast<HANDLE>(snapshot.get()),
            &entry));
    }
    for (const DWORD processId : matched) {
        UniqueHandle process(OpenProcess(
            PROCESS_TERMINATE | SYNCHRONIZE,
            FALSE,
            processId));
        if (!process) {
            error = L"无法打开 SSR 进程：" + Win32Error(GetLastError());
            return false;
        }
        if (!TerminateProcess(
                static_cast<HANDLE>(process.get()),
                0)) {
            error = L"无法停止 SSR：" + Win32Error(GetLastError());
            return false;
        }
        WaitForSingleObject(
            static_cast<HANDLE>(process.get()),
            10000);
    }
    return true;
}

bool SsrWatchdog::StartProcess(std::wstring& error) const {
    std::wstring commandLine = Quote(executablePath_.wstring());
    std::vector<wchar_t> mutableCommand(
        commandLine.begin(),
        commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            executablePath_.c_str(),
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            options_.directory.c_str(),
            &startup,
            &process)) {
        error = L"无法重启 SSR：" + Win32Error(GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool SsrWatchdog::WaitForLocalPort(
    const std::stop_token stopToken) const {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return false;
    }
    const auto cleanup = []() { WSACleanup(); };
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(20);
    while (!stopToken.stop_requested()
           && std::chrono::steady_clock::now() < deadline) {
        SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socketHandle != INVALID_SOCKET) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<u_short>(localPort_));
            InetPtonW(AF_INET, L"127.0.0.1", &address.sin_addr);
            const bool connected = connect(
                socketHandle,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == 0;
            closesocket(socketHandle);
            if (connected) {
                cleanup();
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    cleanup();
    return false;
}

bool SsrWatchdog::ProbeRound(
    const std::stop_token stopToken) const {
    if (options_.googleOnly) {
        return RunProbeCurl(
            localPort_, {},
            L"https://www.google.com/generate_204", stopToken);
    }
    auto bkg = std::async(
        std::launch::async,
        [&]() {
            return RunProbeCurl(
                localPort_,
                {},
                L"https://igs.bkg.bund.de/root_ftp/IGS/highrate/2025/212/",
                stopToken);
        });
    auto ign = std::async(
        std::launch::async,
        [&]() {
            return RunProbeCurl(
                localPort_,
                {L"--list-only"},
                L"ftp://igs.ign.fr/pub/igs/data/highrate/2025/212/",
                stopToken);
        });
    auto esa = std::async(
        std::launch::async,
        [&]() {
            return RunProbeCurl(
                localPort_,
                {L"--list-only"},
                L"ftp://gssc.esa.int/gnss/data/highrate/2025/212/00/",
                stopToken);
        });
    const bool bkgOk = bkg.get();
    const bool ignOk = ign.get();
    const bool esaOk = esa.get();
    return static_cast<int>(bkgOk)
        + static_cast<int>(ignOk)
        + static_cast<int>(esaOk) >= 2;
}

bool SsrWatchdog::ProbeCurrentNode(
    const std::stop_token stopToken,
    const LogCallback& log) const {
    if (!available()) {
        if (log) log(L"没有找到可控制的 ShadowsocksR.exe/gui-config.json。");
        return false;
    }
    ConfigInfo config;
    std::wstring error;
    if (!ReadConfig(config, error)) {
        if (log) log(error);
        return false;
    }
    localPort_ = config.localPort;
    const bool healthy = ProbeRound(stopToken);
    if (log) {
        log(healthy
                ? (options_.googleOnly
                    ? L"当前 SSR 节点可以访问 Google，保留当前节点。"
                    : L"当前 SSR 节点通过匿名高频端到端探测，保留当前节点。")
                : (options_.googleOnly
                    ? L"当前 SSR 节点无法访问 Google，将尝试切换节点。"
                    : L"当前 SSR 节点未通过匿名高频端到端探测，将尝试切换节点。"));
    }
    return healthy;
}

bool SsrWatchdog::RestartAndProbe(
    const int index,
    const bool changeIndex,
    const std::stop_token stopToken,
    const LogCallback& log) {
    ConfigInfo config;
    std::wstring error;
    if (!ReadConfig(config, error)) {
        log(error);
        return false;
    }
    localPort_ = config.localPort;
    if (!StopExactProcess(error)) {
        log(error);
        return false;
    }
    if (changeIndex && index != config.index
        && !WriteIndex(config, index, error)) {
        log(error);
        StartProcess(error);
        return false;
    }
    if (!StartProcess(error)) {
        log(error);
        return false;
    }
    if (!WaitForLocalPort(stopToken)) {
        log(L"SSR 已启动，但本地 SOCKS 端口未在 20 秒内就绪。");
        return false;
    }
    log(options_.googleOnly
        ? L"SSR 本地端口已就绪，正在进行两轮 Google HTTPS 探测……"
        : L"SSR 本地端口已就绪，正在进行 HTTPS/FTP/FTPS 两轮探测……");
    if (!ProbeRound(stopToken)) {
        return false;
    }
    InterruptibleSleep(std::chrono::seconds(1), stopToken);
    return !stopToken.stop_requested() && ProbeRound(stopToken);
}

bool SsrWatchdog::ConfigureProcessProxy(std::wstring& error) {
    ConfigInfo config;
    if (!ReadConfig(config, error)) {
        return false;
    }
    localPort_ = config.localPort;
    const std::wstring proxy = L"socks5h://127.0.0.1:"
        + std::to_wstring(localPort_);
    if (!SetEnvironmentVariableW(L"ALL_PROXY", proxy.c_str())) {
        error = L"无法设置当前索引器的 ALL_PROXY："
            + Win32Error(GetLastError());
        return false;
    }
    SetEnvironmentVariableW(L"all_proxy", proxy.c_str());
    // A case-stable private name avoids ambiguous inherited environment
    // blocks that contain both ALL_PROXY and all_proxy with different values.
    SetEnvironmentVariableW(L"GNSS_DOWNLOADER_PROXY", proxy.c_str());
    return true;
}

void SsrWatchdog::AppendRecoveryLog(
    const std::wstring& message) const {
    const auto path = options_.directory / L"gnss-index-watchdog.log";
    std::wofstream output(path, std::ios::app);
    if (output) {
        output << L"[" << Timestamp() << L"] " << message << L"\n";
    }
}

bool SsrWatchdog::RecoverUntilHealthy(
    const std::stop_token stopToken,
    const LogCallback& log,
    const bool skipCurrentNode) {
    if (!available()) {
        log(L"没有找到可控制的 ShadowsocksR.exe/gui-config.json，无法自动恢复。");
        return false;
    }
    ConfigInfo original;
    std::wstring error;
    if (!ReadConfig(original, error)) {
        log(error);
        return false;
    }
    localPort_ = original.localPort;
    const std::array<std::chrono::seconds, 4> backoff{
        std::chrono::minutes(1),
        std::chrono::minutes(5),
        std::chrono::minutes(15),
        std::chrono::minutes(30)
    };
    std::size_t round = 0;
    while (!stopToken.stop_requested()) {
        const auto candidates = OrderedCandidateNodes(
            original,
            skipCurrentNode, options_.preferredCandidateNodes);
        const int candidateCount = std::min(
            std::max(1, options_.maximumCandidateNodes),
            static_cast<int>(candidates.size()));
        for (int attempt = 0;
             attempt < candidateCount
                 && !stopToken.stop_requested();
             ++attempt) {
            const int candidate = candidates[static_cast<std::size_t>(attempt)];
            const bool changing = candidate != original.index;
            const std::wstring action = changing
                ? L"正在切换并测试 SSR 候选节点 "
                    + std::to_wstring(attempt + 1)
                    + L"/" + std::to_wstring(candidateCount) + L"……"
                : L"正在重启并复测当前 SSR 节点……";
            log(action);
            AppendRecoveryLog(action);
            if (RestartAndProbe(
                    candidate,
                    changing,
                    stopToken,
                    log)) {
                const std::wstring success =
                    L"代理已通过两轮端到端探测，将重试触发异常的日期。";
                log(success);
                AppendRecoveryLog(success);
                return true;
            }
            log(L"该节点未通过端到端探测。");
        }
        if (stopToken.stop_requested()) {
            break;
        }

        // Leave the user's original node selected while waiting.  The next
        // recovery round will restart and re-test it first.
        ConfigInfo current;
        if (ReadConfig(current, error)
            && current.index != original.index) {
            StopExactProcess(error);
            if (WriteIndex(current, original.index, error)) {
                StartProcess(error);
                WaitForLocalPort(stopToken);
            }
        }
        MessageBeep(MB_ICONWARNING);
        const auto delay = backoff[std::min(round, backoff.size() - 1)];
        const std::wstring paused =
            L"所有候选节点均失败；扫描保持暂停，"
            + std::to_wstring(delay.count() / 60)
            + L" 分钟后再次探测。按 Ctrl+C 可安全退出。";
        log(paused);
        AppendRecoveryLog(paused);
        InterruptibleSleep(delay, stopToken);
        ++round;
        if (ReadConfig(original, error)) {
            localPort_ = original.localPort;
        }
    }
    // The caller may cancel while a candidate is being tested. Restore the
    // original selection before exiting so Ctrl+C does not leave a half-tested
    // node active.
    ConfigInfo current;
    if (ReadConfig(current, error) && current.index != original.index) {
        StopExactProcess(error);
        if (WriteIndex(current, original.index, error)) {
            StartProcess(error);
        }
    }
    return false;
}
