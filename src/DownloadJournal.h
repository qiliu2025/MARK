#pragma once

#include <windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <format>
#include <mutex>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

class DownloadJournal {
    std::filesystem::path path_;
    std::ofstream stream_;
    std::mutex mutex_;
    bool finished_ = false;

    static std::string Utf8(std::wstring_view text) {
        if (text.empty()) return {};
        const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string result(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
            result.data(), size, nullptr, nullptr);
        return result;
    }
    static std::string Timestamp() {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    }
    static std::string Field(std::wstring_view value) {
        std::string text = Utf8(value);
        // A server error can echo its URL. Keep credentials and session queries out of saved logs.
        static const std::regex credentials(R"(([A-Za-z][A-Za-z0-9+.-]*://)[^/\s]*@)");
        static const std::regex query(R"(([A-Za-z][A-Za-z0-9+.-]*://[^\s?]+)\?[^\s]*)");
        text = std::regex_replace(text, credentials, "$1[redacted]@");
        text = std::regex_replace(text, query, "$1?[redacted]");
        std::string escaped;
        for (const char c : text) {
            if (c == '\n') escaped += "\\n";
            else if (c == '\r') escaped += "\\r";
            else if (c == '\t') escaped += "\\t";
            else escaped += c;
        }
        return escaped;
    }
public:
    DownloadJournal(const std::filesystem::path& folder, const std::wstring& mode) noexcept {
        try {
            static std::atomic_uint64_t sequence{0};
            const auto logs = folder / L"logs";
            std::filesystem::create_directories(logs);
            SYSTEMTIME now{};
            GetSystemTime(&now);
            path_ = logs / std::format(L"MARK-{}-{:04}{:02}{:02}-{:02}{:02}{:02}-{}-{}.log",
                mode, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                GetCurrentProcessId(), sequence.fetch_add(1));
            stream_.open(path_, std::ios::binary | std::ios::trunc);
            if (!stream_) { path_.clear(); return; }
            stream_ << "MARK 1.0.0 download log\nTimes are UTC.\n"
                << "Time\tStatus\tItem\tFile\tSource\tBytes\tDetails\n";
            Record(L"STARTED", mode, folder, L"", 0, L"Download task started");
        } catch (...) { path_.clear(); }
    }
    ~DownloadJournal() {
        if (!finished_) Record(L"INTERRUPTED", L"", {}, L"", 0,
            L"Task ended before a completion summary was written; completed files are retained.");
    }
    const std::filesystem::path& Path() const noexcept { return path_; }
    void Record(std::wstring_view status, std::wstring_view item,
                const std::filesystem::path& file, std::wstring_view source,
                std::uint64_t bytes, std::wstring_view details = {}) noexcept {
        try {
            std::scoped_lock lock(mutex_);
            if (!stream_) return;
            stream_ << Timestamp() << '\t' << Field(status) << '\t' << Field(item) << '\t'
                << Field(file.wstring()) << '\t' << Field(source) << '\t' << bytes << '\t'
                << Field(details) << '\n';
            stream_.flush();
        } catch (...) {}
    }
    void Finish(std::size_t succeeded, std::size_t skipped, std::size_t failed, bool cancelled,
                const std::vector<std::wstring>& errors, const std::vector<std::wstring>& warnings = {}) noexcept {
        if (finished_) return;
        for (const auto& error : errors) Record(L"ERROR", L"", {}, L"", 0, error);
        for (const auto& warning : warnings) Record(L"WARNING", L"", {}, L"", 0, warning);
        Record(cancelled ? L"CANCELLED" : L"FINISHED", L"", {}, L"", 0,
            std::format(L"Succeeded: {}; already present: {}; failed: {}.{}", succeeded, skipped, failed,
                cancelled ? L" Unstarted items were cancelled; completed files are retained." : L""));
        finished_ = true;
    }
};
