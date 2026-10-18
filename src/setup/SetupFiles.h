#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

namespace mark::setup {
inline DWORD PublishPreparedDirectory(const std::filesystem::path& source,
    const std::filesystem::path& destination, const std::atomic_bool& cancelled,
    std::chrono::milliseconds wait = std::chrono::seconds(15)) {
    const auto move = [&](const std::filesystem::path& from, const std::filesystem::path& to) -> DWORD {
        const auto deadline = std::chrono::steady_clock::now() + wait;
        for (;;) {
            if (cancelled) return ERROR_CANCELLED;
            if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return ERROR_SUCCESS;
            const auto code = GetLastError();
            if ((code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION
                    && code != ERROR_LOCK_VIOLATION && code != ERROR_ALREADY_EXISTS)
                || std::chrono::steady_clock::now() >= deadline) return code;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    };
    if (cancelled) return ERROR_CANCELLED;
    {
        std::error_code error;
        const bool exists = std::filesystem::exists(destination, error);
        if (error) return static_cast<DWORD>(error.value());
        if (exists) {
            const bool directory = std::filesystem::is_directory(destination, error);
            if (error) return static_cast<DWORD>(error.value());
            if (!directory) return ERROR_DIR_NOT_EMPTY;
            const bool empty = std::filesystem::is_empty(destination, error);
            if (error) return static_cast<DWORD>(error.value());
            if (!empty) return ERROR_DIR_NOT_EMPTY;
            // Keep an existing empty folder: Explorer can hold its directory
            // handle open even though writing files inside it is permitted.
        } else {
            const auto code = move(source, destination);
            if (code == ERROR_SUCCESS || code == ERROR_CANCELLED) return code;
            if (code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION
                && code != ERROR_LOCK_VIOLATION) return code;
            if (!CreateDirectoryW(destination.c_str(), nullptr)) return GetLastError();
        }
    }
    std::error_code error;
    std::vector<std::filesystem::path> entries;
    for (std::filesystem::directory_iterator it(source, error), end; !error && it != end; it.increment(error))
        entries.push_back(it->path());
    if (error) return static_cast<DWORD>(error.value());
    std::sort(entries.begin(), entries.end(), [](const auto& first, const auto& second) {
        if (first.filename() == L"MARK.installation.json") return false;
        if (second.filename() == L"MARK.installation.json") return true;
        return first < second;
    });
    std::vector<std::filesystem::path> moved;
    for (const auto& entry : entries) {
        const auto code = move(entry, destination / entry.filename());
        if (code != ERROR_SUCCESS) {
            // Roll back only entries moved by this installation. Never replace
            // files that appeared in the target while setup was running.
            for (auto it = moved.rbegin(); it != moved.rend(); ++it)
                MoveFileExW((destination / it->filename()).c_str(), it->c_str(), MOVEFILE_WRITE_THROUGH);
            return code;
        }
        moved.push_back(entry);
    }
    RemoveDirectoryW(source.c_str());
    return ERROR_SUCCESS;
}
}
