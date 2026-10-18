#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

bool SendAll(const SOCKET socket, const char* data, std::size_t size) {
    while (size > 0) {
        const int chunk = send(
            socket,
            data,
            static_cast<int>(std::min<std::size_t>(size, 64U * 1024U)),
            0);
        if (chunk <= 0) return false;
        data += chunk;
        size -= static_cast<std::size_t>(chunk);
    }
    return true;
}

bool ReceiveExact(const SOCKET socket, char* data, std::size_t size) {
    while (size > 0) {
        const int chunk = recv(
            socket,
            data,
            static_cast<int>(size),
            0);
        if (chunk <= 0) return false;
        data += chunk;
        size -= static_cast<std::size_t>(chunk);
    }
    return true;
}

bool WriteAll(HANDLE output, const char* data, std::size_t size) {
    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(
                output,
                data,
                static_cast<DWORD>(
                    std::min<std::size_t>(size, 64U * 1024U)),
                &written,
                nullptr)
            || written == 0) {
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

bool IsAllowedTarget(const std::wstring_view host, const int port) {
    return (host == L"sftp.igm.gub.uy" && port == 2222)
        || (host == L"geodesia2.inegi.org.mx" && port == 22)
        || (host == L"terras.gsi.go.jp" && port == 22);
}

}  // namespace

int wmain(const int argumentCount, wchar_t** arguments) {
    if (argumentCount != 4) return 2;
    const std::wstring host(arguments[1]);
    int targetPort = 0;
    int proxyPort = 0;
    try {
        targetPort = std::stoi(arguments[2]);
        proxyPort = std::stoi(arguments[3]);
    } catch (...) {
        return 2;
    }
    if (!IsAllowedTarget(host, targetPort)
        || proxyPort < 1 || proxyPort > 65535
        || host.size() > 255) {
        return 2;
    }

    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 3;
    const SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET) {
        WSACleanup();
        return 3;
    }
    sockaddr_in proxy{};
    proxy.sin_family = AF_INET;
    proxy.sin_port = htons(static_cast<u_short>(proxyPort));
    InetPtonW(AF_INET, L"127.0.0.1", &proxy.sin_addr);
    DWORD timeout = 20'000;
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO,
        reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    if (connect(
            socketHandle,
            reinterpret_cast<const sockaddr*>(&proxy),
            sizeof(proxy)) == SOCKET_ERROR) {
        closesocket(socketHandle);
        WSACleanup();
        return 4;
    }

    const std::array<char, 3> greeting{5, 1, 0};
    std::array<char, 2> greetingReply{};
    if (!SendAll(socketHandle, greeting.data(), greeting.size())
        || !ReceiveExact(
            socketHandle, greetingReply.data(), greetingReply.size())
        || greetingReply[0] != 5 || greetingReply[1] != 0) {
        closesocket(socketHandle);
        WSACleanup();
        return 5;
    }

    const int utf8Length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS,
        host.data(), static_cast<int>(host.size()),
        nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0 || utf8Length > 255) {
        closesocket(socketHandle);
        WSACleanup();
        return 5;
    }
    std::string utf8Host(static_cast<std::size_t>(utf8Length), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS,
        host.data(), static_cast<int>(host.size()),
        utf8Host.data(), utf8Length, nullptr, nullptr);
    std::vector<char> request;
    request.reserve(7 + utf8Host.size());
    request.insert(request.end(), {5, 1, 0, 3});
    request.push_back(static_cast<char>(utf8Host.size()));
    request.insert(request.end(), utf8Host.begin(), utf8Host.end());
    request.push_back(static_cast<char>((targetPort >> 8) & 0xff));
    request.push_back(static_cast<char>(targetPort & 0xff));
    std::array<char, 4> reply{};
    if (!SendAll(socketHandle, request.data(), request.size())
        || !ReceiveExact(socketHandle, reply.data(), reply.size())
        || reply[0] != 5 || reply[1] != 0) {
        closesocket(socketHandle);
        WSACleanup();
        return 6;
    }
    std::size_t addressBytes = 0;
    if (reply[3] == 1) addressBytes = 4;
    else if (reply[3] == 4) addressBytes = 16;
    else if (reply[3] == 3) {
        char length = 0;
        if (!ReceiveExact(socketHandle, &length, 1)) {
            closesocket(socketHandle);
            WSACleanup();
            return 6;
        }
        addressBytes = static_cast<unsigned char>(length);
    } else {
        closesocket(socketHandle);
        WSACleanup();
        return 6;
    }
    std::vector<char> discarded(addressBytes + 2);
    if (!ReceiveExact(socketHandle, discarded.data(), discarded.size())) {
        closesocket(socketHandle);
        WSACleanup();
        return 6;
    }

    timeout = 0;
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO,
        reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    std::jthread upstream([&]() {
        std::array<char, 64U * 1024U> buffer{};
        while (true) {
            DWORD read = 0;
            if (!ReadFile(input, buffer.data(),
                    static_cast<DWORD>(buffer.size()), &read, nullptr)
                || read == 0
                || !SendAll(socketHandle, buffer.data(), read)) {
                shutdown(socketHandle, SD_SEND);
                return;
            }
        }
    });
    std::array<char, 64U * 1024U> buffer{};
    while (true) {
        const int received = recv(
            socketHandle, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0
            || !WriteAll(output, buffer.data(),
                static_cast<std::size_t>(received))) {
            break;
        }
    }
    CancelSynchronousIo(upstream.native_handle());
    upstream.join();
    closesocket(socketHandle);
    WSACleanup();
    return 0;
}
