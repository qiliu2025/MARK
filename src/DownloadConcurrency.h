#pragma once

#include "HttpClient.h"
#include "Models.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

constexpr std::size_t kAutomaticDownloadGlobalWorkerLimit = 12;

struct DownloadEndpointProfile {
    std::wstring key;
    int initial = 1;
    int minimum = 1;
    int maximum = 1;
    int growAfter = 12;
};

enum class DownloadConcurrencyFeedback {
    Success,
    RateLimited,
    TransientFailure,
    NeutralFailure,
    Cancelled,
};

struct DownloadConcurrencySnapshot {
    int active = 0;
    int limit = 0;
    int minimum = 0;
    int maximum = 0;
    bool coolingDown = false;
};

// Classify one concrete transfer endpoint.  Limits are deliberately per host:
// a slow FTP or Terras response must never reduce the concurrency available to
// an unrelated S3/HTTPS archive.
DownloadEndpointProfile ClassifyDownloadEndpoint(std::wstring_view url);

// Conservative text classification for the current HttpDownloadResult API.
// Unknown/local/permanent failures are neutral and never reduce concurrency.
DownloadConcurrencyFeedback ClassifyDownloadConcurrencyFeedback(
    const HttpDownloadResult& result);

std::wstring PrimaryDownloadEndpointUrl(const DatedStationFile& item);

// Return indices rather than copying the potentially large StationFile
// objects.  Weighted host round-robin prevents a block of Terras tasks from
// occupying every global worker while an unrelated fast archive is ready.
std::vector<std::size_t> BuildAutomaticDownloadDispatchOrder(
    const std::vector<DatedStationFile>& files);

class AutomaticDownloadConcurrency {
public:
    class Lease {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        ~Lease();

        void finish(const HttpDownloadResult& result) noexcept;
        [[nodiscard]] explicit operator bool() const noexcept {
            return owner_ != nullptr;
        }

    private:
        friend class AutomaticDownloadConcurrency;
        Lease(
            AutomaticDownloadConcurrency* owner,
            std::wstring key,
            std::chrono::steady_clock::time_point started) noexcept;
        void abandon() noexcept;

        AutomaticDownloadConcurrency* owner_ = nullptr;
        std::wstring key_;
        std::chrono::steady_clock::time_point started_{};
    };

    struct AcquiredLease {
        std::size_t candidateIndex = 0;
        Lease lease;
    };

    AutomaticDownloadConcurrency() = default;
    AutomaticDownloadConcurrency(const AutomaticDownloadConcurrency&) = delete;
    AutomaticDownloadConcurrency& operator=(
        const AutomaticDownloadConcurrency&) = delete;

    // Non-blocking acquisition is used by multi-route IGS files so a worker
    // can immediately try another healthy mirror instead of queueing behind a
    // saturated preferred host.
    [[nodiscard]] std::optional<Lease> tryAcquire(std::wstring_view url);

    // The blocking form polls in short intervals, preserving pause/cancel
    // responsiveness while waiting for a host slot or cooldown to expire.
    [[nodiscard]] std::optional<Lease> acquire(
        std::wstring_view url,
        std::stop_token stopToken,
        const DownloadControlPtr& control = nullptr);

    // Wait for whichever candidate endpoint becomes ready first.  This keeps
    // IGS mirror fallback responsive when the preferred host is cooling down
    // but another archive releases a slot.
    [[nodiscard]] std::optional<AcquiredLease> acquireAny(
        std::span<const std::wstring_view> urls,
        std::stop_token stopToken,
        const DownloadControlPtr& control = nullptr);

    [[nodiscard]] DownloadConcurrencySnapshot snapshot(
        std::wstring_view url) const;

private:
    struct HostState {
        DownloadEndpointProfile profile;
        int active = 0;
        int limit = 1;
        int adaptiveCeiling = 1;
        int pressureFailures = 0;
        int sampleSuccesses = 0;
        int peakActive = 0;
        std::uint64_t sampleBytes = 0;
        double sampleSeconds = 0.0;
        double acceptedAggregateBytesPerSecond = 0.0;
        bool evaluatingHigherLimit = false;
        std::chrono::steady_clock::time_point cooldownUntil{};
    };

    HostState& ensureStateLocked(const DownloadEndpointProfile& profile);
    void release(
        const std::wstring& key,
        const HttpDownloadResult* result,
        std::chrono::steady_clock::time_point started) noexcept;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::unordered_map<std::wstring, HostState> states_;
    std::size_t activeTotal_ = 0;
};

// Both download panes use one process-wide controller.  This shares the
// twelve-transfer ceiling and each archive's adaptive host limit.
AutomaticDownloadConcurrency& SharedAutomaticDownloadConcurrency() noexcept;
