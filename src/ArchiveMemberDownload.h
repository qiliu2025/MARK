#pragma once

// SPSLux distributes observation bundles inside annual ZIP archives. The index
// records verified central-directory offsets; retrieve just that member.
#include "HttpClient.h"
#include <Windows.h>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <optional>

namespace archive_member {
inline std::uint16_t U16(const std::string& s, std::size_t p) {
    return static_cast<unsigned char>(s[p])
        | (static_cast<unsigned char>(s[p + 1]) << 8);
}
inline void Put(std::string& s, std::uint64_t n, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) s.push_back(static_cast<char>(n >> (8 * i)));
}
inline std::string Wrapper(const std::string& compressed, std::uint32_t raw,
                           std::uint16_t method, std::uint32_t crc) {
    constexpr std::string_view name = "observation.bin";
    std::string s;
    Put(s, 0x04034b50, 4); Put(s, 20, 2); Put(s, 0, 2); Put(s, method, 2);
    Put(s, 0, 2); Put(s, 0, 2); Put(s, crc, 4); Put(s, compressed.size(), 4);
    Put(s, raw, 4); Put(s, name.size(), 2); Put(s, 0, 2); s += name; s += compressed;
    const auto center = s.size();
    Put(s, 0x02014b50, 4); Put(s, 20, 2); Put(s, 20, 2); Put(s, 0, 2);
    Put(s, method, 2); Put(s, 0, 2); Put(s, 0, 2); Put(s, crc, 4);
    Put(s, compressed.size(), 4); Put(s, raw, 4); Put(s, name.size(), 2);
    Put(s, 0, 2); Put(s, 0, 2); Put(s, 0, 2); Put(s, 0, 2); Put(s, 0, 4);
    Put(s, 0, 4); s += name;
    const auto centerSize = s.size() - center;
    Put(s, 0x06054b50, 4); Put(s, 0, 2); Put(s, 0, 2); Put(s, 1, 2);
    Put(s, 1, 2); Put(s, centerSize, 4); Put(s, center, 4); Put(s, 0, 2);
    return s;
}
inline HttpDownloadResult Download(const std::wstring& route,
                                  const std::filesystem::path& destination,
                                  std::stop_token stop, const DownloadProgressCallback& progress,
                                  const DownloadControlPtr& control) {
    HttpDownloadResult result;
    auto fail = [&](const wchar_t* message) { result.error = message; return result; };
    auto ready = [&]() {
        if (stop.stop_requested() || (control && !control->waitIfPaused(stop))) {
            result.cancelled = true; result.error = L"下载已取消。"; return false;
        }
        return true;
    };
    if (!ready()) return result;
    const auto marker = route.find(L"#gnss-zip=");
    if (marker == std::wstring::npos) return fail(L"ZIP 成员下载路径不完整。");
    const auto base = route.substr(0, marker);
    if (!base.starts_with(L"https://download.data.public.lu/resources/raw-gnss-data-from-the-spslux-network/")
        && !base.starts_with(L"https://s3.eu-central-1.amazonaws.com/download.data.public.lu/resources/raw-gnss-data-from-the-spslux-network/"))
        return fail(L"ZIP 成员下载源不受支持。");
    std::array<std::uint64_t, 5> fields{};
    const auto spec = route.substr(marker + 10);
    std::size_t at = 0;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        auto end = spec.find(L',', at);
        if (i == 4) end = spec.size();
        if (end == std::wstring::npos || end == at || end - at > 20)
            return fail(L"ZIP 成员参数无效。");
        for (auto p = at; p < end; ++p) {
            if (spec[p] < L'0' || spec[p] > L'9'
                || fields[i] > (UINT64_MAX - (spec[p] - L'0')) / 10)
                return fail(L"ZIP 成员参数无效。");
            fields[i] = fields[i] * 10 + (spec[p] - L'0');
        }
        at = end + 1;
    }
    const auto [offset, compressed, raw, method, crc] = fields;
    constexpr std::uint64_t cap = 64ULL * 1024 * 1024;
    if (!compressed || !raw || compressed > cap || raw > cap || crc > UINT32_MAX
        || (method != 0 && method != 8) || offset > UINT64_MAX - cap - 131100)
        return fail(L"ZIP 成员大小或压缩方法不受支持。");
    auto header = HttpGetByteRange(base, std::to_wstring(offset) + L"-"
        + std::to_wstring(offset + 29), stop, 4096);
    if (!ready()) return result;
    if (header.statusCode != 206 || header.body.size() != 30
        || header.body.compare(0, 4, "PK\x03\x04") != 0)
        return fail(L"服务器未返回有效 ZIP 字节范围，未下载年度档案。");
    if ((U16(header.body, 6) & 1) || U16(header.body, 8) != method)
        return fail(L"ZIP 文件头与知识库成员记录不一致。");
    const auto start = offset + 30 + U16(header.body, 26) + U16(header.body, 28);
    if (progress) progress(0, compressed);
    auto payload = HttpGetByteRange(base, std::to_wstring(start) + L"-"
        + std::to_wstring(start + compressed - 1), stop,
        static_cast<std::size_t>(compressed + 1024));
    if (!ready()) return result;
    if (payload.statusCode != 206 || payload.body.size() != compressed)
        return fail(L"ZIP 成员字节范围下载失败，请重试。");
    if (progress) progress(compressed, compressed);
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    const auto suffix = L".member-" + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(GetCurrentThreadId());
    const auto temporary = std::filesystem::path(destination.wstring() + suffix);
    const auto wrapper = std::filesystem::path(temporary.wstring() + L".zip");
    struct Cleanup {
        std::filesystem::path a, b;
        ~Cleanup() { std::error_code e; std::filesystem::remove(a, e); std::filesystem::remove(b, e); }
    } cleanup{temporary, wrapper};
    if (method == 0) {
        if (compressed != raw) return fail(L"ZIP 非压缩成员大小不一致。");
        std::ofstream out(temporary, std::ios::binary);
        out.write(payload.body.data(), static_cast<std::streamsize>(payload.body.size()));
        if (!out) return fail(L"无法保存 ZIP 成员。");
    } else {
        {
            const auto archive = Wrapper(payload.body, static_cast<std::uint32_t>(raw),
                static_cast<std::uint16_t>(method), static_cast<std::uint32_t>(crc));
            std::ofstream out(wrapper, std::ios::binary);
            out.write(archive.data(), static_cast<std::streamsize>(archive.size()));
            if (!out) return fail(L"无法创建 ZIP 解压临时文件。");
        }
        wchar_t system[MAX_PATH]{};
        if (!GetSystemDirectoryW(system, MAX_PATH)) return fail(L"无法定位系统 ZIP 解压工具。");
        const auto executable = std::filesystem::path(system) / L"tar.exe";
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &sa,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE nullHandle = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        if (output == INVALID_HANDLE_VALUE || nullHandle == INVALID_HANDLE_VALUE) {
            if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
            if (nullHandle != INVALID_HANDLE_VALUE) CloseHandle(nullHandle);
            return fail(L"无法打开 ZIP 解压输出。");
        }
        STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        si.hStdOutput = output; si.hStdInput = nullHandle; si.hStdError = nullHandle;
        std::wstring command = L"\"" + executable.wstring() + L"\" -xOf \""
            + wrapper.wstring() + L"\" observation.bin";
        const bool created = CreateProcessW(executable.c_str(), command.data(), nullptr,
            nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != FALSE;
        if (created) {
            CloseHandle(pi.hThread);
            const auto started = GetTickCount64();
            while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT) {
                if (!ready() || GetTickCount64() - started > 60000) {
                    TerminateProcess(pi.hProcess, 1); break;
                }
            }
            WaitForSingleObject(pi.hProcess, 2000);
            DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess);
            CloseHandle(output); CloseHandle(nullHandle);
            if (result.cancelled) return result;
            if (code != 0) return fail(L"ZIP 成员解压或校验失败。");
        } else {
            CloseHandle(output); CloseHandle(nullHandle);
            return fail(L"系统 ZIP 解压工具无法启动。");
        }
    }
    if (!ready()) return result;
    if (std::filesystem::file_size(temporary, ec) != raw || ec)
        return fail(L"ZIP 成员解压后的大小不一致。");
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return fail(L"无法保存下载文件，请检查文件夹权限。");
    result.success = true; result.bytesWritten = raw; return result;
}
} // namespace archive_member
